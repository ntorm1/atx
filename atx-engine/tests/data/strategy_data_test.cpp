#include <gtest/gtest.h>

#include <fstream>
#include <string>
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

// Review B-3 (Ruling E-10): only a declared universe.delisting.returns_applied true refuses a
// signal role; a base role, a universe without delisting and a marked-only role pass, and a
// malformed declaration refuses, each refusal naming the manifest.
TEST(StrategyResearchRole, DelistingReturnsRoleIsRefusedAsSignalRole) {
  namespace ed = atx::engine::data;
  const std::string path = "roles/b0c/manifest.json";
  const std::string base = R"({"schema":"atx.recent-research-role/v1"})";
  const std::string universe = R"({"universe":{"id":"linked-operating-v1"}})";
  const std::string marked = R"({"universe":{"delisting":{"returns_applied":false}}})";
  const std::string applied = R"({"universe":{"delisting":{"returns_applied":true,"applied":{}}}})";
  for (const auto* text : {&base, &universe, &marked}) {
    const auto flag = ed::role_delisting_returns_applied(*text);
    ASSERT_TRUE(flag) << *text;
    EXPECT_FALSE(*flag) << *text;
    EXPECT_TRUE(ed::refuse_delisting_returns_signal_role(*text, path)) << *text;
  }
  const auto flag = ed::role_delisting_returns_applied(applied);
  ASSERT_TRUE(flag);
  EXPECT_TRUE(*flag);
  const auto refused = ed::refuse_delisting_returns_signal_role(applied, path);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), core::ErrorCode::InvalidArgument);
  const auto message = refused.error().message();
  EXPECT_NE(message.find(path), std::string::npos) << message;
  EXPECT_NE(message.find("--delisting-returns"), std::string::npos) << message;
  EXPECT_NE(message.find("universe.delisting.returns_applied"), std::string::npos) << message;
  for (const std::string& malformed : {std::string("[1]"), std::string("{"),
                                      std::string(R"({"universe":[]})"),
                                      std::string(R"({"universe":{"delisting":true}})"),
                                      std::string(R"({"universe":{"delisting":{}}})"),
                                      std::string(R"({"universe":{"delisting":{"returns_applied":"yes"}}})")}) {
    EXPECT_FALSE(ed::role_delisting_returns_applied(malformed)) << malformed;
    const auto status = ed::refuse_delisting_returns_signal_role(malformed, path);
    ASSERT_FALSE(status) << malformed;
    EXPECT_NE(status.error().message().find(path), std::string::npos) << malformed;
  }
}
