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

## Interim detection

Until then, compare live index entries to the tree's row count and flag when
entries are **fewer** than rows. A unique index holding fewer entries than rows
means those rows are not indexed; there is no benign explanation. Count with
`pageinspect`, excluding the high key present on every non-rightmost leaf:

```sql
SELECT sum(s.live_items) AS entries
  FROM generate_series(1, pg_relation_size($1)/8192 - 1) b
  CROSS JOIN LATERAL bt_page_stats($1, b) s
 WHERE s.type = 'l';
```

Do not substitute emptiness (`leaf_pages = 0`, size, page count, tree level) for
this. Those detect only a *freshly* emptied index; once writes have re-armed
their own entries it regains pages and passes every structural measure while
still missing every pre-existing row — which is the state anyone actually
discovers, weeks later. The inverse reading of the same comparison (entries far
exceeding rows) indicates deferred-drain debris; both are cleared by a rebuild.
