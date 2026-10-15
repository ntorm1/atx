#pragma once
// Composition schedule mom-volman-v1 (P9 lane AL-COMB, cell P9-M; registration
// scripts/specs/p9/rules/mom-volman-v1.json, DEC-16: read after Y-2). Hypothesis (Barroso and
// Santa-Clara 2015, "Momentum has its moments", JFE 116(1); Daniel and Moskowitz 2016, JFE 122(2);
// real-time form Cederburg, O'Doherty, Wang and Yan 2020, JFE 138(1)): momentum's own realised
// volatility forecasts its next-month volatility but not its mean, so scaling the momentum sleeve
// to a constant volatility raises its Sharpe (WML .53 -> .97); among the anomalies whose volatility
// management fails in real time, momentum is the exception. Only the price_momentum theme's mass
// moves; Y-1 (book volatility) and theme-tsmom-v1 (the sign of every theme's trailing return) are
// other objects. Registration (every constant fixed before any read: BSC's six-month window, the
// house's monthly step, the house's decision-to-return lag):
//   1. The sleeve r(d) is the price_momentum theme's sleeve over the parent fit's role decisions,
//      as theme-tsmom-v1's step 1 forms it (sum over the theme's members of (w_k / W_t) s_k f_k(d),
//      flat 0, 0 outside the TRAIN mask; composition_theme_tsmom.sleeves). The fitter only records
//      it, with the decision sessions, in the theme_schedule block (composition_mom_volman.py);
//      every estimate below is this file's.
//   2. Block b starts at decision index j_b = 126 + 2 - 1 + 21 b while j_b < n (n decisions).
//   3. sigma_b = sqrt(mean of r(d)^2 over d = j_b - 2 - 126 + 1 .. j_b - 2): the realised
//      volatility of the 126 decisions ending two before the block (BSC's estimate: squared
//      returns, no mean).
//   4. target_b = (sigma_0 + ... + sigma_b) / (b + 1): the running mean of the sleeve's own
//      estimates, the real-time constant of Cederburg et al. (no full-sample constant).
//   5. scale_b = target_b / sigma_b; the block's masses are the parent's W_t for every theme except
//      price_momentum, W_pm scale_b. Before the first block the parent's masses. No cap.
// The IC runner applies the masses through the theme schedule theme-tsmom-v1 uses
// (IcComposition::schedule_theme_masses): the block is the theme_schedule block with rule
// mom-volman-v1, so the two schedules exclude each other. It rides on a theme_standardise rule with
// rerank true and is refused with theme_residualise.
// The registered constants are defined once, in the engine (P9 lane D3W-PB, review D3W S-2): the
// C++ composition fit records the sleeve against the same bounds.
#include <span>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/composition/mom_volman.hpp"

namespace atx::impl::strategy {
using atx::engine::research::composition::mom_volman_rule;
using atx::engine::research::composition::mom_volman_theme;
using atx::engine::research::composition::mom_volman_lookback;
using atx::engine::research::composition::mom_volman_lag;
using atx::engine::research::composition::mom_volman_step;
using atx::engine::research::composition::mom_volman_max_decisions;

struct MomVolmanBlock {
  atx::usize decision{};  // j_b, an index into the sleeve
  atx::f64 sigma{};       // step 3
  atx::f64 target{};      // step 4
  atx::f64 scale{};       // step 5: target / sigma
};
// Steps 2-5 on one recorded sleeve (decision order). Err (InvalidArgument, "mom-volman-v1: ...")
// for a sleeve of at most 127 or more than mom_volman_max_decisions values, a non-finite value, a
// block whose sigma is not finite and > 0, or one whose target or scale is not finite (no cap is
// registered: a tiny positive sigma gives a large finite scale, accepted).
[[nodiscard]] atx::core::Result<std::vector<MomVolmanBlock>> mom_volman_blocks(
    std::span<const atx::f64> sleeve);
} // namespace atx::impl::strategy
