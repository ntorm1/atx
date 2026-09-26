#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/data/fundamental_fields_artifact.hpp"

namespace atx_test_d1_security_link {
namespace fund = atx::engine::data::fundamentals;
constexpr auto day = fund::kNanosPerDay;
fund::PitRecord record(int begin, int end, int clock, std::string owner, int period, double value) {
  fund::PitRecord r;
  r.identity_rule = fund::IdentityRule::DatedLinksV2;
  r.identity_valid_from_ns = begin * day; r.identity_valid_to_ns = end * day;
  r.link_available_ns = clock * day - 1; r.owner_id = std::move(owner); r.link_id = "proof";
  r.available_ns = 100 * day - 1; r.period_end_ns = period * day;
  r.values.fill(std::numeric_limits<double>::quiet_NaN()); r.values[0] = value;
  return r;
}
std::vector<atx::i64> axis() { return {100*day, 101*day, 102*day, 103*day, 104*day, 105*day}; }

TEST(SecurityLinkIntervals, ExpiryAndSeparateKnowledgeClockPermitOlderPeriodSuccessor) {
  const auto a = record(100, 103, 101, "A", 99, 10.0);
  const auto b = record(103, 105, 104, "B", 90, 20.0);
  const std::vector<fund::PitRecord> rows{a, b};
  fund::AlignConfig cfg; cfg.lag_sessions = 0;
  auto got = fund::align_pit_records(rows, axis(), 1, cfg);
  ASSERT_TRUE(got);
  EXPECT_TRUE(std::isnan(got->raw[0][0]));
  EXPECT_EQ(got->raw[0][1], 10.0); EXPECT_EQ(got->raw[0][2], 10.0);
  EXPECT_TRUE(std::isnan(got->raw[0][3]));
  EXPECT_EQ(got->raw[0][4], 20.0);
  EXPECT_TRUE(std::isnan(got->raw[0][5]));
}

TEST(SecurityLinkIntervals, FutureConflictMarkerDoesNotBackdateAndOverrideIsExplicit) {
  auto a = record(100, 106, 100, "A", 99, 10.0);
  auto b = record(100, 106, 103, "B", 0, 0.0);
  b.identity_only = true; b.values.fill(std::numeric_limits<double>::quiet_NaN());
  fund::AlignConfig cfg; cfg.lag_sessions = 0;
  std::vector<fund::PitRecord> rows{a, b};
  auto got = fund::align_pit_records(rows, axis(), 1, cfg);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->raw[0][2], 10.0);
  EXPECT_TRUE(std::isnan(got->raw[0][3]));
  EXPECT_EQ(got->stats.ambiguous_identity_cells, 3U);
  auto override_row = record(100, 106, 104, "A", 99, 10.0);
  override_row.link_priority = 1;
  rows.push_back(override_row);
  got = fund::align_pit_records(rows, axis(), 1, cfg);
  ASSERT_TRUE(got); EXPECT_EQ(got->raw[0][4], 10.0);
  EXPECT_TRUE(std::isnan(got->raw[0][3]));
  rows.back().identity_rule = fund::IdentityRule::LegacyStaticV1;
  EXPECT_FALSE(fund::align_pit_records(rows, axis(), 1, cfg));
}

TEST(SecurityLinkIntervals, VersionedDecoderFeedsActualAlignerAndRejectsMalformedRows) {
  std::string data = "ATX-FUNDAMENTAL-INTERVALS\t2\n";
  data += fund::kIntervalHeader;
  for (auto field : fund::kRawFieldNames) { data += '\t'; data += field; }
  data += "\n7\tSEC-CIK-0000000001\tproof\t" + std::to_string(100*day) + "\t" +
      std::to_string(90*day) + "\t" + std::to_string(100*day) + "\t" +
      std::to_string(103*day) + "\t" + std::to_string(101*day - 1) + "\t0\t0\t8";
  for (atx::usize i = 1; i < fund::kRawFieldCount; ++i) data += '\t';
  data += '\n';
  const std::vector<std::string> ids{"7"};
  auto records = fund::decode_interval_points(data, ids);
  ASSERT_TRUE(records); ASSERT_EQ(records->size(), 1U);
  fund::AlignConfig cfg; cfg.lag_sessions = 0;
  auto got = fund::align_pit_records(*records, axis(), 1, cfg);
  ASSERT_TRUE(got); EXPECT_EQ(got->raw[0][1], 8.0);
  EXPECT_TRUE(std::isnan(got->raw[0][3]));
  EXPECT_FALSE(fund::decode_interval_points("sr_id,cik,available_date\n", ids));
  EXPECT_FALSE(fund::decode_interval_points(data + "bad\n", ids));
  EXPECT_FALSE(fund::decode_interval_points(data, ids, 0));
}

TEST(SecurityLinkIntervals, DatedClockEqualityIsWithheldAndLegacyArithmeticIsPreserved) {
  auto row = record(100, 106, 100, "A", 99, 10.0);
  row.link_available_ns = 101 * day;
  fund::AlignConfig cfg; cfg.lag_sessions = 0;
  std::vector<fund::PitRecord> rows{row};
  auto got = fund::align_pit_records(rows, axis(), 1, cfg);
  ASSERT_TRUE(got);
  EXPECT_TRUE(std::isnan(got->raw[0][1])); EXPECT_EQ(got->raw[0][2], 10.0);
  rows[0].link_available_ns = 99 * day; rows[0].available_ns = 101 * day;
  got = fund::align_pit_records(rows, axis(), 1, cfg);
  ASSERT_TRUE(got);
  EXPECT_TRUE(std::isnan(got->raw[0][1])); EXPECT_EQ(got->raw[0][2], 10.0);
  rows[0].identity_rule = fund::IdentityRule::LegacyStaticV1;
  got = fund::align_pit_records(rows, axis(), 1, cfg);
  ASSERT_TRUE(got); EXPECT_EQ(got->raw[0][1], 10.0);
}
} // namespace atx_test_d1_security_link
