#include "analytics/earnings_forecast_loader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

// Coverage for Task 5's `load_earnings_events`: parses a real-schema
// tblstockearnforecasthist TSV slice (59 columns, TAB, CRLF -- see
// earnings_forecast_loader.cpp's module comment) and returns one ticker's
// `nextEarnDate1..8` UTC instants as sorted epoch-ns.
//
// The primary fixture (support/earnings_forecast_sample.tsv) is a byte-exact
// 4-row slice of a REAL 2026-02-10 SpiderRock export (header + AACG + AAIC +
// NVDA), not synthesized: AACG has ZERO real forward dates (all 8 slots hold
// the confirmed real "no forecast" sentinel "1970-01-01 00:00:01.000000"),
// AAIC has exactly 2, NVDA has the full 8. The two secondary fixtures
// (_bad_header / _bad_row) are minimal derivatives exercising the
// Err(InvalidArgument) paths.

namespace {

using atx::vol::load_earnings_events;

namespace fs = std::filesystem;

// The fixtures sit next to this test source (tests/support/). __FILE__ is NOT
// reliably absolute here (ninja invokes clang-cl with a path relative to the
// build dir), so probe a few relative candidates the way
// tests/support/oracle_pde_golden.cpp's golden_path() does -- robust to
// whichever directory ctest happens to run the test binary from.
fs::path fixture(const char *name) {
  const fs::path rel = fs::path("support") / name;
  for (const char *base : {"../../../atx-vol/tests", "atx-vol/tests", "../atx-vol/tests",
                           "."}) {
    fs::path candidate = fs::path(base) / rel;
    if (fs::exists(candidate)) return candidate;
  }
  return fs::path(__FILE__).parent_path() / "support" / name;
}

// Independently computed (bash `date -u -d ... +%s`, not via the code under
// test) UTC epoch-ns for the real NVDA/AAIC forward dates below -- an oracle
// external to the implementation, per the TDD brief.
constexpr std::int64_t kNvda1 = 1'772'056'800'000'000'000LL; // 2026-02-25 22:00:00 UTC
constexpr std::int64_t kNvda2 = 1'780'002'000'000'000'000LL; // 2026-05-28 21:00:00 UTC
constexpr std::int64_t kNvda3 = 1'787'778'000'000'000'000LL; // 2026-08-26 21:00:00 UTC
constexpr std::int64_t kNvda4 = 1'795'039'200'000'000'000LL; // 2026-11-18 22:00:00 UTC
constexpr std::int64_t kNvda5 = 1'803'592'800'000'000'000LL; // 2027-02-25 22:00:00 UTC
constexpr std::int64_t kNvda6 = 1'811'365'200'000'000'000LL; // 2027-05-26 21:00:00 UTC
constexpr std::int64_t kNvda7 = 1'819'227'600'000'000'000LL; // 2027-08-25 21:00:00 UTC
constexpr std::int64_t kNvda8 = 1'826'575'200'000'000'000LL; // 2027-11-18 22:00:00 UTC

constexpr std::int64_t kAaic1 = 1'772'488'800'000'000'000LL; // 2026-03-02 22:00:00 UTC
constexpr std::int64_t kAaic2 = 1'804'024'800'000'000'000LL; // 2027-03-02 22:00:00 UTC

// ── Step 1/2/4: happy path -- 8-date ticker (NVDA) ─────────────────────────

TEST(EarningsForecastLoader, EightDateTicker_SortedAscendingWithKnownFirstInstant) {
  const auto res = load_earnings_events(fixture("earnings_forecast_sample.tsv").string(), "NVDA");
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const std::vector<std::int64_t> &events = res.value();

  EXPECT_EQ(events.size(), std::size_t{8});
  EXPECT_TRUE(std::is_sorted(events.begin(), events.end()));
  EXPECT_EQ(events.front(), kNvda1);
  EXPECT_EQ(events.back(), kNvda8);

  const std::vector<std::int64_t> expected{kNvda1, kNvda2, kNvda3, kNvda4,
                                           kNvda5, kNvda6, kNvda7, kNvda8};
  EXPECT_EQ(events, expected);
}

// ── Boundary: a ticker with only 2 forward dates (rest sentinel) ──────────

TEST(EarningsForecastLoader, TwoDateTicker_DropsSentinelSlots) {
  const auto res = load_earnings_events(fixture("earnings_forecast_sample.tsv").string(), "AAIC");
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const std::vector<std::int64_t> &events = res.value();

  EXPECT_TRUE(std::is_sorted(events.begin(), events.end()));
  const std::vector<std::int64_t> expected{kAaic1, kAaic2};
  EXPECT_EQ(events, expected);
}

// ── Boundary: a ticker with ZERO forward dates (all 8 slots sentinel) ─────
// Confirms the sentinel-year check (year <= 1970) never leaks a spurious
// epoch-0/epoch-1 event, and that "no forecasts yet" is Ok(empty), not Err.

TEST(EarningsForecastLoader, ZeroDateTicker_ReturnsEmptyOk) {
  const auto res = load_earnings_events(fixture("earnings_forecast_sample.tsv").string(), "AACG");
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_TRUE(res.value().empty());
}

// ── Boundary: ticker absent from the file -> Err(NotFound) ────────────────

TEST(EarningsForecastLoader, MissingTicker_ReturnsNotFound) {
  const auto res = load_earnings_events(fixture("earnings_forecast_sample.tsv").string(), "ZZZZ");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::NotFound);
}

// ── Boundary: empty ticker argument -> Err(InvalidArgument) ───────────────

TEST(EarningsForecastLoader, EmptyTicker_ReturnsInvalidArgument) {
  const auto res = load_earnings_events(fixture("earnings_forecast_sample.tsv").string(), "");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── Error path: file cannot be opened -> Err(IoError) ──────────────────────

TEST(EarningsForecastLoader, MissingFile_ReturnsIoError) {
  const auto res = load_earnings_events("this/path/does/not/exist.tsv", "NVDA");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::IoError);
}

// ── Error path: header missing a required nextEarnDateN column ────────────

TEST(EarningsForecastLoader, MalformedHeader_ReturnsInvalidArgument) {
  const auto res =
      load_earnings_events(fixture("earnings_forecast_sample_bad_header.tsv").string(), "AAIC");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── Error path: matched row carries an un-parseable (non-sentinel, non-
//    empty) date cell ──────────────────────────────────────────────────────

TEST(EarningsForecastLoader, UnparseableDateCellInMatchedRow_ReturnsInvalidArgument) {
  const auto res =
      load_earnings_events(fixture("earnings_forecast_sample_bad_row.tsv").string(), "AAIC");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

} // namespace
