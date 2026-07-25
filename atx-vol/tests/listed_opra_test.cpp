#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "atx/vol/data.hpp"
#include "atx/vol/listed_opra.hpp"

namespace {

using atx::vol::iso_to_ns;
using atx::vol::ListedContractDefinition;
using atx::vol::ListedDefinitionTable;
using atx::vol::OpraInstrumentIdentity;
using atx::vol::OpraPanel;
using atx::vol::QuoteRow;
using atx::vol::Side;

constexpr const char *kDate = "2026-06-05";

OpraPanel panel() {
  OpraPanel out;
  out.frame.uid = "SPY";
  out.frame.snapshot_iso = "2026-06-05T19:55:00Z";
  out.frame.snapshot_ts_ns = iso_to_ns(out.frame.snapshot_iso);
  out.frame.rows = {
      QuoteRow{.uid = "SPY",
               .expiry_iso = "2026-07-17",
               .strike = 600.0,
               .side = Side::Call,
               .bid = 12.0,
               .ask = 12.2},
      QuoteRow{.uid = "SPY",
               .expiry_iso = "2026-07-17",
               .strike = 600.0,
               .side = Side::Put,
               .bid = 11.8,
               .ask = 12.0},
  };
  out.source_schema_version = 2;
  out.source_fingerprint = 991;
  out.provenance_complete = true;
  out.source_instrument_ids = {101, 102};
  out.source_identities = {
      OpraInstrumentIdentity{101, "SPY   260717C00600000"},
      OpraInstrumentIdentity{102, "SPY   260717P00600000"},
  };
  return out;
}

std::vector<ListedContractDefinition> definitions() {
  const std::int64_t definition_ts = iso_to_ns("2026-06-05T12:00:00Z");
  const std::int64_t expiry_ts = iso_to_ns("2026-07-17T20:00:00Z");
  return {
      ListedContractDefinition{kDate, 101, "SPY   260717C00600000", definition_ts, expiry_ts, 100.0,
                               true, true, 501},
      ListedContractDefinition{kDate, 102, "SPY   260717P00600000", definition_ts, expiry_ts, 100.0,
                               true, true, 502},
  };
}

TEST(ListedOpra, StrictJoinPreservesExactSourceIdentityAndExpiry) {
  auto table = ListedDefinitionTable::create(definitions());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  const OpraPanel source = panel();
  auto quotes =
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table);
  ASSERT_TRUE(quotes) << (quotes ? std::string{} : quotes.error().to_string());
  ASSERT_EQ(quotes->size(), 2u);
  EXPECT_EQ((*quotes)[0].instrument_id, 101u);
  EXPECT_EQ((*quotes)[0].raw_symbol, "SPY   260717C00600000");
  EXPECT_EQ((*quotes)[0].expiry_ts_ns, iso_to_ns("2026-07-17T20:00:00Z"));
  EXPECT_EQ((*quotes)[0].quote_ts_ns, source.frame.snapshot_ts_ns);
  EXPECT_EQ((*quotes)[0].multiplier, 100.0);
  EXPECT_TRUE((*quotes)[0].standard_monthly);
  EXPECT_TRUE((*quotes)[0].standard_deliverable);
  EXPECT_NE((*quotes)[0].source_fingerprint, 0u);
  EXPECT_NE((*quotes)[0].source_fingerprint, source.source_fingerprint);
}

TEST(ListedOpra, DefinitionTsvRoundTripsDeterministically) {
  auto table = ListedDefinitionTable::create(definitions());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  const std::string first = atx::vol::serialize_listed_definitions(*table);
  auto parsed = atx::vol::parse_listed_definitions(first);
  ASSERT_TRUE(parsed) << (parsed ? std::string{} : parsed.error().to_string());
  EXPECT_TRUE(std::ranges::equal(parsed->definitions(), table->definitions()));
  EXPECT_EQ(atx::vol::serialize_listed_definitions(*parsed), first);
  EXPECT_EQ(parsed->fingerprint(), table->fingerprint());

  const auto path = std::filesystem::temp_directory_path() / "atx-listed-definitions.tsv";
  ASSERT_TRUE(atx::vol::write_listed_definitions_file(path.string(), *table));
  auto loaded = atx::vol::read_listed_definitions_file(path.string());
  ASSERT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().to_string());
  EXPECT_TRUE(std::ranges::equal(loaded->definitions(), table->definitions()));
  std::error_code error;
  std::filesystem::remove(path, error);
}

TEST(ListedOpra, RejectsMissingLookAheadAndContradictoryDefinitions) {
  const OpraPanel source = panel();
  auto missing_definitions = ListedDefinitionTable::create({});
  ASSERT_TRUE(missing_definitions);
  EXPECT_FALSE(atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source,
                                                 *missing_definitions));

  auto future = definitions();
  future[0].definition_ts_ns = source.frame.snapshot_ts_ns + 1;
  auto future_table = ListedDefinitionTable::create(std::move(future));
  ASSERT_TRUE(future_table);
  EXPECT_FALSE(
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *future_table));

  auto wrong_expiry = definitions();
  wrong_expiry[0].expiry_ts_ns = iso_to_ns("2026-07-18T20:00:00Z");
  auto wrong_table = ListedDefinitionTable::create(std::move(wrong_expiry));
  ASSERT_TRUE(wrong_table);
  EXPECT_FALSE(
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *wrong_table));
}

TEST(ListedOpra, MissingNumericRootAdjustmentsAreExcluded) {
  OpraPanel source = panel();
  source.source_identities = {
      OpraInstrumentIdentity{101, "SPY1  260717C00600000"},
      OpraInstrumentIdentity{102, "SPY1  260717P00600000"},
  };
  auto empty = ListedDefinitionTable::create({});
  ASSERT_TRUE(empty);
  auto quotes =
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *empty);
  ASSERT_TRUE(quotes) << (quotes ? std::string{} : quotes.error().to_string());
  EXPECT_TRUE(quotes->empty());
}

TEST(ListedOpra, SkipsSameSessionZeroDteContractsMissingDefinition) {
  // Post-merge regression repro: the OPRA panel now keeps same-session (0DTE)
  // contracts (their 16:00-ET expiry instant is after the 19:55Z snapshot), but
  // the frozen definition authority predates them. The join therefore saw a
  // standard-root contract with NO definition row and hard-errored. Same-session
  // contracts are outside this consumer's 21-60 DTE universe and must be skipped
  // exactly like the numeric-root adjustments, not treated as a fatal gap.
  OpraPanel source = panel();
  source.frame.rows = {
      QuoteRow{.uid = "SPY",
               .expiry_iso = kDate, // expiry == trade date => same-session (0DTE)
               .strike = 600.0,
               .side = Side::Call,
               .bid = 1.0,
               .ask = 1.2},
  };
  source.source_instrument_ids = {201};
  source.source_identities = {
      OpraInstrumentIdentity{201, "SPY   260605C00600000"},
  };
  auto empty = ListedDefinitionTable::create({});
  ASSERT_TRUE(empty);
  auto quotes =
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *empty);
  ASSERT_TRUE(quotes) << (quotes ? std::string{} : quotes.error().to_string());
  EXPECT_TRUE(quotes->empty());
}

TEST(ListedOpra, SameSessionSkipIsDateGatedNotRootGated) {
  // The 0DTE skip is narrow: it keys on OSI expiry == trade date, NOT on the
  // missing-definition condition itself. A NON-0DTE standard-root contract with
  // no definition remains a hard error (a genuinely missing authority), while
  // the identical missing-definition condition on a same-session contract is
  // silently skipped.
  auto empty = ListedDefinitionTable::create({});
  ASSERT_TRUE(empty);

  OpraPanel non_zero_dte = panel(); // panel() expiries are 2026-07-17 (~40 DTE)
  EXPECT_FALSE(atx::vol::listed_quotes_from_opra(kDate, non_zero_dte.frame.snapshot_ts_ns,
                                                 non_zero_dte, *empty));

  OpraPanel same_session = panel();
  same_session.frame.rows = {
      QuoteRow{.uid = "SPY",
               .expiry_iso = kDate,
               .strike = 600.0,
               .side = Side::Put,
               .bid = 0.5,
               .ask = 0.7},
  };
  same_session.source_instrument_ids = {202};
  same_session.source_identities = {
      OpraInstrumentIdentity{202, "SPY   260605P00600000"},
  };
  auto quotes = atx::vol::listed_quotes_from_opra(kDate, same_session.frame.snapshot_ts_ns,
                                                  same_session, *empty);
  ASSERT_TRUE(quotes) << (quotes ? std::string{} : quotes.error().to_string());
  EXPECT_TRUE(quotes->empty());
}

TEST(ListedOpra, UnlistedContractFatalUnderErrorButDroppedUnderSkipUnlisted) {
  using atx::vol::MissingDefinitionPolicy;
  // Full-window blocker repro (2026-04-28): the OPRA panel quotes a standard-root,
  // non-0DTE contract that was listed intraday, so the point-in-time definition
  // authority has NO row for it. Under the default Error policy this is a fatal
  // NotFound; under the consumer-scoped SkipUnlisted policy the un-authoritative
  // quote is dropped and every defined contract still joins.
  OpraPanel source = panel(); // rows 101/102: standard 2026-07-17, both defined
  source.frame.rows.push_back(QuoteRow{.uid = "SPY",
                                       .expiry_iso = "2026-08-21", // ~non-0DTE, standard root
                                       .strike = 650.0,
                                       .side = Side::Call,
                                       .bid = 5.0,
                                       .ask = 5.2});
  source.source_instrument_ids.push_back(103);
  source.source_identities.push_back(OpraInstrumentIdentity{103, "SPY   260821C00650000"});

  auto table = ListedDefinitionTable::create(definitions()); // only 101, 102 defined
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());

  // Default argument and explicit Error: the missing definition is fatal.
  EXPECT_FALSE(
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table));
  EXPECT_FALSE(atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table,
                                                 MissingDefinitionPolicy::Error));

  // SkipUnlisted: the two defined contracts survive; the unlisted one is dropped.
  auto quotes = atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table,
                                                  MissingDefinitionPolicy::SkipUnlisted);
  ASSERT_TRUE(quotes) << (quotes ? std::string{} : quotes.error().to_string());
  ASSERT_EQ(quotes->size(), 2u);
  EXPECT_EQ((*quotes)[0].instrument_id, 101u);
  EXPECT_EQ((*quotes)[0].raw_symbol, "SPY   260717C00600000");
  EXPECT_EQ((*quotes)[1].instrument_id, 102u);
  EXPECT_EQ((*quotes)[1].raw_symbol, "SPY   260717P00600000");
}

TEST(ListedOpra, NumericRootAndSameSessionSkippedUnderBothPolicies) {
  using atx::vol::MissingDefinitionPolicy;
  // The structural skips in the nullptr branch (numeric-root adjustments and
  // same-session/0DTE contracts) fire BEFORE the policy check, so they drop
  // identically under Error and SkipUnlisted — the policy only governs the
  // genuine missing-authority fall-through, never these structural exclusions.
  OpraPanel source = panel();
  source.frame.rows = {
      QuoteRow{.uid = "SPY",
               .expiry_iso = "2026-07-17", // non-0DTE: skipped purely on numeric root
               .strike = 600.0,
               .side = Side::Call,
               .bid = 1.0,
               .ask = 1.2},
      QuoteRow{.uid = "SPY",
               .expiry_iso = kDate, // expiry == trade date => same-session (0DTE)
               .strike = 600.0,
               .side = Side::Put,
               .bid = 0.5,
               .ask = 0.7},
  };
  source.source_instrument_ids = {301, 302};
  source.source_identities = {
      OpraInstrumentIdentity{301, "SPY1  260717C00600000"}, // numeric root
      OpraInstrumentIdentity{302, "SPY   260605P00600000"}, // same-session
  };
  auto empty = ListedDefinitionTable::create({});
  ASSERT_TRUE(empty);
  for (const MissingDefinitionPolicy policy :
       {MissingDefinitionPolicy::Error, MissingDefinitionPolicy::SkipUnlisted}) {
    auto quotes = atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source,
                                                    *empty, policy);
    ASSERT_TRUE(quotes) << (quotes ? std::string{} : quotes.error().to_string());
    EXPECT_TRUE(quotes->empty());
  }
}

TEST(ListedOpra, LookAheadViolationFatalUnderSkipUnlisted) {
  using atx::vol::MissingDefinitionPolicy;
  // A definition that EXISTS but post-dates the valuation instant is corrupted
  // authority, not absent authority. SkipUnlisted softens only the nullptr
  // branch, so the look-ahead/expiry guard stays fatal even under SkipUnlisted.
  const OpraPanel source = panel();
  auto future = definitions();
  future[0].definition_ts_ns = source.frame.snapshot_ts_ns + 1; // look-ahead
  auto future_table = ListedDefinitionTable::create(std::move(future));
  ASSERT_TRUE(future_table);
  EXPECT_FALSE(atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source,
                                                 *future_table,
                                                 MissingDefinitionPolicy::SkipUnlisted));
}

TEST(ListedOpra, RejectsDailyIdentityAndAlignmentViolations) {
  auto table = ListedDefinitionTable::create(definitions());
  ASSERT_TRUE(table);

  OpraPanel source = panel();
  source.source_instrument_ids.pop_back();
  EXPECT_FALSE(
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table));

  source = panel();
  source.source_identities[1].raw_symbol = source.source_identities[0].raw_symbol;
  EXPECT_FALSE(
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table));

  source = panel();
  source.provenance_complete = false;
  EXPECT_FALSE(
      atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table));
}

// --- Calendar-free standard-monthly classifier ---------------------------
//
// standard_monthly_sessions derives each month's monthly settlement date from
// one trade date's full observed expiry set; is_standard_monthly_expiry tests
// membership. The rule: a month's monthly session is the third Friday if any
// observed contract expires then, else the Thursday immediately before it, else
// nothing. All dates are 2026 (weekday-verified in the task brief): third Friday
// is Apr-17, May-15, Jun-19, Aug-21; the Thursday before is Apr-16/May-14/
// Jun-18/Aug-20 respectively.

using atx::vol::is_standard_monthly_expiry;
using atx::vol::standard_monthly_sessions;

// Midnight-UTC expiry stamp for a "YYYY-MM-DD" date (matches the pulled
// definitions, which stamp expiration at midnight-UTC of the expiry date).
std::int64_t expiry_at(const char *date) { return iso_to_ns(std::string(date) + "T00:00:00Z"); }

TEST(StandardMonthlyClassifier, NormalMonthFlagsThirdFridayNotThursdayBefore) {
  // April 2026: third Friday Apr-17 present -> it is the monthly session. The
  // Apr-16 weekly Thursday and the Apr-10 (2nd) weekly Friday must NOT be
  // flagged when the Friday is in the set.
  const std::vector<std::int64_t> expiries = {
      expiry_at("2026-04-10"), expiry_at("2026-04-16"), expiry_at("2026-04-17"),
      expiry_at("2026-04-24")};
  const std::vector<std::int64_t> sessions = standard_monthly_sessions(expiries);
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_TRUE(is_standard_monthly_expiry(sessions, expiry_at("2026-04-17")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-04-16")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-04-10")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-04-24")));
}

TEST(StandardMonthlyClassifier, HolidayMonthShiftsToThursdayBefore) {
  // June 2026: the third Friday (Jun-19) is Juneteenth, an exchange holiday, so
  // NO contract expires then; monthlies settle Thursday Jun-18. The classifier
  // must flag Jun-18 (present) and never Jun-19 (absent), with no external
  // calendar. Jun-18 midnight-UTC ns is 1781740800000000000, Jun-19 is
  // 1781827200000000000 (task brief).
  const std::vector<std::int64_t> expiries = {
      expiry_at("2026-06-05"), expiry_at("2026-06-12"), expiry_at("2026-06-18"),
      expiry_at("2026-06-26")};
  ASSERT_EQ(expiry_at("2026-06-18"), 1781740800000000000LL);
  ASSERT_EQ(expiry_at("2026-06-19"), 1781827200000000000LL);
  const std::vector<std::int64_t> sessions = standard_monthly_sessions(expiries);
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_TRUE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-18")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-19")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-12")));
}

TEST(StandardMonthlyClassifier, MonthWithNeitherFridayNorThursdayFlagsNothing) {
  // June 2026 with only weeklies that are neither Jun-19 (Friday) nor Jun-18
  // (Thursday before): no monthly session exists in this set, flag nothing.
  const std::vector<std::int64_t> expiries = {expiry_at("2026-06-05"), expiry_at("2026-06-12"),
                                              expiry_at("2026-06-26")};
  const std::vector<std::int64_t> sessions = standard_monthly_sessions(expiries);
  EXPECT_TRUE(sessions.empty());
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-18")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-19")));
}

TEST(StandardMonthlyClassifier, HolidayShiftHandledAtDay14Boundary) {
  // May 2026 starts on a Friday, so the third Friday is May-15 and the Thursday
  // before is May-14 (the lower day-of-month boundary). Holiday sub-case:
  // May-15 absent, May-14 present -> May-14 is the session (day-14 Thursday
  // paired with a day-15 third Friday). Normal sub-case: May-15 present ->
  // May-15 is the session and May-14 is not flagged.
  const std::vector<std::int64_t> holiday = {expiry_at("2026-05-08"), expiry_at("2026-05-14"),
                                             expiry_at("2026-05-22")};
  const std::vector<std::int64_t> holiday_sessions = standard_monthly_sessions(holiday);
  ASSERT_EQ(holiday_sessions.size(), 1u);
  EXPECT_TRUE(is_standard_monthly_expiry(holiday_sessions, expiry_at("2026-05-14")));
  EXPECT_FALSE(is_standard_monthly_expiry(holiday_sessions, expiry_at("2026-05-15")));

  const std::vector<std::int64_t> normal = {expiry_at("2026-05-08"), expiry_at("2026-05-14"),
                                            expiry_at("2026-05-15"), expiry_at("2026-05-22")};
  const std::vector<std::int64_t> normal_sessions = standard_monthly_sessions(normal);
  ASSERT_EQ(normal_sessions.size(), 1u);
  EXPECT_TRUE(is_standard_monthly_expiry(normal_sessions, expiry_at("2026-05-15")));
  EXPECT_FALSE(is_standard_monthly_expiry(normal_sessions, expiry_at("2026-05-14")));
}

TEST(StandardMonthlyClassifier, ThirdFridayHandledAtDay21Boundary) {
  // August 2026 starts on a Saturday, so the third Friday is Aug-21 (the upper
  // day-of-month boundary). Aug-21 present -> it is the session; Aug-20 (the
  // Thursday before) must not be flagged when the Friday is present.
  const std::vector<std::int64_t> expiries = {expiry_at("2026-08-14"), expiry_at("2026-08-20"),
                                              expiry_at("2026-08-21"), expiry_at("2026-08-28")};
  const std::vector<std::int64_t> sessions = standard_monthly_sessions(expiries);
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_TRUE(is_standard_monthly_expiry(sessions, expiry_at("2026-08-21")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-08-20")));
}

TEST(StandardMonthlyClassifier, MultiMonthSetClassifiedPerMonthIndependently) {
  // One trade date observes many months at once (as the exporter passes them):
  // a normal April (Friday session) and a holiday-shifted June (Thursday
  // session) in the same set must each resolve on their own evidence.
  const std::vector<std::int64_t> expiries = {
      expiry_at("2026-04-16"), expiry_at("2026-04-17"), // April: Friday present
      expiry_at("2026-06-18")};                         // June: only Thursday (holiday)
  const std::vector<std::int64_t> sessions = standard_monthly_sessions(expiries);
  ASSERT_EQ(sessions.size(), 2u);
  EXPECT_TRUE(is_standard_monthly_expiry(sessions, expiry_at("2026-04-17")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-04-16")));
  EXPECT_TRUE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-18")));
  EXPECT_FALSE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-19")));
}

TEST(StandardMonthlyClassifier, ClassificationIsByUtcDateNotTimeOfDay) {
  // Expiry instants are canonicalized to their UTC date: the pulled definitions
  // stamp midnight-UTC while quote-derived stamps use 16:00 ET (20:00Z). Both
  // must classify identically. Here the session is built from a 20:00Z Jun-18
  // stamp and a midnight-UTC Jun-18 stamp must still be flagged.
  const std::vector<std::int64_t> expiries = {iso_to_ns("2026-06-18T20:00:00Z"),
                                              iso_to_ns("2026-06-05T20:00:00Z")};
  const std::vector<std::int64_t> sessions = standard_monthly_sessions(expiries);
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_TRUE(is_standard_monthly_expiry(sessions, expiry_at("2026-06-18")));
  EXPECT_TRUE(is_standard_monthly_expiry(sessions, iso_to_ns("2026-06-18T23:30:00Z")));
}

TEST(ListedOpra, DefinitionTableRejectsDuplicateAndFutureDateScopedRows) {
  auto duplicate = definitions();
  duplicate.push_back(duplicate.front());
  EXPECT_FALSE(ListedDefinitionTable::create(std::move(duplicate)));

  auto future = definitions();
  future[0].definition_ts_ns = iso_to_ns("2026-06-06T00:00:00Z");
  EXPECT_FALSE(ListedDefinitionTable::create(std::move(future)));
}

} // namespace
