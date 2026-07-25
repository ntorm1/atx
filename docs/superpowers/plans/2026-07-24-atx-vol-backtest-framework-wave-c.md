# Backtest Framework — Wave C: `backtest_driver` spine + example-driver migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close **L11** by extracting the part of the driver spine that is *genuinely* shared across the five example drivers — the timed engine call, the `tearsheet()` fold, and the `EngineRunStats` capture — into a new library seam `atx/vol/backtest_driver.{hpp,cpp}` (`run_timed(...) -> RunOutcome{BacktestResult, TearSheet, EngineRunStats}`), migrate all five drivers onto it with **provably byte-identical output**, and port mag7's renderer artifacts to RunArchive on the **write side only** (the five CSVs stay byte-identical; the Python read-side cutover is out of scope this wave).

**Architecture:** Wave C deliberately implements **less** than design spec §4.3 proposes, because the code does not support the rest — see [§ L11 reality check](#l11-reality-check-the-grounding-table) below, which is this plan's first deliverable. The `BacktestJob` god-struct (`ClockSource` / `StrategyFactory` / `RunConfigOverlay` / `OutputProfile` / `MetaKv` slots) is **rejected on evidence**: the five drivers have four mutually-incompatible output shapes, three clock sources of which two are one-off, and three of five carry no `MetaKv` at all. What IS common to 5/5 is exactly `Clock → engine → BacktestResult → tearsheet → emit → exit`, and the library already owns four of those six nodes. The duplication that is real and removable is the ~8-line *timed-call + error-ladder + tearsheet* glue (5 copies) and the `EngineRunStats` capture (1 copy, which the other four gain for free). Reference the design spec for the module map — do **not** restate it: [`docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md`](../specs/2026-07-21-atx-vol-backtest-framework-design.md) §4.3 (the spine), §6 (I7 output byte-stability), §8 (sequencing), §9 (testing); grounding review [`docs/superpowers/specs/2026-07-21-atx-vol-backtest-review.md`](../specs/2026-07-21-atx-vol-backtest-review.md) **L11** (and note the review's own caveat that the L11 headline is overstated).

**Tech Stack:** C++20 (MSVC, Release preset `build-rel`, AVX2), `atx::vol` library; reuses `run_backtest` (`backtest.hpp`), `run_dispersion_backtest` (`dispersion_backtest.hpp`), `tearsheet` (`tearsheet.hpp`), `EngineRunStats` + the five emitters (`run_report.hpp`), `write_backtest_tsv`/`write_backtest_pnl_tsv` (`tearsheet.hpp`), RunArchive from Wave A (`run_archive.hpp`, `run_archive_schema.hpp`); CMake; gtest target `atx-vol-tests`. **No Python.**

**Base commit:** `587ee97` (Wave B closed: `1157a03` post-review round → `97ad356` closeout → `587ee97` projected-var golden).

---

## Global Constraints

- Work directly on local `main`, in place. Explicit-path `git add` only — **never** `git add -A/-u/.` (the tree carries unrelated uncommitted work: surface-db, sha256, `atx-db/`, `atx-kb/`, the unverified Python test restructure).
- **ONE build at a time.** Release preset only: `cmake --build C:\atx\build-rel --target <tgt>`. Shared deps at `C:\atx-cache\deps`. `parquet.dll` needs `C:\atx\build-rel\bin` on PATH to run examples/tests.
- **Full gtest MUST run from `C:\atx\build-rel` as CWD** — a stale repo-root `artifact-cache/` causes false failures otherwise.
- Do NOT modify golden fixtures (`atx-vol/python/tests/data/runarchive/wave_a_fixture.atxrun` sha256 `71ea9632…29f7424`; `dispersion_paired.atxrun`). Do NOT touch `C:\atx-data` run dirs (controller-only). Never read `C:\atx\.env`.
- **RunArchive schema is FROZEN**: `ra_schema_hash() == 0xdcce47781ac8390d`, `kRaMinor == 0`. A section/column change means a new golden fixture + a `kRaMinor` bump + a regenerated `_schema.py` (which is Python, and therefore impossible this wave) — **STOP and escalate to the controller** rather than doing it silently. T7 is scoped precisely so that no registry edit is needed; its one library addition is a new *encoder function*, not a new section.
- **Python is OUT OF SCOPE for this wave.** No pytest task exists and none may be added. `atx-vol/tests/mag7_dispersion_report_test.py` (`SERIES_HEADER`, `:52-57`) and `atx-vol/tools/mag7_dispersion_report.py` are **READ-ONLY constraints** whose pinned header forbids reordering the mag7 CSV — never edit them.
- **Known pre-existing RED tests — do NOT chase or "fix"** (zero file/include intersection with Wave C): `BoundaryHoist.PriceBitIdenticalToPrechange`, `SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/Latency`, and `…/Balanced`. A green Wave C = all-green **modulo these three**.
- On-disk structs are an ABI: `static_assert` on `sizeof` AND `offsetof`. Host little-endian LP64 only. All text `\n`; `%.17g` for round-trip doubles, `%.10g` for headline metric strings (the two disciplines `run_report.hpp:18-25` pins).
- Windows/PowerShell: `$ErrorActionPreference='Continue'` (native stderr wraps in ErrorRecords); use `git commit -F <file>` or a bash heredoc for multi-line messages (here-strings mangle).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **Wave B's lesson, restated as a rule for this wave.** Wave B's task-level tests were all green and the branch-level review still found a defect (the M1 trim moved the abort one line downstream, because every task test stopped at the new seam and none drove the *production* path). Wave C's analogue would be: five drivers whose *library-level* bit-identity tests pass while the drivers' actual emitted bytes move. The countermeasure is structural, not diligence-based: **T2 captures every driver's real artifact hashes BEFORE any code changes, and every migration task's GREEN step re-runs the actual binary and requires the same hash.** No task may claim green on a library test alone.

---

## L11 reality check (the grounding table)

L11 claims "the strategy-agnostic 9-stage spine is copy-pasted across 5 of 6 drivers". **That is false as stated, and the review already flags the headline as overstated.** Read at the code, stage by stage (`✅` = present in a form that could be shared; `⚠️` = present but structurally different; `❌` = absent):

| # | Stage | `mag7_dispersion_backtest` (306) | `spy_dispersion_pnl` (554) | `spy_strangle_backtest` (635) | `dispersion_backtest` (209) | `strategy_examples` (246) | Shared |
|---|---|---|---|---|---|---|---|
| 1 | `Args` struct + `parse_args` + `nv()` | ✅ `:82-149` | ✅ `:240-317` | ⚠️ inline loop in `main`, no struct, no `usage()` `:273-314` | ❌ `int main()` | ❌ `int main()` | **2/5** |
| 1b | `split`/`join` CSV-list helpers | ✅ `split_csv_list`/`join_csv_list` `:42-69` | ✅ `split(s,delim)`/`join` `:62-89` | ❌ | ❌ | ❌ | **2/5** |
| 1c | `fmt_num` (`%.10g`) | ✅ `:72-76` | ✅ `:56-60` | ⚠️ `os.precision(10)` on a stream `:489-490` | ❌ | ❌ | **2/5** |
| 2 | Clock source | SurfaceDb whole (`Clock::from_surface_db` `:166`) | SurfaceDb **windowed** (`windowed_clock` `:172-199`, re-derives via `from_manifest`) | synthetic-build **OR** `read_manifest_file` `:329-389` | synthetic-build `:118-136` | synthetic-build ×2 `:131-140`, `:182-193` | **0/5 identical** |
| 2b | synthetic-corpus trio (`make_surface`/`write_archive`/`make_manifest`) | ❌ | ❌ | ⚠️ variant (`make_spy_surface`, 1-symbol `write_archive`, seeded mt19937_64 path) | ✅ `:46-114` | ✅ `:45-113` (byte-identical to `dispersion_backtest`'s modulo `&`-placement) | **3/5** — but see note below |
| 3 | Strategy construct | `make_dispersion_strangle_spec` → `DeclarativeStrategy` | `make_dispersion_strangle_spec` → `DeclarativeStrategy` | hand-built `StrategySpec` (`make_strangle_spec`) → `DeclarativeStrategy` | **no `IStrategy` at all** — `DispersionUniverse` + `DispersionBacktestConfig` | hand-built `StrategySpec` ×2 | **0/5 identical** |
| 4 | RunConfig overlay | cache + `ExcludeAndReport` + threads + optional frictions | **same four** | tier flag + adaptive-confirm triple + preload loop + `SnapshotCache(3)`; no frictions, no `unpriced` | via `config.run` default only | **default `RunConfig{}`** | **2/5** |
| 5 | Timed engine run | ✅ `run_backtest` timed `:201-203` | ✅ `run_backtest` timed `:459-461` | ✅ `run_backtest` timed `:457-459` | ✅ **`run_dispersion_backtest`** timed `:155-159` | ⚠️ `run_backtest` **UNTIMED** `:161`, `:222` | **5/5 engine, 4/5 timed** |
| 6 | `tearsheet()` fold | ✅ `:209` | ✅ `:467` | ✅ `:467` | ✅ `:165` | ✅ ×2 `:166`, `:227` | **5/5** |
| 7 | `EngineRunStats` capture | ✅ `:212-215` — **the only one** | ❌ (inline `wall_ms`/`steps_per_s`/`peak_lots`) | ❌ (reads `SnapshotCacheStats` directly `:468-469`) | ❌ | ❌ | **1/5** |
| 8 | Output emit | 5 CSVs via `run_report.hpp` + a byte copy | **1** meta-TSV via `write_backtest_pnl_tsv` (40 meta keys, metrics INLINED) | plain `write_backtest_tsv` **+** a hand-rolled 17-column CSV at `precision(10)` with a 30-line `# k=v` prelude `:484-536` | plain `write_backtest_tsv` | plain `write_backtest_tsv` ×2 | **0/5 identical; 4 distinct shapes** |
| 9 | Console summary | 1 `printf` | 1 `printf` | ~10 `printf` + head/tail table + quote-store confirm + reload-latency probe | 2 `printf` + signal loop + `ATX_VOL_PROFILE`/`COUNTERS` blocks | `print_headline` ×2 | **5/5 in name only** |
| 10 | Exit-code convention | 2 / 1 / 0 | 2 / 1 / 0 | 2 / 1 / 0 | 1 / 0 | 1 / 0 | **3/5** |

**Verdict.** The genuinely shared core is **stages 5+6+7** — and only that. Stages 8, 9 are "shared" only in the sense that all five write *something* and print *something*; their content is four unrelated writers and five unrelated summaries. Stages 1–4 are 0/5 to 2/5. So:

- **What Wave C extracts:** stages 5+6+7 as one function. Real removal: 5 copies of `t0 = steady_clock::now(); auto res = engine(...); t1 = now(); if (!res) { fprintf; return 1; } tearsheet(*res);` plus 1 copy of `EngineRunStats` assembly. Small in lines, but it is the *whole* of what 5/5 drivers actually share, and it is the piece with a real gain attached (four drivers get `EngineRunStats` for free, and `strategy_examples` gets timing it never had).
- **What Wave C explicitly does NOT extract, with reasons** (recorded here so a later wave does not re-litigate it):
  - **`Args`/`parse_args`/`split`/`join`/`fmt_num`** — 2/5 drivers, and those two (`mag7`, `spy_dispersion_pnl`) are genuine near-twins whose flag *sets* are still disjoint (11 vs 16 flags). ~40 lines of example-local text with zero economic content. A shared `examples/driver_cli.hpp` would be defensible hygiene; it would also widen the diff across two byte-gated drivers for no library benefit. **Out.**
  - **The synthetic-corpus trio** — 3/5 drivers, yes, but `rg` finds `make_surface`/`make_manifest`/`write_archive` in **28 files** (`tests/backtest_test.cpp` 57 hits, `tests/strategy_test.cpp` 68, `bench/*`, `python/tests/*`, …). Deduplicating it is a repo-wide fixture-hygiene sweep with its own gate, not a `backtest_driver` concern, and putting eSSVI archive *generators* in the shipping library is the wrong home. **Out.**
  - **An `OutputProfile` slot** — with four output shapes and five drivers, the "abstraction" is a four-arm switch over four existing writers. It adds a dispatch layer without removing a line. Design §6/I7 itself requires keeping both the mag7 five-file convention *and* the `spy_dispersion_pnl` inlined-meta convention, i.e. it requires the divergence the slot pretends to unify. **Out.**
  - **A `ClockSource` slot** — 5 drivers, 5 different sources, 2 of them one-off (`windowed_clock`, the seeded-RNG corpus). **Out.**

**Where design spec §4.3 is wrong about the code** (call this out in the task briefs so implementers do not try to satisfy it):

1. **"The engine slot abstracts `run_backtest` vs `run_dispersion_backtest` vs the tradeable manual evaluator behind `→ BacktestResult`."** The third arm does not exist. `atx-vol/examples/spy_strangle_tradeable.cpp` contains **no** `run_backtest`, **no** `BacktestResult`, **no** `tearsheet` (verified by grep over the whole file). It emits three *parallel comparison series* (INTERPOLATED / TRADEABLE / BRIDGE) over date-**pairs** and two gap decompositions; there is no single P&L track to return. It is also not one of L11's five drivers. Making it produce a `BacktestResult` would be inventing a new artifact, not migrating an existing one. **Wave C has two engine slots, not three.**
2. **"Deletes the spine duplicated across the six example drivers."** There is no sixth driver with the spine. The sixth is `spy_dispersion_backtest.cpp`'s `run_surface_backtest_command` (`:875-940`), which *is* a `run_dispersion_backtest` call site — but it is post-Wave-B thin CLI, has no tearsheet fold, no `EngineRunStats`, and writes `surface_backtest.tsv` inside a RunArchive-bearing run dir. Migrating it would touch the byte-gated dispersion CLI for one saved line. **Left alone; noted as a future `run_timed` consumer.**
3. **`run_backtest_job(job) -> {BacktestResult, TearSheet, EngineRunStats}`** — the **return type is exactly right** and is what T1 implements verbatim as `RunOutcome`. It is only the `BacktestJob` *input* that the code does not support.
4. **"`run_report.hpp` is the already-existing back half."** True and load-bearing: `write_backtest_series_csv`, `write_metrics_csv`, `strategy_metrics`, `result_summary_metrics`, `EngineRunStats`, `engine_metrics`, `write_surface_db_stats_csv` all exist and only `mag7` consumes them. But note the consequence §4.3 misses: **`EngineRunStats` lives in `run_report.hpp`, which includes `surface_db.hpp`.** `backtest_driver.hpp` must therefore include `run_report.hpp`; do **not** relocate `EngineRunStats` (it would break `mag7_dispersion_backtest.cpp:31` and `run_report_test.cpp`).

**The wave's real risk is not the abstraction — it is that none of the five drivers has an output-byte regression anchor today.** Every "gate test" for these drivers (`mag7_dispersion_backtest_test.cpp` 5 tests, `spy_dispersion_pnl_test.cpp` 8, `spy_strangle_backtest_test.cpp` 4, `tearsheet_test.cpp::WorkedExampleA/B`) **re-implements the driver flow in the test and asserts properties** (40-delta resolution, cohort ramps, vega-flatness, attribution closure, thread determinism). Not one reads a file the driver binary wrote. Hence T2.

---

## File Structure

**New library module** (register `src/backtest_driver.cpp` in `atx-vol/CMakeLists.txt` in the source list, immediately after `src/dispersion_backtest.cpp`):
- `atx-vol/include/atx/vol/backtest_driver.hpp` — `RunOutcome` + the two `run_timed` overloads. Includes `backtest.hpp`, `tearsheet.hpp`, `run_report.hpp` (for `EngineRunStats`), `dispersion_backtest.hpp`.
- `atx-vol/src/backtest_driver.cpp` — implementations (thin: time, forward, fold, capture).

**New test** (register in `atx-vol/tests/CMakeLists.txt`'s `add_executable(atx-vol-tests ...)`, beside `run_report_test.cpp`):
- `atx-vol/tests/backtest_driver_test.cpp` — synthetic-corpus fixture in the `tearsheet_test.cpp` shape; bit-identity, tearsheet-equality and stats-capture gates for both overloads.

**Modified — library (additive only):**
- `atx-vol/include/atx/vol/run_archive.hpp`, `atx-vol/src/run_archive.cpp` — (T7) add `encode_meta_kv_section(...)`. **No registry, no struct, no `_schema.py`, no `kRaMinor` change.**
- `atx-vol/tests/run_archive_test.cpp` — (T7) the new encoder's round-trip + a `schema_hash`-unchanged assertion.

**Modified — drivers (one per task, byte-gated):**
- `atx-vol/examples/dispersion_backtest.cpp`, `atx-vol/examples/strategy_examples.cpp` — T3.
- `atx-vol/examples/spy_strangle_backtest.cpp` — T4.
- `atx-vol/examples/spy_dispersion_pnl.cpp` — T5.
- `atx-vol/examples/mag7_dispersion_backtest.cpp` — T6 (spine), T7 (RunArchive write side).

**Modified — test support:**
- `atx-vol/tests/mag7_dispersion_backtest_test.cpp` — T2 only: add `DISABLED_PersistFixtureDbForDriverGoldens` (a fixture-emitting helper, never run by the suite).

**Deliberately NOT touched:** `atx-vol/src/run_report.cpp` (the `nav`-hoisted CSV order stays — see T7), `atx-vol/include/atx/vol/run_report.hpp`'s documented column list, `atx-vol/tests/run_report_test.cpp`'s `kPinnedHeader`, all Python (`mag7_dispersion_report_test.py`, `spy_dispersion_pnl_report_test.py`, `tools/*.py`, `runarchive.py`, `_schema.py`), `run_archive_schema.hpp`, the committed archive fixtures, `spy_strangle_tradeable.cpp`, `spy_dispersion_backtest.cpp` (Wave B's thin CLI), `dispersion_workflow`'s SPY hardcode (Wave D/L12), the `StepObserver` hook (Wave D/L10), perf P1–P7 (Wave E).

---

## Byte-stability protocol (read before T2; every migration task uses it)

Each driver's artifacts fall into three classes. The plan names them per driver in T2; every later task compares against the T2 hex.

- **Pure-economics artifacts → whole-file sha256 golden.** No wall-clock, no file sizes, no sampled counters.
- **Mixed artifacts → filtered golden.** Strip the named wall-clock/telemetry lines, then hash. This is the pattern the controller already validated in Wave B for `projected_var.tsv` field 7 (`projections_per_second` moved 31800.9 → 32822.3 across an immediate rerun while the economics were exact).
- **Not gated.** stdout/stderr (nothing consumes it; every driver already prints a wall-clock number, so it was never stable) and `populate_stats.csv` (a byte copy of a db input, absent from a synthetic db).

Commands (PowerShell, run with `$ErrorActionPreference='Continue'`; `$env:PATH = "C:\atx\build-rel\bin;$env:PATH"` first):

```powershell
# whole-file golden
(Get-FileHash -Algorithm SHA256 <file>).Hash.Substring(0,16)

# filtered golden (drop wall-clock / telemetry lines, keep byte order otherwise)
(Get-Content <file> | Select-String -NotMatch -Pattern '<regex>') -join "`n" |
  ForEach-Object { (Get-FileHash -Algorithm SHA256 -InputStream ([IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes($_)))).Hash.Substring(0,16) }
```

**Determinism must be PROVEN, not assumed.** T2 runs every driver **twice** and requires the two hashes to agree before recording a golden. Anything that does not reproduce across the immediate rerun is added to that artifact's filter list, and the filter is recorded in the plan/ledger with the observed drifting values — never silently widened later.

---

## Task 1: `backtest_driver` spine — `RunOutcome` + `run_timed` (the 5/5-shared stages 5+6+7)

**Files:**
- Create: `atx-vol/include/atx/vol/backtest_driver.hpp`, `atx-vol/src/backtest_driver.cpp`
- Modify: `atx-vol/CMakeLists.txt` (register the `.cpp`)
- Create + register: `atx-vol/tests/backtest_driver_test.cpp` (`atx-vol/tests/CMakeLists.txt`)

**Interfaces (Produces):**

```cpp
// atx/vol/backtest_driver.hpp
struct RunOutcome {
  BacktestResult result;   // exactly what the engine returned — never post-processed
  TearSheet      sheet;    // tearsheet(result)
  EngineRunStats stats;    // wall_clock_ms over the engine call ONLY; n_steps = result.size();
                           // cache = cfg.snapshot_cache ? cfg.snapshot_cache->stats()
                           //                            : SnapshotCacheStats{}
};

// Engine slot A — the IStrategy engine (mag7, spy_dispersion_pnl,
// spy_strangle_backtest, strategy_examples).
[[nodiscard]] Result<RunOutcome> run_timed(const Clock &clock, IStrategy &strat,
                                           const RunConfig &cfg = {});

// Engine slot B — the composed surface-only dispersion driver
// (dispersion_backtest; a future consumer is spy_dispersion_backtest's
// run_surface_backtest_command). Reads cfg.run.snapshot_cache for stats.
[[nodiscard]] Result<RunOutcome> run_timed(const Clock &clock, DispersionUniverse universe,
                                           const DispersionBacktestConfig &cfg = {});
```

**Contract the implementation MUST honor (this is what makes the byte gates possible):**
- `result` is the engine's return value **moved**, with no transformation of any kind. `run_timed` is `{ t0; ATX_TRY(r, engine(...)); t1; return {std::move(r), tearsheet(r), stats}; }` — nothing else.
- The timed interval brackets **only** the engine call, matching what all four timing drivers measure today (`mag7:201-203`, `pnl:459-461`, `strangle:457-459`, `dispersion_backtest:155-159`). It does **not** include the tearsheet fold. Widening it would change `engine_metrics.csv`'s `wall_clock_ms` semantics (Wave B's T9-3 minor is exactly this class of silent telemetry drift — do not repeat it).
- A null `cfg.snapshot_cache` yields `SnapshotCacheStats{}`, mirroring `spy_strangle_backtest.cpp:468-469`'s existing ternary. Two of the five drivers run with no shared cache; this is the path that must not crash.
- On engine `Err`, `run_timed` propagates the error **verbatim** — each driver keeps its own `fprintf` message and exit code, so no console text moves.
- `EngineRunStats` stays in `run_report.hpp`. `backtest_driver.hpp` includes it. Do NOT relocate the struct.

**Notes:** `SnapshotCacheStats` has no `operator==` (`backtest.hpp:160-174`, 8 `uint64_t` fields) — the test compares fields individually. Fixture: the local synthetic-corpus trio in the `tearsheet_test.cpp`/`backtest_driver_test.cpp` style; do NOT try to share it with the examples (see the L11 table's note).

- [ ] **Step 1: Write the failing test** — `atx-vol/tests/backtest_driver_test.cpp`:
  - `RunTimed_ResultIsBitIdenticalToRunBacktest`: build a 12-date synthetic 1-symbol corpus + the `StrategySpec` from `tearsheet_test.cpp::WorkedExampleA` (3m 25Δ put, `EveryStep`+`HoldToExpiry`, daily `DeltaToZero`); run `run_backtest(clock, s1, cfg)` and `run_timed(clock, s2, cfg)` with `cfg.price.n_threads = 1`; assert `date[i]` string-equal and **all 25 double columns** bit-equal via `std::memcmp`-style `bits_equal` for every row (not a 10-column subset — `expect_result_bit_identical` in the existing tests covers only 10, and the archive/CSV writers emit all 25).
  - `RunTimed_SheetEqualsTearsheetOfResult`: every `TearSheet` field bit-equal to `tearsheet(outcome.result)`.
  - `RunTimed_StatsCaptureStepsAndCache`: `stats.n_steps == result.size()`; `stats.wall_clock_ms > 0.0`; with a shared `SnapshotCache`, all 8 `stats.cache` fields equal `cfg.snapshot_cache->stats()`.
  - `RunTimed_NullCacheYieldsZeroedStats`: `RunConfig{}` (null cache) → `run_timed` succeeds and all 8 `stats.cache` fields are 0.
  - `RunTimedDispersion_ResultIsBitIdenticalToRunDispersionBacktest`: the 3-symbol index+2-name corpus from `examples/dispersion_backtest.cpp:123-145` (`IDX`/`NM0`/`NM1`, weights 0.6/0.4, `min_names=2`, `record_diagnostics=true`); `run_dispersion_backtest(clock, u, config)` vs `run_timed(clock, u, config)` — all 25 columns bit-equal **and** `result.signals` name-for-name, value-for-value bit-equal (this overload is the only one that produces signals).
- [ ] **Step 2: Run test to verify it fails** — `cmake --build C:\atx\build-rel --target atx-vol-tests` → expected FAIL: `atx/vol/backtest_driver.hpp` not found.
- [ ] **Step 3: Implement** the header + `.cpp` per the contract above; register `src/backtest_driver.cpp` in `atx-vol/CMakeLists.txt` and the test in `atx-vol/tests/CMakeLists.txt`.
- [ ] **Step 4: Run test to verify it passes** — `cmake --build C:\atx\build-rel --target atx-vol-tests` then `C:\atx\build-rel\bin\atx-vol-tests.exe --gtest_filter=BacktestDriver.*:RunTimed*` → PASS (5/5).
- [ ] **Step 5: Commit** — `git add atx-vol/include/atx/vol/backtest_driver.hpp atx-vol/src/backtest_driver.cpp atx-vol/CMakeLists.txt atx-vol/tests/backtest_driver_test.cpp atx-vol/tests/CMakeLists.txt`

---

## Task 2: Capture every driver's pre-migration byte golden (MUST land before any driver edit)

**Files:**
- Modify: `atx-vol/tests/mag7_dispersion_backtest_test.cpp` (add the fixture-db emitter)
- Modify: this plan (record the hexes in the T2 table below) + `.superpowers/sdd/backtest-wave-c/progress.md`

**Why this task exists and why it is second:** no driver in Wave C's scope has an output-byte anchor. All 19 existing "gate" tests re-implement the flow and assert properties (see the L11 reality check). Without T2, every migration task's green is a library-level green — the exact shape of the Wave B miss.

**Interfaces (Produces):**
- `TEST(Mag7DispersionBacktest, DISABLED_PersistFixtureDbForDriverGoldens)` — builds the existing deterministic 12-partition / 8-symbol fixture db via the file's own `build_fixture_db` logic at a path read from the `ATX_MAG7_FIXTURE_DB` environment variable (skip with `GTEST_SKIP()` if unset), and **does not delete it**. `DISABLED_` prefix means the suite never runs it; the controller/implementer runs it explicitly with `--gtest_also_run_disabled_tests --gtest_filter=Mag7DispersionBacktest.DISABLED_PersistFixtureDbForDriverGoldens`. The db is deterministic in content (fixed `base_ts = 1'700'000'000'000'000'000`, fixed `kBaseSpot`/`kVolBump`, 12 dates `2026-03-01..12`, uids 1..8) but its per-file `file_size`/`created_ts_ns` are not reproducible across rebuilds — so it is built ONCE and reused unchanged for the whole wave, and `db_stats.csv` is a same-db golden only.

**Fixture roots** (all under the scratchpad; never `C:\atx-data`):
- `$FX = C:\Users\natha\AppData\Local\Temp\claude\c--atx\b8ae4870-03de-493c-ad84-2006e8f7409e\scratchpad\wave-c`
- `$FX\mag7_db` (the persisted fixture db), `$FX\pre\<driver>`, `$FX\post\<driver>` (artifact dirs).

**Per-driver artifact + filter table** (fill the hex columns in Step 3):

| Driver (target) | Invocation | Artifact | Class | Filter regex | sha256[0..16) |
|---|---|---|---|---|---|
| `atxvol_dispersion_backtest` | *(no args)* | `%TEMP%\atx-dispersion-backtest\dispersion.tsv` | whole-file | — | `87DA84887A2793AE` |
| `atxvol_strategy_examples` | *(no args)* | `%TEMP%\atx-strategy-examples\exampleA\example_a.tsv` | whole-file | — | `59A8C0174510C8D8` |
| `atxvol_strategy_examples` | *(no args)* | `%TEMP%\atx-strategy-examples\exampleB\example_b.tsv` | whole-file | — | `5647023F4B98FEC8` |
| `spy_strangle_backtest` | *(no args → seeded synthetic corpus)* | `%TEMP%\atx-spy-strangle-backtest\spy_short_strangle.tsv` | whole-file (`write_backtest_tsv` has no meta) | — | `57A351D477E84F10` |
| `spy_strangle_backtest` | *(no args)* | `%TEMP%\atx-spy-strangle-backtest\spy_short_strangle.csv` | filtered | `^# (wall_clock_ms|steps_per_s)=` — **narrower than this plan proposed**: three runs showed `# snapshot_preload_ms` and all four `# pricing_*` sampled-telemetry lines are constant (`0`/`0`/`0`/`0`, `sample_period=64`) on the no-args golden path, so they stay INSIDE the gate | `1B632185037D31B5` |
| `mag7_dispersion_backtest` | `--db $FX\mag7_db --out $FX\pre\mag7 --threads 1` | `series.csv` | whole-file | — | `128DBD4E99118D36` |
| `mag7_dispersion_backtest` | *(as above)* | `strategy_metrics.csv` | whole-file | — | `D49500348A9E5B3C` |
| `mag7_dispersion_backtest` | *(as above)* | `engine_metrics.csv` | filtered | `^(wall_clock_ms|steps_per_s),` | `7A56EA26F3EC5395` |
| `mag7_dispersion_backtest` | *(as above)* | `db_stats.csv` | whole-file, **same-db only**; NOT a wave gate (controller decision 3) | — | `6916983A49E258C5` |
| `spy_dispersion_pnl` | `--db $FX\mag7_db --names AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA --index SPY --min-names 4 --out $FX\pre\pnl --threads 1` | `pnl_track.tsv` | filtered | `^# (wall_clock_ms|steps_per_s)=` | `CC90B900A7116CC3` |

**Hashes above are CONFIRMED (T2, HEAD `e996f2c` + the test-file change): three independent runs
(two back-to-back, a third from a fully wiped `%TEMP%\atx-*` + `$FX\pre`) produced identical values
for all ten. Hashing method: whole-file = `Get-FileHash -Algorithm SHA256 <file>`, first 16 hex.
Filtered = drop lines matching the regex, `-join "`n"`, sha256 of the UTF-8 bytes, first 16 hex
(the protocol above). The full derivation — invocations, filters, drifting values — is duplicated
in `.superpowers/sdd/backtest-wave-c/progress.md` § `T2 goldens`, which is the durable copy
(`$FX` is scratchpad and may be cleaned).**

Notes on the table: `--threads 1` is used on the two db drivers to remove any doubt about thread-count effects, though `Mag7DispersionBacktest.DeterminismAcrossThreads` and `SpyDispersionPnl.Determinism_TwoRunAndThreads` already pin bit-identity across 1 vs 4. `spy_dispersion_pnl` is invoked with `--names` (not `--universe`) so no external universe fixture is needed; the fixture db's 7 names + SPY satisfy `--min-names 4`. `populate_stats.csv` will be absent (a synthetic db has none) — that absence is itself part of the golden and must still be absent post-migration (T2 confirmed: mag7's out dir holds exactly `series.csv`, `strategy_metrics.csv`, `engine_metrics.csv`, `db_stats.csv`).

- [x] **Step 1: Write the failing check** — run `C:\atx\build-rel\bin\atx-vol-tests.exe --gtest_also_run_disabled_tests --gtest_filter=Mag7DispersionBacktest.DISABLED_PersistFixtureDbForDriverGoldens` → expected FAIL/`0 tests ran`: the test does not exist. Also confirm `C:\atx\build-rel\bin` currently contains **only** `atxvol_spy_dispersion_backtest.exe` and `databento_spy_dispersion_definitions.exe`, i.e. none of the five Wave C drivers is built yet. *(Done: `Running 0 tests`, `filter … did not match any test`; `bin` held only `atx-vol-tests.exe`, `atxvol_spy_dispersion_backtest.exe`, `databento_spy_dispersion_definitions.exe`.)*
- [x] **Step 2: Implement the fixture emitter** — add `DISABLED_PersistFixtureDbForDriverGoldens` to `atx-vol/tests/mag7_dispersion_backtest_test.cpp`, reusing `build_fixture_db`'s body but writing to `ATX_MAG7_FIXTURE_DB` and skipping the trailing `fs::remove_all`. Build `atx-vol-tests`; run it with the env var set to `$FX\mag7_db`; assert the db opens (`SurfaceDb::open`) with 12 partitions and 8 symbols. *(Done, with one correction to this plan: `db->symbols()` is **empty**, not 8 — `write_partition` only refreshes provenance on symbols already in the manifest symbol table (`src/surface_db.cpp:1122-1135`) and never adds any, so a db built purely by `write_partition` has no manifest symbol table. The 8 symbols are witnessed instead by `DbPartitionInfo::surface_count == 8` on all 12 partitions, plus `Clock::from_surface_db` succeeding. `std::getenv` trips `/WX -Wdeprecated-declarations`; read via the `_dupenv_s` pattern from `spy_fit_corpus_test.cpp:37-51`.)*
- [x] **Step 3: Build the five drivers and capture goldens** — ONE build invocation, all five targets:
  `cmake --build C:\atx\build-rel --target atxvol_dispersion_backtest atxvol_strategy_examples spy_strangle_backtest mag7_dispersion_backtest spy_dispersion_pnl`.
  Then, with `$env:PATH = "C:\atx\build-rel\bin;$env:PATH"`, run **each driver twice**, hash per the protocol, and require the two hashes to agree. Record every hex in the table above. For any artifact whose two runs disagree, identify the drifting line(s), extend that artifact's filter regex, re-verify agreement, and record **both** the regex and the observed drifting values. *(Done. All five drivers exit 0. 7/10 artifacts were byte-stable unfiltered on the first double run; the only drift anywhere was `wall_clock_ms` + its derived `steps_per_s` in three artifacts — no other telemetry moved, so no filter had to be widened beyond the two keys.)*
- [x] **Step 4: Verify the goldens are a real gate** — confirm each hex is reproducible a third time from a clean artifact dir (delete `%TEMP%\atx-*` and `$FX\pre` first, so the goldens do not depend on leftover state), and that `atx-vol-tests.exe` still reports the same pass/skip/fail counts as before this task (the new test is `DISABLED_`, so the suite total must be unchanged modulo one added disabled test). *(Done: all 216 `%TEMP%\atx-*` dirs and `$FX\pre` wiped, third run reproduced all ten hexes exactly. Each filtered artifact differs from the pre-migration bytes on **exactly two** lines, both matching its filter — verified line-by-line with PowerShell, not `diff`. Suite: 2019 ran / 1973 passed / 43 skipped / 3 failed (the three documented reds) / **7** disabled — identical to the pre-task baseline modulo the one added `DISABLED_`.)*
- [x] **Step 5: Commit** — `git add atx-vol/tests/mag7_dispersion_backtest_test.cpp docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-c.md .superpowers/sdd/backtest-wave-c/progress.md`

**Reference bytes for T3–T6.** T2 also left an unfiltered copy of all ten artifacts at
`$FX\golden-ref\` (same run that produced the table). Migration tasks use it for the
"unfiltered file differs on exactly the filtered lines" audit. It is scratchpad — if it is gone,
re-derive it by rebuilding the drivers **at `e996f2c`** and re-running the invocations above.

**Tooling warning for later tasks.** In this environment the Bash tool's `grep` and `diff` are
proxied and returned WRONG answers during T2 (`diff` reported "Files are identical" for two files
whose sha256 differed; piped `grep -c` reported 0 matches on a stream that had 2). **Do every
hash/line comparison in PowerShell** (`Get-FileHash`, `Get-Content` + an index loop or
`Compare-Object`). A migration task that "verified" its gate through Bash `diff` has verified
nothing.

---

## Task 3: Migrate the two zero-arg synthetic drivers (`dispersion_backtest`, `strategy_examples`)

**Files:**
- Modify: `atx-vol/examples/dispersion_backtest.cpp`, `atx-vol/examples/strategy_examples.cpp`

**Interfaces (Consumes):** T1's `run_timed` (both overloads).

**The changes, precisely:**
- `dispersion_backtest.cpp:155-165` — replace the `steady_clock` bracket + `run_dispersion_backtest` + `tearsheet` trio with `run_timed(*clock, u, config)`; `engine_ms` becomes `outcome.stats.wall_clock_ms`; `r` becomes `outcome.result`, `t` becomes `outcome.sheet`. The `ATX_VOL_PROFILE`/`ATX_VOL_COUNTERS` `reset()` calls (`:149-154`) stay **before** the `run_timed` call and the `snapshot()` blocks (`:177-199`) stay after — the profile regions must still bracket exactly the engine work.
- `strategy_examples.cpp:161-166` and `:222-227` — replace `run_backtest(*clock, strat)` + `tearsheet(*res)` with `run_timed(*clock, strat)` (default `RunConfig{}` → the null-cache path T1 pins). `print_headline` takes `outcome.sheet`; `write_backtest_tsv(outcome.result, tsv)`; `res->size()` becomes `outcome.result.size()`. This driver **gains** timing and `EngineRunStats` it never had — optionally print them, but note the stdout consequence below.

**Byte gate (the whole point):** `dispersion.tsv`, `example_a.tsv`, `example_b.tsv` whole-file hashes **identical to T2**. Stdout is not gated (`dispersion_backtest` already printed a wall-clock `engine_ms`, so its stdout was never stable; `strategy_examples`' `print_headline` prints only tearsheet values, which do not move — so if the implementer adds a timing line, the tearsheet lines above it must be unchanged).

- [ ] **Step 1: Write the failing test** — extend `backtest_driver_test.cpp` with `RunTimedDispersion_SignalsSurviveTheSeam`: over the `IDX`/`NM0`/`NM1` corpus with `record_diagnostics = true`, assert `run_timed`'s outcome carries the **same non-empty signal set** (names in order, every value bit-equal) as `run_dispersion_backtest` — because `dispersion_backtest.cpp:171-176` prints `r.signals` first/last and `write_backtest_tsv` appends one column per signal, so a dropped or reordered signal would move `dispersion.tsv`'s bytes. RED if `run_timed` were to copy rather than move / reorder signals; author it before touching the drivers so the seam property is pinned independently of the driver diff.
- [ ] **Step 2: Run to verify it fails or is a genuine lock** — `cmake --build C:\atx\build-rel --target atx-vol-tests`; run `--gtest_filter=*RunTimedDispersion*`. If it passes immediately (the T1 implementation already moves faithfully), disclose that in the report as a **regression lock, not a RED** — Wave B's T8 set this precedent and its reviewer accepted it explicitly. Do not fabricate a RED.
- [ ] **Step 3: Implement** the two driver edits above. Delete the now-dead `<chrono>` include only if nothing else in the file uses it (`dispersion_backtest.cpp` will still need it if a timing print remains).
- [ ] **Step 4: Run to verify it passes** — ONE build: `cmake --build C:\atx\build-rel --target atx-vol-tests atxvol_dispersion_backtest atxvol_strategy_examples`. Then: `atx-vol-tests.exe --gtest_filter=BacktestDriver.*:RunTimed*:TearSheet.*` → PASS. Then delete `%TEMP%\atx-dispersion-backtest` and `%TEMP%\atx-strategy-examples`, run both binaries, and assert all **three** whole-file hashes equal the T2 hexes. Paste the observed hexes beside the expected ones in the task report.
- [ ] **Step 5: Commit** — `git add atx-vol/examples/dispersion_backtest.cpp atx-vol/examples/strategy_examples.cpp atx-vol/tests/backtest_driver_test.cpp`

---

## Task 4: Migrate `spy_strangle_backtest` (the most divergent driver — keep everything it owns)

**Files:**
- Modify: `atx-vol/examples/spy_strangle_backtest.cpp`

**Interfaces (Consumes):** T1's `run_timed` (`IStrategy` overload).

**What migrates (stages 5+6+7 only):** `:456-469` — the `counters::lightweight` snapshot bracket, the `steady_clock` bracket, `run_backtest`, the error ladder, `tearsheet(r)`, and the `cache_stats` ternary. After the edit:

```cpp
const counters::lightweight::Snapshot telemetry_before = counters::lightweight::snapshot();
auto outcome = run_timed(*clock, strat, run_config);
const counters::lightweight::Snapshot telemetry =
    counters::lightweight::delta(telemetry_before, counters::lightweight::snapshot());
if (!outcome) { std::fprintf(stderr, "run_backtest: %s\n", outcome.error().to_string().c_str()); return 1; }
const BacktestResult &r = outcome->result;
const TearSheet t = outcome->sheet;
const SnapshotCacheStats cache_stats = outcome->stats.cache;   // was the :468-469 ternary
const double run_ms = outcome->stats.wall_clock_ms;            // was t_run1 - t_run0 at :479
```

The `fprintf` text stays `"run_backtest: %s\n"` verbatim — the message is part of the driver's observable behavior and there is no reason to move it.

**What must NOT change** (all of it is this driver's own, none of it is spine): the `business_days`/`make_spy_surface`/`write_archive`/`make_manifest` seeded-RNG synthetic corpus (`:329-361`, seed `0x5391A11ED5EEDULL` — the whole byte golden depends on it), the real-manifest branch (`:362-389`), the five-flag inline arg loop, `make_strangle_spec` + `spec.resolution.fast_screen_cold_confirm`, the `--query-tier` mapping, the adaptive-confirm triple (`:419-430`), the preload loop and its own `preload_ms` timer (`:434-455` — this is a *separate* interval from the engine run and must stay outside `run_timed`), the hand-rolled 17-column CSV with its 30-line prelude (`:484-536`), `confirm_quote_slice_store`, the archive reload-latency probe, and the head/tail table.

**Two traps to check explicitly:**
1. The telemetry delta must still bracket **only** the engine run. `run_timed` calls `tearsheet()` *inside* itself, after the engine — so with the code above, the telemetry window now also covers the tearsheet fold. `tearsheet()` performs no pricing and touches no `counters::lightweight` counter, so the delta is unchanged — **verify this by grep** (`counters::` in `src/tearsheet.cpp`) rather than assuming, and record the finding. If it did touch them, the driver must snapshot inside a narrower scope instead.
2. `cache_stats` is read for the `# cache_*` CSV meta lines (`:498-500`) and the `[cache]` console block (`:550-557`). `run_config.snapshot_cache` is null on the default (non-adaptive, non-preload) path, which is exactly the T2 golden's path — so `outcome->stats.cache` must be all-zero there, identical to the old ternary. T1's `RunTimed_NullCacheYieldsZeroedStats` is the library-level proof; the T2 hash is the end-to-end proof.

**Byte gate:** `spy_short_strangle.tsv` whole-file hash == T2; `spy_short_strangle.csv` filtered hash == T2 (using the exact filter T2 recorded).

- [ ] **Step 1: Write the failing test** — this driver's flow has no library seam left to test that T1 does not already cover, and adding a fifth re-implementation of it would be the "copy to fix a copy" anti-pattern the Wave B triage rejected (T6-2). So the RED for this task is **the byte gate itself, run against the un-migrated binary with the migration's intended `cache_stats` source substituted**: temporarily replace `:468-469`'s ternary with `SnapshotCacheStats{}` unconditionally, rebuild, run, and confirm the `.csv` filtered hash **still matches T2** (proving the `# cache_*` lines are already all-zero on the golden path, i.e. the substitution is byte-safe). Revert the probe. Record the probe and its result — this is the same explicit-RED-probe discipline the Wave B fix round used for the I1 gate.
- [ ] **Step 2: Run to verify the probe result** — ONE build: `cmake --build C:\atx\build-rel --target spy_strangle_backtest`; run; hash; confirm; revert the probe and rebuild clean.
- [ ] **Step 3: Implement** the `run_timed` substitution exactly as shown; grep-verify trap 1; delete the now-unused `t_run0`/`t_run1` locals.
- [ ] **Step 4: Run to verify it passes** — ONE build: `cmake --build C:\atx\build-rel --target atx-vol-tests spy_strangle_backtest`. `atx-vol-tests.exe --gtest_filter=SpyStrangleBacktest.*:BacktestDriver.*:RunTimed*` → PASS (4 + 6). Delete `%TEMP%\atx-spy-strangle-backtest`, run the binary, assert both hashes == T2.
- [ ] **Step 5: Commit** — `git add atx-vol/examples/spy_strangle_backtest.cpp`

---

## Task 5: Migrate `spy_dispersion_pnl` (the inlined-metrics convention must survive)

**Files:**
- Modify: `atx-vol/examples/spy_dispersion_pnl.cpp`

**Interfaces (Consumes):** T1's `run_timed` (`IStrategy` overload).

**What migrates:** `:459-471` only —

```cpp
auto outcome = run_timed(*clock, strat, rc);
if (!outcome) { std::fprintf(stderr, "run_backtest: %s\n", outcome.error().to_string().c_str()); return 1; }
const BacktestResult &r = outcome->result;
const TearSheet ts = outcome->sheet;
const double wall_ms = outcome->stats.wall_clock_ms;
const double steps_per_s = (wall_ms > 0.0) ? 1000.0 * static_cast<double>(r.size()) / wall_ms : 0.0;
const double peak_lots = r.size() ? *std::max_element(r.n_open_lots.begin(), r.n_open_lots.end()) : 0.0;
```

`steps_per_s` and `peak_lots` stay driver-local: `steps_per_s` is derivable from `stats` but its `wall_ms > 0.0` guard is this driver's own text and it lands in the meta header (a golden line, filtered); `peak_lots` is not an `EngineRunStats` field. Do not invent new `EngineRunStats` members for them — `engine_metrics.hpp`'s key set is pinned by `run_report_test.cpp:340-341`.

**What must NOT change:** `read_universe_symbols`, `dedup_alphabet`, the `top_n` truncation, `windowed_clock`, the **entire calendar-gap audit** (`:359-429` — `expected`/`missing`/`window_narrowed`/`calendar_source`/`missing_list` and both `WARNING:` stderr lines), the 40-key `Meta` block (`:479-521`) **including its inlined tearsheet metrics** — this is design §6/I7's "spy_dispersion_pnl inlines metrics into the meta header vs mag7's separate metrics file; the driver must keep BOTH conventions, not force one" — and `write_backtest_pnl_tsv`.

**Byte gate:** `pnl_track.tsv` filtered hash (drop `^# (wall_clock_ms|steps_per_s)=`) == T2. Every other meta key, its position, and the whole series must be byte-identical — in particular the 12 tearsheet-derived meta values (`total_return` … `peak_open_lots`) come from `outcome->sheet`, so a tearsheet that differed by one ULP would show up here. That is the point.

- [ ] **Step 1: Write the failing test** — extend `backtest_driver_test.cpp` with `RunTimed_SheetFieldsAreBitEqualUnderFmtNum`: for a run over the T1 fixture, format all 12 meta-bound `TearSheet` fields with `snprintf("%.10g")` from both `outcome.sheet` and `tearsheet(outcome.result)` and assert the **strings** are equal. This pins the exact quantity `pnl_track.tsv` embeds (a `%.10g` rendering), which is strictly what the byte golden depends on, and it is not a restatement of `RunTimed_SheetEqualsTearsheetOfResult` (that one compares raw doubles; this one compares what reaches the file).
- [ ] **Step 2: Run to verify it fails or is a lock** — build `atx-vol-tests`; `--gtest_filter=*SheetFieldsAreBitEqual*`. Disclose honestly if it is a lock rather than a RED.
- [ ] **Step 3: Implement** the `run_timed` substitution; delete the `t0`/`t1` locals; leave `<chrono>` only if still needed.
- [ ] **Step 4: Run to verify it passes** — ONE build: `cmake --build C:\atx\build-rel --target atx-vol-tests spy_dispersion_pnl`. `atx-vol-tests.exe --gtest_filter=SpyDispersionPnl.*:BacktestDriver.*:RunTimed*` → PASS (8 + 7). Delete `$FX\post\pnl`, run with the T2 invocation but `--out $FX\post\pnl`, assert the filtered hash == T2. **Also assert the unfiltered file differs from T2's unfiltered bytes only on the two filtered lines** (`Compare-Object` the two files line-by-line and require exactly two differing lines, both matching the filter) — this closes the hole where a filter silently masks a real change.
- [ ] **Step 5: Commit** — `git add atx-vol/examples/spy_dispersion_pnl.cpp atx-vol/tests/backtest_driver_test.cpp`

---

## Task 6: Migrate `mag7_dispersion_backtest` onto the spine (five CSVs byte-identical)

**Files:**
- Modify: `atx-vol/examples/mag7_dispersion_backtest.cpp`

**Interfaces (Consumes):** T1's `run_timed` (`IStrategy` overload).

**What migrates:** `:201-215` — the `steady_clock` bracket, `run_backtest`, the error ladder, `tearsheet(r)`, **and the hand-built `EngineRunStats`** (this is the one driver that already assembles it, so the migration *deletes* code rather than adding capability):

```cpp
auto outcome = run_timed(*clock, strat, rc);
if (!outcome) { std::fprintf(stderr, "run_backtest: %s\n", outcome.error().to_string().c_str()); return 1; }
const BacktestResult &r = outcome->result;
const TearSheet ts = outcome->sheet;
const EngineRunStats &stats = outcome->stats;      // deletes :212-215 entirely
const double wall_ms = stats.wall_clock_ms;        // still needed by the console summary :286, :301
```

`stats.cache` now comes from the spine reading `rc.snapshot_cache->stats()` — mag7 always supplies a shared cache (`:189`), so this is the non-null path and must be value-identical to the old `rc.snapshot_cache->stats()` at `:215`.

**What must NOT change:** the 18-key `MetaKv` (`:222-241`) and its exact order — "the keys + order are the BINDING contract the Python renderer reads" (`:220-221`); the five emitter calls in their existing order; the `populate_stats.csv` conditional byte copy; the console `printf`; the exit codes.

**Byte gate:** `series.csv` whole-file == T2; `strategy_metrics.csv` whole-file == T2; `db_stats.csv` whole-file == T2 (same `$FX\mag7_db`); `engine_metrics.csv` filtered (`^(wall_clock_ms|steps_per_s),`) == T2 — and, per the T5 discipline, the unfiltered `engine_metrics.csv` must differ from T2's on **exactly those two rows** (the four `cache_*`/`n_steps` rows are deterministic and must be byte-identical).

- [ ] **Step 1: Write the failing test** — the byte gate is the test here, and it needs a RED that proves it can fail. Probe: temporarily change `stats.n_steps` to `r.size() + 1` in `backtest_driver.cpp`, rebuild `atx-vol-tests` + `mag7_dispersion_backtest`, run the driver, and confirm `engine_metrics.csv`'s filtered hash **differs** from T2 (because `n_steps` and the derived `steps_per_s` move) **and** that `atx-vol-tests --gtest_filter=*StatsCaptureStepsAndCache*` FAILS. Revert. This proves both the library test and the artifact gate are live for this driver.
- [ ] **Step 2: Run to verify the probe** — ONE build for the probe, one to revert.
- [ ] **Step 3: Implement** the substitution above; delete `:201-215`'s timer locals and the `EngineRunStats stats;` assembly.
- [ ] **Step 4: Run to verify it passes** — ONE build: `cmake --build C:\atx\build-rel --target atx-vol-tests mag7_dispersion_backtest`. `atx-vol-tests.exe --gtest_filter=Mag7DispersionBacktest.*:RunReport.*:BacktestDriver.*:RunTimed*` → PASS (5 + 6 + 7). Delete `$FX\post\mag7`, run with `--db $FX\mag7_db --out $FX\post\mag7 --threads 1`, assert all four hashes per the byte gate and the exactly-two-differing-rows check. Confirm `populate_stats.csv` is still absent.
- [ ] **Step 5: Commit** — `git add atx-vol/examples/mag7_dispersion_backtest.cpp`

---

## Task 7: mag7 → RunArchive, WRITE side only (`backtest` + `meta` sections); the five CSVs are UNCHANGED

**Files:**
- Modify: `atx-vol/include/atx/vol/run_archive.hpp`, `atx-vol/src/run_archive.cpp` (one additive encoder)
- Modify: `atx-vol/examples/mag7_dispersion_backtest.cpp`
- Modify: `atx-vol/tests/run_archive_test.cpp`

### The CSV-order question, decided

**The port does NOT change the mag7 CSV. Not one byte.** The evidence, and what would have to move if it did:

- `write_backtest_series_csv`'s order (`src/run_report.cpp:78-104`) and the RunArchive `backtest` registry order (`include/atx/vol/backtest_series_columns.hpp:40-66` / `run_archive_schema.hpp:kBacktestCols`) hold the **same 25 names in the same relative order with `nav` displaced** — canonical index 14 → CSV index 1 (i.e. CSV field 3, after `date,ts_ns,pnl_total`). Formally `csv == [pnl_total, nav] ++ (canonical \ {pnl_total, nav})`. This is the permutation a prior reviewer established in `.superpowers/sdd/backtest-wave-b/minors-triage.md` (T6-1), and it is why a naive `for (col : backtest_series_columns())` in `run_report.cpp` would rewrite the header line and every data row.
- The CSV order is pinned in **four** places, all in the `nav`-hoisted form: `run_report.hpp:48-55` (documented list, under an explicit "BINDING interface, not an implementation detail"), `run_report_test.cpp:129-134` `kPinnedHeader` (asserted verbatim at `:209-210`), `atx-vol/tests/mag7_dispersion_report_test.py:52-57` `SERIES_HEADER`, and `run_report.cpp` itself.
- `atx-vol/tools/mag7_dispersion_report.py:365` reads by column **name**, so the renderer would survive a reorder — but the two pinned-header tests would not, and **one of them is Python, which is out of scope this wave.** Therefore the read-side cutover (renderer → archive) is **not possible in Wave C**, and a write-side reorder would break a test we may not edit.
- Formatting is not an obstacle either way: both writers use `%.17g` for doubles and `%lld` for `ts_ns`; only the separator differs. So the deferred dedup remains *possible* later, via an explicit permutation — it is a schema decision, not a cleanup, exactly as the Wave B triage concluded.

**Consequence:** Wave C writes `<out>/run.atxrun` **alongside** the five CSVs, in canonical registry order, and the five CSVs remain the binding renderer contract until the wave that can touch Python ports `mag7_dispersion_report.py` + `mag7_dispersion_report_test.py`. This is a knowing, scoped deviation from design §2's "no dual-write" — recorded here because §2's hard-cutover rule presumes the Python consumers are ported in the same sprint, and the user has dropped all Python work. **Flag it to the controller in the task report; do not treat it as settled by this plan alone.**

### What fits the frozen schema, and what must be escalated

| mag7 artifact | RunArchive home | Verdict |
|---|---|---|
| `series.csv` (date, ts_ns, 25 doubles, + one column per signal) | `backtest` TimeSeries section via `encode_backtest_section("backtest", r)` | ✅ **exact fit**, zero schema change. mag7's `DeclarativeStrategy` emits no signals today, so the section is exactly the 27 registry columns. |
| the 18-key `MetaKv` + `strategy_metrics(ts)` + `result_summary_metrics(r)` + `engine_metrics(stats)` rows | `meta` ScalarKV — the registry declares it as exactly `{key: DictStr, value: DictStr}` (`run_archive_schema.hpp:81-84`), i.e. **arbitrary key/value pairs** | ✅ fits, zero schema change — but needs a new *encoder overload* (below), because `encode_meta_section` requires a `RunSpec` and mag7 has none. |
| `db_stats.csv` (`key,surface_count,file_size,created_ts_ns` per db partition) | **no such section exists** in `kRaSections` (`meta`, `backtest`, `projected_cold`, `projected_nodiv`, `reconciliation`, `trade_schedule`, `projected_schedule`, `contract_marks`, `mark_divergence`, `diagnostics`) | ❌ **ESCALATE.** A `db_stats` SubTable would be a registry change → new golden fixture + `kRaMinor` bump + regenerated `_schema.py` (Python). **Stays a CSV this wave.** Do not fold its rows into `meta` as flattened keys — that hides a table in a KV store and would have to be undone. |
| `populate_stats.csv` | none — it is a byte copy of a **db input**, not a run result; the design's partition rule keeps authored/input text out of the container | ❌ correctly out. Stays a byte copy. |

### `RunDir` is NOT usable here

`RunDir::run_identity_hash()` **requires `<dir>/run_spec.tsv`** ("run_spec.tsv is REQUIRED — a run dir without it is not a run", `run_archive.hpp:589-593`) and folds in `universe_schedule.tsv` when present. mag7's `--out` directory contains neither and is not a dispersion run dir. So mag7 calls `write_run_archive_file` directly:

```cpp
// Deterministic identity over what actually defines this run's content, so two
// runs against the same db + config produce a byte-identical run.atxrun. Mirrors
// Wave B's T7 policy: created_ts_ns is a CONTENT-DERIVED pseudo-timestamp, NOT
// the wall clock (wall-clock provenance is the out-dir file mtimes).
const std::uint64_t identity = /* fold of db->root(), db->generation(), and the
                                  resolved DispersionStrangleConfig fields */;
ATX_TRY_VOID(write_run_archive_file((fs::path(args.out) / "run.atxrun").string(),
                                    sections, static_cast<std::int64_t>(identity), identity));
```

Note the field ordering discipline from Wave B: fold a **fixed field order** and document the property as *layout-independent* (padding-free), not "order-independent" (the T1-3 minor). `identity` must be forced nonzero (0 means "unset" in the header, and `created_ts_ns == 0` would fall back to the system clock — killing determinism).

**Interfaces (Produces — additive library API, no registry touch):**
```cpp
// run_archive.hpp, beside encode_meta_section:
// `meta` ScalarKV from an already-assembled key/value list, for producers with no
// RunSpec (the mag7 driver). Same section, same two DictStr columns, same registry
// entry — only the source differs. Keys must be unique; values verbatim.
[[nodiscard]] RaSectionData
encode_meta_kv_section(std::span<const std::pair<std::string, std::string>> kv);
```
Implement `encode_meta_section` and `encode_meta_kv_section` over one shared helper so the two can never emit differently shaped `meta` sections.

**Byte gate:**
1. All four T6 CSV hashes **unchanged** (the CSVs are the contract; adding the archive must not perturb them).
2. `run.atxrun` opens (`RunArchive::open_mapped`), `validate_all()` passes, `header().schema_hash == ra_schema_hash() == 0xdcce47781ac8390d`, `kRaMinor == 0`.
3. `section("backtest").f64("nav")` equals `series.csv`'s `nav` column value-for-value (parse the CSV by header name, not by index — the whole point is that the two orders differ), and likewise for `pnl_total` and `gross_vega`; `dict_col("date")` equals the CSV `date` column.
4. `section("meta")` contains all 18 driver meta keys plus every `strategy_metrics`/`result_summary_metrics`/`engine_metrics` key, with values byte-equal to the corresponding CSV cells.
5. **Two consecutive runs against the same db + args produce a byte-identical `run.atxrun`** (`Get-FileHash` equal) — the determinism property, proven not assumed.
6. `git diff --stat` shows **no** change to `run_archive_schema.hpp`, `_schema.py`, or any committed `.atxrun` fixture; `atx-vol-tests --gtest_filter=RunArchive*:RunDir.*` still fully green including `MatchesCommittedPythonFixture`.

- [ ] **Step 1: Write the failing test** — in `atx-vol/tests/run_archive_test.cpp`: `MetaKvSectionRoundTrips` — `encode_meta_kv_section({{"a","1"},{"b","x y"},{"c",""}})` → `write_run_archive` → `open` → `section("meta")`, assert `n_rows == 3`, `dict_col("key")` and `dict_col("value")` round-trip in order including the empty value; `MetaKvSectionMatchesSpecEncoder` — a `RunSpec`-derived `encode_meta_section(spec)` and `encode_meta_kv_section(<the same pairs>)` produce **identical** `RaSectionData` shape (kind, column names, n_rows) and identical written bytes; `MetaKvSectionRejectsDuplicateKeys` → `Err(AlreadyExists)` or the documented error; and `static_assert`/`EXPECT_EQ(ra_schema_hash(), 0xdcce47781ac8390dull)` so the additive API can never move the hash.
- [ ] **Step 2: Run to verify it fails** — `cmake --build C:\atx\build-rel --target atx-vol-tests` → FAIL: `encode_meta_kv_section` undefined.
- [ ] **Step 3: Implement** the encoder (shared helper with `encode_meta_section`), then the mag7 write-side: assemble `sections = { encode_backtest_section("backtest", r), encode_meta_kv_section(all_meta_and_metrics) }` and publish via `write_run_archive_file` with the deterministic identity. Add a header comment in `mag7_dispersion_backtest.cpp` recording (a) that the five CSVs remain the binding renderer contract, (b) that `run.atxrun` is written in **canonical** column order which is a `nav`-displaced permutation of the CSV order, and (c) that `db_stats`/`populate_stats` have no archive home without a `kRaMinor` bump.
- [ ] **Step 4: Run to verify it passes** — ONE build: `cmake --build C:\atx\build-rel --target atx-vol-tests mag7_dispersion_backtest`. `atx-vol-tests.exe --gtest_filter=RunArchive*:RunDir.*:Mag7DispersionBacktest.*:BacktestDriver.*` → PASS. Run the driver twice into `$FX\post\mag7-archive`; verify all six byte-gate items above.
- [ ] **Step 5: Commit** — `git add atx-vol/include/atx/vol/run_archive.hpp atx-vol/src/run_archive.cpp atx-vol/tests/run_archive_test.cpp atx-vol/examples/mag7_dispersion_backtest.cpp`

---

## Task 8: Wave-C integration gate (controller)

**Files:** Modify this plan (check boxes); update `.superpowers/sdd/backtest-wave-c/progress.md` (controller-owned ledger).

- [ ] **Step 1: Full build.** `cmake --build C:\atx\build-rel` (all targets, including all five drivers, `atx-vol-tests`, `atxvol_spy_dispersion_backtest`). Zero new warnings under `/WX`.
- [ ] **Step 2: Full gtest from `C:\atx\build-rel` CWD.** `cd C:\atx\build-rel; .\bin\atx-vol-tests.exe` → all green **modulo exactly the three documented pre-existing reds** (`BoundaryHoist.PriceBitIdenticalToPrechange`, `SurfaceV2Qualification…/Latency`, `…/Balanced`). Record the ran/passed/skipped/failed counts and compare the total against Wave B's post-fix-round baseline plus the tasks' new tests. Any fourth failure blocks the gate.
- [ ] **Step 3: Re-verify every T2 byte golden at HEAD, from clean artifact dirs.** Delete `%TEMP%\atx-dispersion-backtest`, `%TEMP%\atx-strategy-examples`, `%TEMP%\atx-spy-strangle-backtest`, `$FX\post`. Run all five drivers with the T2 invocations. Assert **all ten** hashes in the T2 table. For each filtered artifact, additionally assert the unfiltered file differs from the T2 unfiltered capture on **exactly** the filtered lines. Report a hash table with expected-vs-observed side by side.
- [ ] **Step 4: Freeze verification.** `ra_schema_hash() == 0xdcce47781ac8390d`; `kRaMinor == 0`; `git diff 587ee97..HEAD --stat` contains **no** `run_archive_schema.hpp`, no `_schema.py`, no `*.atxrun`, no `*.py` at all, and no `src/run_report.cpp`/`include/atx/vol/run_report.hpp`/`tests/run_report_test.cpp` **content** change beyond what T7 declares (T7 touches none of the three — confirm the diff agrees). Confirm the committed fixture sha256 `71ea9632…29f7424` is unchanged.
- [ ] **Step 5: Regression check on the Wave B route.** `atxvol_spy_dispersion_backtest` still builds and its Wave B goldens are untouched by construction (Wave C edits none of its files) — confirm by `git diff --name-only 587ee97..HEAD` showing `examples/spy_dispersion_backtest.cpp` absent. The 135-session parity run is **not** required this wave (no economics seam was touched); state that explicitly rather than silently skipping it.
- [ ] **Step 6: Commit** the ledger + checked plan.

---

## Batching (for parallel Opus subagents)

The ONE-build-slot rule serializes every C++ task; there is no Python lane this wave, so parallelism is limited to read-only reviewers.

- **Dependency order:** T1 → T2 → {T3, T4, T5, T6} → T7 → T8.
  - T2 depends on T1 only for the build slot, not for code — but it MUST precede T3–T7, since it defines their gates.
  - T3/T4/T5/T6 are **file-disjoint** from each other (five different example `.cpp`s) and each depends only on T1's seam + T2's goldens, so they may be *authored* concurrently. Their builds still serialize.
  - T7 depends on T6 (same file) and on T2's mag7 goldens.
- **Proposed schedule:** B1 = T1 → B2 = T2 → B3 = T3 → B4 = T4 → B5 = T5 → B6 = T6 → B7 = T7 → T8 (controller).
- **Reviewers** (fresh Opus per batch, read-only, no builds) shadow each task via `rtk proxy git diff BASE..HEAD > .superpowers/sdd/backtest-wave-c/review-<task>-<sha>.diff`.
- **Standing instruction for every reviewer:** the task's library-level test passing is **not** sufficient evidence. Verify the implementer actually re-ran the binary and that the reported hashes match T2's table character-for-character. Wave B shipped a defect whose task tests were green because they stopped at the new seam; the analogue here is a green `run_timed` test beside a driver whose bytes moved.

---

## Self-Review

**Spec coverage (Wave C slice of design §4.3/§8, review L11):**
- `backtest_driver` spine → T1, scoped to the 5/5-shared stages and **explicitly narrower than §4.3**, with the grounding table and the four `BacktestJob` slots rejected on evidence. ✓
- Engine slot convergence on `→ BacktestResult` → T1's two overloads (`IStrategy`, `run_dispersion_backtest`). The third arm §4.3 names (the tradeable manual evaluator) **does not exist in the code** and is documented as a spec error, not silently skipped. ✓
- Migrate the example drivers → T3 (2), T4, T5, T6 (5 of 5, one commit each). ✓
- Port mag7's renderer to RunArchive → T7, **write side only**, with the CSV-order decision stated and evidenced, the two escalation items named (`db_stats` needs a section → `kRaMinor` bump; `populate_stats` is an input), and the §2 no-dual-write deviation flagged to the controller rather than assumed. ✓
- Byte-stability gates → T2 captures ten goldens across five drivers *before* any edit; every migration task's GREEN step re-runs the real binary; filters are proven by a double run and audited by an exactly-N-differing-lines check. ✓
- Controller gate → T8 (full build, full gtest from `build-rel` CWD, all ten goldens re-verified, freeze verification). ✓
- **Correctly OUT of Wave C:** all Python (user directive); `run_report.cpp`'s CSV dedup (a schema decision — Wave B triage T6-1 DEFER stands); the synthetic-corpus-trio dedup (28 files, a separate hygiene sweep); an `examples/driver_cli.hpp` (2/5 drivers, zero economic content); `StepObserver` (Wave D/L10); de-SPY `dispersion_workflow` (Wave D/L12); perf P1–P7 (Wave E); `spy_strangle_tradeable`; `spy_dispersion_backtest`'s `run_surface_backtest_command`. Noted, not gaps.

**Freeze integrity:** T7 is the only task near the format. It adds one encoder **function** over an existing registry section whose columns are already `{key: DictStr, value: DictStr}`; it edits no registry array, no on-disk struct, no `_schema.py`, no fixture. `ra_schema_hash()` is asserted unchanged in T7 Step 1 and again in T8 Step 4. No `kRaMinor` bump anywhere.

**Anti-tautology audit** (the Wave B failure mode, applied to this plan's own tests):
- T1's bit-identity tests compare `run_timed` against the *independently called* `run_backtest`/`run_dispersion_backtest` — two distinct call paths, not `f(x) == f(x)`.
- T3/T5's seam tests are disclosed up front as possibly being **locks rather than REDs**, with instructions to say so rather than manufacture a failure.
- T4 and T6 use **explicit temporary probes** (substitute `SnapshotCacheStats{}`; corrupt `n_steps`) to prove the gate can fail, then revert — the same discipline the Wave B fix round used to prove its I1 test was real.
- The five byte goldens are captured from the **pre-migration binaries**, so they cannot be contaminated by the new code.

**Placeholder scan:** every seam names a real line range in a real file (`mag7:201-215,222-241`; `pnl:459-471,479-521`; `strangle:456-469,484-536`; `dispersion_backtest:155-165`; `strategy_examples:161-166,222-227`; `run_report.cpp:78-104`; `run_archive_schema.hpp:81-84,216-227`; `run_archive.hpp:589-593`) and a real existing primitive (`run_backtest`, `run_dispersion_backtest`, `tearsheet`, `EngineRunStats`, `encode_backtest_section`, `write_run_archive_file`). Every test name is concrete. Every command is runnable. The only blanks are the T2 hash cells, which by construction cannot be pre-filled.

**Type consistency:** `RunOutcome`/`run_timed`/`EngineRunStats`/`RaSectionData`/`encode_meta_kv_section` used consistently T1 → T7. `EngineRunStats` is consumed from `run_report.hpp` and never relocated.

## Open questions for the controller (decide before dispatch)

1. **T7's dual-write deviation.** Design §2 forbids dual-write; it also assumes the Python consumers are ported in the same sprint, which the Python drop makes impossible. Confirm that mag7 writes `run.atxrun` **beside** the five unchanged CSVs, with the read-side cutover deferred — or defer T7's archive write entirely and let Wave C be spine-only.
2. **`db_stats` section.** Confirm it stays a CSV this wave (a new SubTable = `kRaMinor` bump + new golden + regenerated `_schema.py`), and that a later wave owns it.
3. **mag7 fixture db lifetime.** T2 persists a synthetic 12-partition db under the scratchpad and every later task hashes `db_stats.csv` against it. Confirm the scratchpad path survives the wave (if it is cleaned between sessions, `db_stats.csv` drops out of the gate and only the three economics CSVs remain gated for mag7).
4. **`spy_strangle_backtest`'s sampled telemetry.** Its CSV embeds `# pricing_cache_hit_rate` etc. from `counters::lightweight`. If T2's double run shows these drifting, they join the filter and the driver keeps no telemetry golden. Confirm that is acceptable, or ask for a `--no-telemetry`-style deterministic mode instead (a driver change, and therefore a scope addition).
5. **`strategy_examples` stdout.** It currently prints no timing. Migration makes timing available for free. Confirm whether to print it (a visible behavior change on a driver nothing parses) or to discard `stats` there and keep stdout identical. Default in this plan: implementer's choice, with the tearsheet lines required unchanged.

---

## Controller decisions (pre-dispatch, 2026-07-24)

Answering the five open questions above. These OVERRIDE the plan body where they conflict.

1. **T7 is DROPPED. Wave C is spine-only.** (User decision.) mag7's RunArchive
   write is not worth landing while its reader is Python, which is out of scope for
   this sprint: writing `run.atxrun` beside the five unchanged CSVs is precisely the
   dual-write that design §2 forbids and that the sprint's own hard-cutover decision
   ruled out. A half-migrated consumer is worse than an unmigrated one. The archive
   port moves to a later wave that can port the reader in the same breath.
   Wave C is therefore **7 tasks**: T1–T6 plus the controller gate.
2. **`db_stats` stays a CSV.** Confirmed. A `db_stats` SubTable is a registry change
   (`kRaMinor` bump + new golden fixture + regenerated `_schema.py`), and `_schema.py`
   is Python. Moot for this wave now T7 is dropped; recorded so a later wave does not
   rediscover it. Do NOT flatten its rows into `meta` — that hides a table in a KV
   store and would have to be undone.
3. **mag7 fixture db lifetime.** The scratchpad is session-scoped and may be cleaned
   between sessions, so it cannot be the durable home of a golden. With T7 dropped,
   `db_stats.csv` is no longer gated at all; mag7's gate is its three economics CSVs.
   Capture the fixture db under the scratchpad as the plan says, but record the
   captured hashes in the wave ledger so the evidence survives the directory.
4. **`spy_strangle_backtest`'s sampled telemetry.** Filter the non-deterministic
   telemetry lines out of the golden and gate the economics only. Do NOT add a
   `--no-telemetry` mode: that is a driver behaviour change to make a test easier,
   and this wave's whole premise is that driver behaviour does not change. The
   implementer must list exactly which `#`-comment lines were filtered and show the
   two runs agreeing on everything else.
5. **`strategy_examples` stdout stays byte-identical.** Discard `stats` there. Timing
   being available for free is not a reason to change a driver's visible output during
   a migration whose contract is that nothing observable changes. If the timing is
   wanted, it is a separate one-line commit afterwards, not part of the migration.

**Standing constraint for every task in this wave:** the migration's contract is that
each driver's observable output is bit-identical afterwards. T2 captures the goldens
BEFORE any edit, and no migration task may be marked done without its driver's hash
comparison shown. A driver whose golden cannot be captured deterministically is
reported to the controller, not migrated on faith.
