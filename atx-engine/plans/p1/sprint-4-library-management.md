# Sprint S4 — Massive Alpha Library Management (sprint spec)

**Status:** ⏳ proposed (not open). Depends on **S3** (produces the alphas), **P4** (`AlphaStore` to scale).
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
§2 (datafield/library management at scale), §5 (correlation gate is the operational core), §6/§10
(combination at 10⁸ scale, alpha lifecycle, dead-alpha recycling).

---

## Why this sprint

S3 turns the engine into a firehose. WorldQuant's ambition is **100 million alphas**; even a fraction of
that breaks an in-memory `AlphaStore` and an O(N²) correlation re-gate. S4 makes a massive library **live**:
persisted, deduplicated, indexed for fast marginal-correlation gating, lifecycle-managed, and **versioned**
so a library snapshot replays byte-identically.

This is the difference between "I ran a search and kept some alphas in RAM" and "I operate a growing,
deduplicated, auditable library of millions of alphas with a known provenance and lifecycle state for each."

P4's in-memory `AlphaStore` (append-only dense `pnl_[n×T]` + `pos_` matrices, stable insertion-order ids)
is the seed; S4 scales it to disk and adds the indices and lifecycle the in-memory version can't carry.

---

## Scope — units

### S4.0 — Marker + ledger
Open `sprint-4-progress.md`, freeze scope, base SHA.

### S4.1 — Disk-backed append-only library store
Scale the P4 `AlphaStore` to a **disk-backed, append-only** columnar store: per-alpha PnL/position streams +
metrics + provenance (the originating expression / mutation lineage from S3) + canonical hash, written
append-only, **mmap'd for zero-copy reads** (matches the p0 zero-copy/no-hot-path-alloc discipline; the TSDB
v2 shared-memory segment work is the reference for the on-disk layout). Stable global `AlphaId`. Survivorship
preserved (delisted instruments' columns retain final values).

### S4.2 — Canonical-hash dedup index
A persistent index keyed by the **canonical DAG hash** (from S3.2) so the library never stores two
structurally-identical alphas. Lifts S3's per-generation dedup to **library-wide** dedup — a candidate is
checked against the whole library before admission. O(1) average lookup (hash index), deterministic.

### S4.3 — Correlation-neighbor index + online corr-to-pool
The gate that breaks at scale: WQ scores every new alpha's correlation **against the pool**, which is O(N²)
if recomputed. S4 builds a **correlation-neighbor index** (e.g. LSH / signed-projection buckets over PnL
streams, or a maintained covariance summary) so the marginal-correlation gate is **incremental** —
admitting alpha N+1 costs ≪ O(N). Pairwise-complete Pearson semantics preserved (matches the P4 gate);
the index is an *accelerator*, and an exact-vs-approx differential test bounds its error.

### S4.4 — Lifecycle state machine
Each alpha carries a PIT lifecycle state: **candidate → admitted → live → decaying → dead → recycled**.
Transitions are point-in-time (no retroactive relabeling) and driven by gate outcomes (S3/P4), decay signals
(S7's monitor later), and dead-alpha extraction (S7's risk-factor reuse). The state machine is the spine S7
hangs decay-monitoring and recycling on.

### S4.5 — Versioned reproducible library builds + close
A library **snapshot/version**: a content-addressed manifest (alpha ids + hashes + lifecycle states + the
seeds that produced them) such that rebuilding from the manifest yields a **byte-identical** library. This is
the reproducibility guarantee for the whole factory — an experiment cites a library version, and that version
replays exactly. Close ceremony.

---

## Exit criteria

- The store round-trips ≥ a large fixture of alphas to disk and back, mmap zero-copy read, byte-identical to
  the in-memory P4 `AlphaStore` for the overlapping fields.
- Dedup index rejects a structurally-equivalent re-submission (non-vacuous) and admits a genuinely-new one.
- Online corr-to-pool gate matches the exact O(N²) recompute within a documented tolerance, at a fraction of
  the cost (measured speedup in the ledger); the gate is incremental in N.
- Lifecycle transitions are PIT and irreversible-in-time (no retroactive relabel; proven).
- A library snapshot rebuilds byte-identically from its manifest (the reproducibility proof).
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

Determinism (snapshot replay = byte-identical), no survivorship (delisted columns preserved on disk), PIT
(lifecycle transitions on PIT boundaries only), no hot-path alloc (mmap zero-copy reads), differential
correctness (online corr index vs exact recompute, bounded error).

## Dependencies

- **Upstream:** S3 (the alpha firehose + canonical hashes + lineage), P4 (`AlphaStore`, gate semantics),
  p0 TSDB-v2 shared-memory segment layout (reference for the on-disk columnar format).
- **atx-core (Pattern B edge):** possibly an L3 persistent/mmap container helper; the LSH/projection index
  math sits on existing L5/L7. Decide at kickoff.

## Explicitly NOT in this sprint

No combination (S5/P4/S7 read the library; S4 stores it). No decay *detection* (S7 — S4 provides the
lifecycle *states*; S7 drives the transitions). No distributed/sharded-across-machines store (anti-roadmap
#4 — single-box, mmap, one disk). No alternative-data fields (anti-roadmap #3).

## Baton → next

S4 gives the factory a permanent, deduplicated, auditable home and the **online corr-to-pool** that makes
gating scale — the substrate S5 reads features and constituents from, and S7 operates as a book.
