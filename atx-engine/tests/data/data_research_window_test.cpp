// research_window.hpp is generated from atx-impl/strategies/research_window.json (platform v8
// W0-1, research-window-v2): these tests pin the header to the JSON source and read_strategy_role
// to the seal. ATX_RESEARCH_WINDOW_JSON is the JSON's source-tree path (tests/CMakeLists.txt).
#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>
#include "atx/core/error.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "strategy_role_fixture.hpp"

#ifndef ATX_RESEARCH_WINDOW_JSON
#error "ATX_RESEARCH_WINDOW_JSON must name atx-impl/strategies/research_window.json"
#endif

namespace {
using namespace atx;
using namespace atx_test_strategy_role;
namespace rw = atx::engine::data;

// Nanoseconds of YYYY-MM-DD at 00:00 UTC; -1 for anything else.
i64 date_ns(const std::string& text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') return -1;
  int year = 0;
  unsigned month = 0, day = 0;
  const auto field = [&text](usize at, usize len, auto& out) {
    const char* first = text.data() + at;
    const auto parsed = std::from_chars(first, first + len, out);
    return parsed.ec == std::errc{} && parsed.ptr == first + len;
  };
  if (!field(0, 4, year) || !field(5, 2, month) || !field(8, 2, day)) return -1;
  const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month},
                                        std::chrono::day{day}};
  if (!ymd.ok()) return -1;
  return static_cast<i64>(std::chrono::sys_days{ymd}.time_since_epoch().count()) * kDay;
}

// The role's first calendar-day session such that its last one (kD sessions) is `last_ns`.
i64 first_day_ending(i64 last_ns) { return last_ns / kDay - static_cast<i64>(kD) + 1; }

core::Result<engine::data::StrategyRoleData> read_role(const Directory& dir) {
  return engine::data::read_strategy_role((dir.path / "manifest.json").string(), 32ULL << 20);
}
} // namespace

TEST(ResearchWindow, HeaderMatchesJson) {
  std::ifstream in(ATX_RESEARCH_WINDOW_JSON);
  ASSERT_TRUE(in) << ATX_RESEARCH_WINDOW_JSON;
  const auto j = nlohmann::json::parse(in);
  const auto schema = j.at("schema").get<std::string>();
  ASSERT_EQ(schema, "atx.research-window/v2");
  EXPECT_EQ(rw::kTrainBeginNs, date_ns(j.at("train_begin").get<std::string>()));
  EXPECT_EQ(rw::kTrainEndExclusiveNs, date_ns(j.at("train_end_exclusive").get<std::string>()));
  EXPECT_EQ(rw::kSealBeginNs, date_ns(j.at("seal_begin").get<std::string>()));
  EXPECT_EQ(std::string(rw::kSealBeginDate), j.at("seal_begin").get<std::string>());
  // The window id is the schema without "atx." and with '/' -> '-' (research_window.py too).
  std::string id = schema.substr(4);
  id.replace(id.find('/'), 1, "-");
  EXPECT_EQ(std::string(rw::kResearchWindowId), id);
}

TEST(ResearchWindow, IsSealedFromTheSealSessionOn) {
  EXPECT_FALSE(rw::is_sealed(rw::kSealBeginNs - kDay));
  EXPECT_FALSE(rw::is_sealed(rw::kSealBeginNs - 1));
  EXPECT_TRUE(rw::is_sealed(rw::kSealBeginNs));
  EXPECT_TRUE(rw::is_sealed(rw::kSealBeginNs + kDay));
  EXPECT_FALSE(rw::is_sealed(rw::kTrainEndExclusiveNs - kDay));
}

// A synthetic role whose last session is the day after the seal (2024-01-02) is refused, and so
// is one whose last session is the seal day itself; the refusal names the window and the date.
TEST(ResearchWindow, RefusesSealedSession) {
  for (const i64 last : {rw::kSealBeginNs + kDay, rw::kSealBeginNs}) {
    Directory dir;
    ASSERT_TRUE(fixture(dir, false, first_day_ending(last)));
    const auto result = read_role(dir);
    ASSERT_FALSE(result) << last;
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
    const auto& message = result.error().message();
    EXPECT_NE(message.find(std::string(rw::kResearchWindowId)), std::string::npos) << message;
    EXPECT_NE(message.find(std::string(rw::kSealBeginDate)), std::string::npos) << message;
  }
}

// 2023 is TRAIN: a role whose last session is the day before the seal (its score end is the
// seal instant) is read.
TEST(ResearchWindow, Accepts2023) {
  Directory dir;
  ASSERT_TRUE(fixture(dir, false, first_day_ending(rw::kTrainEndExclusiveNs - kDay)));
  const auto result = read_role(dir);
  ASSERT_TRUE(result) << (result ? "" : result.error().to_string());
  EXPECT_EQ(result->session_keys.back(), rw::kTrainEndExclusiveNs - kDay);
  EXPECT_LT(result->session_keys.back(), rw::kSealBeginNs);
  EXPECT_EQ(result->score_end, kD);
}
