#pragma once

// atx::engine::research::eval -- the paired dSR(net) of a cell against its reference (platform
// P9 B2, K-P9-5 `paired`; moved from nav_summ.paired_stats). On aligned daily nets a, b:
//
//   dsr          SR(a) - SR(b), SR = mean / sd (ddof 1) x sqrt(252) (house_sharpe)
//   rho          Pearson correlation (numpy corrcoef); NaN when either sd is 0
//   memmel_se    Memmel (2003): sqrt(max([2 - 2 rho + (a^2 + b^2 - 2 a b rho^2) / 2] / T, 0) x 252)
//                with per-session SRs a, b; t = dsr / memmel_se when that is positive
//   CBB          circular block bootstrap (Politis-Romano 1992): `draws` resamples of ceil(T /
//                block) uniform block starts from numpy default_rng(seed).integers(0, T) (row
//                order), blocks wrap, truncated to T; percentile 95% CI of the resampled dSR
//                over the finite draws (numpy linear quantile)
//   LW           Ledoit-Wolf (2008) studentized circular block bootstrap on the same resamples:
//                population-moment dSR d0 and its delta-method SE s0 with a Bartlett HAC (lag =
//                block - 1); per resample the block estimator Psi* = sum_j S_j S_j' / T of the
//                centred block sums; t* = |d* - d0| / s*; CI d0 -/+ q95(t*) s0; two-sided p =
//                (#{t* >= |d0| / s0} + 1) / (valid + 1); one-sided p (H0: dSR <= 0) =
//                (#{(d* - d0) / s* >= d0 / s0} + 1) / (valid + 1)
//
// numpy's reduction orders are kept (row means pairwise, column means sequential, block sums
// sequential, einsum i-then-j), so the integer counts and the quantiles equal the Python's.

#include <optional>
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::eval {

inline constexpr usize kDefaultDraws = 2000U;
inline constexpr usize kDefaultBlock = 21U;
inline constexpr u64 kDefaultSeed = 20260927U;

struct PairedConfig {
  usize draws{kDefaultDraws};
  usize block{kDefaultBlock};
  u64 seed{kDefaultSeed};
  bool one_sided{false};
};

struct LwStats {
  f64 dsr_population{};          // annualized
  f64 se{};                      // annualized
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
  f64 memmel_se{};
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
