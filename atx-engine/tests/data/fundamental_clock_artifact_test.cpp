#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/data/fundamental_fields_artifact.hpp"

namespace atx_test_fundamental_clock {
namespace fund = atx::engine::data::fundamentals;
constexpr auto day = fund::kNanosPerDay;
constexpr atx::i64 second = 1'000'000'000;
using Cells = std::array<std::string, 20 + fund::kRawFieldCount>;
Cells observed() {
  Cells c{};
  c[0] = "7"; c[1] = "issuer"; c[2] = "link";
  c[3] = std::to_string(100 * day + 60 * second);
  c[4] = std::to_string(90 * day); c[5] = std::to_string(90 * day);
  c[6] = std::to_string(110 * day); c[7] = std::to_string(90 * day);
  c[8] = "0"; c[9] = "0";
  c[10] = c[11] = std::to_string(std::numeric_limits<atx::i64>::max());
  c[12] = c[14] = std::to_string(100 * day);
  c[13] = "1"; c[15] = c[3]; c[16] = "0"; c[17] = c[18] = "1";
  c[19] = "accepted-public-v2"; c[20] = "12.5";
  return c;
}
std::string encode(const std::vector<Cells>& rows) {
  std::string s = "ATX-FUNDAMENTAL-INTERVALS\t4\n";
  s += fund::kIntervalHeader; s += fund::kRetirementHeader;
  s += "\tfiled_ns\tclock_kind\taccepted_ns\tpublished_ns\trevision_available_ns\t"
       "fact_vintage_qualified\tknowledge_clock_qualified\tclock_policy";
  for (auto field : fund::kRawFieldNames) { s += '\t'; s += field; }
  s += '\n';
  for (const auto& row : rows) {
    for (atx::usize i = 0; i < row.size(); ++i) { if (i) s += '\t'; s += row[i]; }
    s += '\n';
  }
  return s;
}
const std::vector<std::string> ids{"7"};

TEST(FundamentalClockV4, DefaultDispatchKeepsStrictPublicationAndIdentityClocks) {
  auto got = fund::decode_interval_points(encode({observed()}), ids);
  ASSERT_TRUE(got); ASSERT_EQ(got->size(), 1U);
  const auto available = 100 * day + 60 * second;
  const std::vector<atx::i64> dates{available - 1, available, available + 1};
  fund::AlignConfig cfg; cfg.lag_sessions = 0;
  auto aligned = fund::align_pit_records(*got, dates, 1, cfg);
  ASSERT_TRUE(aligned);
  EXPECT_TRUE(std::isnan(aligned->raw[0][0])); EXPECT_TRUE(std::isnan(aligned->raw[0][1]));
  EXPECT_EQ(aligned->raw[0][2], 12.5);
  EXPECT_EQ((*got)[0].identity_valid_to_ns, 110 * day);
  EXPECT_EQ((*got)[0].identity_retired_available_ns, std::numeric_limits<atx::i64>::max());
}

TEST(FundamentalClockV4, ModeledOverrideRelaxesOnlyClockNeverFactVintage) {
  auto c = observed();
  c[13] = "3"; c[14] = c[15] = "0"; c[18] = "0";
  c[3] = std::to_string(100 * day + 46 * 3600 * second); c[19] = "filed-plus46h-modeled-v2";
  fund::FundamentalClockAudit audit;
  auto got = fund::decode_qualified_interval_points(encode({c}), ids, {}, &audit);
  ASSERT_TRUE(got); EXPECT_TRUE(std::isnan((*got)[0].values[0]));
  EXPECT_EQ(audit.modeled_clock_rows, 1U); EXPECT_EQ(audit.withheld_rows, 1U);
  fund::QualifiedIntervalConfig cfg;
  cfg.admission = fund::FundamentalClockAdmission::AllowModeledClockV2;
  got = fund::decode_qualified_interval_points(encode({c}), ids, cfg, &audit);
  ASSERT_TRUE(got); EXPECT_EQ((*got)[0].values[0], 12.5); EXPECT_EQ(audit.admitted_rows, 1U);
  c[17] = "0";
  got = fund::decode_qualified_interval_points(encode({c}), ids, cfg, &audit);
  ASSERT_TRUE(got); EXPECT_TRUE(std::isnan((*got)[0].values[0]));
  EXPECT_EQ(audit.unqualified_vintage_rows, 1U);
  c = observed(); c[18] = "0"; // current public event cannot launder modeled inherited history
  got = fund::decode_qualified_interval_points(encode({c}), ids);
  ASSERT_TRUE(got); EXPECT_TRUE(std::isnan((*got)[0].values[0]));
}

TEST(FundamentalClockV4, AcceptanceLagAndRevisionAvailabilityAreValidatedExactly) {
  auto c = observed(); c[13] = "2"; c[15] = "0"; c[18] = "0";
  c[19] = "acceptance-plus180s-modeled-v2";
  c[3] = std::to_string(100 * day + 180 * second);
  fund::QualifiedIntervalConfig cfg; cfg.admission = fund::FundamentalClockAdmission::AllowModeledClockV2;
  auto got = fund::decode_qualified_interval_points(encode({c}), ids, cfg);
  ASSERT_TRUE(got); EXPECT_EQ((*got)[0].values[0], 12.5);
  c[16] = c[3] = std::to_string(102 * day);
  got = fund::decode_qualified_interval_points(encode({c}), ids, cfg);
  ASSERT_TRUE(got); EXPECT_EQ((*got)[0].available_ns, 102 * day);
  c[3] = std::to_string(101 * day);
  EXPECT_FALSE(fund::decode_qualified_interval_points(encode({c}), ids, cfg));
}

TEST(FundamentalClockV4, MalformedClocksAndHiddenAxisRowsFailInsteadOfBecomingMissing) {
  const auto refuse = [](atx::usize field, std::string value) {
    auto c = observed(); c[field] = std::move(value);
    EXPECT_FALSE(fund::decode_qualified_interval_points(encode({c}), ids));
  };
  refuse(3, std::to_string(100 * day));
  refuse(5, "-1"); refuse(7, "-1");
  refuse(15, std::to_string(99 * day));
  refuse(16, std::to_string(100 * day));
  refuse(17, "2"); refuse(19, "unknown"); refuse(20, "nan");
  auto c = observed(); c[0] = "absent"; c[15] = "-1";
  EXPECT_FALSE(fund::decode_qualified_interval_points(encode({c}), ids));
  c = observed(); c[13] = "3"; c[14] = c[15] = c[18] = "0";
  c[19] = "filed-plus46h-modeled-v2"; c[3] = std::to_string(101 * day); // legacy +24h is not V4 +46h
  EXPECT_FALSE(fund::decode_qualified_interval_points(encode({c}), ids));
}

TEST(FundamentalClockV4, WithheldSnapshotDoesNotRefreshOrRevokeOlderQualifiedValue) {
  auto old = observed(), withheld = observed();
  withheld[12] = withheld[14] = std::to_string(101 * day);
  withheld[3] = withheld[15] = std::to_string(101 * day + 60 * second);
  withheld[17] = "0"; withheld[20] = "999";
  auto got = fund::decode_interval_points(encode({old, withheld}), ids);
  ASSERT_TRUE(got); ASSERT_EQ(got->size(), 2U);
  EXPECT_TRUE(std::isnan((*got)[1].values[0]));
  fund::AlignConfig cfg; cfg.lag_sessions = 0; cfg.max_days_since_available = 2;
  const std::vector<atx::i64> dates{100 * day + 60 * second + 1,
      101 * day + 60 * second + 1, 103 * day};
  auto aligned = fund::align_pit_records(*got, dates, 1, cfg);
  ASSERT_TRUE(aligned);
  EXPECT_EQ(aligned->raw[0][0], 12.5);
  EXPECT_EQ(aligned->raw[0][1], 12.5); // prior public evidence, under its original clocks
  EXPECT_TRUE(std::isnan(aligned->raw[0][2])); // withheld event did not refresh age
}

TEST(FundamentalClockV4, MarkersBudgetsAndAuditCommitAreExplicit) {
  auto c = observed(); c[3] = c[4] = "0"; c[8] = "1";
  for (atx::usize i = 12; i < 19; ++i) c[i] = "0";
  c[19].clear(); c[20].clear();
  fund::FundamentalClockAudit audit;
  auto got = fund::decode_qualified_interval_points(encode({c, observed()}), ids, {}, &audit);
  ASSERT_TRUE(got); ASSERT_EQ(got->size(), 2U); EXPECT_TRUE((*got)[0].identity_only);
  EXPECT_EQ(audit.rows, 2U); EXPECT_EQ(audit.markers, 1U); EXPECT_EQ(audit.admitted_rows, 1U);
  fund::QualifiedIntervalConfig cfg; cfg.max_rows = 1;
  EXPECT_FALSE(fund::decode_qualified_interval_points(encode({c, observed()}), ids, cfg, &audit));
  EXPECT_EQ(audit.rows, 2U); // no partial audit on failure
  cfg.max_rows = 2; cfg.max_working_bytes = 1;
  EXPECT_FALSE(fund::decode_qualified_interval_points(encode({c}), ids, cfg));
  c[20] = "1";
  EXPECT_FALSE(fund::decode_qualified_interval_points(encode({c}), ids));
}
} // namespace atx_test_fundamental_clock
