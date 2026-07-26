#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
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

// --- The leg-key filter (`wanted`) ---------------------------------------
//
// A non-empty `wanted` declares the exact contract set the caller will read, so
// the join can skip the definition lookup, the OSI parse and quote construction
// for every other panel row. Two properties have to hold at once:
//
//   RESULT EQUALITY — the filtered result is exactly the unfiltered result
//   intersected with `wanted`, element for element and in panel order. A filter
//   that under-drops changes nothing observable but buys nothing; one that
//   over-drops silently loses a leg the reconciliation then cannot mark.
//
//   THE NARROWED GATES STILL FIRE — three of the join's four fatal checks
//   (definition look-ahead/expiry, quote/OSI/definition economics, future
//   quote) now run only for the consumed keys. That narrowing is deliberate and
//   documented in the header, but each gate MUST still be fatal when the
//   offending row is one the caller asked for. The fourth (aligned source
//   identity missing) is panel-wide and must stay fatal even for a row nobody
//   wants, because it precedes the filter.
//
// Both directions were observed RED: with the filter ignored (a `wanted` the
// implementation accepted and did not apply) the equality tests failed; with the
// filter applied unconditionally to every row, the three narrowed-gate tests
// failed while the panel-wide identity test still passed. The output of both is
// in the task report.

using atx::vol::ListedOptionQuote;
using atx::vol::ListedQuoteKey;

constexpr const char *kWideExpiryIso = "2026-07-17";
constexpr const char *kWideExpiryStamp = "2026-07-17T20:00:00Z";

struct WideRow {
  std::uint32_t id;
  const char *raw_symbol;
  double strike;
  Side side;
};

// Six defined contracts: three strikes x two sides, one expiry. Instrument ids
// ascend so `source_identities` is sorted, as the join's lower_bound requires.
const std::vector<WideRow> &wide_rows() {
  static const std::vector<WideRow> rows = {
      {101u, "SPY   260717C00590000", 590.0, Side::Call},
      {102u, "SPY   260717P00590000", 590.0, Side::Put},
      {103u, "SPY   260717C00600000", 600.0, Side::Call},
      {104u, "SPY   260717P00600000", 600.0, Side::Put},
      {105u, "SPY   260717C00610000", 610.0, Side::Call},
      {106u, "SPY   260717P00610000", 610.0, Side::Put},
  };
  return rows;
}

OpraPanel wide_panel() {
  OpraPanel out;
  out.frame.uid = "SPY";
  out.frame.snapshot_iso = "2026-06-05T19:55:00Z";
  out.frame.snapshot_ts_ns = iso_to_ns(out.frame.snapshot_iso);
  out.source_schema_version = 2;
  out.source_fingerprint = 991;
  out.provenance_complete = true;
  double bid = 10.0;
  for (const WideRow &row : wide_rows()) {
    // Distinct bid/ask per row so an element-wise comparison has something to
    // discriminate beyond the key fields.
    out.frame.rows.push_back(QuoteRow{.uid = "SPY",
                                      .expiry_iso = kWideExpiryIso,
                                      .strike = row.strike,
                                      .side = row.side,
                                      .bid = bid,
                                      .ask = bid + 0.25});
    out.source_instrument_ids.push_back(row.id);
    out.source_identities.push_back(OpraInstrumentIdentity{row.id, row.raw_symbol});
    bid += 1.0;
  }
  return out;
}

std::vector<ListedContractDefinition> wide_definitions_for_panel() {
  const std::int64_t definition_ts = iso_to_ns("2026-06-05T12:00:00Z");
  const std::int64_t expiry_ts = iso_to_ns(kWideExpiryStamp);
  std::vector<ListedContractDefinition> out;
  std::uint64_t print = 501;
  for (const WideRow &row : wide_rows()) {
    out.push_back(ListedContractDefinition{kDate, row.id, row.raw_symbol, definition_ts, expiry_ts,
                                           100.0, true, true, print++});
  }
  return out;
}

ListedQuoteKey wide_key(std::size_t index) {
  const WideRow &row = wide_rows()[index];
  return ListedQuoteKey{row.raw_symbol, iso_to_ns(kWideExpiryStamp), row.strike, row.side};
}

// `wanted` is contractually sorted + deduped, exactly as the run-backtest caller
// builds it.
std::vector<ListedQuoteKey> sorted_wanted(std::vector<ListedQuoteKey> keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

// Every field, on raw doubles — not a key-only comparison, which would pass on a
// filter that rebuilt the quote from the wanted key instead of from the panel.
void expect_quote_eq(const ListedOptionQuote &got, const ListedOptionQuote &want,
                     const char *where) {
  EXPECT_EQ(got.trade_date, want.trade_date) << where;
  EXPECT_EQ(got.symbol, want.symbol) << where;
  EXPECT_EQ(got.instrument_id, want.instrument_id) << where;
  EXPECT_EQ(got.raw_symbol, want.raw_symbol) << where;
  EXPECT_EQ(got.expiry_ts_ns, want.expiry_ts_ns) << where;
  EXPECT_EQ(got.strike, want.strike) << where;
  EXPECT_EQ(got.side, want.side) << where;
  EXPECT_EQ(got.bid, want.bid) << where;
  EXPECT_EQ(got.ask, want.ask) << where;
  EXPECT_EQ(got.quote_ts_ns, want.quote_ts_ns) << where;
  EXPECT_EQ(got.multiplier, want.multiplier) << where;
  EXPECT_EQ(got.standard_monthly, want.standard_monthly) << where;
  EXPECT_EQ(got.standard_deliverable, want.standard_deliverable) << where;
  EXPECT_EQ(got.source_fingerprint, want.source_fingerprint) << where;
}

TEST(ListedOpra, FilteredJoinEqualsUnfilteredOnWantedKeys) {
  using atx::vol::MissingDefinitionPolicy;
  auto table = ListedDefinitionTable::create(wide_definitions_for_panel());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  const OpraPanel source = wide_panel();

  auto full = atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table);
  ASSERT_TRUE(full) << (full ? std::string{} : full.error().to_string());
  ASSERT_EQ(full->size(), 6u); // anti-vacuity: the unfiltered join really is wide

  // Three keys, chosen NON-CONTIGUOUSLY in panel order (rows 0, 3, 4) so the
  // expected subset is not a prefix or a run.
  const std::vector<ListedQuoteKey> wanted =
      sorted_wanted({wide_key(0), wide_key(3), wide_key(4)});
  ASSERT_EQ(wanted.size(), 3u);

  std::vector<ListedOptionQuote> expected;
  for (const ListedOptionQuote &quote : *full) {
    if (std::binary_search(wanted.begin(), wanted.end(), atx::vol::quote_key_of(quote))) {
      expected.push_back(quote);
    }
  }
  // Anti-vacuity: the filter must actually remove rows, and keep some.
  ASSERT_EQ(expected.size(), 3u);
  ASSERT_LT(expected.size(), full->size());

  auto filtered = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, wanted);
  ASSERT_TRUE(filtered) << (filtered ? std::string{} : filtered.error().to_string());
  ASSERT_EQ(filtered->size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect_quote_eq((*filtered)[i], expected[i], "filtered vs unfiltered subset");
  }
  // ...and the surviving order is panel order (590C, 600P, 610C), not wanted order.
  EXPECT_EQ((*filtered)[0].instrument_id, 101u);
  EXPECT_EQ((*filtered)[1].instrument_id, 104u);
  EXPECT_EQ((*filtered)[2].instrument_id, 105u);
}

TEST(ListedOpra, FilteredJoinEmptyWantedIsUnfiltered) {
  using atx::vol::MissingDefinitionPolicy;
  // POSITIVE CONTROL, not a gate: an empty `wanted` is the parameter's default,
  // so this passes by construction against both the pre-change code and any
  // implementation of the filter. It pins the default-argument contract (adding
  // a parameter must not move an existing caller) and nothing more.
  auto table = ListedDefinitionTable::create(wide_definitions_for_panel());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  const OpraPanel source = wide_panel();

  auto full = atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table);
  ASSERT_TRUE(full) << (full ? std::string{} : full.error().to_string());
  ASSERT_EQ(full->size(), 6u);

  auto empty_span = atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source,
                                                      *table, MissingDefinitionPolicy::Error,
                                                      std::span<const ListedQuoteKey>{});
  ASSERT_TRUE(empty_span) << (empty_span ? std::string{} : empty_span.error().to_string());
  ASSERT_EQ(empty_span->size(), full->size());
  for (std::size_t i = 0; i < full->size(); ++i) {
    expect_quote_eq((*empty_span)[i], (*full)[i], "empty wanted vs default");
  }
}

TEST(ListedOpra, FilteredJoinExcludesWantedRawSymbolWithDifferentExpiry) {
  using atx::vol::MissingDefinitionPolicy;
  // The pre-lookup stage keys on raw_symbol alone (the definition's
  // expiry_ts_ns is not known until after the lookup), so the EXACT key has to
  // be re-checked before the quote is emitted. A `wanted` entry carrying the
  // right raw_symbol but a different expiry instant must not admit the row.
  auto table = ListedDefinitionTable::create(wide_definitions_for_panel());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  const OpraPanel source = wide_panel();

  ListedQuoteKey skewed = wide_key(2);
  skewed.expiry_ts_ns += 1; // same raw_symbol, one nanosecond off
  ASSERT_NE(skewed.expiry_ts_ns, wide_key(2).expiry_ts_ns);

  const std::vector<ListedQuoteKey> wanted = sorted_wanted({wide_key(0), skewed});
  ASSERT_EQ(wanted.size(), 2u);

  auto filtered = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, wanted);
  ASSERT_TRUE(filtered) << (filtered ? std::string{} : filtered.error().to_string());
  ASSERT_EQ(filtered->size(), 1u) << "only the exactly-matching key may survive";
  EXPECT_EQ((*filtered)[0].instrument_id, 101u);
}

TEST(ListedOpra, FilteredJoinStillFatalOnWantedLookAhead) {
  using atx::vol::MissingDefinitionPolicy;
  // NARROWED GATE 2. The definition for a WANTED contract post-dates the
  // valuation instant: corrupted authority, and still fatal under the filter.
  const OpraPanel source = wide_panel();
  auto rows = wide_definitions_for_panel();
  rows[2].definition_ts_ns = source.frame.snapshot_ts_ns + 1; // row 2 == wide_key(2)
  auto table = ListedDefinitionTable::create(std::move(rows));
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());

  const std::vector<ListedQuoteKey> wanted = sorted_wanted({wide_key(2), wide_key(5)});
  // Anti-vacuity: the perturbed contract IS in the wanted set.
  ASSERT_TRUE(std::binary_search(wanted.begin(), wanted.end(), wide_key(2)));

  auto filtered = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, wanted);
  ASSERT_FALSE(filtered) << "look-ahead must stay fatal for a wanted key";
  EXPECT_EQ(filtered.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(filtered.error().message(), "listed OPRA join: definition look-ahead or expiry");

  // ...and it is fatal under SkipUnlisted too, exactly as it is unfiltered.
  EXPECT_FALSE(atx::vol::listed_quotes_from_opra(kDate, source.frame.snapshot_ts_ns, source, *table,
                                                 MissingDefinitionPolicy::SkipUnlisted, wanted));

  // Control: the SAME corrupted table is silently tolerated when that contract
  // is NOT wanted — this is the narrowing, asserted rather than assumed.
  const std::vector<ListedQuoteKey> elsewhere = sorted_wanted({wide_key(0), wide_key(5)});
  auto narrowed = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, elsewhere);
  ASSERT_TRUE(narrowed) << (narrowed ? std::string{} : narrowed.error().to_string());
  EXPECT_EQ(narrowed->size(), 2u);
}

TEST(ListedOpra, FilteredJoinStillFatalOnWantedEconomicsMismatch) {
  using atx::vol::MissingDefinitionPolicy;
  // NARROWED GATE 3. The panel row's expiry disagrees with its own OSI symbol.
  // raw_symbol, strike and side are untouched, so the row's wanted key is exactly
  // wide_key(2) — there is no ambiguity about whether the caller asked for it.
  OpraPanel source = wide_panel();
  source.frame.rows[2].expiry_iso = "2026-07-18"; // OSI says 260717
  auto table = ListedDefinitionTable::create(wide_definitions_for_panel());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());

  const std::vector<ListedQuoteKey> wanted = sorted_wanted({wide_key(2), wide_key(5)});
  ASSERT_TRUE(std::binary_search(wanted.begin(), wanted.end(), wide_key(2)));

  auto filtered = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, wanted);
  ASSERT_FALSE(filtered) << "economics disagreement must stay fatal for a wanted key";
  EXPECT_EQ(filtered.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(filtered.error().message(),
            "listed OPRA join: quote, OSI, and definition economics disagree");

  // A row that LIES about its strike must also still trip the gate rather than
  // be filtered out on the strike it is lying about — this is why the pre-lookup
  // stage keys on raw_symbol alone and not on (raw_symbol, strike, side).
  OpraPanel skewed_strike = wide_panel();
  skewed_strike.frame.rows[2].strike = 601.0; // OSI says 00600000
  auto by_strike = atx::vol::listed_quotes_from_opra(kDate, skewed_strike.frame.snapshot_ts_ns,
                                                     skewed_strike, *table,
                                                     MissingDefinitionPolicy::Error, wanted);
  ASSERT_FALSE(by_strike) << "a mis-struck wanted row must not be filtered past the gate";
  EXPECT_EQ(by_strike.error().message(),
            "listed OPRA join: quote, OSI, and definition economics disagree");
}

TEST(ListedOpra, FilteredJoinStillFatalOnWantedFutureQuote) {
  using atx::vol::MissingDefinitionPolicy;
  // NARROWED GATE 4. A WANTED row stamped after the valuation instant.
  OpraPanel source = wide_panel();
  source.frame.rows[2].ts_ns = source.frame.snapshot_ts_ns + 1;
  auto table = ListedDefinitionTable::create(wide_definitions_for_panel());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());

  const std::vector<ListedQuoteKey> wanted = sorted_wanted({wide_key(2), wide_key(5)});
  ASSERT_TRUE(std::binary_search(wanted.begin(), wanted.end(), wide_key(2)));

  auto filtered = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, wanted);
  ASSERT_FALSE(filtered) << "a future quote must stay fatal for a wanted key";
  EXPECT_EQ(filtered.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(filtered.error().message(), "listed OPRA join: future quote");

  // Control: the same future-stamped row is tolerated when it is not wanted.
  const std::vector<ListedQuoteKey> elsewhere = sorted_wanted({wide_key(0), wide_key(5)});
  auto narrowed = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, elsewhere);
  ASSERT_TRUE(narrowed) << (narrowed ? std::string{} : narrowed.error().to_string());
  EXPECT_EQ(narrowed->size(), 2u);
}

TEST(ListedOpra, FilteredJoinStillFatalOnMissingIdentity) {
  using atx::vol::MissingDefinitionPolicy;
  // PRESERVED PANEL-WIDE GATE 1. The identity lookup precedes the filter, so an
  // instrument id with no aligned source identity is fatal even though nothing
  // in `wanted` names that row. This is the partition boundary: the three gates
  // above narrow, this one does not.
  OpraPanel source = wide_panel();
  source.source_instrument_ids[5] = 999u; // no identity row for 999
  ASSERT_EQ(source.source_instrument_ids.size(), source.frame.rows.size());
  auto table = ListedDefinitionTable::create(wide_definitions_for_panel());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());

  // Wanted names rows 0 and 2 only — never the broken row 5.
  const std::vector<ListedQuoteKey> wanted = sorted_wanted({wide_key(0), wide_key(2)});
  ASSERT_FALSE(std::binary_search(wanted.begin(), wanted.end(), wide_key(5)));

  auto filtered = atx::vol::listed_quotes_from_opra(
      kDate, source.frame.snapshot_ts_ns, source, *table, MissingDefinitionPolicy::Error, wanted);
  ASSERT_FALSE(filtered) << "the panel-wide identity gate must not be narrowed";
  EXPECT_EQ(filtered.error().code(), atx::core::ErrorCode::NotFound);
  EXPECT_EQ(filtered.error().message(), "listed OPRA join: aligned source identity missing");

  // A zero instrument id on an unwanted row is equally fatal.
  OpraPanel zeroed = wide_panel();
  zeroed.source_instrument_ids[5] = 0u;
  EXPECT_FALSE(atx::vol::listed_quotes_from_opra(kDate, zeroed.frame.snapshot_ts_ns, zeroed, *table,
                                                 MissingDefinitionPolicy::Error, wanted));
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

// --- create()'s per-row end-of-day bound ---------------------------------
//
// create() admits a row only if its definition_ts_ns is at or before the LAST
// nanosecond of its own trade_date. The bound is derived per row from that row's
// date. These tests pin the bound's exact value and its per-date freshness, so a
// memoized or reformatted derivation has to reproduce the byte-for-byte same
// admit/reject decision as the plain per-row computation.

// The bound computed the long way: heap concatenation + iso_to_ns, exactly the
// expression create() used before any memoization.
std::int64_t reference_trade_end(const std::string &trade_date) {
  return iso_to_ns(trade_date + "T23:59:59.999999999Z");
}

ListedContractDefinition dated_definition(const std::string &trade_date, std::uint32_t id,
                                          const std::string &raw_symbol,
                                          std::int64_t definition_ts_ns) {
  ListedContractDefinition out;
  out.trade_date = trade_date;
  out.instrument_id = id;
  out.raw_symbol = raw_symbol;
  out.definition_ts_ns = definition_ts_ns;
  out.expiry_ts_ns = iso_to_ns("2026-09-18T20:00:00Z");
  out.multiplier = 100.0;
  out.standard_monthly = true;
  out.standard_deliverable = true;
  out.source_fingerprint = 4242;
  return out;
}

TEST(ListedOpra, DefinitionTableTradeEndMemoMatchesPerRowCompute) {
  const std::vector<std::string> dates = {"2026-06-03", "2026-06-04", "2026-06-05"};
  // The three bounds are DISTINCT and strictly increasing. Without this the test
  // could not tell a per-date bound from a stale one.
  ASSERT_LT(reference_trade_end(dates[0]), reference_trade_end(dates[1]));
  ASSERT_LT(reference_trade_end(dates[1]), reference_trade_end(dates[2]));

  // Three rows per date, each stamped AT its own date's exact bound — the
  // tightest admissible value. A bound that failed to refresh on a date change
  // would judge every row after the first date against a smaller bound and
  // reject it.
  std::vector<ListedContractDefinition> rows;
  for (const std::string &date : dates) {
    for (std::uint32_t k = 0; k < 3u; ++k) {
      rows.push_back(dated_definition(date, 100u + k,
                                      "SPY   260918C0060000" + std::to_string(k),
                                      reference_trade_end(date)));
    }
  }
  // Hand them in DESCENDING date order. create() sorts by (trade_date,
  // instrument_id, raw_symbol) before it walks the rows, and the sort is what
  // makes a SINGLE memo slot cheap — it is NOT what makes it correct. The memo
  // compares before it refreshes, so it is exact under any permutation; moving
  // the sort would cost refreshes, not correctness, and this test would still
  // pass. Reversing the input therefore proves the sort ran (asserted below),
  // not that the memo needs it.
  std::reverse(rows.begin(), rows.end());

  auto table = ListedDefinitionTable::create(rows);
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  ASSERT_EQ(table->definitions().size(), 9u);
  for (const ListedContractDefinition &kept : table->definitions()) {
    EXPECT_EQ(kept.definition_ts_ns, reference_trade_end(kept.trade_date)) << kept.trade_date;
  }
  // The sort really did run, and it really is date-major.
  EXPECT_EQ(table->definitions().front().trade_date, dates.front());
  EXPECT_EQ(table->definitions().back().trade_date, dates.back());

  // One nanosecond past its OWN date's bound is rejected — checked for every
  // date, so both a bound that never refreshes (breaks on date 2 and 3) and one
  // that refreshes to the wrong value are caught.
  for (const std::string &date : dates) {
    std::vector<ListedContractDefinition> late = rows;
    std::size_t bumped = 0u;
    for (ListedContractDefinition &row : late) {
      if (row.trade_date == date) {
        row.definition_ts_ns = reference_trade_end(date) + 1;
        ++bumped;
      }
    }
    ASSERT_EQ(bumped, 3u) << date; // anti-vacuity: the perturbation landed
    EXPECT_FALSE(ListedDefinitionTable::create(late)) << "date " << date;
  }
}

TEST(ListedOpra, DefinitionTableRejectsTradeDateTooLongForTheEndOfDayStamp) {
  // create() builds "<trade_date>T23:59:59.999999999Z" into a FIXED stack
  // buffer. Any trade_date too long to fit must still be rejected, exactly as
  // the old heap concatenation was: iso_to_ns refuses a stamp whose 11th
  // character is not 'T'/' ', so every over-long date produced 0 before and
  // must produce 0 now. This pins the buffer guard to the pre-existing
  // rejection branch rather than to a truncation or an overflow.
  const std::int64_t valid_ts = iso_to_ns("2026-06-05T12:00:00Z");
  ASSERT_GT(valid_ts, 0);

  std::vector<ListedContractDefinition> rows = {
      dated_definition("2026-06-05", 101u, "SPY   260918C00600000", valid_ts)};
  ASSERT_TRUE(ListedDefinitionTable::create(rows)); // anti-vacuity: the base admits

  for (const char *bad_date : {"2026-06-05X", "2026-06-0", "2026-06-05T00:00:00.000000000Z",
                               "2026-06-05-padded-well-past-thirty-two-bytes-wide"}) {
    rows[0].trade_date = bad_date;
    EXPECT_EQ(reference_trade_end(rows[0].trade_date), 0) << bad_date;
    EXPECT_FALSE(ListedDefinitionTable::create(rows)) << bad_date;
  }
}

TEST(ListedOpra, DefinitionFingerprintIsLazyStableAndSurvivesValueSemantics) {
  auto table = ListedDefinitionTable::create(definitions());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());

  // Copied BEFORE the fingerprint is ever demanded. A lazily computed
  // fingerprint must not depend on WHEN it was first asked for, and the class
  // must stay copyable and movable — a std::once_flag member would delete both
  // the copy and the move constructor, and every read path moves this table out
  // of a Result.
  ListedDefinitionTable copy_before = *table;

  const std::uint64_t first = table->fingerprint();
  EXPECT_NE(first, 0u);
  EXPECT_EQ(table->fingerprint(), first); // idempotent across repeated calls
  EXPECT_EQ(table->fingerprint(), first);
  EXPECT_EQ(copy_before.fingerprint(), first);

  ListedDefinitionTable copy_after = *table; // copied with the memo already warm
  EXPECT_EQ(copy_after.fingerprint(), first);
  const ListedDefinitionTable moved = std::move(copy_after);
  EXPECT_EQ(moved.fingerprint(), first); // const-callable

  // Still the serialize/parse round-trip invariant.
  auto parsed = atx::vol::parse_listed_definitions(atx::vol::serialize_listed_definitions(*table));
  ASSERT_TRUE(parsed) << (parsed ? std::string{} : parsed.error().to_string());
  EXPECT_EQ(parsed->fingerprint(), first);

  // Negative control: the fingerprint is content-derived, not a constant.
  auto other = definitions();
  other[0].source_fingerprint = 999;
  auto other_table = ListedDefinitionTable::create(std::move(other));
  ASSERT_TRUE(other_table) << (other_table ? std::string{} : other_table.error().to_string());
  EXPECT_NE(other_table->fingerprint(), first);

  // A default-constructed table IS the empty table, and reports the empty
  // table's fingerprint. Pinned deliberately: while the fingerprint was set
  // eagerly by create(), a default-constructed table reported 0 because nothing
  // had ever run. Lazy computation makes the two agree, which is the correct
  // reading of an empty definitions_ — but it is a behaviour change, so it is
  // asserted rather than left to chance.
  auto empty = ListedDefinitionTable::create({});
  ASSERT_TRUE(empty) << (empty ? std::string{} : empty.error().to_string());
  const ListedDefinitionTable default_constructed;
  EXPECT_NE(empty->fingerprint(), 0u);
  EXPECT_EQ(default_constructed.fingerprint(), empty->fingerprint());
  EXPECT_NE(default_constructed.fingerprint(), first);
}

// --- parse_listed_definitions negative surface ---------------------------
//
// The magic/header equality gate and the per-row field-count + numeric parses
// had no direct coverage at all. These are regression LOCKS on current
// behaviour, not REDs: they must pass against the parser as it stands today, so
// that a later rewrite of exactly those lines has something to fail against.

std::vector<std::string> split_on(const std::string &text, char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(delimiter, start);
    parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return parts;
}

std::string join_on(const std::vector<std::string> &parts, char delimiter) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out.push_back(delimiter);
    }
    out += parts[i];
  }
  return out;
}

// A known-good serialized table: magic, header, two rows, trailing '\n'.
std::string good_tsv() {
  auto table = ListedDefinitionTable::create(definitions());
  return table ? atx::vol::serialize_listed_definitions(*table) : std::string{};
}

std::string line_at(const std::string &tsv, std::size_t index) {
  const std::vector<std::string> lines = split_on(tsv, '\n');
  return index < lines.size() ? lines[index] : std::string{};
}

std::string with_line(const std::string &tsv, std::size_t index, const std::string &replacement) {
  std::vector<std::string> lines = split_on(tsv, '\n');
  EXPECT_LT(index, lines.size());
  if (index < lines.size()) {
    lines[index] = replacement;
  }
  return join_on(lines, '\n');
}

// Replace field `field` of data row `row` (0-based over data rows).
std::string with_field(const std::string &tsv, std::size_t row, std::size_t field,
                       const std::string &value) {
  std::vector<std::string> fields = split_on(line_at(tsv, row + 2u), '\t');
  EXPECT_EQ(fields.size(), 9u);
  if (field < fields.size()) {
    fields[field] = value;
  }
  return with_line(tsv, row + 2u, join_on(fields, '\t'));
}

bool parses(const std::string &tsv) { return atx::vol::parse_listed_definitions(tsv).has_value(); }

// WHICH GATE REJECTED `tsv`, not merely THAT something did.
//
// `parse_listed_definitions` re-wraps every create()-level Error as
// `ErrorCode::ParseError` (listed_opra.cpp, the `return Err(ErrorCode::ParseError,
// table.error().to_string())` at the tail), so the error CODE is `ParseError` for
// both layers and cannot discriminate. The wrapped MESSAGE can: the parser's own
// two rejections carry their message verbatim, whereas a create()-level rejection
// carries create()'s own `"<Code>: <message>"` rendering as the wrapped text.
//
// This matters because a later rewrite of the parser could drop its numeric and
// field-count validation entirely and still leave a bare `has_value()` assertion
// green — create()'s structural gate would catch most of these inputs downstream.
// Pinning the layer turns the whole set into a lock on the PARSER.
std::string rejection(const std::string &tsv) {
  auto table = atx::vol::parse_listed_definitions(tsv);
  if (table) {
    return "ACCEPTED";
  }
  EXPECT_EQ(table.error().code(), atx::core::ErrorCode::ParseError);
  return table.error().message();
}

// The parser's own two rejections (the magic/header equality gate and the per-row
// field-count + numeric gate).
constexpr const char *kByParserHeader = "listed definitions: bad header";
constexpr const char *kByParserRow = "listed definitions: malformed row";
// create()'s rejections, as the parser re-renders them.
constexpr const char *kByCreateMalformed =
    "InvalidArgument: listed definitions: malformed definition";
constexpr const char *kByCreateDuplicate =
    "AlreadyExists: listed definitions: duplicate definition key";

TEST(ListedOpra, ParseRejectsMagicAndHeaderCorruption) {
  const std::string good = good_tsv();
  ASSERT_FALSE(good.empty());
  // Anti-vacuity: the unmutated input really does parse, and to two rows.
  auto base = atx::vol::parse_listed_definitions(good);
  ASSERT_TRUE(base) << (base ? std::string{} : base.error().to_string());
  ASSERT_EQ(base->definitions().size(), 2u);

  const std::string magic = line_at(good, 0);
  const std::string header = line_at(good, 1);
  ASSERT_EQ(magic, "ATX_LISTED_DEFINITIONS\t1");
  ASSERT_FALSE(header.empty());

  // Every case below must be rejected by the PARSER's magic/header equality gate
  // specifically (kByParserHeader), never by create() downstream — see
  // `rejection` above for why the error code alone cannot say that.

  // Too few lines to carry a magic + header at all.
  EXPECT_EQ(rejection(""), kByParserHeader);
  EXPECT_EQ(rejection(magic), kByParserHeader);
  EXPECT_EQ(rejection(magic + "\n"), kByParserHeader);

  // Magic: version, separator, case, padding, truncation.
  EXPECT_EQ(rejection(with_line(good, 0, "ATX_LISTED_DEFINITIONS\t2")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 0, "ATX_LISTED_DEFINITIONS 1")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 0, "atx_listed_definitions\t1")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 0, "ATX_LISTED_DEFINITIONS\t1 ")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 0, " ATX_LISTED_DEFINITIONS\t1")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 0, "ATX_LISTED_DEFINITIONS")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 0, "")), kByParserHeader);
  // A '\r' before the '\n' is NOT tolerated on the magic line — the equality is
  // exact, and this file format is LF-only by contract.
  EXPECT_EQ(rejection(with_line(good, 0, magic + "\r")), kByParserHeader);

  // Header: an extra column, a dropped column, a renamed column, a reordering.
  EXPECT_EQ(rejection(with_line(good, 1, header + "\textra")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 1, header.substr(0, header.rfind('\t')))), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 1, "TRADE_DATE" + header.substr(header.find('\t')))),
            kByParserHeader);
  {
    std::vector<std::string> columns = split_on(header, '\t');
    ASSERT_EQ(columns.size(), 9u);
    std::swap(columns[0], columns[1]);
    EXPECT_EQ(rejection(with_line(good, 1, join_on(columns, '\t'))), kByParserHeader);
  }
  EXPECT_EQ(rejection(with_line(good, 1, "")), kByParserHeader);
  EXPECT_EQ(rejection(with_line(good, 1, header + "\r")), kByParserHeader);
  // Magic and header transposed.
  EXPECT_EQ(rejection(with_line(with_line(good, 0, header), 1, magic)), kByParserHeader);
}

TEST(ListedOpra, ParseRejectsMalformedRowFieldCountsAndNumerics) {
  const std::string good = good_tsv();
  ASSERT_FALSE(good.empty());
  ASSERT_TRUE(parses(good)); // anti-vacuity

  const std::string row = line_at(good, 2);
  const std::vector<std::string> fields = split_on(row, '\t');
  ASSERT_EQ(fields.size(), 9u);

  // Every assertion below pins the LAYER that rejected, not merely that something
  // did — see `rejection` above. Cases marked kByParserRow are locks on the
  // parser's own gate and would go RED if a rewrite dropped that validation and
  // let create() catch the input instead.

  // Field count: 8 and 10. Parser gate.
  EXPECT_EQ(rejection(with_line(good, 2,
                                join_on(std::vector<std::string>(fields.begin(), fields.end() - 1),
                                        '\t'))),
            kByParserRow);
  EXPECT_EQ(rejection(with_line(good, 2, row + "\textra")), kByParserRow);
  // A tab-free row is one field, not nine.
  EXPECT_EQ(rejection(with_line(good, 2, "not-a-row")), kByParserRow);

  // instrument_id (field 1): non-numeric, empty, trailing garbage, negative,
  // and one past the uint32 ceiling the parser enforces. All parser gate.
  for (const char *bad : {"abc", "", " 101", "101 ", "101x", "-1", "1.0", "0x65", "+101",
                          "4294967296", "18446744073709551616"}) {
    EXPECT_EQ(rejection(with_field(good, 0, 1, bad)), kByParserRow) << "instrument_id=" << bad;
  }
  EXPECT_TRUE(parses(with_field(good, 0, 1, "4294967295"))) << "uint32 ceiling must still parse";

  // definition_ts_ns (3) and expiry_ts_ns (4): int64, no fractional part. Parser gate.
  for (const char *bad : {"abc", "", "1.5", "1e9", "12345678901234567890123"}) {
    EXPECT_EQ(rejection(with_field(good, 0, 3, bad)), kByParserRow) << "definition_ts_ns=" << bad;
    EXPECT_EQ(rejection(with_field(good, 0, 4, bad)), kByParserRow) << "expiry_ts_ns=" << bad;
  }

  // multiplier (5): the parser rejects non-numeric / trailing-garbage / non-finite
  // forms (parse_double demands full consumption and isfinite)...
  for (const char *bad : {"abc", "", "100x", "inf", "-inf", "nan"}) {
    EXPECT_EQ(rejection(with_field(good, 0, 5, bad)), kByParserRow) << "multiplier=" << bad;
  }
  // ...but "0" and "-100" are VALID doubles, so they pass the parser and are
  // rejected one layer down by create()'s positivity gate. Pinning that split is
  // the point: it says exactly which code owns each rule.
  for (const char *bad : {"0", "-100"}) {
    EXPECT_EQ(rejection(with_field(good, 0, 5, bad)), kByCreateMalformed) << "multiplier=" << bad;
  }

  // standard_monthly (6) / standard_deliverable (7): strictly 0 or 1. Parser gate.
  for (const char *bad : {"2", "-1", "x", "", "01x"}) {
    EXPECT_EQ(rejection(with_field(good, 0, 6, bad)), kByParserRow) << "standard_monthly=" << bad;
    EXPECT_EQ(rejection(with_field(good, 0, 7, bad)), kByParserRow)
        << "standard_deliverable=" << bad;
  }

  // source_fingerprint (8): non-numeric, empty, negative and overflowing forms are
  // the parser's; a well-formed "0" is create()'s non-zero gate.
  for (const char *bad : {"abc", "", "-1", "1.0", "18446744073709551616"}) {
    EXPECT_EQ(rejection(with_field(good, 0, 8, bad)), kByParserRow) << "source_fingerprint=" << bad;
  }
  EXPECT_EQ(rejection(with_field(good, 0, 8, "0")), kByCreateMalformed) << "source_fingerprint=0";

  // Fields the parser does not validate at all reach create()'s structural gates.
  EXPECT_EQ(rejection(with_field(good, 0, 0, "")), kByCreateMalformed) << "empty trade_date";
  EXPECT_EQ(rejection(with_field(good, 0, 2, "")), kByCreateMalformed) << "empty raw_symbol";
  EXPECT_EQ(rejection(with_field(good, 0, 1, "0")), kByCreateMalformed) << "instrument_id 0";
  // Duplicate key: row 1 made identical to row 0. create()'s AlreadyExists gate —
  // a DIFFERENT create() gate from the malformed one, and the message pins which.
  EXPECT_EQ(rejection(with_line(good, 3, row)), kByCreateDuplicate) << "duplicate definition key";
}

TEST(ListedOpra, ParseAcceptsEmptyLinesExactlyAsItDoesToday) {
  const std::string good = good_tsv();
  ASSERT_FALSE(good.empty());
  ASSERT_EQ(good.back(), '\n');

  // The shipped serialization ends with '\n', so split() emits a trailing empty
  // element that the empty-line skip absorbs. This is load-bearing: the writer's
  // own output must round-trip.
  auto trailing = atx::vol::parse_listed_definitions(good);
  ASSERT_TRUE(trailing) << (trailing ? std::string{} : trailing.error().to_string());
  EXPECT_EQ(trailing->definitions().size(), 2u);

  // No trailing newline at all: also accepted, same two rows.
  auto unterminated = atx::vol::parse_listed_definitions(good.substr(0, good.size() - 1));
  ASSERT_TRUE(unterminated) << (unterminated ? std::string{} : unterminated.error().to_string());
  EXPECT_EQ(unterminated->definitions().size(), 2u);

  // Interior and repeated blank lines are skipped, not treated as short rows.
  auto interior = atx::vol::parse_listed_definitions(good + "\n\n\n");
  ASSERT_TRUE(interior) << (interior ? std::string{} : interior.error().to_string());
  EXPECT_EQ(interior->definitions().size(), 2u);

  std::vector<std::string> lines = split_on(good, '\n');
  ASSERT_EQ(lines.size(), 5u); // magic, header, row, row, trailing empty
  lines.insert(lines.begin() + 3, std::string{});
  auto between = atx::vol::parse_listed_definitions(join_on(lines, '\n'));
  ASSERT_TRUE(between) << (between ? std::string{} : between.error().to_string());
  EXPECT_EQ(between->definitions().size(), 2u);

  // Negative control for this whole test: a blank line is skipped, but a line
  // that merely LOOKS blank (a single space) is a one-field row and is rejected
  // by the PARSER's field-count gate — not swallowed by the skip and not deferred
  // to create(). Pinning the layer is what makes this a control on the skip.
  EXPECT_EQ(rejection(with_line(good, 2, " ")), kByParserRow);
}

// --- the single-pass field-boundary scan ---------------------------------
//
// The per-row `std::vector<std::string_view>` from `split` was replaced by a
// forward scan into a stack `std::string_view fields[9]`. Two properties that
// `split(...).size() != 9` gave for free now have to be re-established by hand,
// and each is RED-provable against the naive scan the rewrite invites:
//
//   (1) NO NINTH TAB. A scan that locates the first eight tabs and lets field 8
//       stop at the NEXT tab silently DROPS everything past it: the row then
//       parses exactly as if it had nine fields. Task 4's single 10-field case
//       (`row + "\textra"`) does fire against that scan — it was observed
//       ACCEPTED — but a row whose ninth tab is TRAILING is the sharper
//       discriminator, because then fields 0-8 are byte-identical to a row that
//       parses and no numeric gate can fire for an unrelated reason. The
//       interior cases below additionally pin that the assertion is on the tab
//       COUNT and not on the tail content.
//   (2) NO STALE FIELD. A `fields[9]` array hoisted out of the row loop — the
//       obvious way to write it — is only PARTIALLY overwritten by a short row,
//       so the missing trailing fields keep the PREVIOUS row's values and an
//       eight-field row parses using its predecessor's tail. This is invisible
//       on the first data row (the array is still empty there), which is why
//       these cases mutate the SECOND one.
//
// Both were observed RED against exactly that naive scan before the correct
// scan was written; the failure output is in the task report.

TEST(ListedOpra, ParseRejectsRowsWithMoreThanNineFields) {
  const std::string good = good_tsv();
  ASSERT_FALSE(good.empty());
  const std::string row0 = line_at(good, 2);
  const std::string row1 = line_at(good, 3);
  const std::vector<std::string> parts1 = split_on(row1, '\t');
  ASSERT_EQ(split_on(row0, '\t').size(), 9u);
  ASSERT_EQ(parts1.size(), 9u);
  // Anti-vacuity: unmutated, both data rows really do parse.
  auto base = atx::vol::parse_listed_definitions(good);
  ASSERT_TRUE(base) << (base ? std::string{} : base.error().to_string());
  ASSERT_EQ(base->definitions().size(), 2u);

  // A ninth tab with NOTHING after it. Every field, including the complete
  // source_fingerprint in field 8, is byte-identical to the accepted row, so no
  // numeric gate can fire — only the tab count distinguishes this input.
  EXPECT_EQ(rejection(with_line(good, 2, row0 + "\t")), kByParserRow) << "trailing tab on row 0";
  EXPECT_EQ(rejection(with_line(good, 3, row1 + "\t")), kByParserRow) << "trailing tab on row 1";

  // A ninth tab with content after it, and an eleventh field.
  EXPECT_EQ(rejection(with_line(good, 3, row1 + "\textra")), kByParserRow) << "ten fields";
  EXPECT_EQ(rejection(with_line(good, 3, row1 + "\t7\t8")), kByParserRow) << "eleven fields";

  // An extra EMPTY field inserted at each position in turn: ten fields, with the
  // ninth tab in the interior rather than at the tail.
  for (std::size_t at = 0; at <= parts1.size(); ++at) {
    std::vector<std::string> widened = parts1;
    widened.insert(widened.begin() + static_cast<std::ptrdiff_t>(at), std::string{});
    ASSERT_EQ(widened.size(), 10u) << at;
    EXPECT_EQ(rejection(with_line(good, 3, join_on(widened, '\t'))), kByParserRow)
        << "empty field inserted at " << at;
  }
}

TEST(ListedOpra, ParseRejectsRowsWithFewerThanNineFields) {
  const std::string good = good_tsv();
  ASSERT_FALSE(good.empty());
  const std::string row0 = line_at(good, 2);
  const std::string row1 = line_at(good, 3);
  const std::vector<std::string> parts0 = split_on(row0, '\t');
  const std::vector<std::string> parts1 = split_on(row1, '\t');
  ASSERT_EQ(parts0.size(), 9u);
  ASSERT_EQ(parts1.size(), 9u);
  // Anti-vacuity for the stale-field cases: the two rows DIFFER in the columns a
  // leak would supply, so a leaked value is observable rather than accidentally
  // right. (They share trade_date, expiry and multiplier by construction.)
  ASSERT_NE(parts0[1], parts1[1]); // instrument_id
  ASSERT_NE(parts0[2], parts1[2]); // raw_symbol
  ASSERT_NE(parts0[8], parts1[8]); // source_fingerprint
  auto base = atx::vol::parse_listed_definitions(good);
  ASSERT_TRUE(base) << (base ? std::string{} : base.error().to_string());
  ASSERT_EQ(base->definitions().size(), 2u);

  // Drop each field in turn from the SECOND data row — the row a hoisted
  // `fields[9]` would fill from its predecessor.
  for (std::size_t drop = 0; drop < parts1.size(); ++drop) {
    std::vector<std::string> shortened = parts1;
    shortened.erase(shortened.begin() + static_cast<std::ptrdiff_t>(drop));
    ASSERT_EQ(shortened.size(), 8u) << drop;
    EXPECT_EQ(rejection(with_line(good, 3, join_on(shortened, '\t'))), kByParserRow)
        << "field " << drop << " dropped from row 1";
  }
  // ...and from the FIRST, where there is no predecessor to leak from.
  for (std::size_t drop = 0; drop < parts0.size(); ++drop) {
    std::vector<std::string> shortened = parts0;
    shortened.erase(shortened.begin() + static_cast<std::ptrdiff_t>(drop));
    ASSERT_EQ(shortened.size(), 8u) << drop;
    EXPECT_EQ(rejection(with_line(good, 2, join_on(shortened, '\t'))), kByParserRow)
        << "field " << drop << " dropped from row 0";
  }

  // Every separator count from 0 to 7. `keep == 1` is a tab-free row; the all-
  // empty case has the right number of separators for eight fields and none of
  // the content.
  for (std::size_t keep = 1; keep < parts1.size(); ++keep) {
    const std::vector<std::string> prefix(parts1.begin(),
                                          parts1.begin() + static_cast<std::ptrdiff_t>(keep));
    EXPECT_EQ(rejection(with_line(good, 3, join_on(prefix, '\t'))), kByParserRow)
        << "first " << keep << " fields only";
  }
  EXPECT_EQ(rejection(with_line(good, 3, "\t\t\t\t\t\t\t")), kByParserRow) << "eight empty fields";
}

// A wide, multi-date fixture. Field widths vary in every column (1-to-10-digit
// instrument ids, 1-to-20-digit fingerprints, four raw_symbol roots, five
// multiplier spellings), all four flag combinations occur, and the rows span
// three trade dates so the row loop crosses date boundaries. 60 rows.
std::vector<ListedContractDefinition> wide_definitions() {
  const std::vector<std::string> dates = {"2026-06-01", "2026-06-02", "2026-06-03"};
  const std::vector<std::uint32_t> ids = {1u, 42u, 65535u, 1000000u, 4294967295u};
  const std::vector<double> multipliers = {1.0, 10.0, 100.0, 0.5, 1234.5678};
  const std::vector<std::uint64_t> prints = {1u, 4242u, 18446744073709551615ull};
  const std::vector<std::string> roots = {"A", "SPY", "BRKB", "NDXP"};
  std::vector<ListedContractDefinition> rows;
  std::size_t k = 0;
  for (const std::string &date : dates) {
    for (const std::uint32_t id : ids) {
      for (const std::string &root : roots) {
        ListedContractDefinition row;
        row.trade_date = date;
        row.instrument_id = id;
        row.raw_symbol =
            root + std::string(6u - root.size(), ' ') + "260918C0060000" + std::to_string(k % 10u);
        row.definition_ts_ns = iso_to_ns(date + "T09:30:00Z") + static_cast<std::int64_t>(k);
        row.expiry_ts_ns = iso_to_ns("2026-09-18T20:00:00Z");
        row.multiplier = multipliers[k % multipliers.size()];
        row.standard_monthly = (k % 2u) == 0u;
        row.standard_deliverable = (k % 3u) == 0u;
        row.source_fingerprint = prints[k % prints.size()];
        rows.push_back(row);
        ++k;
      }
    }
  }
  return rows;
}

TEST(ListedOpra, ParseIsByteIdenticalToPriorImplementation) {
  // The round-trip invariant is the PRIMARY correctness oracle for the scan
  // rewrite: it asserts every one of the nine field boundaries on every row, not
  // just the ones a hand-written negative case happens to name.
  auto table = ListedDefinitionTable::create(wide_definitions());
  ASSERT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  ASSERT_EQ(table->definitions().size(), 60u);
  const std::string text = atx::vol::serialize_listed_definitions(*table);
  ASSERT_FALSE(text.empty());

  // Anti-vacuity: the fixture really is ragged. If every row were the same width
  // an off-by-one in the scan could survive.
  {
    const std::vector<std::string> lines = split_on(text, '\n');
    ASSERT_EQ(lines.size(), 63u); // magic, header, 60 rows, trailing empty
    std::vector<std::size_t> widths;
    for (std::size_t i = 2; i + 1 < lines.size(); ++i) {
      ASSERT_EQ(split_on(lines[i], '\t').size(), 9u) << i;
      widths.push_back(lines[i].size());
    }
    std::sort(widths.begin(), widths.end());
    widths.erase(std::unique(widths.begin(), widths.end()), widths.end());
    ASSERT_GE(widths.size(), 6u) << "fixture rows are not ragged enough to pin boundaries";
  }

  auto parsed = atx::vol::parse_listed_definitions(text);
  ASSERT_TRUE(parsed) << (parsed ? std::string{} : parsed.error().to_string());
  EXPECT_EQ(parsed->definitions().size(), 60u);
  EXPECT_TRUE(std::ranges::equal(parsed->definitions(), table->definitions()));
  EXPECT_EQ(atx::vol::serialize_listed_definitions(*parsed), text);
  EXPECT_EQ(parsed->fingerprint(), table->fingerprint());

  // Unterminated input: identical rows, so the final row's field 8 is bounded by
  // end-of-input rather than by a '\n'.
  auto unterminated = atx::vol::parse_listed_definitions(text.substr(0, text.size() - 1));
  ASSERT_TRUE(unterminated) << (unterminated ? std::string{} : unterminated.error().to_string());
  EXPECT_TRUE(std::ranges::equal(unterminated->definitions(), table->definitions()));

  // Negative control: the comparison above can say False. One byte of one field
  // is changed and the re-serialized text must differ from the original.
  const std::size_t last_tab = text.rfind('\t');
  ASSERT_NE(last_tab, std::string::npos);
  std::string mutated = text;
  mutated[last_tab + 1u] = (mutated[last_tab + 1u] == '9') ? '8' : '9';
  ASSERT_NE(mutated, text); // the perturbation landed
  auto reparsed = atx::vol::parse_listed_definitions(mutated);
  ASSERT_TRUE(reparsed) << (reparsed ? std::string{} : reparsed.error().to_string());
  EXPECT_NE(atx::vol::serialize_listed_definitions(*reparsed), text);
  EXPECT_NE(reparsed->fingerprint(), table->fingerprint());
}

} // namespace
