# Sprint 4 — Massive Alpha Library Management — Implementation Progress

**Status:** ⏳ IN PROGRESS
**Worktree:** `C:\Users\natha\atx-wt\atx-engine-library` (dedicated worktree on its own branch — explicit pathspecs only, no push; merges back to `feat/atx-core-stdlib` at close)
**Branch:** `feat/atx-engine-library`
**Base:** `feat/atx-core-stdlib` @ `50be881` (S3-3 `50be881`; S3-2 canonical hash `6d6a5a3` present → S4 dedup key available, no fallback needed; P4 CLOSED `f2d22f4`; S1 `2158a17`; S2 `d7a1b75`)
**Started:** 2026-06-07
**Source plan:** [`sprint-4-library-management-implementation-plan.md`](sprint-4-library-management-implementation-plan.md)
**Spec:** [`sprint-4-library-management.md`](sprint-4-library-management.md)

---

## Plan adjustments vs. the source plan (the §0 as-built amendment)

The implementation plan's §0 reconciles the spec against reconnaissance of the merged engine. The eight load-bearing corrections:

1. **No mmap-RAII or stable-hash in atx-core → reuse existing primitives (§0.1).** The engine already links `atx::tsdb` PUBLIC, so `atx::tsdb::Mapping` (read-only mmap RAII) and `atx::tsdb::crc32` are available without any new atx-core lift. S4 reuses them for segment framing rather than writing new RAII wrappers.

2. **tsdb segment format is bar-specific and built-once → clone the framing, not the layout (§0.2).** The tsdb segment wire format (tag8 magic cookie + `format_version` + per-section byte-offsets + seal marker + integrity CRC + validate-before-expose pass) is the right structural skeleton. S4 clones that framing discipline and builds a NEW library-segment record layout whose fields are AlphaDirEntry + Provenance — completely separate from the bar-data tsdb format.

3. **`combine::AlphaStore` has no serialization and provenance is a non-serializable `ISignalSource*` (§0.3).** The P4 store has zero on-disk representation; its "provenance" is only a raw pointer that dies with the process. S4 defines an on-disk superset record that adds the S3 expression source (DSL text), lineage (parent canonical hashes + generation index), and the stable canonical hash as the cross-run dedup key.

4. **No cross-run-stable hash in atx-core → use the S3 factory canonical hash (§0.4).** `atx-core` ships no fixed-byte-layout, stable-across-runs hash. The persisted dedup key is the S3 `factory::canonical_hash` (commutative-sort + FNV-1a fold, field-NAME keyed), which is present at base `6d6a5a3` — no fallback or re-implementation needed.

5. **No streaming or pairwise-complete-corr primitive in atx-core → SimHash accelerator wrapping the exact reference (§0.5).** `combine/correlation.hpp` exposes `pairwise_complete_corr` as the exact differential reference; atx-core has no LSH or SimHash. S4-3 wraps the EXACT `combine::pairwise_complete_corr` with a SimHash accelerator (signed random projection) for the approximate nearest-neighbor screen.

6. **No LSH anywhere in the engine → engine-local signed random projection (§0.6).** The projection kernel uses `simd::dot` (already in-engine) and `Xoshiro256pp::normal()` for projection-vector generation. The LSH implementation is engine-local to S4 — not a Pattern-B atx-core lift at this stage.

7. **Combine layer has no wall-clock time axis → PIT lifecycle keyed to period index (§0.7).** `combine::AlphaStore` tracks no time dimension — there is no date column to key lifecycle transitions on. S4's point-in-time (PIT) lifecycle state machine is keyed to an as-of PERIOD INDEX (an opaque monotone integer, not a calendar date) so the journal is deterministic under replay and immune to system-clock skew.

8. **`db::sqlite` is a compiled TU, not header-only, and enforces one-Database-per-thread (§0.8).** `atx::core::db::Database` is backed by `atx_sqlite3` (a vendored static lib compiled with `SQLITE_THREADSAFE=2`). S4-0 performs a build smoke-check to confirm sqlite symbols resolve transitively via `atx::engine`. The serial-owner rule (one `Database` per owning thread, never shared across threads) governs all S4 metadata writes.

---

## Kickoff risks

### (a) Pattern-B atx-core edges

- **mmap-RAII + writable mapping → L3 (shipped on `atx::tsdb::Mapping`).** Read-only mmap RAII is already available via `atx::tsdb::Mapping`; the writable-mapping use-case is a potential L3 Pattern-B lift deferred to a future sprint. S4 requires only read-only post-seal access.
- **Fixed-seed stable hash → atx-core (shipped on S3 canonical hash).** The S3 `factory::canonical_hash` at `6d6a5a3` is the stable dedup key; no new hash primitive needed. A future atx-core `stable_hash` library could absorb it.
- **LSH / signed random projection → atx-core (engine-local for now).** The SimHash kernel is engine-local in S4-3. Promotion to atx-core is a close residual.
- **Unbounded pairwise-complete 2-var covariance → L6 (deferred).** The exact `pairwise_complete_corr` is the differential reference; an unbounded streaming covariance primitive is a potential L6 lift recorded as a close residual.

### (b) S3 canonical-hash dependency

SATISFIED at base `6d6a5a3`. The `factory::canonical_hash` (commutative-sort + FNV-1a fold, field-NAME keyed) is present on the branch. No fallback implementation is needed.

### (c) Shared-branch / explicit-pathspec discipline

This branch (`feat/atx-engine-library`) is a DEDICATED worktree. Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`); `git show HEAD --stat` after each commit to verify only the intended files appear; NEVER touch `atx-core/*` or `atx-tsdb/*`; do not push.

### (d) Dominant risk: silent data corruption at scale

The four failure modes that are silent, not loud:
- Round-trip wrong: a sealed segment reads back a different record than was written.
- Dedup drops distinct: two structurally-different alphas hash-collide → one is silently discarded.
- Corr misses a true neighbor: SimHash bucket miss → a correlated alpha is admitted that should have been screened.
- Snapshot not byte-identical: two snapshots of the same library state differ → manifest integrity fails silently.

Each unit carries a non-vacuous proof test against its relevant failure mode.

---

## Per-unit status

| Unit  | Title                                                                    | Status | Commit SHA(s)     | Tests | Notes |
|-------|--------------------------------------------------------------------------|--------|-------------------|-------|-------|
| S4-0  | Marker + ledger + library scaffold + sqlite build check                  | ✅ done | `e97c192`        | —     | ledger + `library/fwd.hpp` + sqlite-link smoke-check: **passed transitively** (no CMake changes needed; `atx_sqlite3` resolves via `atx::core` → `atx::engine` transitive link). `Status` API uses `.has_value()` (not `.ok()`). Smoke test deleted after verification. |
| S4-1  | On-disk record schema + append-only segmented store                      | ✅ done | `8fdbad0`        | 11/11 (LibraryRecord 5/5, LibraryStore 6/6) | `record.hpp` (POD SegmentHeader/AlphaDirEntry/SegmentFooter, LE-pinned + size static_asserts; one-pass sealed `write_segment_bytes`; `SegmentReaderLite` mmap-ro `attach`/in-mem `attach_bytes` with validate-magic/version/seal/crc BEFORE expose) + `store.hpp` (`LibraryStore`: memtable=`combine::AlphaStore`, sqlite segment catalog, union read over sealed segments+memtable via binary-searched base ids, reopen-from-disk). **Round-trips BIT-IDENTICAL** to AlphaStore incl. NaN cells + a delisted/final-value NaN-tail column (proven via `std::bit_cast<u64>` compare of pnl+positions). Multi-segment global-id read (12 alphas across 2 segments), **two-builds byte-identical** (same integrity crc), **sealed segment byte-frozen** after later admits. Framing CLONED from atx::tsdb (tag8/crc32/Mapping reused, NOT the bar layout). Full suite green: **1766/1766** (1 pre-existing Databento smoke skipped). Deviations: `tag8` lives in `atx/tsdb/segment.hpp` (not checksum.hpp); `AlphaStore` has no `clear()` → memtable reset by move-assign; `Database` has no default ctor → opened in init list via `open_or_abort` helper. |
| S4-2  | Library-wide canonical-hash dedup index                                  | ✅ done | `b3a518b`        | 4/4 (LibraryDedup) | `dedup_index.hpp` (`DedupIndex`: in-mem `HashMap<u64,AlphaId,IdentityHash>` cache fronting a sqlite `dedup(canon_hash INTEGER PRIMARY KEY, alpha_id INTEGER)` table; `contains`/`find` are pure cache lookups, `insert` short-circuits known dups then `INSERT OR IGNORE` + `changes()==0` → `Ok(false)` for a raced/library-wide dup, `Ok(true)` only after a real db change updates the cache). PURE-u64-keyed — never calls `canonical_hash` itself (the test does). Cache rebuilt at open by `SELECT … ORDER BY alpha_id ASC` (deterministic, L7). **Commutative-reorder rejected NON-VACUOUSLY**: `add(rank(close),ts_mean(volume,10))` vs operand-swapped both hash equal (`ASSERT_EQ(h,h2)` HELD — Add is in `is_hash_commutative`) → second insert returns false, FIRST id wins. **Genuinely-new admitted** (`ts_mean(close,5)`≠`(close,6)` → both `true`, no false dup). **Persists across reopen** (`0xDEADBEEF` survives destroy+reopen; cache rebuilt from sqlite; re-insert still rejected). **64-bit fidelity** (`0xF0F0F0F0F0F0F0F0` round-trips via `std::bit_cast<i64>` ↔ INTEGER with no top-bit loss; a ±1-bit near-miss is NOT a false hit). Full suite green: **1770/1770** (1 pre-existing Databento smoke skipped). Deviations: u64↔i64 via `std::bit_cast` (not `static_cast`) for all-bits-preserving INTEGER round-trip incl. high-bit hashes; `IdentityHash` declares `is_avalanching` so ankerl skips its wyhash mix (key is already mixed); `Database`/`AlphaId` no-default-ctor handled (Database opened in init list via `open_or_abort` mirroring S4-1). |
| S4-3  | Correlation-neighbor index + incremental corr-to-pool                    | ⏳     |                   |       | |
| S4-4  | Lifecycle state machine (PIT, append-only journal)                       | ⏳     |                   |       | |
| S4-5  | Versioned manifest + Library facade + integration + close                | ⏳     |                   |       | |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `e97c192` | S4-0 | docs(s4-0): open sprint-4 library-management ledger + scaffold |
| `8fdbad0` | S4-1 | feat(s4-1): append-only mmap'd segmented library store + on-disk record |
| `b3a518b` | S4-2 | feat(s4-2): library-wide canonical-hash dedup index (sqlite + cache) |

---

## Close residuals → p1 ROADMAP future-work backlog

_(filled at S4-5 close.)_

## Baton → next

_(filled at close.)_
