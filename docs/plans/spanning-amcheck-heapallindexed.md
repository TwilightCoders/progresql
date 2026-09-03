# amcheck cannot see a spanning index's missing entries

**Status: analysed, not implemented. This is the fork's most consequential
remaining hole, because it makes a *verification* tool report success on a
broken index.**

## The failure this comes from

A `REINDEX` bug left two `GLOBAL` indexes on a production node holding no
entries for any pre-existing row. Cross-child uniqueness was silently
unenforced. It went unnoticed for seven weeks, and the nightly integrity
tripwire reported clean throughout, because the tripwire was
`bt_index_check()` — which cannot observe that failure mode.

Worse, the repair tooling ran the same check *after* performing the operation
that caused the damage, and reported `verified_clean = true`. A repair that
reports success while removing the guarantee it was repairing.

The `REINDEX` bug is fixed. The blindness that let it run for seven weeks is
not, and it will do this again — in someone else's database, for some other
cause — as long as the only available check is structural.

## Why `heapallindexed` is unsupported today

`heapallindexed` is the check that would catch it: it fingerprints every index
tuple, then scans the table and asserts each live tuple is present. That is
precisely "no row is missing from the index".

It is rejected for spanning indexes, deliberately and correctly
(`contrib/amcheck/verify_nbtree.c`):

> Its "table" is the storage-less partitioned root (`rd_tableam == NULL`), so the
> heap scan that fingerprints table tuples would dereference a NULL table AM and
> crash the backend. Validating that every heap tuple is indexed would require
> following the `partseq` -> partition indirection down to each leaf, which
> amcheck does not yet implement.

**The trap is the default.** `bt_index_check(idx)` defaults to
`heapallindexed => false`, so the common call does not error — it returns
cleanly, having verified only that the (possibly empty) btree is well-formed.
An empty btree is perfectly well-formed. Only an explicit
`heapallindexed => true` raises, and an operator with a working structural
tripwire has no reason to pass it.

So the fork removed a guarantee and left a check that still looks authoritative.

## What implementing it requires

The index side already works — nothing about fingerprinting index tuples depends
on the heap. What is missing is the table side, and the shape is already
established in the fork by `spanning_backfill_leaf()` (`spanning_ddl.c`), which
solves the same traversal for a different purpose:

1. Enumerate every storage-bearing leaf beneath the root (`find_all_inheritors`,
   recursive; skip non-`RELKIND_RELATION` parents). Works for declarative
   partitions, `INHERITS` children, and mixed trees at any depth.
2. For each leaf, resolve its index-local `partseq`
   (`SpanningLookupPartseqByRelid`), and remap the index's root-relative key
   attnums — and, for a partial index, its predicate — to that leaf by column
   name (`spanning_remap_keyatts_to_leaf`).
3. Scan the leaf. For each live tuple: skip it if a partial index's predicate
   rejects it (`spanning_index_predicate_holds`); otherwise form the index datum
   with `FormIndexDatum`, overwrite the trailing discriminator with the leaf's
   `partseq`, and probe the bloom filter exactly as the stock path does.

Every one of those pieces exists and is exercised by the backfill and the
executor write paths. The work is assembling them behind amcheck's callback
rather than inventing anything.

Two details that will matter:

- **The root may have storage of its own.** An `INHERITS` parent holds rows
  directly, and those rows are indexed with a `partseq` like any leaf's — they
  must be scanned as part of the leaf set, not via the root's own table AM.
- **Sizing the bloom filter** currently uses the table's estimated tuple count;
  for a spanning index that estimate must come from the whole tree, not the
  (often empty) root.

## Why this and not a fork-specific function

A `pg_global_index_coverage()` that compares entry count to row count would also
catch the failure, and is much less work. It was proposed and is a reasonable
interim. But it only helps operators who learn that the fork ships its own
checker; anyone who wires up `amcheck` — the obvious, documented, correct thing
to do — keeps the blind spot. Fixing `heapallindexed` fixes it for people who
never read this file.

## Interim detection — the supported substitute today

This gap has a working substitute. Use it; do not read the sections above as
meaning spanning indexes cannot be checked at all.

**The test: compare live index entries to the tree's row count, and flag when
entries are FEWER than rows.** A unique index holding fewer entries than rows
means those rows are not indexed, so uniqueness is not enforced for them. There
is no benign explanation. Needs only `pageinspect`.

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;   -- same snapshot for both halves

SELECT sum(s.live_items)                                    -- raw item count
         - count(*) FILTER (WHERE s.btpo_next <> 0)         -- minus one high key
       AS entries                                           --   per non-rightmost leaf
  FROM generate_series(1, pg_relation_size('<index>')/8192 - 1) b
  CROSS JOIN LATERAL bt_page_stats('<index>', b) s
 WHERE s.type = 'l';

SELECT count(*) FROM <root>;    -- inheritance: counts the WHOLE tree by default
COMMIT;
```

Two mistakes have each been made independently, by different people, on the same
index — both make a damaged index look healthy or the numbers look incoherent:

- **Forgetting the high keys.** `bt_page_items` / `bt_page_stats` count the high
  key as an item on every *non-rightmost* leaf, so a raw `live_items` sum reads
  high by roughly one per leaf page. On a real 7,460-leaf index that was 7,459
  phantom entries — enough to shift a reported deficit by that exact amount. The
  `FILTER (WHERE s.btpo_next <> 0)` term above is the correction.
- **A non-recursive row count.** The denominator must cover the *whole
  transitive tree*. `SELECT count(*) FROM root` does that by default for
  inheritance; enumerating children by hand does not, and missing a deep child
  silently shrinks the population. One monitor reported 1,105,275 rows against a
  real 1,595,088 this way — its deficit was roughly right while its total was
  short by ~490,000, which made every reading look untrustworthy even though the
  verdict was correct.

Take both halves in one snapshot. Readings taken minutes apart on a live corpus
cannot be compared, and reconciling them wastes more time than the measurement.

**Do not substitute emptiness** (`leaf_pages = 0`, size, page count, tree level)
for this. Those detect only a *freshly* emptied index; once writes have re-armed
their own entries it regains pages and passes every structural measure while
still missing every pre-existing row — which is the state anyone actually
discovers, weeks later. Nor `pg_class.relpages`/`reltuples`, stale until
`ANALYZE`. The inverse reading of the same comparison (entries far exceeding
rows) indicates deferred-drain debris; both are cleared by a rebuild.

### Is the damage historical or ongoing?

Worth knowing before scheduling a repair: a rebuild that holds is a fix, one that
re-accumulates is a treadmill. If the key is a time-ordered uuid (v7), the newest
key in the index dates the most recent successful indexing:

```sql
-- newest key on the rightmost leaf vs newest row in the heap
SELECT to_timestamp(('x'||substring(replace(<id>::text,'-','') from 1 for 12))
                    ::bit(48)::bigint / 1000.0);
```

If the newest indexed key tracks the newest heap row, writes are being indexed
and the deficit is a fixed historical set — a rebuild is permanent. If it stops
at some past instant, indexing is still failing and the rebuild will not hold.
Measured this way on a live corpus: newest indexed key five minutes old against a
heap row 27 seconds old, confirming a historical deficit from a single event
seven weeks earlier.

### Why `bt_index_check` cannot be made to work by permissions

Worth stating plainly because it is easy to underread: the `heapallindexed`
refusal is **not** a privilege check. Running as superuser raises the identical
error. No grant, role change, or connection change reaches it — the verification
that would catch a hollow spanning index is unavailable to *every* caller on
*every* spanning index. Anything built on the assumption that `amcheck` covers
these indexes is resting on a check that does not run.
