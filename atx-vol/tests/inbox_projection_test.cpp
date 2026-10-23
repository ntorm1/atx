// Lane C4 (r6, 2026-10-23): the exact in-box projection rung (QuoteSliceRepairConfig::
// inbox_projection, src/fitting/inbox_projection.hpp). The C3 review found 31 of 33 SPY
// expiries admitting a certified grid strictly inside every quote while the constructive box
// rung (certify_american_box: per-side Dykstra, then create()) refused them. The library
// fixtures here are three-strike expiries whose fitted grid fails the certificate: one the box
// rung misses (its per-side projections break the affine call-minus-put bound, which only a
// joint projection can respect) and whose in-box grid exists, and one per typed conflict.
#include "fitting/inbox_projection.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

using detail::classify_box_refusal;
using detail::project_american_in_box;

// Spot 100, zero rate (discount 1), prepaid value in [98, 100] (a dividend payer): the
// affine bound is 98 - K <= call - put <= 100 - K.
constexpr AmericanPriceBounds kBounds{100.0, 1.0, 1.0, 98.0, 100.0};

std::vector<AmericanPriceNode> fitted_grid() {
  // The call is 0.5 above its chord at K 100 and call - put = 1 there, above 100 - K = 0:
  // AmericanPriceSlice::create refuses it.
  return {{90.0, 11.0, 2.0}, {100.0, 6.5, 5.5}, {110.0, 1.0, 12.0}};
}

std::vector<AmericanPriceBox> quote_boxes() {
  return {{10.5, 11.5, 1.5, 2.5}, {5.8, 6.8, 5.0, 6.0}, {0.5, 1.5, 11.5, 12.5}};
}

bool strictly_inside(double value, double lo, double hi) { return lo < value && value < hi; }

TEST(InboxProjection, ARefusedFittedGridIsProjectedStrictlyInsideItsBoxes) {
  const auto fitted = fitted_grid();
  const auto boxes = quote_boxes();
  ASSERT_FALSE(AmericanPriceSlice::create(fitted, kBounds).has_value());
  // The constructive box rung misses this expiry: each side's own projection (the call
  // lowered onto its chord, the put untouched) leaves call - put above 100 - K at K 100.
  const auto box = certify_american_box(fitted, boxes, kBounds);
  ASSERT_FALSE(box.has_value());
  EXPECT_NE(box.error().message().find("affine"), std::string::npos) << box.error().message();
  QuoteSliceInboxRecord record;
  const auto inbox =
      project_american_in_box(fitted, boxes, kBounds, box.error().message(), record);
  ASSERT_TRUE(inbox.has_value()) << inbox.error().to_string();
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::Applied) << to_string(record.outcome);
  EXPECT_EQ(record.box_refusal, QuoteSliceBoxRefusal::CertificateRefused);
  EXPECT_EQ(record.conflict, QuoteSliceInboxConflict::None);
  EXPECT_EQ(record.free_sides, 6u);
  EXPECT_EQ(record.fitted_outside, 0u); // every fitted price is inside its quote here
  EXPECT_GT(record.phase2_steps, 0u);
  EXPECT_LT(record.max_mid_half_spreads, 1.0);
  EXPECT_GT(record.max_move_half_spreads, 0.0);
  const auto nodes = inbox->slice.nodes();
  ASSERT_EQ(nodes.size(), fitted.size());
  EXPECT_TRUE(AmericanPriceSlice::create(nodes, kBounds).has_value());
  ASSERT_EQ(inbox->moved.size(), fitted.size());
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(nodes[i].strike, fitted[i].strike);
    EXPECT_TRUE(strictly_inside(nodes[i].call, boxes[i].call_lo, boxes[i].call_hi)) << i;
    EXPECT_TRUE(strictly_inside(nodes[i].put, boxes[i].put_lo, boxes[i].put_hi)) << i;
    EXPECT_EQ((inbox->moved[i] & 1u) != 0u, nodes[i].call != fitted[i].call) << i;
    EXPECT_EQ((inbox->moved[i] & 2u) != 0u, nodes[i].put != fitted[i].put) << i;
  }
  // The joint projection respects the coupling the per-side projections broke.
  EXPECT_LE(nodes[1].call - nodes[1].put, 100.0 - 100.0);
  EXPECT_GT(inbox->moved_nodes, 0u);
  EXPECT_GT(inbox->max_price_move, 0.0);
}

TEST(InboxProjection, AFittedGridAlreadyInsideTheSetComesBackWithinTheBarrierBias) {
  // A feasible fitted grid strictly inside its boxes: the projection is the grid itself,
  // up to the interior-point bias (the barrier keeps every iterate off the boundary).
  const std::vector<AmericanPriceNode> fitted{
      {90.0, 11.0, 2.0}, {100.0, 5.9, 5.95}, {110.0, 1.0, 12.0}};
  ASSERT_TRUE(AmericanPriceSlice::create(fitted, kBounds).has_value());
  QuoteSliceInboxRecord record;
  const auto inbox = project_american_in_box(fitted, quote_boxes(), kBounds, "", record);
  ASSERT_TRUE(inbox.has_value()) << inbox.error().to_string();
  EXPECT_EQ(record.box_refusal, QuoteSliceBoxRefusal::Other);
  EXPECT_LE(record.max_move_half_spreads, 1.0e-2);
}

TEST(InboxProjection, AnEmptyBoxIsInfeasibleDataNamingItsNodeAndSide) {
  // The K 110 put is quoted [8, 9], a full unit below its intrinsic value 10.
  auto boxes = quote_boxes();
  boxes[2].put_lo = 8.0;
  boxes[2].put_hi = 9.0;
  QuoteSliceInboxRecord record;
  const auto inbox = project_american_in_box(fitted_grid(), boxes, kBounds, "", record);
  ASSERT_FALSE(inbox.has_value());
  EXPECT_EQ(inbox.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InfeasibleData) << to_string(record.outcome);
  EXPECT_EQ(record.conflict, QuoteSliceInboxConflict::EmptyBox) << to_string(record.conflict);
  EXPECT_EQ(record.conflict_side, Side::Put);
  EXPECT_EQ(record.conflict_node, 2u);
  EXPECT_EQ(record.conflict_strike, 110.0);
  EXPECT_NEAR(record.conflict_excess, 1.0, 1.0e-12);
  EXPECT_EQ(record.phase1_steps + record.phase2_steps, 0u);
  EXPECT_NE(inbox.error().message().find("node 2"), std::string::npos)
      << inbox.error().message();
}

TEST(InboxProjection, BoxesWithNoConvexCurveAreTypedNoConvexCurve) {
  // The K 100 call is quoted [6.5, 6.6]; the chord of the outer calls' highest quotes is
  // (11.0 + 1.0) / 2 = 6.0, half a unit below that bid.
  auto boxes = quote_boxes();
  boxes[0].call_lo = 10.9;
  boxes[0].call_hi = 11.0;
  boxes[1].call_lo = 6.5;
  boxes[1].call_hi = 6.6;
  boxes[2].call_lo = 0.9;
  boxes[2].call_hi = 1.0;
  QuoteSliceInboxRecord record;
  const auto inbox = project_american_in_box(fitted_grid(), boxes, kBounds, "", record);
  ASSERT_FALSE(inbox.has_value());
  EXPECT_EQ(inbox.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InfeasibleData);
  EXPECT_EQ(record.conflict, QuoteSliceInboxConflict::NoConvexCurve)
      << to_string(record.conflict);
  EXPECT_EQ(record.conflict_side, Side::Call);
  EXPECT_EQ(record.conflict_node, 1u);
  EXPECT_NEAR(record.conflict_excess, 0.5, 1.0e-12);
}

TEST(InboxProjection, AnAffineBoxConflictNamesItsNode) {
  // At K 100 every call inside [5.8, 6.0] minus every put inside [4.0, 4.5] is at least
  // 1.3, above the affine bound 100 - K = 0; each side alone is convex inside its boxes.
  auto boxes = quote_boxes();
  boxes[1] = {5.8, 6.0, 4.0, 4.5};
  QuoteSliceInboxRecord record;
  const auto inbox = project_american_in_box(fitted_grid(), boxes, kBounds, "", record);
  ASSERT_FALSE(inbox.has_value());
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InfeasibleData);
  EXPECT_EQ(record.conflict, QuoteSliceInboxConflict::AffineBox) << to_string(record.conflict);
  EXPECT_EQ(record.conflict_node, 1u);
  EXPECT_NEAR(record.conflict_excess, 1.3, 1.0e-12);
}

TEST(InboxProjection, ACoupledConflictIsProvedByPhaseOne) {
  // Rate > 0 (discount 0.9): 98 - K <= call - put <= 100 - 0.9 K. Every per-node test
  // passes, yet the K 110 call's bid 12.5 forces C(100) >= 12.5 (calls fall with strike),
  // so C(100) - P(100) <= 10 needs P(100) >= 2.5 above its ask 2.0. An LP over the same
  // rows (scipy HiGHS, lane evidence) finds the least worst violation 0.125.
  const AmericanPriceBounds bounds{100.0, 0.9, 1.0, 98.0, 100.0};
  const std::vector<AmericanPriceBox> boxes{
      {10.5, 25.0, 1.0, 3.0}, {5.0, 20.0, 1.0, 2.0}, {12.5, 13.0, 5.0, 40.0}};
  const std::vector<AmericanPriceNode> fitted{
      {90.0, 12.0, 2.0}, {100.0, 12.0, 1.5}, {110.0, 12.8, 20.0}};
  QuoteSliceInboxRecord record;
  const auto inbox = project_american_in_box(fitted, boxes, bounds, "", record);
  ASSERT_FALSE(inbox.has_value());
  EXPECT_EQ(inbox.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InfeasibleData);
  EXPECT_EQ(record.conflict, QuoteSliceInboxConflict::Coupled) << to_string(record.conflict);
  EXPECT_GT(record.conflict_excess, 0.0);
  EXPECT_LE(record.conflict_excess, 0.125 + 1.0e-6);
  EXPECT_GT(record.phase1_steps, 0u);
  EXPECT_EQ(record.phase2_steps, 0u);
}

TEST(InboxProjection, ASpentNewtonBudgetIsNonConvergedAndChangesNothing) {
  QuoteSliceInboxRecord record;
  const auto inbox =
      project_american_in_box(fitted_grid(), quote_boxes(), kBounds, "", record, 1u);
  ASSERT_FALSE(inbox.has_value());
  EXPECT_EQ(inbox.error().code(), atx::core::ErrorCode::OutOfRange);
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::NonConverged) << to_string(record.outcome);
  EXPECT_LE(record.phase1_steps + record.phase2_steps, 1u);
}

TEST(InboxProjection, MalformedInputIsInvalidInput) {
  const auto fitted = fitted_grid();
  const auto boxes = quote_boxes();
  QuoteSliceInboxRecord record;
  const auto short_boxes =
      project_american_in_box(fitted, std::span(boxes).first(2), kBounds, "", record);
  ASSERT_FALSE(short_boxes.has_value());
  EXPECT_EQ(short_boxes.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InvalidInput);
  auto crossed = boxes;
  crossed[1].call_lo = 7.0; // lo > hi
  EXPECT_FALSE(project_american_in_box(fitted, crossed, kBounds, "", record).has_value());
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InvalidInput);
  auto unsorted = fitted;
  unsorted[2].strike = 95.0;
  EXPECT_FALSE(project_american_in_box(unsorted, boxes, kBounds, "", record).has_value());
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InvalidInput);
  const AmericanPriceBounds broken{100.0, 1.0, 1.0, 101.0, 100.0};
  EXPECT_FALSE(project_american_in_box(fitted, boxes, broken, "", record).has_value());
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InvalidInput);
  EXPECT_FALSE(project_american_in_box(std::span(fitted).first(1), std::span(boxes).first(1),
                                       kBounds, "", record)
                   .has_value());
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::InvalidInput);
}

TEST(InboxProjection, ALockedQuoteStaysOnItsQuote) {
  // A locked call quote (bid == ask) is a box of zero width: the side is held there exactly
  // while the rest of the grid is projected around it.
  auto boxes = quote_boxes();
  boxes[0].call_lo = 11.2;
  boxes[0].call_hi = 11.2;
  QuoteSliceInboxRecord record;
  const auto inbox = project_american_in_box(fitted_grid(), boxes, kBounds, "", record);
  ASSERT_TRUE(inbox.has_value()) << inbox.error().to_string();
  EXPECT_EQ(inbox->slice.nodes()[0].call, 11.2);
  EXPECT_EQ(record.free_sides, 5u);
  EXPECT_TRUE(AmericanPriceSlice::create(inbox->slice.nodes(), kBounds).has_value());
}

TEST(InboxProjection, TheNondividendRegimeHoldsTheDiscountedIntrinsicFloor) {
  // Non-dividend bounds (prepaid value = spot, rate 5 %, T 1): the fitted K 90 call 13.8 is
  // inside its quote but below S - K df = 14.39; create() alone would accept it, the
  // non-dividend certificate would not. The projection holds every call on or above the
  // floor and the result passes check_nondividend_american.
  const double df = std::exp(-0.05);
  const AmericanPriceBounds bounds{100.0, df, 1.0, 100.0, 100.0};
  const std::vector<AmericanPriceNode> fitted{
      {90.0, 13.8, 0.3}, {100.0, 8.5, 2.2}, {110.0, 2.5, 10.5}};
  const std::vector<AmericanPriceBox> boxes{
      {13.5, 15.0, 0.2, 0.4}, {6.5, 7.2, 2.0, 2.4}, {2.3, 2.9, 10.2, 10.8}};
  QuoteSliceInboxRecord record;
  const auto inbox = project_american_in_box(fitted, boxes, bounds, "", record);
  ASSERT_TRUE(inbox.has_value()) << inbox.error().to_string();
  const auto nodes = inbox->slice.nodes();
  EXPECT_GE(nodes[0].call, 100.0 - 90.0 * df);
  const auto nondividend = check_nondividend_american(inbox->slice, df);
  EXPECT_TRUE(nondividend.has_value()) << nondividend.error().to_string();
}

TEST(InboxProjection, TheBoxRefusalIsAttributedFromItsMessage) {
  EXPECT_EQ(classify_box_refusal(""), QuoteSliceBoxRefusal::Other);
  // The box rung's own messages, produced by the fixtures above.
  const auto refused = certify_american_box(fitted_grid(), quote_boxes(), kBounds);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(classify_box_refusal(refused.error().message()),
            QuoteSliceBoxRefusal::CertificateRefused);
  auto below = quote_boxes();
  below[2].put_lo = 8.0;
  below[2].put_hi = 9.0;
  const auto exercise = certify_american_box(fitted_grid(), below, kBounds);
  ASSERT_FALSE(exercise.has_value());
  EXPECT_EQ(classify_box_refusal(exercise.error().message()),
            QuoteSliceBoxRefusal::ExerciseBounds);
  auto kinked = quote_boxes();
  kinked[0].call_lo = 10.9;
  kinked[0].call_hi = 11.0;
  kinked[1].call_lo = 6.5;
  kinked[1].call_hi = 6.6;
  kinked[2].call_lo = 0.9;
  kinked[2].call_hi = 1.0;
  const auto convex = certify_american_box(fitted_grid(), kinked, kBounds);
  ASSERT_FALSE(convex.has_value());
  EXPECT_EQ(classify_box_refusal(convex.error().message()), QuoteSliceBoxRefusal::NoConvexCurve);
  EXPECT_EQ(classify_box_refusal("AmericanPriceSlice box: node 0: no arbitrage-free curve "
                                 "inside the boxes (projection did not settle)"),
            QuoteSliceBoxRefusal::NotSettled);
}

// ── The hook: box_certify_slice (quote_slice.cpp) under the key ──────────────

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// quote_surface_test.cpp's carry zig-zag: Andersen-Lake quotes whose per-strike yield
// alternates between two values, quoted +/- max(0.01, price * half_spread).
Chain zigzag_carry_chain(double years, double sigma, double low_q, double high_q,
                         double half_spread) {
  Chain out;
  out.market = QuoteSliceMarket{100.0, years, years, 0.03, low_q, 0.0};
  for (int strike = 80; strike <= 120; strike += 2) {
    const double q = (strike / 2) % 2 == 0 ? low_q : high_q;
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price =
          andersen_lake(100.0, static_cast<double>(strike), years, sigma, 0.03, q, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * half_spread);
      out.quotes.push_back({static_cast<double>(strike), side, std::max(*price - spread, 0.0),
                            *price + spread, q});
    }
  }
  return out;
}

QuoteSliceConfig box_config(bool inbox) {
  QuoteSliceConfig config;
  config.repair.certificate = QuoteSliceCertificate::Box;
  config.repair.box_half_ticks = 0.5;
  config.repair.inbox_projection = inbox;
  return config;
}

TEST(QuoteSliceInboxProjection, ARefusedBoxRungIsTypedByTheRungUnderTheKey) {
  // The carry zig-zag far wider than its one-cent quotes: no convex curve lies inside the
  // boxes. Off: the box rung's refusal alone. On: the rung runs, proves the same emptiness
  // exactly and types it; the slice stays refused and nothing is widened.
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.005, 0.015, 0.0);
  const auto off = fit_quote_slice(c.market, c.quotes, box_config(false));
  ASSERT_TRUE(off) << off.error().to_string();
  ASSERT_EQ(off->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << off->fit_error;
  EXPECT_EQ(off->diagnostics.box.outcome, QuoteSliceRepairOutcome::Infeasible);
  EXPECT_EQ(off->diagnostics.box.inbox.outcome, QuoteSliceInboxOutcome::NotAttempted);
  const auto on = fit_quote_slice(c.market, c.quotes, box_config(true));
  ASSERT_TRUE(on) << on.error().to_string();
  ASSERT_EQ(on->fit_status, QuoteSliceFitStatus::ArbitrageFailure) << on->fit_error;
  EXPECT_FALSE(on->prices);
  const auto &record = on->diagnostics.box;
  EXPECT_EQ(record.outcome, QuoteSliceRepairOutcome::Infeasible) << to_string(record.outcome);
  EXPECT_EQ(record.inbox.outcome, QuoteSliceInboxOutcome::InfeasibleData)
      << to_string(record.inbox.outcome);
  EXPECT_EQ(record.inbox.box_refusal, QuoteSliceBoxRefusal::NoConvexCurve)
      << to_string(record.inbox.box_refusal);
  EXPECT_EQ(record.inbox.conflict, QuoteSliceInboxConflict::NoConvexCurve)
      << to_string(record.inbox.conflict);
  EXPECT_GT(record.inbox.conflict_excess, 0.0);
  // The refusal the fit reports is still the exact certificate's.
  EXPECT_EQ(on->fit_error, off->fit_error);
}

TEST(QuoteSliceInboxProjection, ASliceTheBoxRungServesIsUntouchedByTheKey) {
  // The micro-kinked zig-zag the box rung serves: the rung never runs, bit for bit.
  const auto c = zigzag_carry_chain(0.5, 0.25, 0.0095, 0.0105, 0.05);
  const auto off = fit_quote_slice(c.market, c.quotes, box_config(false));
  const auto on = fit_quote_slice(c.market, c.quotes, box_config(true));
  ASSERT_TRUE(off && on);
  ASSERT_EQ(on->fit_status, QuoteSliceFitStatus::Success) << on->fit_error;
  EXPECT_EQ(on->diagnostics.box.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_EQ(on->diagnostics.box.inbox.outcome, QuoteSliceInboxOutcome::NotAttempted);
  ASSERT_EQ(on->rows.size(), off->rows.size());
  for (std::size_t i = 0; i < on->rows.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(on->rows[i].fitted_price),
              std::bit_cast<std::uint64_t>(off->rows[i].fitted_price))
        << i;
  }
}

} // namespace
} // namespace atx::vol
