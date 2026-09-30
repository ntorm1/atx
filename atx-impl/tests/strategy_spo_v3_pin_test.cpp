// spo-v1 and spo-v2 bit for bit under the spo-v3 code (platform v8 R-6): the SpoPin digest
// procedures (strategy_spo_digest.hpp) on spo-v1 as pre-registered and on spo-v2 (its W1b
// defaults with w_max .5, so its vol target is reachable on the fixture). spo-v3 is a flag
// (--rule spo-v3); with it off nothing moves: spo-v1's digests are SpoPin's pins, spo-v2's are
// pinned here.
//
// CAPTURE PROTOCOL for the spo-v2 pins (no binary may run in the implementing lane): this file
// and strategy_spo_digest.hpp use ONLY the API of the pre-R6 tree. Root copies both files onto
// the v8 integration head WITHOUT lane R6 (feat/platform-v8-20260929 at 41ac94fd, or its
// successor before the R6 merge), adds `strategy_spo_v3_pin_test.cpp` to the
// atx-impl-strategy-target-tests list of atx-impl/tests/CMakeLists.txt, builds that target,
// runs --gtest_filter=SpoV3.V1AndV2DigestsUnchanged and reads the `[spo-v3-pin] v2` line (the
// test passes its spo-v1 checks and then SKIPS while the spo-v2 pins below are the 0
// placeholder). Root pins the two printed values below on the R6 head; the R6 head must then
// print the SAME values and pass. Never invent a value.
//
// Naming: Subject_Condition_ExpectedResult is not used here: the brief names the test.

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

// spo-v1: SpoPin's pins (strategy_spo_pin_test.cpp), the same procedures on the same fixture.
constexpr u64 pinned_v1_weights = 0xda6b6871e7e267c5ULL;
constexpr u64 pinned_v1_replay = 0xaabdbb72f99a6e13ULL;
// spo-v2: PLACEHOLDER (0 = unset) until root captures them on the pre-R6 tree (protocol
// above). While unset the test skips after its spo-v1 checks.
constexpr u64 pinned_v2_weights = 0;
constexpr u64 pinned_v2_replay = 0;

v7::NavV7Options spo(const sp::SpoParams& params, std::shared_ptr<const sp::RiskStore> risk) {
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_params = params;
  o.spo_risk = std::move(risk);
  return o;
}

TEST(SpoV3, V1AndV2DigestsUnchanged) {
  // spo-v1 as pre-registered (SpoParams{}) on SpoPin's role and model.
  const Role v1_role(40, 12, 53);
  const auto v1_model = dg::model_of(v1_role, 3);
  ASSERT_NE(v1_model->risk, nullptr);
  const auto v1_weights = dg::planned_weights(spo(sp::SpoParams{}, v1_model->risk), v1_role);
  const auto v1_replay = dg::replay(spo(sp::SpoParams{}, v1_model->risk), v1_role, 38);
  ASSERT_TRUE(v1_weights.error.empty()) << v1_weights.error;
  ASSERT_TRUE(v1_replay.error.empty()) << v1_replay.error;
  // spo-v2 (v2_params, w_max .5) on the role and model of SpoHook.SpoV2Calibrates...; every
  // column of spo_diagnostics.csv.
  const Role v2_role(30, 12, 71);
  const auto v2_model = dg::model_of(v2_role, 9);
  ASSERT_NE(v2_model->risk, nullptr);
  auto v2 = sp::v2_params();
  v2.w_max = 0.5;
  const auto v2_weights = dg::planned_weights(spo(v2, v2_model->risk), v2_role);
  const auto v2_replay = dg::replay(spo(v2, v2_model->risk), v2_role, dg::all_columns);
  ASSERT_TRUE(v2_weights.error.empty()) << v2_weights.error;
  ASSERT_TRUE(v2_replay.error.empty()) << v2_replay.error;
  std::printf("[spo-v3-pin] v1 weights=0x%016llx replay=0x%016llx\n",
              static_cast<unsigned long long>(v1_weights.value),
              static_cast<unsigned long long>(v1_replay.value));
  std::printf("[spo-v3-pin] v2 weights=0x%016llx (%zu diagnostics rows) replay=0x%016llx "
              "(%zu days)\n",
              static_cast<unsigned long long>(v2_weights.value), v2_weights.count,
              static_cast<unsigned long long>(v2_replay.value), v2_replay.count);
  EXPECT_EQ(v1_weights.value, pinned_v1_weights);
  EXPECT_EQ(v1_replay.value, pinned_v1_replay);
  if (pinned_v2_weights == 0 && pinned_v2_replay == 0) {
    GTEST_SKIP() << "spo-v2 digests unpinned (placeholder 0): capture them on the pre-R6 tree "
                    "with this file and strategy_spo_digest.hpp (protocol at the top), then pin";
  }
  EXPECT_EQ(v2_weights.value, pinned_v2_weights);
  EXPECT_EQ(v2_replay.value, pinned_v2_replay);
}
} // namespace
