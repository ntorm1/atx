#pragma once

// Shared guard for `BacktestResult` consumers.
//
// `BacktestResult` is a struct-of-arrays whose row count is `date.size()`. Every
// consumer in the library (the tearsheet fold, both TSV writers) indexes all
// columns from 0 to that count, because a result produced by `run_backtest` is
// consistent by construction.
//
// The Python bindings expose the columns as writable, so a caller can assemble a
// result by hand (to reload one from a TSV, for example) — and a hand-assembled
// result CAN be ragged. Passing one to a consumer reads out of bounds and takes
// the process down with an access violation. This turns that into a Python
// exception raised before any C++ consumer sees the value.

#include <cstddef>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>

#include "atx/vol/backtest.hpp"

namespace atxvol::python {

inline void require_consistent(const atx::vol::BacktestResult &r, const char *what) {
  const std::size_t n = r.date.size();
  const auto check = [&](std::size_t size, const char *column) {
    if (size != n) {
      throw pybind11::value_error(
          std::string{what} + ": BacktestResult column '" + column + "' has " +
          std::to_string(size) + " entries but 'date' has " + std::to_string(n) +
          "; every column must match the row count (use BacktestResult.resize(n) "
          "to size them all before assigning)");
    }
  };
  check(r.ts_ns.size(), "ts_ns");
#define ATXVOL_CHECK(name) check(r.name.size(), #name)
  ATXVOL_CHECK(pnl_total);
  ATXVOL_CHECK(pnl_delta);
  ATXVOL_CHECK(pnl_gamma);
  ATXVOL_CHECK(pnl_vega);
  ATXVOL_CHECK(pnl_vanna);
  ATXVOL_CHECK(pnl_volga);
  ATXVOL_CHECK(pnl_theta);
  ATXVOL_CHECK(pnl_rho);
  ATXVOL_CHECK(pnl_charm);
  ATXVOL_CHECK(pnl_unexplained);
  ATXVOL_CHECK(pnl_settlement);
  ATXVOL_CHECK(pnl_shares);
  ATXVOL_CHECK(financing);
  ATXVOL_CHECK(cost);
  ATXVOL_CHECK(nav);
  ATXVOL_CHECK(cash);
  ATXVOL_CHECK(gross_delta);
  ATXVOL_CHECK(gross_gamma);
  ATXVOL_CHECK(gross_vega);
  ATXVOL_CHECK(gross_theta);
  ATXVOL_CHECK(turnover_notional);
  ATXVOL_CHECK(turnover_vega);
  ATXVOL_CHECK(n_open_lots);
  ATXVOL_CHECK(n_unpriced_lots);
  ATXVOL_CHECK(n_unpriced_greeks);
#undef ATXVOL_CHECK
  // `step_pnl_total` is deliberately NOT row-parallel (its length tracks the
  // clock, not the recorded rows), so it is exempt.
  for (const auto &[name, series] : r.signals) {
    if (series.size() != n) {
      throw pybind11::value_error(std::string{what} + ": signal '" + name + "' has " +
                                  std::to_string(series.size()) + " entries but 'date' has " +
                                  std::to_string(n));
    }
  }
}

} // namespace atxvol::python
