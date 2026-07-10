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

TEST(ListedOpra, DefinitionTableRejectsDuplicateAndFutureDateScopedRows) {
  auto duplicate = definitions();
  duplicate.push_back(duplicate.front());
  EXPECT_FALSE(ListedDefinitionTable::create(std::move(duplicate)));

  auto future = definitions();
  future[0].definition_ts_ns = iso_to_ns("2026-06-06T00:00:00Z");
  EXPECT_FALSE(ListedDefinitionTable::create(std::move(future)));
}

} // namespace
