#pragma once

#include <limits>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/cost/borrow.hpp"

namespace atx::engine::cost {

enum class BorrowTierRule : atx::u8 { PublicPredictorPriorV1 = 1 };
enum class BorrowTier : atx::u8 { Unavailable = 0, GeneralCollateral = 1, Warm = 2, Special = 3 };
struct BorrowTierRecipe {
  BorrowTierRule rule{BorrowTierRule::PublicPredictorPriorV1};
  // Explicit scenario priors, NOT fitted coefficients or observed lender quotes.
  atx::f64 small_cap_usd{1'000'000'000.0};
  atx::f64 low_raw_price_usd{5.0};
  atx::f64 high_short_interest_fraction{0.10};
  atx::f64 young_ipo_calendar_days{365.0};
  atx::f64 gc_annual_fraction{0.00275};
  atx::f64 warm_annual_fraction{0.03};
  atx::f64 special_annual_fraction{0.275};
};
struct BorrowPredictors {
  bool available{false};
  atx::i64 available_at_ns{}; // maximum publication clock of ALL four predictors
  atx::f64 market_cap_usd{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 raw_price_usd{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 short_interest_to_float{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 ipo_age_calendar_days{std::numeric_limits<atx::f64>::quiet_NaN()};
};
struct BorrowTierEstimate {
  BorrowTier tier{BorrowTier::Unavailable};
  atx::f64 annual_fraction{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::u8 risk_flags{};
  atx::i64 available_at_ns{};
  // A modeled rate never certifies locate availability or a lender's inventory.
};
// Zero risk flags -> GC; one -> warm; two or more -> special. Missing predictors
// remain unavailable. The 25-30bp / 1-5% / 5-50% bands are planning assumptions;
// default point priors lie within them and are not an empirical prediction claim.
[[nodiscard]] atx::core::Result<BorrowTierEstimate> estimate_borrow_tier(
    const BorrowPredictors&, atx::i64 decision_time_ns, const BorrowTierRecipe& = {});
[[nodiscard]] atx::core::Result<atx::f64> annual_fraction_to_bps(atx::f64 annual_fraction);
[[nodiscard]] atx::core::Result<atx::f64> annual_bps_to_fraction(atx::f64 annual_bps);
// Uses the existing explicit day-count/elapsed-time accrual API. Borrow is a
// holding charge, never included in CostSurface's one-way trade quote.
[[nodiscard]] atx::core::Result<BorrowModel> as_borrow_model(
    const BorrowTierEstimate&, DayCount day_count = DayCount::D360);

} // namespace atx::engine::cost
