#pragma once

// Single source of truth for the 25 plain-double series columns of a
// `BacktestResult` that BOTH serializers emit — in this exact order, after the
// leading `date` + `ts_ns` columns:
//   * the TSV writer  `append_backtest_series_tsv` (src/tearsheet.cpp), and
//   * the RunArchive encoder `encode_backtest_section` (src/run_archive.cpp).
// It replaces the two hand-kept `dbl_cols[]` arrays those functions used to
// carry independently (kept in lockstep only by convention).
//
// FREEZE: this table's {name, order} is pinned to the FROZEN RunArchive column
// registry `kBacktestCols[2..26]` (run_archive_schema.hpp) by a compile-time
// `static_assert` in run_archive.cpp — the registry fold feeds
// `ra_schema_hash()` (0xdcce47781ac8390d), so any drift here without a matching
// registry change (a new golden fixture + schema-hash bump) is a build error by
// design. This header does NOT touch the registry; it derives-and-checks
// against it, never alters it. Per-signal columns (`BacktestResult::signals`)
// are appended dynamically by each writer and are deliberately NOT listed here.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp" // BacktestResult (the member pointers below)

namespace atx::vol {

// One ordered {column name, BacktestResult member} binding. `member` is a
// pointer-to-data-member so a single loop over the table drives both
// serializers: `r.*member` yields that column's value vector for row extraction.
struct BacktestSeriesColumn {
  std::string_view name;
  const std::vector<double> BacktestResult::*member;
};

// The 25 F64 series columns (`pnl_total` … `n_unpriced_greeks`), in the exact
// order both writers emit them after `date`/`ts_ns`. This is the only place the
// list lives *for the tearsheet writer and the RunArchive encoder* — the two
// serializers that share this canonical order.
//
// It is NOT the only copy in the codebase. `run_report.cpp:78`
// (`write_backtest_series_csv`) keeps its own hand-maintained list of the same
// 25 names in a DIFFERENT order: `nav` sits at CSV index 1 rather than at
// canonical index 14, i.e. `csv == [pnl_total, nav] ++ (canonical \ {pnl_total,
// nav})`. That ordering is a published CSV contract, pinned by
// `run_report_test.cpp` (`kPinnedHeader`) and `mag7_dispersion_report_test.py`
// (`SERIES_HEADER`), so it cannot be driven off this table without an explicit
// permutation — a naive iteration would move `nav` from CSV field 3 to field 16
// and rewrite every emitted row. Deduplicating it is a deliberate schema
// decision, not a cleanup; see the Wave B minors triage.
inline constexpr BacktestSeriesColumn kBacktestSeriesColumns[] = {
    {"pnl_total", &BacktestResult::pnl_total},
    {"pnl_delta", &BacktestResult::pnl_delta},
    {"pnl_gamma", &BacktestResult::pnl_gamma},
    {"pnl_vega", &BacktestResult::pnl_vega},
    {"pnl_vanna", &BacktestResult::pnl_vanna},
    {"pnl_volga", &BacktestResult::pnl_volga},
    {"pnl_theta", &BacktestResult::pnl_theta},
    {"pnl_rho", &BacktestResult::pnl_rho},
    {"pnl_charm", &BacktestResult::pnl_charm},
    {"pnl_unexplained", &BacktestResult::pnl_unexplained},
    {"pnl_settlement", &BacktestResult::pnl_settlement},
    {"pnl_shares", &BacktestResult::pnl_shares},
    {"financing", &BacktestResult::financing},
    {"cost", &BacktestResult::cost},
    {"nav", &BacktestResult::nav},
    {"cash", &BacktestResult::cash},
    {"gross_delta", &BacktestResult::gross_delta},
    {"gross_gamma", &BacktestResult::gross_gamma},
    {"gross_vega", &BacktestResult::gross_vega},
    {"gross_theta", &BacktestResult::gross_theta},
    {"turnover_notional", &BacktestResult::turnover_notional},
    {"turnover_vega", &BacktestResult::turnover_vega},
    {"n_open_lots", &BacktestResult::n_open_lots},
    {"n_unpriced_lots", &BacktestResult::n_unpriced_lots},
    {"n_unpriced_greeks", &BacktestResult::n_unpriced_greeks},
};

static_assert(std::size(kBacktestSeriesColumns) == 25,
              "the backtest series has exactly 25 F64 columns (date/ts_ns excluded)");

// The one ordered {name, member-ptr} table both serializers iterate.
[[nodiscard]] constexpr std::span<const BacktestSeriesColumn>
backtest_series_columns() noexcept {
  return kBacktestSeriesColumns;
}

} // namespace atx::vol
