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

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"  // BacktestResult
#include "atx/vol/types.hpp"     // Status

namespace atx::vol {

// ── X5: benchmark-relative statistics (Goodwin FAJ 1998 / Grinold-Kahn) ─────
//
// All four are computed over two PAIRED per-step series — the strategy's and the
// benchmark's — truncated to their common length. Definitions, with `rs`/`rb` the
// two series, `ra = rs - rb` the ACTIVE series, and `ppy` the periods per year:
//
//   beta              = cov(rs, rb) / var(rb)                 [sample, n-1]
//   alpha             = (mean(rs) - beta * mean(rb)) * ppy    [ANNUALIZED]
//   active_return     = mean(ra) * ppy                        [ANNUALIZED]
//   tracking_error    = samplestd(ra) * sqrt(ppy)             [ANNUALIZED]
//   information_ratio = active_return / tracking_error
//
// IR is the DIFFERENCE form (active return over tracking error), which is
// Goodwin's standard and the one Grinold-Kahn's IR = IC * sqrt(breadth) targets —
// NOT the regression-residual form. They differ whenever beta != 1; the choice is
// recorded here because the two are routinely conflated.
//
// UNITS ARE THE CALLER'S RESPONSIBILITY. `BacktestResult` carries $ PnL, not
// fractional returns, so a benchmark series must be supplied in the SAME units
// ($ PnL of the benchmark at a comparable risk scale) for beta to be meaningful.
// Feeding a fractional-return benchmark against a $ strategy yields a beta off by
// the notional — arithmetically valid, economically nonsense.
struct BenchmarkStats {
  bool has_benchmark{false}; // false => every field below is 0 and must not be reported
  std::size_t n_obs{0};      // paired observations actually used
  double beta{0};
  double alpha{0};
  double active_return{0};
  double tracking_error{0};
  double information_ratio{0};
  double correlation{0}; // corr(rs, rb); 0 when either series is constant
};

// Pure, allocation-light fold over two ALREADY-PAIRED series. Uses the common
// prefix length min(strategy.size(), benchmark.size()); fewer than 2 paired
// observations, or a benchmark with zero variance, yields `has_benchmark = true`
// with the undefined ratios left at 0 (every divide is guarded). Deterministic:
// all accumulation is in element order.
//
// HAZARD (REVIEW C-6). Pairing is POSITIONAL and this function has no way to
// check it — the two spans carry no dates. A benchmark that is shifted,
// reversed, duplicated, missing a session or simply shorter yields entirely
// plausible alpha/beta/IR/tracking-error numbers for the WRONG observations, and
// the `min` silently drops the strategy tail. Establishing the pairing is the
// CALLER's job. Production callers must join by DATE: see
// `backtest_return_dates` below and `pair_dispersion_benchmark` /
// `dispersion_tearsheet_with_benchmark` in dispersion_run.hpp, which is the one
// route a spec's `benchmark_series` may reach this function through.
[[nodiscard]] BenchmarkStats benchmark_stats(std::span<const double> strategy,
                                             std::span<const double> benchmark,
                                             double periods_per_year = 252.0);

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
  //
  // UNIT / SEMANTICS (C-3, pipeline-m production review). "Gross vega" below is
  // `BacktestResult::gross_vega_abs` — Σ|position-scaled leg vega|, dollars per
  // UNIT vol — NOT the signed `gross_vega` column, which is NET book vega and
  // cancels to a residual for any vega-neutral book. A result that carries no
  // gross series (hand-built, TSV-read or archive-decoded — it is deliberately
  // not serialized) falls back to |gross_vega| bit-for-bit, i.e. the pre-C-3
  // values. The NET average is published separately, as `avg_net_vega`, by
  // `result_summary_metrics` (run_report.hpp).
  double return_on_gross_vega{0}; // total_return / mean(gross vega)
  double vega_adj_sharpe{0};      // mean(pnl_i / gross_vega_{i-1}) / std(...) * sqrt(ppy)
  double pnl_per_vega_traded{0};  // total_return / Σ turnover_vega
  // mean(gross vega); mean of the SIGNED gross_vega column when no gross series
  // is present (the pre-C-3 definition, preserved for hand-built results).
  double avg_gross_vega{0};
  double avg_gross_gamma{0}; // mean(gross_gamma over all rows)

  // ── X5 benchmark-relative block. `has_benchmark` is false unless the sheet was
  // built by `tearsheet_with_benchmark`, so plain `tearsheet()` is unchanged. ──
  BenchmarkStats benchmark{};
};

// Fold a `BacktestResult` into a `TearSheet`. Statistics over the return series
// exclude row 0 (inception); attribution/greek means are over all recorded rows.
// The return-series and vega-adjusted std are SAMPLE std (count-1 denominator,
// 0 when fewer than 2 observations). Every divide is guarded. Deterministic:
// all accumulation is in row order.
[[nodiscard]] TearSheet tearsheet(const BacktestResult& r, double periods_per_year = 252.0);

// `tearsheet(r, ppy)` with the benchmark-relative block filled in against
// `benchmark`, a per-step series ALIGNED to the same return series `tearsheet`
// folds (i.e. `r.step_pnl_total` when present, else `pnl_total` rows 1..n-1 —
// row 0 is inception and carries no return). An empty `benchmark` returns
// exactly `tearsheet(r, ppy)`, so this is a strict superset and never perturbs
// the absolute statistics.
[[nodiscard]] TearSheet tearsheet_with_benchmark(const BacktestResult& r,
                                                 std::span<const double> benchmark,
                                                 double periods_per_year = 252.0);

// `tearsheet_with_benchmark(r, benchmark, ppy)` pairs POSITIONALLY — see the
// hazard note on `benchmark_stats`. It is retained for hand-paired callers and
// for tests; a series read from a file must be joined by date instead.

// The per-step return series `tearsheet` folds, exposed so a caller can align a
// benchmark to it without duplicating the `step_pnl_total` fallback rule.
[[nodiscard]] std::vector<double> backtest_return_series(const BacktestResult& r);

// REVIEW C-6. The DATES of the `backtest_return_series` observations, so a
// benchmark can be joined to the strategy BY DATE rather than by position.
//
// The strategy series is NOT unconditionally date-addressable, and that is a
// property of `BacktestResult`, not of this function: `step_pnl_total` is
// FULL-RESOLUTION (one entry per priced step, length refs-1) while `date` is
// DOWNSAMPLED by `RunConfig::record_every_n`. At any stride > 1 there is simply
// no date for most return observations, and no join is possible — so this fails
// loudly rather than inventing an alignment. At the shipped stride of 1 (and for
// the `pnl_total` fallback, which is parallel to `date` by construction) the
// answer is exactly `date[1..n-1]`: row 0 is inception and carries no return.
//
// Err(InvalidArgument) when the two are not in that relationship, naming both
// counts.
[[nodiscard]] Result<std::vector<std::string>> backtest_return_dates(const BacktestResult& r);

// Write `r` to `path` as a deterministic, tab-separated file: one header row
// naming every column, then one data row per recorded step. Doubles are written
// with `%.17g` (17 significant digits uniquely identify an IEEE-754 double, so
// the values round-trip BIT-EXACTLY through strtod); `ts_ns` as a signed integer;
// `n_open_lots`, `n_unpriced_lots` (positions with no surface this STEP, excluded
// from the row's PnL) and `n_unpriced_greeks` (positions with no surface on this
// row's DATE, excluded from the row's `gross_*`) as doubles. Line endings are
// `\n`, separators `\t`, with no
// trailing tab. Signal series are appended as one column each, by name, in
// `r.signals` order. Returns `IoError` if the file cannot be opened/written.
[[nodiscard]] Status write_backtest_tsv(const BacktestResult& r, std::string_view path);

// WS-D D5 acceptance emit: the PnL-track TSV consumed by the Python renderer
// `tools/spy_dispersion_pnl_report.py`. Identical column layout to
// `write_backtest_tsv` (date, ts_ns, every pnl/greek/turnover column, then one
// column per signal), but PREFIXED by a `# key=value` metadata header — one
// line per `meta` entry, in the given order, each written verbatim as `# k=v`.
// `meta` carries the run's identity + headline stats + engine timing + surface
// stats (the renderer titles the chart and fills its stats box from these
// keys), so ONE self-describing TSV is the whole acceptance artifact. Series
// doubles use `%.17g` (bit-exact round-trip); `\n` line endings, `\t`
// separators, no trailing tab. Deterministic (snprintf into a fixed buffer, no
// stream/locale state). `IoError` if the file cannot be opened/written.
[[nodiscard]] Status
write_backtest_pnl_tsv(const BacktestResult& r,
                       std::span<const std::pair<std::string, std::string>> meta,
                       std::string_view path);

}  // namespace atx::vol
