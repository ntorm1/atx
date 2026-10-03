#pragma once

// atx::engine::research::admission -- the traded-horizon columns of the admission table (platform
// P9 B1, main review F-3; REPORT ONLY: they gate, order and weight nothing).
//
// The gate measures a one-day factor return while the book holds theta .05 (mean lag about 19
// sessions). Beside each gate row this prints the same factor at the horizon the book trades:
// h_k(d), the K-P9-4 factor_h21.f64 series -- the decision-d book q_k(d) held over the 21 sessions
// after its fill session, sum_i q_k(d)_i sum_{j<21} r_i(d+2+j) -- whose values overlap by 20
// sessions. Over the live TRAIN decisions of s_k * h_k:
//   ic_h21        the mean (the factor-return "IC" in the sense ic-shrink reads train_mean)
//   ic_h21_hac_t  its HAC t at the 21-session overlap: eval::hac::mean_tstat(x, HorizonAwareV3,
//                 21) -- uniform kernel at lag max(20, Newey-West rule of thumb), the n / (n - 1)
//                 correction, Bartlett at the same lag when the uniform sum is not positive
//                 (cancellation guarded); undefined when n <= 21.
// s_k = 0 (no prior): both nullopt, as the fitter's report-only f_theta columns.

#include <optional>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::admission {

inline constexpr usize kTradedHorizonSessions = 21;

struct TradedHorizonRow {
  usize days{};              // live TRAIN decisions of h_k
  std::optional<f64> ic;     // mean of s_k * h_k
  std::optional<f64> hac_t;  // its HAC t at the 21-session overlap
};

// factors_h21: decisions x candidates, decision-major (NaN = not live); train_mask: decisions;
// prior_signs: candidates (0 or 1). Err(InvalidArgument) on inconsistent geometry or a mask byte
// above 1; Err(OutOfRange) when allocation fails.
[[nodiscard]] core::Result<std::vector<TradedHorizonRow>>
traded_horizon(usize candidates, usize decisions, std::span<const f64> factors_h21,
               std::span<const u8> train_mask, std::span<const i32> prior_signs);

} // namespace atx::engine::research::admission
