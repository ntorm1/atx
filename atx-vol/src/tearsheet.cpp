#include "atx/vol/tools/tearsheet.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"                   // Err, Ok, ErrorCode
#include "atx/core/math.hpp"                    // atx::core::norm_cdf (B4: PSR/DSR/MinTRL)
#include "atx/vol/backtest.hpp"                 // BacktestResult
#include "atx/vol/detail/backtest_series_columns.hpp"  // backtest_series_columns() (single source)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Sample mean + std (count-1 denominator) over `v`. std == 0 when fewer than two
// observations. Accumulated in element order (deterministic).
struct MeanStd {
  double mean{0.0};
  double std{0.0};
};

[[nodiscard]] MeanStd mean_std(const std::vector<double>& v) noexcept {
  const std::size_t m = v.size();
  if (m == 0) {
    return {};
  }
  double sum = 0.0;
  for (const double x : v) {
    sum += x;
  }
  const double mean = sum / static_cast<double>(m);
  if (m < 2) {
    return {mean, 0.0};
  }
  double ss = 0.0;
  for (const double x : v) {
    const double d = x - mean;
    ss += d * d;
  }
  const double var = ss / static_cast<double>(m - 1);
  return {mean, std::sqrt(var)};
}

// Σ over the whole column (row order).
[[nodiscard]] double col_sum(const std::vector<double>& v) noexcept {
  double s = 0.0;
  for (const double x : v) {
    s += x;
  }
  return s;
}

}  // namespace

std::vector<double> backtest_return_series(const BacktestResult& r) {
  // Mirrors the fallback rule inside `tearsheet` EXACTLY (see the comment there):
  // the full per-step series when retained, else the recorded pnl_total rows
  // 1..n-1. Kept as one definition so a benchmark can be aligned to the same
  // observations the absolute statistics are computed over.
  std::vector<double> returns;
  if (!r.step_pnl_total.empty()) {
    returns.assign(r.step_pnl_total.begin(), r.step_pnl_total.end());
    return returns;
  }
  const std::size_t n = r.size();
  returns.reserve(n > 0 ? n - 1 : 0);
  for (std::size_t i = 1; i < n; ++i) {
    returns.push_back(r.pnl_total[i]);
  }
  return returns;
}

Result<std::vector<std::string>> backtest_return_dates(const BacktestResult& r) {
  // Deliberately re-derives the count through `backtest_return_series`' OWN rule
  // rather than restating it, so the two can never diverge.
  const std::size_t returns = backtest_return_series(r).size();
  if (r.date.size() != returns + 1u) {
    return Err(ErrorCode::InvalidArgument,
               "backtest_return_dates: the return series is not date-addressable — " +
                   std::to_string(returns) + " return observations against " +
                   std::to_string(r.date.size()) +
                   " recorded dates (expected " + std::to_string(returns + 1u) +
                   "). `step_pnl_total` is full-resolution while `date` is downsampled by "
                   "RunConfig::record_every_n, so no per-observation date exists at a stride "
                   "greater than 1.");
  }
  return Ok(std::vector<std::string>(r.date.begin() + 1, r.date.end()));
}

namespace {

// ── B4: standard-normal inverse CDF (Z^-1), private to this TU ─────────────
//
// `atx::core::norm_cdf` (folded in from the ported C `ats-vol` library) gives
// Phi(x); nothing in-tree exposes a LINKABLE Phi^-1(p). analytics_primitives.cpp
// has one (`norm_inv`, a bisection-based exact-by-construction inverse for a
// low-call-count wing-strike solve), but it is itself anonymous-namespace-
// private to that TU and unreachable from here. Rather than promote it across
// a module boundary for one caller, this is a fresh implementation, as the
// task brief allows ("if none exists, implement Acklam's or Wichura's AS241").
//
// Acklam's rational approximation (P. J. Acklam, "An algorithm for computing
// the inverse normal cumulative distribution function", 2003) -- relative
// error < 1.15e-9 on its own -- followed by ONE Halley refinement step
// against `atx::core::norm_cdf`, lifting the accuracy to within a few ULP of
// machine precision. (The same two-stage recipe atx-engine/eval/stats_ext.hpp
// documents for its own norm_ppf; implemented independently here since
// atx-vol does not depend on atx-engine -- see the design note atop
// tearsheet.hpp's B4 section for why the two are not shared.)
//
// DOMAIN: p in (0, 1). p outside that range is a precondition violation of
// this detail function; every public caller below (psr/dsr/min_trl) is
// responsible for guarding the p it passes in, and does so.
[[nodiscard]] double norm_ppf(double p) noexcept {
  static constexpr double kA[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                   -2.759285104469687e+02, 1.383577518672690e+02,
                                   -3.066479806614716e+01, 2.506628277459239e+00};
  static constexpr double kB[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                   -1.556989798598866e+02, 6.680131188771972e+01,
                                   -1.328068155288572e+01};
  static constexpr double kC[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                   -2.400758277161838e+00, -2.549732539343734e+00,
                                   4.374664141464968e+00,  2.938163982698783e+00};
  static constexpr double kD[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                                   2.445134137142996e+00, 3.754408661907416e+00};
  constexpr double kPLow = 0.02425;
  constexpr double kPHigh = 1.0 - kPLow;  // 0.97575

  double x = 0.0;
  if (p < kPLow) {
    // Lower tail: q = sqrt(-2 ln p).
    const double q = std::sqrt(-2.0 * std::log(p));
    x = (((((kC[0] * q + kC[1]) * q + kC[2]) * q + kC[3]) * q + kC[4]) * q + kC[5]) /
        ((((kD[0] * q + kD[1]) * q + kD[2]) * q + kD[3]) * q + 1.0);
  } else if (p <= kPHigh) {
    // Central region: q = p - 0.5, r = q^2.
    const double q = p - 0.5;
    const double r = q * q;
    x = (((((kA[0] * r + kA[1]) * r + kA[2]) * r + kA[3]) * r + kA[4]) * r + kA[5]) * q /
        (((((kB[0] * r + kB[1]) * r + kB[2]) * r + kB[3]) * r + kB[4]) * r + 1.0);
  } else {
    // Upper tail: q = sqrt(-2 ln(1-p)); negated lower-tail form.
    const double q = std::sqrt(-2.0 * std::log(1.0 - p));
    x = -(((((kC[0] * q + kC[1]) * q + kC[2]) * q + kC[3]) * q + kC[4]) * q + kC[5]) /
        ((((kD[0] * q + kD[1]) * q + kD[2]) * q + kD[3]) * q + 1.0);
  }

  // One Halley step against the exact CDF: e = Phi(x) - p; u scales the
  // Newton step by the inverse density (kSqrt2Pi*exp(x^2/2) == 1/phi(x)), the
  // trailing division applies Halley's second-order correction.
  constexpr double kSqrt2Pi = 2.5066282746310002;  // sqrt(2*pi)
  const double e = atx::core::norm_cdf(x) - p;
  const double u = e * kSqrt2Pi * std::exp(x * x / 2.0);
  x -= u / (1.0 + x * u / 2.0);
  return x;
}

// Euler-Mascheroni constant gamma -- the extreme-value weight in the expected
// maximum of N standard normals (Bailey-LdP 2014 Sec 3.3, eq. for E[max]).
constexpr double kEulerMascheroni = 0.5772156649015329;

// The Bailey-LdP variance-scaling term shared by psr/dsr/min_trl:
// 1 - skew*sr + ((kurt-1)/4)*sr^2. `kurt` is RAW (Pearson) kurtosis --
// see tearsheet.hpp's B4 section for why (a Gaussian series has kurt==3).
[[nodiscard]] double psr_var_term(double sr, double skew, double kurt) noexcept {
  return 1.0 - skew * sr + ((kurt - 1.0) / 4.0) * sr * sr;
}

}  // namespace

double psr(double sr, double skew, double kurt, std::size_t T, double benchmark) noexcept {
  // SAFETY: sqrt(T-1) is 0 at T==1 (and undefined below it); T<2 would
  // silently give Phi(0)==0.5 for every input rather than signal "no
  // information", so it is refused explicitly instead.
  if (T < 2U) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double var_term = psr_var_term(sr, skew, kurt);
  // SAFETY: a pathological skew/kurtosis/sr combination can drive var_term to
  // <= 0, where its square root is undefined; the !(>0) form also rejects a
  // NaN var_term. Fail to NaN rather than propagate silently.
  if (!(var_term > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double tf = static_cast<double>(T);
  const double z = (sr - benchmark) * std::sqrt(tf - 1.0) / std::sqrt(var_term);
  return atx::core::norm_cdf(z);
}

double dsr(double sr, double skew, double kurt, std::size_t T, TrialStats trials) noexcept {
  // SAFETY: a variance cannot be negative -- a caller/catalog bug, refused
  // rather than sqrt'd into a NaN SR0 that would silently poison the PSR call
  // below.
  if (trials.sr_variance < 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double sr0 = 0.0;  // N<=1: "no selection" -- nothing to benchmark against.
  if (trials.n_trials > 1U) {
    // SAFETY: Z^-1(1 - 1/N) diverges to -infinity at N<=1 (guarded above by
    // the n_trials>1U branch, not here) -- sqrt(0)*(-infinity) would
    // otherwise be a silent NaN when a caller passes a default-initialized
    // TrialStats{1, 0.0}.
    const double n = static_cast<double>(trials.n_trials);
    const double e = std::exp(1.0);
    const double q_hi = norm_ppf(1.0 - 1.0 / n);         // Phi^-1(1 - 1/N)
    const double q_lo = norm_ppf(1.0 - 1.0 / (n * e));   // Phi^-1(1 - 1/(N*e))
    const double max_z = (1.0 - kEulerMascheroni) * q_hi + kEulerMascheroni * q_lo;
    sr0 = std::sqrt(trials.sr_variance) * max_z;
  }
  return psr(sr, skew, kurt, T, sr0);
}

double min_trl(double sr, double skew, double kurt, double benchmark, double alpha) noexcept {
  // SAFETY: Phi^-1 is undefined outside (0,1).
  if (!(alpha > 0.0 && alpha < 1.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double var_term = psr_var_term(sr, skew, kurt);
  // SAFETY: same guard as psr -- a non-positive variance term has no real
  // square root.
  if (!(var_term > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // sr == benchmark is deliberately NOT special-cased: Z^-1(alpha)/0.0 is the
  // IEEE +-infinity for every alpha != 0.5 (a zero effect size genuinely
  // needs an infinite track record at any nontrivial confidence -- the
  // correct answer, not a bug to mask), and the 0/0 at alpha==0.5 is NaN
  // (PSR is 0.5 for EVERY T when sr==benchmark, so "the smallest T reaching
  // confidence 0.5" is not a single well-defined number there either). Both
  // fall out of the arithmetic unassisted; see tearsheet.hpp's doc comment.
  const double z_alpha = norm_ppf(alpha);
  const double ratio = z_alpha / (sr - benchmark);
  return 1.0 + var_term * ratio * ratio;
}

ResidualAlarm unexplained_alarm(const BacktestResult& r, std::size_t window,
                                double tolerance) noexcept {
  ResidualAlarm out;
  const std::size_t n = r.size();
  // SAFETY (always-on, both debug and release): `window == 0` or fewer than
  // `window` recorded rows cannot form a single complete window. A size
  // mismatch on any of the nine columns (a hand-built or partially-populated
  // result) would read out of bounds below -- refused for the same reason,
  // not merely asserted.
  if (window == 0U || n < window || r.pnl_delta.size() != n || r.pnl_gamma.size() != n ||
      r.pnl_vega.size() != n || r.pnl_vanna.size() != n || r.pnl_volga.size() != n ||
      r.pnl_theta.size() != n || r.pnl_rho.size() != n || r.pnl_charm.size() != n ||
      r.pnl_unexplained.size() != n) {
    return out;
  }

  for (std::size_t start = 0; start + window <= n; ++start) {
    double gross_sum = 0.0;
    double unexpl_sum = 0.0;
    for (std::size_t i = start; i < start + window; ++i) {
      gross_sum += std::fabs(r.pnl_delta[i]) + std::fabs(r.pnl_gamma[i]) +
                   std::fabs(r.pnl_vega[i]) + std::fabs(r.pnl_vanna[i]) +
                   std::fabs(r.pnl_volga[i]) + std::fabs(r.pnl_theta[i]) +
                   std::fabs(r.pnl_rho[i]) + std::fabs(r.pnl_charm[i]);
      unexpl_sum += std::fabs(r.pnl_unexplained[i]);
    }
    // gross_sum == 0 (a perfectly flat window) -> ratio 0: there is no Greek
    // activity to have mismarked, so it can never be the worst window.
    const double ratio = gross_sum > 0.0 ? unexpl_sum / gross_sum : 0.0;
    if (ratio > out.worst_ratio) {
      out.worst_ratio = ratio;
      out.worst_window_start = start;
    }
    if (ratio > tolerance) {
      out.tripped = true;
    }
  }
  return out;
}

BenchmarkStats benchmark_stats(std::span<const double> strategy,
                               std::span<const double> benchmark, double periods_per_year) {
  BenchmarkStats out;
  if (benchmark.empty()) {
    return out;  // no benchmark supplied => nothing is claimed
  }
  out.has_benchmark = true;
  const std::size_t m = std::min(strategy.size(), benchmark.size());
  out.n_obs = m;
  if (m < 2) {
    return out;  // every ratio below needs a sample variance
  }
  const double count = static_cast<double>(m);
  const double ppy = periods_per_year;
  const double sqrt_ppy = ppy > 0.0 ? std::sqrt(ppy) : 0.0;

  double sum_s = 0.0;
  double sum_b = 0.0;
  for (std::size_t i = 0; i < m; ++i) {
    sum_s += strategy[i];
    sum_b += benchmark[i];
  }
  const double mean_s = sum_s / count;
  const double mean_b = sum_b / count;

  // One pass for the three central moments plus the active series' sum of
  // squares. Sample (n-1) denominators throughout, matching `mean_std`.
  double cov = 0.0;
  double var_b = 0.0;
  double var_s = 0.0;
  double ss_active = 0.0;
  const double mean_a = mean_s - mean_b;
  for (std::size_t i = 0; i < m; ++i) {
    const double ds = strategy[i] - mean_s;
    const double db = benchmark[i] - mean_b;
    cov += ds * db;
    var_b += db * db;
    var_s += ds * ds;
    const double da = (strategy[i] - benchmark[i]) - mean_a;
    ss_active += da * da;
  }
  const double denominator = count - 1.0;
  cov /= denominator;
  var_b /= denominator;
  var_s /= denominator;

  out.beta = var_b > 0.0 ? cov / var_b : 0.0;
  out.alpha = (mean_s - out.beta * mean_b) * ppy;
  out.active_return = mean_a * ppy;
  out.tracking_error = std::sqrt(ss_active / denominator) * sqrt_ppy;
  out.information_ratio =
      out.tracking_error > 0.0 ? out.active_return / out.tracking_error : 0.0;
  const double sd_product = std::sqrt(var_s) * std::sqrt(var_b);
  out.correlation = sd_product > 0.0 ? cov / sd_product : 0.0;
  return out;
}

TearSheet tearsheet_with_benchmark(const BacktestResult& r, std::span<const double> benchmark,
                                   double periods_per_year) {
  TearSheet ts = tearsheet(r, periods_per_year);
  if (benchmark.empty()) {
    return ts;  // strict superset: an absent benchmark changes nothing
  }
  const std::vector<double> returns = backtest_return_series(r);
  ts.benchmark = benchmark_stats(returns, benchmark, periods_per_year);
  return ts;
}

TearSheet tearsheet(const BacktestResult& r, double periods_per_year) {
  TearSheet ts;
  const std::size_t n = r.size();
  if (n == 0) {
    return ts;  // well-defined all-zero sheet
  }

  const double ppy = periods_per_year;
  const double sqrt_ppy = ppy > 0.0 ? std::sqrt(ppy) : 0.0;

  // ── Standard: total return + return-series stats (rows 1..n-1) ──
  ts.total_return = r.nav.back();

  // Return series: the TRUE per-step PnL. Under record_every_n>1 the recorded
  // `pnl_total` rows are BLOCK SUMS, so mean/std/hit_rate off them would be
  // stride-dependent; `step_pnl_total` is the full per-step series (steps 1..N-1)
  // retained at any stride, making Sharpe/ann_return/ann_vol/hit_rate stride-
  // invariant. Hand-built results leave it empty and fall back to the pnl_total
  // rows (identical to the stride-1 path).
  std::vector<double> returns;
  std::size_t wins = 0;
  if (!r.step_pnl_total.empty()) {
    returns.reserve(r.step_pnl_total.size());
    for (const double p : r.step_pnl_total) {
      returns.push_back(p);
      if (p > 0.0) {
        ++wins;
      }
    }
  } else {
    returns.reserve(n > 0 ? n - 1 : 0);
    for (std::size_t i = 1; i < n; ++i) {
      const double p = r.pnl_total[i];
      returns.push_back(p);
      if (p > 0.0) {
        ++wins;
      }
    }
  }
  const MeanStd ret = mean_std(returns);
  ts.ann_return = ret.mean * ppy;
  ts.ann_vol = ret.std * sqrt_ppy;
  ts.sharpe = ts.ann_vol > 0.0 ? ts.ann_return / ts.ann_vol : 0.0;
  ts.hit_rate = returns.empty()
                    ? 0.0
                    : static_cast<double>(wins) / static_cast<double>(returns.size());

  // Max drawdown: peak-to-trough of nav, in $ (>= 0), over all rows.
  double peak = r.nav[0];
  double max_dd = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (r.nav[i] > peak) {
      peak = r.nav[i];
    }
    const double dd = peak - r.nav[i];
    if (dd > max_dd) {
      max_dd = dd;
    }
  }
  ts.max_drawdown = max_dd;

  // Average turnover over the traded rows (1..n-1).
  if (n > 1) {
    double tsum = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
      tsum += r.turnover_notional[i];
    }
    ts.avg_turnover = tsum / static_cast<double>(n - 1);
  }
  ts.total_cost = col_sum(r.cost);
  ts.total_financing = col_sum(r.financing);

  // ── Attribution totals (Σ over all rows). ──
  ts.attr_delta = col_sum(r.pnl_delta);
  ts.attr_gamma = col_sum(r.pnl_gamma);
  ts.attr_vega = col_sum(r.pnl_vega);
  ts.attr_vanna = col_sum(r.pnl_vanna);
  ts.attr_volga = col_sum(r.pnl_volga);
  ts.attr_theta = col_sum(r.pnl_theta);
  ts.attr_rho = col_sum(r.pnl_rho);
  ts.attr_charm = col_sum(r.pnl_charm);
  ts.attr_unexplained = col_sum(r.pnl_unexplained);
  ts.attr_settlement = col_sum(r.pnl_settlement);
  ts.attr_shares = col_sum(r.pnl_shares);
  ts.attr_financing = col_sum(r.financing);
  ts.attr_cost = col_sum(r.cost);

  // ── Vega-scaled / per-unit-risk ──
  //
  // C-3 (pipeline-m production review). The three statistics below call
  // themselves GROSS, but `BacktestResult::gross_vega` is the SIGNED aggregate
  // `PriceTotals::vega` — NET book vega. A vega-neutral book (every dispersion
  // book) drives that column to a cancellation residual BY CONSTRUCTION, so
  // dividing a return by it produced an unstable or meaningless number while the
  // book's actual gross leg exposure was large and unchanged. Observed on a live
  // three-session dispersion run: mean|net| = 1.8e-12 against a real gross of
  // 2.0e5, giving return_on_gross_vega = -1.5e13.
  //
  // `run_backtest` now publishes the true Σ|position-scaled leg vega| as
  // `gross_vega_abs`. It is DELIBERATELY not serialized (no schema hash, TSV
  // header or golden moves), so it is EMPTY on a hand-built result, a TSV read
  // or an archive decode — and in that case this falls back to the previous
  // |gross_vega| expressions BIT-FOR-BIT, which is what keeps every hand-built
  // tearsheet expectation unchanged.
  const bool have_gross = r.gross_vega_abs.size() == n;
  const auto row_gross = [&r, have_gross](std::size_t i) noexcept {
    return have_gross ? r.gross_vega_abs[i] : std::fabs(r.gross_vega[i]);
  };
  double vega_sum = 0.0;
  double abs_vega_sum = 0.0;
  double gamma_sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    vega_sum += r.gross_vega[i];
    abs_vega_sum += row_gross(i);
    gamma_sum += r.gross_gamma[i];
  }
  const double mean_abs_vega = abs_vega_sum / static_cast<double>(n);
  // With a real gross series `avg_gross_vega` is that series' mean (non-negative
  // and equal to the `return_on_gross_vega` denominator); without one it stays
  // the signed column's mean, exactly as before. The NET average is separately
  // published as `avg_net_vega` by `result_summary_metrics` (run_report.cpp).
  ts.avg_gross_vega = have_gross ? mean_abs_vega : vega_sum / static_cast<double>(n);
  ts.avg_gross_gamma = gamma_sum / static_cast<double>(n);
  ts.return_on_gross_vega = mean_abs_vega > 0.0 ? ts.total_return / mean_abs_vega : 0.0;

  // vega_adj_sharpe: per-step PnL scaled by the PRIOR row's GROSS vega.
  std::vector<double> x;
  x.reserve(n > 0 ? n - 1 : 0);
  for (std::size_t i = 1; i < n; ++i) {
    const double gv_prev = row_gross(i - 1);
    if (gv_prev > 0.0) {
      x.push_back(r.pnl_total[i] / gv_prev);
    }
  }
  const MeanStd xs = mean_std(x);
  ts.vega_adj_sharpe = xs.std > 0.0 ? (xs.mean / xs.std) * sqrt_ppy : 0.0;

  const double tv_sum = col_sum(r.turnover_vega);
  ts.pnl_per_vega_traded = tv_sum > 0.0 ? ts.total_return / tv_sum : 0.0;

  // ── Margin (Task B2) ──
  //
  // `margin_required` is non-wire and empty on a hand-built/TSV-read/decoded
  // result (see BacktestResult::margin_required's comment), so both stats
  // stay at their guarded 0 default in that case -- there is no earlier
  // series to fall back to, unlike `gross_vega_abs`'s |gross_vega| fallback.
  if (r.margin_required.size() == n) {
    double margin_sum = 0.0;
    double margin_peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      margin_sum += r.margin_required[i];
      if (r.margin_required[i] > margin_peak) {
        margin_peak = r.margin_required[i];
      }
    }
    const double mean_margin = margin_sum / static_cast<double>(n);
    ts.return_on_margin = mean_margin > 0.0 ? ts.total_return / mean_margin : 0.0;
    ts.margin_utilization_peak = margin_peak;
  }

  return ts;
}

namespace {

// Build the deterministic TSV series (header + one row per recorded step) into
// `out`. Shared by `write_backtest_tsv` and `write_backtest_pnl_tsv` so the two
// entry points never drift in column set/order/formatting. `date` and `ts_ns`
// are special; the rest are plain double columns written with %.17g for a
// bit-exact round-trip, followed by one column per signal series.
void append_backtest_series_tsv(std::string& out, const BacktestResult& r) {
  // The 25 F64 columns come from the single source of truth shared with the
  // RunArchive encoder (backtest_series_columns.hpp), so the two serializers can
  // never drift in column set / order. `date` and `ts_ns` are handled specially
  // below; per-signal series are appended after the fixed columns.
  const auto dbl_cols = backtest_series_columns();

  out.reserve(out.size() + r.size() * 640 + 256);

  char buf[64];
  const auto put_double = [&](double v) {
    const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
    out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
  };

  // ── Header ──
  out += "date\tts_ns";
  for (const auto& col : dbl_cols) {
    out += '\t';
    out += col.name;
  }
  for (const auto& sig : r.signals) {
    out += '\t';
    out += sig.first;
  }
  out += '\n';

  // ── Data rows ──
  for (std::size_t i = 0; i < r.size(); ++i) {
    out += r.date[i];
    out += '\t';
    const int len = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(r.ts_ns[i]));
    out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
    for (const auto& col : dbl_cols) {
      out += '\t';
      put_double((r.*col.member)[i]);
    }
    for (const auto& sig : r.signals) {
      out += '\t';
      put_double(sig.second[i]);
    }
    out += '\n';
  }
}

// Append `s` with any structural control character (newline, carriage return,
// tab) replaced by a single space, so a meta key/value can never corrupt the
// `# key=value` header framing or the `\t`-separated body that follows.
void append_sanitized(std::string& out, std::string_view s) {
  for (const char c : s) {
    out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  }
}

// Append the `# key=value` meta header (one line per entry, in order). Keys and
// values are sanitized (newline/CR/tab -> space) so no value can break out of
// its line or inject a spurious column.
void append_meta_header(std::string& out,
                        std::span<const std::pair<std::string, std::string>> meta) {
  for (const auto& [k, v] : meta) {
    out += "# ";
    append_sanitized(out, k);
    out += '=';
    append_sanitized(out, v);
    out += '\n';
  }
}

// Deterministic `\n`-terminated write of `payload` to `path` in binary mode (no
// CRLF translation), shared by both TSV entry points.
[[nodiscard]] Status write_payload(std::string_view path, const std::string& payload,
                                   const char* who) {
  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  if (!os) {
    return Err(ErrorCode::IoError, std::string(who) + ": cannot open file");
  }
  os.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!os) {
    return Err(ErrorCode::IoError, std::string(who) + ": write failed");
  }
  return Ok();
}

}  // namespace

Status write_backtest_tsv(const BacktestResult& r, std::string_view path) {
  // Plan 4.6. `append_backtest_series_tsv` walks every column in row lockstep,
  // so a skewed result used to read out of range here. It has no error channel
  // (it appends to a caller's string); its two file-writing callers do.
  ATX_TRY_VOID(r.validate());
  std::string out;
  append_backtest_series_tsv(out, r);
  return write_payload(path, out, "write_backtest_tsv");
}

Status write_backtest_pnl_tsv(const BacktestResult& r,
                              std::span<const std::pair<std::string, std::string>> meta,
                              std::string_view path) {
  ATX_TRY_VOID(r.validate());  // plan 4.6, as in write_backtest_tsv above
  std::string out;
  append_meta_header(out, meta);
  append_backtest_series_tsv(out, r);
  return write_payload(path, out, "write_backtest_pnl_tsv");
}

}  // namespace atx::vol
