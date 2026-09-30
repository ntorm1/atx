#include <gtest/gtest.h>

#include <fstream>
#include <vector>

#include "atx/engine/data/strategy_data.hpp"
#include "strategy_role_fixture.hpp"

namespace {
using namespace atx;
using namespace atx_test_strategy_role;
} // namespace

TEST(StrategyResearchRole, PreservesIndependentMembershipMissingMarksAndDeclaredClocks) {
  Directory dir; ASSERT_TRUE(fixture(dir));
  auto result = atx::engine::data::read_strategy_role((dir.path / "manifest.json").string(), 32ULL << 20);
  ASSERT_TRUE(result) << (result ? "" : result.error().to_string());
  EXPECT_EQ(result->panel.dates(), kD); EXPECT_EQ(result->panel.instruments(), kN);
  EXPECT_EQ(result->score_begin, 384U); EXPECT_EQ(result->score_end, kD);
  EXPECT_EQ(result->instrument_ids, (std::vector<u64>{7, 81}));
  EXPECT_EQ(result->decision_member[385 * kN], 1);
  EXPECT_FALSE(result->panel.in_universe(385, 0));
  EXPECT_EQ(result->mark_times_ns[384], result->session_keys[384] + 22 * kDay / 24);
  EXPECT_LT(result->mark_times_ns[384], result->decision_times_ns[384]);
  EXPECT_LT(result->decision_times_ns[384], result->mark_times_ns[385]);
  EXPECT_EQ(result->manifest_sha256.size(), 64U);
}

TEST(StrategyResearchRole, RejectsFiniteAbsentPayloadExtentHashAndBudgetViolations) {
  Directory finite; ASSERT_TRUE(fixture(finite, true));
  EXPECT_FALSE(atx::engine::data::read_strategy_role((finite.path / "manifest.json").string(), 32ULL << 20));
  Directory good; ASSERT_TRUE(fixture(good));
  EXPECT_FALSE(atx::engine::data::read_strategy_role((good.path / "manifest.json").string(), 16ULL << 20));
  {
    std::fstream f(good.path / "close.f64", std::ios::binary | std::ios::in | std::ios::out);
    f.put('x'); // exact same size, different numeric bytes
  }
  EXPECT_FALSE(atx::engine::data::read_strategy_role((good.path / "manifest.json").string(), 32ULL << 20));
  {
    std::ofstream f(good.path / "volume.f64", std::ios::binary | std::ios::app); f.put('x');
  }
  EXPECT_FALSE(atx::engine::data::read_strategy_role((good.path / "manifest.json").string(), 32ULL << 20));
}
