// Gate for the `atx-vol-chain-export` schema/row-assembly seam
// (tools/chain_export.hpp): the vendor column contract, the pinned greek
// convention scales, the cash-settled-index spot fallback, and the sentinel
// census. The CLI driver (tools/chain_export_main.cpp) owns only argv parsing,
// I/O and the fit loop; every decision that can silently corrupt a published
// row lives here and is pinned below.

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "chain_export.hpp"

namespace {

using atx::vol::AmericanGreeks;
using atx::vol::Side;
namespace ce = atx::vol::chainexport;

// A greek bundle whose members are all distinct primes-ish so a swapped field
// cannot pass by coincidence.
[[nodiscard]] AmericanGreeks distinct_greeks() noexcept {
  AmericanGreeks g;
  g.delta = 0.5;
  g.gamma = 0.011;
  g.vega = 30.0;
  g.theta = -12.6;
  g.rho = 44.0;
  g.vanna = 7.0;
  g.volga = 900.0;
  g.charm = -5.04;
  g.price = 3.25;
  return g;
}

// ── The pinned convention map ───────────────────────────────────────────────

TEST(ChainExport, ScaleGreeksAppliesThePinnedProductionScales) {
  const AmericanGreeks g = distinct_greeks();
  const ce::VendorGreeks out = ce::scale_greeks(g, /*dp_dq=*/-800.0, ce::kProductionGreekScales);

  EXPECT_DOUBLE_EQ(out.de, 0.5);                 // delta_scale = 1
  EXPECT_DOUBLE_EQ(out.ga, 0.011);               // gamma_scale = 1
  EXPECT_DOUBLE_EQ(out.th, -12.6 / 252.0);       // theta_scale = 1/252
  EXPECT_DOUBLE_EQ(out.ve, 30.0 * 0.01);         // vega_scale  = 0.01
  EXPECT_DOUBLE_EQ(out.rh, 44.0 * 0.01);         // rho_scale   = 0.01
  EXPECT_DOUBLE_EQ(out.ph, -800.0 * 0.01);       // phi_scale   = 0.01
  EXPECT_DOUBLE_EQ(out.vo, 900.0 * 1.0e-4);      // volga_scale = 1e-4
  EXPECT_DOUBLE_EQ(out.va, 7.0 * 0.01);          // vanna_scale = 0.01
  EXPECT_DOUBLE_EQ(out.de_decay, -5.04 / 252.0); // delta_decay_scale = 1/252
}

TEST(ChainExport, ProductionScalesEqualTheOracleCanonicalWinner) {
  // Pinned from `git show oracle/canonical:atx-vol/tools/oracle_conventions.cpp`
  // (`kWinner`). Restated as literals here deliberately: this suite must fail if
  // the scales drift, and the winner map is not linked into this binary.
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.delta, 1.0);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.gamma, 1.0);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.theta, 1.0 / 252.0);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.vega, 0.01);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.rho, 0.01);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.phi, 0.01);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.volga, 1.0e-4);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.vanna, 0.01);
  EXPECT_DOUBLE_EQ(ce::kProductionGreekScales.delta_decay, 1.0 / 252.0);
}

TEST(ChainExport, ScaleGreeksPropagatesANonFiniteCarryRhoAsTheMissingSentinel) {
  const AmericanGreeks g = distinct_greeks();
  const ce::VendorGreeks out =
      ce::scale_greeks(g, std::numeric_limits<double>::quiet_NaN(), ce::kProductionGreekScales);

  // A failed carry solve must NOT publish a scaled NaN dressed as a number.
  EXPECT_DOUBLE_EQ(out.ph, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.de, 0.5); // the other eight are unaffected
}

TEST(ChainExport, ScaleGreeksMapsANonFiniteGreekToTheMissingSentinel) {
  AmericanGreeks g = distinct_greeks();
  g.volga = std::numeric_limits<double>::infinity();
  const ce::VendorGreeks out = ce::scale_greeks(g, /*dp_dq=*/-800.0, ce::kProductionGreekScales);

  EXPECT_DOUBLE_EQ(out.vo, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.va, 7.0 * 0.01);
}

TEST(ChainExport, GreeksAreDefinedOnlyAtAUsableModelIv) {
  EXPECT_TRUE(ce::greeks_are_defined(0.1834));
  EXPECT_TRUE(ce::greeks_are_defined(2.48)); // a wide but real 0DTE wing vol

  // No IV means no point to differentiate at — an all-zero engine bundle must
  // not be published as if it were flat.
  EXPECT_FALSE(ce::greeks_are_defined(ce::kMissingF64));
  EXPECT_FALSE(ce::greeks_are_defined(0.0));
  EXPECT_FALSE(ce::greeks_are_defined(-0.2));
  EXPECT_FALSE(ce::greeks_are_defined(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(ce::greeks_are_defined(std::numeric_limits<double>::infinity()));
}

// ── The cash-settled index complex ──────────────────────────────────────────

TEST(ChainExport, CashSettledIndexRootsAreRecognized) {
  for (const std::string_view root : {"SPX", "SPXW", "XSP", "NDX", "NDXP", "RUT", "RUTW", "XEO",
                                      "VIX", "DJX", "MRUT", "XND", "MXEA", "MXEF"}) {
    EXPECT_TRUE(ce::is_cash_settled_index_root(root)) << root;
  }
}

TEST(ChainExport, EquityAndEtfRootsAreNotTheIndexComplex) {
  for (const std::string_view root : {"SPY", "QQQ", "IWM", "AAPL", "TSLA", "SPXL", "", "SP"}) {
    EXPECT_FALSE(ce::is_cash_settled_index_root(root)) << root;
  }
}

TEST(ChainExport, IndexComplexTakesTheParityForwardSpotAndSentinelNbbo) {
  // SPX has no row in any equity feed; a feed row offered for it must be
  // IGNORED rather than published as an index level.
  const ce::NbboQuote wrong{.bid = 1.0, .ask = 2.0};
  const ce::UnderlierFields out = ce::resolve_underlier("SPX", &wrong, /*parity_spot=*/6543.21);

  EXPECT_EQ(out.source, ce::SpotSource::ParityForward);
  EXPECT_DOUBLE_EQ(out.u_bid, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.u_ask, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.u_prc, 6543.21);
}

TEST(ChainExport, IndexComplexWithoutAParitySpotLeavesEveryUnderlierFieldSentinel) {
  const ce::UnderlierFields out = ce::resolve_underlier("VIX", nullptr, /*parity_spot=*/0.0);

  EXPECT_EQ(out.source, ce::SpotSource::Unavailable);
  EXPECT_DOUBLE_EQ(out.u_bid, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.u_ask, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.u_prc, ce::kMissingF64);
}

TEST(ChainExport, EquityWithAFeedRowTakesTheNbboAndItsMid) {
  const ce::NbboQuote q{.bid = 660.10, .ask = 660.30};
  const ce::UnderlierFields out = ce::resolve_underlier("SPY", &q, /*parity_spot=*/659.0);

  EXPECT_EQ(out.source, ce::SpotSource::Nbbo);
  EXPECT_DOUBLE_EQ(out.u_bid, 660.10);
  EXPECT_DOUBLE_EQ(out.u_ask, 660.30);
  EXPECT_DOUBLE_EQ(out.u_prc, 0.5 * (660.10 + 660.30));
}

TEST(ChainExport, EquityWithoutAFeedRowFallsBackToTheParityForward) {
  const ce::UnderlierFields out = ce::resolve_underlier("AAPL", nullptr, /*parity_spot=*/231.5);

  EXPECT_EQ(out.source, ce::SpotSource::ParityForward);
  EXPECT_DOUBLE_EQ(out.u_bid, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.u_ask, ce::kMissingF64);
  EXPECT_DOUBLE_EQ(out.u_prc, 231.5);
}

TEST(ChainExport, AnUnusableFeedQuoteFallsBackRatherThanPublishingIt) {
  const ce::NbboQuote crossed{.bid = 12.0, .ask = 11.0};
  const ce::UnderlierFields a = ce::resolve_underlier("AAPL", &crossed, /*parity_spot=*/11.5);
  EXPECT_EQ(a.source, ce::SpotSource::ParityForward);
  EXPECT_DOUBLE_EQ(a.u_prc, 11.5);

  const ce::NbboQuote unset{.bid = 0.0, .ask = 11.0};
  const ce::UnderlierFields b = ce::resolve_underlier("AAPL", &unset, /*parity_spot=*/11.5);
  EXPECT_EQ(b.source, ce::SpotSource::ParityForward);

  const ce::NbboQuote nan_side{.bid = 1.0, .ask = std::numeric_limits<double>::quiet_NaN()};
  const ce::UnderlierFields c = ce::resolve_underlier("AAPL", &nan_side, /*parity_spot=*/0.0);
  EXPECT_EQ(c.source, ce::SpotSource::Unavailable);
  EXPECT_DOUBLE_EQ(c.u_prc, ce::kMissingF64);
}

// ── Stamps and keys ─────────────────────────────────────────────────────────

TEST(ChainExport, ExpiryYmdDecodesTheSettlementInstantsCalendarDate) {
  // 2026-06-19 16:00 ET (EDT) == 2026-06-19T20:00:00Z.
  const std::int64_t days = atx::core::time::days_from_civil(2026, 6, 19);
  const std::int64_t ns = days * 86'400'000'000'000LL + 20LL * 3'600'000'000'000LL;

  const ce::ExpiryYmd ymd = ce::expiry_ymd_from_ns(ns);
  EXPECT_EQ(ymd.year, 2026);
  EXPECT_EQ(ymd.month, 6);
  EXPECT_EQ(ymd.day, 19);
}

TEST(ChainExport, ExpiryYmdRefusesANonPositiveInstant) {
  const ce::ExpiryYmd ymd = ce::expiry_ymd_from_ns(0);
  EXPECT_EQ(ymd.year, ce::kMissingI64);
  EXPECT_EQ(ymd.month, ce::kMissingI64);
  EXPECT_EQ(ymd.day, ce::kMissingI64);
}

TEST(ChainExport, VendorStampJoinsTheSessionDateAndTheSnapshotSuffix) {
  EXPECT_EQ(ce::vendor_stamp("2026-08-21", "T19:55:00Z"), "2026-08-21 19:55:00.000000");
  EXPECT_EQ(ce::vendor_stamp("2026-12-01", "T20:55:00Z"), "2026-12-01 20:55:00.000000");
}

TEST(ChainExport, VendorStampRefusesMalformedInput) {
  EXPECT_TRUE(ce::vendor_stamp("2026-8-21", "T19:55:00Z").empty());
  EXPECT_TRUE(ce::vendor_stamp("2026-08-21", "19:55:00Z").empty());
  EXPECT_TRUE(ce::vendor_stamp("", "").empty());
}

TEST(ChainExport, SideLabelUsesTheVendorSpelling) {
  // The live drop spells okey_cp "Call"/"Put", NOT "C"/"P" — verified against
  // C:/atx-cache/oracle/spiderrock/date=2026-08-14/bucket_et=0940.
  EXPECT_EQ(ce::side_label(Side::Call), "Call");
  EXPECT_EQ(ce::side_label(Side::Put), "Put");
}

// ── Universe flags ──────────────────────────────────────────────────────────

TEST(ChainExport, SymbolCsvTrimsUpperCasesAndPreservesOrder) {
  const std::vector<std::string> got = ce::parse_symbol_csv(" spy , AAPL,brk.b ,, TSLA ");
  ASSERT_EQ(got.size(), 4u);
  EXPECT_EQ(got[0], "SPY");
  EXPECT_EQ(got[1], "AAPL");
  EXPECT_EQ(got[2], "BRK.B"); // the dot survives upper-casing
  EXPECT_EQ(got[3], "TSLA");
}

TEST(ChainExport, SymbolCsvYieldsNothingForABlankValue) {
  EXPECT_TRUE(ce::parse_symbol_csv("").empty());
  EXPECT_TRUE(ce::parse_symbol_csv(" , ,\t").empty());
}

TEST(ChainExport, SymbolFileSkipsCommentsAndBlanksAndKeepsFileOrder) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "chain_export_symbols.txt";
  {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << "# a comment\n\n  spy\r\nAAPL\n   \n\t# indented comment\nBRK.B  \n";
  }

  std::vector<std::string> got;
  ASSERT_TRUE(ce::read_symbol_file(path.string(), got));
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], "SPY");
  EXPECT_EQ(got[1], "AAPL");
  EXPECT_EQ(got[2], "BRK.B");
}

TEST(ChainExport, SymbolFileReportsAnUnreadablePath) {
  std::vector<std::string> got{"UNTOUCHED"};
  EXPECT_FALSE(ce::read_symbol_file(
      (std::filesystem::temp_directory_path() / "no_such_symbols_file.txt").string(), got));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], "UNTOUCHED"); // strong guarantee: untouched on failure
}

// ── The emitted table ───────────────────────────────────────────────────────

[[nodiscard]] ce::ExportRow fully_populated_row() {
  ce::ExportRow row;
  row.okey_tk = "SPY";
  row.okey_yr = 2026;
  row.okey_mn = 9;
  row.okey_dy = 18;
  row.okey_xx = 660.0;
  row.okey_cp = "Call";
  row.und_sec_key_tk = "SPY";
  row.u_bid = 660.10;
  row.u_ask = 660.30;
  row.u_prc = 660.20;
  row.bid_prc = 12.10;
  row.ask_prc = 12.20;
  row.bid_sz = 40;
  row.ask_sz = 55;
  row.sr_prc = 12.15;
  row.sr_vol = 0.1834;
  row.greeks = ce::VendorGreeks{.de = 0.52,
                                .ga = 0.012,
                                .th = -0.05,
                                .ve = 0.31,
                                .rh = 0.22,
                                .ph = -0.23,
                                .vo = 0.09,
                                .va = 0.004,
                                .de_decay = -0.001};
  row.rate = 0.0430;
  row.sdiv = 0.0125;
  row.ddiv = 0.0;
  row.years = 0.0767;
  row.date = "2026-08-21 19:55:00.000000";
  row.timestamp = "2026-08-21 19:55:00.000000";
  row.trading_date = "2026-08-21";
  row.error = 0.0;
  return row;
}

TEST(ChainExport, WriteColumnsCarryTheVendorNamesInSchemaOrder) {
  ce::ExportColumns table;
  table.append(fully_populated_row());

  const std::vector<atx::core::io::WriteColumn> cols = table.write_columns();
  ASSERT_EQ(cols.size(), ce::kColumnCount);
  for (std::size_t i = 0; i < cols.size(); ++i) {
    EXPECT_EQ(cols[i].name, ce::kColumnNames[i]) << "column " << i;
  }
}

TEST(ChainExport, WriteColumnsUseTheVendorPhysicalTypes) {
  ce::ExportColumns table;
  table.append(fully_populated_row());
  const std::vector<atx::core::io::WriteColumn> cols = table.write_columns();
  ASSERT_EQ(cols.size(), ce::kColumnCount);

  const auto is_i64 = [](const atx::core::io::WriteColumn &c) {
    return std::holds_alternative<std::span<const std::int64_t>>(c.data);
  };
  const auto is_f64 = [](const atx::core::io::WriteColumn &c) {
    return std::holds_alternative<std::span<const double>>(c.data);
  };
  const auto is_str = [](const atx::core::io::WriteColumn &c) {
    return std::holds_alternative<std::span<const std::string>>(c.data);
  };

  // The four int64 keys + the two sizes; the three stamps + the two tickers +
  // the call/put flag as strings; everything else double.
  EXPECT_TRUE(is_i64(cols[static_cast<std::size_t>(ce::Col::OkeyYr)]));
  EXPECT_TRUE(is_i64(cols[static_cast<std::size_t>(ce::Col::OkeyMn)]));
  EXPECT_TRUE(is_i64(cols[static_cast<std::size_t>(ce::Col::OkeyDy)]));
  EXPECT_TRUE(is_i64(cols[static_cast<std::size_t>(ce::Col::BidSz)]));
  EXPECT_TRUE(is_i64(cols[static_cast<std::size_t>(ce::Col::AskSz)]));
  EXPECT_TRUE(is_f64(cols[static_cast<std::size_t>(ce::Col::OkeyXx)]));
  EXPECT_TRUE(is_f64(cols[static_cast<std::size_t>(ce::Col::SrPrc)]));
  EXPECT_TRUE(is_f64(cols[static_cast<std::size_t>(ce::Col::SrVol)]));
  EXPECT_TRUE(is_f64(cols[static_cast<std::size_t>(ce::Col::DeDecay)]));
  EXPECT_TRUE(is_f64(cols[static_cast<std::size_t>(ce::Col::Error)]));
  EXPECT_TRUE(is_str(cols[static_cast<std::size_t>(ce::Col::OkeyTk)]));
  EXPECT_TRUE(is_str(cols[static_cast<std::size_t>(ce::Col::OkeyCp)]));
  EXPECT_TRUE(is_str(cols[static_cast<std::size_t>(ce::Col::UndSecKeyTk)]));
  EXPECT_TRUE(is_str(cols[static_cast<std::size_t>(ce::Col::Date)]));
  EXPECT_TRUE(is_str(cols[static_cast<std::size_t>(ce::Col::Timestamp)]));
  EXPECT_TRUE(is_str(cols[static_cast<std::size_t>(ce::Col::TradingDate)]));
}

TEST(ChainExport, AFullyPopulatedRowWritesNoSentinel) {
  ce::ExportColumns table;
  table.append(fully_populated_row());

  EXPECT_EQ(table.rows(), 1u);
  EXPECT_EQ(table.sentinel_total(), 0u);
}

TEST(ChainExport, TheCensusCountsEveryUnsetColumnSeparately) {
  ce::ExportRow row = fully_populated_row();
  row.sr_vol = ce::kMissingF64;             // fit produced no IV
  row.greeks.ph = ce::kMissingF64;          // carry solve failed
  row.u_bid = ce::kMissingF64;              // index complex
  row.u_ask = ce::kMissingF64;
  row.bid_sz = ce::kMissingI64;             // source carried no size column
  row.okey_tk.clear();                      // an unresolvable root

  ce::ExportColumns table;
  table.append(row);
  table.append(row);

  EXPECT_EQ(table.rows(), 2u);
  EXPECT_EQ(table.sentinels(ce::Col::SrVol), 2u);
  EXPECT_EQ(table.sentinels(ce::Col::Ph), 2u);
  EXPECT_EQ(table.sentinels(ce::Col::UBid), 2u);
  EXPECT_EQ(table.sentinels(ce::Col::UAsk), 2u);
  EXPECT_EQ(table.sentinels(ce::Col::BidSz), 2u);
  EXPECT_EQ(table.sentinels(ce::Col::OkeyTk), 2u);
  EXPECT_EQ(table.sentinels(ce::Col::SrPrc), 0u);
  EXPECT_EQ(table.sentinels(ce::Col::AskSz), 0u);
  EXPECT_EQ(table.sentinel_total(), 12u);
}

TEST(ChainExport, ADefaultConstructedRowIsEntirelySentinel) {
  // The default row is the refusal shape: nothing computed, nothing claimed.
  ce::ExportColumns table;
  table.append(ce::ExportRow{});

  EXPECT_EQ(table.rows(), 1u);
  EXPECT_EQ(table.sentinel_total(), ce::kColumnCount);
}

TEST(ChainExport, WriteColumnsAreRowAlignedAndBorrowTheTable) {
  ce::ExportColumns table;
  table.append(fully_populated_row());
  ce::ExportRow second = fully_populated_row();
  second.okey_xx = 665.0;
  table.append(second);

  const std::vector<atx::core::io::WriteColumn> cols = table.write_columns();
  for (const atx::core::io::WriteColumn &c : cols) {
    std::visit([](auto sp) { EXPECT_EQ(sp.size(), 2u); }, c.data);
  }
  const auto strikes =
      std::get<std::span<const double>>(cols[static_cast<std::size_t>(ce::Col::OkeyXx)].data);
  EXPECT_DOUBLE_EQ(strikes[0], 660.0);
  EXPECT_DOUBLE_EQ(strikes[1], 665.0);
}

// ── The streaming write ─────────────────────────────────────────────────────
//
// The universe-scale defect these pin: the accumulate-every-row-then-build-one-
// Arrow-table path held the whole export twice and died with
// "build table: Out of memory" after 164 s of work, writing NOTHING. The
// streaming sink writes one row group per underlier as that underlier
// completes, so peak memory is a CHUNK. What must not change while it does:
// the rows, their order, their types and the sentinel census.

[[nodiscard]] std::filesystem::path scratch_parquet(const char *stem) {
  return std::filesystem::temp_directory_path() /
         (std::string("atx_chain_export_") + stem + ".parquet");
}

// `n` rows for `symbol`, every field walked by the row index so a reorder, a
// duplicate or a dropped row cannot pass by coincidence. Row 0 of each symbol
// carries one refused `srVol` so the census has something to count.
[[nodiscard]] std::vector<ce::ExportRow> symbol_rows(const std::string &symbol, std::size_t n,
                                                     double base_strike) {
  std::vector<ce::ExportRow> rows;
  rows.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    ce::ExportRow row = fully_populated_row();
    row.okey_tk = symbol;
    row.und_sec_key_tk = symbol;
    row.okey_xx = base_strike + static_cast<double>(i);
    row.okey_cp = (i % 2 == 0) ? "Call" : "Put";
    row.okey_dy = static_cast<std::int64_t>(1 + i);
    row.bid_sz = static_cast<std::int64_t>(10 + i);
    row.sr_prc = 1.0 + 0.25 * static_cast<double>(i);
    if (i == 0) {
      row.sr_vol = ce::kMissingF64;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

// The three-underlier fixture both write paths are handed, in ONE order.
[[nodiscard]] std::vector<std::vector<ce::ExportRow>> three_boards() {
  return {symbol_rows("SPY", 5, 660.0), symbol_rows("AAPL", 3, 225.0),
          symbol_rows("BRK.B", 4, 470.0)};
}

// Compare one column of two tables value-by-value; `shape` names it and carries
// its physical type.
void expect_column_equal(const atx::core::io::ParquetTable &a,
                         const atx::core::io::ParquetTable &b,
                         const atx::core::io::WriteColumn &shape) {
  const std::string &name = shape.name;
  if (std::holds_alternative<std::span<const std::string>>(shape.data)) {
    const auto va = a.strings(name);
    const auto vb = b.strings(name);
    ASSERT_TRUE(va.has_value()) << name;
    ASSERT_TRUE(vb.has_value()) << name;
    ASSERT_EQ(va->size(), vb->size()) << name;
    for (std::size_t i = 0; i < va->size(); ++i) {
      EXPECT_EQ((*va)[i], (*vb)[i]) << name << " row " << i;
    }
  } else if (std::holds_alternative<std::span<const std::int64_t>>(shape.data)) {
    const auto va = a.column_view<std::int64_t>(name);
    const auto vb = b.column_view<std::int64_t>(name);
    ASSERT_TRUE(va.has_value()) << name;
    ASSERT_TRUE(vb.has_value()) << name;
    ASSERT_EQ(va->size(), vb->size()) << name;
    for (std::size_t i = 0; i < va->size(); ++i) {
      EXPECT_EQ((*va)[i], (*vb)[i]) << name << " row " << i;
    }
  } else {
    const auto va = a.column_view<double>(name);
    const auto vb = b.column_view<double>(name);
    ASSERT_TRUE(va.has_value()) << name;
    ASSERT_TRUE(vb.has_value()) << name;
    ASSERT_EQ(va->size(), vb->size()) << name;
    for (std::size_t i = 0; i < va->size(); ++i) {
      EXPECT_DOUBLE_EQ((*va)[i], (*vb)[i]) << name << " row " << i;
    }
  }
}

// Write every board through the OLD accumulate-then-write path: one
// ExportColumns for the whole universe, one whole-table `write_parquet`. Kept
// alive here (and only here) as the REFERENCE the streamed file must match.
[[nodiscard]] ce::ExportColumns accumulate_all(
    const std::vector<std::vector<ce::ExportRow>> &boards, const std::filesystem::path &path) {
  ce::ExportColumns table;
  for (const std::vector<ce::ExportRow> &board : boards) {
    for (const ce::ExportRow &row : board) {
      table.append(row);
    }
  }
  const std::vector<atx::core::io::WriteColumn> cols = table.write_columns();
  const atx::core::Status wrote = atx::core::io::write_parquet(cols, path.string());
  EXPECT_TRUE(wrote.has_value());
  return table;
}

TEST(ChainExport, StreamedFileContentEqualsTheAccumulatedTableExactly) {
  const std::vector<std::vector<ce::ExportRow>> boards = three_boards();
  const std::filesystem::path ref = scratch_parquet("accumulated");
  const std::filesystem::path streamed = scratch_parquet("streamed");
  std::filesystem::remove(ref);
  std::filesystem::remove(streamed);

  const ce::ExportColumns reference = accumulate_all(boards, ref);

  ce::ChainExportWriter writer;
  ASSERT_TRUE(writer.open(streamed.string()).has_value());
  for (const std::vector<ce::ExportRow> &board : boards) {
    const atx::core::Status st = writer.write_board(board);
    ASSERT_TRUE(st.has_value());
  }
  ASSERT_TRUE(writer.close().has_value());

  const auto ta = atx::core::io::read_parquet(ref.string());
  const auto tb = atx::core::io::read_parquet(streamed.string());
  ASSERT_TRUE(ta.has_value());
  ASSERT_TRUE(tb.has_value());
  EXPECT_EQ(ta->num_rows(), 12);
  EXPECT_EQ(tb->num_rows(), ta->num_rows());
  EXPECT_EQ(tb->num_columns(), ta->num_columns());
  EXPECT_EQ(tb->num_columns(), static_cast<std::int64_t>(ce::kColumnCount));

  // Same columns, same order, same types, same values — one column at a time so
  // a failure names the column that drifted.
  const std::vector<atx::core::io::WriteColumn> shape = reference.write_columns();
  ASSERT_EQ(shape.size(), ce::kColumnCount);
  for (const atx::core::io::WriteColumn &c : shape) {
    expect_column_equal(*ta, *tb, c);
  }
  ASSERT_EQ(tb->schema().size(), ce::kColumnCount);
  for (std::size_t i = 0; i < ce::kColumnCount; ++i) {
    EXPECT_EQ(tb->schema().columns[i].name, ce::kColumnNames[i]) << "column " << i;
  }
}

TEST(ChainExport, StreamedFileCarriesOneRowGroupPerUnderlier) {
  const std::vector<std::vector<ce::ExportRow>> boards = three_boards();
  const std::filesystem::path streamed = scratch_parquet("rowgroups");
  std::filesystem::remove(streamed);

  ce::ChainExportWriter writer;
  ASSERT_TRUE(writer.open(streamed.string()).has_value());
  for (const std::vector<ce::ExportRow> &board : boards) {
    ASSERT_TRUE(writer.write_board(board).has_value());
  }
  ASSERT_TRUE(writer.close().has_value());

  auto lz = atx::core::io::LazyParquet::scan(streamed.string());
  ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->num_rows(), 12);
  // The convention `pull_opra_hive.py::_write_date_file` established: one row
  // group per underlying, so a per-symbol read prunes on the group statistics.
  EXPECT_EQ(lz->num_row_groups(), 3);

  auto stream = lz->stream();
  ASSERT_TRUE(stream.has_value());
  const std::array<const char *, 3> expect_tk{"SPY", "AAPL", "BRK.B"};
  const std::array<std::int64_t, 3> expect_rows{5, 3, 4};
  for (std::size_t g = 0; g < expect_tk.size(); ++g) {
    auto batch = stream->next();
    ASSERT_TRUE(batch.has_value()) << "group " << g;
    ASSERT_TRUE(batch->has_value()) << "group " << g;
    EXPECT_EQ((*batch)->num_rows(), expect_rows[g]) << "group " << g;
    const auto tk = (*batch)->strings("okey_tk");
    ASSERT_TRUE(tk.has_value());
    for (const std::string_view v : *tk) {
      EXPECT_EQ(v, expect_tk[g]) << "group " << g; // min == max => prunable
    }
  }
  auto past_end = stream->next();
  ASSERT_TRUE(past_end.has_value());
  EXPECT_FALSE(past_end->has_value());
}

TEST(ChainExport, StreamingCensusEqualsTheWholeTableCensus) {
  const std::vector<std::vector<ce::ExportRow>> boards = three_boards();
  const std::filesystem::path ref = scratch_parquet("census_ref");
  const std::filesystem::path streamed = scratch_parquet("census_streamed");
  std::filesystem::remove(ref);
  std::filesystem::remove(streamed);

  const ce::ExportColumns reference = accumulate_all(boards, ref);

  ce::ChainExportWriter writer;
  ASSERT_TRUE(writer.open(streamed.string()).has_value());
  for (const std::vector<ce::ExportRow> &board : boards) {
    ASSERT_TRUE(writer.write_board(board).has_value());
  }
  ASSERT_TRUE(writer.close().has_value());

  const ce::ExportCensus &census = writer.census();
  EXPECT_EQ(census.rows, reference.rows());
  EXPECT_EQ(census.sentinel_total(), reference.sentinel_total());
  for (std::size_t c = 0; c < ce::kColumnCount; ++c) {
    EXPECT_EQ(census.sentinels[c], reference.sentinels(static_cast<ce::Col>(c)))
        << ce::kColumnNames[c];
  }
  // Three boards, one refused srVol each: the census is EXACT, not sampled.
  EXPECT_EQ(census.sentinels[ce::col_index(ce::Col::SrVol)], 3u);
  EXPECT_EQ(census.sentinels[ce::col_index(ce::Col::Error)], 0u);
}

TEST(ChainExport, WriterReleasesEachBoardsStorageAsItGoes) {
  // The bound the scale run needs: the sink must not retain a board after it is
  // written, so its buffer never grows past the LARGEST board it was handed.
  const std::filesystem::path streamed = scratch_parquet("bounded");
  std::filesystem::remove(streamed);

  ce::ChainExportWriter writer;
  ASSERT_TRUE(writer.open(streamed.string()).has_value());
  for (std::size_t i = 0; i < 40; ++i) {
    ASSERT_TRUE(writer.write_board(symbol_rows("SYM" + std::to_string(i), 6, 10.0)).has_value());
    EXPECT_EQ(writer.buffered_rows(), 0u) << "board " << i; // released, every time
  }
  ASSERT_TRUE(writer.close().has_value());

  EXPECT_EQ(writer.census().rows, 240u);
  auto lz = atx::core::io::LazyParquet::scan(streamed.string());
  ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->num_rows(), 240);
  EXPECT_EQ(lz->num_row_groups(), 40);
}

TEST(ChainExport, AnEmptyBoardWritesNoRowGroupAndCountsNothing) {
  const std::filesystem::path streamed = scratch_parquet("emptyboard");
  std::filesystem::remove(streamed);

  ce::ChainExportWriter writer;
  ASSERT_TRUE(writer.open(streamed.string()).has_value());
  ASSERT_TRUE(writer.write_board({}).has_value()); // a dropped symbol
  ASSERT_TRUE(writer.write_board(symbol_rows("SPY", 2, 660.0)).has_value());
  ASSERT_TRUE(writer.write_board({}).has_value());
  ASSERT_TRUE(writer.close().has_value());

  EXPECT_EQ(writer.census().rows, 2u);
  auto lz = atx::core::io::LazyParquet::scan(streamed.string());
  ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->num_rows(), 2);
  EXPECT_EQ(lz->num_row_groups(), 1);
}

TEST(ChainExport, ClearResetsTheRowsAndTheCensusTogether) {
  ce::ExportColumns table;
  table.append(fully_populated_row());
  table.append(ce::ExportRow{});
  ASSERT_EQ(table.rows(), 2u);
  ASSERT_EQ(table.sentinel_total(), ce::kColumnCount);

  table.clear();
  EXPECT_EQ(table.rows(), 0u);
  EXPECT_EQ(table.sentinel_total(), 0u);
  // The schema survives a clear: the write plan is still the full contract.
  const std::vector<atx::core::io::WriteColumn> cols = table.write_columns();
  ASSERT_EQ(cols.size(), ce::kColumnCount);
  for (const atx::core::io::WriteColumn &c : cols) {
    std::visit([](auto sp) { EXPECT_EQ(sp.size(), 0u); }, c.data);
  }

  table.append(fully_populated_row());
  EXPECT_EQ(table.rows(), 1u);
  EXPECT_EQ(table.sentinel_total(), 0u);
}

TEST(ChainExport, WritingBeforeOpenIsRefusedRatherThanDroppedOnTheFloor) {
  ce::ChainExportWriter writer;
  const std::vector<ce::ExportRow> board = symbol_rows("SPY", 2, 660.0);
  const atx::core::Status st = writer.write_board(board);
  EXPECT_FALSE(st.has_value());
  EXPECT_EQ(writer.census().rows, 0u);
}

} // namespace
