#include "fitting/raw_price_targets.hpp"

#include <bit>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/american_region.hpp"

namespace atx::vol {
namespace {

detail::RawPriceDraft target_draft(bool recover = false) {
  QuoteSliceConfig config;
  config.repair.certificate = QuoteSliceCertificate::Box;
  config.repair.terminal_floor_recovery = recover;
  const std::vector<QuoteSliceQuote> quotes{
      {90, Side::Call, 8, recover ? 9.0 : 12.0}, {90, Side::Put, 0, 2},
      {100, Side::Call, 3, 7}, {100, Side::Put, 3, 7},
      {110, Side::Call, 0, 2}, {110, Side::Put, 8, 12}};
  auto result = detail::make_raw_price_draft({100, .5, .5, 0, 0, 0}, quotes, config);
  EXPECT_TRUE(result.draft) << result.error;
  if (!result.draft) return {};
  return std::move(*result.draft);
}

TEST(RawPriceTargets, OriginalMidpointRemainsBitIdentical) {
  auto draft = target_draft();
  const auto before = draft.nodes;
  ASSERT_TRUE(detail::select_raw_price_targets(draft, detail::RawPriceDraftTarget::OriginalMid));
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(draft.nodes[i].call),
              std::bit_cast<std::uint64_t>(before[i].call));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(draft.nodes[i].put),
              std::bit_cast<std::uint64_t>(before[i].put));
  }
}

TEST(RawPriceTargets, FeasibleMidUsesProvenFloorsAndKeepsRawProvenance) {
  auto draft = target_draft();
  const auto boxes = draft.boxes;
  ASSERT_TRUE(detail::select_raw_price_targets(draft, detail::RawPriceDraftTarget::FeasibleBoxMid));
  EXPECT_DOUBLE_EQ(draft.nodes[0].call, 11);
  EXPECT_DOUBLE_EQ(draft.nodes[0].put, 1);
  EXPECT_DOUBLE_EQ(draft.nodes[2].put, 11);
  EXPECT_DOUBLE_EQ(draft.raw_targets[0].call, 10);
  EXPECT_DOUBLE_EQ(draft.original_quotes[0].bid, 8);
  EXPECT_DOUBLE_EQ(draft.original_quotes[0].ask, 12);
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    EXPECT_EQ(draft.boxes[i].call_lo, boxes[i].call_lo);
    EXPECT_EQ(draft.boxes[i].call_hi, boxes[i].call_hi);
    EXPECT_EQ(draft.boxes[i].put_lo, boxes[i].put_lo);
    EXPECT_EQ(draft.boxes[i].put_hi, boxes[i].put_hi);
  }
  ASSERT_TRUE(detail::select_raw_price_targets(draft, detail::RawPriceDraftTarget::OriginalMid));
  EXPECT_DOUBLE_EQ(draft.nodes[0].call, draft.raw_targets[0].call);
}

TEST(RawPriceTargets, RecoveredAskOnlyUsesTheExistingDeclaredInterval) {
  auto draft = target_draft(true);
  const double high = draft.boxes[0].call_hi;
  ASSERT_TRUE(detail::select_raw_price_targets(draft, detail::RawPriceDraftTarget::FeasibleBoxMid));
  EXPECT_DOUBLE_EQ(draft.nodes[0].call, .5 * 10 + .5 * high);
  EXPECT_GE(draft.nodes[0].call, 10);
  EXPECT_LE(draft.nodes[0].call, high);
  EXPECT_DOUBLE_EQ(draft.raw_targets[0].call, 8.5);
  EXPECT_EQ(draft.recovery.raised_asks, 1u);
  EXPECT_DOUBLE_EQ(draft.original_quotes[0].ask, 9);
}

TEST(RawPriceTargets, InvalidOrAlreadyProjectedDraftCannotBeRetargeted) {
  auto draft = target_draft();
  ASSERT_FALSE(draft.nodes.empty());
  const auto before = draft.nodes;
  EXPECT_FALSE(detail::select_raw_price_targets(draft,
      static_cast<detail::RawPriceDraftTarget>(255)));
  EXPECT_EQ(draft.nodes[0].call, before[0].call);
  draft.terminal_prepaid.clear();
  EXPECT_FALSE(detail::select_raw_price_targets(draft, detail::RawPriceDraftTarget::FeasibleBoxMid));
  EXPECT_EQ(draft.nodes[0].call, before[0].call);
  draft = target_draft();
  ASSERT_EQ(detail::project_raw_price_drafts(std::span(&draft, 1)).outcome,
            JointProjectionOutcome::Feasible);
  const auto projected = draft.nodes;
  EXPECT_FALSE(detail::select_raw_price_targets(draft, detail::RawPriceDraftTarget::FeasibleBoxMid));
  EXPECT_EQ(draft.nodes[0].call, projected[0].call);
}

TEST(RawPriceTargets, FloorConflictWithoutDeclaredRecoveryDoesNotWidenABox) {
  auto draft = target_draft();
  const auto before = draft.nodes;
  draft.boxes[0].call_hi = 9;
  EXPECT_FALSE(detail::select_raw_price_targets(draft, detail::RawPriceDraftTarget::FeasibleBoxMid));
  EXPECT_EQ(draft.nodes[0].call, before[0].call);
  EXPECT_EQ(draft.boxes[0].call_hi, 9);
  EXPECT_EQ(draft.recovery.raised_asks, 0u);
}

TEST(RawPriceTargets, TwoRecoveredFloorTargetsHaveAuditableClassificationChanges) {
  // Frozen audit cases, never optimizer inputs. The old and new fitted targets
  // differ inside the already declared floor-recovery allowance.
  struct Case {
    int row_id;
    double strike, old_price, old_sigma, new_price;
  };
  const std::array<Case, 2> cases{{
      {27418697, 120, 655.9104557785998, .035424785275844375, 655.9104556897006},
      {27419115, 230, 558.005390252233, .13667756557476937, 558.005390232761}}};
  constexpr double spot = 773;
  constexpr double years = 2.257885694503784;
  constexpr double rate = .0515890046954155;
  constexpr double yield = .005931800012831142;
  const auto options = QuoteSliceConfig{}.al_opts;
  for (const auto &row : cases) {
    const auto payoff = [&](double time) {
      return spot * std::exp(-yield * time) - row.strike * std::exp(-rate * time);
    };
    const double stationary = std::log(rate * row.strike / (yield * spot)) / (rate - yield);
    double zero = std::max({0.0, payoff(0), payoff(years)});
    if (stationary > 0 && stationary < years) zero = std::max(zero, payoff(stationary));
    const double band = 1e-9 * spot + 1e-12; // positive yield => call ceiling is spot
    const auto floor = andersen_lake(spot, row.strike, years, kIvMin, rate, yield,
                                      Side::Call, options);
    const auto old = andersen_lake_classified(spot, row.strike, years, row.old_sigma,
        rate, yield, Side::Call, options, AlExerciseRegion::KernelFloor);
    ASSERT_TRUE(floor && old);
    EXPECT_NEAR(*floor, zero, 1e-10);
    EXPECT_GT(row.old_price, zero + band);
    EXPECT_LE(row.new_price, zero + band);
    IvOpts inverse;
    inverse.al_opts = options;
    inverse.refuse_non_monotone = true;
    const auto old_search = american_implied_vol(row.old_price, spot, row.strike, years,
                                                 rate, yield, Side::Call, inverse);
    const auto new_search = american_implied_vol(row.new_price, spot, row.strike, years,
                                                 rate, yield, Side::Call, inverse);
    ASSERT_EQ(new_search.status, IvStatus::Ok);
    EXPECT_EQ(new_search.iv, kIvMin);
    const nlohmann::json receipt{{"row_id", row.row_id}, {"zero_vol_supremum", zero},
        {"stationary_time", stationary}, {"ceiling", spot}, {"band", band},
        {"cold_price_at_sigma_floor", *floor}, {"old_price", row.old_price},
        {"old_sigma", row.old_sigma}, {"old_cold_price", old->price},
        {"old_exercise_class", static_cast<int>(old->cls)},
        {"old_cold_residual", old->price - row.old_price}, {"new_price", row.new_price},
        {"old_search_status", static_cast<int>(old_search.status)},
        {"old_search_iv", old_search.iv},
        {"new_search_status", static_cast<int>(new_search.status)},
        {"new_search_iv", new_search.iv}};
    RecordProperty("row_" + std::to_string(row.row_id), receipt.dump());
  }
}

} // namespace
} // namespace atx::vol
