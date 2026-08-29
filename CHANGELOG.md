# Changelog — ProgreSQL spanning indexes

Changes ProgreSQL adds on top of stock PostgreSQL (`REL_18_STABLE`). Vanilla
PostgreSQL behavior is unchanged unless a table opts in with the `GLOBAL` keyword.
Newest first.

## Unreleased

### Fixed
- **`REINDEX` of a spanning (`GLOBAL`) index rooted on an inheritance parent
  rebuilt it EMPTY**, silently disabling cross-child uniqueness. `btbuild`
  deliberately skips the root heap scan for any spanning index (a leaf's rows
  must carry a partseq discriminator the ordinary build callback would not
  store), so repopulating from the leaf set is not an optimisation -- it is the
  only thing that puts entries in the index at all. That repopulation was gated
  on the root's relkind being `RELKIND_PARTITIONED_TABLE`, so a root that is an
  ordinary `INHERITS` parent (`RELKIND_RELATION`) fell through it and was left
  hollow. The gate now keys on the spanning marker alone, exactly as
  `nbtsort.c` already keys the scan skip.

  The failure was undetectable from inside: an empty btree is structurally
  valid, so `bt_index_check` reported it clean, and `heapallindexed` -- the check
  that would have caught it -- is unsupported on spanning indexes.

  **To detect a damaged spanning index, compare its live entry count to the row
  count of its tree, and flag when entries are FEWER than rows.** A unique index
  holding fewer entries than rows means those rows are not indexed, so uniqueness
  is not enforced for them; there is no benign explanation. Count with
  `pageinspect`, excluding the high key that `bt_page_items` reports on every
  non-rightmost leaf:

  ```sql
  SELECT sum(s.live_items) AS entries
    FROM generate_series(1, pg_relation_size($1)/8192 - 1) b
    CROSS JOIN LATERAL bt_page_stats($1, b) s
   WHERE s.type = 'l';
  ```

  Do **not** rely on emptiness alone (`leaf_pages = 0`, size, page count, tree
  level): those detect only a *freshly* hollowed index, and once inserts have
  re-armed their own entries a hollow index regains pages and passes every
  structural measure while still missing every pre-existing row. Nor on
  `pg_class.relpages`/`reltuples`, which are stale until `ANALYZE`. The same
  comparison run the other way -- entries far exceeding rows -- indicates
  deferred-drain debris (see 0.2.5); both conditions are cleared by a rebuild.

  **Recovering an index hollowed by an earlier `REINDEX`:** upgrading fixes the
  rebuild path but does not repopulate an already-hollow index. Recreate it --
  `DROP INDEX` + `CREATE UNIQUE INDEX ... GLOBAL`, or for a constraint-backed
  one `ALTER TABLE ... DROP CONSTRAINT` + `ALTER TABLE ... ADD PRIMARY KEY (...)
  GLOBAL` -- inside a transaction, so a failure rolls the drop back rather than
  leaving no index at all. The rebuild doubles as an audit: if rows violating
  uniqueness accumulated while the index was hollow, `CREATE` refuses and names
  the key.
- **A table rewrite of an inheritance child corrupted the spanning index.**
  `VACUUM FULL`, `CLUSTER`, and rewriting `ALTER TABLE` forms move every TID, so
  the leaf's index entries must be retired and rebuilt. That was gated on
  `relispartition`, true only for *declarative* partitions, so an `INHERITS`
  child kept its pre-rewrite entries: the uniqueness probe would follow one into
  a block compaction had removed and raise a bare `could not read blocks ...` from
  an ordinary `INSERT`, and a deleted key could never be reused. Plain `VACUUM`
  did not heal it. The gate now matches the one the DROP/DETACH cleanup path
  already used (declarative partition **or** inheritance child), and the leaf's
  old entries are now retired rather than orphaned under a discarded partseq --
  which previously left every rewrite adding a full set of entries that nothing
  would ever reclaim.
- **A parallel index build of a spanning index on an inheritance root produced a
  malformed duplicate entry per root row.** `btbuild` skips the root heap scan
  for spanning indexes only on the *serial* path; the parallel heap scan has no
  such guard. A declarative root is excluded because it has no storage to scan,
  but an inheritance root does, so a build large enough to go parallel indexed
  the root's own rows with the ordinary callback -- storing the raw `tableoid`
  instead of a `partseq` -- and the backfill then indexed them again correctly.
  Measured at 241,083 entries for 120,100 rows. Spanning index builds are now
  never parallelised. Enforcement was not lost, so the only symptom was an index
  roughly twice the size it should be.
- **`REINDEX ... CONCURRENTLY` was not refused** for a spanning index on an
  inheritance root. The refusal was gated on the same relkind test, so instead
  of erroring it produced the same hollow index by a path that has no
  repopulation step at all. It now keys on the index being spanning.

These four share one root cause: a predicate that is true only for declarative
partitions (`relispartition`, or `relkind = RELKIND_PARTITIONED_TABLE`) standing
in for "is this under a spanning root", which is equally true of `INHERITS`
children. `nbtsort.c` had already reached the correct rule and says so in its own
comment -- key on the spanning marker, not the root's relkind -- and the other
paths did not ask. Every such predicate in spanning-reachable code has now been
audited against that rule.

Pinned by `progresql_reindex`, covering both root kinds, `REINDEX TABLE`, the
root's own rows, the `CONCURRENTLY` refusal, rewrite-then-probe (including that a
deleted key stays reusable), and a parallel-eligible build.

## 2026-08-13 (v18.3-0.2.6)

### Fixed
- **A `WHERE` predicate on a `GLOBAL` index is now enforced.** It was parsed,
  recorded in `pg_index.indpred`, and echoed back by `pg_get_indexdef` — but
  never evaluated, so every row got an index entry and uniqueness applied across
  all of them. That is a strictly **tighter** constraint than the one declared,
  and it failed silently: rows the predicate excludes still collided, and
  `CREATE INDEX` could fail over pre-existing data that was perfectly legal under
  the declared predicate. The predicate is now evaluated on all three paths that
  maintain a spanning index — the executor insert path, the logical-replication
  apply path, and the `CREATE INDEX` / `ATTACH` backfill. Because a spanning
  index lives on the root while its predicate is evaluated against a *leaf*
  tuple, the predicate's `Var`s are remapped to the leaf by column name, the same
  way the key attnums already were; a leaf whose columns are ordered differently
  from the root would otherwise have tested the wrong column entirely.
- **A partial spanning index's predicate columns now block HOT updates on the
  leaves**, as its key columns already did. An update that moves a row across the
  predicate boundary changes whether the row belongs in the index at all; if it
  went HOT the old entry would survive as a redirect and resolve through the HOT
  chain to the new tuple, raising a phantom `duplicate key` violation on exactly
  the "close this version, insert its replacement" pattern partial indexes exist
  to serve. Stock gets this via `ii_Predicate`'s varattnos; a spanning index is
  not in the leaf's own index list, so the fork must contribute them itself.
  Expect a change to a predicate column to cost a cold (non-HOT) update.
- **Upgrading:** new writes are correct immediately, but an index **built** under
  the old behavior still holds entries for rows its predicate rejects. `REINDEX`
  any partial `GLOBAL` index created before 0.2.6 to drop them.

Pinned by `progresql_partial`, which covers both directions at every path —
rows the predicate rejects must coexist across leaves, rows it accepts must still
collide — plus supersede/replace churn and inheritance children whose column
order diverges from the root and from each other.

## 2026-08-02 (v18.3-0.2.5)

### Fixed
- **The deferred VACUUM drain corrupted the spanning index under churn; it is now
  disabled by default** (`spanning_defer_vacuum` defaults to `off`). Under
  sustained UPDATE-heavy churn — both delete+recreate and repeated cold-UPDATE —
  the coalesced drain failed to retire some dead spanning-index entries. Duplicate
  entries accumulated for a single live `(id, partseq)`, and once a stale entry's
  heap slot was reused by a live tuple, the cross-partition uniqueness probe
  examined it and raised a **phantom `duplicate key` violation on a legitimate
  UPDATE**. It also wedged the logical-replication apply worker and, on
  `--enable-cassert` builds, tripped page-split assertions. `bt_index_check`
  reports `item order invariant violated`. The heap was never wrong — the damage
  is index-only — but it is permanent until `REINDEX`. This is a pre-existing
  defect in the deferred path, not a regression in this release. The **eager**
  path (each leaf's VACUUM retires its own entries before the heap slots are
  reaped) is correct and is now the default; it costs ~740µs/row on a rebuild
  burst, which is bounded and not a correctness concern. Existing installations
  should set `spanning_defer_vacuum = off` and `REINDEX` any spanning index that
  `bt_index_check` reports as damaged. Pinned by `progresql_churn_vacuum`;
  full analysis in `docs/plans/spanning-dedup-corruption.md`.
- **The apply worker's cross-partition conflict probe crashed on assert-enabled
  builds.** Resolving the conflicting tuple fetched it from its leaf with an
  *unregistered* snapshot, tripping
  `Assert("snapshot->regd_count > 0 || snapshot->active_count > 0")` in
  `HeapTupleSatisfiesMVCC` and crash-looping the server on any spanning conflict.
  The probe now pushes the snapshot active for the duration of the fetch, mirroring
  `FindConflictTuple`. Assert-disabled builds were not affected in practice, but
  the snapshot could in principle be invalidated mid-fetch. Guarded by
  `033_spanning_conflict.pl` run against a `--enable-cassert` build — the
  configuration that catches it.
- **A cross-partition (spanning / `GLOBAL`) uniqueness conflict hit by the
  logical-replication apply worker is now classified as a conflict**
  (`confl_insert_exists` / `confl_update_exists` in
  `pg_stat_subscription_stats`), the same as an ordinary unique index — instead
  of being raised as a raw, unclassified `apply_error` that the worker retried
  forever. A spanning leaf has no local unique index, so uniqueness is enforced
  only by the spanning index on the partitioned root, which the apply worker
  maintains out-of-band; that path previously raised a bare violation *outside*
  `CheckAndReportConflict`, so the conflict never reached PG 18's conflict
  detection (every `confl_*` counter stayed 0) and was neither observable nor
  resolvable. The apply worker now maintains the spanning index with a
  non-blocking `UNIQUE_CHECK_PARTIAL` and, on a flagged conflict, resolves the
  conflicting cross-partition tuple (by the stored `partseq`) and reports it
  through the normal conflict path. The local-executor path is unchanged (a
  direct duplicate still raises immediately). Only affects tables with a spanning
  index under logical replication; pinned by `033_spanning_conflict.pl`.

## 2026-06-25 (v18.3-0.2.4)

### Fixed
- **A `FOREIGN KEY` referencing a spanning (`GLOBAL`) inheritance root failed to
  validate when the referencing table already held rows**, which broke
  `pg_dump` → restore: a restore loads the data first and *then* re-adds the
  foreign keys, and that bulk re-validation rejected every referencing row. The
  spanning rule "descend into the leaves rather than scanning `ONLY` the root"
  had been applied to the per-row referential checks (`RI_FKey_check`,
  `ri_Check_Pk_Match`) but not to `RI_Initial_Check`, the bulk validator that
  `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` runs. It now descends into the
  leaves there too, so adding such a key to a populated table — and the full
  dump → restore round-trip — succeeds. Cross-leaf FK cloning was confirmed
  deterministic and order-independent in the process. Pinned by a new `pg_dump`
  round-trip case (`006_spanning_roundtrip.pl`) and a determinism regress test
  (`progresql_fk_clone`).

## 2026-06-25 (v18.3-0.2.3)

### Changed
- **`progresql_version()` now returns the fork *release* version** (`'0.2.3'`,
  matching the `v18.3-X.Y.Z` tag) instead of a separate, hand-maintained
  "feature-set" number that had silently stayed at `'1.0'` across several feature
  releases. There is now one coherent fork version that can't drift from the
  tag / tap / image: clients still fork-detect (the function exists at all) and
  now version-gate on a number that actually moves (e.g. `>= '0.2.0'`). The
  `progresql_global` regress test pins the value so future changes stay visible.

## 2026-06-25 (v18.3-0.2.2)

### Fixed
- **Rare hang in cross-partition uniqueness checks under concurrent partition
  DDL.** When enforcing a `GLOBAL` unique/PK, `_bt_check_unique` opened a
  conflicting candidate's (and the new tuple's) partition with a heavyweight
  `AccessShareLock` while holding the index leaf's buffer content lock. If a
  `DROP` / `DETACH` / `TRUNCATE` held that partition's `AccessExclusiveLock` while
  inserts collided on its keys, the buffer content lock landed inside the wait
  cycle — invisible to the deadlock detector — and could hang. The self side now
  opens with `NoLock` (the executor already holds the partition's lock), and the
  candidate side acquires the lock non-blocking (`ConditionalLockRelationOid`),
  releasing the buffer and re-descending if a concurrent DDL holds it. (audit #2)

### Changed
- Internal: the ATTACH-time and CREATE-index-on-populated-root backfill paths now
  share a single `spanning_backfill_leaf()` helper. No behavior change. (audit #9)

## 2026-06-22

### Added
- **Spanning over table inheritance (`INHERITS`)** — a `GLOBAL` index on an
  inheritance root enforces cross-leaf uniqueness over the whole tree (across
  time buckets *and* typed children), with build/live INSERT-UPDATE enforcement,
  dynamic `CREATE…INHERITS` / `ALTER…INHERIT` / `NO INHERIT` / `DROP`, VACUUM +
  DHR drain, dump/restore, crash recovery, logical-rep + master↔master mesh, and
  multi-level declarative partitioning. Root-first DDL (`PRIMARY KEY (id) GLOBAL`
  before children) is supported; a heap-bearing root enforces its own direct rows.
- **Cross-child foreign keys** — a FK referencing a spanning root resolves into
  any typed child; `RESTRICT`/`CASCADE`/`SET NULL` fire on child DML; children
  added after the FK are enforced.
- **Public introspection contract for client tooling** (ORM adapters,
  schema-dump tools): `progresql_version()` returns the fork's
  feature-set version (the supported fork-detection hook — vanilla reports the
  same `server_version`); `pg_index_is_global(regclass)` reports whether an
  existing index is a spanning index; and `pg_index_global_columns(regclass)`
  returns an index's user-facing key columns (excluding the internal partseq
  discriminator) for the schema-dump round-trip. Clients depend on this
  documented API rather than the internal catalog representation.

### Changed
- `GLOBAL` is now accepted on any ordinary table (previously required existing
  children), enabling the root-first DDL pattern. It is still rejected on views,
  materialized views, foreign tables, partitions, and for exclusion constraints.

### Fixed
- The spanning TAP tests (`recovery/050_spanning_inherit_crash`,
  `subscription/031_spanning`, `subscription/032_spanning_inherit`) are now
  registered in `meson.build`; previously they ran only under the Makefile build
  (`prove` auto-discovers `t/*.pl`) and were silently skipped by a meson build.

## 2026-06-13

### Added
- **Foreign keys referencing a spanning (`GLOBAL`) primary/unique key** (#33).
  `REFERENCES parent (cols)` against a spanning key is accepted and fully
  enforced — insert-time existence checks plus `ON DELETE`/`ON UPDATE`
  `RESTRICT` / `NO ACTION` / `CASCADE` / `SET NULL` / `SET DEFAULT`, on operations
  through the partitioned root *and* on individual leaf partitions. New regress
  suite `progresql_fk`.
- **Backup / disaster-recovery runbook** ([`docs/disaster-recovery.md`](docs/disaster-recovery.md)):
  run the fork on the primary, logically replicate to a stock-PostgreSQL replica
  as the supported backup/DR target, and rebuild a ProgreSQL master from that
  data. Validated end-to-end.
- Soak harness knobs: `--local-index` (#38 deferred local-index oracle, with
  `heapallindexed` amcheck) and `--ddl-churn` (#41 concurrent index DROP/CREATE
  during a drain).

### Changed
- **`spanning_defer_vacuum` now defaults to `on`** (#39). Leaf VACUUM enqueues to
  the coalesced drain instead of an O(N²) per-leaf full index scan.
- **Deferred spanning vacuum now applies to leaves with local indexes** (#38),
  not just no-local-index leaves: the coalesced drain vacuums each leaf's local
  indexes for the dead set before reaping, so the deferral is universal.

### Fixed
- Deferred-drain corruption with multiple spanning indexes on one root (#40): the
  drain is now root-coordinated (frees a heap slot only after every spanning index
  on the root has retired its entry).
- Leaf relcaches are invalidated when a spanning index is built (#42), closing a
  latent stale-`rd_hotblockingattr` window.

### Verified (no code change)
- Crash recovery: `recovery/049_spanning_crash` TAP (17/17) + 25 kill-9/recover
  soak cycles.
- Logical replication: `subscription/031_spanning` TAP (7/7). `UPDATE`/`DELETE`
  replication of spanning-indexed tables requires `REPLICA IDENTITY FULL` on the
  leaves (documented).

## Earlier (campaign highlights)

### Added
- The **spanning index**: a single btree on the partitioned root keyed
  `(user_cols…, partseq)` that enforces uniqueness on the user columns across all
  partitions. Opt in via `GLOBAL` — table constraint, `CREATE UNIQUE INDEX …
  GLOBAL`, or `ALTER TABLE … ADD CONSTRAINT … GLOBAL`.
- Index-local `partseq` discriminator + `pg_index_partition` catalog (replacing an
  earlier `tableoid`-based scheme), making dump/restore and `pg_upgrade` correct
  by construction.
- DHR deferred coalesced VACUUM drain (`pg_spanning_drainq`,
  `pg_drain_spanning_index()`, autovacuum work-item trigger + launcher sweep).
- Catalog/DDL coverage: ATTACH backfill, DETACH/DROP retirement, REINDEX,
  TRUNCATE re-map, COPY, `pg_dump`/restore, `pg_upgrade`. `amcheck` made safe on
  spanning indexes.

### Fixed
- Cross-partition uniqueness race under concurrency (#34) — a value lock at the
  `_bt_doinsert` choke point.
- HOT key-change corruption (E7) — spanning keys are HOT-blocking on leaves.
- btree page-recycle crash under autovacuum (#35).
- partseq reuse on highest-partition detach (#2); drain-queue cleanup on
  DETACH/DROP (#4); TRUNCATE retirement crash-durability (#8).

See [`PRODUCTION_READINESS.md`](PRODUCTION_READINESS.md) for the full disposition
and [`docs/plans/`](docs/plans) for design records.
