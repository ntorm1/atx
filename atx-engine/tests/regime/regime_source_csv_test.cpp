#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/regime/source_csv.hpp"

namespace atxtest_regime_csv {
using atx::engine::regime::CsvFormat;
using atx::engine::regime::date_to_nanos;
using atx::engine::regime::parse_series_content;

TEST(RegimeCsv, DateToNanos_KnownEpochDay) {
  auto r = date_to_nanos("1970-01-02");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), static_cast<atx::i64>(86400) * 1000000000LL);
}

TEST(RegimeCsv, Fred_ParsesValueColumn_AndDotIsNaN) {
  const std::string csv =
      "DATE,VALUE\n"
      "2020-01-02,12.5\n"
      "2020-01-03,.\n"
      "2020-01-06,13.0\n";
  auto r = parse_series_content(csv, CsvFormat::Fred, "VALUE");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  const auto& v = r.value();
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0].first, date_to_nanos("2020-01-02").value());
  EXPECT_DOUBLE_EQ(v[0].second, 12.5);
  EXPECT_TRUE(std::isnan(v[1].second));
  EXPECT_DOUBLE_EQ(v[2].second, 13.0);
}

TEST(RegimeCsv, Cboe_ReadsNamedCloseColumn) {
  const std::string csv =
      "DATE,OPEN,HIGH,LOW,CLOSE\n"
      "2021-03-01,10,11,9,10.5\n";
  auto r = parse_series_content(csv, CsvFormat::Cboe, "CLOSE");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  ASSERT_EQ(r.value().size(), 1u);
  EXPECT_DOUBLE_EQ(r.value()[0].second, 10.5);
}

TEST(RegimeCsv, SortsAscending_AndDedupsLastWins) {
  const std::string csv =
      "DATE,VALUE\n"
      "2020-01-06,2\n"
      "2020-01-02,1\n"
      "2020-01-06,9\n";   // duplicate date -> last wins
  auto r = parse_series_content(csv, CsvFormat::Fred, "VALUE");
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r.value().size(), 2u);
  EXPECT_EQ(r.value()[0].first, date_to_nanos("2020-01-02").value());
  EXPECT_DOUBLE_EQ(r.value()[1].second, 9.0);
}

TEST(RegimeCsv, MissingValueColumn_IsError) {
  const std::string csv = "DATE,NOPE\n2020-01-02,1\n";
  EXPECT_FALSE(parse_series_content(csv, CsvFormat::Fred, "VALUE").has_value());
}
}  // namespace atxtest_regime_csv
