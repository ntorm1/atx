# atx-vol SurfaceDb Backfill Throughput Sprint

**Date:** 2026-07-11 (evidence-verified)

**Status:** implementation-ready plan. Every claim about the current code carries a
`file:line`. Every performance number below was measured on this box tonight
(i7-1260P, 12 cores / 16 logical, Windows 11, clang-cl 18.1.8) against the real
MAG7+SPY OPRA hive (`C:/atx/data/mag7_ytd/opra`, 2026-07-06..2026-07-10, 40 boards).

**Scope:** the archive-backfill path only — `mag7_surfdb_populate` →
`load_opra_daterange` → `populate_surface_db` → `fit_board` → `PricerFitter::fit`
(pinned ConvexDense, Fast preset) → `SurfaceDb::write_partition`. Everything
downstream of the fitted surface is covered by the 2026-07-09 throughput sprint.

**Primary goal:** a full-YTD (129-session, 1032-board) MAG7+SPY backfill in **under
one minute** on this box, and a single-name board fit **under 100 ms** serial —
without changing the fitted surface bytes for the pinned-curve path.

---

## 1. What was measured (the headline)

The reported "1.7 s per fit" (68.6 s / 40 boards) was measured on a **Debug binary
with runtime checks**. The same run tonight, after rebuilding with the existing
`rel` preset and flipping already-existing config knobs — zero code changes:

| Configuration | Wall (40 boards) | CPU | eff. cores | vs. baseline |
|---|---|---|---|---|
| Debug (`ninja` preset, `/Od /Ob0 /RTC1`), 16 workers | 68.6 s | — | — | 1.0× |
| **Release** (`rel` preset, `/O2`), 16 workers | **11.5 s** | 82.9 s | 7.2 | **6.0×** |
| Release, 8 board-workers, inner serial (`ATX_VOL_FIT_WORKERS=1`) | 44.7 s | 84.3 s | 1.9 | 1.5× |
| Release + hft-knob symbol configs, 16 workers | **5.2 s** | 31.5 s | 6.1 | **13.2×** |

Per-board serial fit cost (Release, everything serial, 5 boards per symbol):

| Symbol | CPU / board (Fast knobs) | CPU / board (Hft knobs) | knob speedup |
|---|---|---|---|
| SPY (penny-dense) | ~6.1 s | ~2.2 s | 2.8× |
| AAPL | ~1.7 s | ~0.34 s | 5.0× |
| TSLA | ~1.5 s | — | — |

Supporting facts:

- Parquet load is **not** the bottleneck at this scale: a skip-existing rerun
  (load + skip all fits) takes **0.3 s** for 5 SPY files (~0.3 MB each).
- The written partitions are byte-size-identical across Fast and Hft knob runs
  (29.1–29.8 KB per SPY date) — the pinned ConvexDense pin survives the preset
  (the example re-pins after `symbol_config_from_preset`,
  `examples/mag7_surfdb_populate.cpp:140-142`). Byte-identity of contents is NOT
  yet proven — that is P2's acceptance gate, because the Hft preset also caps
  observations (`max_obs_per_slice=48`) which CAN move the fit.
- Run-to-run variance on this laptop is large (same serial SPY config measured
  64.5 s and 26.2 s ten minutes apart — thermal/turbo states). All benchmark
  gates in this sprint must use best-of-3 or median-of-3, and record eff-cores.

Extrapolation to full YTD (1032 boards): Debug ~30 min → Release ~5 min →
Release+knobs ~2.2 min → this sprint's structural fixes target **<1 min**.

---

## 2. Root causes, ranked

### RC1 — The default build is Debug, and nothing warns you (measured 6.0×)

`CMakePresets.json:12` sets `CMAKE_BUILD_TYPE=Debug` in the `_base` preset that
`ninja` inherits; the worktree flow (`scripts/atx-build.ps1:54` → `cmake --preset
ninja`, build dir `build/`) therefore produces `/Od /Ob0 /RTC1` binaries. The `rel`
preset (`CMakePresets.json:56-64`, build dir `build-rel/`) exists and is documented
as "canonical acceptance / benchmarks" — but `mag7_surfdb_populate` and the other
data-job examples print nothing about their build type, so a Debug data job is
silent and looks identical to a Release one.

### RC2 — Inner fit fan-out is uncontrolled: N board-workers × N prepass-workers

`fit_board` sets `PricerConfig.n_threads = 1` with the comment "each board fits
single-threaded; fan-out is ACROSS boards" (`src/corpus_board_fit.cpp:210`) — but
`n_threads` only reaches `value_chain`. The de-Am prepass inside the fit reads
`SurfaceParityInputs::fit_workers`, which defaults to 0 = auto = 16
(`include/atx/vol/surface_parity.hpp:120`, consumed at `src/curve_fit.cpp:262`),
and `VolaSession::build` never sets it. So an 8-board-worker populate runs up to
8 × 16 inner threads. Tonight's inner-serial probe shows the flip side: with the
oversubscription removed *but the per-date barrier kept* (RC3), wall got **worse**
(11.5 s → 44.7 s) — the accidental inner parallelism is currently the only thing
hiding RC3. Both must be fixed together. Note also that the one env knob,
`ATX_VOL_FIT_WORKERS`, feeds **both** levels through `atx_auto_worker_count()`
(`include/atx/vol/parallel_for.hpp:48-80`, `examples/mag7_surfdb_populate.cpp:79`),
so "boards parallel, inner serial" is currently unreachable without a code change
(the probe above cheated: env=1 for inner + explicit `--fit-workers 8` for outer).

### RC3 — Per-date barrier caps parallelism at universe size and lets SPY gate every date

`src/surface_db_populate.cpp:171-192`: `n_workers = min(cfg.n_threads, range_n)`
where `range_n` = boards in one date → an 8-symbol universe silently clamps
`--fit-workers 16` to 8, one board per worker, and the `std::jthread` join (line
191-192) is a hard barrier per date. The next date cannot start until the current
date's slowest fit — SPY, ~4× the cost of a single name (table §1) — finishes.
Measured: inner-serial 8-worker run achieves **1.9 effective cores** on a 12-core
box. 129 dates = 129 sequential barrier/fit/write cycles.

### RC4 — ~60–80% of the pinned-curve fit CPU is diagnostics the served surface never reads (measured 2.8–5.0× via knobs)

For a pinned ConvexDense fit (the entire backfill), with Fast-preset defaults
(`SymbolFitConfig{use_correction_cache=true, score_parity=true}`,
`include/atx/vol/surface_db.hpp:119-122`):

- `build_session_caches` (`src/session.cpp:341-347`) builds **two** CorrectionCaches
  ≈ 1536 + 96 ≈ **1632 Andersen-Lake boundary solves per board, fixed cost
  regardless of board size** (`src/correction.cpp:352-372`, put side not
  row-collapsed) — and the ConvexDense override path never serves from them
  (`session.hpp:401-407`, `served_cache` returns nullptr for curve overrides).
  Their only consumer is:
- `build_parity_data` / `chain_parity` (`src/curve_fit.cpp:218-224`, `:346`, gated
  on `score_parity`) — a **second full de-Am pass** over the board, purely
  diagnostic for a backfill.
- `max_borrow_pairs=12` in the forward/borrow fixed point (Fast,
  `src/session.cpp:227`) vs 1 for Hft.

The Hft knob probe (§1) bounds the recoverable CPU at 2.8× (SPY) to 5.0× (single
names) — but bundles the obs-cap accuracy change; P2 isolates the free subset.

### RC5 — Loader: serial, full-column, deep-copied, never evicted

- `load_opra_daterange` (`src/opra_batch.cpp:344-424`) reads all files one at a
  time on the calling thread, before any fit starts.
- `load_opra_cbbo_parquet` → `io::read_parquet` decodes **every column**
  (`atx-core/src/io/parquet.cpp:167-179`) though only ~8 are consumed
  (`src/opra_panel.cpp:265-286`); the projection-capable `LazyParquet` API already
  exists (`atx-core/include/atx/core/io/parquet.hpp:137-160`).
- `corpus_board_from_opra` takes `OpraPanel` **by value** and the example passes
  `*e.panel` from a `const` batch (`examples/mag7_surfdb_populate.cpp:158,175`) —
  a forced deep copy, so panels AND boards are both fully resident for the whole
  run (2× the largest structure in the program). Same pattern in
  `examples/spy_ytd_corpus.cpp:82`.
- `populate_surface_db` takes `std::span<const CorpusBoard>` — it cannot evict a
  date's boards after writing the partition; only a caller-side windowed driver can.
- Not the current bottleneck (0.3 s / 5 files) but becomes one at 1032 files ×
  ~0.1-0.3 s, and is the whole memory story for long backfills.

### RC6 — `skip_existing` pays a full archive read + CRC per already-present date

`src/surface_db_populate.cpp:136-146` calls `db.open_partition(date)` just to test
existence; `open_partition` (`src/surface_db.cpp:931-941`) reads and parses the
entire `.atxvsa` (full-file read + CRC-32C, `src/surface_archive.cpp:655-676`) and
throws the result away. The O(log P) in-memory check already exists:
`db.manifest()->find_partition(key)` (`src/surface_db.cpp:568-583`). A resumed
mostly-complete YTD backfill re-reads every existing archive for nothing.

### RC7 — Minor allocation churn on the write side

`to_priced_surface()` clones the CurveSurface (`src/session.cpp:524`), then
`with_uid` clones the entire surface **again** to stamp one scalar uid
(`src/dispersion.cpp:510-516`, called at `src/surface_db_populate.cpp:223`).
`persist_locked` re-encodes + re-CRCs + re-parses the whole manifest once per date
(`src/surface_db.cpp:763`) — O(dates²) over a backfill, harmless at 129 dates,
wrong shape at 10⁴.

---

## 3. Guardrails (what must NOT change)

1. **Fitted-surface bytes are the contract.** For P1–P4 and P6 (no math knobs
   touched), the acceptance gate is byte-identical `.atxvsa` partitions vs. the
   pre-sprint Release baseline for the same inputs and knobs. P2 (knob defaults)
   must prove byte-identity for the specific knobs it turns off
   (`use_correction_cache`, `score_parity`) and must NOT adopt the obs-cap or
   borrow-pair knobs without a separately-gated accuracy study.
2. **Determinism regardless of worker count** — already the stated contract
   (`include/atx/vol/surface_db_populate.hpp:36`); the global-queue rewrite (P3)
   keeps the existing bit-identity test pattern (`tests/curve_fit_parallel_test.cpp`)
   and adds one at the populate level: serial run vs. 16-worker run → identical
   partitions + identical stats CSV.
3. **`populate_stats.csv` schema is pinned** (run_report contract) — fields may not
   change meaning.
4. **No new dependencies.** Everything below uses existing infra
   (`parallel_for_dynamic`, `LazyParquet`, `ATX_VOL_COUNTERS`, `ATX_VOL_PROFILE`).

---

## 4. Tasks

### P0 — Ops: make Release the only way to run a data job *(zero risk, do first)*

- Add a one-line startup banner to `mag7_surfdb_populate` and
  `mag7_dispersion_backtest`: build type (`#ifdef NDEBUG`), worker resolution
  (outer/inner), and a **stderr warning when compiled without NDEBUG**:
  `"[warn] DEBUG BUILD — fits are ~6x slower; use the 'rel' preset (build-rel/)"`.
- Document in `atx-vol/README` (or the examples' header comments): data jobs run
  from `build-rel/bin/`, never `build/bin/`.
- Acceptance: banner visible in both binaries; Debug binary prints the warning.

### P1 — Pin the inner fit fan-out to the caller's intent *(fixes RC2)*

- Thread `PricerConfig.n_threads` through `make_session_inputs` into
  `SurfaceParityInputs::fit_workers` in `VolaSession::build`
  (`src/session.cpp:321-334`), so `fit_board`'s `n_threads=1`
  (`src/corpus_board_fit.cpp:210`) actually yields a serial inner fit.
- Keep `0 = auto` semantics for direct/interactive `PricerFitter` users (the
  live-fit path WANTS the parallel prepass) — only an explicit `n_threads=1`
  becomes serial.
- Acceptance: (a) existing `CurveFitParallel` bit-identity tests green;
  (b) `populate_surface_db` with N workers spawns at most N concurrent fit threads
  (assert via a counter test or `ATX_VOL_COUNTERS` thread-spawn counter);
  (c) 40-board Release benchmark does not regress vs. the 11.5 s baseline once P3
  lands (P1 alone WILL regress wall — land P1+P3 in the same benchmark gate).

### P2 — Fit-only knob profile for backfills *(fixes RC4's free subset, measured ≥2.8×)*

- Gate `build_session_caches` (`src/session.cpp:341-347`) on
  `curve.kind == Essvi || !curve_pinned` — a pinned non-eSSVI fit never serves the
  cache; building it is pure waste even when `use_correction_cache=true`.
- Change the **populate default** symbol config (`examples/mag7_surfdb_populate.cpp`
  upsert block, and `SurfaceDbPopulateConfig::fallback`) to
  `use_correction_cache=false, score_parity=false` — preset stays Fast, curve stays
  pinned ConvexDense, calendar floor stays ON. Leave `PricerFitter` defaults
  untouched for every other caller.
- Do NOT adopt `max_borrow_pairs=1` / `max_obs_per_slice=48` here (surface-moving);
  file them as a follow-up accuracy study.
- Acceptance: (a) `.atxvsa` partitions byte-identical to a Fast-knob baseline run
  for all 40 boards (this is the proof the two knobs are diagnostic-only on this
  path; if bytes differ, STOP and investigate before merging);
  (b) serial SPY CPU/board ≤ 3.5 s and AAPL ≤ 0.9 s (headroom vs. the 2.2 s / 0.34 s
  hft-bundle numbers since obs caps stay off); (c) populate_stats schema unchanged.

### P3 — Global board queue in `populate_surface_db` *(fixes RC3)*

- Replace the per-date worker block (`src/surface_db_populate.cpp:148-192`) with
  one `parallel_for_dynamic` (or explicit work-stealing loop) over the whole
  sorted `order` array. Per-date bookkeeping: an atomic pending-count per date;
  the worker that completes a date's last board runs that date's aggregation +
  single `write_partition` call (concurrent writes on distinct keys are documented
  safe, `include/atx/vol/surface_db.hpp:322-335`; same-key single-writer preserved
  by construction). Stats accumulation behind a small mutex or per-worker locals
  merged at the end (map ordering makes output order-independent).
- `skip_existing` check moves to the cheap manifest probe (see P4c) and runs during
  work-list construction, not inside the loop.
- Sizing: workers = `cfg.n_threads` (no `min(range_n)` clamp); inner fit serial
  via P1.
- Acceptance: (a) determinism gate — serial vs. 16-worker runs produce identical
  partitions and identical stats CSV; (b) 40-board Release benchmark (P1+P2+P3,
  16 workers, median-of-3) ≤ **3.5 s** wall and ≥ 9 effective cores;
  (c) `n_dates_written`/`n_ok`/`n_failed` identical to baseline.

### P4 — Loader: parallel, projected, moved, windowed *(fixes RC5 + RC6)*

- **a.** Parallelize `load_opra_daterange` over (symbol,date) cells with
  `parallel_for_dynamic` — entries land by index, no ordering dependency
  (`src/opra_batch.cpp:344-424`).
- **b.** Switch `load_opra_cbbo_parquet` to the projected read
  (`LazyParquet::scan(path).select({...}).collect()`) for the ~8 consumed columns.
- **c.** Replace the `open_partition` existence probe with
  `db.manifest()->find_partition(date) != nullptr` (or add
  `SurfaceDb::has_partition()` as a one-line wrapper) —
  `src/surface_db_populate.cpp:137`.
- **d.** Kill the deep copy: iterate the batch non-const and
  `corpus_board_from_opra(e.date, e.symbol, std::move(*e.panel))`
  (`examples/mag7_surfdb_populate.cpp:158-176`); apply the same fix to
  `examples/spy_ytd_corpus.cpp:82`.
- **e.** Windowed driver for long backfills: the example loops over date chunks
  (default ~20 sessions), load → fit → write → **drop** per chunk, so peak memory
  is O(window), not O(129 sessions). `skip_existing` makes re-chunking free.
- Acceptance: (a) YTD-scale load (1032 files) measured before/after — target ≥4×
  load speedup; (b) peak working set of a 129-session backfill < 2 GB (measure via
  `Get-Process PeakWorkingSet64` on a live process); (c) resumed backfill against a
  fully-populated db completes in seconds (manifest probe only, no archive reads).

### P5 — Fit hot path toward HFT budget *(fixes RC4 remainder + RC7; the open-ended lane)*

Ordered by expected value; each item gets its own before/after counter run:

- **a.** Instrument first: add the missing `ATX_VOL_PROFILE` timer around
  `build_session_caches` (`src/session.cpp:341-347`) and run
  `ATX_VOL_COUNTERS` builds to attribute boundary solves per stage. (The profiler
  currently under-attributes the cache build — `src/curve_fit.cpp:37-51` only
  times `fit_curve_surface`.)
- **b.** `with_uid` move overload / in-place uid stamp (`src/dispersion.cpp:510`) —
  one full surface clone per board, gone.
- **c.** Cross-date reuse for a symbol's consecutive boards: enable
  `use_deam_cache_for_fit` for backfill and/or warm-start boundary seeds from the
  previous date's fit (the boundary is nearly identical day over day). This is the
  main lever toward <100 ms/board for single names (currently ~0.34 s with knobs
  off, so ~3.4× still to find; the 2026-07-09 sprint's P2.0/P2.5 boundary-ladder
  work attacks the same solves from the other side).
- **d.** Reuse `Universe`/`OptionChain` scaffolding across a symbol's dates
  (`src/chain.cpp:14`) instead of a fresh SoA install per board.
- **e.** Batch the manifest: `write_partition` variant that defers
  `persist_locked` to one flush per populate run (or per chunk), fixing the
  O(dates²) manifest re-encode (`src/surface_db.cpp:763`).
- Acceptance: single-name serial board fit (AAPL, Release, knobs per P2)
  **< 100 ms median**; SPY < 500 ms; all fit-contract tests green; partitions
  byte-identical where no math knob changed (b, d, e must be byte-identical; c is
  seed-only and must be too — a warm START may not change the converged result
  beyond the solver's own tolerance gates, assert bit-identity and fall back to
  cold on any mismatch).

### P6 — Acceptance benchmark + regression gate

- Add `atx-vol/examples/` (or bench target) `surfdb_backfill_bench`: runs the
  40-board 5-day corpus 3× and prints median wall, CPU, eff-cores, per-symbol
  CPU/board, peak WS. Wire into `ATX_BUILD_BENCH`.
- Full-YTD one-shot on this box (operator-run): 129 sessions, 8 symbols, 16
  workers, Release, P1-P4 landed. **Gate: < 60 s wall, < 2 GB peak WS, 1032/1032
  ok, resumable, deterministic.** Record the number in the sprint file.
- CI-lite: the 5-day benchmark median goes in the ledger; any future PR touching
  `curve_fit.cpp` / `session.cpp` / `surface_db_populate.cpp` reruns it.

---

## 5. Sequencing and effort

| Order | Task | Size | Depends on | Expected effect (40-board wall) |
|---|---|---|---|---|
| 1 | P0 banner/docs | XS | — | prevents recurrence of the 6× |
| 2 | P2 knob profile + cache gate | S | — | 11.5 s → ~6 s |
| 3 | P1 inner-fit pin + P3 global queue | M (land together) | — | ~6 s → ~2.5-3.5 s |
| 4 | P4 loader (a-e) | M | P3 (for e) | YTD: load ~4× faster, mem O(window) |
| 5 | P5a instrumentation | XS | — | attribution for P5c-e |
| 6 | P5b-e hot path | M-L | P5a | single-name < 100 ms/board |
| 7 | P6 gates + YTD run | S | P1-P4 | the <60 s YTD number |

P1+P3 land as one reviewed unit with one benchmark gate — P1 alone regresses wall
(measured: 44.7 s), P3 alone amplifies oversubscription (8 dates × 16 inner
threads in flight).

## 6. Open questions / deferred

- Adopting `max_borrow_pairs=1` and `max_obs_per_slice=48` (the rest of the Hft
  knob bundle, worth ~a further 1.5-2×) needs an accuracy study against the fit
  corpus (oos_in_band / calendar-violation deltas), not just byte-diffing — the
  surface legitimately changes. Deferred to a follow-up.
- SIMD/ISA (`/arch:AVX2` via the `rel-avx2` work on main) applies to the de-Am
  solves here too; inherit whatever the 2026-07-09 sprint lands rather than
  duplicating.
- Cheby boundary interpolation across the strike ladder (2026-07-09 sprint §P2.5)
  is the deep fix for the same AL solves this sprint only gates/caches; do not
  duplicate the work here.
- The `ATX_VOL_FIT_WORKERS` env var steering both fan-out levels is a footgun even
  after P1; consider splitting (`ATX_VOL_BOARD_WORKERS`) if operators keep using it.
