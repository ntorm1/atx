#pragma once

// atx-vol backtest analytics (Phase B3) — the TearSheet summary and a
// deterministic TSV export over a `BacktestResult`.
//
// Both entry points are PURE functions of a `BacktestResult` (the SoA time
// series produced by `run_backtest`): no pricing, no I/O beyond the single TSV
// write. The tearsheet folds the per-step PnL series and the attribution/greek
// columns into headline metrics plus a vega-scaled / per-unit-risk block; the
// TSV export writes every column with a bit-exact double round-trip so the
// series can be reloaded without loss.
//
// ## Conventions (see backtest.hpp)
//
// Row 0 is inception (zero PnL, `nav == 0`). The per-step "return" series is the
// `pnl_total` column for rows 1..n-1; `nav` is the cumulative Σ pnl_total from
// inception. Attribution totals (`attr_*`) are Σ over ALL rows and satisfy the
// closure identity
//
//   total_return == attr_delta + attr_gamma + attr_vega + attr_vanna + attr_volga
//                 + attr_theta + attr_rho + attr_charm + attr_unexplained
//                 + attr_settlement + attr_shares + attr_financing - attr_cost
//
// (because `pnl_total = axes + unexplained + settlement + shares + financing -
// cost` per step, so its running sum equals the attribution sum).

#include <string_view>

#include "atx/vol/backtest.hpp"  // BacktestResult
#include "atx/vol/types.hpp"     // Status

namespace atx::vol {

// Headline analytics for a completed backtest. All fields default to 0 so an
// empty or degenerate run yields a well-defined (all-zero) sheet. Every divide
// in the definitions below is guarded; see `tearsheet` for the exact formulas.
struct TearSheet {
  // ── Standard ($-PnL series; per-step return = pnl_total, nav = cumulative) ──
  double total_return{0};   // nav.back() — cumulative $ PnL from inception
  double ann_return{0};     // mean(return series) * periods_per_year
  double ann_vol{0};        // sample-std(return series) * sqrt(periods_per_year)
  double sharpe{0};         // ann_return / ann_vol (0 if ann_vol == 0)
  double max_drawdown{0};   // max peak-to-trough drop of nav, in $ (>= 0)
  double hit_rate{0};       // fraction of return-series steps with pnl_total > 0
  double avg_turnover{0};   // mean(turnover_notional over rows 1..n-1)
  double total_cost{0};     // Σ cost
  double total_financing{0};  // Σ financing

  // ── Attribution totals (Σ over ALL rows). Closure identity is a gate. ──
  double attr_delta{0};
  double attr_gamma{0};
  double attr_vega{0};
  double attr_vanna{0};
  double attr_volga{0};
  double attr_theta{0};
  double attr_rho{0};
  double attr_charm{0};
  double attr_unexplained{0};
  double attr_settlement{0};
  double attr_shares{0};
  double attr_financing{0};
  double attr_cost{0};

  // ── Vega-scaled / per-unit-risk ──
  double return_on_gross_vega{0};  // total_return / mean(|gross_vega|)
  double vega_adj_sharpe{0};       // mean(pnl_i / |gross_vega_{i-1}|) / std(...) * sqrt(ppy)
  double pnl_per_vega_traded{0};   // total_return / Σ turnover_vega
  double avg_gross_vega{0};        // mean(gross_vega over all rows)
  double avg_gross_gamma{0};       // mean(gross_gamma over all rows)
};

// Fold a `BacktestResult` into a `TearSheet`. Statistics over the return series
// exclude row 0 (inception); attribution/greek means are over all recorded rows.
// The return-series and vega-adjusted std are SAMPLE std (count-1 denominator,
// 0 when fewer than 2 observations). Every divide is guarded. Deterministic:
// all accumulation is in row order.
[[nodiscard]] TearSheet tearsheet(const BacktestResult& r, double periods_per_year = 252.0);

// Write `r` to `path` as a deterministic, tab-separated file: one header row
// naming every column, then one data row per recorded step. Doubles are written
// with `%.17g` (17 significant digits uniquely identify an IEEE-754 double, so
// the values round-trip BIT-EXACTLY through strtod); `ts_ns` as a signed integer;
// `n_open_lots` as a double. Line endings are `\n`, separators `\t`, with no
// trailing tab. Signal series are appended as one column each, by name, in
// `r.signals` order. Returns `IoError` if the file cannot be opened/written.
[[nodiscard]] Status write_backtest_tsv(const BacktestResult& r, std::string_view path);

}  // namespace atx::vol
