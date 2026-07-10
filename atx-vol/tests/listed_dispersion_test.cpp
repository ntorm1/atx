#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/types.hpp"

using namespace atx::vol;

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

constexpr std::int64_t kDay = static_cast<std::int64_t>(kListedNsPerDay);
constexpr std::int64_t kValuation = 1'700'000'000'000'000'000LL;
constexpr char kDate[] = "2026-07-10";

[[nodiscard]] DispersionUniverse universe(std::size_t n_names = 2) {
  DispersionUniverse u;
  u.index = DispersionMember{"SPY", 1u, 0.0};
  for (std::size_t i = 0; i < n_names; ++i) {
    u.names.push_back(DispersionMember{"N" + std::to_string(i), static_cast<std::uint32_t>(i + 2),
                                       static_cast<double>(i + 1)});
  }
  return u;
}

[[nodiscard]] ListedOptionQuote quote(std::string symbol, std::uint32_t id, std::int64_t expiry,
                                      double strike, Side side, double bid = 2.0,
                                      double ask = 2.2) {
  ListedOptionQuote q;
  q.trade_date = kDate;
  q.symbol = std::move(symbol);
  q.instrument_id = id;
  q.raw_symbol = q.symbol + std::to_string(id);
  q.expiry_ts_ns = expiry;
  q.strike = strike;
  q.side = side;
  q.bid = bid;
  q.ask = ask;
  q.quote_ts_ns = kValuation;
  q.multiplier = 100.0;
  q.standard_monthly = true;
  q.standard_deliverable = true;
  return q;
}

void add_pair(std::vector<ListedOptionQuote> &quotes, const std::string &symbol, std::uint32_t &id,
              std::int64_t expiry, double strike) {
  quotes.push_back(quote(symbol, id++, expiry, strike, Side::Call));
  quotes.push_back(quote(symbol, id++, expiry, strike, Side::Put));
}

[[nodiscard]] ListedForwardLookup forwards(double spy = 100.0, double n0 = 50.0, double n1 = 75.0) {
  return [=](const DispersionMember &member, std::int64_t) -> Result<double> {
    if (member.symbol == "SPY")
      return Ok(spy);
    if (member.symbol == "N0")
      return Ok(n0);
    if (member.symbol == "N1")
      return Ok(n1);
    return Err(ErrorCode::NotFound, "no forward");
  };
}

[[nodiscard]] ListedDispersionSelectionConfig config(std::size_t min_names = 2) {
  ListedDispersionSelectionConfig cfg;
  cfg.min_names = min_names;
  return cfg;
}

} // namespace

TEST(ListedDispersion, SelectsCommonMonthlyExpiryAndNearestListedStrikes) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 99.0);
  add_pair(quotes, "SPY", id, expiry, 101.0);
  add_pair(quotes, "N0", id, expiry, 49.0);
  add_pair(quotes, "N0", id, expiry, 51.0);
  add_pair(quotes, "N1", id, expiry, 74.0);
  add_pair(quotes, "N1", id, expiry, 76.0);

  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_EQ(selected->expiry_ts_ns, expiry);
  EXPECT_DOUBLE_EQ(selected->index.strike, 99.0);
  ASSERT_EQ(selected->names.size(), 2u);
  EXPECT_DOUBLE_EQ(selected->names[0].strike, 49.0);
  EXPECT_DOUBLE_EQ(selected->names[1].strike, 74.0);
  EXPECT_DOUBLE_EQ(selected->names[0].normalized_weight, 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(selected->names[1].normalized_weight, 2.0 / 3.0);
}

TEST(ListedDispersion, TriesNextRankedExpiryUntilMinimumBasketExists) {
  const std::int64_t e29 = kValuation + 29 * kDay;
  const std::int64_t e35 = kValuation + 35 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, e29, 100.0);
  add_pair(quotes, "N0", id, e29, 50.0);
  add_pair(quotes, "SPY", id, e35, 100.0);
  add_pair(quotes, "N0", id, e35, 50.0);
  add_pair(quotes, "N1", id, e35, 75.0);

  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_EQ(selected->expiry_ts_ns, e35);
}

TEST(ListedDispersion, DropsInvalidNameAndRenormalizesSurvivors) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 100.0);
  add_pair(quotes, "N0", id, expiry, 50.0);
  quotes.push_back(quote("N1", id++, expiry, 75.0, Side::Call));
  quotes.push_back(quote("N1", id++, expiry, 75.0, Side::Put, 3.0, 2.0));

  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config(1));
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  ASSERT_EQ(selected->names.size(), 1u);
  EXPECT_DOUBLE_EQ(selected->names[0].normalized_weight, 1.0);
  ASSERT_EQ(selected->dropped.size(), 1u);
  EXPECT_EQ(selected->dropped[0].symbol, "N1");
  EXPECT_EQ(selected->dropped[0].reason, ListedDropReason::NoValidStraddle);
}

TEST(ListedDispersion, RejectsLookAheadAndAmbiguousDailyIdentity) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 100.0);
  add_pair(quotes, "N0", id, expiry, 50.0);
  add_pair(quotes, "N1", id, expiry, 75.0);
  quotes.back().quote_ts_ns = kValuation + 1;

  auto future =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_FALSE(future.has_value());
  EXPECT_EQ(future.error().code(), ErrorCode::InvalidArgument);

  quotes.back().quote_ts_ns = kValuation;
  quotes.back().instrument_id = quotes.front().instrument_id;
  auto ambiguous =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_FALSE(ambiguous.has_value());
  EXPECT_EQ(ambiguous.error().code(), ErrorCode::InvalidArgument);
}

TEST(ListedDispersion, RejectsAdjustedOrNonstandardDeliverables) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 100.0);
  add_pair(quotes, "N0", id, expiry, 50.0);
  add_pair(quotes, "N1", id, expiry, 75.0);
  for (ListedOptionQuote &q : quotes) {
    if (q.symbol == "N1")
      q.standard_deliverable = false;
  }

  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config(1));
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  ASSERT_EQ(selected->names.size(), 1u);
  ASSERT_EQ(selected->dropped.size(), 1u);
  EXPECT_EQ(selected->dropped[0].symbol, "N1");
}

TEST(ListedDispersion, ResultIsInvariantToInputOrder) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 100.0);
  add_pair(quotes, "N0", id, expiry, 50.0);
  add_pair(quotes, "N1", id, expiry, 75.0);
  auto a = select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  std::reverse(quotes.begin(), quotes.end());
  auto b = select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(*a, *b);
}

TEST(ListedDispersion, IndexFailureAndTooFewNamesAreUnavailable) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "N0", id, expiry, 50.0);
  add_pair(quotes, "N1", id, expiry, 75.0);
  auto no_index =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_FALSE(no_index.has_value());
  EXPECT_EQ(no_index.error().code(), ErrorCode::Unavailable);

  add_pair(quotes, "SPY", id, expiry, 100.0);
  quotes.erase(
      std::remove_if(quotes.begin(), quotes.end(), [](const auto &q) { return q.symbol == "N1"; }),
      quotes.end());
  auto too_few =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_FALSE(too_few.has_value());
  EXPECT_EQ(too_few.error().code(), ErrorCode::Unavailable);
}

TEST(ListedDispersion, QuoteValidityContractIsExplicit) {
  ListedOptionQuote q;
  q.bid = 1.0;
  q.ask = 1.1;
  EXPECT_TRUE(is_valid_listed_quote(q));
  q.ask = 0.0;
  EXPECT_FALSE(is_valid_listed_quote(q));
  q.ask = 0.9;
  EXPECT_FALSE(is_valid_listed_quote(q));
  q.bid = -0.1;
  q.ask = 1.0;
  EXPECT_FALSE(is_valid_listed_quote(q));
}
