### Task 5: `populate_surface_db` — fit boards into a SurfaceDb, honoring per-symbol configs

The "fit + store" pipeline stage: boards (from the OPRA hive) are fitted with each symbol's `SymbolFitConfig` from the db manifest applied via `apply_symbol_config`, and stored one partition per date. Records per-symbol fit outcomes (the surface-stats source for the report). Plus the thin CLI example that runs it for real data.

**Files:**
- Create: `atx-vol/include/atx/vol/surface_db_populate.hpp`
- Create: `atx-vol/src/surface_db_populate.cpp`
- Create: `atx-vol/tests/surface_db_populate_test.cpp`
- Create: `atx-vol/examples/mag7_surfdb_populate.cpp`
- Modify: `atx-vol/CMakeLists.txt` (library source; example registration in the `ATX_BUILD_EXAMPLES` block:
  `add_executable(mag7_surfdb_populate examples/mag7_surfdb_populate.cpp)` + `target_link_libraries(... PRIVATE atx::vol atx::core atx_warnings)` with a comment naming the gate test `SurfaceDbPopulate`)
- Modify: `atx-vol/tests/CMakeLists.txt` (test source)

**Interfaces:**
- Consumes: `SurfaceDb` (symbol_config/upsert_symbol/write_partition/partitions), `apply_symbol_config`, `symbol_config_from_preset`, `CorpusBoard` (corpus.hpp:61-76), the board→`PricedSurface` fitting path inside `src/corpus.cpp` (the per-board fit that `build_corpus` runs), `load_opra_daterange`/`corpus_board_from_opra` (opra_batch.hpp) in the example, Task 4's `MetaKv`/`write_metrics_csv` shape for the stats file.
- Produces:

```cpp
// surface_db_populate.hpp
namespace atx::vol {

struct SurfaceDbPopulateConfig {
  // Base fit inputs (preset etc). Per-symbol SymbolFitConfig from the db
  // manifest is overlaid via apply_symbol_config; a symbol absent from the
  // manifest uses `fallback` unchanged.
  SymbolFitConfig fallback{};
  unsigned n_threads{0};          // 0 = serial; determinism must hold regardless
  bool skip_existing{true};       // date key already in db.partitions() -> skip whole date
};

struct PopulateSymbolStats {
  std::string symbol;
  std::uint32_t n_attempted{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_disabled{0};    // skipped because manifest enabled=false
  // Mean fit-quality score over successful fits, when the shared corpus fit
  // path yields one (oos_in_band from curve selection; see corpus.cpp's
  // CorpusEntry.oos_in_band recording). NaN when unavailable (e.g. the
  // pinned-curve path has no OOS score — mirrors corpus.cpp).
  double mean_oos_in_band{std::numeric_limits<double>::quiet_NaN()};
};

struct SurfaceDbPopulateStats {
  std::uint32_t n_boards{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_dates_written{0};
  std::uint32_t n_dates_skipped_existing{0};
  std::vector<PopulateSymbolStats> per_symbol;   // sorted by symbol
};

// Fit every board and store one partition per distinct board date (key =
// date). Boards are grouped by date; within a date, boards fit in symbol
// order (deterministic). A board whose symbol's manifest config has
// enabled=false is skipped (n_disabled). A board whose fit fails records
// n_failed and does NOT abort the date (document per-name failures, don't
// silently drop). A date with zero successful fits writes NO partition.
// Partition write uses SurfaceArchiveItem{symbol, &surface} with owning
// symbol-string storage kept alive across the call.
// Top-level Err only on: empty boards span, db write errors, or a date key
// the db rejects.
[[nodiscard]] Result<SurfaceDbPopulateStats>
populate_surface_db(SurfaceDb &db, std::span<const CorpusBoard> boards,
                    const SurfaceDbPopulateConfig &cfg = {});

// Stats file for the report: meta (caller's, plus n_boards/n_ok/n_failed/
// n_dates_written appended), header
// "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band",
// one row per symbol (success_rate = n_ok / max(1, n_attempted - n_disabled),
// %.10g; mean_oos_in_band prints "nan" when NaN).
[[nodiscard]] Status write_populate_stats_csv(const SurfaceDbPopulateStats &s,
                                              const MetaKv &meta,
                                              std::string_view path);

}  // namespace atx::vol
```

**Implementation constraint:** the board→surface fit must REUSE `build_corpus`'s per-board pipeline (SessionInputs from board env/frame → preset/curve application → fitter → `PricedSurface`, including uid assignment). Read `src/corpus.cpp` first; if that logic is file-local, extract a shared internal function (e.g. into a `src/`-private header) that both `build_corpus` and `populate_surface_db` call — verbatim duplication of the fit block is a review-rejectable defect. The ONLY config difference: after building the board's `SessionInputs`, call `apply_symbol_config(cfg_for_symbol, inputs)` where `cfg_for_symbol` is `db.symbol_config(board.symbol)` if present else `cfg.fallback`.

**Example CLI** (`examples/mag7_surfdb_populate.cpp`, header comment naming gate test `SurfaceDbPopulate`):

```
mag7_surfdb_populate --opra-root DIR --db DIR --symbols A,B,... --start YYYY-MM-DD --end YYYY-MM-DD
                     [--r 0.043] [--preset fast] [--fit-workers N] [--stats FILE]
```

Flow: parse args → `SurfaceDb::open(db)` else `SurfaceDb::create(db)` → for each symbol absent from the manifest, upsert a config mirroring the SPY YTD corpus fit policy (`spy_ytd_corpus.cpp:94-101`): `auto c = symbol_config_from_preset(FitPreset::Fast); c.pin_curve = true; c.curve = CurveConfig{};` (default pinned ConvexDense) → `upsert_symbol(sym, c)` → `load_opra_daterange(OpraBatchSpec{symbols, start, end, root, default template, snapshot_suffix "T19:55:00Z", r})` → `corpus_board_from_opra` per loaded entry (skip `!entry.panel`, count missing) → `populate_surface_db` → `write_populate_stats_csv(stats, meta, --stats else <db>/populate_stats.csv)` → print summary. Honor `ATX_VOL_FIT_WORKERS` env for default `--fit-workers` if the corpus config does (check `corpus.hpp` — mirror whatever `spy_dispersion_backtest.cpp build-corpus` does with `fit_workers`).

- [ ] **Step 1: Write the failing tests.** `atx-vol/tests/surface_db_populate_test.cpp`. Fixture: build genuinely fittable boards with the `make_board_spec`/`fit_board` synthetic-panel pattern from `dispersion_test.cpp:76-120` (`SynthPanelSpec` → `make_synthetic_american_panel` → `OptionChain::from_frame` → boards). 2 symbols ("AAA","BBB") × 2 dates ("2026-03-02","2026-03-03").
  - `SurfaceDbPopulate.FitsAndStoresPartitionsPerDate`: fresh db; populate; assert `n_dates_written==2`, `db.partitions().size()==2`, `db.load_surface("2026-03-02","AAA")` serves, stats per_symbol has both symbols `n_ok==2`.
  - `SurfaceDbPopulate.HonorsDisabledSymbol`: upsert `BBB` with `enabled=false`; populate; partitions contain only AAA surfaces (`open_partition(...)->map_symbol("BBB")` NotFound), stats `n_disabled==2` for BBB.
  - `SurfaceDbPopulate.SkipExistingResumes`: populate once; populate again same boards; second stats `n_dates_skipped_existing==2`, `n_dates_written==0`, db generation advanced only by the first run's writes.
  - `SurfaceDbPopulate.FailedFitRecordedNotFatal`: corrupt one board (e.g. empty frame) → populate succeeds, that (symbol,date) counted in `n_failed`, the date's partition still written with the other symbol.
  - `SurfaceDbPopulate.StatsCsvShape`: write stats file; assert header exact and success_rate arithmetic.
  - `SurfaceDbPopulate.PinnedConfigHonored`: upsert `AAA` with `pin_curve=true` + a distinctive `CurveConfig` (e.g. ConvexDense node_cap 48 — copy the pinned-config pattern from `surface_db_test.cpp`'s `ConfigureStoreReloadServe`); populate; `load_surface(date,"AAA")` reports that curve kind (`kind_at(0)`), proving `apply_symbol_config` reached the fit.
- [ ] **Step 2: Build; verify failure.**
- [ ] **Step 3: Implement** library + example per contracts above.
- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDbPopulate|SurfaceDb|Corpus"` — ALL PASS. Also build the example: `& .\scripts\atx-build.ps1 build mag7_surfdb_populate`.
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): populate_surface_db - fit OPRA boards into SurfaceDb with per-symbol configs"
```

---

