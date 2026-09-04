--
-- progresql_ddl: DDL lifecycle for spanning indexes
--
-- Verifies the spanning index correctly tracks cross-partition uniqueness
-- across all DDL paths that mutate the partition tree:
--   - DROP partition
--   - DETACH partition (non-concurrent)
--   - COPY (executor INSERT path)
--   - REINDEX
--   - ATTACH partition (with pre-existing rows)
--   - CASCADE DROP cleanup
--
-- The cleanup path is keyed on (key, tableoid), not on heap TID, so two
-- partitions whose first row both live at heap TID (0,1) do not collide.
-- REINDEX repopulates the spanning index from leaf partitions after the
-- empty rebuild on the partitioned root.  ATTACH backfills the spanning
-- index with pre-existing rows in the attaching partition.
--

-- Setup
CREATE TABLE pgddl_data (
    id   bigint,
    kind text,
    ts   timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL
) PARTITION BY RANGE (ts);

CREATE TABLE pgddl_2023 PARTITION OF pgddl_data
    FOR VALUES FROM ('2023-01-01') TO ('2024-01-01');
CREATE TABLE pgddl_2024 PARTITION OF pgddl_data
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE pgddl_2025 PARTITION OF pgddl_data
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

-- Each partition's first row lands at heap TID (0,1), so the cleanup
-- callback used to false-positive across partitions.  Now it filters
-- by tableoid so only the targeted partition's entries are removed.
INSERT INTO pgddl_data VALUES (1, 'a', '2023-06-15 00:00:00+00');  -- pgddl_2023, TID (0,1)
INSERT INTO pgddl_data VALUES (2, 'b', '2024-06-15 00:00:00+00');  -- pgddl_2024, TID (0,1)
INSERT INTO pgddl_data VALUES (3, 'c', '2025-06-15 00:00:00+00');  -- pgddl_2025, TID (0,1)

-- Confirm cross-partition uniqueness before any DDL.
BEGIN;
INSERT INTO pgddl_data VALUES (1, 'dup', '2025-09-01 00:00:00+00');
ROLLBACK;

-- Section 1: DROP partition
-- Spanning entries for the dropped partition are removed; entries for
-- OTHER partitions (which share heap TIDs with the dropped one) are NOT
-- touched.

DROP TABLE pgddl_2023;
SELECT count(*) FROM pgddl_data;  -- 2 rows remain (id=2, id=3)

-- id=1 is re-insertable: only (1, pgddl_2023.OID) was cleaned.
INSERT INTO pgddl_data VALUES (1, 'a2', '2024-08-01 00:00:00+00');
SELECT id, kind FROM pgddl_data ORDER BY id;

-- (2, pgddl_2024.OID) and (3, pgddl_2025.OID) were preserved despite
-- sharing heap TID (0,1) with pgddl_2023's removed entry.
BEGIN;
INSERT INTO pgddl_data VALUES (2, 'dup-tid-collision', '2025-10-01 00:00:00+00');
ROLLBACK;
BEGIN;
INSERT INTO pgddl_data VALUES (3, 'dup-tid-collision', '2025-11-01 00:00:00+00');
ROLLBACK;

-- Section 2: DETACH partition (non-concurrent)
-- Cleanup runs before pg_inherits is updated, so get_partition_ancestors
-- still resolves the parent root and the index entries are correctly
-- removed without affecting other partitions.

INSERT INTO pgddl_data VALUES (2, 'b2', '2024-09-01 00:00:00+00');  -- pgddl_2024, TID (0,2)
INSERT INTO pgddl_data VALUES (3, 'c2', '2025-09-01 00:00:00+00');  -- pgddl_2025, TID (0,2)
SELECT count(*) FROM pgddl_data;  -- 5 rows

ALTER TABLE pgddl_data DETACH PARTITION pgddl_2024;
SELECT count(*) FROM pgddl_data;   -- pgddl_2024 rows removed from parent view
SELECT count(*) FROM pgddl_2024;   -- rows still exist in detached table

-- id=1 and id=2 (formerly in pgddl_2024) are re-insertable in the parent.
-- pgddl_2025 entries with overlapping heap TIDs are unaffected.
INSERT INTO pgddl_data VALUES (1, 'a3', '2025-02-01 00:00:00+00');
INSERT INTO pgddl_data VALUES (2, 'b3', '2025-03-01 00:00:00+00');
SELECT id, kind FROM pgddl_data ORDER BY id;

DROP TABLE pgddl_2024;

-- Section 3: COPY populates spanning index correctly
-- COPY routes each tuple through the executor INSERT path, which in turn
-- calls ExecInsertSpanningIndexTuples to update the root's spanning index.

COPY pgddl_data (id, kind, ts) FROM STDIN;
10	copy-a	2025-07-01 00:00:00+00
11	copy-b	2025-08-01 00:00:00+00
\.

SELECT count(*) FROM pgddl_data;

-- COPYed rows are in the spanning index; same-partition duplicate rejected.
BEGIN;
INSERT INTO pgddl_data VALUES (10, 'dup-copy', '2025-09-01 00:00:00+00');
ROLLBACK;

-- Section 3b: COPY enforces cross-partition uniqueness (single, batch, and a
-- post-COPY INSERT).  COPY has two insert paths -- per-row and multi-insert
-- batching -- and both must maintain the spanning index.  Before they did, a
-- COPYed row was absent from the spanning index and a later duplicate (same- or
-- cross-partition) was silently accepted under a PRIMARY KEY.
CREATE TABLE pgddl_copy (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE pgddl_copy_a PARTITION OF pgddl_copy
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE pgddl_copy_b PARTITION OF pgddl_copy
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
-- distinct keys across partitions load fine (exercises multi-insert batching)
COPY pgddl_copy (id, ts) FROM STDIN;
1	2024-06-01 00:00:00+00
2	2025-06-01 00:00:00+00
\.
-- a COPYed key is now in the spanning index: a cross-partition duplicate INSERT
-- is rejected
INSERT INTO pgddl_copy VALUES (1, '2025-07-01 00:00:00+00');   -- ERROR (cross-part dup)
-- a COPY batch that itself contains a cross-partition duplicate is rejected and
-- the whole COPY rolls back (key 9 must not survive)
COPY pgddl_copy (id, ts) FROM STDIN;
9	2024-03-01 00:00:00+00
9	2025-03-01 00:00:00+00
\.
SELECT count(*) AS copy_dups
  FROM (SELECT id FROM pgddl_copy GROUP BY id HAVING count(*) > 1) d;  -- 0
SELECT id FROM pgddl_copy ORDER BY id;  -- 1, 2 (no 9)
DROP TABLE pgddl_copy;

-- Section 4: REINDEX repopulates the spanning index from partitions
-- index_build leaves the rebuilt index empty (the partitioned root has no
-- storage), so reindex_index calls BuildSpanningIndexFromPartitions to
-- re-insert (key, tableoid) entries for every live row in every leaf
-- partition.  Cross-partition uniqueness is preserved across REINDEX.

REINDEX INDEX pgddl_data_pkey;

-- Both of these should fail: the spanning index is fully repopulated.
BEGIN;
INSERT INTO pgddl_data VALUES (10, 'post-reindex-dup', '2025-08-01 00:00:00+00');
ROLLBACK;
BEGIN;
INSERT INTO pgddl_data VALUES (1, 'post-reindex-dup2', '2025-04-01 00:00:00+00');
ROLLBACK;

SELECT count(*) FROM pgddl_data;

-- REINDEX CONCURRENTLY is rejected for a spanning index: it would build the
-- replacement on the storage-less partitioned root (an empty index, silently
-- dropping cross-partition uniqueness) and skip BuildSpanningIndexFromPartitions.
-- The plain REINDEX above is the supported path.
REINDEX INDEX CONCURRENTLY pgddl_data_pkey;  -- ERROR: cannot reindex spanning index concurrently

-- Section 5: ATTACH PARTITION backfills spanning index
-- AttachPartitionEnsureIndexes correctly skips spanning indexes (they live
-- only on the root), and ATExecAttachPartition then walks the attaching
-- partition's heap and inserts (user_columns..., attachOid) into each
-- spanning index.  Pre-existing rows are visible to cross-partition
-- uniqueness immediately after attach.

CREATE TABLE pgddl_attach (
    id bigint NOT NULL,
    kind text,
    ts timestamptz NOT NULL,
    CONSTRAINT pgddl_attach_pkey PRIMARY KEY (id)
);
INSERT INTO pgddl_attach VALUES (20, 'pre-attach', '2026-03-01 00:00:00+00');

ALTER TABLE pgddl_data ATTACH PARTITION pgddl_attach
    FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');

-- id=20 was inserted before attach; the backfill picks it up and a
-- duplicate insert is correctly rejected.
BEGIN;
INSERT INTO pgddl_data VALUES (20, 'gap-dup', '2025-10-01 00:00:00+00');
ROLLBACK;

-- New inserts AFTER attach are covered by the same spanning index.
INSERT INTO pgddl_data VALUES (21, 'post-attach', '2026-04-01 00:00:00+00');
BEGIN;
INSERT INTO pgddl_data VALUES (21, 'dup-post-attach', '2025-10-01 00:00:00+00');
ROLLBACK;

-- ATTACH of a partition whose rows would violate the spanning constraint must
-- be REJECTED: the backfill inserts with UNIQUE_CHECK_YES, so a pre-existing
-- cross-partition duplicate (here id=21, already live above) fails the ATTACH.
CREATE TABLE pgddl_dupatt (id bigint NOT NULL, kind text, ts timestamptz NOT NULL,
    CONSTRAINT pgddl_dupatt_pkey PRIMARY KEY (id));
INSERT INTO pgddl_dupatt VALUES (21, 'conflicts', '2027-03-01 00:00:00+00');
ALTER TABLE pgddl_data ATTACH PARTITION pgddl_dupatt
    FOR VALUES FROM ('2027-01-01') TO ('2028-01-01');  -- expect ERROR (dup id=21)
-- The failed ATTACH left pgddl_dupatt standalone (not a partition).
SELECT count(*) AS still_one FROM pgddl_data WHERE id = 21;  -- 1
DROP TABLE pgddl_dupatt;

-- Section 6: abort-safety of partition-lifecycle cleanup
-- DETACH/DROP/TRUNCATE retire a partition's spanning entries by marking them
-- LP_DEAD, a NON-transactional page hint.  The marking is therefore deferred
-- to pre-commit: if the command's transaction rolls back, the partition stays
-- attached and its keys must remain enforced.  (Before the fix the hint was
-- set inline and survived the rollback, silently letting a cross-partition
-- duplicate through.)

CREATE TABLE pgddl_ab (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE pgddl_ab_a PARTITION OF pgddl_ab
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE pgddl_ab_b PARTITION OF pgddl_ab
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO pgddl_ab VALUES (1, '2024-06-01 00:00:00+00');  -- key 1 -> pgddl_ab_a

-- Aborted DETACH: pgddl_ab_a stays attached, so key 1 stays enforced.
BEGIN;
ALTER TABLE pgddl_ab DETACH PARTITION pgddl_ab_a;
ROLLBACK;
INSERT INTO pgddl_ab VALUES (1, '2025-06-01 00:00:00+00');  -- expect ERROR (dup)

-- Aborted DROP: same guarantee via the heap.c cleanup path.
BEGIN;
DROP TABLE pgddl_ab_a;
ROLLBACK;
INSERT INTO pgddl_ab VALUES (1, '2025-07-01 00:00:00+00');  -- expect ERROR (dup)

-- Aborted TRUNCATE: rows come back, so key 1 stays enforced.
BEGIN;
TRUNCATE pgddl_ab_a;
ROLLBACK;
INSERT INTO pgddl_ab VALUES (1, '2025-08-01 00:00:00+00');  -- expect ERROR (dup)

-- Savepoint rollback of DETACH: the subtransaction's queued retirement is
-- discarded, so the committed top transaction leaves key 1 enforced.
BEGIN;
SAVEPOINT sp;
ALTER TABLE pgddl_ab DETACH PARTITION pgddl_ab_a;
ROLLBACK TO SAVEPOINT sp;
COMMIT;
INSERT INTO pgddl_ab VALUES (1, '2025-09-01 00:00:00+00');  -- expect ERROR (dup)

-- Committed DETACH still frees the key (the deferred marking runs at commit).
ALTER TABLE pgddl_ab DETACH PARTITION pgddl_ab_a;
INSERT INTO pgddl_ab VALUES (1, '2025-10-01 00:00:00+00');  -- expect SUCCESS
SELECT count(*) AS id1_in_parent FROM pgddl_ab WHERE id = 1;  -- 1

DROP TABLE pgddl_ab CASCADE;
DROP TABLE pgddl_ab_a;  -- detached, now standalone

-- Section 7: same-txn DETACH/DROP of a partition + a colliding INSERT.
-- After DETACH/DROP the departed partition's spanning entries are still live
-- (LP_DEAD retirement is deferred to pre-commit) but its partseq no longer
-- resolves.  _bt_check_unique must SKIP such an entry, not probe the
-- storage-less partitioned root (rd_tableam == NULL would crash the backend).
-- Within the transaction the key is logically free, so the colliding INSERT
-- succeeds and leaves no cross-partition duplicate.

CREATE TABLE pgddl_sx (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE pgddl_sx_a PARTITION OF pgddl_sx
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE pgddl_sx_b PARTITION OF pgddl_sx
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO pgddl_sx VALUES (1, '2024-06-01 00:00:00+00');  -- key 1 -> pgddl_sx_a
BEGIN;
ALTER TABLE pgddl_sx DETACH PARTITION pgddl_sx_a;
INSERT INTO pgddl_sx VALUES (1, '2025-06-01 00:00:00+00');  -- key now free; succeeds
COMMIT;
SELECT id, count(*) FROM pgddl_sx GROUP BY id HAVING count(*) > 1;  -- no rows
SELECT count(*) AS sx_id1 FROM pgddl_sx WHERE id = 1;              -- 1 (in pgddl_sx_b)
DROP TABLE pgddl_sx CASCADE;
DROP TABLE pgddl_sx_a;  -- detached, now standalone

-- DROP-in-txn variant (heap.c cleanup path), same crash guard.
CREATE TABLE pgddl_dx (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE pgddl_dx_a PARTITION OF pgddl_dx
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE pgddl_dx_b PARTITION OF pgddl_dx
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO pgddl_dx VALUES (1, '2024-06-01 00:00:00+00');
BEGIN;
DROP TABLE pgddl_dx_a;
INSERT INTO pgddl_dx VALUES (1, '2025-06-01 00:00:00+00');  -- succeeds; no crash
COMMIT;
SELECT count(*) AS dx_id1 FROM pgddl_dx WHERE id = 1;  -- 1
DROP TABLE pgddl_dx CASCADE;

-- Section 7b: a table rewrite of a leaf partition (VACUUM FULL / CLUSTER /
-- ALTER COLUMN TYPE) relocates every tuple to a new TID.  The spanning index on
-- the root is not a local index of the leaf, so it must be rebuilt for the
-- rewritten leaf; otherwise its entries dangle at the freed storage and the
-- relocated rows go unindexed -> silent cross-partition duplicates.
CREATE TABLE pgddl_rw (id bigint NOT NULL, ts timestamptz NOT NULL, v int,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE pgddl_rw_a PARTITION OF pgddl_rw FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE pgddl_rw_b PARTITION OF pgddl_rw FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO pgddl_rw SELECT g, '2024-06-01', g FROM generate_series(1, 20) g;
INSERT INTO pgddl_rw SELECT g, '2025-06-01', g FROM generate_series(21, 40) g;

VACUUM FULL pgddl_rw_a;                       -- rewrites leaf a
INSERT INTO pgddl_rw VALUES (5, '2025-03-01', 0);   -- id 5 in a, dup into b: ERROR

CREATE INDEX pgddl_rw_b_v ON pgddl_rw_b (v);
CLUSTER pgddl_rw_b USING pgddl_rw_b_v;        -- rewrites leaf b
INSERT INTO pgddl_rw VALUES (25, '2024-03-01', 0);  -- id 25 in b, dup into a: ERROR

ALTER TABLE pgddl_rw ALTER COLUMN v TYPE bigint;    -- rewrites every leaf
INSERT INTO pgddl_rw VALUES (10, '2025-04-01', 0);  -- id 10 in a, dup into b: ERROR

-- a genuinely new key still inserts; no duplicates anywhere
INSERT INTO pgddl_rw VALUES (500, '2024-05-01', 0);
SELECT count(*) AS total, count(*) FILTER (WHERE cnt > 1) AS dup_keys
  FROM (SELECT id, count(*) cnt FROM pgddl_rw GROUP BY id) s;
DROP TABLE pgddl_rw;

-- Section 8: CASCADE DROP cleans up all objects.
DROP TABLE pgddl_data CASCADE;

SELECT COUNT(*) FROM pg_class WHERE relname LIKE 'pgddl%';

--
-- ADD CONSTRAINT ... USING INDEX on a spanning index
--
-- Promoting an existing index to a constraint is the only route that adds the
-- pg_constraint row WITHOUT building a second index -- it is a catalog change,
-- not an index build, so it is safe on a large tree under concurrent writes
-- where ADD CONSTRAINT ... GLOBAL (which builds) is not.
--
-- It used to be rejected: the validation walks indnkeyatts columns insisting on
-- default opclass/collation/sort, and on a spanning index indnkeyatts counts the
-- trailing partseq discriminator -- an internal system column with no default
-- opclass. So every spanning index failed with "column number N does not have
-- default sorting behavior", and had it passed it would have produced
-- UNIQUE (userkey, tableoid) rather than the declared UNIQUE (userkey) GLOBAL.
CREATE TABLE ui_root (id int NOT NULL, v text);
CREATE TABLE ui_a (a text) INHERITS (ui_root);
CREATE TABLE ui_b (b text) INHERITS (ui_root);
CREATE UNIQUE INDEX ui_g ON ui_root (id) GLOBAL;
INSERT INTO ui_a (id, v, a) SELECT g, 'a', 'x' FROM generate_series(1, 50) g;
INSERT INTO ui_b (id, v, b) SELECT 100 + g, 'b', 'y' FROM generate_series(1, 50) g;

-- the promotion must not rebuild: same relfilenode before and after
SELECT relfilenode AS before_filenode FROM pg_class WHERE relname = 'ui_g' \gset
ALTER TABLE ui_root ADD CONSTRAINT ui_g UNIQUE USING INDEX ui_g;
SELECT relfilenode = :before_filenode AS index_not_rebuilt
  FROM pg_class WHERE relname = 'ui_g';

-- the constraint is the USER key only -- no tableoid -- and stays spanning
SELECT conname, contype, pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'ui_g';
SELECT pg_index_is_global(conindid) AS still_global FROM pg_constraint WHERE conname = 'ui_g';

-- and it still enforces across children
INSERT INTO ui_b (id, v, b) VALUES (1, 'dup', 'y');

-- PRIMARY KEY takes the same path
CREATE TABLE ui_pk (id int NOT NULL, v text);
CREATE TABLE ui_pk_c (w text) INHERITS (ui_pk);
CREATE UNIQUE INDEX ui_pk_g ON ui_pk (id) GLOBAL;
INSERT INTO ui_pk_c (id, v, w) SELECT g, 'a', 'b' FROM generate_series(1, 50) g;
ALTER TABLE ui_pk ADD CONSTRAINT ui_pk_g PRIMARY KEY USING INDEX ui_pk_g;
SELECT contype, pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'ui_pk_g';
INSERT INTO ui_pk (id, v) VALUES (1, 'dup');

-- an ordinary index is unaffected, and a genuinely non-default sort is still refused
CREATE TABLE ui_plain (id int NOT NULL);
CREATE UNIQUE INDEX ui_plain_i ON ui_plain (id);
ALTER TABLE ui_plain ADD CONSTRAINT ui_plain_uq UNIQUE USING INDEX ui_plain_i;
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'ui_plain_uq';

CREATE TABLE ui_desc (id int NOT NULL);
CREATE UNIQUE INDEX ui_desc_i ON ui_desc (id DESC);
ALTER TABLE ui_desc ADD CONSTRAINT ui_desc_uq UNIQUE USING INDEX ui_desc_i;  -- must fail

DROP TABLE ui_desc, ui_plain, ui_pk_c, ui_pk, ui_a, ui_b, ui_root CASCADE;

--
-- Clause fidelity: every clause the parser accepts must survive into the index
--
-- Three defects have shared one shape: the DDL layer answering a question it did
-- not actually evaluate.  WHERE was accepted and never enforced (fixed 0.2.6);
-- INCLUDE was accepted and silently dropped; USING INDEX was rejected for the
-- wrong reason.  Appending the partseq discriminator is where a clause gets
-- lost, so this asserts the whole matrix at once rather than one clause at a
-- time -- a fourth member should fail here rather than in someone's schema.
CREATE TABLE cf (id int NOT NULL, t text, n int, c text COLLATE "C");
CREATE TABLE cf_child (x text) INHERITS (cf);

CREATE UNIQUE INDEX cf_incl    ON cf (id) INCLUDE (t) GLOBAL;
CREATE UNIQUE INDEX cf_incl2   ON cf (n) INCLUDE (t, c) GLOBAL;
CREATE UNIQUE INDEX cf_where   ON cf (id) WHERE n IS NOT NULL GLOBAL;
CREATE UNIQUE INDEX cf_desc    ON cf (id DESC) GLOBAL;
CREATE UNIQUE INDEX cf_nulls   ON cf (id NULLS FIRST) GLOBAL;
CREATE UNIQUE INDEX cf_opclass ON cf (t text_pattern_ops) GLOBAL;
CREATE UNIQUE INDEX cf_collate ON cf (c COLLATE "POSIX") GLOBAL;
CREATE UNIQUE INDEX cf_ff      ON cf (id) WITH (fillfactor=70) GLOBAL;
CREATE UNIQUE INDEX cf_nnd     ON cf (n) NULLS NOT DISTINCT GLOBAL;
-- the same index without GLOBAL, as the control for INCLUDE
CREATE UNIQUE INDEX cf_v_incl  ON cf (t) INCLUDE (n);

SELECT c.relname, i.indnatts, i.indnkeyatts, i.indnuniqatts,
       pg_get_indexdef(i.indexrelid) AS definition
  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
 WHERE c.relname LIKE 'cf\_%'
 ORDER BY c.relname;

-- and the discriminator sits between the keys and the INCLUDEs, not after them
SELECT a.attname, a.attnum
  FROM pg_attribute a
 WHERE a.attrelid = 'cf_incl2'::regclass AND a.attnum > 0
 ORDER BY a.attnum;

-- enforcement is unaffected by carrying an INCLUDE payload
INSERT INTO cf_child (id, t, n, c, x) VALUES (1, 'a', 1, 'p', 'z');
INSERT INTO cf (id, t, n, c) VALUES (1, 'b', 2, 'q');   -- must fail on cf_incl

DROP TABLE cf_child, cf CASCADE;
