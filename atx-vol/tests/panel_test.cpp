#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/dividend.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_time.hpp"

// Option-chain PANEL fixture coverage: the deterministic known-truth synthetic
// American-equity generator and the self-contained CSV chain loader. Together
// these are the fixture path for the Vola-parity harness (the repo commits no
// option-chain data and the SpiderRock Parquet loader is deferred).

namespace {

using atx::vol::american_price;
using atx::vol::Chain;
using atx::vol::CsvChainSpec;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::hybrid_forward;
using atx::vol::iso_to_ns;
using atx::vol::load_chain_csv;
using atx::vol::make_synthetic_american_panel;
using atx::vol::QuoteFrame;
using atx::vol::S3Params;
using atx::vol::Side;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanel;
using atx::vol::SynthPanelSpec;
using atx::vol::TimeConvention;
using atx::vol::TimeSpec;
using atx::vol::Universe;
using atx::vol::vol_time_years;
using atx::vol::VolTimeCalendar;
using atx::vol::VolTimeParams;
using atx::vol::year_fraction;
using atx::vol::QuoteRow;

// Canonical two-expiry, five-strike spec (2 x 5 x 2 = 20 rows).
SynthPanelSpec make_spec() {
  SynthPanelSpec spec;
  spec.uid = "SYNTH";
  spec.snapshot_iso = "2026-06-19";
  spec.spot = 100.0;
  spec.r = 0.05;
  spec.expiries = {
      SynthExpiry{"2026-09-18", 0.25, S3Params{0.22, -0.10, 0.40}},
      SynthExpiry{"2026-12-18", 0.50, S3Params{0.20, -0.08, 0.35}},
  };
  spec.strikes = {80.0, 90.0, 100.0, 110.0, 120.0};
  return spec;
}

// ── Synthetic generator ─────────────────────────────────────────────────────

TEST(Panel, Synthetic_ValidSpec_EmitsAllRowsBidLtAskMidPositive) {
  const SynthPanelSpec spec = make_spec();
  const auto res = make_synthetic_american_panel(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const SynthPanel& panel = *res;

  const std::size_t expected =
      spec.expiries.size() * spec.strikes.size() * 2u;
  EXPECT_EQ(panel.frame.rows.size(), expected);
  EXPECT_EQ(panel.truth_iv.size(), expected);
  EXPECT_EQ(panel.truth_forward.size(), spec.expiries.size());

  for (const auto& row : panel.frame.rows) {
    EXPECT_LT(row.bid, row.ask);
    EXPECT_GT(0.5 * (row.bid + row.ask), 0.0);
    EXPECT_GE(row.bid, 0.0);
  }
  for (const double iv : panel.truth_iv) {
    EXPECT_GT(iv, 0.0);
    EXPECT_TRUE(std::isfinite(iv));
  }
}

TEST(Panel, Synthetic_RederivedAmericanMid_LiesWithinHalfSpread) {
  const SynthPanelSpec spec = make_spec();
  const auto res = make_synthetic_american_panel(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const SynthPanel& panel = *res;

  const std::size_t n_k = spec.strikes.size();
  for (std::size_t i = 0; i < panel.frame.rows.size(); ++i) {
    const auto& row = panel.frame.rows[i];
    const std::size_t eidx = i / (n_k * 2u); // expiry-major layout
    const double T = spec.expiries[eidx].T;
    const double F = panel.truth_forward[eidx];
    const double q_eff = spec.r - std::log(F / spec.spot) / T;

    const auto mid = american_price(spec.spot, row.strike, T, panel.truth_iv[i], spec.r,
                                    q_eff, row.side, spec.method);
    ASSERT_TRUE(mid.has_value()) << mid.error().to_string();
    // The re-derived truth mid must sit inside the emitted [bid, ask] band.
    EXPECT_GE(*mid, row.bid - 1.0e-9);
    EXPECT_LE(*mid, row.ask + 1.0e-9);
  }
}

TEST(Panel, Synthetic_Frame_InstallsIntoUniverse) {
  const SynthPanelSpec spec = make_spec();
  const auto res = make_synthetic_american_panel(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, res->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();

  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  EXPECT_EQ((*under)->chains.size(), spec.expiries.size());
  for (const Chain& c : (*under)->chains) {
    EXPECT_EQ(c.n_strikes(), spec.strikes.size());
    EXPECT_GT(c.T, 0.0);
  }
}

TEST(Panel, Synthetic_TruthForward_EqualsHybridForward) {
  const SynthPanelSpec spec = make_spec();
  const auto res = make_synthetic_american_panel(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const SynthPanel& panel = *res;

  const std::int64_t now_ns = iso_to_ns(spec.snapshot_iso);
  for (std::size_t e = 0; e < spec.expiries.size(); ++e) {
    const std::int64_t expiry_ns = iso_to_ns(spec.expiries[e].expiry_iso);
    const double F = hybrid_forward(spec.spot, spec.r, spec.borrow, spec.expiries[e].T,
                                    spec.cash_divs, expiry_ns, now_ns, spec.hyb);
    EXPECT_DOUBLE_EQ(panel.truth_forward[e], F);
  }
}

TEST(Panel, Synthetic_DiscreteCashDividend_DepressesForwardBelowCarry) {
  SynthPanelSpec spec;
  spec.snapshot_iso = "2026-06-19";
  spec.spot = 100.0;
  spec.r = 0.05;
  spec.expiries = {SynthExpiry{"2026-12-18", 0.50, S3Params{0.20, 0.0, 0.30}}};
  spec.strikes = {100.0};
  DividendEvent d;
  d.ex_date_ns = iso_to_ns("2026-09-15"); // strictly inside (snapshot, expiry]
  d.amount = 2.0;
  spec.cash_divs = {d};

  const auto res = make_synthetic_american_panel(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  const double pure_carry = spec.spot * std::exp(spec.r * spec.expiries[0].T);
  EXPECT_LT(res->truth_forward[0], pure_carry);
}

TEST(Panel, Synthetic_EmptyExpiries_ReturnsInvalidArgument) {
  SynthPanelSpec spec = make_spec();
  spec.expiries.clear();
  const auto res = make_synthetic_american_panel(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── CSV loader ───────────────────────────────────────────────────────────────

std::string temp_csv_path(const char* name) {
  return std::string(::testing::TempDir()) + name;
}

TEST(Panel, Csv_RoundTrip_LoadsRowsAndFields) {
  const std::string path = temp_csv_path("atx_panel_roundtrip.csv");
  {
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    out << "uid,snapshot_iso,spot,expiry_iso,strike,side,bid,ask,bid_size,ask_size,under_spot\n";
    out << "SPY,2026-06-19,400.0,2026-09-18,400,C,5.0,5.4,10,12,400.0\n";
    out << "  SPY , 2026-06-19 , 400.0 , 2026-09-18 , 405 , Put , 4.0 , 4.3 , 8 , 9 , 400.0 , 0.045 , 1.5\n";
  }

  CsvChainSpec spec;
  spec.path = path;
  spec.yc_pillar_t = {0.25, 1.0};
  spec.yc_pillar_r = {0.045, 0.045};

  const auto res = load_chain_csv(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const QuoteFrame& frame = *res;

  ASSERT_EQ(frame.rows.size(), std::size_t{2});
  EXPECT_EQ(frame.uid, std::string{"SPY"});
  EXPECT_EQ(frame.snapshot_iso, std::string{"2026-06-19"});
  EXPECT_DOUBLE_EQ(frame.spot, 400.0);

  EXPECT_EQ(frame.rows[0].side, Side::Call);
  EXPECT_DOUBLE_EQ(frame.rows[0].strike, 400.0);
  EXPECT_DOUBLE_EQ(frame.rows[0].bid, 5.0);
  EXPECT_DOUBLE_EQ(frame.rows[0].ask, 5.4);

  EXPECT_EQ(frame.rows[1].side, Side::Put);
  EXPECT_DOUBLE_EQ(frame.rows[1].strike, 405.0);
  EXPECT_EQ(frame.rows[1].bid_size, 8);
  EXPECT_DOUBLE_EQ(frame.rows[1].rate_source, 0.045);
  EXPECT_DOUBLE_EQ(frame.rows[1].ddiv_source, 1.5);

  // A rate/ddiv-bearing frame builds the per-(uid, expiry) source-input table.
  EXPECT_FALSE(frame.expiry_inputs.empty());

  // The pillars ride along from the spec, so the frame installs.
  Universe u;
  const auto uid = atx::vol::data_install(u, frame);
  EXPECT_TRUE(uid.has_value()) << (uid.has_value() ? "" : uid.error().to_string());
}

TEST(Panel, Csv_MalformedRow_ReturnsParseError) {
  const std::string path = temp_csv_path("atx_panel_malformed.csv");
  {
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    out << "uid,snapshot_iso,spot,expiry_iso,strike,side,bid,ask,bid_size,ask_size,under_spot\n";
    out << "SPY,2026-06-19,400.0,2026-09-18,NOT_A_NUMBER,C,5.0,5.4,10,12,400.0\n";
  }

  CsvChainSpec spec;
  spec.path = path;
  const auto res = load_chain_csv(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(Panel, Csv_TooFewColumns_ReturnsParseError) {
  const std::string path = temp_csv_path("atx_panel_shortrow.csv");
  {
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    out << "uid,snapshot_iso,spot,expiry_iso,strike,side,bid,ask,bid_size,ask_size,under_spot\n";
    out << "SPY,2026-06-19,400.0,2026-09-18\n";
  }

  CsvChainSpec spec;
  spec.path = path;
  const auto res = load_chain_csv(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(Panel, Csv_MissingFile_ReturnsIoError) {
  CsvChainSpec spec;
  spec.path = temp_csv_path("atx_panel_does_not_exist_9f3a.csv");
  const auto res = load_chain_csv(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::IoError);
}

// ── data_install's TimeSpec threading (I3: production T convention) ────────

// Friday 16:00 ET anchor (2026-07-10 20:00 UTC, EDT) -> Monday 10:00 ET expiry
// (2026-07-13 14:00 UTC): the intervening weekend is pure non-trading time, so
// the VolTime clock compresses T well below the plain Calendar365 fraction.
TEST(Panel, ChainCarriesVolTimeT) {
  QuoteFrame f;
  f.uid = "WKEND";
  f.snapshot_iso = "2026-07-10 20:00:00";
  f.snapshot_ts_ns = iso_to_ns(f.snapshot_iso);
  f.spot = 100.0;
  f.spot_ts_ns = f.snapshot_ts_ns;
  f.yc_pillar_t = {1.0};
  f.yc_pillar_r = {0.03};

  QuoteRow row;
  row.uid = "WKEND";
  row.expiry_iso = "2026-07-13 14:00:00";
  row.strike = 100.0;
  row.side = Side::Call;
  row.bid = 1.0;
  row.ask = 1.2;
  f.rows.push_back(row);

  TimeSpec spec;
  spec.convention = TimeConvention::VolTime;

  Universe u;
  const auto uid = atx::vol::data_install(u, f, spec);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();

  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{1});
  const Chain& c = (*under)->chains.front();

  const std::int64_t now_ns = iso_to_ns(f.snapshot_iso);
  const std::int64_t expiry_ns = iso_to_ns(row.expiry_iso);
  const double expected_vol_T =
      vol_time_years(now_ns, expiry_ns, VolTimeParams{}, VolTimeCalendar::us_default());
  EXPECT_GT(expected_vol_T, 0.0);
  EXPECT_DOUBLE_EQ(c.T, expected_vol_T);

  const double calendar_T = year_fraction(f.snapshot_iso, row.expiry_iso);
  EXPECT_LT(c.T, calendar_T);
}

// Same (frame, expiry) pair, default TimeSpec: chain.T must be bit-identical
// to the pre-I3 Calendar365 `year_fraction` result -- default-off, no
// behavior change for every caller that omits the TimeSpec argument.
TEST(Panel, ChainDefaultTimeSpecMatchesYearFraction) {
  QuoteFrame f;
  f.uid = "WKEND2";
  f.snapshot_iso = "2026-07-10 20:00:00";
  f.snapshot_ts_ns = iso_to_ns(f.snapshot_iso);
  f.spot = 100.0;
  f.spot_ts_ns = f.snapshot_ts_ns;
  f.yc_pillar_t = {1.0};
  f.yc_pillar_r = {0.03};

  QuoteRow row;
  row.uid = "WKEND2";
  row.expiry_iso = "2026-07-13 14:00:00";
  row.strike = 100.0;
  row.side = Side::Call;
  row.bid = 1.0;
  row.ask = 1.2;
  f.rows.push_back(row);

  Universe u;
  const auto uid = atx::vol::data_install(u, f); // no TimeSpec: default Calendar365
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();

  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  const Chain& c = (*under)->chains.front();

  EXPECT_EQ(c.T, year_fraction(f.snapshot_iso, row.expiry_iso));
}

} // namespace
