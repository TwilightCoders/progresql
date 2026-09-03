/*-------------------------------------------------------------------------
 *
 * spanning.h
 *	  ProgreSQL: public entry points for cross-partition ("spanning")
 *	  unique-index maintenance.  Implementation in src/backend/access/spanning/.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spanning.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPANNING_H
#define SPANNING_H

#include "access/itup.h"
#include "nodes/execnodes.h"
#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/relcache.h"

/*
 * ProgreSQL fork release version -- the X.Y.Z that follows the PostgreSQL base
 * in the release tag (v18.3-X.Y.Z), independent of the PostgreSQL base version
 * itself (server_version reports that as plain "18.3").  Exposed to SQL via the
 * built-in progresql_version() -- the supported hook for client tooling (e.g.
 * an ORM adapter) to detect the fork and version-gate features.
 *
 * This is the one in-tree source of truth for the fork version; bump it in
 * lockstep with the release tag (it is part of cutting a release, alongside the
 * CHANGELOG/tag/formula/image), and the progresql_global regress test pins it so
 * a change is always visible in the diff.  (Earlier this was a decoupled
 * "feature-set" number that silently stayed at "1.0" across several feature
 * releases -- tying it to the release version is what stops that drift.)
 */
#define PROGRESQL_VERSION_STR "0.2.7"

extern void ExecInsertSpanningIndexTuples(TupleTableSlot *slot,
										  ItemPointer tupleid,
										  Relation partition,
										  EState *estate);

/*
 * Apply-worker variant: maintain the spanning indexes for a replicated
 * INSERT / cold UPDATE, but on a cross-partition uniqueness conflict do NOT
 * raise a raw violation -- detect it (non-blocking) and return the conflicting
 * local tuple so the caller can classify it through logical-replication conflict
 * detection (confl_insert_exists / confl_update_exists) instead of an opaque,
 * retry-looping apply_error.  Returns true and fills *conflictIndex /
 * *conflictSlot when a live cross-partition duplicate exists; the caller is
 * expected to report it at ERROR (the ensuing abort releases the slot and the
 * leaf it was fetched from).  Returns false (indexes maintained) otherwise.
 */
extern bool ExecInsertSpanningIndexTuplesApply(TupleTableSlot *slot,
											   ItemPointer tupleid,
											   Relation partition,
											   EState *estate,
											   Oid *conflictIndex,
											   TupleTableSlot **conflictSlot);
extern void ProgresqlReleasePartitionCache(EState *estate);

/*
 * spanning_lock.c — the cross-partition "value lock".
 *
 * Stock btree enforces uniqueness under concurrency by holding the write lock
 * on the leaf page the key belongs to: any other inserter of the same key must
 * take the same page lock, so the check-and-insert is serialized.  A spanning
 * index breaks that invariant -- the same USER key inserted into two different
 * partitions forms two different full keys (userkey, partseq_a) and
 * (userkey, partseq_b) that can sit on different btree pages -- so the page
 * lock no longer serializes them.  SpanningLockUserKey restores the invariant
 * by taking a short-duration heavyweight lock keyed on (index, hash(userkey))
 * around the check-and-insert; SpanningUnlockUserKey releases it once the new
 * entry is physically in the tree (whereupon it serves as the SnapshotDirty
 * conflict marker for the next inserter, exactly as in stock btree).
 */
extern void SpanningLockUserKey(Relation indexRel, IndexTuple itup,
								LOCKTAG *locktag);
extern void SpanningUnlockUserKey(const LOCKTAG *locktag);

/* spanning_ddl.c — partition-lifecycle maintenance (ATTACH/DETACH/DROP/TRUNCATE) */
extern void progresql_clean_spanning_indexes_for_partition(Relation partRel,
														   bool drop_map);
extern void progresql_backfill_spanning_indexes_for_attached_partition(Relation attachrel);
extern void progresql_rebuild_spanning_for_rewritten_partition(Oid relid);
extern void BuildSpanningIndexFromPartitions(Relation rel, Oid indexRelationId);
/*
 * Remap a spanning index's root-relative user-key attnums -- and, for a partial
 * index, its root-relative predicate -- to a leaf, by column name.
 */
extern void spanning_remap_keyatts_to_leaf(IndexInfo *idxInfo,
										   const AttrNumber *rootKeyAtts,
										   List *rootPredicate,
										   Oid rootOid, Oid leafOid);

/*
 * Does this (leaf-remapped) index's partial predicate accept the row in slot?
 * Always true for a non-partial index.  estate supplies the per-tuple context.
 */
extern bool spanning_index_predicate_holds(IndexInfo *idxInfo,
										   TupleTableSlot *slot,
										   EState *estate);

/* spanning_relcache.c — leaf->root resolution + HOT-blocking attrs (E7) */
extern List *progresql_spanning_ancestors(Oid relid);
extern void progresql_add_spanning_hotblocking_attrs(Relation relation,
													 Bitmapset **hotblockingattrs);
extern bool progresql_leaf_has_spanning_ancestor(Relation relation);
extern bool RelationHasSpanningAncestor(Relation relation);
/* true if relation has a spanning (GLOBAL) index of its own (FK referenced-side) */
extern bool RelationHasSpanningIndex(Relation relation);
/* true if the given index OID is a spanning (GLOBAL) index -- cheap syscache probe */
extern bool RelidIsSpanningIndex(Oid indexOid);

/*
 * RelationCanBeSpanningLeaf
 *		Could this relation hold rows covered by a spanning (GLOBAL) index on
 *		some ancestor?
 *
 * THE RULE, and the reason this is a function rather than an open-coded test:
 * a spanning leaf is a declarative partition OR an inheritance child.  Never
 * assume the former stands for both.  `relispartition` and
 * `relkind == RELKIND_PARTITIONED_TABLE` are true only under declarative
 * partitioning, and a spanning index's root is equally often an ordinary
 * INHERITS parent -- so either one used as a proxy for "is this under a
 * spanning root" silently excludes every inheritance tree.
 *
 * Four separate call sites (reindex repopulation, the REINDEX CONCURRENTLY
 * refusal, the post-rewrite rebuild, and the parallel-build exclusion) each
 * independently reached for that proxy, and each disabled or corrupted spanning
 * behaviour for inheritance roots while looking obviously correct.  Call this,
 * or key on the spanning marker itself (RelationIsSpanning / IndexFormIsSpanning
 * / RelidIsSpanningIndex); do not reintroduce the proxy.
 *
 * The general failure this guards against, worth having in mind before touching
 * anything spanning-related: the fork tends to implement spanning support for
 * DECLARATIVE PARTITIONING and leave INHERITANCE behind.  It shows up two ways --
 * a declarative-only predicate standing in for "under a spanning root" (what
 * this function exists to prevent), and an inheritance counterpart simply never
 * written for a partition operation that has one.  A known live example of the
 * second: a child joining a spanning tree acquires the referenced-side FK clones
 * via progresql_clone_referenced_fks_to_child, but ALTER TABLE ... NO INHERIT has
 * no counterpart that removes them, so a child cannot leave the way a partition
 * can DETACH.  When adding any spanning behaviour, ask what the inheritance case
 * does -- and what the *undo* does.
 *
 * Note this asks only whether the relation participates in a hierarchy.  A
 * caller that also needs the leaf to have storage of its own must check
 * relkind == RELKIND_RELATION separately -- an inheritance root has storage, a
 * declarative root does not.
 */
extern bool RelationCanBeSpanningLeaf(Relation relation);

#endif							/* SPANNING_H */
