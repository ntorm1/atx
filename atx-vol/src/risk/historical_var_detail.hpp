#pragma once

// Private seam of historical_var (lane D): the per-pair evaluate + explain step and the
// admission rule. historical_var builds every workspace and frame itself, so the public entry
// point can never be handed a hostile one; these are exposed so a test can drive the step
// with a workspace bound to another book and check that the failure is the call's error, not
// a silently dropped scenario.

#include <cstdint>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/risk/historical_var.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"

namespace atx::vol::historical_var_detail {

enum class PairOutcome : std::uint8_t {
  Explained = 0, // `out` holds the pair's explain
  Refused = 1,   // a scenario-level refusal: the scenario is not admitted
};

// Evaluate `prepared` on both states of `scenario` with `kernel` and explain base -> shifted
// into `out`. If the pair moves a rate while `needs.rho` is off, the base frame is
// re-evaluated with rho so the move lands in pnl_rho.
//
// Refused: either state's TimeSpec refuses one of the book's expiries, or the base TimeSpec
// refuses the step (VolTime OutOfRange). Checked here, before the kernel runs, with the
// kernel's and explain's own tests, because both report those refusals as InvalidArgument,
// the same code as a structural error. Any error left after that check is structural (a
// workspace or frame bound to another book, an unprepared book) and is returned: as a
// scenario-level outcome it would drop every scenario without a trace.
//
// `ws`, `base` and `shifted` must already be bound to `prepared` (the caller re-binds them).
[[nodiscard]] Result<PairOutcome>
evaluate_pair(const RiskKernel &kernel, const PreparedRiskBook &prepared,
              const HistoricalVarScenario &scenario, const RiskNeeds &needs,
              const ExplainOptions &opts, RiskWorkspace &ws, RiskFrame &base, RiskFrame &shifted,
              ExplainResult &out);

// Admission of an explained pair (historical_var.hpp: no exclusions, or an excluded premium
// BELOW `max_excluded_premium_frac` of |pv_base|): at least one leg in the totals, and when a
// leg is excluded every excluded premium must be known and their sum strictly below the
// fraction. explain books an excluded leg whose BASE value is unavailable at premium 0; that
// premium is unknown, not zero, so a pair with such a leg is never admitted.
[[nodiscard]] bool admit(const ExplainResult &explained, double max_excluded_premium_frac) noexcept;

} // namespace atx::vol::historical_var_detail
