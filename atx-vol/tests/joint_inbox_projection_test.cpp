#include "fitting/joint_inbox_projection.hpp"
#include "fitting/inbox_projection.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace atx::vol {
namespace {

constexpr AmericanPriceBounds kCarry{100.0, 1.0, 1.0, 98.0, 100.0};

TEST(JointInboxProjection, CrossingAndParityAreSolvedTogetherInsideExactBoxes) {
  std::vector<AmericanPriceNode> near{{90, 12, 2}, {100, 7, 6}, {110, 2, 12}};
  std::vector<AmericanPriceNode> far{{90, 11, 3}, {100, 5, 6}, {110, 1, 13}};
  const std::vector<AmericanPriceBox> boxes{{10.5, 13, 1.5, 4}, {4, 7, 4, 8},
                                            {0.5, 3, 11.5, 14}};
  const std::array<detail::JointInboxSlice, 2> input{{{near, boxes, kCarry}, {far, boxes, kCarry}}};
  const auto result = detail::project_joint_in_box(input);
  ASSERT_EQ(result.outcome, JointProjectionOutcome::Feasible) << to_string(result.outcome);
  const auto a = AmericanPriceSlice::create(near, kCarry);
  const auto b = AmericanPriceSlice::create(far, kCarry);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(check_american_calendar(*a, *b).has_value());
  for (const auto& slice : input) {
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      EXPECT_GE(slice.nodes[i].call, boxes[i].call_lo);
      EXPECT_LE(slice.nodes[i].call, boxes[i].call_hi);
      EXPECT_GE(slice.nodes[i].put, boxes[i].put_lo);
      EXPECT_LE(slice.nodes[i].put, boxes[i].put_hi);
    }
  }
}

TEST(JointInboxProjection, BoundaryOnlyFeasibleSetWithLockedQuotesIsAccepted) {
  // The middle call can only equal its neighbours' chord. A strict-interior
  // barrier has no feasible starting point, but the closed polytope is valid.
  std::vector<AmericanPriceNode> nodes{{90, 12, 3}, {100, 7, 6}, {110, 2, 13}};
  const std::vector<AmericanPriceBox> boxes{{12, 12, 3, 3}, {7, 8, 6, 8}, {2, 2, 13, 13}};
  const std::array<detail::JointInboxSlice, 1> input{{{nodes, boxes, kCarry}}};
  const auto result = detail::project_joint_in_box(input);
  ASSERT_EQ(result.outcome, JointProjectionOutcome::Feasible) << to_string(result.outcome);
  EXPECT_NEAR(nodes[1].call, 7.0, 1e-8);
  EXPECT_GE(nodes[1].put, 7.0 - 1e-8);
  EXPECT_TRUE(AmericanPriceSlice::create(nodes, kCarry).has_value());
}

TEST(JointInboxProjection, BudgetExhaustionRollsBackEveryNode) {
  std::vector<AmericanPriceNode> nodes{{90, 11, 2}, {100, 6.5, 5.5}, {110, 1, 12}};
  const auto before = nodes;
  const std::vector<AmericanPriceBox> boxes{{10.5, 11.5, 1.5, 2.5}, {5.8, 6.8, 5, 6},
                                            {0.5, 1.5, 11.5, 12.5}};
  const std::array<detail::JointInboxSlice, 1> input{{{nodes, boxes, kCarry}}};
  EXPECT_EQ(detail::project_joint_in_box(input, 1).outcome, JointProjectionOutcome::NonConverged);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(nodes[i].call, before[i].call);
    EXPECT_EQ(nodes[i].put, before[i].put);
  }
}

TEST(JointInboxProjection, ContradictoryCalendarBoxesAreTypedInfeasible) {
  std::vector<AmericanPriceNode> near{{90, 12, 3}, {100, 7, 8}, {110, 2, 13}};
  std::vector<AmericanPriceNode> far{{90, 11, 2}, {100, 6, 7}, {110, 1, 12}};
  const std::vector<AmericanPriceBox> near_boxes{{12, 12, 3, 3}, {7, 7, 8, 8}, {2, 2, 13, 13}};
  const std::vector<AmericanPriceBox> far_boxes{{11, 11, 2, 2}, {6, 6, 7, 7}, {1, 1, 12, 12}};
  const std::array<detail::JointInboxSlice, 2> input{{{near, near_boxes, kCarry},
                                                    {far, far_boxes, kCarry}}};
  EXPECT_EQ(detail::project_joint_in_box(input).outcome, JointProjectionOutcome::InfeasibleData);
}

TEST(JointInboxProjection, PositiveWidthZeroBidWingsCanHaveBoundaryOnlyFeasibility) {
  // Convexity and the middle bid force all puts to equal two. The wing bids
  // are zero and every quote has positive width, yet no strict interior exists.
  std::vector<AmericanPriceNode> nodes{{80, 22, 1}, {90, 12, 2.5}, {100, 2, 1}};
  const std::vector<AmericanPriceBox> boxes{{20, 23, 0, 2}, {10, 13, 2, 3}, {0, 3, 0, 2}};
  const std::array<detail::JointInboxSlice, 1> input{{{nodes, boxes, kCarry}}};
  const auto result = detail::project_joint_in_box(input);
  ASSERT_EQ(result.outcome, JointProjectionOutcome::Feasible) << to_string(result.outcome);
  const auto certificate = AmericanPriceSlice::create(nodes, kCarry);
  ASSERT_TRUE(certificate.has_value());
  for (const auto &node : nodes) EXPECT_NEAR(node.put, 2.0, 1e-8);
}

TEST(JointInboxProjection, NonuniformDomainsCheckNonadjacentCalendarKnots) {
  std::vector<AmericanPriceNode> near{{90, 12, 3}, {100, 7, 8}, {110, 2, 13}};
  std::vector<AmericanPriceNode> middle{{95, 10, 6}, {105, 5, 11}};
  std::vector<AmericanPriceNode> far{{90, 11, 2}, {100, 6, 7}, {110, 1, 12}};
  const std::vector<AmericanPriceBox> wide{{10, 15, 1, 6}, {4, 10, 4, 11}, {0, 5, 10, 16}};
  const std::vector<AmericanPriceBox> mid_boxes{{8, 12, 4, 8}, {3, 7, 9, 13}};
  const std::array<detail::JointInboxSlice, 3> input{{{near, wide, kCarry},
      {middle, mid_boxes, kCarry}, {far, wide, kCarry}}};
  ASSERT_EQ(detail::project_joint_in_box(input).outcome, JointProjectionOutcome::Feasible);
  const auto a = AmericanPriceSlice::create(near, kCarry);
  const auto b = AmericanPriceSlice::create(middle, kCarry);
  const auto c = AmericanPriceSlice::create(far, kCarry);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(check_american_calendar(*a, *b).has_value());
  EXPECT_TRUE(check_american_calendar(*b, *c).has_value());
  EXPECT_TRUE(check_american_calendar(*a, *c).has_value());
}

TEST(JointInboxProjection, TerminalDiscountRejectsAskBelowItsProvenCallFloor) {
  std::vector<AmericanPriceNode> nodes{{90, 12, 3}, {100, 10, 8}, {110, 4, 12}};
  const AmericanPriceBounds bounds{100, .9, 1, 98, 100};
  const std::vector<AmericanPriceBox> boxes{{12, 16, 1, 5}, {8, 12, 6, 10}, {2, 6, 10, 14}};
  const std::array<detail::JointInboxSlice, 1> input{{{nodes, boxes, bounds, .9}}};
  const auto before = nodes;
  EXPECT_EQ(detail::project_joint_in_box(input).outcome, JointProjectionOutcome::InfeasibleData);
  EXPECT_EQ(nodes.front().call, before.front().call);
}

TEST(JointInboxProjection, FeasibleInputKeepsItsPricesExactly) {
  std::vector<AmericanPriceNode> nodes{{90, 12, 3}, {100, 7, 8}, {110, 2, 13}};
  const auto before = nodes;
  const std::vector<AmericanPriceBox> boxes{{11, 13, 2, 4}, {6, 8, 7, 9}, {1, 3, 12, 14}};
  const std::array<detail::JointInboxSlice, 1> input{{{nodes, boxes, kCarry}}};
  ASSERT_EQ(detail::project_joint_in_box(input).outcome, JointProjectionOutcome::Feasible);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(nodes[i].call, before[i].call);
    EXPECT_EQ(nodes[i].put, before[i].put);
  }
}

TEST(JointInboxProjection, ExplicitFloorRecoveryChangesOnlyTheProvenImpossibleAsk) {
  std::vector<AmericanPriceNode> nodes{{90, 12, 3}, {100, 10, 8}, {110, 4, 12}};
  const AmericanPriceBounds bounds{100, .9, 1, 98, 100};
  std::vector<AmericanPriceBox> boxes{{12, 16, 1, 5}, {8, 12, 6, 10}, {2, 6, 10, 14}};
  const auto original = boxes;
  const std::vector<double> prepaid(3, 98);
  const std::vector<std::uint8_t> quoted(3, 3u);
  const detail::JointInboxSlice slice{nodes, boxes, bounds, .9, prepaid};
  detail::TerminalFloorRecovery recovery;
  ASSERT_TRUE(detail::recover_terminal_floor_asks(slice, quoted, boxes, recovery));
  EXPECT_EQ(recovery.raised_asks, 1u);
  EXPECT_GT(boxes[0].call_hi, 17);
  EXPECT_LT(boxes[0].call_hi, 17.000001);
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    EXPECT_EQ(boxes[i].call_lo, original[i].call_lo);
    EXPECT_EQ(boxes[i].put_lo, original[i].put_lo);
    EXPECT_EQ(boxes[i].put_hi, original[i].put_hi);
    if (i > 0u) EXPECT_EQ(boxes[i].call_hi, original[i].call_hi);
  }
  EXPECT_EQ(detail::project_joint_in_box(std::span(&slice, 1)).outcome,
            JointProjectionOutcome::Feasible);
}

TEST(JointInboxProjection, FloorRecoveryDoesNotRelaxAMaterialConvexityConflict) {
  std::vector<AmericanPriceNode> nodes{{90, 12, 3}, {100, 10, 8}, {110, 4, 12}};
  const AmericanPriceBounds bounds{100, .9, 1, 98, 100};
  std::vector<AmericanPriceBox> boxes{{12, 16, 2, 2.5}, {8, 12, 8, 9}, {2, 6, 12, 12.5}};
  const std::vector<double> prepaid(3, 98);
  const std::vector<std::uint8_t> quoted(3, 3u);
  const detail::JointInboxSlice slice{nodes, boxes, bounds, .9, prepaid};
  detail::TerminalFloorRecovery recovery;
  ASSERT_TRUE(detail::recover_terminal_floor_asks(slice, quoted, boxes, recovery));
  EXPECT_EQ(detail::project_joint_in_box(std::span(&slice, 1)).outcome,
            JointProjectionOutcome::InfeasibleData);
}

TEST(JointInboxProjection, ExistingInboxRungRecoversAZeroInteriorFeasibleSlice) {
  const std::vector<AmericanPriceNode> nodes{{80, 22, 1}, {90, 12, 2.5}, {100, 2, 1}};
  const std::vector<AmericanPriceBox> boxes{{20, 23, 0, 2}, {10, 13, 2, 3}, {0, 3, 0, 2}};
  QuoteSliceInboxRecord record;
  const auto result = detail::project_american_in_box(nodes, boxes, kCarry, "", record);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_EQ(record.outcome, QuoteSliceInboxOutcome::Applied);
  for (const auto &node : result->slice.nodes()) EXPECT_NEAR(node.put, 2, 1e-7);
}

TEST(JointInboxProjection, NegativeRateUsesItsActualTerminalDiscount) {
  std::vector<AmericanPriceNode> nodes{{90, 12, 2}, {100, 7, 12}, {110, 2, 23}};
  const AmericanPriceBounds bounds{100, 1, 1.1, 98, 100};
  const std::vector<AmericanPriceBox> boxes{{10, 14, 0, 4}, {5, 9, 11, 13}, {0, 4, 22, 24}};
  const std::vector<double> prepaid(3, 99);
  const detail::JointInboxSlice slice{nodes, boxes, bounds, 1.1, prepaid};
  ASSERT_EQ(detail::project_joint_in_box(std::span(&slice, 1)).outcome,
            JointProjectionOutcome::Feasible);
  for (const auto &node : nodes) EXPECT_GE(node.put, 1.1 * node.strike - 99);
}

TEST(JointInboxProjection, TerminalPrepaidOutsideItsHorizonEnvelopeIsInvalid) {
  std::vector<AmericanPriceNode> nodes{{90, 12, 3}, {100, 7, 8}, {110, 2, 13}};
  const std::vector<AmericanPriceBox> boxes{{11, 13, 2, 4}, {6, 8, 7, 9}, {1, 3, 12, 14}};
  const std::vector<double> prepaid(3, 97);
  const detail::JointInboxSlice slice{nodes, boxes, kCarry, 1, prepaid};
  EXPECT_EQ(detail::project_joint_in_box(std::span(&slice, 1)).outcome,
            JointProjectionOutcome::InvalidInput);
}

TEST(JointInboxProjection, ExternalRawBoxFixturePassesEveryIndependentCertificate) {
  std::string path;
#ifdef _WIN32
  std::size_t length = 0;
  ASSERT_EQ(getenv_s(&length, nullptr, 0, "ATX_VOL_JOINT_BOX_FIXTURE"), 0);
  if (length > 0u) {
    std::vector<char> buffer(length);
    ASSERT_EQ(getenv_s(&length, buffer.data(), buffer.size(), "ATX_VOL_JOINT_BOX_FIXTURE"), 0);
    path = buffer.data();
  }
#else
  if (const char *value = std::getenv("ATX_VOL_JOINT_BOX_FIXTURE")) path = value;
#endif
  if (path.empty()) GTEST_SKIP() << "Set ATX_VOL_JOINT_BOX_FIXTURE to an independently exported raw-box fixture";
  std::ifstream input(path);
  ASSERT_TRUE(input.good());
  nlohmann::json fixture;
  input >> fixture;
  const std::size_t count = fixture.at("slices").size();
  std::vector<std::vector<AmericanPriceNode>> nodes(count);
  std::vector<std::vector<AmericanPriceBox>> boxes(count);
  std::vector<std::vector<double>> prepaid(count);
  std::vector<detail::JointInboxSlice> slices;
  slices.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto &slice = fixture.at("slices").at(i);
    const auto &b = slice.at("bounds");
    const AmericanPriceBounds bounds{b.at("spot").get<double>(), b.at("discount_min").get<double>(),
        b.at("discount_max").get<double>(), b.at("prepaid_min").get<double>(), b.at("prepaid_max").get<double>()};
    for (const auto &row : slice.at("nodes")) {
      nodes[i].push_back({row.at(0).get<double>(), row.at(1).get<double>(), row.at(2).get<double>()});
      boxes[i].push_back({row.at(3).get<double>(), row.at(4).get<double>(), row.at(5).get<double>(), row.at(6).get<double>()});
    }
    if (slice.contains("terminal_prepaid")) prepaid[i] = slice.at("terminal_prepaid").get<std::vector<double>>();
    const std::optional<double> discount = slice.contains("terminal_discount")
        ? std::optional<double>(slice.at("terminal_discount").get<double>()) : std::nullopt;
    slices.push_back({nodes[i], boxes[i], bounds, discount, prepaid[i]});
  }
  detail::JointInboxDiagnostics diagnostics;
  const auto result = detail::project_joint_in_box(slices, 150, &diagnostics);
  std::cout << "joint fixture slices=" << count << " iterations=" << result.sweeps << " outcome="
            << to_string(result.outcome) << " primal=" << diagnostics.primal_residual
            << " dual=" << diagnostics.dual_residual << " gap=" << diagnostics.duality_gap
            << " vars=" << diagnostics.variables << " rows=" << diagnostics.constraints << '\n';
  ASSERT_EQ(result.outcome, JointProjectionOutcome::Feasible);
  std::vector<AmericanPriceSlice> certified;
  std::size_t outside_original = 0;
  for (std::size_t i = 0; i < count; ++i) {
    auto c = AmericanPriceSlice::create(nodes[i], slices[i].bounds);
    ASSERT_TRUE(c.has_value());
    certified.push_back(std::move(*c));
    for (std::size_t j = 0; j < nodes[i].size(); ++j) {
      EXPECT_GE(nodes[i][j].call, boxes[i][j].call_lo);
      EXPECT_LE(nodes[i][j].call, boxes[i][j].call_hi);
      EXPECT_GE(nodes[i][j].put, boxes[i][j].put_lo);
      EXPECT_LE(nodes[i][j].put, boxes[i][j].put_hi);
      const auto &source = fixture.at("slices").at(i);
      if (source.contains("original_boxes")) {
        const auto &original = source.at("original_boxes").at(j);
        if (nodes[i][j].call < original.at(0).get<double>() - 1e-9 ||
            nodes[i][j].call > original.at(1).get<double>() + 1e-9) ++outside_original;
        if (nodes[i][j].put < original.at(2).get<double>() - 1e-9 ||
            nodes[i][j].put > original.at(3).get<double>() + 1e-9) ++outside_original;
      }
    }
  }
  for (std::size_t a = 0; a < count; ++a) {
    for (std::size_t b = a + 1u; b < count; ++b) {
      EXPECT_TRUE(check_american_calendar(certified[a], certified[b]).has_value()) << a << ' ' << b;
    }
  }
  std::cout << "outside original boxes=" << outside_original << '\n';
}

} // namespace
} // namespace atx::vol
