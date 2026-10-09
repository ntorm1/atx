#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/backtest/dispersion.hpp"
#include "atx/vol/api/backtest/listed_dispersion.hpp"
#include "atx/vol/api/core/types.hpp"

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

// â”€â”€ WS-F F6 (BT-P2-8): quote-quality admission â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// The loader drops crossed NBBO, but SELECTION accepted a zero bid (mid becomes
// the fiction ask/2, on a side nobody can sell to) and had no staleness check at
// all beyond "not in the future" â€” a 30-minute-old row passed as current. Both
// could carry a straddle into the book and into the recorded `raw_mid` series.

TEST(ListedDispersion, ZeroBidStrikeIsNotSelectableAndIsCounted) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 99.0);
  add_pair(quotes, "SPY", id, expiry, 101.0);
  // N0 forward is 50.0: |49-50| == |51-50|, and the tie goes to the LOWER
  // strike, so 49 is what selection picks unless something excludes it.
  quotes.push_back(quote("N0", id++, expiry, 49.0, Side::Call, /*bid=*/0.0, /*ask=*/2.2));
  quotes.push_back(quote("N0", id++, expiry, 49.0, Side::Put));
  add_pair(quotes, "N0", id, expiry, 51.0);
  add_pair(quotes, "N1", id, expiry, 75.0);

  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  ASSERT_EQ(selected->names.size(), 2u);
  EXPECT_DOUBLE_EQ(selected->names[0].strike, 51.0)
      << "the zero-bid 49 strike is not an executable market and must not be selected";
  EXPECT_EQ(selected->quote_rejects.zero_bid, 1u);
  EXPECT_EQ(selected->quote_rejects.stale, 0u);

  // And the primitive says so directly.
  ListedOptionQuote zero_bid;
  zero_bid.bid = 0.0;
  zero_bid.ask = 2.2;
  EXPECT_FALSE(is_valid_listed_quote(zero_bid));
  zero_bid.quote_ts_ns = kValuation;
  EXPECT_EQ(classify_listed_quote(zero_bid, kValuation, ListedQuoteQualityConfig{}),
            ListedQuoteReject::ZeroBid);
}

TEST(ListedDispersion, StaleQuoteIsRejectedWithANamedReasonAndTheThresholdIsConfigurable) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  constexpr std::int64_t kMinute = 60LL * 1'000'000'000LL;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 99.0);
  add_pair(quotes, "N0", id, expiry, 49.0);
  // N1's only listed pair: the put is a 30-minute-old NBBO row.
  quotes.push_back(quote("N1", id++, expiry, 75.0, Side::Call));
  ListedOptionQuote stale = quote("N1", id++, expiry, 75.0, Side::Put);
  stale.quote_ts_ns = kValuation - 30 * kMinute;
  quotes.push_back(stale);

  // min_names = 1 so the run survives N1's removal and we can inspect the drop.
  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config(1));
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  ASSERT_EQ(selected->names.size(), 1u);
  EXPECT_EQ(selected->names[0].symbol, "N0");
  ASSERT_EQ(selected->dropped.size(), 1u);
  EXPECT_EQ(selected->dropped[0].symbol, "N1");
  EXPECT_EQ(selected->dropped[0].reason, ListedDropReason::NoValidStraddle);
  EXPECT_NE(selected->dropped[0].detail.find("Stale"), std::string::npos)
      << selected->dropped[0].detail;
  EXPECT_EQ(selected->quote_rejects.stale, 1u);

  // The threshold is policy, not law: widen it past 30 minutes and N1 returns.
  ListedDispersionSelectionConfig wide = config(1);
  wide.quality.max_quote_age_ns = 60 * kMinute;
  auto with_wide =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), wide);
  ASSERT_TRUE(with_wide.has_value()) << with_wide.error().to_string();
  EXPECT_EQ(with_wide->names.size(), 2u);
  EXPECT_EQ(with_wide->quote_rejects.stale, 0u);

  // 0 disables the check entirely (the pre-F6 contract), so the same fixture
  // that was rejected above is admitted â€” this is what makes the gate
  // observable rather than a property of the fixture.
  ListedDispersionSelectionConfig off = config(1);
  off.quality.max_quote_age_ns = 0;
  auto disabled = select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), off);
  ASSERT_TRUE(disabled.has_value()) << disabled.error().to_string();
  EXPECT_EQ(disabled->names.size(), 2u);
}

TEST(ListedDispersion, LockedMarketIsFlaggedButAdmittedUnlessPolicySaysOtherwise) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 99.0);
  // N0's nearest strike (49) has a LOCKED call: ask == bid. The mid is still the
  // true price, so the default policy admits it â€” but it must be COUNTED.
  quotes.push_back(quote("N0", id++, expiry, 49.0, Side::Call, /*bid=*/2.0, /*ask=*/2.0));
  quotes.push_back(quote("N0", id++, expiry, 49.0, Side::Put));
  add_pair(quotes, "N0", id, expiry, 51.0);
  add_pair(quotes, "N1", id, expiry, 75.0);

  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  ASSERT_EQ(selected->names.size(), 2u);
  EXPECT_DOUBLE_EQ(selected->names[0].strike, 49.0) << "a locked market is not a bad price";
  EXPECT_EQ(selected->quote_rejects.locked, 1u) << "the locked market must still be flagged";
  EXPECT_EQ(selected->quote_rejects.locked_dropped, 0u) << "the default policy drops nothing";
  EXPECT_EQ(selected->quote_rejects.total_dropped(), 0u);

  // An operator who does not want to trade locked markets can say so.
  ListedDispersionSelectionConfig strict = config();
  strict.quality.reject_locked = true;
  auto dropped_locked =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), strict);
  ASSERT_TRUE(dropped_locked.has_value()) << dropped_locked.error().to_string();
  ASSERT_EQ(dropped_locked->names.size(), 2u);
  EXPECT_DOUBLE_EQ(dropped_locked->names[0].strike, 51.0);
  EXPECT_EQ(dropped_locked->quote_rejects.locked, 1u);
  // FIX-F M2: the quote the POLICY refused has to appear in a dropped total.
  // Before this it appeared in none — `locked` was a flag count that
  // `total_dropped()` deliberately excluded, so a policy-dropped quote was
  // invisible to every total, including the persisted `total_dropped` column.
  EXPECT_EQ(dropped_locked->quote_rejects.locked_dropped, 1u);
  EXPECT_EQ(dropped_locked->quote_rejects.total_dropped(), 1u);
}

// FIX-F m4. A date whose selection FAILS is the date an operator most wants the
// admission tally for, and it used to produce no tally at all: the counts died
// with the Result's error, so `quote_rejects.tsv` had a row only where nothing
// went wrong. The out-parameter reports the first candidate expiry's tally
// regardless of outcome.
TEST(ListedDispersion, TheAdmissionTallySurvivesAFailedSelection) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  add_pair(quotes, "SPY", id, expiry, 99.0);
  // Every component quote is zero-bid, so no basket can form on this expiry —
  // but the gates rejected six quotes on the way to finding that out.
  for (const char *symbol : {"N0", "N1"}) {
    quotes.push_back(quote(symbol, id++, expiry, 50.0, Side::Call, /*bid=*/0.0, /*ask=*/2.0));
    quotes.push_back(quote(symbol, id++, expiry, 50.0, Side::Put, /*bid=*/0.0, /*ask=*/2.0));
  }

  ListedQuoteRejectCounts attempted{};
  auto failed = select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(),
                                         config(), &attempted);
  ASSERT_FALSE(failed.has_value()) << "the fixture must actually fail selection";
  EXPECT_EQ(attempted.zero_bid, 4u) << "the tally must survive the error";
  EXPECT_EQ(attempted.total_dropped(), 4u);

  // Passing no out-parameter is still valid and observes nothing.
  auto again =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  EXPECT_FALSE(again.has_value());
}

TEST(ListedDispersion, AZeroBidIndexQuoteCannotNominateAnExpiry) {
  // Expiry ranking is driven by INDEX quotes. A zero-bid SPY row used to be an
  // admissible nominator, so it could steer the whole basket onto a tenor no
  // tradeable index straddle existed at.
  // `bad` sits EXACTLY on the 30-day target, so it ranks first and wins unless
  // its index quotes are inadmissible. Both expiries carry a full name basket,
  // so nothing but the index-quote gate can change the outcome.
  const std::int64_t bad = kValuation + 30 * kDay;
  const std::int64_t good = kValuation + 35 * kDay;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  quotes.push_back(quote("SPY", id++, bad, 100.0, Side::Call, /*bid=*/0.0, /*ask=*/3.0));
  quotes.push_back(quote("SPY", id++, bad, 100.0, Side::Put, /*bid=*/0.0, /*ask=*/3.0));
  add_pair(quotes, "N0", id, bad, 50.0);
  add_pair(quotes, "N1", id, bad, 75.0);
  add_pair(quotes, "SPY", id, good, 100.0);
  add_pair(quotes, "N0", id, good, 50.0);
  add_pair(quotes, "N1", id, good, 75.0);

  auto selected =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_EQ(selected->expiry_ts_ns, good)
      << "an unquotable index row must not nominate its expiry";
}

// F6 review follow-up (Important #1). The staleness gate cannot fire on the
// current OPRA path: `opra_panel.cpp` never assigns `QuoteRow::ts_ns` and
// `listed_opra.cpp:330` substitutes the valuation instant when it is 0, so
// every quote's age is exactly 0.
//
// MEASURED, not assumed: `C:\atx-data\spy-dispersion\opra\*\2026-01-02.parquet`
// (6 symbols, 852-2833 rows each) carries ONE distinct `ts` per file — the
// 19:55:00Z snapshot instant. It is a snapshot stamp, not a per-quote
// observation time, so there is nothing to plumb through.
//
// A `stale = 0` count would therefore be a statement about the FEED wearing the
// clothes of a statement about the MARKET. This pins the distinction: quotes the
// gate could not evaluate are counted separately, and they are NOT rejections.
TEST(ListedDispersion, StalenessGateReportsWhenItCannotEvaluateRatherThanReportingZero) {
  const std::int64_t expiry = kValuation + 30 * kDay;
  constexpr std::int64_t kMinute = 60LL * 1'000'000'000LL;
  std::uint32_t id = 1;
  std::vector<ListedOptionQuote> quotes;
  // Every quote stamped AT the valuation instant — exactly what the OPRA join
  // produces from a snapshot-stamped panel.
  add_pair(quotes, "SPY", id, expiry, 99.0);
  add_pair(quotes, "N0", id, expiry, 49.0);
  add_pair(quotes, "N1", id, expiry, 75.0);

  auto snapshot_stamped =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(snapshot_stamped.has_value()) << snapshot_stamped.error().to_string();
  EXPECT_EQ(snapshot_stamped->names.size(), 2u);
  // Nothing was rejected as stale ...
  EXPECT_EQ(snapshot_stamped->quote_rejects.stale, 0u);
  // ... and that zero is explained rather than trusted: every inspected quote
  // was unevaluable, so the gate measured nothing.
  EXPECT_EQ(snapshot_stamped->quote_rejects.stale_unevaluable, 6u);
  // An unevaluable quote is ADMITTED, so it must not inflate the dropped count.
  EXPECT_EQ(snapshot_stamped->quote_rejects.total_dropped(), 0u);

  // A source that DOES carry an independent observation time is evaluated
  // normally — the machinery is inert on this feed, not broken.
  for (ListedOptionQuote &q : quotes) {
    q.quote_ts_ns = kValuation - 2 * kMinute; // fresh, but independently stamped
  }
  auto timestamped =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_TRUE(timestamped.has_value()) << timestamped.error().to_string();
  EXPECT_EQ(timestamped->quote_rejects.stale, 0u);
  EXPECT_EQ(timestamped->quote_rejects.stale_unevaluable, 0u)
      << "an independently stamped quote is evaluable, whether or not it is stale";

  // ... and the same source past the bound is genuinely rejected.
  for (ListedOptionQuote &q : quotes) {
    q.quote_ts_ns = kValuation - 30 * kMinute;
  }
  auto expired =
      select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), config());
  ASSERT_FALSE(expired.has_value()) << "every leg is stale; no basket should survive";

  // With the gate DISABLED nothing is unevaluable either — there is no gate to
  // be unable to evaluate.
  ListedDispersionSelectionConfig off = config();
  off.quality.max_quote_age_ns = 0;
  for (ListedOptionQuote &q : quotes) {
    q.quote_ts_ns = kValuation;
  }
  auto gate_off = select_listed_dispersion(kDate, kValuation, universe(), quotes, forwards(), off);
  ASSERT_TRUE(gate_off.has_value()) << gate_off.error().to_string();
  EXPECT_EQ(gate_off->quote_rejects.stale_unevaluable, 0u);
  EXPECT_EQ(gate_off->quote_rejects.stale, 0u);
}
