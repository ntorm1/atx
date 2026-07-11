### Task 4: `run_report` emitters — machine-readable run outputs

atx-vol emits data only; the Python renderer consumes these files. Library code with unit tests. The `# key=value` metadata-header convention comes from `spy_strangle_backtest.cpp:398-437`; the deterministic-column discipline from `tearsheet.hpp::write_backtest_tsv`.

**Files:**
- Create: `atx-vol/include/atx/vol/run_report.hpp`
- Create: `atx-vol/src/run_report.cpp`
- Create: `atx-vol/tests/run_report_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (library source list), `atx-vol/tests/CMakeLists.txt` (test source)

**Interfaces:**
- Consumes: `BacktestResult`, `TearSheet` + `tearsheet()` (tearsheet.hpp), `SnapshotCacheStats{loads,hits,prefetches}`, `SurfaceDb` (`root()`, `generation()`, `symbols()`, `partitions()`).
- Produces (all writers: `\n` line endings, `\t`-free CSV with `,` separators, doubles `%.17g` for series and `%.10g` for metric values, `IoError` on failure, deterministic output):

```cpp
// run_report.hpp
namespace atx::vol {

using MetaKv = std::vector<std::pair<std::string, std::string>>;

// File shape shared by every writer below:
//   # key=value          (one line per meta entry, in given order)
//   <header row>
//   <data rows>
//
// write_backtest_series_csv columns, exactly this order:
//   date,ts_ns,pnl_total,nav,pnl_delta,pnl_gamma,pnl_vega,pnl_vanna,pnl_volga,
//   pnl_theta,pnl_rho,pnl_charm,pnl_unexplained,pnl_settlement,pnl_shares,
//   financing,cost,cash,gross_delta,gross_gamma,gross_vega,gross_theta,
//   turnover_notional,turnover_vega,n_open_lots,n_unpriced_lots,
//   n_unpriced_greeks
// then one extra column per entry of r.signals, in order, named by the signal.
[[nodiscard]] Status write_backtest_series_csv(const BacktestResult &r,
                                               const MetaKv &meta,
                                               std::string_view path);

// Generic two-column metrics table: header "metric,value"; one row per entry.
[[nodiscard]] Status write_metrics_csv(const MetaKv &meta, const MetaKv &metrics,
                                       std::string_view path);

// TearSheet -> metrics rows (keys exactly): total_return, ann_return, ann_vol,
// sharpe, max_drawdown, hit_rate, avg_turnover, total_cost, total_financing,
// attr_delta, attr_gamma, attr_vega, attr_vanna, attr_volga, attr_theta,
// attr_rho, attr_charm, attr_unexplained, return_on_gross_vega,
// vega_adj_sharpe, pnl_per_vega_traded, avg_gross_vega, avg_gross_gamma.
[[nodiscard]] MetaKv strategy_metrics(const TearSheet &ts);

// BacktestResult -> summary rows (keys exactly): total_pnl (nav.back()),
// avg_daily_pnl (mean pnl_total over rows 1..n-1), avg_net_vega
// (mean gross_vega over rows with n_open_lots > 0), avg_net_theta (same over
// gross_theta), avg_open_lots, peak_open_lots, total_unpriced_lots,
// total_unpriced_greeks, n_steps.
[[nodiscard]] MetaKv result_summary_metrics(const BacktestResult &r);

// Engine performance -> metrics rows (keys exactly): wall_clock_ms,
// steps_per_s, n_steps, cache_loads, cache_hits, cache_prefetches.
struct EngineRunStats {
  double wall_clock_ms{0.0};
  std::uint64_t n_steps{0};
  SnapshotCacheStats cache{};
};
[[nodiscard]] MetaKv engine_metrics(const EngineRunStats &s);   // steps_per_s derived

// SurfaceDb inventory. Meta gets (in addition to caller meta, appended):
// db_root, generation, n_symbols, n_partitions, total_file_size.
// Header: "key,surface_count,file_size,created_ts_ns"; one row per partition,
// ascending key order.
[[nodiscard]] Status write_surface_db_stats_csv(const SurfaceDb &db,
                                                const MetaKv &meta,
                                                std::string_view path);

}  // namespace atx::vol
```

- [ ] **Step 1: Write the failing tests.** `atx-vol/tests/run_report_test.cpp`:
  - `RunReport.SeriesCsvRoundTrips`: build a tiny `BacktestResult` by hand (3 rows, one signal series, one double chosen to need full precision e.g. `0.1 + 0.2`), write, re-read the file as text; assert: every meta line starts `# ` and contains `=`; header EXACTLY the pinned column string + `,sig_name`; 3 data rows; the full-precision double round-trips via `std::stod` to bit-equal (`%.17g` discipline).
  - `RunReport.MetricsCsv`: `write_metrics_csv({{"a","b"}}, {{"sharpe","1.25"}}, path)`; assert file == `"# a=b\nmetric,value\nsharpe,1.25\n"`.
  - `RunReport.StrategyAndSummaryMetrics`: feed a hand-built `TearSheet`/`BacktestResult`, assert exact key set and spot-check values (`total_pnl == nav.back()`, `peak_open_lots` max, `avg_net_vega` skips zero-lot rows).
  - `RunReport.EngineMetrics`: `EngineRunStats{2000.0, 10, {5,4,3}}` → `steps_per_s == 5`, all six keys present.
  - `RunReport.DbStatsCsv`: create a temp `SurfaceDb`, write 2 partitions (reuse `make_essvi`-style fixture from surface_db_test.cpp or a minimal 1-surface archive), write stats; assert meta contains `generation=`, `n_partitions=2`, rows sorted by key.
- [ ] **Step 2: Build; verify failure.**
- [ ] **Step 3: Implement** in `src/run_report.cpp`. Single internal helper writes the meta+header+rows shape; all public writers go through it. No iostream formatting state leaks (`snprintf` into a buffer for doubles, like tearsheet.cpp).
- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 -Ctest -R "RunReport"` and `-R "TearSheet"` — ALL PASS.
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): run_report emitters - metadata-header CSV outputs for backtest runs"
```

---

