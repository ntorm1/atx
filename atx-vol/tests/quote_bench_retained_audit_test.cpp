#include "../tools/quote_bench_retained_audit.hpp"
#include "../tools/quote_bench_acceptance.hpp"

#include "atx/vol/api/pricing/black76.hpp"
#include "support/alloc_counter.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace atx::vol;
using quote_bench_acceptance::RetainedRun;
using Json = nlohmann::json;

TEST(RetainedApiAudit, InvocationAfterTheFitSnapshotInvalidatesTheProof) {
  RetainedRun owner;
  owner.begin(1);
  const std::array<std::uint64_t, 1> members{17};
  const auto index = owner.attempt({"A", "A", "1"}, members);
  owner.note_fit_invocation();
  owner.retain(index, std::vector<QuoteSurfaceSlice>(1));
  owner.published();
  owner.snapshots_complete();
  owner.note_fit_invocation(); // Synthetic accidental-refit event, not a numerical fit.
  Json payload;
  auto marker = owner.audit(payload);
  EXPECT_FALSE(marker.at("audit_completed").get<bool>());
  EXPECT_EQ(marker.at("graded_surface_fit_invocations_before"), 1);
  EXPECT_EQ(marker.at("graded_surface_fit_invocations_after"), 2);
  EXPECT_EQ(marker.at("refit_count"), 1);
  owner.release(marker);
}

TEST(RetainedApiAudit, AllocationFailureCannotProduceACompletedCapture) {
  RetainedRun owner;
  bool refused = false;
  testkit::alloc_counter_arm();
  testkit::alloc_counter_fail_once_at(1);
  try {
    owner.begin(1);
  } catch (const std::bad_alloc &) {
    refused = true;
  }
  const auto allocations = testkit::alloc_counter_disarm();
  ASSERT_TRUE(refused);
  EXPECT_EQ(allocations, 1U);
  EXPECT_THROW(owner.published(), std::logic_error);

  owner.begin(1);
  const std::array<std::uint64_t, 1> members{17};
  refused = false;
  testkit::alloc_counter_arm();
  testkit::alloc_counter_fail_once_at(1);
  try {
    static_cast<void>(owner.attempt({"A", "A", "1"}, members));
  } catch (const std::bad_alloc &) {
    refused = true;
  }
  static_cast<void>(testkit::alloc_counter_disarm());
  ASSERT_TRUE(refused);
  owner.published();
  owner.snapshots_complete();
  Json payload;
  auto marker = owner.audit(payload);
  EXPECT_FALSE(marker.at("audit_completed").get<bool>());
  EXPECT_EQ(marker.at("graded_surface_fit_invocations_before"), 0);
  owner.release(marker);
}

TEST(RetainedApiAudit, EnforcesPublicationSnapshotAuditReleaseOrder) {
  RetainedRun owner;
  Json payload;
  EXPECT_THROW(static_cast<void>(owner.audit(payload)), std::logic_error);
  owner.begin(1);
  EXPECT_THROW(owner.snapshots_complete(), std::logic_error);
  EXPECT_THROW(static_cast<void>(owner.audit(payload)), std::logic_error);
  owner.published();
  EXPECT_THROW(static_cast<void>(owner.audit(payload)), std::logic_error);
  owner.snapshots_complete();
  auto marker = owner.audit(payload);
  EXPECT_FALSE(marker.at("audit_completed").get<bool>());
  owner.release(marker);
  EXPECT_TRUE(marker.at("retained_owner_released_after_audit").get<bool>());
  EXPECT_THROW(static_cast<void>(owner.audit(payload)), std::logic_error);
}

TEST(RetainedApiAudit, RefusedSlotsAndMissingOwnersAreNotDiscarded) {
  RetainedRun owner;
  owner.begin(2);
  const std::array<std::uint64_t, 1> a{17}, b{19}, c{23};
  const auto first = owner.attempt({"A", "A", "1"}, a);
  owner.note_fit_invocation();
  std::vector<QuoteSurfaceSlice> refused(1);
  refused.front().refusal = "test_refusal";
  owner.retain(first, std::move(refused));
  const auto second = owner.attempt({"B", "B", "1"}, b);
  owner.note_fit_invocation();
  owner.failed(second, "whole_surface_failed");
  owner.unsupported(c);
  owner.published();
  owner.snapshots_complete();
  Json payload;
  auto marker = owner.audit(payload);
  EXPECT_FALSE(marker.at("audit_completed").get<bool>());
  EXPECT_EQ(marker.at("attempted_chain_ids"), Json({17, 19}));
  EXPECT_EQ(marker.at("retained_chain_ids"), Json({17}));
  EXPECT_EQ(marker.at("missing_owner_chain_ids"), Json({19}));
  EXPECT_EQ(marker.at("unsupported_chain_ids"), Json({23}));
  EXPECT_EQ(payload.at("snapshots").at(0).at(0).at("refusal"), "test_refusal");
  EXPECT_EQ(marker.at("refit_count"), 0);
  owner.release(marker);
}

TEST(RetainedApiAudit, RejectsDuplicateMembersShapeMismatchAndRepeatedCapture) {
  RetainedRun owner;
  owner.begin(2);
  const std::array<std::uint64_t, 2> duplicates{17, 17};
  EXPECT_THROW(static_cast<void>(owner.attempt({"A", "A", "1"}, duplicates)),
               std::invalid_argument);
  const std::array<std::uint64_t, 1> a{17};
  const auto index = owner.attempt({"A", "A", "1"}, a);
  EXPECT_THROW(static_cast<void>(owner.attempt({"A", "A", "2"}, a)),
               std::invalid_argument);
  EXPECT_THROW(owner.retain(index, {}), std::invalid_argument);
  owner.retain(index, std::vector<QuoteSurfaceSlice>(1));
  EXPECT_THROW(owner.retain(index, std::vector<QuoteSurfaceSlice>(1)), std::logic_error);
}

TEST(RetainedApiAudit, QueriesTheMovedSurfaceAfterItsInputStorageIsDestroyed) {
  RetainedRun owner;
  owner.begin(1);
  Json expected;
  {
    QuoteSliceMarket market{100.0, 0.5, 0.5, 0.0, 0.0, 0.0};
    std::vector<QuoteSliceQuote> quotes;
    for (int strike = 80; strike <= 120; strike += 4) {
      for (const Side side : {Side::Call, Side::Put}) {
        const double price = black76_price(100.0, static_cast<double>(strike), 0.5, 0.3,
                                           1.0, side);
        quotes.push_back({static_cast<double>(strike), side, price - 0.001, price + 0.001});
      }
    }
    QuoteSliceConfig config;
    config.floor_policy = QuoteSliceFloorPolicy::PriceOnly;
    const std::array expiries{QuoteSurfaceExpiry{market, quotes}};
    const std::array<std::uint64_t, 1> members{17};
    const auto index = owner.attempt({"A", "A", "1"}, members);
    owner.note_fit_invocation();
    auto fitted = fit_quote_surface(expiries, config);
    ASSERT_TRUE(fitted) << fitted.error().to_string();
    ASSERT_EQ(fitted->size(), 1U);
    ASSERT_TRUE(fitted->front().fit.prices);
    expected = quote_bench_acceptance::surface(*fitted, members);
    owner.retain(index, std::move(*fitted));
  }
  owner.published();
  owner.snapshots_complete();
  Json payload;
  auto marker = owner.audit(payload);
  ASSERT_TRUE(marker.at("audit_completed").get<bool>());
  EXPECT_EQ(payload.at("snapshots").at(0), expected);
  EXPECT_EQ(marker.at("serialized_chain_ids"), Json({17}));
  EXPECT_EQ(marker.at("graded_surface_fit_invocations_before"), 1);
  EXPECT_EQ(marker.at("graded_surface_fit_invocations_after"), 1);
  owner.release(marker);
  EXPECT_EQ(payload.at("snapshots").at(0), expected);
}
} // namespace
