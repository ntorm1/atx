# Surface DB Production Feature Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Production surface-database feature: date-partitioned OPRA hive v2 + C++ loader, auto per-symbol fit-config generation, one-call `build_surface_db` driver (C++ API + CLI + Python), migration/pull tooling, and the built 2026-07 production dataset (≤ $100 Databento spend).

**Architecture:** Reuse the blessed stack end-to-end: `load_opra_cbbo_parquet` grows an in-memory-table seam; a new `load_opra_hive` reads one parquet per date and fans out per-underlying panels through that seam; `generate_symbol_configs` maps `select_fit_policy`/`select_curve` decisions into manifest-stored `SymbolFitConfig`s; `build_surface_db` chains load → configs → `populate_universe_streaming` (already cell-aware resumable). Python tools migrate the existing per-symbol hive ($0) and pull only missing cells.

**Tech Stack:** C++23 (clang-cl + Ninja via `scripts/atx-build.ps1`), atx-core `io::read_parquet`/`io::write_parquet`/`io::write_hive_parquet`, GTest, pybind11 (`atxvol`), Python 3 + pyarrow + databento, pytest.

**Spec:** `docs/superpowers/specs/2026-07-22-surface-db-production-design.md` — read it first.

## Global Constraints

- Work in the dedicated worktree branch `feat/surface-db-prod` off local `main`. ONE C++ build slot: never run two cmake/ninja builds concurrently (Windows link contention). Python-only tasks may run in parallel with a C++ build.
- Configure once per worktree: `pwsh scripts/atx-build.ps1 configure`. Build: `pwsh scripts/atx-build.ps1 build atx-vol-tests`. Test: `pwsh scripts/atx-build.ps1 -Ctest -R <TestRegex>` (foreground, serial; never background a build).
- Commits: explicit paths only (`git add <files>` / `git commit -- <files>`), never `git add -A` (the tree carries unrelated WIP). No stash. Conventional-commit subjects. Hooks rewrite git→rtk transparently; do not bypass.
- Hive v2 layout is FROZEN by the spec: `<root>/date=YYYY-MM-DD/data.parquet`, 8 columns `ts(timestamp[ns]), underlying(str), symbol(str), instrument_id(i64), bid_px(i64), ask_px(i64), bid_sz(i64), ask_sz(i64)`, px 1e-9 fixed-point, unset side = INT64_MIN, snapshot minute fixed `T19:55:00Z`.
- Determinism invariants hold everywhere: byte-identical results for any thread count; missing dates non-fatal; fail-closed on schema drift.
- Header doc-comment discipline mirrors `surface_db.hpp`: every new public API documents contract, errors, thread-safety.
- Real-money commands (Task 10 only): free `get_cost` preflight before ANY paid call; hard `--cap`; `--dry-run` evidence logged before each paid phase; cumulative spend tracked in the run report. Total budget $100.

---

### Task 1: `load_opra_cbbo_from_table` seam (opra_panel refactor)

**Files:**
- Modify: `atx-vol/include/atx/vol/opra_panel.hpp` (add one declaration + doc-comment near `load_opra_cbbo_parquet`, line ~220)
- Modify: `atx-vol/src/opra_panel.cpp` (extract table-driven core)
- Test: `atx-vol/tests/opra_panel_test.cpp` (append one test)

**Interfaces:**
- Consumes: `atx::core::io::ParquetTable` (`atx/core/io/parquet.hpp`), existing `OpraLoadSpec`.
- Produces: `Result<OpraPanel> load_opra_cbbo_from_table(const atx::core::io::ParquetTable &table, const OpraLoadSpec &spec);` — byte-identical panel to `load_opra_cbbo_parquet(spec)` when `table` holds the same rows the file path would read. `spec.path` is used only for error messages. Task 3 depends on this exact signature.

- [ ] **Step 1: Write the failing test.** In `opra_panel_test.cpp`, locate an existing test that loads a real/synthetic parquet fixture through `load_opra_cbbo_parquet` (reuse its fixture path + spec). Add:

```cpp
TEST(OpraPanelTable, TableSeamMatchesFileLoad) {
  const OpraLoadSpec spec = /* same spec an existing passing load test uses */;
  auto file_panel = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(file_panel.has_value()) << file_panel.error().message;
  auto table = atx::core::io::read_parquet(spec.path);
  ASSERT_TRUE(table.has_value());
  auto tbl_panel = load_opra_cbbo_from_table(*table, spec);
  ASSERT_TRUE(tbl_panel.has_value()) << tbl_panel.error().message;
  EXPECT_EQ(tbl_panel->quotes().size(), file_panel->quotes().size());
  EXPECT_EQ(tbl_panel->spot(), file_panel->spot());       // adapt accessors to OpraPanel's API
  EXPECT_EQ(tbl_panel->snapshot_ts_ns(), file_panel->snapshot_ts_ns());
}
```

Adapt the equality block to `OpraPanel`'s real accessors (read `opra_panel.hpp`); compare every cheap identity the panel exposes (row count, spot, snapshot ts, first/last quote fields, fingerprint if present). If `OpraPanel` has `operator==`, use it.

- [ ] **Step 2:** `pwsh scripts/atx-build.ps1 build atx-vol-tests` → expect compile FAIL (`load_opra_cbbo_from_table` undeclared).
- [ ] **Step 3: Implement.** In `opra_panel.cpp`, the current `load_opra_cbbo_parquet` (~line 400) does: `LazyParquet::scan` (projection/pushdown) or `read_parquet(path, projection)`, then column extraction → row filtering (underlying) → OSI parse → PCP spot implication → panel assembly. Split it: everything AFTER the table is materialized moves into a new internal `panel_from_table(const io::ParquetTable&, const OpraLoadSpec&)`; the public file loader keeps its read strategy and delegates; the new public `load_opra_cbbo_from_table` validates the table has the 8 required columns (missing column → `InvalidArgument` naming the column and `spec.path`) then delegates to the same core. NO behavior change on the file path — this is a pure extraction. If the LazyParquet fast path filters rows *during* scan, keep that fast path in the file loader and make the shared core accept pre-filtered or unfiltered rows uniformly (the core re-applies the `underlying` filter; filtering twice is idempotent).
- [ ] **Step 4:** `pwsh scripts/atx-build.ps1 build atx-vol-tests` then `pwsh scripts/atx-build.ps1 -Ctest -R OpraPanel` → all pass (old + new).
- [ ] **Step 5: Commit** `feat(vol): in-memory table seam for OPRA cbbo loader` with the three files explicitly.

### Task 2: Synthetic multi-symbol hive fixture (test support)

**Files:**
- Create: `atx-vol/tests/support/synthetic_opra_hive.hpp`
- Test: `atx-vol/tests/opra_hive_test.cpp` (created here with fixture self-tests; grows in Task 3)
- Modify: `atx-vol/tests/CMakeLists.txt` (add `opra_hive_test.cpp` to the source list)

**Interfaces:**
- Consumes: `atx::core::io::write_parquet`, `write_hive_parquet`, `WriteColumn` (`atx/core/io/parquet_writer.hpp`), `atx::core::time::Timestamp`.
- Produces (Tasks 3/5 depend on these exact signatures):

```cpp
namespace atx::vol::testsupport {
struct SyntheticHiveSpec {
  std::vector<std::string> symbols{{"AAA", "BBB", "CCC"}};
  std::vector<std::string> dates{{"2026-07-01", "2026-07-02", "2026-07-06"}};
  double spot{100.0};
  double r{0.03};
};
// Writes NEW layout: <root>/date=<d>/data.parquet (all symbols per file).
void write_synthetic_hive_v2(const std::filesystem::path &root, const SyntheticHiveSpec &spec);
// Writes OLD layout: <root>/<symbol>/<date>.parquet (parity/migration tests).
void write_synthetic_hive_v1(const std::filesystem::path &root, const SyntheticHiveSpec &spec);
}
```

- [ ] **Step 1: Write fixture + self-test.** Row generation (shared by both writers): per (symbol, date), 9 strikes K ∈ {80,85,…,120}, 2 expiries at trade date + 28d and + 56d, both C and P per strike (PCP pairs so the loader can imply spot). Mid price from Black european with flat r, smile vol `0.25 + 0.02*((K/S)-1)^2` (self-contained ~15-line `black_price` helper with an `erfc`-based normal CDF — do NOT include atx pricing headers into test support). `bid = 0.98*mid`, `ask = 1.02*mid`, px stored as `int64(round(px*1e9))`, sizes 10. Symbol column = OSI: root left-padded to 6 chars + `YYMMDD` + `C|P` + 8-digit strike*1000 (e.g. `"AAA   260729C00100000"`). `ts` = `<date>T19:55:00Z` for every row. `underlying` = the plain symbol. `instrument_id` = running counter. v2 writer: assemble ALL symbols' rows for one date sorted (underlying, symbol), add a `date` string column, call `write_hive_parquet(cols, root, "date")`. v1 writer: per symbol call `write_parquet` to `<root>/<symbol>/<date>.parquet`. Self-tests in `opra_hive_test.cpp`:

```cpp
TEST(SyntheticHive, V2LayoutFilesExist) {
  auto tmp = /* std::filesystem::temp_directory_path() unique subdir */;
  testsupport::write_synthetic_hive_v2(tmp, {});
  EXPECT_TRUE(std::filesystem::exists(tmp / "date=2026-07-01" / "data.parquet"));
}
TEST(SyntheticHive, V2FileLoadsPerSymbolThroughTableSeam) {
  // read_parquet(date file) -> load_opra_cbbo_from_table(underlying="AAA")
  // -> panel with 36 quotes and an implied spot within 1% of 100.0.
}
```

- [ ] **Step 2:** Build → FAIL (header absent). **Step 3:** implement. **Step 4:** `-Ctest -R SyntheticHive` → PASS (proves the fixture is loader-consumable BEFORE anything builds on it). **Step 5: Commit** `test(vol): synthetic multi-symbol OPRA hive fixture`.

### Task 3: `load_opra_hive` (opra hive v2 C++ loader)

**Files:**
- Create: `atx-vol/include/atx/vol/opra_hive.hpp`, `atx-vol/src/opra_hive.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `src/opra_hive.cpp` to the lib sources)
- Test: `atx-vol/tests/opra_hive_test.cpp` (extend)

**Interfaces:**
- Consumes: Task 1 seam, Task 2 fixture, `OpraBatchResult`/`OpraBatchEntry`/`OpraBatchProgress`/`CorpusMarketInputTable`/`MissingMarketInputPolicy` from `opra_batch.hpp`.
- Produces (Task 5 depends on):

```cpp
struct OpraHiveSpec {
  std::string root_dir;                 // holds date=*/data.parquet
  std::string date_lo, date_hi;         // inclusive "YYYY-MM-DD"
  std::vector<std::string> symbols;     // empty = every underlying discovered
  std::string snapshot_suffix{"T19:55:00Z"};
  double r{0.0};
  std::vector<double> yc_pillar_t, yc_pillar_r;
  CorpusMarketInputTable market_inputs{};
  MissingMarketInputPolicy missing_market_inputs{MissingMarketInputPolicy::UseFallback};
  OpraProvenanceMode provenance_mode{OpraProvenanceMode::Compatibility};
  unsigned n_threads{0};                // 0 = auto; identical result any value
};
[[nodiscard]] Result<OpraBatchResult> load_opra_hive(const OpraHiveSpec &spec,
                                                     const OpraBatchProgress &progress = {});
```

- [ ] **Step 1: Failing tests** (append to `opra_hive_test.cpp`):

```cpp
TEST(OpraHive, LoadsAllSymbolsAllDates)        // 3x3 fixture -> n_loaded=9, entries date-major then symbol-major
TEST(OpraHive, MissingDateNonFatal)            // range includes 2026-07-03 (no dir) -> n_missing counts it, Ok overall
TEST(OpraHive, EmptySymbolsDiscoversUnderlyings) // spec.symbols empty -> all 3 discovered, sorted
TEST(OpraHive, SubsetSymbolsLoadsOnlyRequested)  // symbols={"BBB"} -> 3 entries, all BBB
TEST(OpraHive, DeterministicAcrossThreadCounts)  // n_threads 1 vs 8 -> identical entry order, counts, per-panel row counts
TEST(OpraHive, ParityWithPerSymbolLayout)      // same rows via v1 fixture + load_opra_daterange vs v2 + load_opra_hive -> panels equal per cell
TEST(OpraHive, MalformedSpecIsTopLevelErr)     // date_hi < date_lo; mismatched pillar arrays -> Err(InvalidArgument)
TEST(OpraHive, CorruptDateFileCountsError)     // truncate a data.parquet -> that date's symbols get Err entries, n_error>0, batch still Ok
```

Fill each with real assertions against the fixture (counts above are exact for the default `SyntheticHiveSpec`).
- [ ] **Step 2:** Build → FAIL (header absent).
- [ ] **Step 3: Implement** `opra_hive.cpp`. Shape: (1) validate spec (mirrors `load_opra_daterange`'s malformed-spec taxonomy — empty root, reversed dates, pillar mismatch → top-level `Err(InvalidArgument)`). (2) Enumerate calendar dates in range; a date's path is `<root>/date=<d>/data.parquet`; absent file → per-symbol `Err(NotFound)` entries (or one per requested symbol; when `symbols` empty and the file is absent the date contributes a single anonymous missing entry with `symbol=""`) and `n_missing` bumps. (3) Per existing date (fan out per DATE over `n_threads` workers, each date writing pre-sized disjoint slots exactly like `load_opra_daterange` W4.3): one `io::read_parquet` of the full 8 columns; when `spec.symbols` empty, discover the sorted distinct `underlying` set from that table (union across dates, resolved BEFORE parallel panel construction so entry order is globally deterministic — implement as a serial pre-pass that reads each file's `underlying` column once, then a parallel panel pass; ONE materialized read per date is the contract, so the pre-pass table is cached and reused by the panel pass). (4) Per (date, symbol): build `OpraLoadSpec{path, underlying=symbol, snapshot_iso=date+snapshot_suffix, r, spot_override=0, cash_divs from market_inputs cell like load_opra_daterange does, pillars, provenance_mode, fit_context from market_inputs}` and call `load_opra_cbbo_from_table(table, spec)`; wrap into `OpraBatchEntry{symbol, date, path, snapshot_ts_ns, panel}`. Reuse `load_opra_daterange`'s market-input resolution + `iso_to_ns` memoization verbatim (read `src/opra_batch.cpp` and lift shared helpers into a detail header if needed rather than copy-pasting). (5) Serial post-join progress + counter aggregation, date-major then symbol-major.
- [ ] **Step 4:** Build + `-Ctest -R OpraHive` → all PASS. Also run `-Ctest -R OpraPanel` (no regression through the seam).
- [ ] **Step 5: Commit** `feat(vol): date-partitioned OPRA hive v2 loader (load_opra_hive)`.

### Task 4: `generate_symbol_configs` (auto config → manifest)

**Files:**
- Create: `atx-vol/include/atx/vol/surface_db_build.hpp`, `atx-vol/src/surface_db_build.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add source)
- Test: `atx-vol/tests/surface_db_build_test.cpp` (create; add to tests/CMakeLists.txt)

**Interfaces:**
- Consumes: `SurfaceDb`, `SymbolFitConfig`, `symbol_config_from_preset` (surface_db.hpp); `CorpusBoard` + `corpus_board_from_opra` (opra_batch.hpp:215); `select_fit_policy(const Underlying&, std::string_view ticker, const FitContext&, const FitPolicyConfig&) noexcept -> FitDecision` (fit_policy.hpp:229); `select_curve(const Underlying&, const SurfaceParityInputs&, const SelectorConfig&)` + `production_selector_config()` (curve_selector.hpp:186); how a board becomes an `Underlying`: mirror `src/pricer_fitter.cpp:624` (`chain.underlying()`) via the corpus board fit path (`src/corpus_board_fit.hpp`).
- Produces (Task 5 depends on):

```cpp
struct AutoConfigSpec {
  std::string config_date{};            // "" = each symbol's earliest board date
  FitPreset preset{FitPreset::Populate};
  std::string index_symbol{};           // gets the dense index recipe (mirror populate_universe_streaming's seeding)
  bool deep_selection{false};           // false = select_fit_policy features only; true = full select_curve OOS search
  bool overwrite_existing{false};
};
struct AutoConfigReport {
  std::uint32_t n_symbols{0}, n_configured{0}, n_skipped_existing{0}, n_disabled_failed{0};
  std::vector<std::string> failed_symbols;   // sorted; mirrored as disabled configs in the db
};
[[nodiscard]] Result<AutoConfigReport>
generate_symbol_configs(SurfaceDb &db, std::span<const CorpusBoard> boards,
                        const AutoConfigSpec &spec);
```

- [ ] **Step 1: Failing tests** in `surface_db_build_test.cpp` (build boards from the Task 2 fixture via `load_opra_hive` + `corpus_board_from_opra`):

```cpp
TEST(GenerateSymbolConfigs, StoresConfigPerSymbol)      // fresh db -> n_configured=3; db.symbol_config("AAA") Ok, enabled, preset==Populate
TEST(GenerateSymbolConfigs, IdempotentSkipsExisting)    // second run -> n_skipped_existing=3, generation unchanged configs (upsert not called)
TEST(GenerateSymbolConfigs, OverwriteReplacesExisting)  // overwrite_existing=true after hand-editing band_k -> band_k restored
TEST(GenerateSymbolConfigs, IndexSymbolPinnedDense)     // index_symbol="AAA" -> AAA config differs per the dense recipe seeding
TEST(GenerateSymbolConfigs, SelectionFailureStoredDisabled) // feed one symbol whose board is gutted (drop all but 1 quote) -> config stored with enabled=false; n_disabled_failed=1; top-level still Ok
```

- [ ] **Step 2:** Build → FAIL. **Step 3: Implement.** Per symbol: pick the config board (`spec.config_date` or earliest); build the chain/`Underlying` through the same path `corpus_board_fit` uses; base config = `symbol_config_from_preset(spec.preset)`; run `select_fit_policy(under, symbol, board.fit_context, {})` → overlay `preset=decision.preset`, `pin_curve=true`, `curve=decision.curve`; when `deep_selection`, run `select_curve(under, parity_inputs, production_selector_config())` and pin the winner instead (fall back to the fit-policy decision on `NotFound`/`Unavailable`, recording `used_fallback`); `index_symbol` gets the dense-recipe seeding copied from `populate_universe_streaming`'s existing seeding code (read `src/surface_db_populate.cpp`, extract that seeding into a shared internal helper — do not duplicate). Any selection failure → store `symbol_config_from_preset(spec.preset)` with `enabled=false` and record the symbol. All stores via `db.upsert_symbol`; existing symbols skipped unless `overwrite_existing`.
- [ ] **Step 4:** Build + `-Ctest -R GenerateSymbolConfigs` → PASS; also `-R SurfaceDbPopulate` (seeding-helper extraction must not regress populate).
- [ ] **Step 5: Commit** `feat(vol): auto per-symbol fit-config generation into SurfaceDb manifest`.

### Task 5: `build_surface_db` driver

**Files:**
- Modify: `atx-vol/include/atx/vol/surface_db_build.hpp`, `atx-vol/src/surface_db_build.cpp`
- Test: `atx-vol/tests/surface_db_build_test.cpp` (extend)

**Interfaces:**
- Consumes: Tasks 3+4; `populate_universe_streaming` + `UniversePopulateSpec` + `UniversePopulateCoverage` (surface_db_populate.hpp:131-158); `SurfaceDb::create/open`.
- Produces (Tasks 6+7 depend on):

```cpp
struct SurfaceDbBuildSpec {
  std::string db_root;                  // created if absent, else opened
  OpraHiveSpec hive;
  AutoConfigSpec auto_config{};
  FitPreset preset{FitPreset::Populate};
  unsigned fit_workers{0};
};
struct SurfaceDbBuildReport {
  AutoConfigReport config;
  UniversePopulateCoverage coverage;
  std::size_t n_dates_loaded{0}, n_dates_missing{0}, n_load_errors{0};
};
[[nodiscard]] Result<SurfaceDbBuildReport> build_surface_db(const SurfaceDbBuildSpec &spec);
[[nodiscard]] Status write_build_report_csv(const SurfaceDbBuildReport &r, std::string_view path);
```

- [ ] **Step 1: Failing tests:**

```cpp
TEST(BuildSurfaceDb, EndToEndOnFixtureHive)   // fresh db_root + 3x3 hive -> Ok; report: n_dates_loaded=3, config.n_configured=3, coverage.cells_ok==9 (or ==cells_to_fit with n_failed==0); db reopens: 3 partitions, 3 symbols; map_surface("2026-07-01","AAA") Ok
TEST(BuildSurfaceDb, RerunFitsZero)           // immediate second build_surface_db -> coverage.cells_to_fit==0, dates_written==0
TEST(BuildSurfaceDb, IncrementalNewDateOnly)  // add a 4th date to the hive, rebuild -> only that date written (dates_written==1)
TEST(BuildSurfaceDb, UnpulledWindowGracefulNoop) // empty hive root -> Ok, all-zero coverage, no partitions
TEST(BuildSurfaceDb, ReportCsvRoundTrips)     // write_build_report_csv writes header+rows; file exists, first line matches pinned header
```

- [ ] **Step 2:** Build → FAIL. **Step 3: Implement:** create-or-open db (create iff no `manifest.atxdb` at root — mirror `SurfaceDb::open` NotFound probe); `load_opra_hive`; boards = `corpus_board_from_opra(entry.date, entry.symbol, std::move(*entry.panel))` for each loaded entry; `generate_symbol_configs`; `populate_universe_streaming(db, boards, UniversePopulateSpec{index_symbol=spec.auto_config.index_symbol, preset=spec.preset, fit_workers=spec.fit_workers})`; assemble report. CSV: one `key,value` section for scalars + one `symbol,n_attempted,n_ok,n_failed,n_disabled` row per `coverage.per_symbol` entry (reuse `write_populate_stats_csv`'s formatting discipline).
- [ ] **Step 4:** Build + `-Ctest -R BuildSurfaceDb` → PASS. **Step 5: Commit** `feat(vol): one-call surface database build driver`.

### Task 6: CLI tool + feature doc

**Files:**
- Create: `atx-vol/tools/surface_db_build_main.cpp`, `atx-vol/docs/surface-db-build.md`
- Modify: `atx-vol/CMakeLists.txt` (exe target `atx-vol-surface-db-build`, linked like the existing `tools/migrate_atxvsa_v1_to_v2.cpp` tool target — copy that block)

**Interfaces:** Consumes Task 5. Produces the production CLI:
`atx-vol-surface-db-build --db <root> --hive <root> --from 2026-07-01 --to 2026-07-31 [--symbols A,B,C] [--index SPY] [--preset populate] [--deep-selection] [--fit-workers N] [--report out.csv]`

- [ ] **Step 1:** Implement `main`: hand-rolled arg loop (match the repo's existing tool style — no new dependency), map to `SurfaceDbBuildSpec`, run, print the report summary (one line per report field) to stdout, `write_build_report_csv` when `--report`, exit 0 on Ok / 1 on Err with the error message on stderr. Empty `--symbols` = discover-all.
- [ ] **Step 2:** `pwsh scripts/atx-build.ps1 build atx-vol-surface-db-build` → builds. Smoke: run it against a Task-2 fixture hive in a temp dir; expect exit 0 + 3 partitions.
- [ ] **Step 3:** Write `docs/surface-db-build.md`: layout spec (§3 of the design doc condensed), CLI usage, resume semantics, scale posture (§6), pointers to the pull/migrate tools.
- [ ] **Step 4: Commit** `feat(vol): surface-db build CLI tool + docs`.

### Task 7: Python bindings + pytest

**Files:**
- Modify: `atx-vol/python/src/bindings/surface_db.cpp` (extend), `atx-vol/python/src/bindings/module.cpp` (only if a new registration hook is needed)
- Test: `atx-vol/python/tests/test_surface_db_build.py` (create)

**Interfaces:** Consumes Task 5. Produces Python API (keyword-args, snake_case):

```python
import atxvol
report = atxvol.build_surface_db(
    db_root=str, hive_root=str, date_lo=str, date_hi=str,
    symbols=list[str] | None, index_symbol="", preset="populate",
    deep_selection=False, fit_workers=0)   # -> dict with config/coverage/date counters (plain dict, matches SurfaceDbBuildReport fields)
```

- [ ] **Step 1: Failing pytest** `test_surface_db_build.py`: build a tiny v2 hive with pyarrow (mirror the Task 2 generator in ~40 lines of python: same OSI/px/PCP construction, 2 symbols x 2 dates), call `atxvol.build_surface_db`, assert `report["coverage"]["cells_ok"] == 4`, then reopen via the existing `SurfaceDb` binding and assert 2 partitions + `map_surface`/`load_surface` succeeds for one (date, symbol). Second call asserts `cells_to_fit == 0`.
- [ ] **Step 2:** Run `pytest atx-vol/python/tests/test_surface_db_build.py -v` → FAIL (attribute missing). **Step 3:** implement the binding: one function translating kwargs → `SurfaceDbBuildSpec`, releasing the GIL around `build_surface_db`, returning nested dicts (follow the result/error translation pattern already in `bindings/surface_db.cpp` and `result.hpp`). Rebuild the extension the same way the existing python tests expect (see `atx-vol/python/CMakeLists.txt` / its README). **Step 4:** pytest PASS + existing `test_atxvol.py` still green. **Step 5: Commit** `feat(vol): python binding for build_surface_db`.

### Task 8: `tools/migrate_opra_hive.py`

**Files:**
- Create: `atx-vol/tools/migrate_opra_hive.py`
- Test: `atx-vol/python/tests/test_migrate_opra_hive.py`

**Interfaces:** standalone CLI, importable functions for tests:
`python migrate_opra_hive.py --src C:/atx-data/spy-dispersion/opra --dst C:/atx-data/opra-hive [--from D --to D] [--dry-run]`. Produces `migrate(src, dst, date_lo=None, date_hi=None) -> MigrationStats` and per-date `_merge_date(src_files, dst_file)`.

- [ ] **Step 1: Failing pytest:** build an old-layout tree in tmp_path (pyarrow, 2 symbols x 2 dates, exact 8-col schema), run `migrate`, assert: `date=<d>/data.parquet` exists per date; row count per date == sum of source files; rows sorted (underlying, symbol); schema == canonical 8 cols; re-run → `stats.n_skipped == 2`, `n_written == 0` (idempotent); `--from/--to` filters; a dst date file whose row count already ≥ union is never rewritten; partial-source tolerance (symbol missing that date just isn't in the file).
- [ ] **Step 2:** pytest → FAIL. **Step 3: Implement:** enumerate `<src>/<symbol>/<date>.parquet`; group by date; per date read all symbol files (validate schema equality, hard error on drift), concat, sort by (underlying, symbol), write to `<dst>/date=<d>/data.parquet.tmp` then `os.replace` (atomic); skip when dst exists AND its distinct-underlying set ⊇ the source's (footer metadata read only); write `<dst>/migration_manifest_<ts>.csv` (date, n_source_files, n_rows, status); `--dry-run` prints the plan. Zero network. **Step 4:** pytest PASS. **Step 5: Commit** `feat(vol): OPRA hive v1->v2 migration tool`.

### Task 9: `tools/pull_opra_hive.py`

**Files:**
- Create: `atx-vol/tools/pull_opra_hive.py`
- Test: `atx-vol/python/tests/test_pull_opra_hive.py`

**Interfaces:** CLI mirrors `pull_opra_universe_batch.py` flags (`--universe/--symbols-file, --start, --end, --snap-utc 19:55, --out, --cap, --dry-run, --force, --env-file, --index-symbol, --min-degrade-names, --sample-days`) targeting the v2 layout. Core functions injectable for tests: `plan_missing(out_root, symbols, dates) -> dict[date, list[sym]]` (reads existing date files' distinct-underlying sets from parquet metadata), `pull(client, plan, ...)`, `merge_date_file(existing_path|None, new_frame, tmp_swap)`.

- [ ] **Step 1: Failing pytests** with a `FakeHistorical` (records calls; `metadata.get_cost` returns fixed unit; `timeseries.get_range` returns a canned `DBNStore`-like with `.to_df()` from a fixture frame; NO real client import needed in tests):
  - `plan_missing` on empty root → all cells; after a date file with {A} exists and request={A,B} → only B for that date; complete file → date absent from plan.
  - preflight math: estimate == unit x n_missing_cells; over-cap degrades keep-set exactly like v1 (index kept, weight-ranked fill, BLOCK exit 3 below floor) — port those code paths and pin with the same numbers.
  - `merge_date_file`: existing {A} + new {B} → union file, sorted, atomic tmp; `--force` rewrites requested symbols.
  - dry-run: zero `get_range` calls recorded.
  - resume: DBN cache hit → zero `get_range` calls, boards still written.
- [ ] **Step 2:** FAIL. **Step 3: Implement** by porting `pull_opra_universe_batch.py` (keep: key handling, calendar, snap_window, get_cost retry/sampling, degrade, DBN cache + quarantine, manifest, spend accounting — all verbatim where possible) with the v2 deltas: plan unit = (date → missing symbol set) from date-file metadata; ONE `get_range` per date over the union of missing parents; decode → single frame → `merge_date_file` per §3 of the spec. **Step 4:** pytest PASS. **Step 5: Commit** `feat(vol): date-partitioned OPRA hive pull tool`.

### Task 10: Production run — 2026-07 dataset + surface db (orchestrator-run, real spend ≤ $100)

**Files:** Create `atx-vol/research/2026-07-surface-db-production-run.md` (running log + final report). No library code.

**Gates: every paid step is preceded by its own `--dry-run` preflight, whose output is pasted into the log. Stop immediately if cumulative estimate would exceed $100.**

- [ ] **Step 1 — Migrate (free):** `python atx-vol/tools/migrate_opra_hive.py --src C:/atx-data/spy-dispersion/opra --dst C:/atx-data/opra-hive` → expect ~135 date partitions incl. 2026-07-01..17; log stats.
- [ ] **Step 2 — Smoke pull (pennies):** `pull_opra_hive.py --symbols-file <3 names: SPY,AAPL,NVDA> --start 2026-07-20 --end 2026-07-21 --out C:/atx-data/opra-hive --cap 5 --dry-run` → log estimate → run paid. Expect 2 date files gaining 3 symbols each.
- [ ] **Step 3 — Smoke build:** `atx-vol-surface-db-build --db C:/atx-data/surface-db/2026 --hive C:/atx-data/opra-hive --from 2026-07-01 --to 2026-07-22 --symbols SPY,AAPL,NVDA --index SPY --report smoke_report.csv` → verify: configs stored, ~15 partitions touched, coverage clean; immediate re-run → `cells_to_fit==0`. Query check via python binding (`map_surface` one cell).
- [ ] **Step 4 — Top-up pull (small):** full existing universe (`--universe atx-vol/data/universe/spy_top50_2026-01-01.csv`) for 2026-07-20..21, `--cap 20`, dry-run first. Log realized spend.
- [ ] **Step 5 — Scale decision:** free preflight for a top-100 universe over ALL of 2026-07 (missing cells only). If estimate ≤ remaining budget minus $10 reserve → pull; else stay at 50 names. Log decision + numbers either way.
- [ ] **Step 6 — Full production build:** `atx-vol-surface-db-build` over 2026-07-01..(last completed session), full universe, `--report`. Re-run proves fits-zero. Record: partitions, symbols, surfaces, db bytes, wall time, coverage table, cumulative spend.
- [ ] **Step 7 — Final report + commit** `docs(vol): 2026-07 surface-db production run report` (log file only; data stays out of git).

---

## Self-review notes

- Spec §3 layout/resume → Tasks 2/3/8/9; §4 auto-config → Task 4; §5 driver/CLI/Python → Tasks 5/6/7; §7 tooling → Tasks 8/9; §8 run → Task 10; §9 testing folded per task; §6 scale posture documented in Task 6 doc. No uncovered spec section.
- Type names cross-checked: `load_opra_cbbo_from_table` (T1→T3), `SyntheticHiveSpec`/`write_synthetic_hive_v2` (T2→T3/T5), `OpraHiveSpec`/`load_opra_hive` (T3→T5), `AutoConfigSpec`/`AutoConfigReport`/`generate_symbol_configs` (T4→T5), `SurfaceDbBuildSpec`/`SurfaceDbBuildReport`/`build_surface_db` (T5→T6/T7).
- Deliberate adaptation points (NOT placeholders): OpraPanel accessor names in T1's equality block and the `Underlying` construction in T4 depend on internals the implementer must read (`opra_panel.hpp`, `src/corpus_board_fit.hpp`, `src/pricer_fitter.cpp:624`); the contract each must satisfy is pinned above.
