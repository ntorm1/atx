#pragma once

// atx::engine::eval -- the house's registered DSR definitions, beside the engine's named
// methods (deflated_sharpe.hpp: SummaryDsrRule, AccountingDsrRule). Platform P9 B2 (DEC-6,
// K-P9-5): these are the statistics of record the research verdicts print, moved here from
// atx-impl/tools/backtest_integrity.py and dsr_total.py so one engine definition decides.
//
//   house_v1   DSR = PSR(SR*), SR* = expected_max_sharpe(N, V), with
//              N = the ledger's construction trial count by the defect rule (research/ledger
//                  ledger_n: + 1 for a cell not yet ledgered),
//              V = the ddof-1 variance of the per-session S2 net SRs of the construction
//                  lines ledgered on the current research window (research/ledger
//                  dsr_variance). (OR section 3; v8-prereg item 3, OD-4.)
//   tot        the same V with N = N_tot, every trial the defect rule counts in the ledger
//              (any kind) plus the mining campaigns' registry counts (v8x-prereg section 4).
//   cross      V = the ddof-1 variance of the listed cells' per-session SRs (nav_summ R6').
//   lo         V = the Lo (2002) sampling variance (1 + SR^2 / 2) / T of the one cell.
//   onc        N = the ONC cluster count, V = the variance of the clusters' IVP-series SRs.
//
// Every row is the same formula, PSR at SR*_N; only (N, V) differ, and the research ledger
// and the eval verb supply them. Moments are the house's: SR = mean / sd (ddof 1), skewness
// and RAW kurtosis from population moments (research/eval house_moments.hpp); the engine's
// probabilistic_sharpe takes EXCESS kurtosis, so the raw value is shifted by 3 here. The
// callers decide when a row is undefined (absent V, undefined moments, T < 2, N < 2), as
// the Python rows did; house_dsr computes a defined row.
//
// SR*_N is the house's own (ruling B2-9): house_expected_max_sr, with the quantile found by
// bisection on the house normal CDF (house_norm_ppf), the retired Python's rule operation for
// operation. The engine's expected_max_sharpe uses the Acklam + Halley norm_ppf, which agrees
// with the bisection to 1e-12 only up to N = 1e3; a row of record must agree for any N
// (mining campaigns' registry counts can take N_tot past 1e4).

#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>

#include "atx/core/types.hpp"
#include "atx/engine/eval/deflated_sharpe.hpp"

namespace atx::engine::eval {

enum class HouseDsrMethod : atx::u8 {
  HouseV1 = 1,  // N = construction count, V = window cell variance (the verdict's DSR)
  TotalV1 = 2,  // N = N_tot of the ledger, same V (DSR_tot)
  CrossV1 = 3,  // V = cross-cell variance of the listed cells
  LoNullV1 = 4, // V = Lo (2002) single-cell sampling variance
  OncV1 = 5,    // N = ONC clusters, V = cluster IVP-series SR variance
};

[[nodiscard]] constexpr std::string_view house_dsr_method_name(HouseDsrMethod m) noexcept {
  switch (m) {
  case HouseDsrMethod::HouseV1: return "house_v1";
  case HouseDsrMethod::TotalV1: return "tot";
  case HouseDsrMethod::CrossV1: return "cross";
  case HouseDsrMethod::LoNullV1: return "lo";
  case HouseDsrMethod::OncV1: return "onc";
  }
  return "unknown";
}

// The house's standard-normal CDF, 0.5 erfc(-x / sqrt 2): the retired Python norm_cdf, in its
// operation order (a division by sqrt 2, where stats_ext's norm_cdf multiplies by 1 / sqrt 2).
[[nodiscard]] inline atx::f64 house_norm_cdf(atx::f64 x) noexcept {
  return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

// The house's standard-normal quantile (ruling B2-9): bisection on house_norm_cdf over
// [-40, 40], at most 200 halvings, stopping once hi - lo <= 1e-15 max(1, |mid|), returning the
// midpoint. Each step is one IEEE-754 operation of the retired Python norm_ppf, so the result
// is the same double. p outside (0, 1) is the Python's ValueError; here it is NaN.
[[nodiscard]] inline atx::f64 house_norm_ppf(atx::f64 p) noexcept {
  if (!(p > 0.0 && p < 1.0)) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 lo = -40.0;
  atx::f64 hi = 40.0;
  for (int i = 0; i < 200; ++i) {
    const atx::f64 mid = 0.5 * (lo + hi);
    if (house_norm_cdf(mid) < p) {
      lo = mid;
    } else {
      hi = mid;
    }
    if (hi - lo <= 1e-15 * std::fmax(1.0, std::fabs(mid))) {
      break;
    }
  }
  return 0.5 * (lo + hi);
}

// SR*_N = sqrt(V) [(1 - gamma) PhiInv(1 - 1/N) + gamma PhiInv(1 - 1/(N e))] with the house
// quantile (the retired Python expected_max_sr, same operation order). Precondition: n >= 2.
[[nodiscard]] inline atx::f64 house_expected_max_sr(atx::f64 variance, atx::usize n) noexcept {
  const atx::f64 nf = static_cast<atx::f64>(n);
  return std::sqrt(variance) *
         ((1.0 - kEulerMascheroni) * house_norm_ppf(1.0 - 1.0 / nf) +
          kEulerMascheroni * house_norm_ppf(1.0 - 1.0 / (nf * std::numbers::e)));
}

// SR* of N trials at variance V and the DSR at it (per-session units). dsr is empty when
// the PSR is undefined (the variance term 1 - g3 SR + (g4 - 1) SR^2 / 4 is not positive, or
// T < 2): the Python row's None, with SR* still reported.
struct HouseDsr {
  atx::f64 sr0_daily{};
  std::optional<atx::f64> dsr;
};

// Precondition (the caller's checks): n >= 2 and the moments are defined.
[[nodiscard]] inline HouseDsr house_dsr(atx::f64 sr_daily, atx::usize sessions, atx::f64 skew,
                                        atx::f64 kurtosis_raw, atx::usize n,
                                        atx::f64 variance) noexcept {
  const atx::f64 sr0 = house_expected_max_sr(variance, n);
  const atx::f64 dsr = probabilistic_sharpe(sr_daily, sr0, sessions, skew, kurtosis_raw - 3.0);
  HouseDsr out{sr0, std::nullopt};
  if (!std::isnan(dsr)) {
    out.dsr = dsr;
  }
  return out;
}

// The Lo (2002) sampling variance (1 + SR^2 / 2) / T of one cell's per-session SR.
[[nodiscard]] inline atx::f64 lo_sampling_variance(atx::f64 sr_daily,
                                                   atx::usize sessions) noexcept {
  return (1.0 + sr_daily * sr_daily / 2.0) / static_cast<atx::f64>(sessions);
}

} // namespace atx::engine::eval
