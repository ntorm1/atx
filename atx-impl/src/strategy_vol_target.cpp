// vol-target-v1 (platform v8 Y, lane YCOMB): the published text and parameter block. The rule is
// stated in strategy_vol_target.hpp and engine::book::vol_target.hpp.
#include "strategy_vol_target.hpp"

#include <string>
#include <nlohmann/json.hpp>
#include "atx/engine/book/risk_target.hpp"
#include "atx/engine/book/vol_target.hpp"

namespace atx::impl::strategy::vol_target {

std::string declaration() {
  return "vol-target-v1 (platform v8 Y, lane YCOMB; the risk-managed form of the leverage cell): "
         "at every decision d each book's aim leverage is L_t = clip(L x sigma_ref_t / "
         "sigma_hat_t, 1, L) with L = --aim-leverage (the cap: the fixed leverage it replaces); "
         "sigma_hat_t = sqrt(252 x book_variance) of the book's current weights (its DECIDE's, "
         "after the session's fills) scaled to gross 1 over the whole book, on the atx-risk-v1 "
         "row d of the pinned store (--risk-model; the names with a risk row, no specific "
         "ceiling), risk-target-v1's forecast; sigma_ref_t = the mean of the book's estimates so "
         "far, sigma_hat_t included (point in time; the first estimate gives L). Estimated at "
         "the book's first decision with a forecast, a book that is not flat and a positive "
         "variance, then at the first such decision at least 21 sessions after the previous "
         "estimate, held between estimates, L before the first. Registered constants: floor 1, "
         "cadence 21, annualisation 252. aim-partial-v5 moves toward L_t x desired; the shared "
         "desired target is unchanged; every book runs on its own state, the warm-up included, "
         "each pass from a clean state";
}

nlohmann::json parameters_json() {
  namespace eb = atx::engine::book;
  return nlohmann::json{
      {"rule", rule_id},
      {"declaration", declaration()},
      {"floor", eb::vol_target_floor},
      {"cap", "--aim-leverage"},
      {"cadence_sessions", eb::vol_target_cadence},
      {"periods_per_year", eb::risk_target_periods_per_year},
      {"reference", "mean of the book's estimates so far, the current one included"},
      {"series", "vol_target.csv: one row per scored decision and book of the main pass "
                 "(sigma_hat, sigma_ref and L_t in force)"}};
}

} // namespace atx::impl::strategy::vol_target
