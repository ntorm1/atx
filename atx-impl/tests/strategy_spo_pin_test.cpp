// spo-v1 bit-for-bit guard (platform v7 W1b; R2 review M-4): two FNV-1a digests of spo-v1 as
// pre-registered (SpoParams{}: alpha horizon 1, no specific ceiling, gamma = max(gamma_vol,
// gamma_bind), gross budget = --aim-leverage) on a fixed synthetic role and risk model:
//   * weights: every planned weight (bit pattern) of two books (S1, S2) planned in lockstep
//     through the v7 seam (v7::plan: rebalance and hold decisions, carried without drift),
//     then the decision fields of each plan;
//   * replay: the NAV replay of the S2 book (planned gross/net/turnover, net return, traded
//     and cost dollars, long/short dollars, post-trade NAV per day) and columns 1-38 of
//     spo_diagnostics.csv (the columns the real-data identity run (b) compares).
// A change to spo-v2, spo-v3 or to the flag machinery must leave both digests unchanged.
// The digest procedures live in strategy_spo_digest.hpp (moved there verbatim by platform v8
// R-6, shared with SpoV3.V1AndV2DigestsUnchanged).
//
// CAPTURE PROTOCOL (no binary may run in the implementing lane): this file and
// strategy_spo_fixture.hpp use ONLY the API of the pre-W1b commit c8503bb3, so the commit that
// introduces them compiles on that base unchanged. Root builds atx-impl-strategy-target-tests
// at that commit, runs --gtest_filter=SpoPin.*, reads the printed `[spo-pin]` digests and pins
// them below; the W1b head must print the SAME two values. Pinned (see PIN below); zero
// constants would mean unpinned (the test then fails and prints the digests).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cstdio>
#include <memory>
#include <utility>
#include <gtest/gtest.h>
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "strategy_spo_digest.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using atx::impl::strategy::spo::fixture::Role;
namespace sp = atx::impl::strategy::spo;
namespace v7 = atx::impl::strategy::v7;
namespace dg = atx::impl::strategy::spo::digest;

// PIN: captured by root on the pre-W1b base (pool-2 decdf947 = the c8503bb3 line + cd01f74e
// only, build v7-4: weights over 54 diagnostics rows, replay over 40 days) and reproduced
// identically on the W1b head (pool-2 66e0774d = 4c4ce75f + F3, build v7-5): spo-v1 is bit
// for bit. Any change here means a spo-v1 planned weight moved.
constexpr u64 pinned_weights = 0xda6b6871e7e267c5ULL;
constexpr u64 pinned_replay = 0xaabdbb72f99a6e13ULL;

v7::NavV7Options spo_v1(std::shared_ptr<const sp::RiskStore> risk) {
  v7::NavV7Options o;
  o.spo_v1 = true; // SpoParams{}: spo-v1 as pre-registered
  o.spo_risk = std::move(risk);
  return o;
}

// Role(40, 12, 53) with a clean risk model (seed 3): the ReplayPlansNeutral... fixture.
TEST(SpoPin, SpoV1PlannedWeightsOfTwoLockstepBooks_MatchThePinnedDigest) {
  const Role role(40, 12, 53);
  const auto m = dg::model_of(role, 3);
  ASSERT_NE(m->risk, nullptr);
  const auto w = dg::planned_weights(spo_v1(m->risk), role);
  ASSERT_TRUE(w.error.empty()) << w.error;
  std::printf("[spo-pin] weights=0x%016llx (%zu diagnostics rows)\n",
              static_cast<unsigned long long>(w.value), w.count);
  if (pinned_weights == 0 && pinned_replay == 0) {
    ADD_FAILURE() << "spo-v1 digests unpinned: capture on c8503bb3 + this file, then pin";
    return;
  }
  EXPECT_EQ(w.value, pinned_weights);
}

TEST(SpoPin, SpoV1ReplayAndDiagnosticsColumns1To38_MatchThePinnedDigest) {
  const Role role(40, 12, 53);
  const auto m = dg::model_of(role, 3);
  ASSERT_NE(m->risk, nullptr);
  const auto r = dg::replay(spo_v1(m->risk), role, 38);
  ASSERT_TRUE(r.error.empty()) << r.error;
  std::printf("[spo-pin] replay=0x%016llx (%zu days)\n", static_cast<unsigned long long>(r.value),
              r.count);
  if (pinned_weights == 0 && pinned_replay == 0) {
    ADD_FAILURE() << "spo-v1 digests unpinned: capture on c8503bb3 + this file, then pin";
    return;
  }
  EXPECT_EQ(r.value, pinned_replay);
}
} // namespace
