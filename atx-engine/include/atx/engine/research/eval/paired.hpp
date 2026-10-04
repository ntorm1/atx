#pragma once

// atx::engine::research::eval -- the paired dSR(net) of a cell against its reference (platform
// P9 B2, K-P9-5 `paired`; moved from nav_summ.paired_stats). On aligned daily nets a, b:
//
//   dsr          SR(a) - SR(b), SR = mean / sd (ddof 1) x sqrt(252) (house_sharpe)
//   rho          Pearson correlation (numpy corrcoef); NaN when either sd is 0
//   1 - rho      from the standardized difference series: sum((za - zb)^2) / (2 (T - 1)), za =
//                (a - mean) / sd (ddof 1), the same quantity without the cancellation of 1 - rho
//   memmel_se    Memmel (2003): sqrt(max([2 - 2 rho + (a^2 + b^2 - 2 a b rho^2) / 2] / T, 0) x 252)
//                with per-session SRs a, b, formed as
//                [2 omr + ((a - b)^2 + 2 a b omr (2 - omr)) / 2] / T (omr = 1 - rho);
//                t = dsr / memmel_se when that is positive
//   CBB          circular block bootstrap (Politis-Romano 1992): `draws` resamples of ceil(T /
//                block) uniform block starts from numpy default_rng(seed).integers(0, T) (row
//                order), blocks wrap, truncated to T; percentile 95% CI of the resampled dSR
//                over the finite draws (numpy linear quantile)
//   LW           Ledoit-Wolf (2008) studentized circular block bootstrap on the same resamples:
//                population-moment dSR d0 and its delta-method SE s0 with a Bartlett HAC (lag =
//                block - 1); per resample the block estimator Psi* = sum_j S_j S_j' / T of the
//                centred block sums; t* = |d* - d0| / s*; CI d0 -/+ q95(t*) s0; two-sided p =
//                (#{t* >= |d0| / s0} + 1) / (valid + 1); one-sided p (H0: dSR <= 0) =
//                (#{(d* - d0) / s* >= d0 / s0} + 1) / (valid + 1). Every g' Psi g is formed as
//                the (long-run) variance of the scalar influence series g'(y_k - mean), the
//                difference of the two Sharpe influence series.
//
// Ruling B2-R1 (P9 fix round 1). The Python formed 1 - rho, Memmel and the HAC quadratic forms by
// cancellation (numpy BLAS), so its error grows like 1 / (1 - rho); bit-matching it is not
// attainable. (a) The engine forms them from the difference series as above (algebraically the
// same statistics); (b) Python-vs-engine tolerance for these floats is 1e-12 / (1 - rho), and
// printed lines and decision counts are equal while 1 - rho >= 1e-6; (c) a pair with 1 - rho below
// kDegenerateOneMinusRho is refused as degenerate: memmel_se, t, lw.se, the LW CI and p-values are
// absent (valid_draws 0), as for a cell paired with itself (ruling B2-4). dSR, rho, d0 and the CBB
// interval are still reported.
//
// numpy's reduction orders are kept elsewhere (row means pairwise, column means sequential), so the
// counts and the quantiles equal the Python's.

#include <optional>
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::eval {

inline constexpr usize kDefaultDraws = 2000U;
inline constexpr usize kDefaultBlock = 21U;
inline constexpr u64 kDefaultSeed = 20260927U;
inline constexpr f64 kDegenerateOneMinusRho = 1e-6; // ruling B2-R1 (c)

struct PairedConfig {
  usize draws{kDefaultDraws};
  usize block{kDefaultBlock};
  u64 seed{kDefaultSeed};
  bool one_sided{false};
};

struct LwStats {
  f64 dsr_population{};          // annualized
  f64 se{};                      // annualized; NaN for a degenerate pair
  std::optional<f64> ci_lo;      // annualized
  std::optional<f64> ci_hi;
  std::optional<f64> p_value;    // two-sided
  std::optional<f64> p_one_sided; // computed always; nav_summ prints it with one_sided only
  usize valid_draws{};
};

struct PairedStats {
  usize sessions{};
  f64 sr{};            // NaN when undefined (Python float("nan"))
  f64 sr_reference{};
  f64 dsr{};
  f64 rho{};
  f64 one_minus_rho{}; // the stable form; NaN with rho
  bool degenerate{false}; // 1 - rho < kDegenerateOneMinusRho: SE / t / LW p refused
  f64 memmel_se{};     // NaN for a degenerate pair
  std::optional<f64> t;
  f64 cbb_lo{};        // NaN without a finite draw
  f64 cbb_hi{};
  usize cbb_valid_draws{};
  LwStats lw;
  PairedConfig config;
};

// InvalidArgument when the series differ in length, T < 3, draws == 0, block == 0, or T does
// not fit the 32-bit draw range.
[[nodiscard]] core::Result<PairedStats> paired_stats(std::span<const f64> a,
                                                     std::span<const f64> b,
                                                     const PairedConfig &config);

} // namespace atx::engine::research::eval
