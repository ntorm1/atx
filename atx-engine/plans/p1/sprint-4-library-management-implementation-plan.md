# Sprint S4 — Massive Alpha Library Management — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the FROZEN *how*; the sprint spec [`sprint-4-library-management.md`](sprint-4-library-management.md) is the *what*. On conflict, **§0 (this plan's as-built amendment) overrides** the spec.

**Goal:** Give the S3 alpha firehose a permanent home — a **disk-backed, append-only, mmap-zero-copy** alpha library that scales the in-memory P4 `combine::AlphaStore` to 10⁵–10⁹ alphas, with a **library-wide canonical-hash dedup index**, an **incremental (non-O(N²)) correlation-to-pool gate**, a **point-in-time lifecycle state machine** (candidate→admitted→live→decaying→dead→recycled), and **content-addressed versioned snapshots** that rebuild **byte-identically**.

**Architecture:** A new header-only `library/` layer (namespace `atx::engine::library`) on top of the as-built engine. The bulk store is a sequence of **immutable sealed segments** (the LSM "memtable → immutable SSTable" pattern): admitted alphas accumulate in an in-memory staging buffer (the P4 `AlphaStore`, reused verbatim) and are flushed to a whole-file-one-pass **sealed segment** — reusing the as-built `atx-tsdb` segment framing (magic + format-version + section byte-offsets + seal-marker + integrity-CRC + validate-before-expose) and the `atx::tsdb::Mapping` read-only mmap RAII (already a PUBLIC engine dependency). Each segment carries dense `pnl[n×T]` / `pos[n×T×N]` f64 column blocks (tsdb-style, zero-copy) **plus a provenance blob** (the S3 expression source + mutation lineage + canonical hash the in-memory store never held). The *mutable* metadata — the canonical-hash dedup index, the PIT lifecycle journal, and the segment catalog — lives in **`atx::core::db::sqlite`** (the wrapper the codebase already ships), append-only (lifecycle transitions are journal rows, never in-place UPDATEs → no retroactive relabel). The correlation-neighbor index is an in-memory **signed random-projection (SimHash) accelerator** over demeaned PnL streams: a query returns a small candidate neighbor set, and the gate computes the **exact `combine::pairwise_complete_corr`** only over those candidates — turning the O(N) per-admit corr-to-pool scan into o(N), with an exact-vs-approx differential test bounding recall. Determinism is by construction: segment bytes, the manifest content-address, the SRP hyperplanes (seeded `Xoshiro256pp`), and every reduction are order-fixed, so a snapshot replays byte-identically.

**Tech Stack:** C++20, header-only inline (`#pragma once`), namespace `atx::engine::library`; reuses `atx::tsdb::{Mapping, crc32, tag8, SegmentReader idioms}` (already linked PUBLIC by `atx::engine`), `atx::core::db::{Database, Statement, Transaction, BlobStream, OpenMode}`, `combine::{AlphaStore, AlphaId, AlphaRecord, AlphaMetrics, compute_metrics, AlphaGate, GateConfig, GateVerdict, pairwise_complete_corr}`, `alpha::{AlphaStreams, Panel}`, `eval::{deflated_sharpe}` (admission, via S1), the S3 `factory::canonical_hash` (the stable dedup key — §0.4), `atx::core::{simd::dot, random::Xoshiro256pp, container::HashMap/HashSet, Result, Status, u32, u64, usize, f64}`. GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake edit). clang-cl `/W4 /permissive- /WX` **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). Build + ctest are the gates; clang-tidy disabled (noise).

---

## §0 — As-built reconciliation amendment (the recon fixes)

The spec was drafted from the research north-stars before this sprint's reconnaissance against the merged `feat/atx-core-stdlib` engine + atx-core + atx-tsdb. Eight load-bearing corrections; each changes a unit's scope.

### 0.1 There is **no mmap RAII and no stable hash in atx-core** — both already exist in `atx-tsdb`, which the engine already links
The spec posits "mmap'd zero-copy reads" and "possibly an L3 persistent/mmap container helper" as an open atx-core request. Reconnaissance: **atx-core has zero memory-mapping code** (the only `CreateFileMapping`/`mmap` hits are inside vendored SQLite). The **sole reusable mmap RAII wrapper in the repo is `atx::tsdb::Mapping`** (`atx-tsdb/include/atx/tsdb/mapping.hpp`): move-only, `static Result<Mapping> map_file_ro(const std::string&)` (Win32 `CreateFileW`→`CreateFileMappingW(PAGE_READONLY)`→`MapViewOfFile(FILE_MAP_READ)`; POSIX `mmap`+`madvise`), accessors `const u8* base() / usize size() / void prefetch()`. **Critically it is READ-ONLY — no writable mapping, no resize, no append-to-mapping.** And `atx::engine` **already links `atx::tsdb` PUBLIC** (`atx-engine/CMakeLists.txt:17–20`; `ShmBarFeed` exposes `tsdb::SegmentReader`), so `Mapping`/`crc32`/the segment framing are in-reach with **no new module edge**. Consequence: S4 **reuses `atx::tsdb::Mapping` for the read path** and writes the append path with ordinary buffered file I/O then maps the sealed file read-only (exactly how `tsdb::SegmentBuilder::write` already operates). *(Recorded as the Pattern-B lift: promote a general mmap-RAII + a writable/growable mapping to atx-core **L3**; ship on `atx::tsdb::Mapping` now, mirroring S1's `stats_ext` / S2's `DetPool` engine-local-then-lift precedent.)*

### 0.2 The tsdb segment format is **bar-data-specific and build-once** — reuse the *framing*, build the *record layout*
`atx::tsdb::segment.hpp` is a fixed dense `F×T×N` f64 grid (Field × Time × iNstrument) with a unix-nanos time axis + per-cell present bitmap — purpose-built for a rectangular market panel, and **`SegmentBuilder` materializes the whole grid in RAM and writes the entire sealed file in one pass (NOT append-only, no incremental writer)**. What *is* directly reusable: (a) `Mapping` (0.1); (b) `atx::tsdb::crc32` (`checksum.hpp:36`, CRC-32/zlib poly, byte-wise, **cross-process/platform stable** — the integrity primitive); (c) the **framing idioms** — `consteval u64 tag8("ATXSEG01")` magic, a `format_version` field + `is_supported_version`, all sections addressed as **byte offsets from `base()`**, a `kSealMarker` footer, a footer `integrity_crc` over `[header .. footer)`, and the **validate-magic/version/sealed/crc-before-exposing-any-byte** discipline in `SegmentReader::attach`. Consequence: S4 **clones the framing** for a *new* library-segment layout that the bar grid cannot hold: dense `pnl[n×T]` + `pos[n×T×N]` f64 column blocks (the AlphaStore shape, §0.3) **plus a per-alpha metadata directory and a variable-length provenance string blob**. The library is append-only **at segment granularity** (immutable sealed segments + a catalog), the LSM pattern — not a mutable single file.

### 0.3 `AlphaStore` persists **nothing** and carries **no serializable provenance** — S4 defines the on-disk record
`combine::AlphaStore` (`combine/store.hpp`) has **zero serialization** (no save/load/mmap/fstream anywhere — grep-clean) and stores per alpha only `AlphaRecord{AlphaId id; AlphaMetrics metrics; ISignalSource* source}` — where `source` is a **non-owning raw pointer, NOT serializable**; there is **no expression string, no name, no canonical hash, no lineage**. The dense data is two flat alpha-major vectors: `pnl_[a*n_periods + t]` and `pos_[(a*n_periods + period)*n_instruments + j]`, with `n_periods_`/`n_instruments_` fixed at first insert and **NaN cells significant (stored verbatim** — the survivorship guarantee at this layer). `AlphaId{u32 value}` is the insertion index == array row. Consequence: S4.1 must **define a serializable record schema** — `pnl`/`pos`/`metrics`/`AlphaId` (the "overlapping fields" the exit criterion round-trips byte-identically) **plus** the provenance the store never had: the S3 expression source string, the canonical hash, and the S3 lineage (`{parent canon_hash(es), mutation op, seed}`). The disk store is a *superset* of `AlphaStore`, not a dump of it. The in-memory `AlphaStore` remains the staging buffer (the "memtable").

### 0.4 No cross-run-stable hash exists in atx-core — the persisted dedup key is the **S3 canonical hash**
`atx::core::hash.hpp` exposes `hash_bytes` (wyhash) and `hash_combine` (`std::hash`); both are **explicitly NOT stable across process restarts / platforms** (`hash.hpp:14–15`: *"wyhash seeds are compile-time constants; endianness is not normalised by design"*; `hash_combine` delegates to implementation-defined `std::hash`). There is **no fixed-seed FNV-1a**. (atx-tsdb hit the same wall and rolled its own `crc32` for exactly this reason — `checksum.hpp:5–7`.) The cross-run-stable structural key already designed for this is the **S3 `factory::canonical_hash`** (S3 plan §0.5/§4.4: recursive commutative-sort + fold over a **fixed byte layout**, **field-name-keyed**, FNV-1a-style — built *because* `alpha::NodeKeyHash` is unstable). Consequence: S4.2's dedup index keys on `factory::canonical_hash` **directly**. *If S3's canonical hash is unavailable at S4 kickoff (S3 shipped without it / fallback), S4 defines the same fixed-seed stable hash locally* (mirroring the tsdb-crc precedent), but the design assumes S3 delivered it (the S3 close baton explicitly hands "persisted canonical-hash dedup → S4 library-wide index"). The `ankerl` `HashSet<u64>`/`HashMap<u64,…>` accept a **custom identity hasher** for these pre-mixed u64 keys (`container/hash_map.hpp:50,166`).

### 0.5 No streaming/full-history correlation primitive exists — the **exact** reference is `pairwise_complete_corr`, and it is the only one
The corr-to-pool screen `AlphaGate::max_abs_corr_to_pool` (`gate.hpp:130`) is a **private static** computing **MAX |corr|** over the pool (cutoff `cfg.max_pool_corr = 0.7`), built on the **only** correlation primitive in the engine, `combine::pairwise_complete_corr(a,b)->f64` (`correlation.hpp:76`, pairwise-complete Pearson, NaN-skipping, 0 on <2 valid pairs / zero variance, clamped `[-1,1]`). atx-core's streaming stats are **single-stream only** (`online_stats.hpp`: `RunningMean`/`RunningVariance`(Welford)/`Ewma` — **no 2-var covariance**); `rolling.hpp` has `RollingCovariance<W>`/`RollingCorrelation<W>` **but they are fixed-window, population, and NOT pairwise-complete** (lock-step both-present, no NaN handling), so they are **not** a drop-in for the pool's pairwise-complete semantics. Consequence: S4.3's "online corr-to-pool" is **not** an existing streaming accumulator — it is an *accelerator* (a candidate-neighbor index) wrapped around the **exact** `pairwise_complete_corr`, which stays the differential reference. The gate's MAX-|corr|-vs-`max_pool_corr` semantics are preserved exactly on the candidate set.

### 0.6 No LSH / random-projection primitive exists anywhere — engine-local **signed random projection** (decided)
Grep across atx-core for `lsh`/`locality-sensitive`/`random-projection`/`minhash`/`simhash`: **none**. The building blocks do exist: `simd::dot(span,span)->T` (FMA, `simd.hpp:102`) and `random::Xoshiro256pp::normal()` (Box-Muller N(0,1), `random.hpp:184`). Consequence: S4.3 ships an **engine-local signed random-projection (SimHash) index** — `K` seeded random hyperplanes; a stream's signature bit `k` is `sign(dot(demean(pnl), h_k))`; correlation ≈ cosine of demeaned vectors, and SRP buckets approximate cosine similarity, so a query yields a high-recall candidate neighbor set. **Decision (recorded):** build it engine-local on `simd::dot` + `Xoshiro256pp::normal()`; record **"LSH / signed-projection primitive → atx-core"** as the Pattern-B lift (the maintained-covariance-summary alternative is noted as a residual). The index is an accelerator; recall is **measured and documented** by the exact-vs-approx differential (§0.5).

### 0.7 The combine layer has **no wall-clock time axis** — PIT lifecycle needs an external date vector
Neither `AlphaStore` nor `AlphaStreams` carries any timestamp: time is a **positional `usize` period index** `t ∈ [0, n_periods)` (`streams.hpp`, `combiner.hpp` `fit_begin/fit_end` are `usize`); wall-clock dates live **upstream in `alpha::Panel`**. Consequence: the S4.4 lifecycle state machine cannot key transitions on a date the store doesn't hold — it keys them on an **as-of period index** (and the optional external date vector the caller threads from the `Panel`). "PIT transition" is operationalized as: a transition is recorded **at** an as-of period and is **never** back-dated; `state_as_of(alpha, t)` is the latest journal row with `as_of_period ≤ t`. No retroactive relabel = journal-append-only (§0.8), proven by a test that a later transition does not alter an earlier `state_as_of` query.

### 0.8 `db::sqlite` is a **compiled TU** (not header-only) and **single-Database-per-thread** — S4 is the engine's first use
`atx::core::db::{Database,Statement,Transaction,BlobStream}` are **PIMPL declarations with impl in `atx-core/src/db/sqlite.cpp`** (built `SQLITE_THREADSAFE=2`, vendored `atx_sqlite3`) — *not* header-only like the rest of the engine. Errors are `Result<T>`/`Status` (no exceptions); BLOBs are first-class (`bind(i32, span<const std::byte>)`, `column_blob`, incremental `BlobStream`); `Transaction` is RAII begin/commit with auto-ROLLBACK; `prepare_cached` returns a borrowed cache-stable `Statement*`. Consequence: (a) **Build reconciliation** — S4-0 must confirm the `atx-engine-tests` target resolves the sqlite symbols transitively through `atx::core`; if `atx_sqlite3` is PRIVATE to atx::core and not propagated, add it (one-line `target_link_libraries`); this is the first engine TU to touch `db::`. (b) **Concurrency rule** — `SQLITE_THREADSAFE=2` = one `Database` per thread; the S2 parallel workers **never** touch sqlite; only the **serial admit path** owns the index `Database`. Bulk segment writes are likewise serial (the cold flush path).

> **Net scope shift vs spec:** the on-disk store reuses `atx::tsdb::Mapping`+`crc32`+framing (0.1/0.2) — no atx-core mmap request blocks S4 — over a **new** library-segment record layout that adds the **serializable provenance** `AlphaStore` lacks (0.3). The dedup key is the **S3 canonical hash** (0.4). The "online corr-to-pool" is a **SimHash accelerator around exact `pairwise_complete_corr`**, engine-local (0.5/0.6), recall-bounded by a differential. Lifecycle PIT is keyed to an **as-of period** + an append-only journal (0.7). The mutable indices live in **`db::sqlite`**, a compiled TU needing a one-time build check + a serial-owner concurrency rule (0.8).

---

## §1 — Research foundation: the library design rules (with citations)

Derived from the research north-stars (`worldquant-systems-deep-dive.md` §2/§5/§10, `renaissance-technologies-systems-deep-dive.md`) and the carried-forward p1 invariants. **Non-negotiable**; every S4 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **L1** | **Append-only, immutable, content-addressed.** Admitted alphas are written to **immutable sealed segments**; a record is never rewritten or deleted in place. `AlphaId` = global append index = segment_base + local row (matches `AlphaStore`'s stable-insertion-id). Lifecycle changes are *journal appends*, not mutations. | Reproducibility + no-survivorship **by construction**; LSM-tree immutability [O'Neil 1996]; Git/Merkle content addressing. WQ §10 (auditable alpha lifecycle). |
| **L2** | **mmap zero-copy reads; validate before expose.** Reads go through `atx::tsdb::Mapping` (read-only `MapViewOfFile`/`mmap`); `SegmentReader::attach` validates magic/version/seal/`crc32` over `[header..footer)` **before** returning any byte. No steady-state allocation on the read path. | p1 invariant #6 (no hot-path alloc); the p0 TSDB-v2 segment is the named reference (spec); integrity = `tsdb::crc32` (0.2). |
| **L3** | **Library-wide canonical-hash dedup.** A candidate is checked against the **whole library** (not just the current generation) before admission; the key is the cross-run-stable S3 `canonical_hash`. Hash-equal ⇒ the VM evaluates bit-identical (soundness inherited from S3 F6). | Lifts S3's per-generation dedup to the library; the throughput lever — never store/score a structural duplicate. WQ §5 (the correlation/dup gate is the operational core). |
| **L4** | **Incremental corr-to-pool (o(N), not O(N²)).** Admitting alpha N+1 costs ≪ O(N): a SimHash neighbor query → a small candidate set → **exact** `pairwise_complete_corr` over candidates → MAX \|corr\| vs `max_pool_corr`. The accelerator's recall is measured against the exact O(N²) recompute and documented. | WQ §5: every new alpha is scored on correlation to the pool; recomputing is O(N²) and breaks at scale. SimHash/SRP [Charikar 2002]; LSH [Indyk-Motwani 1998]. |
| **L5** | **PIT lifecycle, irreversible-in-time.** Each alpha has a state in {candidate, admitted, live, decaying, dead, recycled}; transitions are recorded **at an as-of period** and **never back-dated**. `state_as_of(alpha, t)` = latest journal row with `as_of_period ≤ t`. The state machine is the spine S7's decay-monitor + dead-alpha-recycling hang on. | p1 invariant #4 (PIT); WQ §6/§10 (alpha lifecycle); Kakushadze 1709.06641 (dead alphas → risk factors, the S7 baton). |
| **L6** | **Versioned, reproducible builds.** A snapshot is a **content-addressed manifest** (ordered `{AlphaId, canon_hash, lifecycle@snapshot, segment crc}` + the master seeds) such that rebuilding from the manifest yields a **byte-identical** library (same segment bytes, same crcs). An experiment cites a version; the version replays exactly. | p1 invariant #1 (determinism); the reproducibility guarantee for the whole factory; content-addressed/Merkle snapshots. |
| **L7** | **Determinism end-to-end.** Segment byte layout, the manifest content-address, the SRP hyperplanes (seeded `Xoshiro256pp`, never time/thread), the dedup-index iteration, and every reduction are order-fixed. Same inputs → byte-identical segments + manifest digest. | p1 invariant #1; the only thing that breaks bit-reproducibility is unordered iteration / FP non-associativity. |
| **L8** | **No survivorship on disk.** Delisted instruments' columns keep their final values; NaN cells are written **verbatim** and round-trip exactly. The library never retroactively drops a symbol or an alpha. | p1 invariant #3; `AlphaStore` stores NaN verbatim (0.3) — the disk format must preserve that, not coerce. |

**One-sentence thesis:** *the engine already has the two on-disk primitives this needs (`atx::tsdb`'s mmap+crc+framing for immutable bulk, `atx::core::db::sqlite` for mutable indices) and the stable dedup key (S3's canonical hash); S4 is the append-only segmented library that composes them, and the only new correctness risks are (a) dedup/round-trip soundness (L2/L3/L8), (b) the corr accelerator's recall vs exact (L4), and (c) PIT-irreversibility + byte-identical snapshot replay (L5/L6/L7).*

---

## §2 — File structure

### 2.1 atx-core Pattern-B requests (decided at kickoff; engine-local / atx-tsdb fallback ships S4)

> The engine adds no general-purpose primitive (project rule). S4 records four cross-module edges and ships on existing primitives, exactly as S1 (`stats_ext`→atx-core L6), S2 (`DetPool`→L4), S3 (sep-CMA-ES→L7) did:
>
> 1. **mmap-RAII + a writable/growable mapping → atx-core L3.** Ship on `atx::tsdb::Mapping` (read-only; the append path uses buffered file I/O + remap, §0.1).
> 2. **A fixed-seed cross-run-stable hash → atx-core.** Ship on the S3 `factory::canonical_hash` (the persisted key, §0.4); the `tsdb::crc32` precedent shows the need is real.
> 3. **An LSH / signed-random-projection primitive → atx-core.** Ship engine-local on `simd::dot` + `Xoshiro256pp::normal()` (§0.6).
> 4. **An unbounded, pairwise-complete 2-var covariance accumulator → atx-core L6.** Not built this sprint — the accelerator wraps exact `pairwise_complete_corr` (§0.5); recorded as the residual if a maintained-summary path is wanted later.

### 2.2 Engine `library/` layer (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/library/fwd.hpp` | forward decls + the doc block (per-unit header map, namespace `atx::engine::library`, the LSM memtable→segment model, the sqlite-owns-mutable-metadata split) | S4-0 |
| `include/atx/engine/library/record.hpp` | the on-disk schema: `SegmentHeader`/`SegmentFooter` (tsdb-framing clone, §0.2), `AlphaDirEntry{AlphaId, canon_hash, lifecycle@seal, AlphaMetrics, prov_off, prov_len}`, `Provenance{expr_source, parent_hashes, mutation_op, seed}` (de)serialize; `kLibMagic = tag8("ATXALIB1")`, `kLibFormatVersion` (§0.3) | S4-1 |
| `include/atx/engine/library/store.hpp` | `LibraryStore` — append-only segmented store: `stage(...)` (into the in-mem `AlphaStore` memtable), `flush()` (write a sealed segment, one-pass, crc), `attach`/mmap-ro read view; read API **mirrors `AlphaStore`** (`n_alphas/pnl(id)/positions(id,t)/get(id)`) over the union of segments; survivorship/NaN verbatim (§0.3/L1/L2/L8) | S4-1 |
| `include/atx/engine/library/dedup_index.hpp` | `DedupIndex` — library-wide `canonical_hash → AlphaId`; sqlite table (UNIQUE on hash) + in-mem `HashMap<u64,AlphaId>` cache w/ identity hasher; `contains/find/insert`; lifts S3 dedup to the library (§0.4/L3) | S4-2 |
| `include/atx/engine/library/corr_index.hpp` | `CorrNeighborIndex` — SimHash signed-projection over demeaned PnL (`K` seeded hyperplanes); `add(id, pnl)`, `neighbors(pnl)→candidate ids`; `online_corr_to_pool(candidate, store, index)` = MAX \|exact corr\| over candidates; the exact-vs-approx differential (§0.5/§0.6/L4) | S4-3 |
| `include/atx/engine/library/lifecycle.hpp` | `LifecycleState` enum + legal-transition table; `LifecycleJournal` (sqlite append-only `{AlphaId, from, to, as_of_period, seq}`); `state_as_of(id, t)`; PIT no-retroactive (§0.7/L5) | S4-4 |
| `include/atx/engine/library/manifest.hpp` | `LibraryManifest` — content-addressed snapshot: ordered `{AlphaId, canon_hash, lifecycle@snapshot, segment_crc}` + master seeds + a manifest `crc`; `write`/`read`/`rebuild_equals(...)` (byte-identical proof, §0.2/L6/L7) | S4-5 |
| `include/atx/engine/library/library.hpp` | `Library` facade — `open(dir)`, `admit(candidate, gate)` (dedup→corr-gate→stage; flush on batch), `snapshot()→version`, `restore(version)`; ties store+dedup+corr+lifecycle+manifest; the dangling-span discipline (corr **before** stage) (§4.7) | S4-5 |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)
`library_record_test.cpp` (S4-1), `library_store_test.cpp` (S4-1), `library_dedup_index_test.cpp` (S4-2), `library_corr_index_test.cpp` (S4-3), `library_lifecycle_test.cpp` (S4-4), `library_manifest_test.cpp` (S4-5), `library_integration_test.cpp` (S4-5, the round-trip + dedup + incremental-corr-differential + PIT + byte-identical-snapshot proofs). Bench: `bench/library_bench.cpp` (S4-5).

### 2.4 Ledger
`sprint-4-progress.md` (S4-0), updated per unit (copy `sprint-3-progress.md` / `sprint-2-progress.md` shape). Likely sub-sprint split **S4-a** (S4-0…S4-2: store + dedup) / **S4-b** (S4-3…S4-5: corr index, lifecycle, manifest+close) per the ROADMAP's ">7 units" rule (S4 is 6 units incl. marker — borderline; split only if S4.1+S4.3 over-run).

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Suite_Condition_ExpectedResult`; cover happy path, **boundaries** (empty library, single alpha, one-segment vs many-segment reads, all-NaN stream, delisted/final-value column, empty pool corr, duplicate-on-first-insert, lifecycle on a fresh id, snapshot of an empty library), and the **invariant proofs** (L2/L8 byte-identical round-trip incl. NaN, L3 dedup non-vacuous, L4 corr-index-vs-exact differential, L5 PIT no-retroactive, L6 byte-identical rebuild).
- **Append-only / immutability (L1):** no code path rewrites or truncates a sealed segment; lifecycle/dedup are append/insert only; a test asserts a sealed segment file's bytes are unchanged after subsequent admits.
- **Determinism (L7):** SRP hyperplanes seeded by a recorded master seed via `Xoshiro256pp` (never time/thread); all index/catalog iteration in `AlphaId` order; segment + manifest bytes reproducible. A **two-builds-equal** test (same inputs → byte-identical segment + manifest crc) is mandatory for S4-1 and S4-5.
- **No look-ahead / PIT (L5):** lifecycle transitions keyed to an as-of period; `state_as_of` monotone; a later transition never alters an earlier query. The store/library never reads not-yet-known data (it stores already-realized streams).
- **mmap safety (L2):** every read goes through `Mapping` + `attach`-validated bytes; `// SAFETY:` on every span that aliases the mapping (it dangles when the `Mapping`/`SegmentReader` is destroyed or the library re-flushes); the read view holds the `Mapping` alive.
- **Dangling-span (combine):** `AlphaStore::pnl()/positions()` and `LibraryStore` read spans alias backing storage — compute corr-to-pool and copy out **before** any `stage()`/`flush()`/`insert()` (§0.3, the S3-inherited hazard).
- **No hot-path alloc (L2):** the read/query path reuses pre-sized scratch (signature buffers, candidate vectors); the cold flush/compile/snapshot path may allocate (documented).
- **sqlite discipline (§0.8):** one `Database` on the serial admit/flush thread only; wrap multi-row writes in a `Transaction`; use `prepare_cached`; never hand a `Statement`/`Database` to an S2 worker.
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). clang-tidy disabled — the strict build + ctest are the gate.
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures (open-fail, crc-mismatch, dup, unknown-id); weakest sufficient types (`std::span`, `const&`, `std::string_view`); functions ≤ ~60 lines; **reuse `atx::tsdb` (`Mapping`/`crc32`/framing) and `atx::core::db` — do NOT reinvent mmap, crc, or a sqlite layer**; no new general-purpose primitive in the engine (SRP is the one self-contained numeric helper, recorded as a Pattern-B lift).
- **clangd noise:** ignore squiggles; only a real `cmake --build` + ctest are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree:
atx-tsdb/include/atx/tsdb/segment.hpp + segment_reader.hpp + mapping.hpp + checksum.hpp (REUSE these: the
read-only mmap RAII `Mapping::map_file_ro`, the byte-stable `crc32`, the tag8/magic + section-byte-offset +
seal-marker + validate-before-expose framing you CLONE for the library-segment layout); atx-tsdb/builder.hpp
(the whole-file one-pass sealed write you mirror per segment); atx-core/include/atx/core/db/sqlite.hpp +
db/blob.hpp (the sqlite wrapper for the MUTABLE indices: Database/Statement/Transaction/BlobStream, Result<T>,
BLOB bind/read); combine/store.hpp (the in-memory AlphaStore you SCALE — flat alpha-major pnl_/pos_, AlphaId =
row index, NaN verbatim, ZERO serialization, provenance is only a non-serializable ISignalSource*);
combine/gate.hpp + combine/correlation.hpp (the MAX-|corr| screen + the ONLY exact corr primitive
pairwise_complete_corr — your accelerator's differential reference); the S3 factory/canonical.hpp
(factory::canonical_hash = the cross-run-STABLE dedup key); atx-core simd.hpp (dot) + random.hpp
(Xoshiro256pp::normal for seeded SRP hyperplanes) + container/hash_map.hpp (HashMap/HashSet, custom identity
hasher for u64 keys).

THIS SPRINT'S DOMINANT RISK IS SILENT DATA CORRUPTION AT SCALE: a store that round-trips WRONG, a dedup that
drops a distinct alpha, a corr-accelerator that misses a true high-correlation neighbor, or a snapshot that
does not replay byte-identically. The gates:
  - BYTE-IDENTICAL ROUND-TRIP (L2/L8): write a fixture (incl. delisted/final-value columns and NaN cells)
    to a sealed segment, mmap it back, assert EVERY pnl/pos/metric byte equals the in-memory AlphaStore.
    NaN must round-trip as the same bit pattern. crc32 validated on attach.
  - DEDUP SOUNDNESS (L3): re-submitting a structurally-equivalent alpha is REJECTED (non-vacuous: a genuinely
    new one is ADMITTED). The key is the S3 canonical_hash; hash-equal => bit-identical eval (S3 F6).
  - CORR ACCELERATOR DIFFERENTIAL (L4): the index's MAX-|corr|-to-pool must MATCH the exact O(N^2) recompute
    within a DOCUMENTED recall bound on a fixture (a test that only checks "it returned something" is vacuous);
    record the measured speedup AND the miss rate in the ledger. The corr over the returned candidates is EXACT
    (pairwise_complete_corr); only neighbor RECALL is approximate.
  - PIT IRREVERSIBILITY (L5): lifecycle transitions are journal APPENDS keyed to an as-of period; a later
    transition must NOT change an earlier state_as_of(id, t) query. No in-place UPDATE, ever.
  - BYTE-IDENTICAL SNAPSHOT REPLAY (L6/L7): rebuilding from a manifest yields identical segment bytes + manifest
    crc. Two-builds-equal is mandatory.

APPEND-ONLY, IMMUTABLE: never rewrite/truncate a sealed segment; AlphaId = global append index. Reads go
through Mapping; every span aliasing the mapping DANGLES when the Mapping/SegmentReader dies — hold it alive,
// SAFETY: every such borrow. AlphaStore/LibraryStore read spans DANGLE after the next stage()/flush()/insert()
— compute corr-to-pool and copy out BEFORE growth. sqlite is SINGLE-Database-per-thread (SQLITE_THREADSAFE=2):
only the serial admit/flush path owns it; NEVER hand it to an S2 worker; wrap batch writes in a Transaction.

No UB, no hidden look-ahead, no second corr/fitness convention (reuse pairwise_complete_corr + compute_metrics).
Header-only inline EXCEPT you now link atx-core's COMPILED db/sqlite TU (confirm the test target resolves it,
S4-0). Functions <= ~60 lines. Build gate: cmake --build build --config Debug --target atx-engine-tests
(/W4 /permissive- /WX + /fp:precise) + ctest -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER
git add -A; after committing run `git show HEAD --stat` (only your files); never touch atx-core/* or atx-tsdb/*;
do not push. End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 On-disk record + segment framing (S4-1)

The library = an ordered set of **immutable sealed segments** + a sqlite catalog + a manifest. Each segment clones the `atx::tsdb` framing (§0.2) over an alpha-library payload:
```
// section order (one contiguous file; all refs are byte offsets from base()):
//   SegmentHeader | AlphaDirectory | PnlBlock | PosBlock | ProvenanceBlob | SegmentFooter
struct SegmentHeader {                  // POD, trivially copyable, LE (static_assert)
  u64 magic;            // kLibMagic = tag8("ATXALIB1")
  u32 format_version;   // kLibFormatVersion (is_supported_version guards reads)
  u32 flags;            // bit0 = sealed
  u64 total_bytes;
  u32 n_alphas;         // rows in THIS segment
  u32 n_instruments;    // N (== AlphaStore n_instruments_)
  u64 n_periods;        // T
  u64 base_alpha_id;    // global AlphaId of local row 0 (L1 stable id)
  u64 content_hash;     // tsdb::crc32 over the data sections (zero-extended u64)
  u64 off_dir, off_pnl, off_pos, off_prov, off_footer;
};
struct AlphaDirEntry {                   // one per alpha; fixed-width => O(1) addressable
  u64 alpha_id;         // global; == base_alpha_id + local row
  u64 canon_hash;       // the S3 stable key (L3)
  u32 lifecycle_at_seal;// LifecycleState as of seal (snapshot convenience; journal is authoritative)
  u32 _pad;
  AlphaMetrics metrics; // 7 x f64 (combine::AlphaMetrics, copied verbatim)
  u64 prov_off, prov_len; // slice into ProvenanceBlob
};
struct SegmentFooter { u64 seal_marker; u32 integrity_crc; u32 reserved; };  // crc over [header..footer)
// PnlBlock: n_alphas * n_periods f64, ALPHA-MAJOR (a*T + t)         -- mirrors AlphaStore pnl_ (0.3)
// PosBlock: n_alphas * n_periods * n_instruments f64, alpha->period->instrument  -- mirrors pos_ (0.3)
// ProvenanceBlob: concatenated serialized Provenance records (variable length)
struct Provenance { string expr_source; vector<u64> parent_hashes; u16 mutation_op; u64 seed; };
```
**Write (one pass, mirrors `tsdb::SegmentBuilder::write`):** lay out sections, copy `pnl_`/`pos_` verbatim (NaN preserved, L8), serialize provenance, compute `content_hash` then `integrity_crc` via `tsdb::crc32`, set sealed, write whole file. **Read:** `tsdb::Mapping::map_file_ro` → validate magic/version/seal/crc **before** exposing → reinterpret header + sections off `base()` (zero-copy, L2).

### 4.2 LibraryStore — append-only staging + segmented read (S4-1)

```
class LibraryStore {
  AlphaStore                  memtable_;     // the P4 in-mem store = the staging buffer (reused verbatim)
  vector<SegmentReaderLite>   segments_;     // each holds a tsdb::Mapping + validated header (read view)
  Database                    catalog_;      // sqlite: segment_id -> {path, base_alpha_id, n_alphas, crc}
  u64                         next_alpha_id_;

  stage(source, pnl, pos_flat, metrics, prov) -> Result<AlphaId>:
    id := memtable_.insert(source, pnl, pos_flat, metrics)     // dense append, dims fixed at first insert
    pending_prov_[id] := prov                                   // provenance the AlphaStore can't hold (0.3)
    return AlphaId{ next_alpha_id_ + id.value }

  flush() -> Status:                                            // cold path; allocation OK
    if memtable_.n_alphas() == 0: return Ok
    seg := write_segment(memtable_, pending_prov_, base = next_alpha_id_)   // §4.1 one-pass sealed write
    catalog_.insert(seg.id, seg.path, next_alpha_id_, memtable_.n_alphas(), seg.crc)  // Transaction
    segments_.push( SegmentReaderLite::attach(seg.path) )       // mmap-ro + validate (L2)
    next_alpha_id_ += memtable_.n_alphas()
    memtable_.clear(); pending_prov_.clear()
    return Ok

  // read API mirrors AlphaStore, over the UNION of sealed segments + the live memtable:
  n_alphas() := next_alpha_id_ + memtable_.n_alphas()
  pnl(AlphaId g) -> span<const f64>:                            // SAFETY: aliases a Mapping; dangles on re-flush
    if g >= next_alpha_id_: return memtable_.pnl(local(g))      // live rows
    (seg, local) := locate(g)                                   // binary search base_alpha_id
    return seg.pnl_row(local)                                   // zero-copy into the mmap
  positions(AlphaId g, period) -> span<const f64>:  ... (same dispatch)
  get(AlphaId g) -> AlphaRecordView:  ... (metrics + provenance + canon_hash)
}
```
**Byte-identical exit criterion (L2/L8):** stage a fixture into `memtable_`, `flush()`, re-`attach`, and assert `library.pnl(id)` / `positions(id,t)` / `get(id).metrics` equal the in-memory `AlphaStore` values **bit-for-bit** (NaN cells equal by bit pattern). The provenance is the *added* field (no in-memory counterpart to compare).

### 4.3 DedupIndex — library-wide canonical-hash dedup (S4-2)

```
class DedupIndex {
  HashMap<u64, AlphaId, IdentityHash> cache_;   // in-mem; u64 canon_hash already mixed (0.4)
  Database                            db_;        // sqlite: TABLE dedup(canon_hash INTEGER PRIMARY KEY, alpha_id INTEGER)

  contains(u64 h) -> bool                         := cache_.contains(h)
  find(u64 h)     -> optional<AlphaId>            := cache_.find(h)
  insert(u64 h, AlphaId id) -> Result<bool>:      // false = already present (dup), true = newly inserted
    if cache_.contains(h): return Ok(false)
    db_.exec("INSERT OR IGNORE INTO dedup VALUES(?,?)")   // UNIQUE => library-wide (L3)
    if db_.changes() == 0: return Ok(false)               // raced/duplicate
    cache_.insert(h, id); return Ok(true)
  // open(): SELECT canon_hash, alpha_id  -> rebuild cache_ in AlphaId order (deterministic, L7)
}
```
A candidate's `canonical_hash` (from S3) is checked **before** scoring/admission; a hit short-circuits (never re-evaluate or re-store a structural duplicate). Soundness is S3's F6 (hash-equal ⇒ bit-identical eval). Non-vacuous test: a second admit of `add(rank(close), ts_mean(volume,10))` written as `add(ts_mean(volume,10), rank(close))` (commutative reorder → same canonical hash) is **rejected**; a genuinely different expression is **admitted**.

### 4.4 CorrNeighborIndex — the incremental corr-to-pool gate (S4-3)

The o(N) replacement for the O(N²) re-gate (L4). Correlation = cosine of demeaned vectors; SimHash buckets approximate cosine, so neighbors-by-bucket yield a high-recall candidate set, over which we compute the **exact** `pairwise_complete_corr`:
```
class CorrNeighborIndex {
  u32                         K;             // # hyperplanes (signature bits), e.g. 64
  vector<vector<f64>>         H;             // K random unit vectors over R^T (seeded, L7)
  HashMap<u64, vector<AlphaId>> buckets_;    // signature (or banded prefix) -> member ids

  ctor(master_seed, T, K):
    rng := Xoshiro256pp(master_seed)
    for k in 0..K: H[k] := normalize([ rng.normal() for _ in 0..T ])   // §0.6: simd over Xoshiro::normal

  signature(pnl) -> u64:                                          // T-vector -> K-bit SimHash
    d := demean_ignoring_nan(pnl)                                 // NaN -> 0 after demean (pairwise-safe-ish)
    bits := 0
    for k in 0..K: bits |= (simd::dot(d, H[k]) >= 0 ? 1 : 0) << k  // sign of projection (Charikar)
    return bits

  add(id, pnl):    buckets_[ band(signature(pnl)) ].push(id)     // band = multi-probe prefix for recall
  neighbors(pnl) -> vector<AlphaId>:                             // union of probed bands (small candidate set)
    return union over probes p of buckets_[ band_p(signature(pnl)) ]
}

online_corr_to_pool(candidate_pnl, store, index) -> f64:        // the gate's MAX |corr|, incrementally
  worst := 0.0
  for id in index.neighbors(candidate_pnl):                      // o(neighbors), NOT o(N)
    c := combine::pairwise_complete_corr(candidate_pnl, store.pnl(id))   // EXACT over the candidate (0.5)
    worst := max(worst, abs(c))
  return worst                                                   // empty neighbor set -> 0 (matches empty pool)
```
**Exact-vs-approx differential (L4, the load-bearing test):** on a fixture pool, compare `online_corr_to_pool` against the exact `max over ALL ids of |pairwise_complete_corr|`; the corr value over returned neighbors is exact, so the only error is **recall** (a true high-corr member not bucketed together). Measure recall at the chosen `(K, probe-width)`, assert it ≥ a documented bound, and record the **speedup** (neighbors / N) in the ledger. A periodic full audit (cold path) bounds drift. The index is deterministic: `H` is seeded; bucket member lists are kept in `AlphaId` order.

### 4.5 Lifecycle state machine — PIT, irreversible (S4-4)

```
enum class LifecycleState : u8 { Candidate, Admitted, Live, Decaying, Dead, Recycled };
// legal transitions (the spine S7 drives):
//   Candidate -> Admitted -> Live -> Decaying -> {Live (recover) | Dead}
//   Dead -> Recycled            (Kakushadze dead-alpha -> risk factor, S7)
//   (any) -> itself: no-op rejected; illegal pair -> Err(InvalidArgument)

class LifecycleJournal {
  Database db_;   // TABLE journal(seq INTEGER PK AUTOINCREMENT, alpha_id, from_state, to_state, as_of_period)

  transition(id, to, as_of_period) -> Result<void>:             // APPEND ONLY (L5)
    cur := state_as_of(id, +inf)                                 // latest known state
    if not legal(cur -> to): return Err(InvalidArgument)
    db_.exec("INSERT INTO journal(alpha_id,from_state,to_state,as_of_period) VALUES(?,?,?,?)")
    // NO UPDATE/DELETE, ever -> no retroactive relabel
  state_as_of(id, t) -> LifecycleState:                          // PIT query (0.7)
    row := db_.query("SELECT to_state FROM journal WHERE alpha_id=? AND as_of_period<=? "
                     "ORDER BY as_of_period DESC, seq DESC LIMIT 1", id, t)
    return row ? row.to_state : Candidate                        // unborn id => Candidate
}
```
**PIT no-retroactive proof (L5):** record `transition(id, Decaying, as_of=100)`; query `state_as_of(id, 50) == Admitted` (or Candidate); then `transition(id, Dead, as_of=200)`; re-query `state_as_of(id, 50)` — **unchanged**. A later transition never alters an earlier query.

### 4.6 Manifest + reproducible rebuild (S4-5)

```
struct ManifestEntry { u64 alpha_id; u64 canon_hash; u8 lifecycle_at_snapshot; u64 segment_crc; };
struct LibraryManifest {
  u64               version_id;        // content-address = crc32 over the ordered entries + seeds
  vector<u64>       master_seeds;      // the S3 search seed(s) + the SRP seed (so the index rebuilds identically)
  vector<ManifestEntry> entries;       // ordered by alpha_id (L7)
};

snapshot(library) -> LibraryManifest:                            // content-addressed (L6)
  entries := [ {id, store.get(id).canon_hash, journal.state_as_of(id, now), seg_crc(id)}
               for id in 0..library.n_alphas() ]                 // alpha_id order
  m := { crc32(entries ++ seeds), seeds, entries }
  write(m, dir/manifest.<version_id>)                            // + sqlite snapshot of the catalog
  return m

rebuild_equals(manifest, dir) -> bool:                           // the reproducibility proof
  // re-emit each segment from its provenance + the recorded seeds, byte-for-byte;
  // assert every recomputed segment_crc == manifest.entries[*].segment_crc
  // assert recomputed manifest version_id == manifest.version_id
```
**Byte-identical snapshot replay (L6/L7):** build a library twice from the same inputs/seeds → identical segment bytes and identical `manifest.version_id`. An experiment cites `version_id`; `restore(version_id)` re-attaches exactly those segment crcs.

### 4.7 Library facade — the admit path (S4-5)

```
Library::admit(candidate, gate) -> AdmitVerdict:                 // candidate = {canon_hash, pnl, pos_flat, metrics, prov}
  // 1. library-wide dedup (L3) — cheapest gate first
  if dedup_.contains(candidate.canon_hash): return Duplicate
  // 2. corr-to-pool BEFORE staging (dangling-span discipline, §0.3): copy pnl out, query the index
  worst_corr := online_corr_to_pool(candidate.pnl, store_, corr_)          // o(neighbors)
  // 3. the P4 gate semantics, preserved exactly (MAX |corr| vs max_pool_corr)
  if candidate.metrics.sharpe   <  gate.cfg.min_sharpe:   return RejectSharpe
  if candidate.metrics.fitness  <  gate.cfg.min_fitness:  return RejectFitness
  if candidate.metrics.turnover >  gate.cfg.max_turnover: return RejectTurnover
  if worst_corr                 >  gate.cfg.max_pool_corr:return RejectCorrelated
  // 4. admit: stage -> index -> dedup -> lifecycle (Candidate -> Admitted), all in AlphaId order (L7)
  id := store_.stage(candidate.source, candidate.pnl, candidate.pos_flat, candidate.metrics, candidate.prov)
  corr_.add(id, candidate.pnl); dedup_.insert(candidate.canon_hash, id)
  journal_.transition(id, Admitted, as_of_period = candidate.as_of)
  if store_.memtable_.n_alphas() >= cfg.flush_batch: store_.flush()        // cold path
  return Accept(id)
```
**Note:** step 3 re-expresses `AlphaGate::admit`'s fixed-order checks but feeds the **incremental** `worst_corr` instead of the gate's internal O(N) `max_abs_corr_to_pool` — same verdict, o(N) cost. A `FullGateEquivalence` test asserts the facade verdict equals `AlphaGate::admit(metrics, pnl, equivalent_in_mem_pool)` on a fixture (the differential, §0.5/L4). The trial/admit counters feed the ledger throughput story (bench §5 S4-5).

---

## §5 — Per-unit plan

> Sequential dispatch (each unit consumes the prior). Fresh implementer → spec-compliance review → code-quality review → fix loop → ledger SHA, per `superpowers:subagent-driven-development`. **Shared branch `feat/atx-core-stdlib` → explicit-pathspec commits** (handoff block). Suggested split: **S4-a** = S4-0…S4-2, **S4-b** = S4-3…S4-5.

### Task S4-0: Marker + ledger + library scaffold + sqlite build check
**Files:** Create `atx-engine/plans/p1/sprint-4-progress.md`, `atx-engine/include/atx/engine/library/fwd.hpp`. Possibly Modify `atx-engine/CMakeLists.txt` / `atx-engine/tests/CMakeLists.txt` (only if the sqlite symbol check fails).
- [ ] **Step 1:** Write the ledger from the `sprint-3-progress.md` shape: header (`Base: feat/atx-core-stdlib @ <HEAD>`, in-place, shared-branch note), a "Plan adjustments vs source" paragraph quoting §0 (reuse-atx-tsdb-Mapping/crc32-not-atx-core; tsdb-framing-cloned-record-layout-built; AlphaStore-has-no-provenance-and-no-serialization; canonical-key-is-S3's; corr-is-SimHash-accelerator-around-exact-pairwise_complete_corr; no-LSH-in-core; no-time-axis-PIT-by-as-of-period; **sqlite-is-a-compiled-TU**), empty per-unit table S4-0…S4-5, empty commits table. Record the **four Pattern-B edges** (§2.1) and the **S3-canonical-hash dependency** (with the local-fallback note, §0.4) as kickoff risks.
- [ ] **Step 2: sqlite build smoke-check** (the §0.8 reconciliation). Add a throwaway `tests/library_link_smoke_test.cpp` that opens an in-memory DB:
```cpp
#include "atx/core/db/sqlite.hpp"
#include <gtest/gtest.h>
TEST(LibraryLinkSmoke, OpensInMemorySqlite) {
  auto db = atx::core::db::Database::open_memory();
  ASSERT_TRUE(db.has_value());
  EXPECT_TRUE(db->exec("CREATE TABLE t(x INTEGER)").ok());
}
```
Run: `cmake --build build --config Debug --target atx-engine-tests` + `ctest -R LibraryLinkSmoke`. **Expected:** PASS (symbols resolve transitively via `atx::core`). **If it fails to link** (unresolved `sqlite3_*`/`atx_sqlite3`), add `atx_sqlite3` (or the atx-core db component) to `atx-engine-tests` `target_link_libraries` — one line — and re-run. Record which in the ledger. Then **delete the smoke test** (it was a wiring probe, not a unit).
- [ ] **Step 3:** `library/fwd.hpp` — forward decls (the `combine/fwd.hpp` pattern): namespace `atx::engine::library`; `struct SegmentHeader; struct AlphaDirEntry; struct Provenance; class LibraryStore; class DedupIndex; class CorrNeighborIndex; enum class LifecycleState : u8; class LifecycleJournal; struct LibraryManifest; class Library;` + a doc block listing the per-unit headers, the **LSM memtable→immutable-segment** model, and the **immutable-bulk-in-segments / mutable-metadata-in-sqlite** split.
- [ ] **Step 4:** Commit (marker): `git add -- atx-engine/plans/p1/sprint-4-progress.md atx-engine/include/atx/engine/library/fwd.hpp [CMakeLists.txt if changed]; git commit -- <them> -m "docs(s4-0): open sprint-4 library-management ledger + scaffold" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"; git show HEAD --stat`.

### Task S4-1: On-disk record schema + append-only segmented store
**Files:** Create `library/record.hpp`, `library/store.hpp`; Test `tests/library_record_test.cpp`, `tests/library_store_test.cpp`.
**Scope:** §4.1/§4.2 + §0.1/§0.2/§0.3/§0.8. The segment framing (tsdb clone), the `AlphaDirEntry`/`Provenance` (de)serialize, `LibraryStore` stage/flush/attach + the AlphaStore-mirroring read API. **First verify** `atx::tsdb::{Mapping::map_file_ro, crc32, tag8, SegmentHeader/Footer pattern}` and `combine::AlphaStore`'s exact layout (`pnl_` alpha-major `a*T+t`; `pos_` `(a*T+period)*N+j`; `AlphaMetrics` 7×f64; NaN verbatim) against the headers.
- [ ] **Step 1 (record tests):** suite `LibraryRecord` —
```cpp
TEST(LibraryRecord, HeaderRoundTripsLittleEndian) {
  SegmentHeader h = make_header(/*n_alphas*/3,/*N*/4,/*T*/8,/*base*/100);
  std::array<std::byte, sizeof(SegmentHeader)> buf;
  write_header(buf, h); SegmentHeader g = read_header(buf);
  EXPECT_EQ(g.magic, kLibMagic); EXPECT_EQ(g.base_alpha_id, 100u);
  EXPECT_TRUE(is_supported_version(g.format_version));
}
TEST(LibraryRecord, ProvenanceRoundTrips) {
  Provenance p{"add(rank(close), ts_mean(volume, 10))", {0xABCDull, 0x1234ull}, /*op*/2, /*seed*/777};
  std::vector<std::byte> b; serialize(p, b);
  Provenance q = deserialize_provenance(b);
  EXPECT_EQ(q.expr_source, p.expr_source);
  EXPECT_EQ(q.parent_hashes, p.parent_hashes);
  EXPECT_EQ(q.seed, 777u);
}
TEST(LibraryRecord, RejectsBadMagicAndCrc) {
  auto buf = make_sealed_segment_bytes(small_fixture());
  buf[0] ^= std::byte{0xFF};                              // corrupt magic
  EXPECT_FALSE(SegmentReaderLite::attach_bytes(buf).has_value());
  auto buf2 = make_sealed_segment_bytes(small_fixture());
  buf2[buf2.size()-8] ^= std::byte{0x01};                 // corrupt a data byte after crc was computed
  EXPECT_FALSE(SegmentReaderLite::attach_bytes(buf2).has_value());  // crc mismatch (L2)
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `record.hpp` (§4.1): POD `SegmentHeader`/`AlphaDirEntry`/`SegmentFooter` (`static_assert(is_trivially_copyable)` + size pins, LE), `kLibMagic = atx::tsdb::tag8("ATXALIB1")`, `is_supported_version`, `Provenance` serialize/deserialize (length-prefixed strings + vectors), and `crc` via `atx::tsdb::crc32`. **Step 4:** `ctest -R LibraryRecord` → pass.
- [ ] **Step 5 (store tests):** suite `LibraryStore` — the **round-trip + survivorship + determinism** proofs:
```cpp
TEST(LibraryStore, FlushedSegmentRoundTripsByteIdenticalToAlphaStore) {     // L2/L8 — load-bearing
  AlphaStore mem; auto fx = fixture_with_nan_and_delisted_cols();           // incl. NaN + final-value cols
  fill_store(mem, fx);
  LibraryStore lib(tmpdir()); stage_all(lib, mem, fx.provenance); lib.flush();
  LibraryStore reopened(tmpdir());                                          // attach from disk (mmap-ro)
  for (AlphaId id{0}; id.value < mem.n_alphas(); ++id.value) {
    auto a = mem.pnl(id);  auto b = reopened.pnl(id);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i=0;i<a.size();++i)
      EXPECT_EQ(std::bit_cast<u64>(a[i]), std::bit_cast<u64>(b[i]));        // bit-identical incl NaN
    EXPECT_EQ(reopened.get(id).metrics.fitness, mem.get(id).metrics.fitness);
  }
}
TEST(LibraryStore, ReadsAcrossMultipleSegments) {
  LibraryStore lib(tmpdir());
  stage_n(lib, 5); lib.flush(); stage_n(lib, 7); lib.flush();              // two sealed segments
  EXPECT_EQ(lib.n_alphas(), 12u);
  EXPECT_EQ(lib.pnl(AlphaId{11}).size(), lib.n_periods());                  // global id spans segments
}
TEST(LibraryStore, TwoBuildsByteIdentical) {                                // L7
  auto crc1 = build_and_seal(tmpdir("a"), fixed_fixture());
  auto crc2 = build_and_seal(tmpdir("b"), fixed_fixture());
  EXPECT_EQ(crc1, crc2);                                                    // identical segment bytes
}
TEST(LibraryStore, SealedSegmentImmutableAfterMoreAdmits) {                 // L1
  LibraryStore lib(tmpdir()); stage_n(lib,3); lib.flush();
  auto before = read_file_bytes(lib.segment_path(0));
  stage_n(lib,3); lib.flush();
  EXPECT_EQ(before, read_file_bytes(lib.segment_path(0)));                  // first segment untouched
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement `store.hpp` (§4.2): `memtable_` = `combine::AlphaStore`; `stage` (insert + buffer provenance), `flush` (one-pass sealed write via `record.hpp`, catalog row in a `Transaction`, `Mapping`-attach the new segment), `n_alphas/pnl/positions/get` dispatch (binary-search `base_alpha_id`, zero-copy span into the mmap with a `// SAFETY:` borrow note that the span dies with the `Mapping`), `open(dir)` re-attaches all catalog segments in id order. **Step 8:** `ctest -R "LibraryRecord|LibraryStore"` → pass; full suite green. **Step 9:** Commit + ledger row (`feat(s4-1): append-only mmap'd segmented library store + on-disk record`).

### Task S4-2: Library-wide canonical-hash dedup index
**Files:** Create `library/dedup_index.hpp`; Test `tests/library_dedup_index_test.cpp`.
**Scope:** §4.3 + §0.4. `DedupIndex` (sqlite UNIQUE table + in-mem `HashMap<u64,AlphaId>` identity-hashed cache); `contains/find/insert`; `open` rebuilds the cache in id order. **First verify** the S3 `factory::canonical_hash` signature (consume it; if absent, define the identical fixed-seed stable hash locally per §0.4) and the `db::Database`/`Statement`/`Transaction` API.
- [ ] **Step 1:** failing tests, suite `LibraryDedup` —
```cpp
TEST(LibraryDedup, RejectsStructurallyEquivalentResubmission) {            // L3 — non-vacuous
  DedupIndex idx(tmpdir());
  u64 h = factory::canonical_hash(parse("add(rank(close), ts_mean(volume,10))"));
  u64 h2= factory::canonical_hash(parse("add(ts_mean(volume,10), rank(close))")); // commutative reorder
  ASSERT_EQ(h, h2);                                                        // S3 canonicalization (F6)
  EXPECT_TRUE (idx.insert(h,  AlphaId{0}).value());                        // first: newly inserted
  EXPECT_FALSE(idx.insert(h2, AlphaId{1}).value());                        // dup: rejected library-wide
  EXPECT_TRUE (idx.contains(h));
}
TEST(LibraryDedup, AdmitsGenuinelyNew) {
  DedupIndex idx(tmpdir());
  EXPECT_TRUE(idx.insert(factory::canonical_hash(parse("ts_mean(close,5)")),  AlphaId{0}).value());
  EXPECT_TRUE(idx.insert(factory::canonical_hash(parse("ts_mean(close,6)")),  AlphaId{1}).value());
}
TEST(LibraryDedup, PersistsAcrossReopen) {                                  // L1 — survives process restart
  { DedupIndex idx(tmpdir()); idx.insert(0xDEADBEEFull, AlphaId{0}); }
  DedupIndex reopened(tmpdir());
  EXPECT_TRUE(reopened.contains(0xDEADBEEFull));                            // sqlite-backed, cache rebuilt
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `dedup_index.hpp` (§4.3): `Database` table `dedup(canon_hash INTEGER PRIMARY KEY, alpha_id INTEGER)` (UNIQUE = library-wide); `HashMap<u64,AlphaId,IdentityHash>` cache (`IdentityHash{ u64 operator()(u64 k) const noexcept { return k; } }`); `insert` = cache-check → `INSERT OR IGNORE` → `changes()` → cache-update; `open` = `SELECT … ORDER BY alpha_id` rebuild. **Step 4:** `ctest -R LibraryDedup` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s4-2): library-wide canonical-hash dedup index (sqlite + cache)`).

### Task S4-3: Correlation-neighbor index + incremental corr-to-pool
**Files:** Create `library/corr_index.hpp`; Test `tests/library_corr_index_test.cpp`.
**Scope:** §4.4 + §0.5/§0.6. `CorrNeighborIndex` (seeded SimHash over `simd::dot`+`Xoshiro::normal`); `online_corr_to_pool` = MAX |exact `pairwise_complete_corr`| over neighbors; the exact-vs-approx differential. **First verify** `simd::dot`, `Xoshiro256pp::normal`, and `combine::pairwise_complete_corr` signatures.
- [ ] **Step 1:** failing tests, suite `LibraryCorrIndex` — the **differential** (the load-bearing one) + determinism:
```cpp
TEST(LibraryCorrIndex, MatchesExactMaxCorrWithinRecallBound) {             // L4 — non-vacuous
  auto streams = random_pnl_pool(/*N*/512, /*T*/256, /*seed*/3);           // incl. a few near-duplicates
  LibraryStore store = staged(streams);
  CorrNeighborIndex idx(/*seed*/9, /*T*/256, /*K*/64); add_all(idx, store);
  int hits = 0;
  for (auto& q : query_set(streams, /*m*/64)) {
    f64 approx = online_corr_to_pool(q, store, idx);
    f64 exact  = exact_max_abs_corr(q, store);                             // O(N) reference (pairwise_complete)
    if (std::abs(approx - exact) < 1e-12) ++hits;                          // approx is EXACT when recalled
  }
  EXPECT_GE(hits / 64.0, 0.90);                                            // documented recall bound
}
TEST(LibraryCorrIndex, NeighborSetMuchSmallerThanPool) {                    // L4 — the speedup
  auto store = staged(random_pnl_pool(2000, 128, 1));
  CorrNeighborIndex idx(1, 128, 64); add_all(idx, store);
  EXPECT_LT(median_neighbor_count(idx, store), 0.2 * store.n_alphas());     // o(N), recorded in ledger
}
TEST(LibraryCorrIndex, IdenticalStreamIsItsOwnNeighborCorrOne) {
  auto store = staged({stream_a});
  CorrNeighborIndex idx(7, len(stream_a), 64); idx.add(AlphaId{0}, stream_a);
  EXPECT_NEAR(online_corr_to_pool(stream_a, store, idx), 1.0, 1e-9);
}
TEST(LibraryCorrIndex, SameSeedSameSignatures) {                            // L7
  CorrNeighborIndex a(42, 64, 32), b(42, 64, 32);
  EXPECT_EQ(a.signature(stream_x), b.signature(stream_x));                  // seeded hyperplanes reproducible
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `corr_index.hpp` (§4.4): seeded `H` (normalized `Xoshiro::normal` vectors), `signature` (`simd::dot` sign bits over demeaned/NaN-zeroed stream), banded multi-probe `buckets_` (member lists in `AlphaId` order), `neighbors` (probe-union), `online_corr_to_pool` (exact `pairwise_complete_corr` over neighbors, MAX |·|; `// SAFETY:` corr computed on the candidate's copied pnl **before** any store growth, §0.3). Document the `(K, probe-width)` → recall trade-off inline. **Step 4:** `ctest -R LibraryCorrIndex` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s4-3): SimHash corr-neighbor index + incremental corr-to-pool`). Note the **LSH→atx-core** Pattern-B residual + the measured recall/speedup.

### Task S4-4: Lifecycle state machine (PIT, append-only journal)
**Files:** Create `library/lifecycle.hpp`; Test `tests/library_lifecycle_test.cpp`.
**Scope:** §4.5 + §0.7. `LifecycleState` enum + legal-transition table; `LifecycleJournal` (sqlite append-only) with `transition`/`state_as_of`. **First verify** the `db::Database`/`Transaction` API.
- [ ] **Step 1:** failing tests, suite `LibraryLifecycle` — the **PIT no-retroactive** proof:
```cpp
TEST(LibraryLifecycle, LegalTransitionsOnly) {
  LifecycleJournal j(tmpdir());
  EXPECT_TRUE (j.transition(AlphaId{0}, LifecycleState::Admitted, 10).ok());  // Candidate->Admitted ok
  EXPECT_TRUE (j.transition(AlphaId{0}, LifecycleState::Live,     20).ok());
  EXPECT_FALSE(j.transition(AlphaId{0}, LifecycleState::Candidate,30).ok());  // Live->Candidate illegal
}
TEST(LibraryLifecycle, TransitionsArePitIrreversible) {                       // L5 — load-bearing
  LifecycleJournal j(tmpdir());
  j.transition(AlphaId{0}, LifecycleState::Admitted, 100);
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 50),  LifecycleState::Candidate);       // before its birth
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 150), LifecycleState::Admitted);
  j.transition(AlphaId{0}, LifecycleState::Decaying, 200);                    // a LATER transition
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 150), LifecycleState::Admitted);        // earlier query UNCHANGED
  EXPECT_EQ(j.state_as_of(AlphaId{0}, 250), LifecycleState::Decaying);
}
TEST(LibraryLifecycle, DeadToRecycled) {                                      // S7 baton
  LifecycleJournal j(tmpdir());
  j.transition(AlphaId{0}, LifecycleState::Admitted, 1);
  j.transition(AlphaId{0}, LifecycleState::Live, 2);
  j.transition(AlphaId{0}, LifecycleState::Decaying, 3);
  j.transition(AlphaId{0}, LifecycleState::Dead, 4);
  EXPECT_TRUE(j.transition(AlphaId{0}, LifecycleState::Recycled, 5).ok());
}
TEST(LibraryLifecycle, PersistsAcrossReopen) {                                // L1
  { LifecycleJournal j(tmpdir()); j.transition(AlphaId{0}, LifecycleState::Admitted, 1); }
  LifecycleJournal r(tmpdir());
  EXPECT_EQ(r.state_as_of(AlphaId{0}, 999), LifecycleState::Admitted);
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `lifecycle.hpp` (§4.5): the enum + a `constexpr legal(from,to)` table, `Database` table `journal(seq INTEGER PRIMARY KEY AUTOINCREMENT, alpha_id, from_state, to_state, as_of_period)`, `transition` (legality check → INSERT, **no UPDATE/DELETE ever**), `state_as_of` (`ORDER BY as_of_period DESC, seq DESC LIMIT 1`). **Step 4:** `ctest -R LibraryLifecycle` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s4-4): PIT append-only lifecycle state machine`).

### Task S4-5: Versioned manifest + Library facade + integration + bench + close
**Files:** Create `library/manifest.hpp`, `library/library.hpp`, `tests/library_manifest_test.cpp`, `tests/library_integration_test.cpp`, `bench/library_bench.cpp`; Modify ledger + `ROADMAP.md` (S4 row) + spec (closed) + create `sprint-4.md` user ref.
**Scope:** §4.6/§4.7 — the content-addressed snapshot, the facade admit path, the spec's exit criteria, the bench, the close.
- [ ] **Step 1 (manifest tests):** suite `LibraryManifest` —
```cpp
TEST(LibraryManifest, RebuildIsByteIdentical) {                              // L6/L7 — the reproducibility proof
  auto v1 = build_library(tmpdir("a"), fixed_inputs(/*seed*/5)).snapshot();
  auto v2 = build_library(tmpdir("b"), fixed_inputs(/*seed*/5)).snapshot();
  EXPECT_EQ(v1.version_id, v2.version_id);                                   // content-address equal
  EXPECT_EQ(segment_crcs(tmpdir("a")), segment_crcs(tmpdir("b")));           // identical bytes
}
TEST(LibraryManifest, VersionIdChangesWithContent) {
  auto v1 = build_library(tmpdir("a"), fixed_inputs(5)).snapshot();
  auto v2 = build_library(tmpdir("b"), one_more_alpha(fixed_inputs(5))).snapshot();
  EXPECT_NE(v1.version_id, v2.version_id);
}
```
- [ ] **Step 2 (integration tests):** suite `LibraryIntegration` — the spec exit criteria end-to-end:
```cpp
TEST(LibraryIntegration, RoundTripsLargeFixtureZeroCopy) {                   // exit #1
  Library lib(tmpdir()); admit_fixture(lib, /*n*/4096);
  Library re(tmpdir());                                                      // mmap-ro reattach
  EXPECT_EQ(re.n_alphas(), 4096u);
  EXPECT_EQ(re.get(AlphaId{4095}).metrics.sharpe, expected_last_sharpe());
}
TEST(LibraryIntegration, DedupRejectsEquivalentAdmitsNew) {                  // exit #2
  Library lib(tmpdir()); AlphaGate gate{default_gate_cfg()};
  EXPECT_EQ(lib.admit(cand("add(rank(close),ts_mean(volume,10))"), gate).kind, Accept);
  EXPECT_EQ(lib.admit(cand("add(ts_mean(volume,10),rank(close))"), gate).kind, Duplicate); // reorder
  EXPECT_EQ(lib.admit(cand("ts_std(close,20)"), gate).kind, Accept);
}
TEST(LibraryIntegration, IncrementalGateMatchesExactGate) {                  // exit #3 (L4 facade differential)
  Library lib(tmpdir()); AlphaGate gate{default_gate_cfg()}; admit_fixture(lib, 1000);
  for (auto& c : probe_candidates(64)) {
    auto fast = lib.admit_verdict_only(c, gate);                            // SimHash-accelerated
    auto slow = exact_gate_verdict(c, gate, lib);                          // AlphaGate over the whole pool
    EXPECT_EQ(fast, slow);                                                  // same verdict, o(N) cost
  }
}
TEST(LibraryIntegration, LifecycleIsPitThroughFacade) {                      // exit #4
  Library lib(tmpdir()); auto id = lib.admit(cand("ts_mean(close,5)"), default_gate()).id();
  lib.mark(id, LifecycleState::Live, /*as_of*/100);
  lib.mark(id, LifecycleState::Decaying, /*as_of*/200);
  EXPECT_EQ(lib.state_as_of(id, 150), LifecycleState::Live);                 // not retroactively Decaying
}
TEST(LibraryIntegration, SnapshotReplaysByteIdentical) {                     // exit #5
  auto va = build_library(tmpdir("a"), fixed_inputs(7)).snapshot();
  auto vb = build_library(tmpdir("b"), fixed_inputs(7)).snapshot();
  EXPECT_EQ(va.version_id, vb.version_id);
}
```
- [ ] **Step 3:** Build → FAIL. **Step 4:** Implement `manifest.hpp` (§4.6: ordered entries + seeds, `crc32` content-address, `write`/`read`/`rebuild_equals`) and `library.hpp` (§4.7: `open`/`admit` (dedup→corr-gate→stage→index→journal, corr **before** stage)/`mark`/`state_as_of`/`snapshot`/`restore`; flush on batch). **Step 5:** `ctest -R "LibraryManifest|LibraryIntegration"` → pass; full suite green.
- [ ] **Step 6:** `bench/library_bench.cpp` — **admits/sec** and **corr-gate cost vs pool size** (the o(N) story): the median neighbor-set size / N curve, the dedup hit-rate, segment flush throughput (alphas/sec written + MB/s), and mmap reattach time at {10⁴, 10⁵, 10⁶} alphas. Wire into `atx-engine-bench` (CONFIGURE_DEPENDS). Capture the curve into the ledger; do **not** claim O(1) — the corr gate is o(N) with a measured constant; record the recall/speedup honestly.
- [ ] **Step 7: Sprint close** (per `../docs/sprint.md`): fill the ledger (per-unit rows, commits table, the throughput + recall numbers, "What S4 proves / baton"); lift residuals to the ROADMAP backlog — **(a) mmap-RAII + writable/growable mapping → atx-core L3** (shipped on `atx::tsdb::Mapping`), **(b) fixed-seed stable hash → atx-core** (shipped on the S3 canonical hash), **(c) LSH / signed-projection → atx-core** (shipped engine-local), **(d) unbounded pairwise-complete 2-var covariance accumulator → atx-core L6** (deferred; accelerator wraps exact corr), **(e) segment compaction / merge** (the LSM merge tier — not needed at v2 scale, recorded); flip `p1/ROADMAP.md` S4 row `⏳ → ✅ <sha>` + bump `Last reviewed`; mark `sprint-4-library-management.md` `Status: ✅ closed`; create `sprint-4.md` user reference (the `library::` public API + the append-only/mmap/dedup/incremental-corr/PIT-lifecycle/reproducible-snapshot guarantees). **Step 8:** Commit close (explicit pathspecs; `git show HEAD --stat`).

---

## §6 — Exit criteria · invariants · dependencies · NOT-in-scope · baton

**Exit criteria (from the spec, made concrete):**
- The store round-trips ≥ a large fixture of alphas to disk and back, **mmap zero-copy** read, **byte-identical** to the in-memory P4 `AlphaStore` for the overlapping fields (pnl/pos/metrics/id), NaN bit-patterns preserved (`LibraryStore.FlushedSegmentRoundTripsByteIdenticalToAlphaStore`; `LibraryIntegration.RoundTripsLargeFixtureZeroCopy`).
- Dedup index **rejects** a structurally-equivalent re-submission (non-vacuous) and **admits** a genuinely-new one (`LibraryDedup.*`; `LibraryIntegration.DedupRejectsEquivalentAdmitsNew`).
- Online corr-to-pool gate **matches** the exact O(N²) recompute within a **documented recall bound**, at a fraction of the cost (measured speedup + miss rate in the ledger); the gate is **incremental in N** (`LibraryCorrIndex.MatchesExactMaxCorrWithinRecallBound` + `NeighborSetMuchSmallerThanPool`; `LibraryIntegration.IncrementalGateMatchesExactGate`).
- Lifecycle transitions are **PIT and irreversible-in-time** (no retroactive relabel; proven) (`LibraryLifecycle.TransitionsArePitIrreversible`; `LibraryIntegration.LifecycleIsPitThroughFacade`).
- A library snapshot **rebuilds byte-identically** from its manifest (`LibraryManifest.RebuildIsByteIdentical`; `LibraryIntegration.SnapshotReplaysByteIdentical`).
- `/W4 /permissive- /WX` **+ /fp:precise** clean; a `*_test.cpp` per unit; full suite stays green.

**Invariants proven (L1–L8):** append-only/immutable/content-addressed (L1); mmap zero-copy + validate-before-expose (L2); library-wide canonical-hash dedup, hash-equal ⇒ bit-identical eval (L3); incremental corr-to-pool, recall-bounded vs exact (L4); PIT lifecycle, irreversible-in-time (L5); versioned byte-identical rebuild (L6); determinism end-to-end (L7); no survivorship on disk, NaN verbatim (L8). Differential correctness: the corr accelerator is bounded against the exact `pairwise_complete_corr`; the disk round-trip is bit-checked against the in-memory store.

**Dependencies:** Upstream **S3** (the alpha firehose + the **canonical hash** = the dedup key §0.4 + the **provenance/lineage** stored on disk §0.3); **P4** (closed `f2d22f4`) — `combine::{AlphaStore,AlphaId,AlphaRecord,AlphaMetrics,compute_metrics,AlphaGate,GateConfig,GateVerdict,pairwise_complete_corr}` (the seed scaled + the gate semantics preserved); **S1** (closed `2158a17`) — `eval::deflated_sharpe` (admission, if the facade applies the S1 bar); **atx-tsdb** (already linked PUBLIC) — `Mapping::map_file_ro`, `crc32`, `tag8`, the segment framing (§0.1/§0.2); **atx-core** — `db::{Database,Statement,Transaction,BlobStream}` (the mutable indices, a **compiled TU** §0.8), `simd::dot`, `random::Xoshiro256pp::normal`, `container::{HashMap,HashSet}`, `Result`/`Status`. **Pattern-B requests (§2.1):** mmap-RAII+writable-mapping → atx-core L3; fixed-seed stable hash → atx-core; LSH/SRP → atx-core; unbounded pairwise-complete 2-var covariance → atx-core L6. **No P4/S3 source edits** — S4 is purely additive (`library/` + tests).

**Explicitly NOT in this sprint** (spec + ROADMAP anti-roadmap): **no combination** (S5/P4/S7 *read* the library; S4 *stores* it — the facade exposes the `AlphaStore`-mirroring read API S5/S7 consume); **no decay *detection*** (S7 — S4 provides the lifecycle *states* + the PIT journal; S7 *drives* the transitions via `mark`); **no distributed / sharded-across-machines store** (anti-roadmap #4 — single-box, mmap, one disk; segment compaction/merge recorded as a residual, not built); **no alternative-data fields** (anti-roadmap #3); **no learned signals / ML** (S5); **no new DSL operators or factory search logic** (S3); **no live broker / UI** (p2).

**Baton → next:** S4 gives the factory a permanent, deduplicated, auditable home and the **online corr-to-pool** that makes gating scale — the substrate **S5** reads features/constituents from (via the `library::Library` read API + `get(id).source`/`.metrics`/provenance), and **S7** operates as a book on (the **lifecycle state machine** is exactly the spine S7's decay-monitor demotes through and S7's dead-alpha→risk-factor extraction recycles from — `Dead → Recycled` is wired and tested here). The **content-addressed manifest** is the reproducibility anchor every later experiment cites. The append-only-segment + mmap-read + sqlite-index + PIT-journal pattern is the template S7's book-level reporting + capital-allocation artifacts reuse.

---

## §7 — References (open-source web research)

**WorldQuant / library management at scale (the sprint thesis)**
- Tulchinsky, I. (ed.) *Finding Alphas: A Quantitative Approach to Building Trading Strategies.* Wiley (2nd ed. 2019) — §datafield/library management, the correlation gate as the operational core, the alpha lifecycle. (WQ BRAIN: 19→10⁷ alphas; the library *is* the product.)
- Kakushadze, Z. & Yu, W. *How to Combine a Billion Alphas.* arXiv:1603.05937 — O(N) combination at 10⁹ scale; why the library/correlation structure (not per-alpha cleverness) is the constraint. https://arxiv.org/abs/1603.05937
- Kakushadze, Z. & Yu, W. *Dead Alphas as Risk Factors.* arXiv:1709.06641 — flatlined alphas → risk factors (the `Dead → Recycled` lifecycle edge + the S7 baton). https://arxiv.org/abs/1709.06641
- Kakushadze, Z. *101 Formulaic Alphas.* arXiv:1601.00991 — **mean pairwise correlation 15.9%** across 101 alphas: the diversification structure the corr-neighbor index must preserve at scale. https://arxiv.org/abs/1601.00991
- atx-engine internal: `research/worldquant-systems-deep-dive.md` §2 (datafield/library management), §5 (correlation gate), §6/§10 (combination at 10⁸ scale, alpha lifecycle, dead-alpha recycling); `research/renaissance-technologies-systems-deep-dive.md` (the one-unified-library discipline + OOS rigor).

**Locality-sensitive hashing / similarity search (the incremental corr index)**
- Charikar, M. *Similarity Estimation Techniques from Rounding Algorithms.* STOC (2002) — **SimHash / signed random projections**: `Pr[sign(⟨u,r⟩)=sign(⟨v,r⟩)] = 1 − θ(u,v)/π`, the cosine-similarity LSH S4.3 ships. https://www.cs.princeton.edu/courses/archive/spring04/cos598B/bib/CharikarEstim.pdf
- Indyk, P. & Motwani, R. *Approximate Nearest Neighbors: Towards Removing the Curse of Dimensionality.* STOC (1998) — the LSH framework (banding, multi-probe, recall/cost trade-off). https://www.cs.princeton.edu/courses/archive/spr05/cos598E/bib/p604-indyk.pdf
- Datar, M. et al. *Locality-Sensitive Hashing Scheme Based on p-Stable Distributions.* SoCG (2004) — random-projection LSH for L2/cosine (the multi-probe extension). https://www.cs.princeton.edu/courses/archive/spring05/cos598E/bib/p253-datar.pdf
- Slaney, M. & Casey, M. *Locality-Sensitive Hashing for Finding Nearest Neighbors.* IEEE Signal Processing Magazine (2008) — the practitioner's recall-vs-probes tuning guide. https://ieeexplore.ieee.org/document/4472264

**On-disk / append-only / content-addressed storage (the store + manifest)**
- O'Neil, P. et al. *The Log-Structured Merge-Tree (LSM-Tree).* Acta Informatica 33 (1996) — the memtable→immutable-segment append-only pattern S4.1/S4.2 follow. https://www.cs.umb.edu/~poneil/lsmtree.pdf
- SQLite: *Appropriate Uses For SQLite* + *SQLite As An Application File Format.* https://www.sqlite.org/whentouse.html · https://www.sqlite.org/appfileformat.html — the rationale for sqlite-as-the-mutable-index-store (the wrapper the codebase ships).
- Git internals — the content-addressed object model / Merkle DAG (the manifest `version_id` content-address). https://git-scm.com/book/en/v2/Git-Internals-Git-Objects
- Apache Arrow / Parquet — columnar memory + the file footer/metadata + crc framing idioms (available in atx-core as the export path; the dense-column layout reference). https://arrow.apache.org/docs/format/Columnar.html
- atx-engine internal: `atx-tsdb/include/atx/tsdb/{segment,segment_reader,builder,mapping,checksum}.hpp` — the as-built segment framing (magic/version/offsets/seal/crc) + the read-only mmap RAII this sprint reuses.

**Anti-overfitting / reproducibility (the admission + version discipline — reused from S1)**
- López de Prado, M. *Advances in Financial Machine Learning.* Wiley (2018) — Ch. 7 purged+embargoed CPCV (the OOS firewall an admitted alpha cleared); the reproducible-research discipline a cited library version enforces.
- Bailey, D. & López de Prado, M. *The Deflated Sharpe Ratio.* J. Portfolio Management (2014) — the deflation an admitted alpha cleared before it reaches the library. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551

---

## §8 — Self-review (against the spec)

- **Spec coverage:** S4.0 (marker + ledger)→S4-0 (+ the §0.8 sqlite build check); S4.1 (disk-backed append-only mmap columnar store scaling P4 `AlphaStore`; survivorship)→S4-1; S4.2 (canonical-hash dedup index, library-wide, O(1) avg)→S4-2; S4.3 (correlation-neighbor index + online/streaming corr-to-pool, incremental, pairwise-complete preserved, exact-vs-approx differential)→S4-3; S4.4 (lifecycle state machine candidate→…→recycled, PIT transitions)→S4-4; S4.5 (versioned reproducible library builds, byte-identical replay + close)→S4-5. Every spec unit + every exit criterion + every "invariant this sprint must prove" maps to a task and a named test. ✅
- **User asks satisfied:** "follow the same sprint building style" — §0 as-built reconciliation, §1 research rules+citations, §2 file structure + Pattern-B decisions, §3 gates + handoff block, §4 algorithms/pseudocode + data structures, §5 per-unit TDD with GoogleTest, §6 exit/invariants/deps/baton, §7 OSS references, §8 self-review — mirrors sprint-2/sprint-3 exactly. ✅ Data structures (`SegmentHeader`/`AlphaDirEntry`/`Provenance`, `LibraryStore`, `DedupIndex`, `CorrNeighborIndex`, `LifecycleState`/`LifecycleJournal`, `LibraryManifest`, `Library`) ✅; algorithms/pseudocode (§4.1–§4.7: segment framing + record (de)serialize, memtable→sealed-segment flush + mmap-ro read dispatch, sqlite dedup, SimHash neighbor index + incremental corr, PIT append-only journal, content-addressed manifest + rebuild, the facade admit path) ✅; "we already have a sqlite wrapper that could be made use here" — **explicitly used** for the mutable indices (dedup, lifecycle journal, segment catalog), §0.8/§4.3/§4.5 ✅; OSS references (§7: SimHash/LSH, LSM-tree, sqlite-as-file-format, Git content-addressing, Arrow/Parquet, WQ/RenTech, dead-alpha) ✅; "ties everything together" — consumes S3 (canonical hash + provenance), P4 (`AlphaStore`/`AlphaGate`/`pairwise_complete_corr`), atx-tsdb (mmap+crc+framing), atx-core db (sqlite) ✅.
- **As-built fixes applied (the recon's value):** no atx-core mmap/stable-hash → reuse `atx::tsdb::Mapping`+`crc32` (engine already links it) (§0.1); tsdb format is bar-specific+build-once → clone the framing, build the record layout (§0.2); `AlphaStore` has no serialization + no serializable provenance → S4 defines the on-disk superset record (§0.3); no stable hash in core → the dedup key is the S3 canonical hash (§0.4); no streaming/pairwise-complete corr primitive → SimHash accelerator around exact `pairwise_complete_corr` (§0.5); no LSH anywhere → engine-local SRP, Pattern-B lift recorded (§0.6); no wall-clock time axis → PIT keyed to an as-of period (§0.7); sqlite is a compiled TU + per-thread → build smoke-check + serial-owner rule (§0.8). ✅
- **Reproducibility/determinism rigor:** the load-bearing acceptances are the byte-identical disk round-trip (incl. NaN bit-patterns, L2/L8), the two-builds-equal segment crc (L7), and the byte-identical manifest rebuild (L6) — all named tests. ✅
- **Scale rigor:** the corr-gate exit is **non-vacuous** — `MatchesExactMaxCorrWithinRecallBound` (recall ≥ documented) + `NeighborSetMuchSmallerThanPool` (o(N) speedup) + the facade `IncrementalGateMatchesExactGate` differential against `AlphaGate` over the whole pool; the accelerator never silently replaces the exact corr — it bounds *which members* are checked, and the corr over them is exact (`pairwise_complete_corr`). The honest framing (recall, not exactness) is documented, not hidden. ✅
- **Type consistency:** `AlphaId{u32}` (P4) extended to a global append index; `SegmentHeader{magic,format_version,…,base_alpha_id,content_hash,off_*}`, `AlphaDirEntry{alpha_id,canon_hash,lifecycle_at_seal,AlphaMetrics,prov_off,prov_len}`, `Provenance{expr_source,parent_hashes,mutation_op,seed}`, `LibraryStore::{stage,flush,n_alphas,pnl,positions,get}`, `DedupIndex::{contains,find,insert}`, `CorrNeighborIndex::{add,neighbors,signature}` + `online_corr_to_pool(pnl,store,index)`, `LifecycleJournal::{transition,state_as_of}` over `LifecycleState`, `LibraryManifest{version_id,master_seeds,entries}` + `snapshot`/`rebuild_equals`, `Library::{open,admit,mark,state_as_of,snapshot,restore}` — consistent across §2/§4/§5. Reused symbols match the as-built signatures verified in recon (`Mapping::map_file_ro`, `tsdb::crc32`, `tag8`, `db::Database::{open,exec,prepare_cached}` + `Transaction`, `AlphaStore::{insert,pnl,positions,get,n_alphas}`, `AlphaMetrics` 7×f64 + alpha-major `a*T+t` / `(a*T+period)*N+j` layouts, `pairwise_complete_corr`, `AlphaGate::admit` + `GateConfig`, `simd::dot`, `Xoshiro256pp::normal`, `HashMap`/`HashSet` w/ identity hasher, `factory::canonical_hash`). ✅
