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

} // namespace atxtest_fitness_capacity_turnover_test
