/*-------------------------------------------------------------------------
 *
 * spanning_relcache.c
 *	  ProgreSQL: relcache-side HOT-blocking support for cross-partition
 *	  ("spanning") unique indexes (the E7 correctness fix).
 *
 * A spanning index lives on the partitioned root, so a leaf has no local index
 * on the spanning key and an UPDATE changing that key would wrongly be treated
 * as HOT.  These helpers, called from RelationGetIndexAttrBitmap, add the
 * spanning key columns to the leaf's hot-blocking attribute set.  Catalog scans
 * only, so they are safe on the cached relcache path.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/spanning/spanning_relcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "nodes/bitmapset.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "access/spanning.h"

/*
 * progresql_spanning_ancestors
 *
 * Return the OIDs of every ancestor of relid, walking pg_inherits upward and
 * covering BOTH declarative partition parents and table-inheritance (INHERITS)
 * parents -- and any mix, at any depth (multiple inheritance included).  This is
 * the leaf->root resolution that lets one spanning index be maintained for a
 * leaf regardless of whether it is reached via partitioning, inheritance, or a
 * combination of the two.
 *
 * Unlike get_partition_ancestors (partition-only, single-parent), this follows
 * every inhparent link.  It honors detach-in-progress the same way: a partition
 * whose inheritance row is marked detach-pending is no longer a spanning member,
 * so it is skipped (and its own ancestors are not pursued through that link).
 * Catalog scans only (safe from the cached relcache path); caller frees the list.
 */
List *
progresql_spanning_ancestors(Oid relid)
{
	List	   *result = NIL;
	List	   *queue = list_make1_oid(relid);
	Relation	inhRel;

	inhRel = table_open(InheritsRelationId, AccessShareLock);

	while (queue != NIL)
	{
		Oid			cur = linitial_oid(queue);
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;

		queue = list_delete_first(queue);

		ScanKeyInit(&skey, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(cur));
		scan = systable_beginscan(inhRel, InheritsRelidSeqnoIndexId, true,
								  NULL, 1, &skey);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(tup);
			Oid			parent = inh->inhparent;

			/* a detach-in-progress partition is no longer a spanning member */
			if (inh->inhdetachpending)
				continue;

			if (!list_member_oid(result, parent))
			{
				result = lappend_oid(result, parent);
				queue = lappend_oid(queue, parent);
			}
		}

		systable_endscan(scan);
	}

	table_close(inhRel, AccessShareLock);
	return result;
}

/*
 * RelationCanBeSpanningLeaf
 *		See the rule stated in access/spanning.h -- a spanning leaf is a
 *		declarative partition OR an inheritance child.
 */
bool
RelationCanBeSpanningLeaf(Relation relation)
{
	return relation->rd_rel->relispartition ||
		has_superclass(RelationGetRelid(relation));
}

/*
 * progresql_add_spanning_hotblocking_attrs
 *
 * ProgreSQL: a spanning (GLOBAL) index lives on a partitioned ROOT, not on the
 * leaf partitions that hold the rows.  A leaf therefore has no *local* index on
 * the spanning key columns, so without this an UPDATE that changes a spanning
 * key on a leaf would be considered HOT-safe (the leaf's own index set does not
 * mention those columns).  A HOT update does not maintain the leaf's indexes and
 * keeps the old line pointer live as a redirect, which would leave the old
 * spanning-index entry pointing at a still-live heap chain -- a stale entry that
 * vacuum can never reclaim and that causes false cross-partition uniqueness
 * conflicts.
 *
 * To prevent that, the spanning index's user-key columns must BLOCK HOT on the
 * leaf, exactly as a real local index on those columns would.  We add them to
 * the leaf's hot-blocking attribute set so a spanning-key change forces a
 * non-HOT update: the old heap tuple dies normally, its spanning entry becomes
 * reclaimable, and the liveness probe no longer mistakes it for a live row.
 *
 * Done with catalog scans only (no relation/index opens) so it is safe to call
 * from this cached relcache path; the result is memoized in rd_hotblockingattr.
 */
void
progresql_add_spanning_hotblocking_attrs(Relation relation,
										 Bitmapset **hotblockingattrs)
{
	Oid			leafOid = RelationGetRelid(relation);
	List	   *ancestors;
	ListCell   *lc;
	Relation	pg_index_rel;

	if (!RelationCanBeSpanningLeaf(relation))
		return;

	ancestors = progresql_spanning_ancestors(leafOid);
	if (ancestors == NIL)
		return;

	pg_index_rel = table_open(IndexRelationId, AccessShareLock);

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;

		ScanKeyInit(&skey, Anum_pg_index_indrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(parentOid));
		scan = systable_beginscan(pg_index_rel, IndexIndrelidIndexId, true,
								  NULL, 1, &skey);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_index pgidx = (Form_pg_index) GETSTRUCT(tup);
			int			nuser;
			int			i;

			if (!IndexFormIsSpanning(pgidx))
				continue;

			/*
			 * The leading indnuniqatts columns are the user-visible unique key;
			 * the trailing column is the partseq discriminator (a system column
			 * not derived from any user column) and must be skipped.
			 */
			nuser = pgidx->indnuniqatts;
			for (i = 0; i < nuser; i++)
			{
				AttrNumber	rootattno = pgidx->indkey.values[i];
				char	   *attname;
				AttrNumber	leafattno;

				if (rootattno <= 0)		/* expression/system column: skip */
					continue;

				/*
				 * Map root attribute -> leaf attribute by name (robust to
				 * attribute-number divergence across the partition tree).
				 */
				attname = get_attname(parentOid, rootattno, true);
				if (attname == NULL)
					continue;
				leafattno = get_attnum(leafOid, attname);
				if (leafattno == InvalidAttrNumber)
					continue;

				*hotblockingattrs =
					bms_add_member(*hotblockingattrs,
								   leafattno - FirstLowInvalidHeapAttributeNumber);
			}

			/*
			 * A partial spanning index's PREDICATE columns must block HOT for
			 * the same reason its key columns do -- in fact more sharply.  An
			 * update that moves a row across the predicate boundary changes
			 * whether the row belongs in the index at all.  If such an update
			 * went HOT, the old entry would survive as a redirect and resolve
			 * through the HOT chain to the *new* tuple, so the liveness probe
			 * would report a conflict for a row the predicate no longer covers
			 * -- a phantom violation on exactly the "close this version and
			 * insert its replacement" pattern partial indexes exist to serve.
			 * Stock gets this for free by pulling ii_Predicate's varattnos into
			 * the index attribute set; a spanning index is not in the leaf's own
			 * index list, so we must do it here.
			 */
			{
				Datum		predDatum;
				bool		predIsNull;

				predDatum = heap_getattr(tup, Anum_pg_index_indpred,
										 RelationGetDescr(pg_index_rel),
										 &predIsNull);
				if (!predIsNull)
				{
					char	   *predString = TextDatumGetCString(predDatum);
					Node	   *pred = (Node *) stringToNode(predString);
					Bitmapset  *predattrs = NULL;
					int			x = -1;

					/* index predicates are stored with varno 1 */
					pull_varattnos(pred, 1, &predattrs);

					while ((x = bms_next_member(predattrs, x)) >= 0)
					{
						AttrNumber	rootattno =
							x + FirstLowInvalidHeapAttributeNumber;
						char	   *pattname;
						AttrNumber	pleafattno;

						if (rootattno <= 0)		/* system column: skip */
							continue;
						pattname = get_attname(parentOid, rootattno, true);
						if (pattname == NULL)
							continue;
						pleafattno = get_attnum(leafOid, pattname);
						pfree(pattname);
						if (pleafattno == InvalidAttrNumber)
							continue;

						*hotblockingattrs =
							bms_add_member(*hotblockingattrs,
										   pleafattno - FirstLowInvalidHeapAttributeNumber);
					}
					pfree(predString);
				}
			}
		}
		systable_endscan(scan);
	}

	table_close(pg_index_rel, AccessShareLock);
	list_free(ancestors);
}

/*
 * progresql_leaf_has_spanning_ancestor
 *
 * Cheap predicate: true if relation is a leaf partition that has at least one
 * spanning (GLOBAL) index on an ancestor root.  Used to keep
 * RelationGetIndexAttrBitmap from taking its "no local indexes" fast-path
 * exits for such a leaf, since its hot-blocking set is non-empty even though it
 * owns no local index.  Catalog scans only; early-exits on first match.
 */
bool
progresql_leaf_has_spanning_ancestor(Relation relation)
{
	List	   *ancestors;
	ListCell   *lc;
	Relation	pg_index_rel;
	bool		found = false;

	if (!RelationCanBeSpanningLeaf(relation))
		return false;

	ancestors = progresql_spanning_ancestors(RelationGetRelid(relation));
	if (ancestors == NIL)
		return false;

	pg_index_rel = table_open(IndexRelationId, AccessShareLock);

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;

		ScanKeyInit(&skey, Anum_pg_index_indrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(parentOid));
		scan = systable_beginscan(pg_index_rel, IndexIndrelidIndexId, true,
								  NULL, 1, &skey);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			if (IndexFormIsSpanning((Form_pg_index) GETSTRUCT(tup)))
			{
				found = true;
				break;
			}
		}
		systable_endscan(scan);
		if (found)
			break;
	}

	table_close(pg_index_rel, AccessShareLock);
	list_free(ancestors);
	return found;
}

/*
 * RelationHasSpanningAncestor
 *
 * Relcache-cached predicate for the per-row write path: does this relation
 * participate in a spanning index as a storage relation?  That is true for a
 * leaf with a spanning index on an ANCESTOR root, and ALSO for a heap-bearing
 * root that carries its OWN spanning index and receives direct inserts (an
 * inheritance root that holds its own rows, e.g. a concrete base table, or a
 * standalone GLOBAL table before it has children) -- in that case the root is
 * effectively a leaf of its own index and its direct inserts must be maintained
 * through the same hook (the normal index path skips spanning indexes).
 *
 * The answer changes only when a spanning index is added to or dropped from this
 * relation or an ancestor.  Spanning-index BUILD/ATTACH explicitly invalidate the
 * relevant leaf relcaches (#42), so an added index is tracked promptly; a DROP
 * does NOT, so a stale "true" can linger until the leaf relcache is invalidated
 * for some other reason.  That is fail-safe: the per-insert maintenance hook
 * re-resolves the ancestor/index set every statement (spanning_exec.c) and
 * early-outs when none remains, so a stale "true" only costs that re-resolution
 * and can never admit a cross-partition duplicate.  This lets the hook early-out
 * with a single field read for the do-no-harm common case (any table not in a
 * spanning index).
 */
bool
RelationHasSpanningAncestor(Relation relation)
{
	if (!relation->rd_progresql_spanning_leaf_valid)
	{
		relation->rd_progresql_spanning_leaf =
			progresql_leaf_has_spanning_ancestor(relation) ||
			RelationHasSpanningIndex(relation);
		relation->rd_progresql_spanning_leaf_valid = true;
	}
	return relation->rd_progresql_spanning_leaf;
}

/*
 * RelationHasSpanningIndex
 *
 * True if relation has at least one spanning (GLOBAL) index of its own.  The RI
 * foreign-key machinery uses this to decide that a FK referencing this relation
 * must resolve across the whole inheritance/partition tree (not ONLY the root):
 * the spanning index enforces the referenced key's uniqueness across every
 * child, so the referenced row may live in any descendant.  Catalog scan only.
 */
bool
RelationHasSpanningIndex(Relation relation)
{
	List	   *indexoidlist = RelationGetIndexList(relation);
	ListCell   *lc;
	bool		found = false;

	foreach(lc, indexoidlist)
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID,
										  ObjectIdGetDatum(lfirst_oid(lc)));

		if (HeapTupleIsValid(tup))
		{
			if (IndexFormIsSpanning((Form_pg_index) GETSTRUCT(tup)))
				found = true;
			ReleaseSysCache(tup);
		}
		if (found)
			break;
	}
	list_free(indexoidlist);
	return found;
}
