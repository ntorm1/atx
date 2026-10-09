#pragma once

// atx-vol run-report emitters — machine-readable, metadata-header CSV outputs
// for a completed backtest run. atx-vol emits data only; a separate Python
// renderer (later work) turns these files into HTML/SVG tearsheets, so the
// column order, metric key names, and file shape below are a BINDING
// interface, not an implementation detail.
//
// ## File shape (every writer below)
//
//   # key=value          (one line per meta entry, in the given order)
//   <header row>
//   <data rows>
//
// Meta lines always come first, one per `MetaKv` entry, in the order given
// (`write_surface_db_stats_csv` appends its own entries after the caller's).
// Every writer uses `\n` line endings and `,`-separated, `\t`-free CSV. Series
// doubles (per-row backtest columns) are written with `%.17g` — 17 significant
// digits round-trip an IEEE-754 double bit-exactly through `strtod`. Metric
// values (headline scalars) are formatted with `%.10g` by the `*_metrics`
// helpers before being handed to `write_metrics_csv` as plain strings — see
// `tearsheet.cpp::write_backtest_tsv` for the same double-formatting
// discipline over `\t`. No iostream locale/format state is used (snprintf into
// a fixed buffer, like `tearsheet.cpp`), so output is deterministic across
// platforms and independent of any prior stream state. Every writer returns
// `Err(ErrorCode::IoError, ...)` if the file cannot be opened or written.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"   // BacktestResult, SnapshotCacheStats
#include "atx/vol/api/storage/surface_db.hpp" // SurfaceDb, DbPartitionInfo
#include "atx/vol/tools/tearsheet.hpp"  // TearSheet
#include "atx/vol/api/core/types.hpp"      // Status

namespace atx::vol {

// Ordered key/value pairs. Used both for `# key=value` meta headers (values
// are opaque strings, e.g. dates or already-formatted numbers) and for the
// generic two-column metrics table (`write_metrics_csv`).
using MetaKv = std::vector<std::pair<std::string, std::string>>;

// Write `r` as a metadata-header CSV: one row per recorded step. Columns,
// EXACTLY this order:
//   date,ts_ns,pnl_total,nav,pnl_delta,pnl_gamma,pnl_vega,pnl_vanna,pnl_volga,
//   pnl_theta,pnl_rho,pnl_charm,pnl_unexplained,pnl_settlement,pnl_shares,
//   financing,cost,cash,gross_delta,gross_gamma,gross_vega,gross_theta,
//   turnover_notional,turnover_vega,n_open_lots,n_unpriced_lots,
//   n_unpriced_greeks
// then one extra column per entry of `r.signals`, in order, named by the
// signal. `date` is written verbatim; `ts_ns` as a signed integer; every
// other column is a double written with `%.17g`.
[[nodiscard]] Status write_backtest_series_csv(const BacktestResult &r, const MetaKv &meta,
                                               std::string_view path);

// Generic two-column metrics table: header `metric,value`, then one row per
// `metrics` entry (key, value) verbatim — callers pre-format numeric values
// (see `strategy_metrics`/`result_summary_metrics`/`engine_metrics`, all of
// which use `%.10g`).
[[nodiscard]] Status write_metrics_csv(const MetaKv &meta, const MetaKv &metrics,
                                       std::string_view path);

// `TearSheet` -> metrics rows, `%.10g`-formatted, keys EXACTLY:
//   total_return, ann_return, ann_vol, sharpe, max_drawdown, hit_rate,
//   avg_turnover, total_cost, total_financing, attr_delta, attr_gamma,
//   attr_vega, attr_vanna, attr_volga, attr_theta, attr_rho, attr_charm,
//   attr_unexplained, return_on_gross_vega, vega_adj_sharpe,
//   pnl_per_vega_traded, avg_gross_vega, avg_gross_gamma.
[[nodiscard]] MetaKv strategy_metrics(const TearSheet &ts);

// `BacktestResult` -> summary rows, `%.10g`-formatted, keys EXACTLY:
//   total_pnl        = nav.back() (0 if empty)
//   avg_daily_pnl    = mean(pnl_total) over rows 1..n-1 (0 if fewer than 2 rows)
//   avg_net_vega     = mean(gross_vega) over rows with n_open_lots > 0 (0 if none)
//   avg_net_theta    = mean(gross_theta) over the SAME rows (0 if none)
//   avg_open_lots    = mean(n_open_lots) over all rows
//   peak_open_lots   = max(n_open_lots) over all rows
//   total_unpriced_lots   = Sum n_unpriced_lots
//   total_unpriced_greeks = Sum n_unpriced_greeks
//   n_steps          = r.size()
[[nodiscard]] MetaKv result_summary_metrics(const BacktestResult &r);

// Engine performance stats for one run: wall-clock time, step count, and the
// shared `SnapshotCache`'s counters at the end of the run.
struct EngineRunStats {
  double wall_clock_ms{0.0};
  std::uint64_t n_steps{0};
  SnapshotCacheStats cache{};
};

// `EngineRunStats` -> metrics rows, `%.10g`-formatted, keys EXACTLY:
//   wall_clock_ms, steps_per_s, n_steps, cache_loads, cache_hits,
//   cache_prefetches. `steps_per_s` = n_steps / (wall_clock_ms/1000), 0 when
//   wall_clock_ms <= 0.
[[nodiscard]] MetaKv engine_metrics(const EngineRunStats &s);

// Write a `SurfaceDb`'s partition inventory as a metadata-header CSV. `meta`
// gets, in addition to the caller's entries (appended after them, in this
// order): db_root, generation, n_symbols, n_partitions, total_file_size (Sum
// of partition file_size). Header: `key,surface_count,file_size,created_ts_ns`;
// one row per partition, in ascending key order; surface_count/file_size/
// created_ts_ns are written as plain integers (not `%.17g`/`%.10g` — they are
// integer fields, not doubles).
[[nodiscard]] Status write_surface_db_stats_csv(const SurfaceDb &db, const MetaKv &meta,
                                                std::string_view path);

} // namespace atx::vol
