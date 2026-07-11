#include <gtest/gtest.h>

#include <string>

#include "atx/vol/occ_ess.hpp"

namespace {

constexpr const char *kReport =
    "1THE OPTIONS CLEARING CORPORATION\r\n"
    " NON-STANDARD SETTLEMENTS MRD REPORT ACTIVITY DATE 01/02/26 PROGRAM-ID DLVC1910AS\r\n"
    " REC PROD PKND SRTK ONN CMPN CMPN SECU UNIT SETL STRK FIXED PROCESS SETTLE\r\n"
    "0706  ACET1   OSTK  USD   EU    02     01   ACET   6  CNS  100  0.000000  20260102  00000000\r\n"
    "0706  ACET1   OSTK  USD   EU    02     02   USD  100  MON  0  0.001900  20260102  20260102\r\n"
    "0706  ADVM    OSTK  USD   EU    01     01   ADVM 100  MON 100  3.560000  20260102  20251209\r\n";

TEST(OccEss, ParsesCanonicalSpecialSymbolSetAndFingerprint) {
  auto report = atx::vol::parse_occ_ess_report(kReport);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_EQ(report->trade_date(), "2026-01-02");
  ASSERT_EQ(report->special_symbols().size(), 2u);
  EXPECT_EQ(report->special_symbols()[0], "ACET1");
  EXPECT_EQ(report->special_symbols()[1], "ADVM");
  EXPECT_TRUE(report->is_special("ADVM"));
  EXPECT_FALSE(report->is_special("AAPL"));
  EXPECT_NE(report->source_fingerprint(), 0u);

  auto again = atx::vol::parse_occ_ess_report(kReport);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->source_fingerprint(), report->source_fingerprint());
}

TEST(OccEss, RejectsWrongDateOrMalformedRows) {
  std::string malformed = kReport;
  const std::size_t process = malformed.find("20260102");
  ASSERT_NE(process, std::string::npos);
  malformed.replace(process, 8u, "20260103");
  EXPECT_FALSE(atx::vol::parse_occ_ess_report(malformed));

  malformed = kReport;
  malformed.replace(malformed.find("OSTK"), 4u, "FUTR");
  EXPECT_FALSE(atx::vol::parse_occ_ess_report(malformed));

  EXPECT_FALSE(atx::vol::parse_occ_ess_report("NON-STANDARD SETTLEMENTS"));
}

} // namespace
