#pragma once

// atx::engine::combine — AlphaMetrics + compute_metrics: per-alpha realized-
// performance summary (P4-1 defined the POD; P4-2 fills it).
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  AlphaMetrics is the plain POD summary of one alpha's realized performance,
//  computed over its PnL stream (alpha::AlphaStreams::pnl(a)). P4-1 defined the
//  value type — AlphaStore stores one per AlphaRecord. THIS unit (P4-2) adds the
//  pure `compute_metrics(...)` free function that populates it. All fields are
//  f64 and carry NaN where the statistic is undefined (e.g. a degenerate, zero-
//  variance stream has an undefined Sharpe).
//
//  Rule of Zero: a trivial aggregate, copied/moved by value (it is small and
//  owns nothing). No invariants are enforced in the struct — a metrics value is
//  only ever produced by compute_metrics, never hand-mutated on a hot path.
//
// ===========================================================================
//  §0-F: index 0 is a STRUCTURAL ZERO — excluded from the moment computations
// ===========================================================================
//  The input stream IS alpha::AlphaStreams::pnl(a), the constant-weight analytic
//  return stream. AlphaStreams writes pnl[..][0] = 0 because period 0 has no
//  prior weight or prior close (see streams.hpp's no-look-ahead alignment).
//  Period 0 is therefore a STRUCTURAL zero, not a real observation: folding it
//  into the mean/variance would bias both downward (an extra synthetic 0 sample
//  shrinks the mean and inflates N). compute_metrics computes mean/std/returns
//  over r[1..T) ONLY. The drawdown walk DOES start at period 0: equity =
//  cumprod(1+r) with r[0] = 0 simply keeps equity at 1.0 through period 0, which
//  is harmless (it cannot be a new peak below 1.0 nor a drawdown).
//
//  Turnover is derived from the position stream (AlphaStreams exposes no
//  turnover stream): u[t] = Σ_j |w_j[t] − w_j[t-1]| / book_size for t >= 1, and
//  u[0] = Σ_j |w_j[0]| / book_size — the cost of trading IN from a flat book.
//  turnover = mean(u) over ALL periods (u[0] is a real trade, so it is NOT
//  excluded, unlike the structural PnL zero).

#include <cmath>   // std::sqrt, std::abs, std::isnan
#include <limits>  // std::numeric_limits (quiet_NaN for undefined moments)
#include <span>    // std::span (non-owning stream inputs)
#include <vector>  // std::vector (opt-in per-period turnover output)

#include "atx/core/stats/online_stats.hpp" // stats::RunningVariance (Welford)
#include "atx/core/types.hpp"              // atx::f64, atx::usize

namespace atx::engine::combine {

// ===========================================================================
//  AlphaMetrics — realized-performance summary for one alpha (§4 data model).
//
//  All fields f64; NaN where the statistic is undefined. Aggregate-initialized
//  in declaration order. Annualization uses 252 trading periods/year. The
//  fitness formula and its turnover floor follow WorldQuant's published metric
//  (WQ §4.4) — see the per-field notes.
// ===========================================================================
struct AlphaMetrics {
  // Annualized Sharpe: sqrt(252) * mean(pnl) / std(pnl). NaN if std(pnl) == 0.
  atx::f64 sharpe;
  // Mean daily dollar-traded as a fraction of book size (Σ|Δw| per period, mean).
  atx::f64 turnover;
  // Annualized mean periodic return: 252 * mean(pnl).
  atx::f64 returns;
  // Max peak-to-trough drawdown of the cumulative-return curve (fraction, [0,1]).
  atx::f64 drawdown;
  // Return per dollar traded, in bps: returns / max(turnover, eps).
  atx::f64 margin;
  // WQ §4.4 fitness: sqrt(abs(returns) / max(turnover, 0.125)) * sharpe.
  // The 0.125 turnover floor is WorldQuant's published low-turnover guard
  // (prevents a near-zero denominator inflating the fitness of an idle alpha).
  atx::f64 fitness;
  // Mean holding horizon in periods, ~ 1 / turnover.
  atx::f64 holding_days;
};

// ===========================================================================
//  Named constants (no magic numbers inline; §5.1 / WQ §4.4).
// ===========================================================================
// Trading periods per year — the annualization factor for Sharpe and returns
// (§5.1: sharpe = sqrt(252)*mean/std, returns = 252*mean).
inline constexpr atx::f64 kAnnualizationDays = 252.0;
// WQ §4.4 low-turnover blow-up guard: the fitness denominator is floored at
// 0.125 so a near-idle alpha's tiny turnover cannot inflate its fitness.
inline constexpr atx::f64 kTurnoverFloor = 0.125;
// Div-by-zero floor for margin / holding_days (§5.1 max(.,1e-9) guards).
inline constexpr atx::f64 kEps = 1e-9;

namespace detail {

// ---------------------------------------------------------------------------
//  Moments over r[1..T): the §0-F structural-zero exclusion.
//
//  Welford (atx-core RunningVariance) folds r[1..T) only; index 0 is the
//  structural zero. With 0 observations (length-0 or length-1 stream) the mean
//  is undefined -> NaN (DOCUMENTED §8 boundary). std is POPULATION (divisor N,
//  RunningVariance::variance()) to match §5.1's population `std`.
// ---------------------------------------------------------------------------
struct PnlMoments {
  atx::f64 mean;    // mean(r[1..T)); NaN if no observations
  atx::f64 std_pop; // population std(r[1..T)); 0 for a flat stream
  atx::usize n;     // number of observations (T-1, clamped at 0)
};

[[nodiscard]] inline PnlMoments pnl_moments(std::span<const atx::f64> pnl) noexcept {
  atx::core::stats::RunningVariance acc;
  for (atx::usize t = 1U; t < pnl.size(); ++t) {
    acc.update(pnl[t]); // r[0] (structural zero) deliberately skipped
  }
  const atx::usize n = acc.count();
  if (n == 0U) {
    // No real observations: mean/std undefined. NaN propagates to sharpe/returns.
    const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
    return PnlMoments{nan, nan, 0U};
  }
  return PnlMoments{acc.mean(), acc.std_dev(), n};
}

// Max peak-to-trough drawdown of equity = cumprod(1+r), walking from period 0.
// r[0] == 0 keeps equity at 1.0 through the structural period (harmless). One
// pass, O(T), no allocation. Returns 0 for an empty stream.
[[nodiscard]] inline atx::f64 max_drawdown(std::span<const atx::f64> pnl) noexcept {
  atx::f64 equity = 1.0;
  atx::f64 peak = 1.0;
  atx::f64 max_dd = 0.0;
  for (const atx::f64 r : pnl) {
    equity *= (1.0 + r);
    if (equity > peak) {
      peak = equity;
    }
    // peak >= equity > 0 here for any sane return path; guard a 0 peak anyway.
    const atx::f64 dd = (peak > 0.0) ? (peak - equity) / peak : 0.0;
    if (dd > max_dd) {
      max_dd = dd;
    }
  }
  return max_dd;
}

// Shared turnover loop: fills *out_turnover (if non-null) with per-period u[t]
// and returns the mean. Both compute_metrics (passing nullptr) and
// compute_metrics_with_turnover call this so the formula lives in ONE place.
//   u[0]   = Σ_j |w_j[0]|           / book_size   (trade IN from flat)
//   u[t>0] = Σ_j |w_j[t]−w_j[t-1]|  / book_size
// positions_flat is period-major then instrument-minor (n_periods*n_instruments).
// One pass, O(T*N), no allocation on the hot path. Returns 0 for empty input.
[[nodiscard]] inline atx::f64
turnover_fill(std::span<const atx::f64> positions_flat, atx::usize n_instruments,
              atx::f64 book_size, std::vector<atx::f64>* out_turnover) noexcept {
  if (n_instruments == 0U || positions_flat.empty()) {
    if (out_turnover != nullptr) {
      out_turnover->clear();
    }
    return 0.0;
  }
  const atx::usize n_periods = positions_flat.size() / n_instruments;
  const atx::f64 inv_book = 1.0 / ((book_size > kEps) ? book_size : kEps);
  // Resize the output vector once up front (avoids repeated reallocation).
  if (out_turnover != nullptr) {
    out_turnover->resize(n_periods);
  }
  atx::f64 sum_u = 0.0;
  for (atx::usize t = 0U; t < n_periods; ++t) {
    const atx::usize off = t * n_instruments;
    atx::f64 traded = 0.0;
    for (atx::usize j = 0U; j < n_instruments; ++j) {
      const atx::f64 cur = positions_flat[off + j];
      const atx::f64 prev = (t == 0U) ? 0.0 : positions_flat[off - n_instruments + j];
      const atx::f64 dw = cur - prev;
      traded += (dw < 0.0) ? -dw : dw;
    }
    const atx::f64 u_t = traded * inv_book;
    if (out_turnover != nullptr) {
      (*out_turnover)[t] = u_t;
    }
    sum_u += u_t;
  }
  return (n_periods == 0U) ? 0.0 : sum_u / static_cast<atx::f64>(n_periods);
}

// Thin wrapper: mean turnover only (no per-period vector). Passes nullptr so
// turnover_fill skips the vector path entirely — allocation-free.
[[nodiscard]] inline atx::f64 mean_turnover(std::span<const atx::f64> positions_flat,
                                            atx::usize n_instruments, atx::f64 book_size) noexcept {
  return turnover_fill(positions_flat, n_instruments, book_size, nullptr);
}

} // namespace detail

// ===========================================================================
//  compute_metrics — §5.1 (reconciled with §0-F) over one alpha's streams.
//
//  PURE, allocation-free, single-pass per statistic. `pnl` is the alpha's
//  realized-return stream (alpha::AlphaStreams::pnl(a)); index 0 is the §0-F
//  STRUCTURAL ZERO and is excluded from the Sharpe/vol/returns moments (computed
//  over r[1..T)). `positions_flat` is the alpha's target-weight stream, period-
//  major then instrument-minor (length == n_periods*n_instruments), used to
//  derive per-period turnover (u[0] = trade-in-from-flat; see header). `book_size`
//  divides the traded notional (pass 1.0 when weights are already gross-normalized
//  fractions).
//
//  NaN policy (DOCUMENTED): a flat stream (std == 0) gives sharpe 0 (not inf);
//  0 observations (length-0/1 stream) or an all-NaN stream give NaN
//  sharpe/returns/margin/fitness; turnover == 0 AND returns == 0 gives fitness 0.
//  margin/holding_days floor their denominators at 1e-9 (no div-by-zero).
// ===========================================================================
[[nodiscard]] inline AlphaMetrics compute_metrics(std::span<const atx::f64> pnl,
                                                  std::span<const atx::f64> positions_flat,
                                                  atx::usize n_instruments,
                                                  atx::f64 book_size) noexcept {
  const detail::PnlMoments mom = detail::pnl_moments(pnl);

  // Sharpe: flat stream (std == 0) -> 0 by NaN policy; else sqrt(252)*mean/std.
  // A NaN mean (no observations) propagates to a NaN sharpe naturally.
  const atx::f64 sharpe = (mom.std_pop == 0.0 && !std::isnan(mom.mean))
                              ? 0.0
                              : std::sqrt(kAnnualizationDays) * mom.mean / mom.std_pop;
  const atx::f64 returns = kAnnualizationDays * mom.mean;
  const atx::f64 turnover = detail::mean_turnover(positions_flat, n_instruments, book_size);

  // margin = returns / max(turnover, 1e-9); holding = 1 / max(turnover, 1e-9).
  const atx::f64 turnover_eps = (turnover > kEps) ? turnover : kEps;
  const atx::f64 margin = returns / turnover_eps;
  const atx::f64 holding_days = 1.0 / turnover_eps;

  // fitness = sqrt(abs(returns)/max(turnover, 0.125)) * sharpe   (WQ §4.4).
  // turnover == 0 AND returns == 0 -> fitness 0 (NaN policy, avoids 0*inf forms).
  const atx::f64 fitness_denom = (turnover > kTurnoverFloor) ? turnover : kTurnoverFloor;
  const atx::f64 fitness = (turnover == 0.0 && returns == 0.0)
                               ? 0.0
                               : std::sqrt(std::abs(returns) / fitness_denom) * sharpe;

  const atx::f64 drawdown = detail::max_drawdown(pnl);

  return AlphaMetrics{sharpe, turnover, returns, drawdown, margin, fitness, holding_days};
}

// ===========================================================================
//  compute_metrics_with_turnover — opt-in variant that ALSO fills a per-period
//  turnover vector u[t] (S4-4).
//
//  When out_turnover is non-null it is resized to n_periods and filled with
//  per-period u[t] = Σ_j |Δw_j| / book_size. The returned AlphaMetrics is
//  byte-identical to compute_metrics(...) on the same inputs — the per-period
//  vector is additional output, not an alternative path. When out_turnover is
//  null this function behaves identically to compute_metrics (no allocation).
//
//  NOTE: out_turnover is a scratch allocation in the CALLER's hands; this
//  function does not allocate on the hot path beyond what the caller requests
//  by passing a non-null pointer.
// ===========================================================================
[[nodiscard]] inline AlphaMetrics compute_metrics_with_turnover(
    std::span<const atx::f64> pnl, std::span<const atx::f64> positions_flat,
    atx::usize n_instruments, atx::f64 book_size,
    std::vector<atx::f64>* out_turnover /* nullable */) noexcept {
  const detail::PnlMoments mom = detail::pnl_moments(pnl);

  const atx::f64 sharpe = (mom.std_pop == 0.0 && !std::isnan(mom.mean))
                              ? 0.0
                              : std::sqrt(kAnnualizationDays) * mom.mean / mom.std_pop;
  const atx::f64 returns = kAnnualizationDays * mom.mean;

  // turnover_fill: fills out_turnover (if non-null) and returns the mean.
  // This is the SINGLE shared loop — no formula duplication vs. compute_metrics.
  const atx::f64 turnover =
      detail::turnover_fill(positions_flat, n_instruments, book_size, out_turnover);

  const atx::f64 turnover_eps = (turnover > kEps) ? turnover : kEps;
  const atx::f64 margin = returns / turnover_eps;
  const atx::f64 holding_days = 1.0 / turnover_eps;

  const atx::f64 fitness_denom = (turnover > kTurnoverFloor) ? turnover : kTurnoverFloor;
  const atx::f64 fitness = (turnover == 0.0 && returns == 0.0)
                               ? 0.0
                               : std::sqrt(std::abs(returns) / fitness_denom) * sharpe;

  const atx::f64 drawdown = detail::max_drawdown(pnl);

  return AlphaMetrics{sharpe, turnover, returns, drawdown, margin, fitness, holding_days};
}

} // namespace atx::engine::combine
