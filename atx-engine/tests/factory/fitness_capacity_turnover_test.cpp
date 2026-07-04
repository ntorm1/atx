// atx::engine::factory — S4 (p9): capacity + turnover as first-class NSGA
// objectives. Unit + end-to-end proofs for the two new OPT-IN objective columns
// kObjCapacity (sqrt-law impact capacity score) and kObjTurnover (signal AR(1)
// autocorrelation / alpha-decay persistence score). Both reuse frozen fitters
// (cost::round_trip_cost_bps + cost::capacity_point; alpha::detail::ou_ar1_fit) —
// ZERO new estimator math.

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/cost/calibration.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/search_driver.hpp"

namespace atxtest_fitness_capacity_turnover_test {
using atx::f64;
using atx::usize;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::Panel;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::factory::capacity_sqrt_law_score;
namespace cost = atx::engine::cost;

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// Deterministic alternating +/-1% oscillation (sigma>0, no RNG) -- ONLY the
// "volume" column differs between the two fixtures (ADV, hence participation).
[[nodiscard]] Panel capacity_panel(f64 volume_level) {
  constexpr usize kDates = 10;
  std::vector<f64> close(kDates);
  std::vector<f64> volume(kDates, volume_level);
  f64 px = 100.0;
  for (usize t = 0; t < kDates; ++t) {
    px *= (t % 2 == 0) ? 1.01 : 0.99;
    close[t] = px;
  }
  return make_panel(kDates, 1, {"close", "volume"}, {close, volume});
}

[[nodiscard]] AlphaStreams full_weight_strm(usize periods, f64 per_period_edge) {
  AlphaStreams s;
  s.n_alphas_ = 1;
  s.n_periods_ = periods;
  s.n_instruments_ = 1;
  s.pnl_flat.assign(periods, per_period_edge); // constant small positive OOS edge
  s.pos_flat.assign(periods, 0.0);
  s.pos_flat[periods - 1] = 1.0; // full weight, last period
  return s;
}

// A CalibratedCost with the default √-impact coefficients but ZERO fixed slippage.
// WHY: the default SlippageCfg{}.bps (5.0) charges a round-trip 10 bps that is
// ADV-INDEPENDENT — it floors EVERY alpha's net edge equally, so for a modest
// gross edge the capacity score collapses to its grid-floor constant for ALL ADV
// levels, masking the very ADV signal the objective exists to measure. Isolating
// the √-law impact (the ONLY ADV-dependent cost term) is what makes the ADV
// discrimination observable. The frozen cost model is untouched — this only
// chooses the test input coefficients.
[[nodiscard]] cost::CalibratedCost impact_only_cost(ImpactCfg imp) {
  SlippageCfg sl{};
  sl.bps = 0.0; // zero fixed slippage -> cost is pure sqrt-law impact
  return cost::CalibratedCost{imp, sl, cost::FitReport{}};
}

// ===========================================================================
//  S4-0 — enum/constant + defaults pin (frozen-prefix, append-only).
// ===========================================================================
TEST(FitnessCapacityTurnover, DefaultsAndEnumPin) {
  using namespace atx::engine::factory;
  static_assert(kMaxObjectives == 9, "S4 must grow kMaxObjectives 7->9");
  static_assert(kObjCapacity == 7, "kObjCapacity must be appended at slot 7");
  static_assert(kObjTurnover == 8, "kObjTurnover must be appended at slot 8");
  const FitnessCfg fc{};
  EXPECT_FALSE(fc.capacity_objective);
  EXPECT_FALSE(fc.turnover_objective);
  const SearchConfig sc{};
  EXPECT_FALSE(sc.capacity_objective);
  EXPECT_FALSE(sc.turnover_objective);
  const FitnessReport fr{};
  EXPECT_EQ(fr.capacity_score, 0.0);
  EXPECT_EQ(fr.turnover_autocorr, 0.0);
}

// ===========================================================================
//  S4-1 — capacity_sqrt_law_score (the kObjCapacity column). Unwired helper.
// ===========================================================================
TEST(CapacityObjective, HighAdvLowImpactScoresAboveLowAdv) {
  // Two single-instrument books identical but for ADV (the "volume" column). The
  // deeper-ADV book bears LOWER sqrt-law impact at any AUM, so its net-edge curve
  // crosses zero at a HIGHER capacity AUM -> a strictly higher bounded score. Both
  // sit in the interior (0,1) of the transform (neither floored nor saturated).
  const Panel high_adv = capacity_panel(1.0e6); // deep ADV -> low participation
  const Panel low_adv = capacity_panel(1.0e5);  // thin ADV -> high participation
  const AlphaStreams strm = full_weight_strm(10, 0.002); // 20 bps gross edge/period
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{0.8, 0.5, 0.3});
  constexpr f64 kTargetAum = 1.0e6;

  const f64 score_high = capacity_sqrt_law_score(strm, high_adv, cc, kTargetAum);
  const f64 score_low = capacity_sqrt_law_score(strm, low_adv, cc, kTargetAum);

  EXPECT_GT(score_high, score_low)
      << "the deep-ADV/low-impact book must score a HIGHER capacity headroom";
  EXPECT_GT(score_low, 0.0) << "sanity: the thin-ADV book still has SOME capacity";
  EXPECT_LT(score_high, 1.0) << "deep-ADV book still crosses within the grid (not saturated)";
  EXPECT_LE(score_low, 1.0);
}

TEST(CapacityObjective, NonPositiveTargetAumIsADocumentedNoOp) {
  const Panel panel = capacity_panel(1.0e6);
  const AlphaStreams strm = full_weight_strm(10, 0.002);
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{0.8, 0.5, 0.3});
  EXPECT_EQ(capacity_sqrt_law_score(strm, panel, cc, 0.0), 0.0);
  EXPECT_EQ(capacity_sqrt_law_score(strm, panel, cc, -1.0), 0.0);
}

TEST(CapacityObjective, PureFunction_TwiceRunByteIdentical) {
  const Panel panel = capacity_panel(5.0e5);
  const AlphaStreams strm = full_weight_strm(10, 0.002);
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{0.6, 0.6, 0.2});
  const f64 a = capacity_sqrt_law_score(strm, panel, cc, 2.5e5);
  const f64 b = capacity_sqrt_law_score(strm, panel, cc, 2.5e5);
  EXPECT_EQ(a, b);
}

TEST(CapacityObjective, NeverInfOrNaN) {
  // A near-zero impact coefficient (Y~0) -> the net-edge curve never crosses zero
  // on the grid -> capacity_point returns +inf -- the EXACT case the bounded
  // transform must absorb without ever handing NSGA an inf/NaN objective.
  const Panel panel = capacity_panel(1.0e9);
  const AlphaStreams strm = full_weight_strm(10, 0.001);
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{1.0e-9, 0.5, 0.0});
  const f64 score = capacity_sqrt_law_score(strm, panel, cc, 1.0e6);
  EXPECT_TRUE(std::isfinite(score));
  EXPECT_GE(score, 0.0);
  EXPECT_LE(score, 1.0);
}

} // namespace atxtest_fitness_capacity_turnover_test
