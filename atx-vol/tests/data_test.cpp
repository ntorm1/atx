#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/data.hpp"
#include "atx/vol/universe.hpp"

// Data-ingestion / install coverage, ported from the C ats-vol test
// `test_data_loader.c`. The C file drives CSV/Parquet fixtures through the
// arena loaders; those file paths are deferred in this port (see data.hpp), so
// the mirror here builds the equivalent `QuoteFrame` in memory and exercises
// the same install semantics and assertions the C tests make:
//
//   fixture_loads / install_into_universe -> DataInstall_SyntheticFrame_*
//   install_multi_uid_rows_into_universe  -> DataInstall_MultiUid_*
//   install_rejects_snapshot_without_yield_curve -> DataInstall_NoYieldCurve_*
//   year_fraction_helper                  -> YearFraction_*
//
// Plus coverage the C spreads across the Parquet source-plane tests
// (build_expiry_inputs / source_atm_vol stamping / find_expiry_inputs) exercised
// here directly against the in-memory frame.

namespace {

using atx::vol::build_expiry_inputs;
using atx::vol::build_uid_list;
using atx::vol::Chain;
using atx::vol::data_install;
using atx::vol::ErrorCode;
using atx::vol::ExpiryInputField;
using atx::vol::ExpiryInputs;
using atx::vol::find_expiry_inputs;
using atx::vol::has_flag;
using atx::vol::iso_to_ns;
using atx::vol::kQFlagCrossed;
using atx::vol::kQFlagLocked;
using atx::vol::load_spiderrock_parquet;
using atx::vol::ns_to_iso_date;
using atx::vol::QuoteFrame;
using atx::vol::QuoteRow;
using atx::vol::Side;
using atx::vol::SpiderRockLoadSpec;
using atx::vol::Uid;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::UniverseStats;
using atx::vol::year_fraction;

// Build a synthetic chain frame: `expiries.size()` expiries, `n_k` strikes each,
// call+put per strike. Mirrors the committed synthetic fixture
// (3 expiries x 11 strikes x 2 sides = 66 rows).
QuoteFrame make_synthetic_frame(const std::vector<std::string> &expiries, int n_k) {
  QuoteFrame f;
  f.uid = "FAKE";
  f.snapshot_iso = "2026-05-01";
  f.snapshot_ts_ns = iso_to_ns("2026-05-01");
  f.spot = 100.0;
  f.spot_ts_ns = f.snapshot_ts_ns;
  f.yc_pillar_t = {0.05, 1.0};
  f.yc_pillar_r = {0.05, 0.05};

  for (const std::string &exp : expiries) {
    for (int k = 0; k < n_k; ++k) {
      const double strike = 90.0 + 5.0 * static_cast<double>(k);
      for (int s = 0; s < 2; ++s) {
        QuoteRow row;
        row.uid = "FAKE";
        row.expiry_iso = exp;
        row.strike = strike;
        row.side = (s == 0) ? Side::Call : Side::Put;
        row.bid = 1.0 + static_cast<double>(k);
        row.ask = 1.2 + static_cast<double>(k);
        row.bid_size = 10;
        row.ask_size = 12;
        row.under_spot = 100.0;
        f.rows.push_back(row);
      }
    }
  }
  return f;
}

// ── year_fraction (test_data_loader.c year_fraction_helper) ─────────────────

TEST(DataYearFraction, KnownOffsets_Match) {
  EXPECT_LT(std::fabs(year_fraction("2026-05-01", "2027-05-01") - 365.0 / 365.25), 1.0e-6);
  EXPECT_LT(std::fabs(year_fraction("2026-05-01", "2026-05-01")), 1.0e-12);
  EXPECT_LT(std::fabs(year_fraction("2026-05-01", "2026-06-12") - 42.0 / 365.25), 1.0e-6);

  const double intraday =
      year_fraction("2026-05-01 12:00:00.000000", "2026-05-01 18:00:00.000000");
  EXPECT_LT(std::fabs(intraday - (6.0 / 24.0) / 365.25), 1.0e-12);
}

TEST(DataYearFraction, UnparseableInput_ReturnsNaN) {
  EXPECT_TRUE(std::isnan(year_fraction("not-a-date", "2026-05-01")));
  EXPECT_TRUE(std::isnan(year_fraction("2026-05-01", "")));
}

// ── iso_to_ns / ns_to_iso_date ──────────────────────────────────────────────

TEST(DataIso, IsoToNs_DateOnly_RoundTripsThroughNsToIsoDate) {
  const std::int64_t ns = iso_to_ns("2026-06-19");
  EXPECT_GT(ns, std::int64_t{0});
  EXPECT_EQ(ns_to_iso_date(ns), std::string{"2026-06-19"});
}

TEST(DataIso, IsoToNs_Invalid_ReturnsZero) {
  EXPECT_EQ(iso_to_ns("garbage"), std::int64_t{0});
  EXPECT_EQ(iso_to_ns("2026-13-01"), std::int64_t{0}); // month out of range
}

TEST(DataIso, NsToIsoDate_KnownEpochDays_Match) {
  EXPECT_EQ(ns_to_iso_date(0), std::string{"1970-01-01"});
  EXPECT_EQ(ns_to_iso_date(iso_to_ns("2000-02-29")), std::string{"2000-02-29"});
}

// ── Install: synthetic frame (fixture_loads / install_into_universe) ────────

TEST(DataInstall, SyntheticFrame_PopulatesChainsStrikesQuotes) {
  const QuoteFrame f =
      make_synthetic_frame({"2026-06-19", "2026-09-18", "2026-12-18"}, /*n_k=*/11);
  ASSERT_EQ(f.rows.size(), std::size_t{66});

  Universe u;
  const auto uid = data_install(u, f);
  ASSERT_TRUE(uid.has_value());

  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{3});
  for (const Chain &c : (*under)->chains) {
    EXPECT_EQ(c.n_strikes(), std::size_t{11});
    EXPECT_GT(c.T, 0.0);
    EXPECT_LT(c.T, 1.5);
  }

  // Every (strike, side) slot should carry a positive ask after install.
  int observed = 0;
  for (const Chain &c : (*under)->chains) {
    for (const double ask : c.asks) {
      if (ask > 0.0) {
        ++observed;
      }
    }
  }
  EXPECT_EQ(observed, 66);

  const UniverseStats st = u.stats();
  EXPECT_EQ(st.n_underlyings, std::uint32_t{1});
  EXPECT_EQ(st.n_chains, std::uint32_t{3});
  EXPECT_EQ(st.n_strikes, std::uint64_t{33});
}

TEST(DataInstall, SyntheticFrame_SortsChainsAscendingInT) {
  // Rows arrive with the far expiry first; install must sort chains by T and
  // re-issue expiry_id to the sorted positions.
  const QuoteFrame f =
      make_synthetic_frame({"2026-12-18", "2026-06-19", "2026-09-18"}, /*n_k=*/3);

  Universe u;
  const auto uid = data_install(u, f);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());

  const std::vector<Chain> &chains = (*under)->chains;
  ASSERT_EQ(chains.size(), std::size_t{3});
  for (std::size_t i = 0; i < chains.size(); ++i) {
    EXPECT_EQ(chains[i].expiry_id, static_cast<atx::vol::ExpiryId>(i));
    if (i > 0) {
      EXPECT_LT(chains[i - 1].T, chains[i].T);
    }
  }
  // The front chain is the nearest expiry (2026-06-19).
  EXPECT_EQ(ns_to_iso_date(chains[0].expiry_ns), std::string{"2026-06-19"});
}

TEST(DataInstall, SyntheticFrame_MidsAreHalfBidPlusAsk) {
  const QuoteFrame f = make_synthetic_frame({"2026-06-19"}, /*n_k=*/2);
  Universe u;
  const auto uid = data_install(u, f);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  const Chain &c = (*under)->chains[0];
  // strike k=1: bid=2.0, ask=2.2, mid=2.1 (call slot).
  const std::size_t call_idx = atx::vol::chain_index(1u, Side::Call);
  EXPECT_DOUBLE_EQ(c.mids[call_idx], 0.5 * (2.0 + 2.2));
}

// ── Install: multi-uid (install_multi_uid_rows_into_universe) ────────────────

TEST(DataInstall, MultiUidRows_SplitInto_SeparateUnderlyings) {
  QuoteFrame f;
  f.uid = "AAPL";
  f.snapshot_iso = "2026-05-01 13:30:00.000000";
  f.snapshot_ts_ns = iso_to_ns(f.snapshot_iso);
  f.spot = 278.0;
  f.spot_ts_ns = f.snapshot_ts_ns;
  f.yc_pillar_t = {0.05, 1.0};
  f.yc_pillar_r = {0.045, 0.045};

  const char *uids[2] = {"AAPL", "MSFT"};
  for (int uu = 0; uu < 2; ++uu) {
    for (int s = 0; s < 2; ++s) {
      QuoteRow row;
      row.uid = uids[uu];
      row.expiry_iso = "2026-06-19";
      row.strike = (uu == 0) ? 275.0 : 520.0;
      row.side = (s == 0) ? Side::Call : Side::Put;
      row.bid = (s == 0) ? 4.0 : 3.5;
      row.ask = (s == 0) ? 4.5 : 4.0;
      row.bid_size = 10;
      row.ask_size = 12;
      row.under_spot = (uu == 0) ? 278.0 : 515.0;
      row.ts_ns = iso_to_ns("2026-05-01 13:30:01.000000");
      f.rows.push_back(row);
    }
  }

  Universe u;
  const auto first = data_install(u, f);
  ASSERT_TRUE(first.has_value());

  const UniverseStats st = u.stats();
  EXPECT_EQ(st.n_underlyings, std::uint32_t{2});
  EXPECT_EQ(st.n_chains, std::uint32_t{2});
  EXPECT_EQ(st.n_strikes, std::uint64_t{2});

  const auto msft = u.intern_ticker("MSFT");
  ASSERT_TRUE(msft.has_value());
  const auto msft_under = u.get_underlying(*msft);
  ASSERT_TRUE(msft_under.has_value());
  EXPECT_GT((*msft_under)->spot, 500.0);
}

// ── Install: yield-curve gate (install_rejects_snapshot_without_yield_curve) ─

TEST(DataInstall, NoYieldCurve_ReturnsInvalidArgument) {
  QuoteFrame f;
  f.uid = "AAPL";
  f.snapshot_iso = "2026-05-01 13:30:00.000000";
  f.snapshot_ts_ns = iso_to_ns(f.snapshot_iso);
  f.spot = 200.0;
  f.spot_ts_ns = f.snapshot_ts_ns;
  // yc pillars deliberately empty.

  QuoteRow row;
  row.uid = "AAPL";
  row.expiry_iso = "2026-06-19";
  row.strike = 200.0;
  row.side = Side::Call;
  row.bid = 1.0;
  row.ask = 1.1;
  row.bid_size = 1;
  row.ask_size = 1;
  row.under_spot = 200.0;
  f.rows.push_back(row);

  Universe u;
  const auto res = data_install(u, f);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(res.error().message().find("yield curve"), std::string::npos);
}

// ── Install: quote flags ─────────────────────────────────────────────────────

TEST(DataInstall, CrossedAndLockedQuotes_SetFlagBits) {
  QuoteFrame f;
  f.uid = "SPY";
  f.snapshot_iso = "2026-05-01";
  f.snapshot_ts_ns = iso_to_ns("2026-05-01");
  f.spot = 400.0;
  f.spot_ts_ns = f.snapshot_ts_ns;
  f.yc_pillar_t = {0.5};
  f.yc_pillar_r = {0.05};

  // Locked (bid == ask) call, crossed (bid > ask) put, both on strike 400.
  QuoteRow locked;
  locked.uid = "SPY";
  locked.expiry_iso = "2026-06-19";
  locked.strike = 400.0;
  locked.side = Side::Call;
  locked.bid = 2.0;
  locked.ask = 2.0;
  QuoteRow crossed;
  crossed.uid = "SPY";
  crossed.expiry_iso = "2026-06-19";
  crossed.strike = 400.0;
  crossed.side = Side::Put;
  crossed.bid = 3.0;
  crossed.ask = 2.5;
  f.rows.push_back(locked);
  f.rows.push_back(crossed);

  Universe u;
  const auto uid = data_install(u, f);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  const Chain &c = (*under)->chains[0];
  EXPECT_EQ(c.flags[atx::vol::chain_index(0u, Side::Call)], kQFlagLocked);
  EXPECT_EQ(c.flags[atx::vol::chain_index(0u, Side::Put)], kQFlagCrossed);
}

// ── Install: row validation ──────────────────────────────────────────────────

TEST(DataInstall, InvalidRow_ReturnsInvalidArgument) {
  const auto make_base = []() {
    QuoteFrame f;
    f.uid = "AAPL";
    f.snapshot_iso = "2026-05-01";
    f.snapshot_ts_ns = iso_to_ns("2026-05-01");
    f.spot = 100.0;
    f.spot_ts_ns = f.snapshot_ts_ns;
    f.yc_pillar_t = {0.5};
    f.yc_pillar_r = {0.05};
    return f;
  };
  const auto base_row = []() {
    QuoteRow r;
    r.uid = "AAPL";
    r.expiry_iso = "2026-06-19";
    r.strike = 100.0;
    r.side = Side::Call;
    r.bid = 1.0;
    r.ask = 1.1;
    return r;
  };

  {
    QuoteFrame f = make_base();
    QuoteRow r = base_row();
    r.strike = -1.0; // non-positive strike
    f.rows.push_back(r);
    Universe u;
    const auto res = data_install(u, f);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
  {
    QuoteFrame f = make_base();
    QuoteRow r = base_row();
    r.bid_size = -5; // negative size
    f.rows.push_back(r);
    Universe u;
    const auto res = data_install(u, f);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
  {
    QuoteFrame f = make_base();
    QuoteRow r = base_row();
    r.bid = std::numeric_limits<double>::quiet_NaN(); // non-finite bid
    f.rows.push_back(r);
    Universe u;
    const auto res = data_install(u, f);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
  {
    QuoteFrame f = make_base();
    QuoteRow r = base_row();
    r.expiry_iso = "not-a-date"; // unparseable expiry
    f.rows.push_back(r);
    Universe u;
    const auto res = data_install(u, f);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  }
}

// ── build_uid_list ───────────────────────────────────────────────────────────

TEST(DataUidList, MultiUidRows_DeduplicatesInFirstSeenOrder) {
  QuoteFrame f; // no default uid
  for (const char *tk : {"AAPL", "MSFT", "AAPL"}) {
    QuoteRow r;
    r.uid = tk;
    r.expiry_iso = "2026-06-19";
    r.strike = 100.0;
    f.rows.push_back(r);
  }
  const auto st = build_uid_list(f);
  ASSERT_TRUE(st.has_value());
  ASSERT_EQ(f.uid_strs.size(), std::size_t{2});
  EXPECT_EQ(f.uid_strs[0], std::string{"AAPL"});
  EXPECT_EQ(f.uid_strs[1], std::string{"MSFT"});
  EXPECT_EQ(f.uid, std::string{"AAPL"}); // default backfilled from first distinct
}

TEST(DataUidList, DefaultUidNoRows_YieldsSingletonList) {
  QuoteFrame f;
  f.uid = "FAKE";
  const auto st = build_uid_list(f);
  ASSERT_TRUE(st.has_value());
  ASSERT_EQ(f.uid_strs.size(), std::size_t{1});
  EXPECT_EQ(f.uid_strs[0], std::string{"FAKE"});
}

TEST(DataUidList, EmptyRowUid_ReturnsInvalidArgument) {
  QuoteFrame f; // no default uid
  QuoteRow r;   // r.uid empty -> resolves to empty frame uid
  r.expiry_iso = "2026-06-19";
  r.strike = 100.0;
  f.rows.push_back(r);
  const auto st = build_uid_list(f);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

// ── build_expiry_inputs / find_expiry_inputs / source_atm_vol stamping ──────

TEST(DataExpiryInputs, BuildDedupesPerExpiryAndSetsCompleteness) {
  QuoteFrame f;
  f.uid = "SPY";
  // Two rows, same (uid, expiry): first carries rate+years, second carries
  // atmVol. Dedupe collapses to one cell with all three bits set.
  QuoteRow a;
  a.uid = "SPY";
  a.expiry_iso = "2026-06-19";
  a.strike = 400.0;
  a.rate_source = 0.045;
  a.years_source = 0.13;
  QuoteRow b;
  b.uid = "SPY";
  b.expiry_iso = "2026-06-19";
  b.strike = 405.0;
  b.atm_vol_source = 0.22;
  f.rows.push_back(a);
  f.rows.push_back(b);

  build_expiry_inputs(f);
  ASSERT_EQ(f.expiry_inputs.size(), std::size_t{1});
  const ExpiryInputs &cell = f.expiry_inputs[0];
  EXPECT_TRUE(has_flag(cell.completeness, ExpiryInputField::Rate));
  EXPECT_TRUE(has_flag(cell.completeness, ExpiryInputField::T));
  EXPECT_TRUE(has_flag(cell.completeness, ExpiryInputField::AtmVol));
  EXPECT_FALSE(has_flag(cell.completeness, ExpiryInputField::Sdiv));
  // All-required (rate + T) mask is satisfied.
  EXPECT_EQ(cell.completeness & atx::vol::kExpiryInputAllRequired,
            atx::vol::kExpiryInputAllRequired);
  EXPECT_DOUBLE_EQ(cell.rate, 0.045);
  EXPECT_DOUBLE_EQ(cell.T_vol, 0.13);
  EXPECT_DOUBLE_EQ(cell.atm_vol, 0.22);
}

TEST(DataExpiryInputs, FindMissingKey_ReturnsNull) {
  QuoteFrame f;
  f.uid = "SPY";
  QuoteRow r;
  r.uid = "SPY";
  r.expiry_iso = "2026-06-19";
  r.strike = 400.0;
  r.atm_vol_source = 0.2;
  f.rows.push_back(r);
  build_expiry_inputs(f);
  EXPECT_EQ(find_expiry_inputs(f, "NOPE", "2026-06-19"), nullptr);
  EXPECT_EQ(find_expiry_inputs(f, "SPY", "2099-01-01"), nullptr);
  EXPECT_NE(find_expiry_inputs(f, "SPY", "2026-06-19"), nullptr);
}

TEST(DataInstall, StampsSourceAtmVolFromExpiryInputs) {
  QuoteFrame f;
  f.uid = "SPY";
  f.snapshot_iso = "2026-05-01";
  f.snapshot_ts_ns = iso_to_ns("2026-05-01");
  f.spot = 400.0;
  f.spot_ts_ns = f.snapshot_ts_ns;
  f.yc_pillar_t = {0.5};
  f.yc_pillar_r = {0.05};

  // Expiry A carries an atmVol source; expiry B does not.
  QuoteRow a;
  a.uid = "SPY";
  a.expiry_iso = "2026-06-19";
  a.strike = 400.0;
  a.side = Side::Call;
  a.bid = 1.0;
  a.ask = 1.2;
  a.atm_vol_source = 0.25;
  QuoteRow b;
  b.uid = "SPY";
  b.expiry_iso = "2026-09-18";
  b.strike = 400.0;
  b.side = Side::Call;
  b.bid = 1.0;
  b.ask = 1.2;
  // b.atm_vol_source stays NaN.
  f.rows.push_back(a);
  f.rows.push_back(b);
  build_expiry_inputs(f);

  Universe u;
  const auto uid = data_install(u, f);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());

  int n_present = 0;
  int n_absent = 0;
  for (const Chain &c : (*under)->chains) {
    if (c.source_atm_vol_present) {
      ++n_present;
      EXPECT_DOUBLE_EQ(c.source_atm_vol, 0.25);
      EXPECT_EQ(ns_to_iso_date(c.expiry_ns), std::string{"2026-06-19"});
    } else {
      ++n_absent;
      EXPECT_TRUE(std::isnan(c.source_atm_vol));
    }
  }
  EXPECT_EQ(n_present, 1);
  EXPECT_EQ(n_absent, 1);
}

// ── Deferred SpiderRock Parquet loader ──────────────────────────────────────

TEST(DataParquet, LoadSpiderRock_Deferred_ReturnsNotImplemented) {
  SpiderRockLoadSpec spec;
  spec.parquet_path = "does-not-matter.parquet";
  spec.symbols = {"SPY"};
  spec.snapshot_time = "13:30";
  const auto res = load_spiderrock_parquet(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotImplemented);
}

} // namespace
