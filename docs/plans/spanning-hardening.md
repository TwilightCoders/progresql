# Hardening the spanning-index feature: what production found, and why none of it announced itself

**Status: active program. Items below are individually tracked; this document is
the pattern behind them.**

Most of the spanning feature's test coverage is synthetic — suites written
alongside the code, exercising what the author already had in mind. This document
records what a *production* workload found instead, and it found a different
class of thing: an inheritance tree of ~25 tables and ~1.6M rows carrying ~21
polymorphic foreign keys, under seven weeks of uninterrupted writes, with a
monitoring layer actively trying to verify itself.

Seven defects came out of it. **Not one of them produced an error, a slowdown, or
a log line.** That is the finding worth carrying forward, more than any individual
bug.

---

## 1. The meta-finding: these failures are silent by construction

Not "easy to miss" — *structurally incapable* of announcing themselves. Each has a
specific reason it cannot surface:

| failure | why nothing reports it |
|---|---|
| Spanning index rebuilt **empty** | no error; no slowdown (the planner never reads it); and the verification tool **refuses to run** on this index class |
| Index entries **doubled** by a parallel build | enforcement stays *correct* — the only symptom is an index twice the size it should be |
| Entries left pointing at **pre-rewrite TIDs** | surfaces arbitrarily later, as a bare `could not read blocks` from an unrelated `INSERT` |
| `NO INHERIT` leaving **FK clones** behind | the command succeeds; the failure appears only at a later `DROP` |
| `REINDEX CONCURRENTLY` **not refused** | the operation an operator chooses *because* they are being careful about locks |

The practical consequence: **a hardening program for this feature cannot be built
out of symptom detection.** There is no symptom. It has to assert invariants
directly and continuously —

- live index entries vs. a **recursive** row count of the tree
- FK clone counts before and after `INHERIT` / `NO INHERIT`
- index size against what the row count implies
- entry count stable across a rebuild

— because every one of the seven was found either by measuring an invariant or by
auditing for a pattern, and **none** by something failing loudly.

## 2. The predictive theme: declarative supported, inheritance left behind

The fork consistently implements spanning support for **declarative
partitioning** and leaves **table inheritance** behind. It shows up two ways:

1. **A declarative-only predicate** (`relispartition`, or
   `relkind = RELKIND_PARTITIONED_TABLE`) used as a proxy for "is this under a
   spanning root", which is equally true of `INHERITS` children. Four separate
   sites did this independently.
2. **An inheritance counterpart never written** for a partition operation that
   has one — e.g. `DETACH PARTITION` cleans up FK clones; `NO INHERIT` does not.

Section 1 says *what the failures look like*; this says *where to look for them*.
Together they are the search: **for any spanning behaviour, ask what the
inheritance case does — and what the undo does.** The rule is stated at the code
site in `RelationCanBeSpanningLeaf()` (`access/spanning.h`), which exists so the
next author meets it rather than reconstructing a proxy.

## 3. The findings, with what each cost

Cost is recorded deliberately. "REINDEX rebuilds spanning indexes empty" is a bug
report; what follows is what makes it possible to prioritise correctly — including
by whoever reads this once the context is gone.

### 3.1 `amcheck` cannot verify a spanning index at all — **open, ranked first**

`bt_index_check(idx, heapallindexed => true)` refuses: a spanning index's table is
the storage-less root (`rd_tableam == NULL`), so the fingerprint scan would
dereference NULL and crash the backend. The refusal is correct.

**But it is not a privilege check — superuser raises the identical error.** The
verification designed to catch "a row is missing from the index" is unavailable to
*every* caller on *every* spanning index, permanently. And the trap is the
default: `bt_index_check(idx)` defaults `heapallindexed => false`, so the ordinary
call does not error — it returns *clean*, having verified only that a possibly
empty btree is well-formed. Which it is.

**Cost:** this is why 3.2 survived seven weeks. Every other item here is a bug
with a workaround; this is the absence of the thing that would have caught them.
Scoped in [`spanning-amcheck-heapallindexed.md`](./spanning-amcheck-heapallindexed.md),
which also documents the working `pageinspect` substitute.

### 3.2 `REINDEX` rebuilt an inheritance-rooted spanning index empty — **fixed**

Repopulation was gated on the root's relkind being `RELKIND_PARTITIONED_TABLE`;
an `INHERITS` root is `RELKIND_RELATION` and fell through.

**Cost, measured on a live corpus:** one index missing **333,602 of 1,595,088
rows** (20.9%); a second in the same tree missing 6,925 of 26,245. Cross-child
uniqueness unenforced on those rows for **seven weeks**, undetected, on a
1.6M-row production dataset. Detected only when someone measured entries against
rows by hand.

### 3.3 Three more instances of the same predicate — **fixed**

Found by auditing for the pattern in 2.1 rather than by any report:

- **`REINDEX CONCURRENTLY` not refused** on an inheritance root — produced the
  same hollow index via a path with *no* repopulation step at all.
- **Table rewrite** (`VACUUM FULL`, `CLUSTER`, rewriting `ALTER`) on an
  inheritance child left entries on pre-rewrite TIDs; an ordinary `INSERT` then
  raised `could not read blocks ...`, and a deleted key could never be reused.
  Plain `VACUUM` did not heal it.
- **Parallel index build** on an inheritance root: **241,083 entries for 120,100
  rows**, a malformed duplicate per root row. Enforcement stayed correct.

**Cost:** none realised — all three were found before they fired in production.
That is the argument for auditing over waiting: the parallel-build one has no
symptom a human would ever report, so its expected discovery time was unbounded.

### 3.4 `NO INHERIT` does not undo what `INHERIT` did — **open**

A child joining a spanning tree acquires the referenced-side FK clones of every
key targeting a spanning ancestor (`progresql_clone_referenced_fks_to_child`).
Nothing removes them when it leaves, so a child cannot be detached and dropped the
way a partition can.

Note the surrounding behaviour is **stock PostgreSQL, not a fork defect**:
`DROP TABLE` names the *base* constraint (the clone is an internal dependency of
the base, so Postgres reports the owner), and `CASCADE` therefore drops the base
with all its clones on other tables. Reproducible with plain declarative
partitioning and no `GLOBAL` index anywhere. The fork's gap is only the missing
counterpart.

**Cost:** every new child table is a one-way door, which inverts the normally-safe
"additive migrations are cheap and reversible". Workaround verified — drop each
affected base FK, drop the table, recreate the FKs, in a single transaction (see
README Limitations). Ranked below 3.1 because a workaround exists.

### 3.5 `idx_scan = 0` is by design and must be documented where operators look

`plancat.c` skips spanning indexes outright: rows live in the leaves, the root has
no storage, and the appended discriminator makes the key shape useless for
planning. A spanning index is **never** read by a query.

**Cost:** two compounding hazards. First, a broken spanning index costs *nothing*
in read performance and *everything* in enforcement — so no query ever gets
slower to warn you. Second, an operator reading "unused index, 0 scans" on a
monitoring dashboard would draw exactly the wrong conclusion and might drop it.

### 3.6 The self-heal pathology — a warning owed to consumers

A monitoring layer paired `bt_index_check` with automatic `REINDEX` of anything
it called corrupt. `REINDEX` rebuilt the index empty; `bt_index_check` returns
clean on an empty index. **The remedy caused the damage and the check then
certified it.** It fired twice and never alarmed again, because a hollow index
passes forever.

The bug was in the consumer, but 3.1 and 3.2 together made it inevitable, and any
consumer pairing detect-with-auto-repair against this fork can reproduce it.
**Do not auto-repair a spanning index on the basis of a structural check.** Until
3.1 lands, verification and repair must not be wired to each other.

### 3.7 Non-recursive row counts against an inheritance tree

Two independent consumers each counted a root's depth-1 children and missed a
deep child holding most of the volume — one reported 1,105,275 rows against a real
1,595,088.

**Cost:** no data harm, but it makes every reading look untrustworthy even when
the verdict is sound, which is precisely how a monitor loses its audience. Two
independent parties making the identical mistake suggests the ergonomics invite
it; the corrected recipe is documented alongside the detector in
[`spanning-amcheck-heapallindexed.md`](./spanning-amcheck-heapallindexed.md).

## 4. Where the program goes next

In priority order, on the argument that absence-of-verification outranks
individual bugs:

1. **Make `heapallindexed` work for spanning indexes** (3.1). The traversal is
   already solved by `spanning_backfill_leaf` for a different purpose. This
   retires 3.6 as a hazard and gives 3.7 a supported answer.
2. **An invariant assertion suite** rather than symptom detection, per section 1 —
   entries vs recursive rows, clone counts across `INHERIT`/`NO INHERIT`, entry
   count stable across rebuild. These are the checks that found everything here.
3. **The `NO INHERIT` counterpart** (3.4).
4. **Operator-facing documentation** of 3.5 and 3.6, where someone reading a
   dashboard will meet it.

## 5. The durable lesson

Four of these seven were found by auditing for a pattern; three by measuring an
invariant. **None** by something failing loudly, because none of them can fail
loudly. A feature whose failures are silent by construction cannot be hardened by
waiting — only by asserting, and by asking of every new behaviour what the
inheritance case does and what the undo does.
