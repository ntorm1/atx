#pragma once

// atx::engine::eval::hac — autocorrelation-robust inference for the mean of one series
// (W0-E0a; closes E-02 and E-03 together with cross_section_ic and the combine t-stats).
//
// ===========================================================================
//  Why this unit exists
// ===========================================================================
//  A per-date IC measured against an h-day forward return overlaps its neighbours by h-1
//  days, so the IC series is (at least) MA(h-1). The IID t-statistic mean / (sd / sqrt(n))
//  then overstates significance by roughly sqrt(h) (E-03), and a bootstrap whose block is
//  shorter than the dependence range makes intervals too narrow (E-02). This header holds
//  the single spelling of the corrections every caller uses:
//
//    * `long_run_sum`      the kernel-weighted autocovariance sum of a demeaned series,
//                          formula-identical to statsmodels' `S_hac_simple`;
//    * `mean_inference`    the HAC standard error and t-statistic of the mean, identical
//                          to statsmodels `OLS(y, ones).fit(cov_type='HAC',
//                          cov_kwds={'maxlags': L, 'use_correction': c, 'kernel': k})`;
//    * lag rules           Newey-West rule of thumb and the Newey-West (1994) automatic
//                          plug-in bandwidth for the Bartlett kernel;
//    * `mean_tstat` /
//      `ewma_variance_inflation`  the versioned rules the combine sites dispatch on;
//    * `politis_white`     the Politis-White (2004, with the Patton-Politis-White 2009
//                          correction) automatic block length.
//
//  PURE, header-only, allocation-free and noexcept: every routine walks the caller's span
//  with a loop bounded by its length (and, for lags, by n - 1). No RNG, no I/O.
//
//  NUMERICAL CONTRACT. Accumulation is in ascending index order, so every result is
//  bit-reproducible run to run. Non-finite inputs are the caller's responsibility: every
//  call site compacts its series to finite values first; a non-finite value here yields a
//  non-finite result, flagged by `MeanInference::defined == 0`.

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include "atx/core/types.hpp"

namespace atx::engine::eval::hac {

// ---------------------------------------------------------------------------
//  Kernel — the lag window of the long-run variance.
//
//  Frozen integer values (they are serialized beside every HAC figure).
// ---------------------------------------------------------------------------
enum class Kernel : atx::u8 {
  Unknown = 0,    // never a working value
  BartlettV1 = 1, // Newey-West (1987): w_j = 1 - j / (L + 1). Positive semi-definite, but
                  // biased down on MA(q) when L is near q.
  UniformV1 = 2,  // Hansen-Hodrick (1980): w_j = 1 for j <= L. Exactly unbiased for MA(L) in
                  // population; can go negative in a sample, so callers fall back to Bartlett.
};

// ---------------------------------------------------------------------------
//  TStatRule — the versioned t-statistic rule of the combine sites (E-03).
//
//  `IidV1` is the pre-W0 formula, kept so frozen fits can be re-derived; the default moved
//  to `NeweyWestAutoV2`.
// ---------------------------------------------------------------------------
enum class TStatRule : atx::u8 {
  Unknown = 0,
  IidV1 = 1,           // mean / (sd / sqrt(n)), sample sd (n - 1). Inflated ~sqrt(h) on an
                       // MA(h-1) series.
  NeweyWestAutoV2 = 2, // Bartlett at `newey_west_auto_lag`, with the n / (n - 1) correction so
                       // that lag 0 reproduces IidV1 exactly.
  HorizonAwareV3 = 3, // Uniform kernel for overlapping labels, lag >= horizon - 1;
                      // unweighted horizon 1 retains V2; weighted inference uses the
                      // exact sandwich variance. Non-positive LRV falls back.
};

inline constexpr TStatRule kDefaultTStatRule = TStatRule::NeweyWestAutoV2;

// ---------------------------------------------------------------------------
//  MeanInference — the mean of a series with its (HAC) standard error.
//
//  `defined == 0` means `se` and `t` are 0.0 and MUST be ignored: n < 2, a zero long-run
//  variance (a constant series) or a non-finite input.
// ---------------------------------------------------------------------------
struct MeanInference {
  atx::f64 mean{};
  atx::f64 se{};
  atx::f64 t{};
  atx::usize n{};
  atx::usize lag{};                // lag actually used (clamped to n - 1)
  Kernel kernel{Kernel::Unknown};  // kernel actually used (after any fallback)
  atx::u8 defined{};
  atx::u8 fell_back{};             // 1 -> UniformV1 gave S <= 0; BartlettV1 at the same lag used
};

// ---------------------------------------------------------------------------
//  kernel_weight — w_j of the lag window. j == 0 is always 1; j > lag is always 0.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr atx::f64 kernel_weight(Kernel kernel, atx::usize j,
                                               atx::usize lag) noexcept {
  if (j == 0U) {
    return 1.0;
  }
  if (j > lag) {
    return 0.0;
  }
  switch (kernel) {
  case Kernel::BartlettV1:
    return 1.0 - static_cast<atx::f64>(j) / static_cast<atx::f64>(lag + 1U);
  case Kernel::UniformV1:
    return 1.0;
  case Kernel::Unknown:
    return 0.0;
  }
  return 0.0;
}

namespace detail {

// Arithmetic mean in ascending order; 0.0 for an empty span.
[[nodiscard]] inline atx::f64 mean_of(std::span<const atx::f64> x) noexcept {
  if (x.empty()) {
    return 0.0;
  }
  atx::f64 total = 0.0;
  for (const atx::f64 v : x) {
    total += v;
  }
  return total / static_cast<atx::f64>(x.size());
}

// sum_{t=j}^{n-1} (x_t - m)(x_{t-j} - m). Requires j < n.
[[nodiscard]] inline atx::f64 lag_cross_product(std::span<const atx::f64> x, atx::f64 m,
                                                atx::usize j) noexcept {
  atx::f64 acc = 0.0;
  for (atx::usize t = j; t < x.size(); ++t) {
    acc += (x[t] - m) * (x[t - j] - m);
  }
  return acc;
}

} // namespace detail

// ---------------------------------------------------------------------------
//  long_run_sum — S = sum_t e_t^2 + 2 sum_{j=1}^{L} w_j sum_t e_t e_{t-j}, e = x - m.
//
//  This is statsmodels' `S_hac_simple(e, nlags=L, weights_func)` for a single regressor.
//  `lag` is clamped to n - 1 (a longer window has no pairs). Returns 0.0 for an empty span.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 long_run_sum(std::span<const atx::f64> x, atx::f64 m,
                                           Kernel kernel, atx::usize lag) noexcept {
  const atx::usize n = x.size();
  if (n == 0U) {
    return 0.0;
  }
  const atx::usize l = (lag < n) ? lag : (n - 1U);
  atx::f64 s = detail::lag_cross_product(x, m, 0U);
  for (atx::usize j = 1U; j <= l; ++j) {
    s += 2.0 * kernel_weight(kernel, j, l) * detail::lag_cross_product(x, m, j);
  }
  return s;
}

// ---------------------------------------------------------------------------
//  mean_inference — HAC standard error and t-statistic of the mean of `x`.
//
//      var(mean) = S / n^2  [ * n / (n - 1) when small_sample_correction ]
//
//  identical to statsmodels OLS on a constant with cov_type='HAC' (maxlags = lag,
//  use_correction = small_sample_correction, kernel = Bartlett or uniform). Under
//  `UniformV1` a non-positive S (possible in a sample) falls back to `BartlettV1` at the
//  same lag, which is positive semi-definite, and sets `fell_back`.
// ---------------------------------------------------------------------------
[[nodiscard]] inline MeanInference mean_inference(std::span<const atx::f64> x, Kernel kernel,
                                                  atx::usize lag,
                                                  bool small_sample_correction,
                                                  bool guard_cancellation = false) noexcept {
  MeanInference out{};
  out.n = x.size();
  out.mean = detail::mean_of(x);
  out.kernel = kernel;
  if (out.n < 2U || kernel == Kernel::Unknown) {
    return out;
  }
  out.lag = (lag < out.n) ? lag : (out.n - 1U);
  atx::f64 s = long_run_sum(x, out.mean, kernel, out.lag);
  const bool cancelled = guard_cancellation && kernel == Kernel::UniformV1 &&
      s <= 64.0 * std::numeric_limits<atx::f64>::epsilon() *
               long_run_sum(x, out.mean, Kernel::BartlettV1, 0U);
  if ((!(s > 0.0) || cancelled) && kernel == Kernel::UniformV1) {
    s = long_run_sum(x, out.mean, Kernel::BartlettV1, out.lag);
    out.kernel = Kernel::BartlettV1;
    out.fell_back = atx::u8{1};
  }
  const atx::f64 nf = static_cast<atx::f64>(out.n);
  atx::f64 var = s / (nf * nf);
  if (small_sample_correction) {
    var *= nf / (nf - 1.0);
  }
  if (!(var > 0.0) || !std::isfinite(var) || !std::isfinite(out.mean)) {
    return out;
  }
  out.se = std::sqrt(var);
  out.t = out.mean / out.se;
  out.defined = atx::u8{1};
  return out;
}

// ---------------------------------------------------------------------------
//  newey_west_rule_of_thumb_lag — floor(4 (n / 100)^(2/9)), statsmodels' default `nlags`.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::usize newey_west_rule_of_thumb_lag(atx::usize n) noexcept {
  if (n < 2U) {
    return 0U;
  }
  const atx::f64 v = std::floor(4.0 * std::pow(static_cast<atx::f64>(n) / 100.0, 2.0 / 9.0));
  const auto lag = static_cast<atx::usize>(v); // v >= 0 and far below usize max
  return (lag < n) ? lag : (n - 1U);
}

// ---------------------------------------------------------------------------
//  newey_west_auto_lag — the Newey-West (1994) automatic bandwidth for the Bartlett kernel.
//
//      q      = floor(4 (n/100)^(2/9))                       pre-whitening-free pilot
//      s0     = g0 + 2 sum_{j=1}^{q} g_j,  s1 = 2 sum_{j=1}^{q} j g_j   (g_j = autocov / n)
//      gamma  = 1.1447 ((s1 / s0)^2)^(1/3)
//      lag    = floor(gamma n^(1/3)),  clamped to [0, n - 1]
//
//  Falls back to the pilot q when s0 <= 0 (a degenerate pilot window).
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::usize newey_west_auto_lag(std::span<const atx::f64> x) noexcept {
  const atx::usize n = x.size();
  if (n < 2U) {
    return 0U;
  }
  const atx::usize q = newey_west_rule_of_thumb_lag(n);
  const atx::f64 m = detail::mean_of(x);
  const atx::f64 nf = static_cast<atx::f64>(n);
  atx::f64 s0 = detail::lag_cross_product(x, m, 0U) / nf;
  atx::f64 s1 = 0.0;
  for (atx::usize j = 1U; j <= q; ++j) {
    const atx::f64 g = detail::lag_cross_product(x, m, j) / nf;
    s0 += 2.0 * g;
    s1 += 2.0 * static_cast<atx::f64>(j) * g;
  }
  if (!(s0 > 0.0) || !std::isfinite(s1)) {
    return q;
  }
  const atx::f64 ratio = s1 / s0;
  const atx::f64 gamma = 1.1447 * std::cbrt(ratio * ratio);
  const atx::f64 v = std::floor(gamma * std::cbrt(nf));
  if (!(v >= 0.0)) {
    return q;
  }
  if (v >= static_cast<atx::f64>(n - 1U)) {
    return n - 1U;
  }
  return static_cast<atx::usize>(v); // 0 <= v < n - 1: exact
}

// ---------------------------------------------------------------------------
//  mean_tstat — the combine sites' t-statistic of a series mean under a versioned rule.
//
//  IidV1 reproduces the pre-W0 spelling term for term (ascending sum, sample sd):
//      mean / (sd / sqrt(n)),   defined iff n >= 2 and sd > 0.
//  NeweyWestAutoV2 is `mean_inference(x, BartlettV1, newey_west_auto_lag(x), true)`.
//  HorizonAwareV3 requires more observations than the overlap horizon and guards
//  uniform-kernel cancellation. `Unknown` returns an undefined result.
// ---------------------------------------------------------------------------
[[nodiscard]] inline MeanInference mean_tstat(std::span<const atx::f64> x,
                                              TStatRule rule,
                                              atx::usize label_horizon = 1U) noexcept {
  switch (rule) {
  case TStatRule::IidV1: {
    MeanInference out{};
    out.n = x.size();
    out.kernel = Kernel::BartlettV1; // lag 0: the kernel is immaterial
    if (out.n == 0U) {
      return out;
    }
    atx::f64 total = 0.0;
    for (const atx::f64 v : x) {
      total += v;
    }
    out.mean = total / static_cast<atx::f64>(out.n);
    if (out.n < 2U) {
      return out;
    }
    atx::f64 ss = 0.0;
    for (const atx::f64 v : x) {
      ss += (v - out.mean) * (v - out.mean);
    }
    const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(out.n - 1U));
    if (!(sd > 0.0)) {
      return out;
    }
    out.se = sd / std::sqrt(static_cast<atx::f64>(out.n));
    out.t = out.mean / out.se;
    out.defined = std::isfinite(out.t) ? atx::u8{1} : atx::u8{0};
    return out;
  }
  case TStatRule::NeweyWestAutoV2:
    return mean_inference(x, Kernel::BartlettV1, newey_west_auto_lag(x), true);
  case TStatRule::HorizonAwareV3:
    if (label_horizon == 0U || (label_horizon > 1U && label_horizon >= x.size())) {
      return MeanInference{};
    }
    return mean_inference(x, label_horizon > 1U ? Kernel::UniformV1 : Kernel::BartlettV1,
                          std::max(label_horizon - 1U, newey_west_auto_lag(x)), true, true);
  case TStatRule::Unknown:
    break;
  }
  return MeanInference{};
}

// ---------------------------------------------------------------------------
//  ewma_variance_inflation — the HAC correction of a WEIGHTED mean's variance.
//
//  For m_w = sum w_r x_r / sum w_r and u_r = w_r (x_r - m_w):
//
//      VIF = [ sum u_r^2 + 2 sum_{k=1}^{L} (1 - k/(L+1)) sum_r u_r u_{r+k} ] / sum u_r^2
//
//  i.e. the Bartlett long-run sum of u over its lag-0 term. A caller multiplies its IID
//  variance of the weighted mean by VIF (divides its t by sqrt(VIF)). Bartlett keeps
//  VIF >= 0. L = newey_west_auto_lag(x) on the unweighted series. Under IidV1 the result
//  is exactly 1.0, so a caller's pre-W0 t is reproduced bit for bit. HorizonAwareV3
//  instead divides the direct weighted sandwich variance by the caller's weighted
//  IID variance; this distinction matters for unequal weights. Unsupported overlap
//  horizons return NaN under V3. Returns 1.0 when
//  sum u^2 == 0 or the spans disagree in size (no information to correct with).
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 ewma_variance_inflation(std::span<const atx::f64> x,
                                                      std::span<const atx::f64> w,
                                                      atx::f64 weighted_mean,
                                                      TStatRule rule,
                                                      atx::usize label_horizon = 1U) noexcept {
  const bool corrected = rule == TStatRule::HorizonAwareV3;
  if (corrected && (label_horizon == 0U ||
                   (label_horizon > 1U && label_horizon >= x.size()))) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  if ((rule != TStatRule::NeweyWestAutoV2 && rule != TStatRule::HorizonAwareV3) ||
      label_horizon == 0U || x.size() != w.size() || x.size() < 2U) {
    return 1.0;
  }
  const atx::usize n = x.size();
  const bool overlap = rule == TStatRule::HorizonAwareV3 && label_horizon > 1U;
  const atx::usize lag = std::min(n - 1U,
      std::max(overlap ? label_horizon - 1U : 0U, newey_west_auto_lag(x)));
  const auto u = [&](atx::usize r) noexcept { return w[r] * (x[r] - weighted_mean); };
  atx::f64 s0 = 0.0;
  atx::f64 sw = 0.0;
  atx::f64 sw2 = 0.0;
  atx::f64 swv = 0.0;
  for (atx::usize r = 0U; r < n; ++r) {
    s0 += u(r) * u(r);
    if (corrected) {
      if (!(w[r] >= 0.0) || !std::isfinite(w[r]) || !std::isfinite(x[r])) {
        return std::numeric_limits<atx::f64>::quiet_NaN();
      }
      sw += w[r];
      sw2 += w[r] * w[r];
      swv += w[r] * (x[r] - weighted_mean) * (x[r] - weighted_mean);
    }
  }
  if (!(s0 > 0.0) || !std::isfinite(s0)) {
    return 1.0;
  }
  atx::f64 s = s0;
  atx::f64 bartlett = s0;
  for (atx::usize k = 1U; k <= lag && k < n; ++k) {
    atx::f64 c = 0.0;
    for (atx::usize r = k; r < n; ++r) {
      c += u(r) * u(r - k);
    }
    const atx::f64 tapered = 2.0 * kernel_weight(Kernel::BartlettV1, k, lag) * c;
    bartlett += tapered;
    s += overlap ? 2.0 * c : tapered;
  }
  if (overlap && (!(s > 64.0 * std::numeric_limits<atx::f64>::epsilon() * s0) ||
                  !std::isfinite(s))) {
    s = bartlett;
  }
  if (corrected) {
    // The caller's IID variance is (swv/sw)*(sw2/sw^2). Normalize against it so
    // the resulting variance is exactly the weighted sandwich s/sw^2.
    const atx::f64 vif = (s / swv) * (sw / sw2);
    return (vif > 0.0 && std::isfinite(vif)) ? vif
                                           : std::numeric_limits<atx::f64>::quiet_NaN();
  }
  const atx::f64 vif = s / s0;
  return (vif > 0.0 && std::isfinite(vif)) ? vif : 1.0;
}

// ---------------------------------------------------------------------------
//  BlockLength / politis_white — the automatic block length of Politis & White (2004)
//  with the Patton, Politis & White (2009) correction, transcribed from the papers. It is
//  NOT verified against the `arch` package's `optimal_block_length`: arch scans lags
//  m..m+K_N-1 with its own correlation normalization, so its m^ can differ by one lag.
//  The test reference is an independent numpy transcription of the formulas below:
//
//      K_N   = max(5, floor(log10 n)),  m_max = ceil(sqrt n) + K_N,
//      c     = 2 sqrt(log10(n) / n)
//      m^    = smallest m with |rho^(m + k)| < c for k = 1..K_N  (else m_max)
//      M     = min(2 max(m^, 1), m_max)
//      G     = sum_{|k|<=M} lambda(k/M) |k| R(k),  g0 = sum_{|k|<=M} lambda(k/M) R(k)
//      b_CB  = (2 G^2 / ((4/3) g0^2))^(1/3) n^(1/3),  b_SB = (2 G^2 / (2 g0^2))^(1/3) n^(1/3)
//
//  with the flat-top window lambda(t) = 1 on |t| <= 1/2, 2(1 - |t|) on (1/2, 1], and both
//  lengths capped at ceil(min(3 sqrt n, n / 3)). `defined == 0` for n < 8 or a constant
//  series. O(n * m_max) time, no allocation.
// ---------------------------------------------------------------------------
struct BlockLength {
  atx::f64 circular{};   // b_CB (circular / moving block bootstrap)
  atx::f64 stationary{}; // b_SB (stationary bootstrap mean block length)
  atx::usize m_hat{};
  atx::usize bandwidth{}; // M
  atx::u8 defined{};
};

[[nodiscard]] inline BlockLength politis_white(std::span<const atx::f64> x) noexcept {
  BlockLength out{};
  const atx::usize n = x.size();
  if (n < 8U) {
    return out;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 m = detail::mean_of(x);
  const atx::f64 r0 = detail::lag_cross_product(x, m, 0U) / nf;
  if (!(r0 > 0.0) || !std::isfinite(r0)) {
    return out;
  }
  const auto log_floor = static_cast<atx::usize>(std::floor(std::log10(nf)));
  const atx::usize kn = (log_floor > 5U) ? log_floor : 5U;
  const auto root_ceil = static_cast<atx::usize>(std::ceil(std::sqrt(nf)));
  atx::usize m_max = root_ceil + kn;
  if (m_max > n - 1U) {
    m_max = n - 1U;
  }
  const atx::f64 crit = 2.0 * std::sqrt(std::log10(nf) / nf);
  const auto acov = [&](atx::usize k) noexcept { return detail::lag_cross_product(x, m, k) / nf; };

  // Smallest m^ whose next K_N autocorrelations are all insignificant; lags past m_max are
  // not searched, so an unresolved search leaves m^ = m_max.
  atx::usize m_hat = m_max;
  for (atx::usize cand = 0U; cand + kn <= m_max; ++cand) {
    bool quiet = true;
    for (atx::usize k = 1U; k <= kn && quiet; ++k) {
      quiet = std::fabs(acov(cand + k) / r0) < crit;
    }
    if (quiet) {
      m_hat = cand;
      break;
    }
  }
  atx::usize big_m = 2U * ((m_hat > 1U) ? m_hat : 1U);
  if (big_m > m_max) {
    big_m = m_max;
  }
  atx::f64 g = 0.0;
  atx::f64 g0 = r0;
  for (atx::usize k = 1U; k <= big_m; ++k) {
    const atx::f64 ratio = static_cast<atx::f64>(k) / static_cast<atx::f64>(big_m);
    const atx::f64 lambda = (ratio <= 0.5) ? 1.0 : 2.0 * (1.0 - ratio);
    const atx::f64 rk = acov(k);
    g += 2.0 * lambda * static_cast<atx::f64>(k) * rk;
    g0 += 2.0 * lambda * rk;
  }
  if (!(g0 > 0.0) || !std::isfinite(g)) {
    return out;
  }
  const atx::f64 b_max = std::ceil(std::fmin(3.0 * std::sqrt(nf), nf / 3.0));
  const atx::f64 n13 = std::cbrt(nf);
  const atx::f64 b_cb = std::cbrt((2.0 * g * g) / ((4.0 / 3.0) * g0 * g0)) * n13;
  const atx::f64 b_sb = std::cbrt((2.0 * g * g) / (2.0 * g0 * g0)) * n13;
  out.circular = std::fmin(b_cb, b_max);
  out.stationary = std::fmin(b_sb, b_max);
  out.m_hat = m_hat;
  out.bandwidth = big_m;
  out.defined = atx::u8{1};
  return out;
}

} // namespace atx::engine::eval::hac
