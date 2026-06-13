#include "atx/engine/validation/bias_audit.hpp"

#include <optional> // std::nullopt (deflated_sharpe selection benchmark)
#include <span>     // std::span
#include <vector>   // std::vector (synthetic performance matrix)

#include "atx/core/types.hpp"                  // atx::f64, atx::usize
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, probabilistic_sharpe
#include "atx/engine/eval/pbo.hpp"             // eval::pbo_cscv

namespace atx::engine::validation {

bool catches_overfit_synthetic() {
  // (1) PBO synthetic: disjoint single-sub-period spikes, flat-negative else.
  constexpr atx::usize kN = 16U, kS = 8U, kT = 64U, kW = kT / kS;
  constexpr atx::f64 kSpike = 1.0;   // in-sample-only edge on a candidate's own window
  constexpr atx::f64 kFlatNeg = -0.02; // dead everywhere else (so it dies OOS)
  std::vector<atx::f64> m(kN * kT);
  for (atx::usize c = 0U; c < kN; ++c) {
    for (atx::usize t = 0U; t < kT; ++t) {
      m[c * kT + t] = (t / kW == c % kS) ? kSpike : kFlatNeg;
    }
  }
  const atx::f64 pbo = eval::pbo_cscv(std::span<const atx::f64>{m}, kN, kS).pbo;

  // (2) DSR synthetic: single-test "significant", deflated kill across N trials.
  constexpr atx::f64 kSr = 0.12;
  constexpr atx::usize kT2 = 250U, kTrials = 1000U;
  const atx::f64 single_test = eval::probabilistic_sharpe(kSr, 0.0, kT2, 0.0, 0.0);
  const atx::f64 dsr = eval::deflated_sharpe(kSr, kT2, 0.0, 0.0, kTrials, std::nullopt).dsr;

  // Thresholds are gate-defining, not tunable: HIGH PBO, single-test pass, deflated fail.
  constexpr atx::f64 kPboHigh = 0.5;
  constexpr atx::f64 kSingleSignificant = 0.9;
  constexpr atx::f64 kDeflatedKill = 0.5;
  return (pbo > kPboHigh) && (single_test > kSingleSignificant) && (dsr < kDeflatedKill);
}

} // namespace atx::engine::validation
