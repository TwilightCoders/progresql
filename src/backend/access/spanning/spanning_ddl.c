/*-------------------------------------------------------------------------
 *
 * spanning_ddl.c
 *	  ProgreSQL: partition-lifecycle maintenance for cross-partition
 *	  ("spanning") unique indexes.
 *
 * The ATTACH backfill and the DROP/DETACH/TRUNCATE cleanup of spanning index
 * entries, called as thin hooks from tablecmds.c.  Kept out of tablecmds.c so
 * the incision there stays the call sites plus the inline DDL guards.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/spanning/spanning_ddl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "access/genam.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/partition.h"
#include "catalog/pg_index.h"
#include "catalog/pg_index_partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_spanning_drainq.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "partitioning/partdesc.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "access/spanning.h"

/*
 * progresql_version
 *
 * SQL-callable fork identity + version.  Returns the fork RELEASE version (the
 * X.Y.Z in the v18.3-X.Y.Z tag), not a separate number -- so it matches the tag,
 * tap, and image, and can't drift from them.  The supported public hook for
 * client tooling (e.g. an ORM adapter) to
 * (a) detect that the server is the ProgreSQL fork rather than vanilla
 * PostgreSQL -- the underlying server_version reports plain "18.3" either way --
 * via `to_regproc('progresql_version') IS NOT NULL`, and (b) version-gate
 * features on the returned release version (e.g. `>= '0.2.0'`) as the fork evolves.
 */
PG_FUNCTION_INFO_V1(progresql_version);
Datum
progresql_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(PROGRESQL_VERSION_STR));
}

/*
 * RelidIsSpanningIndex
 *		True if the given index OID is a ProgreSQL spanning (GLOBAL) index.
 *		Cheap syscache probe, for callers that hold only the index's OID and
 *		must not key off the *table's* relkind -- a spanning index's root may be
 *		a declarative partitioned table OR an ordinary inheritance parent, and
 *		the two have different relkinds.
 */
bool
RelidIsSpanningIndex(Oid indexOid)
{
	HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexOid));
	bool		result;

	if (!HeapTupleIsValid(tup))
		return false;			/* index vanished; not our problem here */
	result = IndexFormIsSpanning((Form_pg_index) GETSTRUCT(tup));
	ReleaseSysCache(tup);
	return result;
}

/*
 * pg_index_is_global
 *
 * SQL-callable: is the given index a ProgreSQL spanning (GLOBAL) index?  The
 * supported introspection hook for tooling that must recover `GLOBAL` from an
 * existing index (e.g. a schema dumper round-trip),
 * so clients depend on this rather than on the internal indnuniqatts/indnkeyatts
 * key-padding representation.  Returns NULL if the argument is not an index.
 */
PG_FUNCTION_INFO_V1(pg_index_is_global);
Datum
pg_index_is_global(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	HeapTuple	tup;
	bool		result;

	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
	if (!HeapTupleIsValid(tup))
		PG_RETURN_NULL();		/* not an index, or it does not exist */

	result = IndexFormIsSpanning((Form_pg_index) GETSTRUCT(tup));
	ReleaseSysCache(tup);

	PG_RETURN_BOOL(result);
}

/*
 * pg_index_global_columns
 *
 * SQL-callable: the user-facing key column NAMES of an index, as text[].  For a
 * spanning (GLOBAL) index this is the leading indnuniqatts key columns -- it
 * EXCLUDES the trailing partseq discriminator, which has no pg_attribute name
 * and makes stock catalog introspection (e.g. ActiveRecord's indexes(), which
 * maps every indkey entry to a name) choke on the unnamed column.  For an
 * ordinary index it is simply all key columns, so callers can use it uniformly.
 * Returns NULL if the argument is not an index, or if a user key is an
 * expression (callers should fall back to pg_get_indexdef in that case).
 */
PG_FUNCTION_INFO_V1(pg_index_global_columns);
Datum
pg_index_global_columns(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	HeapTuple	tup;
	Form_pg_index pgidx;
	Oid			indrelid;
	int			nuser;
	int			i;
	Datum	   *elems;
	ArrayType  *arr;

	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
	if (!HeapTupleIsValid(tup))
		PG_RETURN_NULL();		/* not an index, or it does not exist */

	pgidx = (Form_pg_index) GETSTRUCT(tup);
	/* spanning: user columns are the leading indnuniqatts; ordinary: all keys */
	nuser = IndexFormIsSpanning(pgidx) ? pgidx->indnuniqatts : pgidx->indnkeyatts;
	indrelid = pgidx->indrelid;

	elems = (Datum *) palloc(sizeof(Datum) * nuser);
	for (i = 0; i < nuser; i++)
	{
		AttrNumber	attno = pgidx->indkey.values[i];
		char	   *name;

		if (attno <= 0)			/* expression/system column: not representable here */
		{
			ReleaseSysCache(tup);
			PG_RETURN_NULL();
		}
		name = get_attname(indrelid, attno, false);
		elems[i] = CStringGetTextDatum(name);
		pfree(name);
	}
	ReleaseSysCache(tup);

	arr = construct_array(elems, nuser, TEXTOID, -1, false, TYPALIGN_INT);
	PG_RETURN_ARRAYTYPE_P(arr);
}

/*
 * Deferred, abort-safe retirement of a partition's spanning-index entries.
 *
 * Retirement marks the matching index entries LP_DEAD (see
 * spanning_retire_entries below).  An LP_DEAD page hint is NOT transactional:
 * once set it survives a ROLLBACK.  Doing it inline during DETACH / DROP /
 * TRUNCATE was therefore unsafe -- if the command's transaction aborted, the
 * catalog changes rolled back (partition stays attached, rows return) but the
 * hints did not, leaving a live partition's keys invisible to
 * _bt_check_unique.  Cross-partition uniqueness was then silently unenforced
 * (a duplicate could be inserted into another partition).
 *
 * Fix: queue (indexOid, partseq) during the command and perform the actual
 * LP_DEAD marking from an XACT_EVENT_PRE_COMMIT callback, so it happens only
 * if the transaction commits.  An aborting (sub)transaction discards its
 * queued entries, leaving the index untouched.
 *
 * Caveats (both conservative -- they over-enforce or no-op, never corrupt):
 *  - Marking is deferred to commit, so within the SAME transaction a DETACH/DROP
 *    leaves the departed partition's entries present-but-unresolvable until the
 *    pre-commit pass.  _bt_check_unique skips an entry whose partseq no longer
 *    resolves (nbtinsert.c) rather than probing the storage-less root, so a
 *    same-transaction reinsert of the freed key succeeds.
 *  - PREPARE TRANSACTION discards the queue (a committed-prepared detach leaves
 *    stale-but-live entries that VACUUM reclaims); like a lost LP_DEAD hint
 *    after a crash, the only effect is a spurious conflict, not lost rows.
 */
typedef struct SpanningPendingRetire
{
	Oid			indexOid;
	int32		partseq;
	int			nestLevel;		/* subxact nest level at enqueue time */
} SpanningPendingRetire;

/* pending list lives in TopTransactionContext; reset at end of every xact */
static List *spanning_pending_retire = NIL;
static bool spanning_retire_cb_registered = false;

static void spanning_retire_entries(Oid indexOid, int32 partseq);

/*
 * XACT callback: mark queued entries LP_DEAD at pre-commit; discard on abort.
 */
static void
spanning_retire_xact_cb(XactEvent event, void *arg)
{
	ListCell   *lc;

	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			foreach(lc, spanning_pending_retire)
			{
				SpanningPendingRetire *p = (SpanningPendingRetire *) lfirst(lc);

				spanning_retire_entries(p->indexOid, p->partseq);
			}
			spanning_pending_retire = NIL;	/* freed with TopTransactionContext */
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			/* discard without marking (see caveats above) */
			spanning_pending_retire = NIL;
			break;

		default:
			break;
	}
}

/*
 * Sub-XACT callback: a rolled-back subtransaction must drop the entries it
 * queued, otherwise a savepoint rollback of a DETACH would still retire the
 * partition's entries at top-level commit.
 */
static void
spanning_retire_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB && spanning_pending_retire != NIL)
	{
		int			abortLevel = GetCurrentTransactionNestLevel();
		List	   *kept = NIL;
		ListCell   *lc;

		foreach(lc, spanning_pending_retire)
		{
			SpanningPendingRetire *p = (SpanningPendingRetire *) lfirst(lc);

			/* keep only entries queued at a shallower (surviving) level */
			if (p->nestLevel < abortLevel)
				kept = lappend(kept, p);
		}
		spanning_pending_retire = kept;
	}
}

/*
 * Queue one (spanning index, partseq) pair for pre-commit retirement.
 */
static void
spanning_queue_retire(Oid indexOid, int32 partseq)
{
	MemoryContext old;
	SpanningPendingRetire *p;

	if (!spanning_retire_cb_registered)
	{
		RegisterXactCallback(spanning_retire_xact_cb, NULL);
		RegisterSubXactCallback(spanning_retire_subxact_cb, NULL);
		spanning_retire_cb_registered = true;
	}

	old = MemoryContextSwitchTo(TopTransactionContext);
	p = (SpanningPendingRetire *) palloc(sizeof(SpanningPendingRetire));
	p->indexOid = indexOid;
	p->partseq = partseq;
	p->nestLevel = GetCurrentTransactionNestLevel();
	spanning_pending_retire = lappend(spanning_pending_retire, p);
	MemoryContextSwitchTo(old);
}

/*
 * Mark LP_DEAD every entry in the given spanning index whose trailing partseq
 * key equals partseq.  Tolerates the index (or its root) having been dropped
 * in the same committing transaction (e.g. DROP of the whole partitioned root).
 */
static void
spanning_retire_entries(Oid indexOid, int32 partseq)
{
	Relation		idxRel;
	Relation		rootRel;
	IndexScanDesc	scan;
	ScanKeyData		skey;

	idxRel = try_index_open(indexOid, RowExclusiveLock);
	if (idxRel == NULL)
		return;					/* index dropped in this txn */

	if (!RelationIsSpanning(idxRel))
	{
		index_close(idxRel, RowExclusiveLock);
		return;
	}

	/*
	 * The spanning index lives on the partitioned root; open it as the
	 * heapRelation for index_beginscan.  The root has no table AM
	 * (rd_tableam == NULL); index_beginscan skips table_index_fetch_begin in
	 * that case and we only call index_getnext_tid (TID-only), so
	 * xs_heapfetch is never dereferenced.
	 */
	rootRel = try_table_open(idxRel->rd_index->indrelid, AccessShareLock);
	if (rootRel == NULL)
	{
		index_close(idxRel, RowExclusiveLock);
		return;
	}

	ScanKeyInit(&skey,
				idxRel->rd_index->indnkeyatts,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(partseq));

	scan = index_beginscan(rootRel, idxRel, SnapshotAny, NULL, 1, 0);
	index_rescan(scan, &skey, 1, NULL, 0);

	while (index_getnext_tid(scan, ForwardScanDirection) != NULL)
	{
		/*
		 * Mark this entry LP_DEAD on the next index_getnext_tid call (or on
		 * index_endscan).  _bt_check_unique skips LP_DEAD items, so future
		 * inserts won't see these retired entries as conflicts; they're
		 * physically reclaimed when the page is next modified or VACUUMed.
		 */
		scan->kill_prior_tuple = true;
	}

	index_endscan(scan);
	table_close(rootRel, AccessShareLock);
	index_close(idxRel, RowExclusiveLock);
}

/*
 * Clean all ProgreSQL spanning index entries that reference partRel.
 * Called after a partition's heap storage is gone (TRUNCATE, DROP, or
 * DETACH); any stale entries in spanning indexes on ancestor partitioned
 * roots are removed so they don't dangle to a now-invalid heap TID.
 *
 * Implementation: resolve each spanning index's partseq for this partition
 * and queue (indexOid, partseq) for pre-commit retirement (see
 * spanning_queue_retire / spanning_retire_entries).  Retirement marks the
 * matching entries LP_DEAD; _bt_check_unique skips LP_DEAD items
 * (nbtinsert.c, the !ItemIdIsDead test in the leaf scan loop), so future
 * uniqueness checks correctly ignore them.  Physical removal happens lazily
 * via btree's simple/bottom-up deletion passes when pages are next modified,
 * or via VACUUM.
 *
 * The marking is deferred to commit because an LP_DEAD page hint is not
 * transactional: doing it here would leave a live partition's keys retired
 * if the command's transaction later rolled back.  Filtering by
 * (key, partseq) via the index scan key is exact: no false positives,
 * no false negatives.
 *
 * drop_map: see the prototype in tablecmds.h.  DROP/DETACH pass true (retire
 * the partseq mapping); TRUNCATE passes false (the partition stays attached
 * and must keep its partseq).
 */
void
progresql_clean_spanning_indexes_for_partition(Relation partRel, bool drop_map)
{
	List	   *ancestors;
	ListCell   *lc;
	Oid			partOid = RelationGetRelid(partRel);

	/*
	 * Only a relation that participates in a hierarchy (a declarative partition
	 * or an inheritance child) can sit under a spanning root.
	 */
	if (!partRel->rd_rel->relispartition && !has_superclass(partOid))
		return;

	ancestors = progresql_spanning_ancestors(partOid);

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		Relation	parentRel;
		List	   *indexoidlist;
		ListCell   *il;

		parentRel = table_open(parentOid, AccessShareLock);
		indexoidlist = RelationGetIndexList(parentRel);

		foreach(il, indexoidlist)
		{
			Oid				indexOid = lfirst_oid(il);
			Relation		idxRel;
			int32			partseq;

			idxRel = index_open(indexOid, RowExclusiveLock);

			/* Only spanning indexes. */
			if (!RelationIsSpanning(idxRel))
			{
				index_close(idxRel, RowExclusiveLock);
				continue;
			}

			/*
			 * Resolve this partition's index-local partseq while its map row
			 * still exists (drop_map removes it only after this loop).  If the
			 * partition was never mapped to this index there is nothing to
			 * clean.
			 */
			partseq = SpanningLookupPartseqByRelid(idxRel, partOid);
			if (partseq < 0)
			{
				index_close(idxRel, RowExclusiveLock);
				continue;
			}

			/*
			 * Queue the LP_DEAD marking for pre-commit rather than doing it
			 * inline: the marking is a non-transactional page hint and must
			 * not survive a rollback of this command (spanning_queue_retire).
			 */
			spanning_queue_retire(indexOid, partseq);

			/*
			 * Reap this (now-retired) partseq's deferred-drain queue row: its
			 * pending obligation is satisfied by the retirement above and the
			 * number is leaving (DROP/DETACH) or being superseded (TRUNCATE
			 * re-maps below).  A transactional catalog delete --- safe inline,
			 * unlike the LP_DEAD hint.
			 */
			RemoveSpanningDrainqForPartseq(indexOid, partseq);

			if (!drop_map)
			{
				/*
				 * TRUNCATE (#8): the partition stays attached, but its heap is
				 * now a fresh relfilenode whose TIDs restart at (0,1).  The old
				 * spanning entries are retired above only by a non-durable
				 * LP_DEAD page hint; a crash that loses it would resurrect them,
				 * and because the partition kept its partseq they would still
				 * RESOLVE to the refilled partition and alias a reused TID --- a
				 * spurious unique violation or silently lost enforcement.
				 *
				 * Make TRUNCATE crash-safe by the same mechanism as DETACH:
				 * re-map the partition to a FRESH partseq.  Deleting the old
				 * (index, partition) map row is a WAL-logged, crash-durable
				 * catalog change, after which the old entries are unresolvable
				 * and safely skipped by _bt_check_unique (the LP_DEAD hint is
				 * then only a space optimisation).  The still-attached partition
				 * gets a new partseq from the no-reuse counter for future
				 * inserts.
				 */
				RemoveSpanningPartitionMapEntry(indexOid, partOid);
				CommandCounterIncrement();	/* make the delete visible below */
				(void) SpanningGetOrAllocPartseq(idxRel, partOid);
			}

			index_close(idxRel, RowExclusiveLock);
		}

		list_free(indexoidlist);
		table_close(parentRel, AccessShareLock);
	}

	list_free(ancestors);

	/*
	 * C1: for DROP/DETACH, now that every spanning index's entries for this
	 * partition have been retired, drop the partition's partseq map rows.  This
	 * runs LAST so the per-index partseq lookups above still see the map;
	 * deleting it earlier would leave the entry scans unable to find the
	 * partseq.  Removing the rows keeps indpartrelid from dangling (or aliasing
	 * a future OID reuse).
	 *
	 * TRUNCATE (drop_map=false) keeps the partition attached but has already
	 * re-mapped it to a fresh partseq per index inside the loop above (so the
	 * truncated heap's stale entries become unresolvable, the crash-safe
	 * analogue of DETACH); there is nothing left to drop here.
	 */
	if (drop_map)
		RemoveSpanningPartitionMapForPartition(partOid);
}

/*
 * spanning_remap_keyatts_to_leaf
 *
 * A spanning index records its user-key columns as attribute numbers of the
 * root the index lives on.  A leaf may order those columns differently -- a
 * declarative partition attached with a divergent layout, or (more commonly for
 * inheritance) a standalone table whose columns are in a different order that is
 * later ALTER ... INHERIT'd.  Before reading a leaf tuple with FormIndexDatum we
 * translate each user-key attnum from the root to this leaf by column name, into
 * idxInfo->ii_IndexAttrNumbers.  The trailing discriminator key is left as the
 * root's value -- it is overwritten with the partseq and never read from the
 * heap.  Spanning indexes never have expression keys, so every user-key attnum
 * is a plain (positive) column number.
 *
 * rootKeyAtts holds the index's original (root-relative) key attnums, captured
 * once before the per-leaf loop, so the translation is always root -> leaf.
 */
void
spanning_remap_keyatts_to_leaf(IndexInfo *idxInfo, const AttrNumber *rootKeyAtts,
							   List *rootPredicate, Oid rootOid, Oid leafOid)
{
	int			nkey = idxInfo->ii_NumIndexKeyAttrs;
	int			i;

	for (i = 0; i < nkey - 1; i++)
	{
		AttrNumber	ratt = rootKeyAtts[i];
		char	   *name;

		if (ratt <= 0)
			continue;			/* not expected for spanning user keys */
		name = get_attname(rootOid, ratt, false);
		idxInfo->ii_IndexAttrNumbers[i] = get_attnum(leafOid, name);
		pfree(name);
	}
	idxInfo->ii_IndexAttrNumbers[nkey - 1] = rootKeyAtts[nkey - 1];

	/*
	 * A partial index's predicate is root-relative too, and is evaluated
	 * against a *leaf* tuple, so its Vars need the same by-name translation --
	 * otherwise a leaf whose columns are ordered differently would test the
	 * wrong column.  Always re-derive from the caller's root-relative copy
	 * rather than from ii_Predicate: this runs once per leaf, and mapping an
	 * already-mapped predicate would compound the translation.
	 *
	 * map_partition_varattnos does the by-name Var mapping (index predicates
	 * are stored with varno 1).  Both relations are already lock-held by the
	 * caller, hence NoLock.
	 */
	if (rootPredicate != NIL)
	{
		if (rootOid == leafOid)
			idxInfo->ii_Predicate = rootPredicate;
		else
		{
			Relation	rootRel = relation_open(rootOid, NoLock);
			Relation	leafRel = relation_open(leafOid, NoLock);

			idxInfo->ii_Predicate = map_partition_varattnos(rootPredicate, 1,
														   leafRel, rootRel);
			relation_close(leafRel, NoLock);
			relation_close(rootRel, NoLock);
		}

		/* the cached ExprState was compiled against the pre-remap expression */
		idxInfo->ii_PredicateState = NULL;
	}
}

/*
 * spanning_backfill_leaf
 *
 * Backfill one storage-bearing leaf's existing rows into a spanning index.
 * Shared by the ATTACH backfill, the CREATE-index-on-populated-root build, and
 * the post-rewrite rebuild that routes through the former: each walks a set of
 * leaves and, for every live tuple in each, inserts a
 * (user_columns..., partseq) key so cross-partition uniqueness sees the
 * pre-existing rows.
 *
 *   idxRel       the spanning index being populated (RowExclusiveLock held)
 *   idxInfo      its IndexInfo; ii_IndexAttrNumbers is remapped per leaf here
 *   rootKeyAtts  the key attnums relative to rootOid (the caller snapshots
 *                these before the first remap, since the remap mutates idxInfo)
 *   rootOid      the relation idxInfo's key attnums are expressed against
 *   leafOid      the leaf to scan
 *   heapRel      passed as index_insert's heapRelation -- the spanning root,
 *                NOT leafRel.  nbtinsert keys its heap-liveness skip
 *                (_bt_simpledel_pass / _bt_bottomupdel_pass, which cannot
 *                safely process TIDs spread across multiple partitions) on the
 *                index being spanning, and resolves the real heap from the
 *                stored partseq.
 *
 * The caller must have an active snapshot pushed -- table_beginscan reads
 * GetActiveSnapshot() and DDL does not push one automatically.  Only relations
 * with physical storage are spanning leaves; intermediate parents the tree-walk
 * returns (the declarative root, sub-partitioned mid-level nodes, abstract
 * inheritance parents) hold no rows of their own and are skipped here.
 */
static void
spanning_backfill_leaf(Relation idxRel, IndexInfo *idxInfo,
					   const AttrNumber *rootKeyAtts, List *rootPredicate,
					   Oid rootOid, Oid leafOid, Relation heapRel)
{
	Relation	leafRel;
	TupleTableSlot *slot;
	TableScanDesc scan;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int32		partseq;
	EState	   *estate = NULL;

	leafRel = table_open(leafOid, AccessShareLock);

	if (leafRel->rd_rel->relkind != RELKIND_RELATION)
	{
		table_close(leafRel, AccessShareLock);
		return;
	}

	/*
	 * This leaf's index-local partseq (get-or-allocate; stable across
	 * rebuilds): the trailing discriminator stored in each backfilled entry.
	 * For a freshly created, still-empty leaf this just allocates the partseq
	 * so later INSERTs resolve; the scan below is then a no-op.
	 */
	partseq = SpanningGetOrAllocPartseq(idxRel, leafOid);

	/* read this leaf's columns by name (layout may differ from the root) */
	spanning_remap_keyatts_to_leaf(idxInfo, rootKeyAtts, rootPredicate,
								   rootOid, leafOid);

	/* a partial index needs an expression context to test its predicate */
	if (idxInfo->ii_Predicate != NIL)
		estate = CreateExecutorState();

	slot = table_slot_create(leafRel, NULL);
	scan = table_beginscan(leafRel, GetActiveSnapshot(), 0, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		int			k;

		/*
		 * A partial index covers only the rows its predicate accepts; the rest
		 * must not be backfilled, or the index would enforce uniqueness over
		 * rows it does not describe (and could fail to build over pre-existing
		 * data that is perfectly legal under the declared predicate).
		 */
		if (estate != NULL)
		{
			ResetPerTupleExprContext(estate);
			if (!spanning_index_predicate_holds(idxInfo, slot, estate))
				continue;
		}

		FormIndexDatum(idxInfo, slot, NULL, values, isnull);

		/* Trailing key column: this leaf's partseq. */
		k = idxInfo->ii_NumIndexKeyAttrs - 1;
		values[k] = Int32GetDatum(partseq);
		isnull[k] = false;

		/* UNIQUE_CHECK_YES so pre-existing cross-leaf duplicates are caught. */
		index_insert(idxRel, values, isnull, &slot->tts_tid,
					 heapRel, UNIQUE_CHECK_YES, false, idxInfo);
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	if (estate != NULL)
		FreeExecutorState(estate);

	/*
	 * Reset this leaf's cached relcache state.  Adding a spanning index changes
	 * every leaf's HOT-blocking attribute set (the spanning key columns become
	 * HOT-blocking via progresql_add_spanning_hotblocking_attrs), so a
	 * spanning-key UPDATE must be forced cold.  ALTER TABLE / CREATE INDEX
	 * invalidate the partitioned root but not its leaves, so a backend that
	 * cached a leaf's rd_hotblockingattr BEFORE this build would keep the stale
	 * (pre-spanning) set, treat a spanning-key UPDATE as HOT, drop the spanning
	 * entry, and silently break cross-partition uniqueness (#42).  This also
	 * re-arms the executor maintenance hook for subsequent DML on the leaf.
	 */
	CacheInvalidateRelcacheByRelid(leafOid);

	table_close(leafRel, AccessShareLock);
}

/*
 * progresql_backfill_spanning_indexes_for_attached_partition
 *
 * After ATTACH PARTITION, walk all spanning indexes on partitioned-root
 * ancestors of attachrel and insert (user_columns..., attachrel_oid) keys
 * for each existing row in attachrel.  Without this step, BUG C: rows
 * already in the attached partition are absent from the spanning index,
 * so cross-partition uniqueness checks miss them.
 *
 * Called from ATExecAttachPartition immediately after
 * AttachPartitionEnsureIndexes, while pg_inherits already has the new
 * row so get_partition_ancestors can find ancestor roots.
 */
void
progresql_backfill_spanning_indexes_for_attached_partition(Relation attachrel)
{
	List	   *ancestors;
	List	   *leafoids;
	ListCell   *lc;

	/*
	 * The attaching relation may be a single leaf (a declarative partition or an
	 * inheritance child) or the root of a subtree being grafted in at once; in
	 * every case the storage-bearing leaves of its own subtree are what must be
	 * registered (partseq) and backfilled into the ancestor spanning indexes.
	 */
	leafoids = find_all_inheritors(RelationGetRelid(attachrel), AccessShareLock, NULL);

	/*
	 * Spanning roots above the attaching relation -- declarative partition roots
	 * and/or inheritance (INHERITS) parents, at any depth.
	 */
	ancestors = progresql_spanning_ancestors(RelationGetRelid(attachrel));

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		Relation	parentRel;
		List	   *indexoidlist;
		ListCell   *il;

		parentRel = table_open(parentOid, AccessShareLock);
		indexoidlist = RelationGetIndexList(parentRel);

		foreach(il, indexoidlist)
		{
			Oid			indexOid = lfirst_oid(il);
			Relation	idxRel;
			IndexInfo  *idxInfo;
			AttrNumber	rootKeyAtts[INDEX_MAX_KEYS];
			List	   *rootPredicate;
			ListCell   *ll;

			idxRel = index_open(indexOid, RowExclusiveLock);

			if (!RelationIsSpanning(idxRel))
			{
				index_close(idxRel, RowExclusiveLock);
				continue;
			}

			idxInfo = BuildIndexInfo(idxRel);
			/* key attnums and predicate are root-relative; remapped per leaf */
			memcpy(rootKeyAtts, idxInfo->ii_IndexAttrNumbers,
				   idxInfo->ii_NumIndexKeyAttrs * sizeof(AttrNumber));
			rootPredicate = idxInfo->ii_Predicate;

			foreach(ll, leafoids)
			{
				Oid			leafOid = lfirst_oid(ll);

				/*
				 * spanning_backfill_leaf scans under the active snapshot, which
				 * DDL does not push automatically, so wrap each leaf.  (partseq
				 * allocation and key remap touch only catalogs, so running them
				 * under the snapshot too is harmless.)
				 */
				PushActiveSnapshot(GetTransactionSnapshot());
				spanning_backfill_leaf(idxRel, idxInfo, rootKeyAtts,
									   rootPredicate, parentOid, leafOid,
									   parentRel);
				PopActiveSnapshot();
			}

			index_close(idxRel, RowExclusiveLock);
		}

		list_free(indexoidlist);
		table_close(parentRel, AccessShareLock);
	}

	list_free(ancestors);
	list_free(leafoids);
}

/*
 * progresql_rebuild_spanning_for_rewritten_partition
 *
 * A table rewrite (CLUSTER, VACUUM FULL, or an ALTER TABLE that rewrites the
 * heap) of a LEAF partition gives it a new relfilenode and relocates every live
 * tuple to a new TID.  The spanning (GLOBAL) index lives on the partitioned
 * ROOT, not as a local index of the leaf, so the rewrite's reindex_relation
 * (which only rebuilds the leaf's own indexes) leaves the spanning index full of
 * entries pointing at the freed old storage -- stale TIDs that alias dead/other
 * rows AND the relocated tuples missing entirely.  Either way cross-partition
 * uniqueness is silently broken.
 *
 * Rebuild the leaf's spanning entries with the same crash-safe re-map mechanism
 * TRUNCATE/DETACH use: drop the leaf's (now-stale) partseq map rows so its old
 * entries become unresolvable (and are skipped by _bt_check_unique), then
 * backfill fresh entries for the relocated tuples under a freshly-allocated
 * partseq.  No-op for non-partition relations and for partitions with no
 * spanning ancestor.  Called from finish_heap_swap after the swap+reindex.
 */
void
progresql_rebuild_spanning_for_rewritten_partition(Oid relid)
{
	Relation	rel;

	/* The rewrite holds AccessExclusiveLock on relid already. */
	rel = try_table_open(relid, AccessShareLock);
	if (rel == NULL)
		return;

	/*
	 * Only a storage-bearing leaf can sit under a spanning root -- but a leaf is
	 * a declarative partition OR an inheritance child, exactly as
	 * progresql_clean_spanning_indexes_for_partition already tests.  Keying this
	 * on relispartition alone skipped every inheritance child, leaving its
	 * entries pointing at pre-rewrite TIDs: the uniqueness probe would then
	 * follow one into a block the compaction had removed and raise a bare
	 * "could not read blocks" error from an ordinary INSERT, and a deleted key
	 * could never be reused.
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION &&
		(rel->rd_rel->relispartition || has_superclass(relid)))
	{
		/*
		 * Retire this leaf's existing entries before re-inserting.  Previously
		 * only the partseq *mapping* was dropped, which left the old entries in
		 * the tree under a partseq that no longer resolved: inert (the probe
		 * skips an unresolvable partseq) but permanent, so every rewrite added a
		 * full set and the index grew without bound -- 300 rows became 600
		 * entries, then 900.  Retirement is queued and applied at pre-commit,
		 * and the backfill below allocates a *fresh* partseq because the map was
		 * dropped, so the queued retirement can never reach the new entries.
		 */
		progresql_clean_spanning_indexes_for_partition(rel, true);
		CommandCounterIncrement();	/* make the map delete visible to the backfill */
		progresql_backfill_spanning_indexes_for_attached_partition(rel);
	}

	table_close(rel, AccessShareLock);
}

/*
 * BuildSpanningIndexFromPartitions
 *
 * After creating a ProgreSQL spanning index on a partitioned root, populate
 * it with entries for all existing tuples in all leaf child partitions.
 * Without this step the spanning index is empty after ALTER TABLE ADD
 * CONSTRAINT on a non-empty table, so cross-partition uniqueness checks
 * miss pre-existing rows.
 *
 * For each live tuple in each partition we compute the index key
 * (user_columns... + tableoid) via FormIndexDatum and call index_insert with
 * UNIQUE_CHECK_YES so that pre-existing duplicates are caught.
 */
void
BuildSpanningIndexFromPartitions(Relation rel, Oid indexRelationId)
{
	Relation	idxRel;
	IndexInfo  *idxInfo;
	AttrNumber	rootKeyAtts[INDEX_MAX_KEYS];
	List	   *rootPredicate;
	List	   *leafoids;
	ListCell   *lc;
	Snapshot	snapshot;

	idxRel = index_open(indexRelationId, RowExclusiveLock);
	idxInfo = BuildIndexInfo(idxRel);
	/* key attnums and predicate are root-relative; remapped per leaf by name */
	memcpy(rootKeyAtts, idxInfo->ii_IndexAttrNumbers,
		   idxInfo->ii_NumIndexKeyAttrs * sizeof(AttrNumber));
	rootPredicate = idxInfo->ii_Predicate;

	/*
	 * Enumerate every storage-bearing leaf beneath this root, at any depth,
	 * whether the tree is built from declarative partitions, table inheritance
	 * (INHERITS), or a mix of both.  find_all_inheritors walks pg_inherits
	 * recursively and returns the root plus all descendants; spanning_backfill_leaf
	 * skips the non-storage parents among them (only leaves actually hold rows).
	 */
	leafoids = find_all_inheritors(RelationGetRelid(rel), AccessShareLock, NULL);

	/*
	 * Push the transaction snapshot as active so that heap visibility checks
	 * inside table_scan_getnextslot (and the btree uniqueness check path) can
	 * satisfy the snapshot active_count assertions.  DDL commands do not
	 * push a snapshot automatically, so we must do it explicitly here.
	 */
	snapshot = GetTransactionSnapshot();
	PushActiveSnapshot(snapshot);

	foreach(lc, leafoids)
	{
		Oid			partOid = lfirst_oid(lc);

		spanning_backfill_leaf(idxRel, idxInfo, rootKeyAtts, rootPredicate,
							   RelationGetRelid(rel), partOid, rel);
	}

	list_free(leafoids);

	PopActiveSnapshot();

	index_close(idxRel, RowExclusiveLock);
}
