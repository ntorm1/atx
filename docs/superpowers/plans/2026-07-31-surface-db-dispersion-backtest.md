# Surface-DB Dispersion Backtest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One command — start date, end date, dispersion config file, surface-DB path — runs a full index-vs-names dispersion backtest off a fitted surface database, correctly and fast, against the real `sp100-*` production DBs.

**Architecture:** The bridge already exists: `SurfaceDb::open` → `Clock::from_surface_db` → `run_backtest`. What is missing is (1) a date-window subset on `Clock`, (2) a universe derived from the DB manifest, (3) a plain-file dispersion config reader, (4) a library entry point + thin example binary composing them, (5) tests proving correctness on clustered-absence data shaped like the real DBs, and (6) a perf pass using the engine's existing prefetch/sealed-mmap levers. All new logic lives in one new header/source pair; the engine and the DB layer are not modified except for the one `Clock` method.

**Tech Stack:** C++20 (atx-vol), GoogleTest via ctest, pybind11 bindings, PowerShell build script.

## Current state (verified 2026-07-31, branch `main` @ 649daaa)

What exists and is tested:

- `SurfaceDb::open(root)` (`atx-vol/include/atx/vol/surface_db.hpp:565`), thread-safe const reads, 16-partition LRU cache. Production roots: `C:/atx-data/surface-db/sp100-2025` (104 partitions, 2025-08-01..2025-12-31), `sp100-2026` (140 partitions, ..2026-07-24). 102 symbols **including SPY** (verified via admin `symbols` + `query`: SPY 2026-07-24 iv=0.316, forward=741.1, 33 slices).
- `Clock::from_surface_db(const SurfaceDb&)` (`atx-vol/include/atx/vol/backtest.hpp:80`, impl `backtest.cpp:1241`): one `SnapshotRef{date=partition key, archive_path=<root>/partitions/<KEY>.atxvsa}` per partition, sorted. Gate-tested (`atx-vol/tests/surface_db_backtest_test.cpp`).
- The whole dispersion stack consumes a `Clock` source-agnostically: `run_timed(const Clock&, DispersionUniverse, const DispersionBacktestConfig&)` (`backtest_driver.hpp:60`) → `DispersionStrategy` → `run_backtest` → `RunOutcome{BacktestResult, TearSheet, EngineRunStats}`.
- Absence tolerance exists: `dispersion_config_from` forces `MissingNameSpec{DropRenormalize, min_names}` (`src/dispersion_backtest.cpp:49`), and dropped names are reported per step (`DroppedName`/`DropReason::SurfaceNotFound`). Real DBs have clustered absences (2025-11-24 carries only 95/102 symbols) — the engine must drop-and-renormalize, never abort.
- `examples/mag7_dispersion_backtest.cpp` is the closest prior art (SurfaceDb → Clock → strangle DSL → run_timed → CSVs) — but it has **no date window**, uses the strangle DSL rather than classic dispersion, and has only ever run on synthetic throwaway DBs.

What is missing (the gaps this plan closes):

- **G1** No way to run a sub-window: `Clock::from_surface_db` always yields every partition.
- **G2** No universe source from the DB itself (mag7 takes `--names` on argv; the SP100 universe lives in the DB manifest).
- **G3** No file-based dispersion config (the strict `DispersionRunConfig` reader demands `opra_root`/definitions — corpus-route baggage).
- **G4** No entry point taking `(db, from, to, config)`; no example; no test ever pointed the dispersion stack at a production-shaped DB.
- **G5** Perf levers (`prefetch_depth`, `ArchiveBacking::Sealed` private cache, `n_threads=0`) not wired into any SurfaceDb-route example; no recorded baseline for a 100-name × 140-session run.

## Global Constraints

- **Everything under `C:\atx-data` is READ-ONLY.** Tests and examples may open production DBs for reading; nothing may ever write, create, or delete inside that tree. All outputs go to `%TEMP%` or a `--out` directory outside it.
- **No provider calls, no metered spend.** This sprint reads local disk only. The API key lives in `C:/atx/.env` — never read, log, echo, or persist it; nothing in this sprint needs it.
- **C++ builds ONLY via `scripts\atx-build.ps1`** (`dev` = Debug → `build\`, `rel` = Release → `build-rel\`). `-j` is unusable (binds `-Jobs`); use `--parallel N`. Invoke the script directly from PowerShell — never through a nested `powershell -Command` (the embedded `|` in the ctest regex becomes a real pipeline).
- **C++ test gate:** `.\scripts\atx-build.ps1 -Ctest -R "SurfaceDb|BuildSurfaceDb|GenerateSymbolConfigs|SurfaceArchive|OpraHive|SyntheticHive|Dispersion"` — name every new C++ test suite `SurfaceDbDispersion*` so both the `SurfaceDb` and `Dispersion` alternates match it. Baseline before this sprint: 315/315 (1 disabled) on the narrower regex.
- **Python tests:** run per-file (`python -m pytest <file> -q` from `atx-vol/python`); **never import `pyarrow` and `atxvol` in the same process** — follow the conventions of `atx-vol/python/tests/test_dispersion_runarchive_e2e.py`.
- **Bit-identity is a standing engine contract:** `run_backtest` output is bit-identical at any `n_threads`. Nothing in this sprint may break that; Task 5 asserts it.
- **Work on branch `feat/surface-db-dispersion-backtest` in a dedicated worktree.** Commit per task. NOTE: branch `feat/vol-v1-release` (unmerged) relocates dispersion headers to `atx-vol/research/include` (S4-T18); this plan targets `main` layout (`atx-vol/include/atx/vol/`). Do not rebase onto or merge that branch; a later integrator resolves the relocation.
- Exact date strings are ISO `YYYY-MM-DD`; lexicographic comparison IS chronological comparison for these keys (partition keys are canonical uppercase ISO dates).

## File Structure

- Create: `atx-vol/include/atx/vol/dispersion_surface_db.hpp` — the sprint's one public seam: `SurfaceDbDispersionSpec`, `read_dispersion_backtest_config`, `universe_from_surface_db`, `run_surface_db_dispersion_backtest`.
- Create: `atx-vol/src/dispersion_surface_db.cpp` — implementations.
- Create: `atx-vol/examples/surface_db_dispersion_backtest.cpp` — thin argv shell over the library function (mirrors mag7's shell-over-library shape).
- Create: `atx-vol/tests/surface_db_dispersion_backtest_test.cpp` — all C++ tests for Tasks 1-6.
- Create: `atx-vol/examples/sp100_dispersion_config.tsv` — worked config file.
- Create: `atx-vol/python/tests/test_surface_db_dispersion.py` — Python composition test (Task 7).
- Modify: `atx-vol/include/atx/vol/backtest.hpp` (+ `atx-vol/src/backtest.cpp`) — add `Clock::between`.
- Modify: `atx-vol/python/src/bindings/backtest.cpp` — bind `Clock.between`.
- Modify: `atx-vol/CMakeLists.txt` — new example target + new test file registration (follow the existing `mag7_dispersion_backtest` / `surface_db_backtest_test` registration patterns exactly).
- Create: `atx-vol/docs/surface-db-dispersion-backtest.md` — operator doc (Task 8).

Interfaces consumed throughout (verify signatures against headers before use; all exist on `main` today):

```cpp
// backtest.hpp
struct SnapshotRef { std::string date; std::string archive_path; };
static Result<Clock> Clock::from_surface_db(const SurfaceDb&);   // :80
std::span<const SnapshotRef> Clock::refs() const;                 // :82
// backtest_driver.hpp
Result<RunOutcome> run_timed(const Clock&, IStrategy&, const RunConfig& = {});                          // :54
Result<RunOutcome> run_timed(const Clock&, DispersionUniverse, const DispersionBacktestConfig& = {});   // :60
// dispersion_backtest.hpp
struct DispersionBacktestConfig;  // target_dte_days{30}, roll_dte_days{7}, gross_index_vega{10000},
                                  // delta_band{0}, min_names{2}, entry_every_n{21}, record_diagnostics,
                                  // RunConfig run{}, side, multiplier{100}, hedge_kind, hedge_cadence,
                                  // weighting, strike, limits
DispersionStrategy make_dispersion_backtest_strategy(std::vector<UniverseRow>, const DispersionBacktestConfig&, std::string_view index_symbol);  // :165
// dispersion.hpp
struct DispersionMember { std::string symbol; std::uint32_t uid; double weight; };
struct DispersionUniverse { DispersionMember index; std::vector<DispersionMember> names; };
// surface_db.hpp
static Result<SurfaceDb> SurfaceDb::open(std::string_view root, const SurfaceDbOpenOpts& = {});  // :565
std::vector<DbSymbolInfo> SurfaceDb::symbols() const;   // :577 — confirm exact element type; carries name + enabled
std::vector<DbPartitionInfo> SurfaceDb::partitions() const;  // :581
// run_report.hpp — CSV writers used by mag7: write_backtest_series_csv, write_metrics_csv,
//                  strategy_metrics, engine_metrics, MetaKv
```

Universe weighting decision (locked): the SP100 DB manifest stores no index weights, so the default universe is **equal-weight across all enabled non-index symbols** (`weight = 1.0 / n_names`). Weighted/PIT universes remain available via `--universe FILE` (existing `UniverseRow` TSV format + `make_pit_universe_resolver`). Do not invent a cap-weight source.

---

### Task 1: `Clock::between(date_lo, date_hi)`

**Files:**
- Modify: `atx-vol/include/atx/vol/backtest.hpp` (Clock class, next to `from_surface_db`)
- Modify: `atx-vol/src/backtest.cpp`
- Test: `atx-vol/tests/surface_db_dispersion_backtest_test.cpp` (new file, register in CMake alongside the existing test-registration block that lists `surface_db_backtest_test.cpp`)

**Interfaces:**
- Consumes: `Clock::refs()`, `SnapshotRef`.
- Produces: `Result<Clock> Clock::between(std::string_view date_lo, std::string_view date_hi) const` — every later task's window mechanism.

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-vol/tests/surface_db_dispersion_backtest_test.cpp
// Suite name SurfaceDbDispersionBacktest — matches both gate regex alternates.
// Build a tiny synthetic SurfaceDb exactly the way surface_db_backtest_test.cpp does
// (reuse its helper pattern: SurfaceDb::create at a %TEMP% root, write_partition with
// hand-built eSSVI surfaces). Factor a local make_test_db(root, dates, symbols) helper.

TEST(SurfaceDbDispersionBacktest, BetweenSelectsInclusiveWindow) {
  // db with partitions 2026-01-05, 2026-01-06, 2026-01-07, 2026-01-08
  const auto clock = Clock::from_surface_db(db);
  const auto sub = clock->between("2026-01-06", "2026-01-07");
  ASSERT_TRUE(sub);
  ASSERT_EQ(sub->refs().size(), 2u);
  EXPECT_EQ(sub->refs().front().date, "2026-01-06");
  EXPECT_EQ(sub->refs().back().date, "2026-01-07");
}

TEST(SurfaceDbDispersionBacktest, BetweenClampsToAvailableRange) {
  // asking for 2020-01-01 .. 2030-01-01 returns all four refs, not an error
  const auto sub = clock->between("2020-01-01", "2030-01-01");
  ASSERT_TRUE(sub);
  EXPECT_EQ(sub->refs().size(), 4u);
}

TEST(SurfaceDbDispersionBacktest, BetweenEmptyWindowIsInvalidArgument) {
  // a window between two real dates that contains no partition
  const auto sub = clock->between("2026-01-06T", "2026-01-06A");  // lo > hi lexicographically
  ASSERT_FALSE(sub);
  EXPECT_EQ(sub.error().code, ErrorCode::InvalidArgument);
  const auto gap = clock->between("2026-02-01", "2026-02-28");    // no partitions in window
  ASSERT_FALSE(gap);
  EXPECT_EQ(gap.error().code, ErrorCode::InvalidArgument);
  // error message must name the available range so the operator can self-serve:
  EXPECT_NE(gap.error().message.find("2026-01-05"), std::string::npos);
  EXPECT_NE(gap.error().message.find("2026-01-08"), std::string::npos);
}
```

- [ ] **Step 2: Run to verify failure**

```powershell
.\scripts\atx-build.ps1 dev --parallel 8
.\scripts\atx-build.ps1 -Ctest -R "SurfaceDbDispersionBacktest"
```
Expected: compile failure (`between` not a member) — that is the failing state for an API-addition test; register the test file in CMake first so the failure is the missing method, not a missing test binary.

- [ ] **Step 3: Implement**

```cpp
// backtest.hpp, inside class Clock, after from_surface_db:
  /// Subset this clock to refs whose date lies in [date_lo, date_hi] (inclusive,
  /// ISO YYYY-MM-DD; lexicographic == chronological for canonical keys).
  /// Out-of-range bounds clamp; a window containing no refs, or date_lo > date_hi,
  /// is InvalidArgument naming the available range.
  [[nodiscard]] Result<Clock> between(std::string_view date_lo, std::string_view date_hi) const;
```

```cpp
// backtest.cpp, next to from_surface_db's impl:
Result<Clock> Clock::between(std::string_view date_lo, std::string_view date_hi) const {
  if (date_lo > date_hi) {
    return Err(ErrorCode::InvalidArgument,
               fmt::format("Clock::between: date_lo '{}' > date_hi '{}' (available {}..{})",
                           date_lo, date_hi, refs_.empty() ? "" : refs_.front().date,
                           refs_.empty() ? "" : refs_.back().date));
  }
  std::vector<SnapshotRef> subset;
  for (const SnapshotRef& r : refs_) {
    if (r.date >= date_lo && r.date <= date_hi) subset.push_back(r);
  }
  if (subset.empty()) {
    return Err(ErrorCode::InvalidArgument,
               fmt::format("Clock::between: no snapshots in [{}, {}] (available {}..{})",
                           date_lo, date_hi, refs_.empty() ? "" : refs_.front().date,
                           refs_.empty() ? "" : refs_.back().date));
  }
  Clock out;
  out.refs_ = std::move(subset);
  return out;
}
```
Match the file's actual error-construction idiom (`Err(...)`/`fmt::format` vs whatever `from_surface_db` uses at `backtest.cpp:1241` — copy that idiom exactly; if the codebase uses `std::format` or a local helper, use that).

- [ ] **Step 4: Run tests to verify pass** — same ctest command, all 3 green.

- [ ] **Step 5: Commit**

```bash
git add atx-vol/include/atx/vol/backtest.hpp atx-vol/src/backtest.cpp atx-vol/tests/surface_db_dispersion_backtest_test.cpp atx-vol/CMakeLists.txt
git commit -m "feat(vol): Clock::between date-window subset for backtest clocks"
```

---

### Task 2: dispersion config file reader

**Files:**
- Create: `atx-vol/include/atx/vol/dispersion_surface_db.hpp`
- Create: `atx-vol/src/dispersion_surface_db.cpp` (register in the library-sources list in `atx-vol/CMakeLists.txt` next to `dispersion_backtest.cpp`)
- Create: `atx-vol/examples/sp100_dispersion_config.tsv`
- Test: `atx-vol/tests/surface_db_dispersion_backtest_test.cpp`

**Interfaces:**
- Consumes: `DispersionBacktestConfig` (`dispersion_backtest.hpp:68`), `DispersionSide`, `WeightingScheme`, `StrikeRule`, `HedgeSpec` enums.
- Produces: `Result<DispersionBacktestConfig> read_dispersion_backtest_config(const std::filesystem::path&)` — consumed by Tasks 4 and 8.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(SurfaceDbDispersionBacktest, ConfigReaderDefaultsAndOverrides) {
  // A file setting a subset of keys; everything else must equal a default-constructed
  // DispersionBacktestConfig field-for-field.
  const auto path = write_temp_file(
      "target_dte_days\t45\n"
      "gross_index_vega\t25000\n"
      "min_names\t60\n"
      "side\tshort_index_long_names\n"
      "weighting\tvega_neutral\n"
      "strike_rule\tatm_forward_straddle\n");
  const auto cfg = read_dispersion_backtest_config(path);
  ASSERT_TRUE(cfg);
  EXPECT_DOUBLE_EQ(cfg->target_dte_days, 45.0);
  EXPECT_DOUBLE_EQ(cfg->gross_index_vega, 25000.0);
  EXPECT_EQ(cfg->min_names, 60u);
  const DispersionBacktestConfig defaults{};
  EXPECT_DOUBLE_EQ(cfg->roll_dte_days, defaults.roll_dte_days);
  EXPECT_EQ(cfg->entry_every_n, defaults.entry_every_n);
}

TEST(SurfaceDbDispersionBacktest, ConfigReaderRejectsUnknownKeyAndBadValue) {
  auto bad_key = read_dispersion_backtest_config(write_temp_file("target_dte_dayz\t45\n"));
  ASSERT_FALSE(bad_key);   // typo safety: unknown key is an error naming the key
  EXPECT_NE(bad_key.error().message.find("target_dte_dayz"), std::string::npos);
  auto bad_val = read_dispersion_backtest_config(write_temp_file("min_names\tmany\n"));
  ASSERT_FALSE(bad_val);
  auto bad_enum = read_dispersion_backtest_config(write_temp_file("side\tsideways\n"));
  ASSERT_FALSE(bad_enum);
}
```

- [ ] **Step 2: Run to verify fail** (compile failure: header does not exist yet).

- [ ] **Step 3: Implement**

```cpp
// dispersion_surface_db.hpp
#pragma once
#include <filesystem>
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/result.hpp"   // match the include the sibling headers use for Result

namespace atx::vol {

/// Parse a key<TAB>value dispersion config. Every key optional; absent keys keep
/// DispersionBacktestConfig defaults. Unknown key, unparsable value, or bad enum
/// token is InvalidArgument naming the offender. Blank lines and lines starting
/// with '#' are ignored.
///
/// Keys: target_dte_days, roll_dte_days, gross_index_vega, delta_band, min_names,
///       entry_every_n, record_diagnostics (0/1), multiplier,
///       side (short_index_long_names|long_index_short_names),
///       weighting (vega_neutral|equal_vega|gamma_neutral|theta_neutral),
///       strike_rule (atm_forward_straddle|fixed_moneyness|delta_strangle),
///       log_moneyness, target_abs_delta,
///       hedge_kind (none|delta_to_zero), hedge_cadence (at_entry|daily),
///       half_spread_bps, per_contract_cost, n_threads, prefetch_depth
[[nodiscard]] Result<DispersionBacktestConfig>
read_dispersion_backtest_config(const std::filesystem::path& path);

}  // namespace atx::vol
```

Implementation: a flat loop over lines, split on first `\t`, dispatch on key via if-chain (match the parsing idiom of `read_run_spec` in `src/dispersion_workflow.cpp` — same TSV-key-value family; reuse its number-parsing helper if it has one). `half_spread_bps`/`per_contract_cost` land in `config.run.frictions`; `n_threads` in `config.run.price.n_threads`; `prefetch_depth` in `config.run.prefetch_depth`.

```tsv
# atx-vol/examples/sp100_dispersion_config.tsv — worked SP100 config
target_dte_days	30
roll_dte_days	7
gross_index_vega	10000
min_names	60
entry_every_n	21
side	short_index_long_names
weighting	vega_neutral
strike_rule	atm_forward_straddle
record_diagnostics	1
n_threads	0
prefetch_depth	2
```

- [ ] **Step 4: Run tests to verify pass.**
- [ ] **Step 5: Commit** — `feat(vol): file reader for DispersionBacktestConfig (surface-db route)`

---

### Task 3: universe from the DB manifest

**Files:**
- Modify: `atx-vol/include/atx/vol/dispersion_surface_db.hpp`, `atx-vol/src/dispersion_surface_db.cpp`
- Test: `atx-vol/tests/surface_db_dispersion_backtest_test.cpp`

**Interfaces:**
- Consumes: `SurfaceDb::symbols()` (enabled flag per symbol), `DispersionUniverse`/`DispersionMember` (`dispersion.hpp:61-68`).
- Produces: `Result<DispersionUniverse> universe_from_surface_db(const SurfaceDb&, std::string_view index_symbol)` — consumed by Task 4.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(SurfaceDbDispersionBacktest, UniverseFromDbEqualWeightsExcludesIndexAndDisabled) {
  // db manifest: SPY + A, B, C enabled, D disabled
  const auto u = universe_from_surface_db(db, "SPY");
  ASSERT_TRUE(u);
  EXPECT_EQ(u->index.symbol, "SPY");
  ASSERT_EQ(u->names.size(), 3u);                       // A, B, C — no SPY, no D
  for (const auto& m : u->names) EXPECT_DOUBLE_EQ(m.weight, 1.0 / 3.0);
  // uid fields are 0 here: uids are snapshot-local and rebound per step by
  // resolve_universe_uids via MarketSnapshot::uid_of — assert that contract:
  EXPECT_EQ(u->index.uid, 0u);
}

TEST(SurfaceDbDispersionBacktest, UniverseFromDbMissingIndexIsInvalidArgument) {
  const auto u = universe_from_surface_db(db, "QQQ");   // not in manifest
  ASSERT_FALSE(u);
  EXPECT_EQ(u.error().code, ErrorCode::InvalidArgument);
  EXPECT_NE(u.error().message.find("QQQ"), std::string::npos);
}
```

- [ ] **Step 2: Run to verify fail.**

- [ ] **Step 3: Implement** — iterate `db.symbols()`; skip disabled; the entry equal to `index_symbol` becomes `universe.index` (weight 1.0); every other enabled symbol becomes a name; after the loop set each name weight to `1.0 / names.size()`; if the index was never seen return InvalidArgument naming it and the manifest size. Names sorted by symbol for determinism (manifest order is already canonical — keep whichever the manifest guarantees, and assert sortedness in the test if so).

- [ ] **Step 4: Run tests to verify pass.**
- [ ] **Step 5: Commit** — `feat(vol): derive equal-weight dispersion universe from a SurfaceDb manifest`

---

### Task 4: the one-call entry point + example binary

**Files:**
- Modify: `atx-vol/include/atx/vol/dispersion_surface_db.hpp`, `atx-vol/src/dispersion_surface_db.cpp`
- Create: `atx-vol/examples/surface_db_dispersion_backtest.cpp` (CMake target `surface_db_dispersion_backtest`, registered exactly like `mag7_dispersion_backtest` at `CMakeLists.txt:419`)
- Test: `atx-vol/tests/surface_db_dispersion_backtest_test.cpp`

**Interfaces:**
- Consumes: Tasks 1-3 outputs; `run_timed` (both overloads); `make_dispersion_backtest_strategy(std::vector<UniverseRow>, cfg, index_symbol)`; `read_universe` (`dispersion_workflow.hpp:69`); CSV writers from `run_report.hpp` (`write_backtest_series_csv`, `write_metrics_csv`, `strategy_metrics`, `engine_metrics`, `MetaKv`).
- Produces:

```cpp
struct SurfaceDbDispersionSpec {
  std::string db_root;                                // required
  std::string date_lo;                                // required, ISO inclusive
  std::string date_hi;                                // required, ISO inclusive
  std::string index_symbol{"SPY"};
  std::optional<std::filesystem::path> universe_path; // optional UniverseRow TSV (PIT schedule)
  DispersionBacktestConfig config{};                  // from Task 2 reader or defaults
};
[[nodiscard]] Result<RunOutcome>
run_surface_db_dispersion_backtest(const SurfaceDbDispersionSpec& spec);
```

- [ ] **Step 1: Write the failing E2E test**

```cpp
TEST(SurfaceDbDispersionBacktest, EndToEndOnSyntheticDbWindow) {
  // db: SPY + 4 names, 6 partitions 2026-01-05..2026-01-12 (business days)
  SurfaceDbDispersionSpec spec;
  spec.db_root = root;
  spec.date_lo = "2026-01-06";
  spec.date_hi = "2026-01-09";
  spec.config.min_names = 2;
  spec.config.entry_every_n = 1;
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->result.size(), 4u);                  // exactly the window's sessions
  EXPECT_EQ(out->result.date.front(), "2026-01-06");
  EXPECT_EQ(out->result.date.back(), "2026-01-09");
  for (double nav : out->result.nav) EXPECT_TRUE(std::isfinite(nav));
  EXPECT_GT(out->stats.n_steps, 0u);
}

TEST(SurfaceDbDispersionBacktest, EndToEndRejectsEmptyWindowWithAvailableRange) {
  spec.date_lo = "2027-01-01"; spec.date_hi = "2027-02-01";
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_FALSE(out);
  EXPECT_NE(out.error().message.find("2026-01-05"), std::string::npos);
}
```

- [ ] **Step 2: Run to verify fail.**

- [ ] **Step 3: Implement the library function**

```cpp
Result<RunOutcome> run_surface_db_dispersion_backtest(const SurfaceDbDispersionSpec& spec) {
  auto db = SurfaceDb::open(spec.db_root);
  if (!db) return Err(db.error());
  auto full = Clock::from_surface_db(*db);
  if (!full) return Err(full.error());
  auto clock = full->between(spec.date_lo, spec.date_hi);
  if (!clock) return Err(clock.error());

  if (spec.universe_path) {                       // weighted / PIT route
    auto rows = read_universe(*spec.universe_path);
    if (!rows) return Err(rows.error());
    auto strat = make_dispersion_backtest_strategy(std::move(*rows), spec.config,
                                                   spec.index_symbol);
    return run_timed(*clock, strat, spec.config.run);
  }
  auto universe = universe_from_surface_db(*db, spec.index_symbol);   // equal-weight route
  if (!universe) return Err(universe.error());
  return run_timed(*clock, std::move(*universe), spec.config);
}
```
Check `make_dispersion_backtest_strategy`'s exact return type (`DispersionStrategy` by value) and pass it to the `IStrategy&` overload of `run_timed`. Do NOT pass a shared `SnapshotCache` in `spec.config.run` — leaving `snapshot_cache` null selects the engine's private cache, which is what enables `ArchiveBacking::Sealed` mmap and `referenced_uids` subsetting (see `backtest.cpp:2283-2293`); this is the perf-correct default for this route and Task 6 verifies it.

- [ ] **Step 4: Implement the example shell** (thin; every branch already tested at the library layer)

```
surface_db_dispersion_backtest --db DIR --from YYYY-MM-DD --to YYYY-MM-DD
    [--config FILE] [--out DIR] [--index SPY] [--universe FILE]
```
Parse argv exactly in the style of `mag7_dispersion_backtest.cpp:83-153` (`Args` struct + `parse_args`). `--config` routes through `read_dispersion_backtest_config`. On success write, into `--out` (default `%TEMP%/atx-surface-db-dispersion`): `series.csv`, `strategy_metrics.csv`, `engine_metrics.csv` via the same `run_report.hpp` writers and `MetaKv` block mag7 uses (`data_source=surface_db`, `db_root`, `db_generation`, `window=<lo>..<hi>`, `index=<sym>`, `n_names`), and print the headline (window, steps, total PnL, sharpe, max drawdown, wall-clock ms, snapshot-cache stats) to stdout. Exit 0 on success, 1 on runtime error, 2 on usage error.

- [ ] **Step 5: Run tests to verify pass; build + run the example against the synthetic DB from the test fixture manually once** (write the fixture to `%TEMP%`, run the binary, eyeball the CSVs).

- [ ] **Step 6: Commit** — `feat(vol): surface-db dispersion backtest entry point + example CLI`

---

### Task 5: correctness on production-shaped data

**Files:**
- Test: `atx-vol/tests/surface_db_dispersion_backtest_test.cpp`

**Interfaces:** consumes everything above; produces no new API — this task is the evidence layer.

**Fixture-realism mandate (binding, learned the hard way in the surface-db sprint):** absence fixtures must be *clustered* like the real DBs (one session missing many names at once — the real 2025-11-24 misses 12 of 102), never spread uniformly. A uniform fixture made two prior defects invisible.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(SurfaceDbDispersionBacktest, ClusteredAbsenceDropsAndRenormalizes) {
  // 5 names + SPY, 4 partitions; partition 3 written WITHOUT names D and E
  // (write_partition simply omits them — absence == not in the archive directory).
  spec.config.min_names = 2;                  // 3 survivors >= 2 → run continues
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->result.size(), 4u);          // absent session still produces a row
  // diagnostics record the drop on that step (record_diagnostics=1):
  // assert via the signals/diagnostics channel that step 2 carries 2 dropped names
  // with DropReason::SurfaceNotFound — locate the exact accessor on BacktestResult
  // (signals[] / DispersionSignal::dropped) and pin it.
}

TEST(SurfaceDbDispersionBacktest, ClusteredAbsenceBelowMinNamesFailsLoudly) {
  spec.config.min_names = 4;                  // 3 survivors < 4 → the step must error,
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_FALSE(out);                          // not silently skip
}

TEST(SurfaceDbDispersionBacktest, MissingIndexOnOneDateFailsLoudly) {
  // partition 2 written without SPY: an index-less dispersion step is meaningless
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_FALSE(out);
  // pin whatever error the stack produces (SurfaceNotFound for the index uid) so a
  // future refactor cannot turn it into a silent drop.
}

TEST(SurfaceDbDispersionBacktest, BitIdenticalAcrossThreadCounts) {
  auto one = spec;  one.config.run.price.n_threads = 1;
  auto many = spec; many.config.run.price.n_threads = 0;   // all cores
  const auto a = run_surface_db_dispersion_backtest(one);
  const auto b = run_surface_db_dispersion_backtest(many);
  ASSERT_TRUE(a); ASSERT_TRUE(b);
  ASSERT_EQ(a->result.size(), b->result.size());
  for (std::size_t i = 0; i < a->result.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.pnl_total[i]),
              std::bit_cast<std::uint64_t>(b->result.pnl_total[i]));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.nav[i]),
              std::bit_cast<std::uint64_t>(b->result.nav[i]));
  }
}

TEST(SurfaceDbDispersionBacktest, WindowSubsetMatchesFullRunPrefix) {
  // Determinism of windowing itself: a run over [d1..d4] and a run over [d1..d2]
  // must agree bit-for-bit on the shared prefix. Guards against window-dependent
  // state leaking into early steps.
  auto full = spec;  full.date_lo = "2026-01-05"; full.date_hi = "2026-01-12";
  auto sub  = spec;  sub.date_lo  = "2026-01-05"; sub.date_hi  = "2026-01-08";
  const auto a = run_surface_db_dispersion_backtest(full);
  const auto b = run_surface_db_dispersion_backtest(sub);
  ASSERT_TRUE(a); ASSERT_TRUE(b);
  ASSERT_LT(b->result.size(), a->result.size());
  for (std::size_t i = 0; i < b->result.size(); ++i) {
    EXPECT_EQ(a->result.date[i], b->result.date[i]);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.nav[i]),
              std::bit_cast<std::uint64_t>(b->result.nav[i]));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->result.pnl_total[i]),
              std::bit_cast<std::uint64_t>(b->result.pnl_total[i]));
  }
}
```

- [ ] **Step 2: Run to verify fail** (the diagnostics accessor and error pins will genuinely fail until located/asserted correctly; the bit-identity and prefix tests should pass immediately — for those two, verify they FAIL when sabotaged: temporarily flip the prefix test to compare mismatched indices, watch it fail, restore. State in the report which tests were sabotage-verified rather than red-green.)

- [ ] **Step 3: Fix anything these tests flush out.** Expected soft spot: whether a below-`min_names` step errors the whole run or halts — pin whichever the engine actually does, and if it silently continues with an empty book, that is a defect: fix at the `DispersionStrategy`/`build_dispersion_book` error-propagation level, not by loosening the test.

- [ ] **Step 4: All green; commit** — `test(vol): production-shaped correctness pins for the surface-db dispersion route`

---

### Task 6: performance — wire the levers, record a baseline

**Files:**
- Test: `atx-vol/tests/surface_db_dispersion_backtest_test.cpp`
- Modify (only if a lever is found unwired): `atx-vol/src/dispersion_surface_db.cpp`

**Interfaces:** consumes `EngineRunStats` (`run_report.hpp:88` — `wall_clock_ms`, `n_steps`, `SnapshotCacheStats cache`).

- [ ] **Step 1: Write the lever-verification test**

```cpp
TEST(SurfaceDbDispersionBacktest, PrivateCachePathIsUsed) {
  // spec.config.run.snapshot_cache deliberately left null.
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out);
  // EngineRunStats.cache must show hits/loads consistent with a private bounded
  // cache + prefetch (loads == n_steps, no re-loads). Pin the exact fields.
  EXPECT_EQ(out->stats.cache.loads, out->stats.n_steps);
}
```

- [ ] **Step 2: Sanity-check the Sealed/mmap route is actually taken.** Read `backtest.cpp:2283-2293` (private-cache construction) and confirm the conditions under which `ArchiveBacking::Sealed` is selected apply to this route (no shared cache, default `QueryCacheBuildPolicy`). If Sealed is NOT selected by default, wire it: the fix belongs in `run_surface_db_dispersion_backtest` (set the documented `RunConfig` knob), never inside the engine. Record in the report which case held, with the file:line evidence.

- [ ] **Step 3: Benchmark against the real DB, read-only, env-gated**

```cpp
TEST(SurfaceDbDispersionBacktest, RealSp100Baseline) {
  const char* root = std::getenv("ATX_SP100_SURFACE_DB");   // e.g. C:/atx-data/surface-db/sp100-2026
  if (!root) GTEST_SKIP() << "set ATX_SP100_SURFACE_DB to run";
  SurfaceDbDispersionSpec spec;
  spec.db_root = root;
  spec.date_lo = "2026-01-02"; spec.date_hi = "2026-07-24";
  spec.config.min_names = 60;
  spec.config.entry_every_n = 21;
  const auto out = run_surface_db_dispersion_backtest(spec);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_GE(out->result.size(), 100u);
  for (double nav : out->result.nav) EXPECT_TRUE(std::isfinite(nav));
  std::cerr << "[baseline] steps=" << out->stats.n_steps
            << " wall_ms=" << out->stats.wall_clock_ms << "\n";
  // No hard wall-clock assertion — machine-dependent. The NUMBER goes in the report.
}
```
Run it in `rel` (`.\scripts\atx-build.ps1 rel --parallel 8`, then the rel ctest with `ATX_SP100_SURFACE_DB` set) — a Debug baseline number is meaningless. Record in the report: wall-clock for the ~140-session window at `n_threads=0`, `prefetch_depth` 1 vs 2, and the snapshot-load share from `engine_metrics`/profile phases. If wall-clock exceeds ~2 s per 100 sessions in Release, profile before merging (`ATX_VOL_PROFILE` env → `backtest_profile.tsv`) and report where the time went — do not optimize blind and do not touch engine internals in this sprint.

- [ ] **Step 4: Commit** — `perf(vol): verify cache/prefetch levers + record sp100 dispersion baseline`

---

### Task 7: Python parity

**Files:**
- Modify: `atx-vol/python/src/bindings/backtest.cpp` (bind `Clock.between` next to the existing `from_surface_db` staticmethod at `:118-126`)
- Test: `atx-vol/python/tests/test_surface_db_dispersion.py`

**Interfaces:**
- Consumes: existing bindings — `SurfaceDb` (create/open/write_partition, `bindings/surface_db.cpp:496-550`), `Clock.from_surface_db` (`bindings/backtest.cpp:118-126`), `run_dispersion_backtest(clock, universe, config)` (`bindings/dispersion.cpp:219-227`).
- Produces: `Clock.between(date_lo, date_hi)` in Python; the first test anywhere of the `from_surface_db → run_dispersion_backtest` composition.

- [ ] **Step 1: Write the failing test** — build a synthetic SurfaceDb via the Python bindings (mirror whatever `python/tests` fixture already builds surfaces via `atxvol`; follow `test_dispersion_runarchive_e2e.py` conventions for skips/fixtures), then:

```python
def test_surface_db_clock_between_and_dispersion_run(synthetic_db_root):
    import atxvol as av
    db = av.SurfaceDb.open(str(synthetic_db_root))
    clock = av.Clock.from_surface_db(db).between("2026-01-06", "2026-01-09")
    names = ["NM0", "NM1", "NM2"]                      # the fixture's non-index symbols
    universe = av.DispersionUniverse(
        index=av.DispersionMember(symbol="SPY", uid=0, weight=1.0),
        names=[av.DispersionMember(symbol=s, uid=0, weight=1.0 / len(names))
               for s in names])
    # (verify the bound constructor shape in bindings/dispersion.cpp before use —
    # if the binding exposes field assignment rather than kwargs, build it that way)
    cfg = av.DispersionBacktestConfig()
    cfg.min_names = 2
    result = av.run_dispersion_backtest(clock, universe, cfg)
    assert len(result.date) == 4
    assert all(math.isfinite(x) for x in result.nav)

def test_clock_between_empty_window_raises(synthetic_db_root):
    ...  # pytest.raises, message carries the available range
```
Confirm the exact bound names (`av.SurfaceDb`, `av.DispersionBacktestConfig`, `av.run_dispersion_backtest`) from the binding sources before writing — do not guess spellings.

- [ ] **Step 2: Run to verify fail** (`between` unbound → AttributeError). `python -m pytest atx-vol/python/tests/test_surface_db_dispersion.py -q`. Never import `pyarrow` in this file.

- [ ] **Step 3: Bind** — one `.def("between", &Clock::between, ...)` following the file's existing Result-unwrapping convention for throwing on error. Rebuild the wheel the way the repo's python build does (see `atx-vol/python/CMakeLists` / existing build dir `cp312-cp312-win_amd64`).

- [ ] **Step 4: Run tests to verify pass.**
- [ ] **Step 5: Commit** — `feat(vol): bind Clock.between; first py test of surface-db → dispersion composition`

---

### Task 8: operator doc + worked real-data example

**Files:**
- Create: `atx-vol/docs/surface-db-dispersion-backtest.md`
- Modify: `atx-vol/docs/surface-db-build.md` (one cross-link line in its "consumers" / intro section)

**Interfaces:** consumes everything; produces the document a fresh operator follows.

- [ ] **Step 1: Write the doc.** Sections, each with exact commands:
  1. **What this runs** — one paragraph: index-vs-names dispersion off fitted surfaces; the three inputs (window, config file, DB root); what the outputs are.
  2. **Quickstart** (worked, real):
     ```powershell
     .\scripts\atx-build.ps1 rel --parallel 8
     .\build-rel\bin\surface_db_dispersion_backtest.exe `
         --db C:\atx-data\surface-db\sp100-2026 `
         --from 2026-01-02 --to 2026-07-24 `
         --config atx-vol\examples\sp100_dispersion_config.tsv `
         --out %TEMP%\sp100-dispersion
     ```
     State plainly: the DB is opened read-only; the run writes nothing under `C:\atx-data`; outputs land in `--out`.
  3. **Config reference** — every key from Task 2, its type, default, and enum tokens. Numbers copied from the shipped `DispersionBacktestConfig` defaults, not from memory.
  4. **Universe** — default equal-weight-from-manifest behavior, `--index`, and the `--universe` TSV escape hatch (`UniverseRow` columns, PIT semantics).
  5. **Absence semantics** — real DBs have absent cells (worst real session 2025-11-24: 95/102 present); drop-and-renormalize vs `min_names`; missing *index* is always fatal.
  6. **Performance** — the recorded Task 6 baseline (machine-stamped), the two knobs (`n_threads`, `prefetch_depth`), and the pointer to `ATX_VOL_PROFILE`.
  7. **Limits** — what this route is NOT: no listed-contract execution realism (that is the corpus/listed route with definitions + OCC-ESS evidence); marks are model marks off fitted surfaces.
- [ ] **Step 2: Every number and command in the doc executed or read from disk before being written down.** A doc number that cannot be reproduced is left out, not guessed — quoted-number drift caused a full fix round in the previous sprint.
- [ ] **Step 3: Commit** — `docs(vol): operator guide for the surface-db dispersion backtest route`

---

## Final gates (run by the controller before merge)

1. C++ gate: `.\scripts\atx-build.ps1 -Ctest -R "SurfaceDb|BuildSurfaceDb|GenerateSymbolConfigs|SurfaceArchive|OpraHive|SyntheticHive|Dispersion"` — every pre-existing test still green (baseline 315/315 + all pre-existing Dispersion* suites) plus all new `SurfaceDbDispersionBacktest` tests.
2. Python: new test file green; the four surface-db sprint-lane files still green.
3. Env-gated real-DB test run once in `rel` with `ATX_SP100_SURFACE_DB=C:/atx-data/surface-db/sp100-2026`; baseline number recorded in the final report.
4. `git diff --name-only` shows no file under any path that writes to `C:\atx-data`; the production roots' mtimes unchanged.
5. Example binary run end-to-end against the real 2026 DB, output eyeballed, headline numbers in the final report.

## Out of scope

- The listed/corpus route (`spy_dispersion_backtest`, definitions, OCC-ESS) — untouched.
- Changing engine internals (`run_backtest`, `compute_step`, `SnapshotCache`) — the one engine-adjacent change is the additive `Clock::between`.
- `SurfaceDb`'s own partition-open strategy (`open_file` vs `open_mapped` inside `SurfaceDb::cached_partition`) — the backtest route bypasses `SurfaceDb`'s cache entirely (engine loads `.atxvsa` paths directly), so this is irrelevant here; leave it.
- Cap-weighted SP100 universes (no weight source exists in the DB manifest; the `--universe` TSV covers it when someone builds one).
- The `feat/vol-v1-release` header relocation — integrator's problem, later.
