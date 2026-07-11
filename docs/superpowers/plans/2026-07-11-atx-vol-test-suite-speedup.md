# atx-vol Test-Suite Speedup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the atx-vol test suite wall-clock from ~30 min (debug, serial ctest) / ~4-6 min (release) to under ~2 min debug / ~40 s release without weakening any correctness gate.

**Architecture:** Four independent levers, ordered by effort-to-impact: (1) parallelize ctest and split fast/slow labels; (2) replace runtime PDE-oracle recomputation with a checked-in golden table; (3) cache the expensive fitted-surface / corpus artifacts on disk and let consumer tests reload them; (4) compile the atx-vol numeric kernels optimized even in the Debug preset. Perf-scoreboard tests that assert sweep-count/timing relationships get env-gated out of the default run (existing `ATX_VOL_LONG_CORPUS` precedent).

**Tech Stack:** CMake 3.27+ presets, CTest, GoogleTest (`gtest_discover_tests`), clang-cl + Ninja on Windows, existing `atx::vol` `surface_archive` binary serialization, existing `build_corpus` on-disk layout.

## Measured Baseline (2026-07-11, 16-core machine)

| Configuration | Tests | Wall | Source |
|---|---|---|---|
| Debug, ctest serial (overnight run, Jul 11 00:19) | 908 | **1766.9 s (29.4 min)** | `build/Testing/Temporary/LastTest.log` |
| Debug, single process, controlled (today's binary) | 922 | **508.3 s (8 m 29 s)** | this investigation (gtest XML) |
| Release, single process, controlled | 922 | **230.5 s (3 m 50 s)** | this investigation (gtest XML) |
| Debug process-spawn cost | 922 spawns | ~0.14 s each ≈ 2+ min | measured `--gtest_filter` no-match run |

Two caveats on the overnight number: (a) it pays one process spawn + image load per test (and likely AV scanning / machine contention overnight), so its *per-test* figures run 2-6× hotter than today's controlled single-process run; (b) yesterday's boundary-kernel perf commits (`c53ece8`, `fb6e7c1`) made fit-heavy tests substantially faster (e.g. `MultinamePipeline` suite: 310 s overnight → 52.5 s today). Use today's controlled numbers as the optimization baseline; the overnight run is what the *developer experience* of `ctest --preset ninja` actually was.

Distribution is extreme Pareto in both configs: debug top 44 tests = 80 % of 508 s; release top 41 = 80 % of 230 s. ctest runs **serial** today — no `-j` anywhere (presets, scripts, tasks.json).

Debug suite totals today (top): CallGreeksAl 52.6 s, MultinamePipeline 52.5 s, ImplicitDiff 38.1 s, WarmAcrossTime 38.0 s, SigmaInterpCorpus 32.6 s, SigmaInterp 30.1 s, BoundaryHoist 21.3 s, AndersenLake 20.3 s, SpyFitCorpus 18.6 s, Corpus 17.7 s, CorpusGeneratedProperty 15.6 s.

### Root causes (from code investigation, file:line verified)

1. **Serial ctest + process-per-test.** `gtest_discover_tests` registers each of the 922 cases as its own `atx-vol-tests.exe` spawn; no preset/script passes `-j`. Pure overhead ≈ 2 min debug, and zero parallelism on a 16-core box.
2. **PDE oracle recomputed every run.** `oracle_pde_american` ([oracle_pricer_pde.cpp:28](../../atx-vol/tests/support/oracle_pricer_pde.cpp#L28)) is a pure function of hardcoded literals (2000×4000 Crank-Nicolson march ≈ 8M cell-updates; the ThetaCharm fine grid is 4×). ~150 oracle calls across 7 suites, **zero caching**. Debug cost today (controlled): `CallGreeksAl.ThetaCharm` 41.1 s (24 fine-grid marches), `ImplicitDiff.MeetsPdeGreekGates` 38.0 s (56 marches), `SigmaInterp.MeetsPdeGreekGates` 29.1 s (30 marches + ~340 AL solves), `AndersenLake.VsPdeOracle_*Grid` ~24 s (24 marches), `CallGreeksAl.MeetsPdeGreekGates` 11.5 s (25 marches), `AndersenLakeRegime.CornerGrid` (12), `CallGreeksFd.Fast_MeetsPdeGreekGates` (9). Oracle-bound suites total ≈ 155 s of the 508 s debug run.
3. **The same 14k-contract SPY board is cold-fit ~13× across the suite** (load parquet → de-Americanize every strike via Andersen-Lake → `fit_curve_surface` ConvexDense). Five tests use the *identical* config (Fast + ConvexDense + node_cap 40). `surface_archive` round-trip is already proven bit-exact by `spy_archive_roundtrip_test`, so a disk cache is a safe drop-in.
4. **MultinamePipeline rebuilds the same synthetic corpora ~13×** (~130 board fits): every test calls `make_multiname_boards` + `build_corpus` from scratch, then varies one policy knob. `GrossVegaIsUnderReportedWhenALegIsUnpriced` builds **two** corpora. Only 3 distinct corpora exist across all consumers. Suite totals 52.5 s debug / 31.5 s release today. `Corpus.Manifest_RoundTrips` does a 4-board build purely to get a populated manifest — the assertions are a pure string round-trip (its sibling `Manifest_RoundTripsEveryCurveKind` proves zero fits are needed).
5. **Benchmark-shaped tests inside the gate.** `WarmAcrossTime.SweepReduction` ([american_test.cpp:2546](../../atx-vol/tests/american_test.cpp#L2546), 13.6 s debug) and `BoundaryHoist.SeedSpike_SweepCount` ([american_test.cpp:1859](../../atx-vol/tests/american_test.cpp#L1859), 20.2 s debug) are sprint kill-gate scoreboards (1800-point sweeps, printf reports). `CurveFitParallel.SpyBoardBitIdenticalAcrossWorkers` (12.5 s debug / 9.8 s release) + `CurveFitParity.ParityOffSkipsSecondDeAmOnSpy` = 4+3 full cold-board passes whose correctness twins (`SyntheticBoardBitIdenticalAcrossWorkers`, `ParityOffMatchesParityOnSurface`) are synthetic and fast. `CorpusBuildSession.SyntheticThirteenNameThreeDateBreadthScoreboard` (3.7 s release) bundles a 13×3 scoreboard + three backtests. Precedent for relocation already exists: `bench/CMakeLists.txt:81-94` documents `DISABLED_*` → `atx-vol-reloc-bench` moves.
6. **Debug build config taxes numeric kernels 6-8×.** Debug = `/Od /Ob0 /RTC1 -MDd` → `_ITERATOR_DEBUG_LEVEL=2` checked STL iterators in every pricing loop (`build/build.ninja:149`). No optimized-kernel option exists.
7. **Not fundamentally wrong, but worth knowing:** `make_synthetic_american_panel` American-prices every (strike × expiry × side) single-threaded inside the test body before `build_corpus` even starts ([panel.cpp:211-219](../../atx-vol/src/panel.cpp#L211-L219)); `price_in_band`'s cold re-Americanization scorer ([opra_fixture.hpp:197](../../atx-vol/tests/support/opra_fixture.hpp#L197)) is per-liquid-quote and independent of the fit, so surface caching does not remove it; `greeks_fd_reference` fans out 17 AL solves per case ([american_test.cpp:499-543](../../atx-vol/tests/american_test.cpp#L499-L543)). The production kernels themselves are not misimplemented — the waste is recomputation and build flags.

### Known pre-existing failures (do NOT chase in this plan)

5 **release-only** failures exist at baseline (debug run is fully green): `AndersenLakeRegime.PositiveRateGrid_BitIdenticalToPrechange`, `Pin.EvalAndEvalGradBitIdentical`, `Pin.AmericanGreeksBundleBitIdentical`, `Pin.EvalPartialsMatchesEvalGrad`, `PreparedPortfolio.GroupedPriceEqualsIndependentOracleAndPinnedFingerprint` — bit-identity pins, most likely from the staged `american.cpp`/`american.hpp` working-tree changes. Record them before starting; every task's "verify" step compares failures against this baseline, not against zero. Their release-only nature also matters for Task 8: it shows these pins are already sensitive to optimized codegen of the current working-tree kernels — resolve them (or land/revert the staged changes) before drawing conclusions from Task 8's pin check.

## Global Constraints

- Never weaken a numeric tolerance or delete a correctness assertion to make a test faster.
- Bit-identical pinned baselines (MultinamePipeline `:870-889`, `:964-974`, `:1216-1226`) must keep passing byte-for-byte after artifact caching — the archive round-trip is bit-exact, so any diff means the cache key is wrong.
- Cached artifacts live under the **build tree** (`<build>/atx-vol/tests/artifact-cache/`), never committed, and must be safe under `ctest -j 16` (atomic temp-file + rename; concurrent misses may both compute — correct, just briefly wasteful).
- The golden oracle table IS committed (it is a test vector, not a cache) and must carry a regeneration recipe in its header comment.
- Env-gated tests use the existing pattern: `GTEST_SKIP() << "set ATX_VOL_X=1 to run"` (precedent: `ATX_VOL_LONG_CORPUS` in corpus_test.cpp:1158).
- All timings quoted below are on the 16-core reference machine; re-measure, don't assume.
- Windows + clang-cl + Ninja presets (`ninja` = debug at `build/`, `rel` at `build-rel/`). PowerShell 5.1 scripts — no `&&` chaining.

---

### Task 1: Parallelize ctest everywhere

**Files:**
- Modify: `CMakePresets.json` (testPresets section, ~line 94-121)
- Modify: `scripts/atx-build.ps1:48`
- (No change needed to `scripts/dev-build.ps1` / `.vscode/tasks.json` — they invoke `ctest --preset`, which inherits the preset's `execution.jobs`.)

**Interfaces:**
- Produces: every `ctest --preset <name>` run schedules 16 test processes concurrently. Later tasks assume parallel runs are the norm (artifact caches must be race-safe).

- [ ] **Step 1: Record the baseline failure set**

```powershell
ctest --test-dir build-rel -R "atx_vol" -L atx_vol --output-on-failure -j 16 2>$null; ctest --test-dir build-rel -L atx_vol -N | Select-Object -Last 2
```

Save the list of failing test names to compare against later (expect the 5 pre-existing pin failures listed above).

- [ ] **Step 2: Add `execution.jobs` to every testPreset**

In `CMakePresets.json`, for each entry in `"testPresets"` add:

```json
{
  "name": "ninja",
  "configurePreset": "ninja",
  "output": { "outputOnFailure": true },
  "execution": { "jobs": 16, "scheduleRandom": false }
}
```

(Repeat the `"execution"` block verbatim for each existing testPreset: `ninja`, `dev`, `rel`, and any others present. Do not add `"jobs"` to hygiene if that preset is used for sanitizer-style runs that need determinism — check; if unsure, add it there too, nothing in the suite requires serial execution.)

- [ ] **Step 3: Fix the raw ctest invocation in atx-build.ps1**

`scripts/atx-build.ps1:48` currently:

```powershell
ctest --test-dir build --output-on-failure
```

Change to:

```powershell
ctest --test-dir build --output-on-failure -j 16
```

- [ ] **Step 4: Verify parallel run is green (modulo baseline failures) and time it**

```powershell
Measure-Command { ctest --preset rel -L atx_vol --output-on-failure }
```

Expected: wall time drops from ~6 min serial to roughly the longest-pole test + scheduling (~1 min with current offenders). Failure set identical to Step 1 baseline. Watch specifically for new failures in tests that write disk output (`MultinamePipeline`, `Corpus`, backtest tearsheet tests) — those use `fresh_out_dir`-style unique dirs and should be safe; if any collide, that's a test bug to fix (unique-ify the temp dir), not a reason to go back to serial.

- [ ] **Step 5: Run the debug preset the same way**

```powershell
Measure-Command { ctest --preset ninja -L atx_vol --output-on-failure }
```

Expected: ~29 min (overnight serial experience) → ~2-4 min wall (longest pole `CallGreeksAl.ThetaCharm` ~41 s until Task 3 lands; spawn overhead now amortized across 16 workers).

- [ ] **Step 6: Commit**

```powershell
git add CMakePresets.json scripts/atx-build.ps1
git commit -m @'
build: run ctest with -j 16 in all test presets and scripts

The suite was running 900+ test processes strictly serially; nothing in
the suite requires it. Longest-pole tests are addressed separately.
'@
```

---

### Task 2: Split fast/slow test labels + a fast inner-loop preset

**Files:**
- Modify: `atx-vol/tests/CMakeLists.txt:113-114`
- Modify: `CMakePresets.json` (testPresets)

**Interfaces:**
- Produces: ctest labels `atx_vol_fast` / `atx_vol_slow` (both still carry `atx_vol`), and a `ninja-fast` testPreset. Developers' inner loop: `ctest --preset ninja-fast` (~15-20 s debug once Tasks 3-6 land).

- [ ] **Step 1: Define the slow filter from measured data**

Replace the single discovery call in `atx-vol/tests/CMakeLists.txt`:

```cmake
include(GoogleTest)
gtest_discover_tests(atx-vol-tests PROPERTIES LABELS atx_vol)
```

with a two-way split. The slow list = every suite whose serial-debug total exceeded ~10 s in the 2026-07-11 measurement:

```cmake
include(GoogleTest)

# Suites measured >10s (serial debug, 2026-07-11). Everything here still runs
# under `ctest -L atx_vol`; `-L atx_vol_fast` is the developer inner loop.
# Re-derive from build/Testing/Temporary/LastTest.log when the shape changes.
set(ATX_VOL_SLOW_FILTER
    "MultinamePipeline.*:Corpus.*:CorpusGeneratedProperty.*:CorpusBuildSession.*:QualifiedCorpus.*:OpraBreadthCorpus.*:SpyFitCorpus.*:SigmaInterpCorpus.*:CallGreeksAl.*:CallGreeksFd.*:AndersenLake.*:AndersenLakeRegime.*:ImplicitDiff.*:SigmaInterp.*:CurveFitParallel.*:CurveFitParity.*:CurveSurfaceNoArb.*:SpyBidAskRegression.*:SpyPortfolioPnl.*:PnlGreeksConsistency.*:SpyArchiveRoundTrip.*:SpyRealCalendarReporting.*:SpyRealOpra.*:BacktestReal.*:BreadthRegime.*:WarmAcrossTime.*:BoundaryHoist.*:Dispersion.*:CorrectionCache.*:PricerFitterTest.*:PreparedPortfolio.*:AmericanGreeks.*")

gtest_discover_tests(atx-vol-tests
    TEST_FILTER "-${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS "atx_vol;atx_vol_fast")
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS "atx_vol;atx_vol_slow")
```

- [ ] **Step 2: Reconfigure + rebuild so discovery re-runs, then verify the split**

```powershell
cmake --preset ninja; cmake --build build --target atx-vol-tests
ctest --test-dir build -L atx_vol_fast -N | Select-Object -Last 2
ctest --test-dir build -L atx_vol_slow -N | Select-Object -Last 2
```

Expected: fast + slow counts sum to the previous `atx_vol` count (922 + 3 python). Spot-check: `MultinamePipeline.*` only under slow; `Black76.*` only under fast.

- [ ] **Step 3: Add the fast testPreset**

In `CMakePresets.json` testPresets:

```json
{
  "name": "ninja-fast",
  "configurePreset": "ninja",
  "output": { "outputOnFailure": true },
  "execution": { "jobs": 16 },
  "filter": { "include": { "label": "atx_vol_fast" } }
}
```

- [ ] **Step 4: Verify and time the fast preset**

```powershell
Measure-Command { ctest --preset ninja-fast }
```

Expected: all fast-label tests pass in well under a minute (debug).

- [ ] **Step 5: Commit**

```powershell
git add atx-vol/tests/CMakeLists.txt CMakePresets.json
git commit -m @'
build(atx-vol): label fast/slow tests, add ninja-fast inner-loop preset

Slow list derived from measured serial-debug suite totals >10s.
'@
```

---

### Task 3: Golden PDE-oracle table (kill ~150 Crank-Nicolson marches per run)

**Files:**
- Create: `atx-vol/tests/support/oracle_pde_golden.hpp`
- Create: `atx-vol/tests/support/oracle_pde_golden.cpp`
- Create: `atx-vol/tests/support/oracle_pde_golden.tsv` (generated, committed)
- Modify: `atx-vol/tests/CMakeLists.txt` (add the .cpp to the target sources, next to `support/oracle_pricer_pde.cpp`)
- Modify: `atx-vol/tests/american_test.cpp` — call sites at `:251` `:276` `:309` `:661-683` `:776-824` `:839-867` `:1723-1741` `:1765` `:2278-2347` `:2625-2657`
- Test: new gtest cases inside `oracle_pde_golden.cpp` consumers — see steps.

**Interfaces:**
- Consumes: `atx::vol::test::oracle_pde_american(S,K,T,sigma,r,q,side,opts)` and `OraclePdeOpts` from `support/oracle_pricer_pde.hpp:19-30`.
- Produces: `atx::vol::test::oracle_pde_golden(double S, double K, double T, double sigma, double r, double q, Side side, const OraclePdeOpts& opts = {})` → `double`. Drop-in replacement for `oracle_pde_american` at every hardcoded-literal call site. Behavior: exact-key hit → stored value; miss + `ATX_VOL_ORACLE_REGEN` set → compute live, append to the TSV, return it; miss otherwise → `ADD_FAILURE()` with the regen recipe and return live-computed value so the rest of the test still reports.

- [ ] **Step 1: Write the header**

```cpp
#pragma once

// Golden-value front end for the Crank-Nicolson PDE oracle (test-only).
//
// oracle_pde_american() is a pure function and every test call site passes
// compile-time literals, so its values are constants. Recomputing the
// 2000x4000 (or 4000x8000) CN march at test runtime cost ~5 min of debug
// wall per run. This shim serves the values from a committed TSV instead.
//
// Regenerate (release build, single process) after ANY oracle change:
//   $env:ATX_VOL_ORACLE_REGEN = "1"
//   build-rel/bin/atx-vol-tests.exe --gtest_filter=<affected suites>
//   Remove-Item Env:ATX_VOL_ORACLE_REGEN
// then commit the updated oracle_pde_golden.tsv. Keys are exact %.17g
// round-trips of the inputs; any drift in inputs is a MISS, never a stale hit.

#include "atx/vol/types.hpp"
#include "oracle_pricer_pde.hpp"

namespace atx::vol::test {

[[nodiscard]] double oracle_pde_golden(double S, double K, double T,
                                       double sigma, double r, double q,
                                       Side side,
                                       const OraclePdeOpts& opts = {});

}  // namespace atx::vol::test
```

- [ ] **Step 2: Write the implementation**

```cpp
#include "oracle_pde_golden.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace atx::vol::test {
namespace {

namespace fs = std::filesystem;

// The TSV sits next to the test sources; probe the same way opra_fixture.hpp
// probes data/ so it works from any ctest working directory.
fs::path golden_path() {
  for (const char* p : {"../../../atx-vol/tests/support/oracle_pde_golden.tsv",
                        "atx-vol/tests/support/oracle_pde_golden.tsv",
                        "../atx-vol/tests/support/oracle_pde_golden.tsv",
                        "C:/atx/atx-vol/tests/support/oracle_pde_golden.tsv"}) {
    if (fs::exists(p)) return fs::path{p};
  }
  return fs::path{"C:/atx/atx-vol/tests/support/oracle_pde_golden.tsv"};
}

std::string key_of(double S, double K, double T, double sigma, double r,
                   double q, Side side, const OraclePdeOpts& o) {
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "%.17g|%.17g|%.17g|%.17g|%.17g|%.17g|%d|%d|%d|%.17g|%.17g", S,
                K, T, sigma, r, q, static_cast<int>(side), o.n_t, o.n_x,
                o.s_min_mult, o.s_max_mult);
  return buf;
}

std::unordered_map<std::string, double>& table() {
  static std::unordered_map<std::string, double> t = [] {
    std::unordered_map<std::string, double> m;
    std::ifstream in(golden_path());
    std::string k;
    double v;
    while (in >> k >> v) m.emplace(std::move(k), v);
    return m;
  }();
  return t;
}

}  // namespace

double oracle_pde_golden(double S, double K, double T, double sigma, double r,
                         double q, Side side, const OraclePdeOpts& opts) {
  static std::mutex mu;
  const std::string k = key_of(S, K, T, sigma, r, q, side, opts);
  {
    std::lock_guard<std::mutex> lock(mu);
    auto it = table().find(k);
    if (it != table().end()) return it->second;
  }
  const double live = oracle_pde_american(S, K, T, sigma, r, q, side, opts);
  if (std::getenv("ATX_VOL_ORACLE_REGEN") != nullptr) {
    std::lock_guard<std::mutex> lock(mu);
    table().emplace(k, live);
    std::ofstream out(golden_path(), std::ios::app);
    out.precision(17);
    out << k << '\t' << std::scientific << live << '\n';
  } else {
    ADD_FAILURE() << "oracle_pde_golden miss for key " << k
                  << " — regenerate: set ATX_VOL_ORACLE_REGEN=1 and rerun "
                     "this test in the release build, then commit "
                     "atx-vol/tests/support/oracle_pde_golden.tsv";
  }
  return live;
}

}  // namespace atx::vol::test
```

- [ ] **Step 3: Register the new TU and create an empty TSV**

In `atx-vol/tests/CMakeLists.txt` add `support/oracle_pde_golden.cpp` directly under the existing `support/oracle_pricer_pde.cpp` line. Create `oracle_pde_golden.tsv` as an empty file.

- [ ] **Step 4: Swap ONE call site and verify the miss path fails loudly**

In `american_test.cpp`, `AndersenLake.VsPdeOracle_PutGrid` (`:251`), change `oracle_pde_american(...)` → `oracle_pde_golden(...)` (add `#include "support/oracle_pde_golden.hpp"`). Build and run in **release**:

```powershell
cmake --build build-rel --target atx-vol-tests
build-rel/bin/atx-vol-tests.exe --gtest_filter=AndersenLake.VsPdeOracle_PutGrid
```

Expected: FAIL with the "regenerate" message (table is empty). This proves a stale/missing table can never silently pass.

- [ ] **Step 5: Regenerate and verify the hit path**

```powershell
$env:ATX_VOL_ORACLE_REGEN = "1"
build-rel/bin/atx-vol-tests.exe --gtest_filter=AndersenLake.VsPdeOracle_PutGrid
Remove-Item Env:ATX_VOL_ORACLE_REGEN
build-rel/bin/atx-vol-tests.exe --gtest_filter=AndersenLake.VsPdeOracle_PutGrid
```

Expected: first run passes (regen mode computes live; values are by construction identical), TSV now has 12 rows; second run passes **fast** (no CN march — should drop from ~1.7 s to ms in release).

- [ ] **Step 6: Swap the remaining call sites**

Same mechanical edit (`oracle_pde_american(` → `oracle_pde_golden(`) at every literal-parameter call site in `american_test.cpp`:
- `AndersenLake.VsPdeOracle_CallGrid` `:276`
- `Baw.VsPdeOracle_WithinApproximationTolerance` `:309`
- `CallGreeksFd.Fast_MeetsPdeGreekGates` `:681-683`
- `CallGreeksAl.MeetsPdeGreekGates` `:812`, `:816-817`, `:823-824`
- `CallGreeksAl.ThetaCharm_MoreAccurateThanFd` `:861-867` (fine-grid opts — the key includes n_t/n_x so fine and default grids coexist)
- `AndersenLakeRegime.CornerGrid_VsPdeOracle` `:1741`
- `AndersenLakeRegime.UnsupportedPutRegression_OldEuropeanWasWrong` `:1765`
- `SigmaInterp.MeetsPdeGreekGates` `:2341,2346,2347`
- `ImplicitDiff.MeetsPdeGreekGates` `:2650-2657`

Do NOT touch any call site whose parameters are not compile-time constants (grep first: `grep -n "oracle_pde_american" atx-vol/tests/*.cpp` — if a site feeds runtime-derived params, leave it live and note it).

- [ ] **Step 7: Regenerate the full table in release, then verify both builds**

```powershell
$env:ATX_VOL_ORACLE_REGEN = "1"
build-rel/bin/atx-vol-tests.exe --gtest_filter="AndersenLake.*:Baw.*:CallGreeks*:AndersenLakeRegime.*:SigmaInterp.*:ImplicitDiff.*"
Remove-Item Env:ATX_VOL_ORACLE_REGEN
build-rel/bin/atx-vol-tests.exe --gtest_filter="AndersenLake.*:Baw.*:CallGreeks*:AndersenLakeRegime.*:SigmaInterp.*:ImplicitDiff.*"
cmake --build build --target atx-vol-tests
build/bin/atx-vol-tests.exe --gtest_filter="AndersenLake.*:Baw.*:CallGreeks*:AndersenLakeRegime.*:SigmaInterp.*:ImplicitDiff.*"
```

Expected: release regen populates ~150 rows; the post-regen release run passes with the same pass/fail set as before this task; the **debug** run of these suites drops from ~155 s to under ~20 s (the residual is `greeks_fd_reference`'s AL solves, which stay live — they are the thing under test).

- [ ] **Step 8: Commit**

```powershell
git add atx-vol/tests/support/oracle_pde_golden.hpp atx-vol/tests/support/oracle_pde_golden.cpp atx-vol/tests/support/oracle_pde_golden.tsv atx-vol/tests/american_test.cpp atx-vol/tests/CMakeLists.txt
git commit -m @'
test(atx-vol): serve PDE-oracle reference values from a committed golden table

oracle_pde_american is a pure function called only with literal params;
~150 Crank-Nicolson marches per run (about 5 min of debug wall) were
recomputing constants. Misses fail loudly with a regen recipe;
ATX_VOL_ORACLE_REGEN=1 recomputes and appends.
'@
```

---

### Task 4: Cache the fitted SPY ConvexDense surface for consumer tests

**Files:**
- Create: `atx-vol/tests/support/cached_artifacts.hpp`
- Create: `atx-vol/tests/support/cached_artifacts.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt` (add the .cpp)
- Modify: `atx-vol/tests/spy_bidask_regression_test.cpp` (`ConvexDenseServedViaSessionInBand`, `PricerFitterExplicitConvexInBand`)
- Modify: `atx-vol/tests/spy_portfolio_pnl_test.cpp` (SPY leg)
- Modify: `atx-vol/tests/pnl_greeks_consistency_test.cpp` (`Session_ConvexDense_GreeksPrice_BitEqual_FairValue`)
- Modify: `atx-vol/tests/curve_noarb_test.cpp` (`SpyDenseIsCalendarArbFree`)

**Interfaces:**
- Consumes: `write_surface_archive_file` (surface_archive.hpp:276), `SurfaceArchive::open_file` (:292), `map_symbol` (:306); the existing per-file fit helpers (each consumer test today builds the surface via `VolaSession::build`/`PricerFitter` with Fast + ConvexDense + node_cap 40 on `data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet`).
- Produces: `atx::vol::test::cached_spy_convex_dense()` → the archive **file path** (`std::filesystem::path`) of a fitted, serialized SPY ConvexDense-nc40 surface, fitting and writing it on first call (process-local + on-disk cache). Consumers reload via `SurfaceArchive::open_file` exactly as `spy_archive_roundtrip_test` already does. Returns empty path if the parquet fixture is absent (caller then `GTEST_SKIP`s, same as today).

**Cache-key rule (applies to Task 5 too):** file name = `spy_convexdense_nc40_v<N>.atxvsa` where `<N>` is the surface-archive format version constant from `surface_archive.hpp`. Bump-on-format-change comes free; a *fitter behavior* change is caught by the fit-gate tests that still fit live (`SpyBoardBitIdentical…` synthetic twins, `spy_archive_roundtrip`, `SpyFitCorpus.HftColdStart…`) — and by CI running from a clean build tree, where the cache is always rebuilt fresh.

- [ ] **Step 1: Write the helper**

```cpp
// cached_artifacts.hpp
#pragma once

#include <filesystem>

namespace atx::vol::test {

// Path to a fitted+serialized SPY ConvexDense (Fast preset, node_cap=40)
// surface for the 2026-06-05 OPRA fixture. Fits once per build tree, then
// reloads. Empty path if the parquet fixture is unavailable.
[[nodiscard]] std::filesystem::path cached_spy_convex_dense();

}  // namespace atx::vol::test
```

Implementation (`cached_artifacts.cpp`) — the fit block is the exact recipe shared by `spy_bidask_regression_test.cpp:55-61` and `spy_archive_roundtrip_test.cpp:72-88` (Fast preset, ConvexDense, node_cap 40; same config → bit-identical artifact):

```cpp
#include "cached_artifacts.hpp"

#include "opra_fixture.hpp"  // load_opra_board / make_session_inputs

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>

namespace atx::vol::test {
namespace fs = std::filesystem;

fs::path cached_spy_convex_dense() {
  const fs::path dir{"artifact-cache"};  // under the ctest CWD (build tree)
  std::error_code ec;
  fs::create_directories(dir, ec);
  // Suffix with the surface-archive format-version constant from
  // surface_archive.hpp (use its real name) so format bumps invalidate.
  const fs::path file = dir / "spy_convexdense_nc40_v1.atxvsa";
  if (fs::exists(file)) return file;

  auto board = load_opra_board("spy", "SPY");
  if (!board.has_value()) return {};  // caller GTEST_SKIPs, same as today

  // The 99.5% recipe — verbatim from spy_archive_roundtrip_test.cpp:72-76.
  SessionInputs in = make_session_inputs(FitPreset::Fast, board->spot(),
                                         board->r, board->now_ns());
  in.cash_divs = board->panel.frame.divs;
  in.curve.kind = VolCurveKind::ConvexDense;
  in.curve.convex.node_cap = 40;

  auto sess = VolaSession::build(board->underlying(), in);
  if (!sess.has_value()) { ADD_FAILURE() << sess.error().to_string(); return {}; }
  auto priced = sess->to_priced_surface();
  if (!priced.has_value()) { ADD_FAILURE() << priced.error().to_string(); return {}; }

  // Atomic publish so concurrent ctest -j misses never observe a torn file.
  std::mt19937_64 rng{std::random_device{}()};
  const fs::path tmp =
      dir / (file.filename().string() + "." + std::to_string(rng()) + ".tmp");
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &*priced}};
  // write_surface_archive_file: surface_archive.hpp:276 — match its actual
  // (items, path) argument order.
  auto wrote = write_surface_archive_file(items, tmp.string());
  if (!wrote.has_value()) { ADD_FAILURE() << wrote.error().to_string(); return {}; }
  fs::rename(tmp, file, ec);
  if (ec) fs::remove(tmp, ec);  // lost the race: someone else published — fine
  return file;
}

}  // namespace atx::vol::test
```

Consumers reload with `SurfaceArchive::open_file(path)` → `map_symbol("SPY")` → `PricedSurface` (the exact idiom at spy_archive_roundtrip_test.cpp:91-95, which also proves the reload prices bit-identically to the live session). Caution: a consumer that calls `VolaSession`-only APIs must be re-expressed against `PricedSurface`'s equivalents (fv/iv/greeks all exist there — that is what the roundtrip test asserts); if one genuinely needs live-session state that the archive does not carry, leave that test fitting live and say so in the commit message.

- [ ] **Step 2: Convert `pnl_greeks_consistency_test.cpp` first (it already asserts bit-equality — the perfect canary)**

Replace its build-the-session setup with: `const auto p = cached_spy_convex_dense(); if (p.empty()) GTEST_SKIP(...); auto arch = SurfaceArchive::open_file(p.string()); ...` mirroring how `spy_archive_roundtrip_test.cpp` reloads and reconstructs the session. Run it:

```powershell
cmake --build build-rel --target atx-vol-tests
build-rel/bin/atx-vol-tests.exe --gtest_filter=PnlGreeksConsistency.Session_ConvexDense_GreeksPrice_BitEqual_FairValue
```

Expected: PASS (bit-equal assertions prove the cached artifact is equivalent). Second run: same test drops to ~load-time (release: 2.7 s → well under 1 s; debug: 33.6 s → a few seconds).

- [ ] **Step 3: Convert the remaining consumers**

Same edit in `spy_bidask_regression_test.cpp` (the two ConvexDense consumer tests — leave `PricerFitterHftColdStartInBand` and `AutoSelectPicksDenseForSpy` fitting live; auto-select and cold-start ARE the fit path), `spy_portfolio_pnl_test.cpp` (SPY leg only; the XOM board is 21 KB and cheap), `curve_noarb_test.cpp` (`arb_check_calendar` runs on the reconstructed surface — the calendar-arb property lives in the artifact).

Do NOT convert (fit gates, keep fitting live): `spy_archive_roundtrip_test.cpp` (it validates fit→serialize→reload), `spy_real_test.cpp` `DenseSurfaceReportsMeasuredCalendar` (asserts `SessionDiagnostics.calendar_arb_free`, a fit byproduct not stored in the archive), `SpyFitCorpus.HftColdStart…`, `CurveFitParallel.*`.

- [ ] **Step 4: Verify the converted set, both builds, twice (cold cache + warm cache)**

```powershell
Remove-Item -Recurse -Force build-rel/atx-vol/tests/artifact-cache -ErrorAction SilentlyContinue
ctest --test-dir build-rel -R "PnlGreeksConsistency|SpyBidAskRegression|SpyPortfolioPnl|CurveSurfaceNoArb" -j 16 --output-on-failure
ctest --test-dir build-rel -R "PnlGreeksConsistency|SpyBidAskRegression|SpyPortfolioPnl|CurveSurfaceNoArb" -j 16 --output-on-failure
```

Expected: both runs pass with the same result set as baseline; the cold run pays one fit (whichever process wins), the warm run pays zero. The `-j 16` cold run exercises the rename race deliberately.

- [ ] **Step 5: Commit**

```powershell
git add atx-vol/tests/support/cached_artifacts.hpp atx-vol/tests/support/cached_artifacts.cpp atx-vol/tests/CMakeLists.txt atx-vol/tests/spy_bidask_regression_test.cpp atx-vol/tests/spy_portfolio_pnl_test.cpp atx-vol/tests/pnl_greeks_consistency_test.cpp atx-vol/tests/curve_noarb_test.cpp
git commit -m @'
test(atx-vol): reload cached SPY ConvexDense surface in consumer tests

The identical Fast+ConvexDense+nc40 fit of the 14k-contract SPY board was
recomputed by five tests per run. Fit gates (roundtrip, cold-start,
auto-select, worker parity) still fit live; consumers reload the archived
surface, which spy_archive_roundtrip proves bit-exact.
'@
```

---

### Task 5: Prebuilt corpora for MultinamePipeline consumers + Manifest fix

**Files:**
- Modify: `atx-vol/tests/support/cached_artifacts.hpp` / `.cpp` (add corpus caching)
- Modify: `atx-vol/tests/multiname_pipeline_test.cpp` (consumer tests listed below)
- Modify: `atx-vol/tests/corpus_test.cpp:332-357` (`Manifest_RoundTrips`)

**Interfaces:**
- Consumes: `build_corpus(std::vector<CorpusBoard>, std::string out_dir)` → `Result<CorpusManifest>`; the file-local helpers `make_multiname_boards(dates)` / `missing_bbb_boards(...)` in multiname_pipeline_test.cpp:162-173/758-770; `MarketSnapshot::load`.
- Produces: `atx::vol::test::cached_corpus(const char* key, const std::function<std::vector<CorpusBoard>()>& boards)` → `std::filesystem::path` of a corpus directory (date `.atxvsa` files + `manifest.tsv`), built once per build tree per key. Keys used: `"multiname-full-3d"` (12 boards), `"multiname-missing-bbb-3d"` (11 boards).

- [ ] **Step 1: Add `cached_corpus` to the helper**

```cpp
// cached_artifacts.hpp — add:
#include <functional>
#include <vector>
// forward declarations of CorpusBoard as needed via the corpus header

// Directory containing a built corpus (one .atxvsa per date + manifest.tsv)
// for the given key, building it on first use. Safe under ctest -j: builds
// into a unique tmp dir, atomically renamed into place.
[[nodiscard]] std::filesystem::path cached_corpus(
    const char* key, const std::function<std::vector<CorpusBoard>()>& boards);
```

```cpp
// cached_artifacts.cpp — add:
fs::path cached_corpus(const char* key,
                       const std::function<std::vector<CorpusBoard>()>& boards) {
  const fs::path dir = fs::path{"artifact-cache"} / key;
  if (fs::exists(dir / "manifest.tsv")) return dir;
  fs::create_directories("artifact-cache");
  std::mt19937_64 rng{std::random_device{}()};
  const fs::path tmp =
      fs::path{"artifact-cache"} / (std::string{key} + "." + std::to_string(rng()) + ".tmp");
  auto man = build_corpus(boards(), tmp.string());
  if (!man.has_value()) {
    ADD_FAILURE() << "cached_corpus(" << key << "): " << man.error().to_string();
    return tmp;  // let the caller fail on load with context
  }
  std::error_code ec;
  fs::rename(tmp, dir, ec);
  if (ec) fs::remove_all(tmp, ec);  // lost the publish race — the winner's dir is equivalent
  return dir;
}
```

Note the determinism prerequisite: `Corpus.Deterministic_AcrossThreadCounts` and the pinned bit-identical baselines already prove `build_corpus` output is bit-stable for fixed inputs, so "winner's dir is equivalent" holds byte-for-byte.

- [ ] **Step 2: Move the board-builder helpers where the cache can see them**

`make_multiname_boards` and `missing_bbb_boards` are file-local to multiname_pipeline_test.cpp. Keep them there; the test passes them in as the lambda: `cached_corpus("multiname-full-3d", [&]{ return make_multiname_boards(dates3); })`. No helper relocation needed.

- [ ] **Step 3: Convert consumer tests one at a time, running each after conversion**

Consumers (replace `fresh_out_dir` + `build_corpus(...)` with `cached_corpus(...)`; everything after the manifest/load line is untouched):
- `CorpusWithMissingNameOnOneDateRunsToCompletion :383` → missing-bbb key (verify its board set matches `missing_bbb_boards`; if it omits CCC not BBB, give it its own key `"multiname-missing-ccc-inception"`)
- `AllNamesMissingIsNoTradeStepNotAbort :468` → own key (distinct board set)
- `HeldNameGoesMissingMidRunAndRunCompletes :634` → missing-bbb key
- `HeldLotWithoutSurfaceIsCountedNotHidden :835`, `UnpricedLotPolicyErrorAborts :914`, `BookGreeksUnderCountIsReported :1003` → missing-bbb key
- `DefaultPolicyFullBasketBitIdentical :942`, `DefaultPolicyStillBitIdentical :1190` → full key (**bit-identical pins — if these fail after conversion the cache key is wrong, stop and fix**)
- `GrossVegaIsUnderReportedWhenALegIsUnpriced :1061` → BOTH keys (was 2 fresh builds = 23 board fits, becomes 0 on warm cache)
- `NoTradeOnRollDateLeavesBookIntact :541`, `UnpricedGreeksPolicyErrorAborts :1126` → own keys if their board sets are unique; check each against the shared builders before assigning
- Leave fitting live: `MultiSymbolDateLoadsWithDistinctUids :178`, `UniverseAuthoredBySymbolResolvesOnEveryDate :246` (uid stamping happens at archive **write** time — the write is the gate), `ResolveUniverseRejectsUnknownAndDuplicateSymbols :309`, `UidOfIsCaseInsensitive :343` (4-board builds; optionally point both at one shared small key later).

Run after each conversion:

```powershell
build-rel/bin/atx-vol-tests.exe --gtest_filter=MultinamePipeline.<JustConverted>
```

- [ ] **Step 4: Fix `Corpus.Manifest_RoundTrips` to not build a corpus at all**

Mirror its sibling `Manifest_RoundTripsEveryCurveKind` (corpus_test.cpp:359-388): construct a `CorpusManifest` struct by hand with the same fields the built one would carry, then run the identical `serialize_manifest`→`parse_manifest`→`read_manifest_file` assertions. Delete the `make_mixed_boards`/`build_corpus` setup lines (:334-337). 22.5 s (debug) → ms.

- [ ] **Step 5: Full-file verify, cold + warm, parallel**

```powershell
Remove-Item -Recurse -Force build-rel/atx-vol/tests/artifact-cache -ErrorAction SilentlyContinue
ctest --test-dir build-rel -R "MultinamePipeline|^Corpus\." -j 16 --output-on-failure
ctest --test-dir build-rel -R "MultinamePipeline|^Corpus\." -j 16 --output-on-failure
```

Expected: identical pass set to baseline both runs; warm run's MultinamePipeline consumers each complete in load+backtest time only (debug: 52.5 s suite → ~10 s; release: 31.5 s → ~5 s).

- [ ] **Step 6: Commit**

```powershell
git add atx-vol/tests/support/cached_artifacts.hpp atx-vol/tests/support/cached_artifacts.cpp atx-vol/tests/multiname_pipeline_test.cpp atx-vol/tests/corpus_test.cpp
git commit -m @'
test(atx-vol): share prebuilt corpora across MultinamePipeline consumers

Each policy-knob test rebuilt and refit the same synthetic corpus
(~130 board fits per suite run across 3 distinct corpora). Consumers now
load a per-build-tree cached corpus; archive-write gates (uid stamping,
determinism, roundtrip) still build live. Manifest_RoundTrips constructs
its manifest directly - the corpus build was incidental to what it asserts.
'@
```

---

### Task 6: Cache the 10 fitted corpus boards for SigmaInterpCorpus

**Files:**
- Modify: `atx-vol/tests/support/cached_artifacts.hpp` / `.cpp`
- Modify: `atx-vol/tests/spy_fit_corpus_test.cpp:71-170` (`SigmaInterpCorpus.RealBoard_WithinGates`)

**Interfaces:**
- Consumes: `load_spy_fit_fixture` / `kSpyFitFixtures` (support/spy_fit_fixture.hpp:24, 10 slices), `PricerFitter{Hft}` as used at spy_fit_corpus_test.cpp:42-45.
- Produces: `atx::vol::test::cached_hft_fit(const SpyFitFixture&)` → archive path of that fixture's Hft-preset fitted surface (same key rule: `hftfit_<fixture.id>_v<N>.atxvsa`).

- [ ] **Step 1: Add `cached_hft_fit` following the Task 4 pattern** (fit block copied from `SpyFitCorpus.HftColdStart…`'s loop body at spy_fit_corpus_test.cpp:42-45; atomic publish identical to Task 4 Step 1).

- [ ] **Step 2: Convert `SigmaInterpCorpus.RealBoard_WithinGates`** to reload each fixture's surface from `cached_hft_fit(fixture)` instead of refitting (spy_fit_corpus_test.cpp:83). The interpolant-vs-cold-AL parity sweep body is untouched — the cold per-strike Andersen-Lake comparison **is the assertion** and stays live. `SpyFitCorpus.HftColdStartPreserves98PctOnEveryAvailableSlice` keeps fitting live (it is the cold-start gate) — it does not consume the cache, and the cache does not consume it (independent under `-j`).

- [ ] **Step 3: Verify**

```powershell
build-rel/bin/atx-vol-tests.exe --gtest_filter=SigmaInterpCorpus.RealBoard_WithinGates
build-rel/bin/atx-vol-tests.exe --gtest_filter=SigmaInterpCorpus.RealBoard_WithinGates
```

Expected: PASS both; warm run drops the 10 Hft fits (release 29.5 s / debug 32.6 s → dominated by the cold-AL parity ladder only; measure and record). If the residual cold-AL sweep still exceeds ~10 s release, additionally stratify: assert every ladder but only on every 2nd strike, with a full-strike sweep behind `ATX_VOL_SCOREBOARDS=1` — only if needed, YAGNI otherwise.

- [ ] **Step 4: Commit**

```powershell
git add atx-vol/tests/support/cached_artifacts.hpp atx-vol/tests/support/cached_artifacts.cpp atx-vol/tests/spy_fit_corpus_test.cpp
git commit -m @'
test(atx-vol): SigmaInterpCorpus reloads cached Hft fits of the 10 slices

The sigma-interpolant parity gate refit all 10 corpus boards that
HftColdStart had already fit in the same suite run. The cold per-strike
AL parity comparison (the actual assertion) stays live.
'@
```

---

### Task 7: Env-gate the sprint scoreboards and real-data parity duplicates

**Files:**
- Modify: `atx-vol/tests/american_test.cpp:1859` (`BoundaryHoist.SeedSpike_SweepCount`), `:2546` (`WarmAcrossTime.SweepReduction`)
- Modify: `atx-vol/tests/curve_fit_parallel_test.cpp` (`SpyBoardBitIdenticalAcrossWorkers` ~:380, `ParityOffSkipsSecondDeAmOnSpy` ~:487)
- Modify: `atx-vol/tests/corpus_test.cpp:1317` (`CorpusBuildSession.SyntheticThirteenNameThreeDateBreadthScoreboard`)
- Modify: `CMakePresets.json` (nightly test preset)

**Interfaces:**
- Produces: env flag `ATX_VOL_SCOREBOARDS=1` runs them; default runs skip. A `nightly` testPreset runs everything including scoreboards and `ATX_VOL_LONG_CORPUS`.

Rationale per test (all four keep their assertions — they are just too expensive for every run, and each has a fast correctness twin still gating):
- `WarmAcrossTime.SweepReduction`: kill-gate evidence scoreboard for a feature deliberately left OFF (comment at :2539-2545 says so); `ConvergesToCold`/`MoveGuard_ColdReseeds` keep gating warm==cold correctness.
- `BoundaryHoist.SeedSpike_SweepCount`: 1800-point × 3-seed-mode sweep-count study; correctness of the boundary kernel is gated elsewhere (`AndersenLake.*`, pins).
- `CurveFitParallel.SpyBoardBitIdenticalAcrossWorkers` / `CurveFitParity.ParityOffSkipsSecondDeAmOnSpy`: 4 + 3 full real-board cold passes (139 s debug); synthetic twins `SyntheticBoardBitIdenticalAcrossWorkers` / `ParityOffMatchesParityOnSurface` gate the same invariants per run.
- `CorpusBuildSession.SyntheticThirteenNameThreeDateBreadthScoreboard`: 13×3 breadth scoreboard + 3 backtests; the individual build/admission/backtest gates it bundles all exist as separate tests.

- [ ] **Step 1: Add the guard to each of the four tests** (first line of the test body, exact existing pattern from corpus_test.cpp:1158):

```cpp
  if (std::getenv("ATX_VOL_SCOREBOARDS") == nullptr) {
    GTEST_SKIP() << "sprint scoreboard - set ATX_VOL_SCOREBOARDS=1 (nightly preset) to run";
  }
```

(Ensure `<cstdlib>` is included; american_test.cpp and corpus_test.cpp already use `std::getenv`.)

- [ ] **Step 2: Add the nightly preset**

```json
{
  "name": "nightly",
  "configurePreset": "rel",
  "output": { "outputOnFailure": true },
  "execution": { "jobs": 16 },
  "environment": { "ATX_VOL_SCOREBOARDS": "1", "ATX_VOL_LONG_CORPUS": "1" }
}
```

(If `testPresets` entries in this file don't support `"environment"` under the pinned CMake version, fall back to a `scripts/nightly-tests.ps1` that sets both env vars and calls `ctest --preset rel`.)

- [ ] **Step 3: Verify skip + nightly paths**

```powershell
build-rel/bin/atx-vol-tests.exe --gtest_filter="WarmAcrossTime.SweepReduction:BoundaryHoist.SeedSpike_SweepCount:CurveFitParallel.SpyBoardBitIdenticalAcrossWorkers:CurveFitParity.ParityOffSkipsSecondDeAmOnSpy:CorpusBuildSession.SyntheticThirteenNameThreeDateBreadthScoreboard"
$env:ATX_VOL_SCOREBOARDS = "1"
build-rel/bin/atx-vol-tests.exe --gtest_filter="WarmAcrossTime.SweepReduction:BoundaryHoist.SeedSpike_SweepCount"
Remove-Item Env:ATX_VOL_SCOREBOARDS
```

Expected: first run = 5 SKIPPED in milliseconds; gated run = tests execute and pass.

- [ ] **Step 4: Commit**

```powershell
git add atx-vol/tests/american_test.cpp atx-vol/tests/curve_fit_parallel_test.cpp atx-vol/tests/corpus_test.cpp CMakePresets.json
git commit -m @'
test(atx-vol): gate sprint scoreboards behind ATX_VOL_SCOREBOARDS

SweepReduction, SeedSpike_SweepCount, the two real-board worker/parity
studies, and the 13x3 breadth scoreboard are evidence sweeps whose
invariants are gated per-run by fast synthetic twins. Nightly preset
runs them plus ATX_VOL_LONG_CORPUS.
'@
```

---

### Task 8: Optimized numeric kernels in the Debug preset

**Files:**
- Modify: `atx-vol/CMakeLists.txt` (where the `atx-vol` library target is defined)
- Modify: `CMakeLists.txt` (root — add the option)
- Modify: `CMakePresets.json` (`ninja`/`dev` presets set the option ON)

**Interfaces:**
- Produces: CMake option `ATX_FAST_DEBUG_KERNELS` (default OFF; ON in `ninja`/`dev` presets). When ON and config is Debug, the `atx-vol` **library** TUs compile `/O2 /Ob2` without `/RTC1`; test TUs, gtest, and everything else stay full-debug. Turn it OFF (`cmake --preset ninja -DATX_FAST_DEBUG_KERNELS=OFF`) when stepping through kernel code.

Why this is safe: mixing `/O2` and `/Od` object files links fine; the CRT (`-MDd`) and `_ITERATOR_DEBUG_LEVEL=2` are **unchanged**, so there is no LNK2038 ABI mismatch with vcpkg's debug gtest. And the toolchain is **clang-cl** (see `build/build.ninja:149`), where `/RTC1` is a documented ignored no-op and the **last** optimization flag on the command line wins — so appending `/O2 /Ob2` per-target after the directory-level `/Od /Ob0` is a clean override with no flag-stripping surgery.

- [ ] **Step 1: Add the option at root**

```cmake
option(ATX_FAST_DEBUG_KERNELS
    "Compile the atx-vol numeric-kernel library optimized (/O2 /Ob2) in Debug builds" OFF)
```

- [ ] **Step 2: Append the per-target override in atx-vol/CMakeLists.txt** (after the `atx-vol` target's existing `target_compile_*` calls, near [atx-vol/CMakeLists.txt:17](../../atx-vol/CMakeLists.txt#L17); the library target and the tests are in different directory scopes — `tests/` is an `add_subdirectory` — so tests are untouched by construction):

```cmake
if(ATX_FAST_DEBUG_KERNELS AND MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # clang-cl: /RTC1 in CMAKE_CXX_FLAGS_DEBUG is an ignored no-op, and the
    # LAST optimization flag wins, so this cleanly overrides the directory
    # /Od /Ob0 for the kernel library only. CRT (-MDd) and
    # _ITERATOR_DEBUG_LEVEL stay untouched — no ABI mismatch with the rest
    # of the debug build. Guarded to clang-cl: plain MSVC (vs preset) errors
    # on /RTC1 + /O2 (D8016), so it never takes this path.
    target_compile_options(atx-vol PRIVATE $<$<CONFIG:Debug>:/O2 /Ob2>)
endif()
```

Acceptance check for this step (run after Step 4's reconfigure): `ninja -C build -t commands` for one `atx-vol` library TU must show `/O2` **after** `/Od`; one test TU must show only `/Od /Ob0`. If clang-cl emits a hard error on the flag combination (it should only warn, if that), fall back to giving the library its own directory scope: move the `add_library(atx-vol ...)` block from [atx-vol/CMakeLists.txt:17](../../atx-vol/CMakeLists.txt#L17) into a new `atx-vol/src/CMakeLists.txt` (mechanical move, `add_subdirectory(src)` in its place) and do `string(REPLACE "/Od /Ob0 /RTC1" "/O2 /Ob2" CMAKE_CXX_FLAGS_DEBUG ...)` inside that scope only. Do not try mid-file save/restore of `CMAKE_CXX_FLAGS_DEBUG` around the target — directory flags are taken from the value at the *end* of the directory's CMakeLists, so save/restore silently does nothing.

- [ ] **Step 3: Set it ON in the dev presets** (`CMakePresets.json`, in the `ninja` and `dev` configurePresets' `cacheVariables`):

```json
"ATX_FAST_DEBUG_KERNELS": "ON"
```

- [ ] **Step 4: Reconfigure, rebuild, verify flags and measure**

```powershell
cmake --preset ninja; cmake --build build --target atx-vol-tests
build/bin/atx-vol-tests.exe --gtest_filter="MultinamePipeline.DefaultPolicyStillBitIdentical" 
```

Expected: the kernel-bound test drops ~3-6× vs its pre-task debug time (record exact numbers). Then run the full debug suite and confirm the pass set matches baseline — **including the bit-identical pins**: optimization can legitimately change floating-point results only if the code relies on non-deterministic FP (it must not — `/fp:precise` is the default and unchanged). If any pin diverges under `/O2`, STOP: that is a real finding about FP-contract fragility in the kernel — report it, don't paper over it (the pins passing in Release `/O2` today makes this unlikely).

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt atx-vol/CMakeLists.txt atx-vol/src/CMakeLists.txt CMakePresets.json
git commit -m @'
build(atx-vol): ATX_FAST_DEBUG_KERNELS - optimized kernel TUs in Debug

Debug /Od /Ob0 /RTC1 + IDL=2 made pricing kernels 6-8x slower than
release, dominating test wall time. The atx-vol library now compiles
/O2 /Ob2 without /RTC1 in dev presets; tests, gtest, CRT, and iterator
debugging are unchanged (no ABI mismatch). Disable with
-DATX_FAST_DEBUG_KERNELS=OFF when debugging kernel internals.
'@
```

---

### Task 9: Re-measure, set the regression guardrail, document

**Files:**
- Modify: `atx-vol/README.md` (testing section)
- Create: `atx-vol/docs/test-suite-budget.md`

**Interfaces:**
- Consumes: everything above.
- Produces: recorded before/after table + the invocation contract (`ninja-fast` inner loop, `ninja`/`rel` full, `nightly` scoreboards).

- [ ] **Step 1: Full timed runs, all four presets**

```powershell
Remove-Item -Recurse -Force build/atx-vol/tests/artifact-cache, build-rel/atx-vol/tests/artifact-cache -ErrorAction SilentlyContinue
Measure-Command { ctest --preset ninja -L atx_vol }        # cold cache
Measure-Command { ctest --preset ninja -L atx_vol }        # warm cache
Measure-Command { ctest --preset rel  -L atx_vol }
Measure-Command { ctest --preset ninja-fast }
```

- [ ] **Step 2: Acceptance targets** (fail the task if not met; each shortfall names its lever):

| Run | Target |
|---|---|
| `ctest --preset ninja` (debug, warm cache, -j16) | ≤ 90 s wall |
| `ctest --preset rel` (release, warm cache, -j16) | ≤ 40 s wall |
| `ctest --preset ninja-fast` | ≤ 15 s wall |
| Slowest single test, debug, warm cache | ≤ 20 s |
| Pass/fail set | identical to the recorded pre-plan baseline (5 release-only known failures; debug green) |

- [ ] **Step 3: Write `atx-vol/docs/test-suite-budget.md`** — the before/after table, the four root causes, the artifact-cache key rule, the golden-table regen recipe, and a one-line policy: *"a new test that adds > 5 s (release) to the default run must either consume cached artifacts or carry a scoreboard/long-corpus gate."*

- [ ] **Step 4: Update `atx-vol/README.md` testing section** with the three invocations (fast/full/nightly) and a pointer to the budget doc.

- [ ] **Step 5: Commit**

```powershell
git add atx-vol/README.md atx-vol/docs/test-suite-budget.md
git commit -m "docs(atx-vol): test-suite time budget, invocation contract, cache/regen recipes"
```

---

## Expected Impact Summary (from today's controlled measurements)

| Lever | Task | Debug CPU saved (of 508 s) | Release CPU saved (of 230 s) |
|---|---|---|---|
| ctest -j 16 | 1 | wall: ~29 min experienced → ~2-4 min (no CPU change) | wall: ~5-6 min → ~1 min |
| Golden PDE oracle | 3 | ~130-140 s (oracle-bound suites total 155 s) | ~20 s |
| SPY surface cache | 4 | ~12-15 s (was ~10× larger pre-perf-commits; still the right structure) | ~7 s |
| Corpora cache + manifest fix | 5 | ~45 s (MultinamePipeline 52.5 + Manifest) | ~28 s |
| SigmaInterp fit reuse | 6 | ~20-25 s of 32.6 s | ~15-20 s of 29.5 s |
| Scoreboard gating | 7 | ~50 s (SweepReduction 13.6, SeedSpike 20.2, SPY parity pair ~15, scoreboard ~3) | ~14 s |
| Fast debug kernels | 8 | remaining kernel-bound time ÷ ~3-6 | n/a |

Compounding: after Tasks 3-7 the debug CPU total drops from ~508 s to roughly 240-270 s; Task 8 divides most of the remainder (→ ~80-130 s CPU); Task 1 spreads it over 16 cores. End state ≈ 30-60 s debug wall, ≈ 20-30 s release wall, ≈ 10-15 s inner loop (`ninja-fast`). The overnight-experience number (29 min) collapses ~30-50×.

## Execution Order & Independence

Task 1 → Task 2 first (infrastructure, everything else is measured against them). Tasks 3, 4/5/6 (share the `cached_artifacts` files — do 4 → 5 → 6 in order), 7, and 8 are mutually independent and can proceed in any order or in parallel worktrees. Task 9 last.
