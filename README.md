# ProgreSQL

**A PostgreSQL fork that adds native _spanning indexes_ — true cross-partition
`PRIMARY KEY` / `UNIQUE` enforcement across a partitioned table *or an inheritance
hierarchy*, without forcing the partition key into the constraint.**

> ⚠️ **Beta.** Cross-partition uniqueness is now tested for single-node
> correctness, concurrency, and crash recovery: every write path (INSERT, UPDATE
> incl. cross-partition moves, COPY, logical-replication apply, table rewrite)
> maintains the spanning index, the cross-partition uniqueness race is closed with
> a dedicated value lock, and the regression, isolation, crash-recovery,
> logical-replication, and `pg_upgrade` suites pass (plus a multi-client pgbench
> soak: zero duplicates, zero deadlocks). It is **not yet** battle-tested at scale
> or independently reviewed — keep backups and validate against your own workload
> before trusting production data to it.

Built on [PostgreSQL](https://github.com/postgres/postgres) 18
(`REL_18_STABLE`). Everything stock Postgres does, ProgreSQL does — plus one
feature the upstream planner / executor / access-method layers were extended to
support.

---

## The problem

In vanilla PostgreSQL, a `UNIQUE` or `PRIMARY KEY` on a partitioned table **must
include every partition-key column**:

```sql
-- Vanilla PG: rejected unless (id) includes the partition key (ts)
CREATE TABLE events (id bigint PRIMARY KEY, ts timestamptz)
  PARTITION BY RANGE (ts);
-- ERROR: unique constraint on partitioned table must include all
--        partitioning columns
```

That means you **cannot** have a globally-unique `id` across partitions when you
partition by `ts`. The usual workarounds — a shadow table, triggers, or an
application-level uniqueness check — are slow, racy, or both.

## What ProgreSQL adds

A **spanning index**: a real B-tree built on the partitioned *root* that stores
`(user_columns…, partseq)` for every live row in every partition, and enforces
uniqueness on just the user columns. The trailing `partseq` is a small,
index-local partition sequence number — a stable discriminator that survives OID
reuse and `pg_upgrade`. You opt in with the **`GLOBAL`** keyword. Cross-partition
`PRIMARY KEY` / `UNIQUE` simply works:

```sql
CREATE TABLE events (
    id   bigint NOT NULL,
    kind text,
    ts   timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL       -- unique across ALL partitions, not just within one
) PARTITION BY RANGE (ts);

CREATE TABLE events_2024 PARTITION OF events
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE events_2025 PARTITION OF events
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

INSERT INTO events VALUES (1, 'a', '2024-06-01');   -- ok
INSERT INTO events VALUES (1, 'b', '2025-06-01');   -- ERROR: duplicate key (id)=(1)
                                                    --        across partitions ✔
```

`GLOBAL` is the explicit opt-in, available three ways — a table constraint
(`PRIMARY KEY (…) GLOBAL` / `UNIQUE (…) GLOBAL`), a standalone index
(`CREATE UNIQUE INDEX … ON root (…) GLOBAL`), or `ALTER TABLE … ADD CONSTRAINT …
GLOBAL`. Ordinary partitioned tables behave exactly like stock PostgreSQL. (The
syntax aligns with Oracle's `GLOBAL` partitioned indexes and the in-core
"global index" proposal under discussion on pgsql-hackers.)

A spanning `PRIMARY KEY` / `UNIQUE` can also be the target of a **foreign key**:
`REFERENCES events (id)` from another table is accepted and fully enforced
(INSERT-time existence checks plus `ON DELETE` / `ON UPDATE` `RESTRICT` /
`CASCADE` / `SET NULL` / `SET DEFAULT`, on operations through the partitioned root
*and* on individual leaf partitions), since the spanning index gives the
referenced side a single cross-partition unique key to point at. When the root is
an inheritance tree, the referenced row is resolved into whatever typed child
actually holds it (see below) — the FK works across child tables, not just sibling
partitions.

### Beyond partitions: spanning over table inheritance

A spanning index isn't limited to declarative partitioning. Put `GLOBAL` on an
ordinary **inheritance root** (`INHERITS`) and the index enforces uniqueness across
the *whole* tree — every storage-bearing leaf at any depth, across both time
buckets *and* typed children. The pattern is "inherit at the top, partition/bucket
the leaves":

```sql
CREATE TABLE ent (id int NOT NULL, ts date NOT NULL, kind text);
CREATE TABLE msg (body  text) INHERITS (ent);     -- a typed child
CREATE TABLE fct (claim text) INHERITS (ent);     -- another typed child
CREATE TABLE msg_2026_01 () INHERITS (msg);       -- time-bucket leaves
CREATE TABLE fct_2026_01 () INHERITS (fct);

CREATE UNIQUE INDEX ent_id_g ON ent (id) GLOBAL;  -- one index spans the whole tree

INSERT INTO msg_2026_01 (id, ts, kind) VALUES (1, '2026-01-10', 'm');   -- ok
INSERT INTO fct_2026_01 (id, ts, kind) VALUES (1, '2026-01-12', 'f');   -- ERROR: id=1
                                                        --   collides across a typed child ✔
```

Build- and live-time enforcement, dynamic `CREATE … INHERITS` / `ALTER … INHERIT`
/ `NO INHERIT` / `DROP` of leaves, reordered-column children, multiple-inheritance
diamonds, `COPY`, logical replication, and **cross-child foreign keys** (a `REFERENCES`
to the root resolves into whichever typed child holds the row) are all covered by the
`progresql_inherit` suite. Leaf enumeration and leaf→root resolution are recursive,
so an *existing* multi-level declarative tree (sub-partitions of partitions) is
spanned by the same path.

### Introspecting a spanning index (the client-tooling contract)

Three built-in functions are the **supported introspection contract** for client
tooling (ORM adapters, schema-dump tools) to detect the fork and round-trip a
spanning index through a schema dump — so a client never has to read the internal
`indnuniqatts` key-padding, and can depend on these by name across PG-minor
forward-ports:

| Function | Returns |
|---|---|
| `progresql_version()` | the fork **release** version (`'0.2.7'`, matching the `v18.3-X.Y.Z` tag), distinct from the PostgreSQL base reported by `server_version` — the supported fork-detection + version-gate hook (stock PostgreSQL has no such function) |
| `pg_index_is_global(regclass)` | whether an existing index is a spanning index; `NULL` for a non-index argument |
| `pg_index_global_columns(regclass)` | the index's user-facing key column names, **excluding** the trailing `partseq` discriminator; `NULL` for a non-index or expression key |

For a `PRIMARY KEY` / `UNIQUE` *constraint*, resolve its `conindid` first and pass
that to `pg_index_is_global` / `pg_index_global_columns`.

#### If your tooling reads `indkey` directly — read this first

Tools written against stock PostgreSQL generally derive a table's key columns
straight from `pg_index.indkey`. **That yields a wrong answer on a spanning
index**, and the failure is silent, so it is worth understanding before you put a
`GLOBAL` primary key on anything.

A spanning index carries its discriminator as a trailing key column, and that
column is `tableoid` — a **system** column, attnum **-6**:

```
 idx     | indisprimary | indnatts | indnkeyatts | indnuniqatts | indkey
 mf_pkey | t            |        2 |           2 |            1 | 1 -6
```

Stock PostgreSQL rejects `CREATE INDEX ... (tableoid)` outright (`index creation
on system columns is not supported`), so a negative attnum in `indkey` is a state
vanilla guarantees is unreachable — which is precisely why existing tooling does
not defend against it. Note also that `indnkeyatts` **counts** the discriminator,
while `indnuniqatts` is the user-facing key count.

The concrete failure: an ORM that derives the primary key from `indkey` without
filtering system columns concludes the table has a composite key `(id, tableoid)`.
Observed with the Rails PostgreSQL adapter, whose `primary_keys` join uses
`indkey[idx]` with no attnum filter — every `reload` / `update` / `destroy` then
emits

```sql
WHERE "t"."id" = $1 AND "t"."tableoid" IS NULL
```

`tableoid` is never NULL, so those statements match zero rows **while reporting
success**: `update!` returns true and nothing is written.

Two ways to be correct:

- **Preferred:** use `pg_index_global_columns()` above, which excludes the
  discriminator by construction.
- **For tooling you must patch rather than replace:** filter system attnums —
  `AND a.attnum > 0`. This is a no-op against stock PostgreSQL (which can never
  have a negative attnum there) and load-bearing here, so it is safe to carry
  upstream in a shared tool rather than maintaining a fork-specific branch.

---

## Quickstart

Install the prebuilt binaries from the Homebrew tap (keg-only, so it won't
clash with a stock `postgresql`):

```sh
brew install twilightcoders/tap/progresql
# brew prints the keg path; add its bin/ to PATH, or use it explicitly
```

Or build it like any PostgreSQL source tree:

```sh
git clone -b progresql-18 https://github.com/TwilightCoders/progresql.git
cd progresql

./configure --prefix="$PWD/install" --enable-debug --enable-cassert
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" && make install

install/bin/initdb -D data
install/bin/pg_ctl -D data -l server.log start
install/bin/psql -d postgres   # then paste the demo above
```

(`--enable-cassert` is for development; drop it for a release build. There's also
a `build.sh` wrapper at the repo root.)

Run the feature's regression suites:

```sh
make -C src/test/regress check    # 243/243, includes the `progresql*` suites
make -C src/test/isolation check  # 122/122, includes the spanning-* specs
```

---

## How it works (the nerd section)

### The index shape
- A new `pg_index` column, **`indnuniqatts`**, marks a spanning index: a value
  `> 0` says "this index stores N+1 key columns, but uniqueness is enforced on
  only the first `indnuniqatts`." The trailing column is an honest **`int4`
  `partseq`** — an index-local partition sequence number recorded in the
  **`pg_index_partition`** catalog (`(index, partseq) → partition`). partseq is
  allocated once when a partition joins the index's domain and never reused, so
  spanning entries survive partition OID reuse and `pg_upgrade`.
- So two rows that share a user key but live in *different* partitions are
  **distinct entries** in the B-tree (different `partseq`), yet a uniqueness
  check that compares only the leading `indnuniqatts` columns still catches the
  collision.

### Uniqueness checks across partitions
- `_bt_check_unique` (`nbtinsert.c`) compares only the first `indnuniqatts`
  columns. When a candidate duplicate is found, the conflicting tuple lives in a
  *partition*, not the root — so the check reads the `partseq` key column,
  resolves it to the partition via `pg_index_partition`, opens that partition,
  and performs the heap-liveness probe there.
- Stale entries are retired with the standard `kill_prior_tuple` / `LP_DEAD`
  mechanism so aborted / deleted rows don't raise false conflicts. A spanning
  index's leaf pages skip the pre-split heap-probing deletion passes (their
  "heap" is the storage-less root); VACUUM and the drain retire dead entries.

### Writing the index (executor)
- After a row lands in a leaf partition, `ExecInsertSpanningIndexTuples`
  (`execIndexing.c`) writes `(user_cols…, partseq)` into the root's spanning
  index. INSERT and COPY both route through this path.
- A **per-statement cache** on `EState` hoists the
  `get_partition_ancestors` → `table_open` → `index_open` → `BuildIndexInfo`
  resolution out of the per-row hot path (built lazily per partition, released
  by `FreeExecutorState`). On a 3-partition bench this cut bulk-INSERT spanning
  overhead ~7.5× versus the naive per-row reopen.
- `UPDATE` skips the spanning write when the unique-key columns are unchanged —
  the existing entry stays valid through the heap HOT chain. A leaf has no local
  index on the spanning-key columns, so those columns are made **HOT-blocking**
  on the leaf (`RelationGetIndexAttrBitmap`): a spanning-key UPDATE is therefore
  non-HOT, the old tuple dies normally, and the stale spanning entry is retired —
  without this, an in-place HOT update would leave a permanent false conflict.

### DDL lifecycle (the hard part)
The spanning index has to stay correct across every operation that mutates the
partition tree (`tablecmds.c`, `index.c`, `heap.c`):
- **DROP / DETACH** — remove only the departing partition's entries, keyed by
  `partseq` (not heap TID, which collides across partitions).
- **ATTACH** — allocate the attaching partition's `partseq` and backfill its
  existing rows into the spanning index.
- **REINDEX** — `index_build` leaves the root index empty (the root has no
  storage), so the rebuild repopulates `(key, partseq)` from every live row in
  every partition.
- **COPY** — flows through the executor INSERT path, so it's covered for free.
- **dump / restore / `pg_upgrade`** — `pg_get_indexdef` / `pg_get_constraintdef`
  and `pg_dump` clip the trailing discriminator and emit `GLOBAL`, so a dump
  replays as a *spanning* index. Without this the restore would silently
  downgrade to a plain composite index and lose cross-partition uniqueness.

### Planner
- Spanning indexes are hidden from the planner's index list (`plancat.c`): rows
  live in partitions, never in the root's (nonexistent) storage, so the planner
  must never consider scanning them.

### VACUUM — the deferred coalesced drain
- A spanning index's entries for one partition are scattered across the whole
  B-tree (the discriminator is the *trailing* key, which keeps the uniqueness
  probe O(log n) on the hot write path). So retiring one leaf's dead entries
  needs a full index scan — and N leaf VACUUMs would each scan the whole index:
  **O(N²) per sweep**.
- The intended optimization (GUC `spanning_defer_vacuum`) is a coalesced drain: a
  leaf VACUUM *enqueues* its dead entries to the durable **`pg_spanning_drainq`**
  catalog and leaves the heap slots `LP_DEAD` (un-reaped, so they can't be
  reused). A single **drain** then retires every queued partition's entries in
  *one* index scan and reaps the now-safe slots — **O(N)**. Measured: per-leaf
  VACUUM cost goes from growing-with-N to flat; the sweep drops ~10× at 64
  partitions. The drain runs as an autovacuum work item or on demand via
  `pg_drain_spanning_index(regclass)`.
- **This path is unsafe and defaults to `off`.** Its correctness rested on the
  invariant that a held `LP_DEAD` slot can only be reaped by the gated drain — so
  a stale entry could never alias a live row before being retired. Under sustained
  UPDATE churn that invariant does not hold in practice: the drain fails to retire
  some dead entries, duplicates accumulate for a single live `(id, partseq)`, and
  once a stale entry's slot is reused by a live tuple the uniqueness probe raises a
  **phantom `duplicate key` violation on a legitimate UPDATE**. The damage is
  index-only (the heap stays correct) but permanent until `REINDEX`, and
  `bt_index_check` reports `item order invariant violated`. Do not enable it.
- The **eager** path (the default): each leaf's VACUUM retires its own spanning
  entries before the heap slots are reaped. Correct, and pinned by
  `progresql_churn_vacuum`. It reinstates the O(N²) sweep cost the drain was
  written to avoid — measured at ~740µs/row on a rebuild burst, bounded and
  non-correctness. Fixing or removing the deferred drain is tracked in
  `docs/plans/spanning-dedup-corruption.md`.

---

## What changed vs. mainline

A focused diff on top of `REL_18_STABLE`
(`git diff upstream/REL_18_STABLE..progresql-18`):

| Area | Files | Why |
|------|-------|-----|
| Catalog | `catalog/pg_index.h` (`indnuniqatts`), `catalog/pg_index_partition.{h,c}` ((index,partseq)→partition map), `catalog/pg_spanning_drainq.{h,c}` (deferred-vacuum queue), `catversion.h`, `catalog/index.c`, `catalog/heap.c` | New index metadata; partseq allocation/resolution; drain queue; build / reindex / drop hooks; int4 discriminator stamp |
| Index AM (nbtree) | `access/nbtree/nbtinsert.c`, `nbtree.c`, `nbtsort.c`, `access/index/genam.c`, `indexam.c` | Cross-partition uniqueness check; build over partitions; coalesced multi-partseq drain; skip heap-probing pre-split deletion; key-description |
| Commands / GUC | `commands/indexcmds.c`, `commands/tablecmds.c`, `commands/vacuum.c`, `parser/gram.y`, `parser/parse_utilcmd.c`, `utils/misc/guc_tables.c` | `GLOBAL` syntax; spanning-index creation / propagation; DROP / DETACH / ATTACH / REINDEX; `spanning_defer_vacuum` GUC |
| Executor | `executor/execIndexing.c`, `executor/nodeModifyTable.c`, `nodes/execnodes.h` | Per-row insert hook + per-statement cache; UPDATE handling |
| VACUUM / autovacuum | `access/heap/vacuumlazy.c`, `postmaster/autovacuum.c`, `utils/cache/relcache.c` | Deferred enqueue + coalesced drain + `pg_drain_spanning_index()`; autovacuum drain work item; HOT-blocking spanning keys on leaves |
| Dump / restore | `utils/adt/ruleutils.c`, `bin/pg_dump/pg_dump.{c,h}` | Clip discriminator + emit `GLOBAL` so dumps replay as spanning indexes |
| Planner | `optimizer/util/plancat.c` | Hide spanning indexes from path generation |
| Tests | `src/test/regress/{sql,expected}/progresql*`, `parallel_schedule` | Feature, DDL lifecycle, partseq, GLOBAL, HOT, vacuum collision, OID reuse, drain, dump round-trip |

## Status

- Based on **PostgreSQL 18** (`REL_18_STABLE`).
- **243/243** core regression tests pass, including the ProgreSQL suites
  (`progresql`, `progresql_ddl`, `progresql_partseq`, `progresql_global`,
  `progresql_hot`, `progresql_vacuum_collision`, `progresql_oid_reuse`,
  `progresql_concurrency`, `progresql_drain`, `progresql_fk`, `progresql_inherit`,
  `progresql_dumpdef`), plus **122/122** isolation tests.
- **TAP**: `recovery/049_spanning_crash` (crash recovery, 17/17) and
  `subscription/031_spanning` (logical replication, 7/7) pass, with inheritance
  variants `recovery/050_spanning_inherit_crash` and
  `subscription/032_spanning_inherit` extending both to `INHERITS` trees.
- **Soak-verified** (cassert + `amcheck` oracle): the cross-partition uniqueness
  race, the deferred-vacuum drain (6 h scale + a 24-partition run), 25 crash/
  recover cycles, local-index leaves, and concurrent index DROP/CREATE during a
  drain — all clean (0 duplicates, 0 crashes). See `src/test/spanning/`.
- Warning-clean under PostgreSQL's standard strict flags.
- Branch layout: `master` tracks upstream PostgreSQL; the spanning-index feature
  is maintained as a rebasable patch series on a working branch and shipped via
  the `twilightcoders/tap` Homebrew tap (keg-only `progresql`).

See [`PRODUCTION_READINESS.md`](./PRODUCTION_READINESS.md) for the honest
production/upstream gap assessment, and
[`docs/disaster-recovery.md`](./docs/disaster-recovery.md) for the
backup/DR runbook.

## Backup & disaster recovery

The recommended production posture keeps the fork on the **primary only** and uses
**stock PostgreSQL as the durable copy** — so all your backup/DR tooling is the
battle-tested, supported kind:

- **ProgreSQL primary** enforces partitioning + cross-partition uniqueness via the
  spanning index.
- **Logical replication → a stock PostgreSQL replica.** Logical decoding ships
  ordinary heap row changes (the spanning index is index-level state; the
  `partseq` discriminator lives in the index, not the heap), so a vanilla
  subscriber applies them with no knowledge of the fork. The replica is a faithful
  data copy you can back up with `pg_dump` / pgBackRest / PITR / etc.
- **The spanning index carries no information not derivable from the data**, so
  recovery is simple: stand up a fresh ProgreSQL, apply your `GLOBAL` schema (from
  source control), and load the data back — the rows route to partitions and the
  spanning index is rebuilt, re-enforcing uniqueness on the way in.

One required setting: because a spanning-PK leaf has no local primary key,
`UPDATE`/`DELETE` won't logically replicate unless the leaves are set to
**`REPLICA IDENTITY FULL`**. INSERT replicates without it.

This is validated end-to-end (a live master→replica→rebuild drill, and the
`031_spanning` TAP test). Full runbook, including the restore recipe and
monitoring, in [`docs/disaster-recovery.md`](./docs/disaster-recovery.md).

## Tracking upstream

This repo keeps `postgres/postgres` as the `upstream` remote. To move onto a
newer PG18 minor:

```sh
git remote add upstream https://github.com/postgres/postgres.git   # once
git fetch upstream
git rebase upstream/REL_18_STABLE progresql-18
```

## Limitations

- The feature is opt-in via the `GLOBAL` keyword; plain partitioned and plain
  inheritance tables are untouched — `GLOBAL` is the only way to request a spanning
  index. (A *single table* that both `INHERITS` and is `PARTITION BY` is still
  rejected, exactly as in stock PostgreSQL. That is separate from a `GLOBAL` index
  on an inheritance *root* whose children are themselves partitioned/bucketed,
  which is fully supported — see "spanning over table inheritance" above.)
- A `GLOBAL` index built over an *existing* multi-level tree — declarative
  sub-partitions, or inheritance depth — spans every storage-bearing leaf at any
  depth (enumeration is recursive). What is still rejected at DDL is *incrementally*
  sub-partitioning a partition (or `ATTACH`ing an already-partitioned table)
  **after** a spanning index already exists on a *declarative* root: the new
  grandchildren would have no `partseq` and would silently escape the index. The
  order matters — build the multi-level tree first, then add `GLOBAL`; or use an
  inheritance root, under which depth can be added freely at any time.
- A partial `GLOBAL` index (`CREATE UNIQUE INDEX ... WHERE <pred> GLOBAL`) is
  supported, and its **predicate columns block HOT updates on the leaves** — the
  same way its key columns do, and for a sharper reason: an update that moves a
  row across the predicate boundary changes whether the row belongs in the index
  at all, so it must not be HOT. Expect a change to a predicate column to cost a
  cold (non-HOT) update. (Before 0.2.6 the predicate was accepted and stored but
  never evaluated, so the index silently enforced uniqueness over *every* row —
  a strictly tighter constraint than declared. Upgrading fixes new writes; an
  index built under the old behavior holds entries for rows the predicate
  rejects, so `REINDEX` it to drop them.)
- **`ALTER TABLE ... NO INHERIT` does not remove a departing child's foreign-key
  clones** (known gap). A child joining a spanning tree acquires the
  referenced-side FK clones of every key targeting a spanning ancestor; nothing
  removes them when it leaves, so — unlike `DETACH PARTITION`, which does the
  equivalent cleanly — a child cannot be detached and then dropped. `DROP TABLE`
  reports the *base* constraint as dependent (upstream's rule: the clone is an
  internal dependency of the base, so Postgres names the owner), and `CASCADE`
  therefore drops the base along with every one of its clones on other tables —
  removing referential integrity from tables unrelated to the one being dropped.
  Note this last part is stock PostgreSQL behavior, reproducible with declarative
  partitioning and no `GLOBAL` index; the fork's gap is only the missing
  `NO INHERIT` counterpart. Until it is fixed, remove such a child by dropping
  each affected base constraint, dropping the table, and recreating the
  constraints **in a single transaction** — the recreate regenerates clones for
  the remaining children, and being one transaction there is no window in which
  integrity is absent.
- The per-statement cache is exactly that — per statement; it is rebuilt for
  each top-level DML.
- **Logical replication of `UPDATE`/`DELETE`** from a spanning-indexed table
  requires `REPLICA IDENTITY FULL` on the leaf partitions (a spanning-PK leaf has
  no local primary key to serve as the replica identity). INSERT needs nothing
  extra. See [`docs/disaster-recovery.md`](./docs/disaster-recovery.md).
- This is a **fork** of PostgreSQL: running it means you maintain the divergence
  (forward-porting minors yourself). The recommended way to bound that risk is to
  confine the fork to the primary and keep a stock-PostgreSQL logical replica as
  the supported backup/DR target (see *Backup & disaster recovery* above). Single-
  node correctness is extensively tested; it has not had independent PG-internals
  review and is not (yet) proposed for upstream merge.

## License

ProgreSQL is distributed under the **PostgreSQL License**, the same terms as
upstream PostgreSQL. See [`COPYRIGHT`](./COPYRIGHT). ProgreSQL's modifications
are offered under the same license.
