#include "atx/vol/api/marketdata/opra_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/io/parquet.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/vol/api/marketdata/data.hpp"      // year_fraction, find_expiry_inputs, ExpiryInputs
#include "atx/vol/api/pricing/rates_curve.hpp"     // YieldCurve
#include "atx/vol/api/core/vol_time.hpp"  // TimeSpec, TimeConvention, vol_time_years

// Loader/parser coverage for the OPRA cbbo-1m (NBBO) Parquet ingestion path.
//
//   parse_osi_symbol          -> ParseOsi_* (unit)
//   load_opra_cbbo_parquet    -> Load_* (round-trip over a synthetic fixture)

namespace {

namespace io = atx::core::io;
namespace fs = std::filesystem;
using atx::i64;
using atx::vol::ExerciseStyle;
using atx::vol::expiry_instant_ns;
using atx::vol::ExpiryCloseConvention;
using atx::vol::iso_to_ns;
using atx::vol::load_opra_cbbo_from_table;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::OpraLoadSpec;
using atx::vol::parse_osi_symbol;
using atx::vol::settlement_instant_ns;
using atx::vol::SettlementSession;
using atx::vol::Side;
using atx::vol::TimeConvention;
using atx::vol::TimeSpec;
using atx::vol::vol_time_years;
using atx::vol::VolTimeCalendar;
using atx::vol::VolTimeParams;
using atx::vol::year_fraction;
using atx::vol::YieldCurve;

// Calendar365 year-fraction from a pure-date snapshot to the TRUE PM-settled
// (16:00 ET) expiry instant — the T the OPRA loader / data_install now compute
// (G1 true expiry instants, gaps finding 3), replacing the legacy midnight-UTC
// `year_fraction(snap, iso)`. The +16:00-ET (i.e. +20h EDT / +21h EST) shift is
// THE FIX: it removes the ~0.8-trading-day front-T understatement.
[[nodiscard]] inline double pm_year_fraction(const std::string &snap_iso,
                                             const std::string &expiry_iso) {
  // Default `TimeSpec{}` is Calendar365, which reads no calendar and therefore
  // cannot hit the fail-closed coverage window (vol_time.hpp) — hence the
  // unconditional unwrap.
  return *atx::vol::time_to_expiry_years(iso_to_ns(snap_iso),
                                         expiry_instant_ns(expiry_iso, SettlementSession::Pm),
                                         TimeSpec{});
}

// ── Shared fixture builders (P2-2 / P2-3) ──────────────────────────────────

// One raw NBBO row: its underlying tag, OSI symbol, and dollar bid/ask.
struct RawRow {
  std::string underlying;
  std::string symbol;
  double bid;
  double ask;
  i64 bid_size{10};
  i64 ask_size{12};
};

// Compose an OSI/OCC 21-char symbol: 6-char space-padded root + YYMMDD + C/P +
// 8-digit strike (price x 1000).
[[nodiscard]] std::string osi_sym(std::string root, const std::string &yymmdd, char cp,
                                  double strike) {
  root.resize(6, ' ');
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08lld", static_cast<long long>(std::llround(strike * 1000.0)));
  return root + yymmdd + std::string(1, cp) + std::string(buf);
}

// Write a cbbo-1m slice (ts/[underlying]/symbol/bid_px/ask_px/bid_sz/ask_sz)
// from raw rows and return its path. `with_underlying=false` omits the column.
//
// `ts_ns` is the constant snapshot stamp written into the `ts` column. The
// default is the historical placeholder, which is fine for every case here that
// passes a bare `YYYY-MM-DD` snapshot_iso (the legacy midnight-UTC valuation
// convention -- FIX-C-1's guard deliberately does not arbitrate those). A case
// that passes a full INSTANT must pass the matching `ts_ns`: since FIX-C-1 the
// loader takes the snapshot instant FROM THE FILE and rejects a stamp that
// disagrees with it, exactly as a real hive file (one constant `ts` == the
// snapshot minute) requires.
[[nodiscard]] std::string write_slice(const std::string &name, const std::vector<RawRow> &rows,
                                      bool with_underlying = true,
                                      std::span<const i64> instrument_ids = {},
                                      i64 ts_ns = 1780000000000000000LL) {
  EXPECT_TRUE(instrument_ids.empty() || instrument_ids.size() == rows.size());
  const auto to_px = [](double d) { return static_cast<i64>(std::llround(d * 1e9)); };
  std::vector<i64> ts_col, bidpx, askpx, bidsz, asksz;
  std::vector<std::string> und_col, sym_col;
  for (const RawRow &rr : rows) {
    ts_col.push_back(ts_ns);
    und_col.push_back(rr.underlying);
    sym_col.push_back(rr.symbol);
    bidpx.push_back(to_px(rr.bid));
    askpx.push_back(to_px(rr.ask));
    bidsz.push_back(rr.bid_size);
    asksz.push_back(rr.ask_size);
  }
  std::vector<io::WriteColumn> cols;
  cols.push_back({"ts", std::span<const i64>(ts_col)});
  if (with_underlying) {
    cols.push_back({"underlying", std::span<const std::string>(und_col)});
  }
  cols.push_back({"symbol", std::span<const std::string>(sym_col)});
  if (!instrument_ids.empty()) {
    cols.push_back({"instrument_id", instrument_ids});
  }
  cols.push_back({"bid_px", std::span<const i64>(bidpx)});
  cols.push_back({"ask_px", std::span<const i64>(askpx)});
  cols.push_back({"bid_sz", std::span<const i64>(bidsz)});
  cols.push_back({"ask_sz", std::span<const i64>(asksz)});

  const fs::path dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::create_directories(dir);
  const fs::path path = dir / name;
  fs::remove(path);
  EXPECT_TRUE(io::write_parquet(cols, path.string()).has_value());
  return path.string();
}

// ── OSI symbol parser ───────────────────────────────────────────────────────

TEST(OpraPanel, ParseOsi_XomCall_ParsesRootExpiryStrike) {
  const auto r = parse_osi_symbol("XOM   260619C00110000");
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(r->root, "XOM");
  EXPECT_EQ(r->expiry_iso, "2026-06-19");
  EXPECT_TRUE(r->side == Side::Call);
  EXPECT_DOUBLE_EQ(r->strike, 110.0);
}

TEST(OpraPanel, ParseOsi_AaplPut_ParsesFractionalStrike) {
  const auto r = parse_osi_symbol("AAPL  270115P00250500");
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(r->root, "AAPL");
  EXPECT_EQ(r->expiry_iso, "2027-01-15");
  EXPECT_TRUE(r->side == Side::Put);
  EXPECT_DOUBLE_EQ(r->strike, 250.5);
}

TEST(OpraPanel, ParseOsi_TooShort_Rejected) {
  const auto r = parse_osi_symbol("XOM");
  EXPECT_FALSE(r.has_value());
}

// ── osi_root_matches_ticker: the ONE ticker <-> OSI-root identity rule ───────
//
// FIX-E (E2-b) collapsed two byte-identical copies of this predicate (this file's
// TU and listed_opra.cpp) into one shared function. The table below is the whole
// acceptance contract; it is here rather than in listed_opra_test.cpp because the
// declaration lives in opra_panel.hpp. The two existing behavioural pins in
// listed_opra_test.cpp (PunctuatedTickerJoinsAgainstItsDotStrippedOsiRoot,
// MissingNumericRootAdjustmentsAreExcluded) are the end-to-end regression gate
// for the same rule and must keep passing UNCHANGED.
TEST(OpraPanel, OsiRootMatchesTicker_ToleratesDotsAndNothingElse) {
  using atx::vol::osi_root_matches_ticker;

  // Identity: no punctuation to reconcile.
  EXPECT_TRUE(osi_root_matches_ticker("AAPL", "AAPL"));
  EXPECT_TRUE(osi_root_matches_ticker("SPY", "SPY"));
  // The encoding artifact this relaxation exists for. The OSI root namespace
  // cannot express '.', so these are two spellings of ONE identity.
  EXPECT_TRUE(osi_root_matches_ticker("BRKB", "BRK.B"));
  EXPECT_TRUE(osi_root_matches_ticker("BFB", "BF.B"));

  // ASYMMETRY, pinned deliberately rather than fixed: dots are skipped in the
  // TICKER only, never in the ROOT. So a root that itself carries a dot does NOT
  // match its identically-spelled ticker -- this predicate is not reflexive over
  // dotted strings. That is the behaviour both merged copies always had, and it
  // is sound because the premise of the whole rule is that the OSI root namespace
  // CANNOT express punctuation: a dotted root is not a thing OPRA emits. It is
  // pinned here so a future reader meets the sharp edge in a test rather than in
  // production, and so a change to it has to be deliberate.
  EXPECT_FALSE(osi_root_matches_ticker("BRK.B", "BRK.B"));

  // Anything OTHER than punctuation is two different underliers.
  EXPECT_FALSE(osi_root_matches_ticker("BRKC", "BRK.B"));
  EXPECT_FALSE(osi_root_matches_ticker("GEX", "GE"));
  EXPECT_FALSE(osi_root_matches_ticker("GE", "GEX"));
  EXPECT_FALSE(osi_root_matches_ticker("AAPL", "MSFT"));
  // Prefix relationships are not matches in either direction: the walk must
  // CONSUME the root exactly, not merely start it.
  EXPECT_FALSE(osi_root_matches_ticker("AAP", "AAPL"));
  EXPECT_FALSE(osi_root_matches_ticker("AAPLX", "AAPL"));

  // Empty inputs: an empty ticker consumes nothing, so only an empty root
  // matches. (Both callers guard emptiness before asking, but the predicate must
  // still be total.)
  EXPECT_TRUE(osi_root_matches_ticker("", ""));
  EXPECT_FALSE(osi_root_matches_ticker("AAPL", ""));
  EXPECT_FALSE(osi_root_matches_ticker("", "AAPL"));
}

// THE NEGATIVE GATE. Do not delete this test, and do not "fix" it by widening the
// predicate.
//
// `pull_opra_hive.py` strips a TRAILING DIGIT from the OSI root before mapping it
// to a universe symbol, so an adjusted `AAPL1` row can reach disk carrying
// `underlying = "AAPL"`. The C++ rule refuses that pair on purpose: after a
// corporate action an `AAPL1` contract's deliverable is not 100 shares of AAPL,
// so its options are not comparable to the vanilla chain at the same strike, and
// the fitter has no deliverable model with which to tell them apart. Accepting it
// would silently MERGE two instruments into one chain — a mispricing, not a lost
// symbol — instead of failing loud in opra_panel.cpp's per-row identity guard.
//
// The strict-consumer / loose-producer asymmetry is therefore DELIBERATE. A
// future cleanup that unifies the C++ predicate onto the Python producers' looser
// rule reintroduces that defect, and this assertion is what stops it.
TEST(OpraPanel, OsiRootMatchesTicker_TrailingDigitIsADifferentInstrumentNotPunctuation) {
  using atx::vol::osi_root_matches_ticker;

  EXPECT_FALSE(osi_root_matches_ticker("AAPL1", "AAPL"))
      << "an adjusted deliverable must never normalise onto its vanilla ticker";
  EXPECT_FALSE(osi_root_matches_ticker("SPY1", "SPY"));
  EXPECT_FALSE(osi_root_matches_ticker("BRKB1", "BRK.B"))
      << "the dot relaxation must not also swallow a trailing digit";
  // ...and the adjusted root is still a perfectly good identity for ITSELF.
  EXPECT_TRUE(osi_root_matches_ticker("AAPL1", "AAPL1"));
  // Digits are not special-cased away anywhere else either: a ticker that
  // genuinely ends in a digit still has to match exactly.
  EXPECT_FALSE(osi_root_matches_ticker("AAPL", "AAPL1"));
}

// The definitions exporter's universe filter (FIX-E, E2-a), exercised through the
// shared predicate rather than the example TU — which is gated behind
// ATX_BUILD_EXAMPLES=OFF and has no test target.
//
// The exporter used to compare `osi->root` against `config.symbols` with a
// byte-exact `std::find`, while its own `parent_symbol` strips dots to build the
// Databento request. So it asked for `BRKB.OPT`, got back roots spelled `BRKB`,
// and rejected every one of them against the universe's `BRK.B` — paying for data
// it then discarded, which is why BRK.B was silently absent from every dispersion
// basket. This is that filter's acceptance table.
TEST(OpraPanel, OsiRootMatchesTicker_DefinitionsUniverseFilterAcceptsPunctuatedTickers) {
  using atx::vol::osi_root_matches_ticker;
  const std::vector<std::string> universe = {"SPY", "AAPL", "BRK.B"};
  // Mirrors the exporter's filter exactly, including the retained exact compare
  // that keeps the change a STRICT relaxation of the old byte-exact `std::find`.
  const auto in_universe = [&universe](std::string_view root) {
    return std::any_of(universe.begin(), universe.end(), [root](const std::string &symbol) {
      return symbol == root || osi_root_matches_ticker(root, symbol);
    });
  };

  EXPECT_TRUE(in_universe("BRKB")) << "the defect: every BRK.B definition was rejected";
  EXPECT_TRUE(in_universe("SPY"));
  EXPECT_TRUE(in_universe("AAPL"));
  EXPECT_FALSE(in_universe("MSFT")) << "an out-of-universe root is still rejected";
  EXPECT_FALSE(in_universe("AAPL1"))
      << "the relaxation is punctuation-only: an adjusted deliverable stays out";
  EXPECT_FALSE(in_universe("BRKA")) << "the sibling class share is a different name";
}

// ── Loader round-trip ───────────────────────────────────────────────────────

// Build a synthetic XOM cbbo-1m slice: two expiries x three strikes x
// {call, put}, with per-strike mids planted so C - P = e^{-rT}(F - K) exactly
// (so every strike implies the same forward F), plus one unset-price row and
// one crossed row that must be dropped. Round-trip it through write_parquet and
// assert the loader's counts and PCP-implied spot.
TEST(OpraPanel, Load_SyntheticXomSlice_CountsAndImpliedSpot) {
  const double r = 0.04;
  const std::string snap = "2026-05-01";

  struct Expiry {
    std::string iso;
    std::string yymmdd;
    double fwd;
  };
  const std::vector<Expiry> expiries = {
      {"2026-06-19", "260619", 111.0}, // front (earliest) -> drives implied spot
      {"2026-09-18", "260918", 112.0},
  };
  const std::vector<double> strikes = {105.0, 110.0, 115.0};

  // Backing storage for the borrowed WriteColumn spans (must outlive the write).
  std::vector<i64> ts_col;
  std::vector<i64> bidpx;
  std::vector<i64> askpx;
  std::vector<i64> bidsz;
  std::vector<i64> asksz;
  std::vector<std::string> und_col;
  std::vector<std::string> sym_col;

  const auto pad_root = [](std::string root) {
    root.resize(6, ' ');
    return root;
  };
  const auto strike_field = [](double k) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08lld", static_cast<long long>(std::llround(k * 1000.0)));
    return std::string(buf);
  };
  const auto to_px = [](double dollars) { return static_cast<i64>(std::llround(dollars * 1e9)); };

  const i64 ts_val = 1780000000000000000LL;
  const auto add_row = [&](const std::string &sym, i64 bpx, i64 apx) {
    ts_col.push_back(ts_val);
    und_col.emplace_back("XOM");
    sym_col.push_back(sym);
    bidpx.push_back(bpx);
    askpx.push_back(apx);
    bidsz.push_back(10);
    asksz.push_back(12);
  };

  for (const Expiry &e : expiries) {
    // Plant mids under the SAME true-instant T the loader now back-solves at
    // (G1): C - P = e^{-r*T_true}(F - K), so the loader recovers F exactly.
    const double t = pm_year_fraction(snap, e.iso);
    const double df = std::exp(-r * t);
    for (const double k : strikes) {
      const double diff = df * (e.fwd - k);
      const double put_mid = 5.0;
      const double call_mid = put_mid + diff;
      add_row(pad_root("XOM") + e.yymmdd + "C" + strike_field(k), to_px(call_mid - 0.05),
              to_px(call_mid + 0.05));
      add_row(pad_root("XOM") + e.yymmdd + "P" + strike_field(k), to_px(put_mid - 0.05),
              to_px(put_mid + 0.05));
    }
  }
  // T6: KEPT as a one-sided quote (unset bid, real ask) — the ask bounds the
  // contract's price from above, so the row is admitted with bid = 0.
  add_row(pad_root("XOM") + "260619C00120000", std::numeric_limits<i64>::min(), to_px(1.0));
  // Dropped: crossed quote (bid > ask).
  add_row(pad_root("XOM") + "260619P00120000", to_px(5.0), to_px(4.0));
  // Dropped: unset ASK. A bid alone bounds nothing from above.
  add_row(pad_root("XOM") + "260619C00125000", to_px(1.0), std::numeric_limits<i64>::min());

  const std::vector<io::WriteColumn> cols = {
      {"ts", std::span<const i64>(ts_col)},
      {"underlying", std::span<const std::string>(und_col)},
      {"symbol", std::span<const std::string>(sym_col)},
      {"bid_px", std::span<const i64>(bidpx)},
      {"ask_px", std::span<const i64>(askpx)},
      {"bid_sz", std::span<const i64>(bidsz)},
      {"ask_sz", std::span<const i64>(asksz)},
  };

  const fs::path dir = fs::temp_directory_path() / "atx_opra_panel_test";
  fs::create_directories(dir);
  const fs::path path = dir / "xom_cbbo.parquet";
  fs::remove(path);
  ASSERT_TRUE(io::write_parquet(cols, path.string()).has_value());

  OpraLoadSpec spec;
  spec.path = path.string();
  spec.underlying = "XOM";
  spec.snapshot_iso = snap;
  spec.r = r;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();

  EXPECT_EQ(loaded->n_contracts, std::size_t{13});
  EXPECT_EQ(loaded->n_expiries, std::size_t{2});
  EXPECT_EQ(loaded->n_dropped, std::size_t{2}); // the crossed put + the ask-less call
  EXPECT_EQ(loaded->n_one_sided, std::size_t{1});
  EXPECT_EQ(loaded->frame.uid, "XOM");
  EXPECT_EQ(loaded->frame.rows.size(), std::size_t{13});
  EXPECT_TRUE(loaded->bid_size_available);
  EXPECT_TRUE(loaded->ask_size_available);
  EXPECT_FALSE(loaded->volume_available);
  EXPECT_FALSE(loaded->open_interest_available);

  // The crossed 120 PUT and the ask-less 125 call stay out; the bid-less 120
  // CALL enters as a bound, carrying bid = 0 and no bid size so every consumer
  // that requires a two-sided market still refuses it.
  std::size_t n_120_call = 0;
  for (const auto &row : loaded->frame.rows) {
    EXPECT_NE(row.strike, 125.0);
    if (row.strike == 120.0) {
      EXPECT_EQ(row.side, atx::vol::Side::Call);
      EXPECT_DOUBLE_EQ(row.bid, 0.0);
      EXPECT_GT(row.ask, 0.0);
      EXPECT_EQ(row.bid_size, 0);
      ++n_120_call;
    }
  }
  EXPECT_EQ(n_120_call, std::size_t{1});

  // And the load is byte-for-byte the historical one when the caller refuses
  // one-sided quotes, so nothing about this change is implicit.
  OpraLoadSpec strict = spec;
  strict.admit_one_sided_quotes = false;
  const auto two_sided_only = load_opra_cbbo_parquet(strict);
  ASSERT_TRUE(two_sided_only.has_value()) << two_sided_only.error().to_string();
  EXPECT_EQ(two_sided_only->n_contracts, std::size_t{12});
  EXPECT_EQ(two_sided_only->n_dropped, std::size_t{3});
  EXPECT_EQ(two_sided_only->n_one_sided, std::size_t{0});
  for (const auto &row : two_sided_only->frame.rows) {
    EXPECT_NE(row.strike, 120.0);
  }

  // Front expiry forward is planted at 111.0; implied spot = F * exp(-r*T) at the
  // TRUE 16:00 ET expiry instant (G1) the loader now discounts at.
  const double t_front = pm_year_fraction(snap, "2026-06-19");
  const double expected_spot = 111.0 * std::exp(-r * t_front);
  EXPECT_NEAR(loaded->implied_spot, expected_spot, 1e-3);
  EXPECT_DOUBLE_EQ(loaded->frame.spot, loaded->implied_spot);

  fs::remove_all(dir);
}

// W1-B (F35): the PCP anchor is chosen by argmin |call_mid - put_mid|, so a leg
// whose bid is zero drags its stored mid toward the other side's and is picked
// *preferentially* — the spot the whole pipeline is built on can be anchored on
// the most defective pair on the board. The anchor must require both legs
// strictly two-sided.
TEST(OpraPanel, ImpliedSpot_IgnoresAZeroBidPairWhenChoosingThePcpAnchor) {
  const double r = 0.04;
  const std::string snap = "2026-05-01";
  const double t = pm_year_fraction(snap, "2026-06-19");
  const double df = std::exp(-r * t);
  constexpr double kForward = 111.0;

  std::vector<RawRow> rows;
  const auto add_pair = [&](double strike, double put_mid) {
    const double call_mid = put_mid + df * (kForward - strike);
    rows.push_back(
        RawRow{"XOM", osi_sym("XOM", "260619", 'C', strike), call_mid - 0.05, call_mid + 0.05});
    rows.push_back(
        RawRow{"XOM", osi_sym("XOM", "260619", 'P', strike), put_mid - 0.05, put_mid + 0.05});
  };
  // Honest pairs whose |C - P| gap is large.
  add_pair(105.0, 5.0);
  add_pair(115.0, 5.0);
  // The defective pair: the put has NO bid (a literal zero), which halves its
  // stored mid and makes |C - P| the smallest on the board. The forward it
  // implies is wrong by construction — an ask alone is not a price.
  rows.push_back(RawRow{"XOM", osi_sym("XOM", "260619", 'C', 110.0), 7.95, 8.05});
  rows.push_back(RawRow{"XOM", osi_sym("XOM", "260619", 'P', 110.0), 0.0, 16.1});

  const std::string path = write_slice("pcp_zero_bid_anchor.parquet", rows);
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = snap;
  spec.r = r;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_NEAR(loaded->implied_spot, kForward * std::exp(-r * t), 1e-3);
}

// The GNK/ATAI shape: no strike carries a two-sided call AND a two-sided put, so
// no co-terminal PCP pair exists at all. That is a refusal — but it must SAY
// that, rather than "no well-conditioned expiry", which reads as a conditioning
// problem an operator could fix by widening a tolerance.
TEST(OpraPanel, ImpliedSpot_RefusalNamesTheMissingTwoSidedCoTerminalPair) {
  std::vector<RawRow> rows;
  for (const double strike : {105.0, 110.0, 115.0}) {
    rows.push_back(RawRow{"XOM", osi_sym("XOM", "260619", 'C', strike), 2.0, 2.4});
    rows.push_back(RawRow{"XOM", osi_sym("XOM", "260619", 'P', strike + 2.5), 3.0, 3.4});
  }
  const std::string path = write_slice("pcp_no_pair.parquet", rows);
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.r = 0.04;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_NE(loaded.error().message().find("two-sided"), std::string::npos)
      << loaded.error().message();
}

TEST(OpraPanel, Load_RejectsDisplayedSizeBeforeInt32Narrowing) {
  const i64 too_large = static_cast<i64>((std::numeric_limits<std::int32_t>::max)()) + 1;
  const std::vector<RawRow> rows{RawRow{"XOM", "XOM   260619C00110000", 2.0, 2.2, too_large, 12}};
  const std::string path = write_slice("oversized_quote_size.parquet", rows);
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.spot_override = 100.0;

  const auto loaded = load_opra_cbbo_parquet(spec);

  ASSERT_FALSE(loaded);
  EXPECT_EQ(loaded.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(OpraPanel, Load_SpotOverride_UsesOverrideNotPcp) {
  // A one-strike slice has no need for PCP: spot_override wins outright.
  std::vector<i64> ts_col = {1780000000000000000LL, 1780000000000000000LL};
  std::vector<std::string> und_col = {"XOM", "XOM"};
  std::vector<std::string> sym_col = {"XOM   260619C00110000", "XOM   260619P00110000"};
  std::vector<i64> bidpx = {static_cast<i64>(std::llround(2.0 * 1e9)),
                            static_cast<i64>(std::llround(1.0 * 1e9))};
  std::vector<i64> askpx = {static_cast<i64>(std::llround(2.2 * 1e9)),
                            static_cast<i64>(std::llround(1.2 * 1e9))};
  std::vector<i64> bidsz = {10, 10};
  std::vector<i64> asksz = {12, 12};

  const std::vector<io::WriteColumn> cols = {
      {"ts", std::span<const i64>(ts_col)},
      {"underlying", std::span<const std::string>(und_col)},
      {"symbol", std::span<const std::string>(sym_col)},
      {"bid_px", std::span<const i64>(bidpx)},
      {"ask_px", std::span<const i64>(askpx)},
      {"bid_sz", std::span<const i64>(bidsz)},
      {"ask_sz", std::span<const i64>(asksz)},
  };

  const fs::path dir = fs::temp_directory_path() / "atx_opra_panel_override_test";
  fs::create_directories(dir);
  const fs::path path = dir / "xom_one.parquet";
  fs::remove(path);
  ASSERT_TRUE(io::write_parquet(cols, path.string()).has_value());

  OpraLoadSpec spec;
  spec.path = path.string();
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.r = 0.04;
  spec.spot_override = 123.45;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_DOUBLE_EQ(loaded->implied_spot, 123.45);
  EXPECT_DOUBLE_EQ(loaded->frame.spot, 123.45);
  EXPECT_EQ(loaded->n_contracts, std::size_t{2});

  fs::remove_all(dir);
}

// Regression: the `ts` column may arrive as a real Arrow TIMESTAMP (this is how
// the databento OPRA puller writes it), not the Int64(ns) the other fixtures use.
// first_ts_ns() must read it through to_column<Timestamp> — column_view<Timestamp>
// is a rejected specialization that ALWAYS errors, which previously left every
// real snapshot_ts_ns == 0 (collapsing backtest aging dt -> 0, zeroing theta PnL).
// With no snapshot_iso override, snapshot_ts_ns is derived from the ts column, so
// this exercises exactly that path.
TEST(OpraPanel, Load_TimestampTsColumn_PopulatesSnapshotNs) {
  using atx::core::time::Timestamp;
  const i64 kSnapNs = 1780000000000000000LL; // round (ns == us == ms aligned) -> unit-agnostic
  std::vector<Timestamp> ts_col = {Timestamp::from_unix_nanos(kSnapNs),
                                   Timestamp::from_unix_nanos(kSnapNs)};
  std::vector<std::string> und_col = {"XOM", "XOM"};
  std::vector<std::string> sym_col = {"XOM   260619C00110000", "XOM   260619P00110000"};
  std::vector<i64> bidpx = {static_cast<i64>(std::llround(2.0 * 1e9)),
                            static_cast<i64>(std::llround(1.0 * 1e9))};
  std::vector<i64> askpx = {static_cast<i64>(std::llround(2.2 * 1e9)),
                            static_cast<i64>(std::llround(1.2 * 1e9))};
  std::vector<i64> bidsz = {10, 10};
  std::vector<i64> asksz = {12, 12};

  const std::vector<io::WriteColumn> cols = {
      {"ts", std::span<const Timestamp>(ts_col)},
      {"underlying", std::span<const std::string>(und_col)},
      {"symbol", std::span<const std::string>(sym_col)},
      {"bid_px", std::span<const i64>(bidpx)},
      {"ask_px", std::span<const i64>(askpx)},
      {"bid_sz", std::span<const i64>(bidsz)},
      {"ask_sz", std::span<const i64>(asksz)},
  };

  const fs::path dir = fs::temp_directory_path() / "atx_opra_panel_tsts_test";
  fs::create_directories(dir);
  const fs::path path = dir / "xom_ts.parquet";
  fs::remove(path);
  ASSERT_TRUE(io::write_parquet(cols, path.string()).has_value());

  OpraLoadSpec spec;
  spec.path = path.string();
  spec.underlying = "XOM";
  spec.r = 0.04;
  spec.spot_override = 123.45; // one strike/pair -> no PCP needed
  // NOTE: snapshot_iso intentionally left empty so snapshot_ts_ns comes from `ts`.

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(loaded->frame.snapshot_ts_ns, kSnapNs); // the bug returned 0 here

  fs::remove_all(dir);
}

TEST(OpraPanel, Load_MissingFile_ReturnsInvalidArgument) {
  OpraLoadSpec spec;
  spec.path = (fs::temp_directory_path() / "atx_opra_does_not_exist.parquet").string();
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(OpraPanel, LegacyFileIsCompatibleButStrictModeRejectsMissingIdentity) {
  const std::vector<RawRow> rows = {
      {"XOM", osi_sym("XOM", "260918", 'C', 110.0), 5.0, 5.1},
      {"XOM", osi_sym("XOM", "260918", 'P', 110.0), 4.0, 4.1},
  };
  const std::string path = write_slice("legacy_identity.parquet", rows);
  OpraLoadSpec compatible;
  compatible.path = path;
  compatible.underlying = "XOM";
  compatible.snapshot_iso = "2026-05-01";
  compatible.spot_override = 110.0;
  const auto legacy = load_opra_cbbo_parquet(compatible);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  EXPECT_EQ(legacy->source_schema_version, 1u);
  EXPECT_FALSE(legacy->provenance_complete);
  EXPECT_EQ(legacy->source_instrument_ids, std::vector<std::uint32_t>({0u, 0u}));

  compatible.provenance_mode = atx::vol::OpraProvenanceMode::Strict;
  const auto strict = load_opra_cbbo_parquet(compatible);
  ASSERT_FALSE(strict.has_value());
  EXPECT_EQ(strict.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(OpraPanel, StrictIdentityPreservesAlignedIdsAndDeduplicatesRawSymbols) {
  const std::vector<RawRow> rows = {
      {"XOM", osi_sym("XOM", "260918", 'C', 110.0), 5.0, 5.1},
      {"XOM", osi_sym("XOM", "260918", 'P', 110.0), 4.0, 4.1},
      {"XOM", osi_sym("XOM", "260918", 'C', 110.0), 5.0, 5.1},
  };
  const std::vector<i64> ids = {101, 102, 101};
  const std::string path = write_slice("strict_identity.parquet", rows, true, ids);
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.spot_override = 110.0;
  spec.provenance_mode = atx::vol::OpraProvenanceMode::Strict;
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(loaded->source_schema_version, 2u);
  EXPECT_TRUE(loaded->provenance_complete);
  EXPECT_EQ(loaded->source_instrument_ids, std::vector<std::uint32_t>({101u, 102u, 101u}));
  ASSERT_EQ(loaded->source_identities.size(), 2u);
  EXPECT_EQ(loaded->source_identities[0].instrument_id, 101u);
  EXPECT_EQ(loaded->source_identities[1].instrument_id, 102u);
}

TEST(OpraPanel, SameDateInstrumentIdMappingToTwoRawSymbolsIsRejected) {
  const std::vector<RawRow> rows = {
      {"XOM", osi_sym("XOM", "260918", 'C', 110.0), 5.0, 5.1},
      {"XOM", osi_sym("XOM", "260918", 'P', 110.0), 4.0, 4.1},
  };
  const std::vector<i64> ids = {777, 777};
  const std::string path = write_slice("ambiguous_identity.parquet", rows, true, ids);
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.spot_override = 110.0;
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), atx::vol::ErrorCode::InvalidArgument);
  EXPECT_NE(loaded.error().to_string().find("ambiguous instrument_id"), std::string::npos);
}

TEST(OpraPanel, InstrumentIdReuseAcrossDatesIsDateScopedAndLegal) {
  const std::vector<i64> ids = {404};
  const std::vector<RawRow> first = {
      {"XOM", osi_sym("XOM", "260918", 'C', 110.0), 5.0, 5.1},
  };
  const std::vector<RawRow> second = {
      {"XOM", osi_sym("XOM", "261218", 'C', 110.0), 6.0, 6.1},
  };
  OpraLoadSpec spec;
  spec.underlying = "XOM";
  spec.spot_override = 110.0;
  spec.provenance_mode = atx::vol::OpraProvenanceMode::Strict;

  spec.path = write_slice("identity_date_one.parquet", first, true, ids);
  spec.snapshot_iso = "2026-05-01";
  const auto date_one = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(date_one.has_value()) << date_one.error().to_string();

  spec.path = write_slice("identity_date_two.parquet", second, true, ids);
  spec.snapshot_iso = "2026-05-02";
  const auto date_two = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(date_two.has_value()) << date_two.error().to_string();
  EXPECT_EQ(date_one->source_identities.front().instrument_id,
            date_two->source_identities.front().instrument_id);
  EXPECT_NE(date_one->source_identities.front().raw_symbol,
            date_two->source_identities.front().raw_symbol);
}

// ── P2-2 multi-symbol validation ────────────────────────────────────────────

// A parquet carrying two distinct underlyings, each with a well-conditioned
// co-terminal call/put pair. Reused across the mixed-symbol cases.
[[nodiscard]] std::vector<RawRow> mixed_xom_aapl_rows() {
  return {
      {"XOM", osi_sym("XOM", "260918", 'C', 110.0), 5.00, 5.10},
      {"XOM", osi_sym("XOM", "260918", 'P', 110.0), 4.00, 4.10},
      {"AAPL", osi_sym("AAPL", "260918", 'C', 250.0), 6.00, 6.10},
      {"AAPL", osi_sym("AAPL", "260918", 'P', 250.0), 5.00, 5.10},
  };
}

TEST(OpraPanel, MixedSymbol_EmptyFilter_Rejected) {
  const std::string path = write_slice("mixed_empty.parquet", mixed_xom_aapl_rows());
  OpraLoadSpec spec;
  spec.path = path;
  spec.snapshot_iso = "2026-05-01";
  spec.r = 0.04;
  // Empty underlying over a 2-symbol parquet is ambiguous.
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), atx::vol::ErrorCode::InvalidArgument);
  EXPECT_NE(loaded.error().to_string().find("mixed-symbol"), std::string::npos)
      << loaded.error().to_string();
}

TEST(OpraPanel, MixedSymbol_FilterSelectsOne) {
  const std::string path = write_slice("mixed_select.parquet", mixed_xom_aapl_rows());
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.r = 0.04;
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(loaded->frame.uid, "XOM");
  EXPECT_EQ(loaded->n_contracts, std::size_t{2}); // only the two XOM legs
  EXPECT_EQ(loaded->n_expiries, std::size_t{1});
  for (const auto &row : loaded->frame.rows) {
    EXPECT_EQ(row.uid, "XOM"); // no AAPL leaked through
  }
}

TEST(OpraPanel, FilterButNoUnderlyingColumn_Rejected) {
  // Same rows, but the parquet omits the `underlying` column entirely.
  const std::string path =
      write_slice("no_und_col.parquet", mixed_xom_aapl_rows(), /*with_underlying=*/false);
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.r = 0.04;
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), atx::vol::ErrorCode::InvalidArgument);
  EXPECT_NE(loaded.error().to_string().find("no 'underlying' column"), std::string::npos)
      << loaded.error().to_string();
}

TEST(OpraPanel, FilterSymbolNotFound_Rejected) {
  const std::string path = write_slice("not_found.parquet", mixed_xom_aapl_rows());
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "ZZZ"; // present column, but no such symbol
  spec.snapshot_iso = "2026-05-01";
  spec.r = 0.04;
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), atx::vol::ErrorCode::InvalidArgument);
  EXPECT_NE(loaded.error().to_string().find("not found"), std::string::npos)
      << loaded.error().to_string();
}

// ── P2-3 term-structure yield curve ─────────────────────────────────────────

// A single-symbol XOM parquet with a short and a long expiry, each carrying a
// co-terminal call/put pair so both expiries surface in the frame.
[[nodiscard]] std::vector<RawRow> xom_two_expiry_rows() {
  return {
      {"XOM", osi_sym("XOM", "260801", 'C', 110.0), 3.00, 3.10}, // short (~0.25y)
      {"XOM", osi_sym("XOM", "260801", 'P', 110.0), 2.00, 2.10},
      {"XOM", osi_sym("XOM", "271101", 'C', 110.0), 8.00, 8.10}, // long  (~1.50y)
      {"XOM", osi_sym("XOM", "271101", 'P', 110.0), 7.00, 7.10},
  };
}

// ── Task 1: in-memory table seam ────────────────────────────────────────────
//
// load_opra_cbbo_from_table must produce a BYTE-IDENTICAL panel to
// load_opra_cbbo_parquet when handed a table holding the same rows the file
// path would read. The fixture carries the full 8-column OPRA schema the seam
// requires; the file loader reads it via projection, the seam reads the same
// file fully — same rows in, identical panel out.
TEST(OpraPanelTable, TableSeamMatchesFileLoad) {
  const std::vector<i64> ids = {201, 202, 203, 204};
  const std::string path = write_slice("table_seam.parquet", xom_two_expiry_rows(),
                                       /*with_underlying=*/true, ids);

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  spec.r = 0.04;

  const auto file_panel = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(file_panel.has_value()) << file_panel.error().to_string();

  const auto table = io::read_parquet(spec.path);
  ASSERT_TRUE(table.has_value()) << table.error().to_string();

  const auto tbl_panel = load_opra_cbbo_from_table(*table, spec);
  ASSERT_TRUE(tbl_panel.has_value()) << tbl_panel.error().to_string();

  // Cheap identities: counts, spot, snapshot stamp, uid, fingerprint, provenance.
  EXPECT_EQ(tbl_panel->frame.rows.size(), file_panel->frame.rows.size());
  EXPECT_EQ(tbl_panel->n_contracts, file_panel->n_contracts);
  EXPECT_EQ(tbl_panel->n_expiries, file_panel->n_expiries);
  EXPECT_EQ(tbl_panel->n_dropped, file_panel->n_dropped);
  EXPECT_DOUBLE_EQ(tbl_panel->implied_spot, file_panel->implied_spot);
  EXPECT_DOUBLE_EQ(tbl_panel->frame.spot, file_panel->frame.spot);
  EXPECT_EQ(tbl_panel->frame.snapshot_ts_ns, file_panel->frame.snapshot_ts_ns);
  EXPECT_EQ(tbl_panel->snapshot_iso, file_panel->snapshot_iso);
  EXPECT_EQ(tbl_panel->frame.uid, file_panel->frame.uid);
  EXPECT_EQ(tbl_panel->source_schema_version, file_panel->source_schema_version);
  EXPECT_EQ(tbl_panel->source_fingerprint, file_panel->source_fingerprint);
  EXPECT_EQ(tbl_panel->source_instrument_ids, file_panel->source_instrument_ids);

  // First/last kept-quote fields match position-for-position.
  ASSERT_FALSE(file_panel->frame.rows.empty());
  const auto &tf = tbl_panel->frame.rows.front();
  const auto &ff = file_panel->frame.rows.front();
  EXPECT_EQ(tf.uid, ff.uid);
  EXPECT_EQ(tf.expiry_iso, ff.expiry_iso);
  EXPECT_TRUE(tf.side == ff.side);
  EXPECT_DOUBLE_EQ(tf.strike, ff.strike);
  EXPECT_DOUBLE_EQ(tf.bid, ff.bid);
  EXPECT_DOUBLE_EQ(tf.ask, ff.ask);
  const auto &tl = tbl_panel->frame.rows.back();
  const auto &fl = file_panel->frame.rows.back();
  EXPECT_EQ(tl.expiry_iso, fl.expiry_iso);
  EXPECT_DOUBLE_EQ(tl.strike, fl.strike);
  EXPECT_DOUBLE_EQ(tl.bid, fl.bid);
  EXPECT_DOUBLE_EQ(tl.ask, fl.ask);

  const fs::path dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::remove_all(dir);
}

TEST(OpraPanel, TermCurve_PerExpiryRateInterpolated) {
  const std::string snap = "2026-05-01";
  const std::string path = write_slice("term_curve.parquet", xom_two_expiry_rows());

  // A materially-sloped curve: 2% short, 6% long, bracketing both expiries.
  const std::vector<double> pt = {0.1, 2.0};
  const std::vector<double> pr = {0.02, 0.06};

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "XOM";
  spec.snapshot_iso = snap;
  spec.r = 0.99; // must be ignored: the term pillars drive every rate
  spec.yc_pillar_t = pt;
  spec.yc_pillar_r = pr;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();

  // The frame carries the caller's pillars verbatim (not the flat {1.0}).
  ASSERT_EQ(loaded->frame.yc_pillar_t, pt);
  ASSERT_EQ(loaded->frame.yc_pillar_r, pr);

  // Independently-built reference curve: the loader must route each expiry's
  // rate through YieldCurve::zero at that expiry's own year-fraction.
  const auto yc = YieldCurve::create(std::span<const double>(pt), std::span<const double>(pr));
  ASSERT_TRUE(yc.has_value()) << yc.error().to_string();
  // Per-expiry rate is stamped at the TRUE 16:00 ET expiry instant (G1).
  const double t_short = pm_year_fraction(snap, "2026-08-01");
  const double t_long = pm_year_fraction(snap, "2027-11-01");

  const auto *cell_short = atx::vol::find_expiry_inputs(loaded->frame, "XOM", "2026-08-01");
  const auto *cell_long = atx::vol::find_expiry_inputs(loaded->frame, "XOM", "2027-11-01");
  ASSERT_NE(cell_short, nullptr);
  ASSERT_NE(cell_long, nullptr);

  // Hand-checked: the per-expiry source rate equals the monotone-Hermite curve
  // rate at each maturity, bit-for-bit.
  EXPECT_DOUBLE_EQ(cell_short->rate, yc->zero(t_short));
  EXPECT_DOUBLE_EQ(cell_long->rate, yc->zero(t_long));
  EXPECT_TRUE(atx::vol::has_flag(cell_short->completeness, atx::vol::ExpiryInputField::Rate));
  EXPECT_TRUE(atx::vol::has_flag(cell_long->completeness, atx::vol::ExpiryInputField::Rate));

  // The two expiries genuinely see DIFFERENT interpolated rates (term structure
  // is live, not collapsed to a single flat number).
  EXPECT_LT(cell_short->rate, cell_long->rate);
  EXPECT_GT(cell_long->rate - cell_short->rate, 0.01);
}

TEST(OpraPanel, SinglePillar_EqualsFlatRate_BitIdentical) {
  const std::string path = write_slice("single_pillar.parquet", mixed_xom_aapl_rows());
  const double r = 0.05;

  // (A) The historical flat path: no pillars, scalar spec.r.
  OpraLoadSpec flat;
  flat.path = path;
  flat.underlying = "XOM";
  flat.snapshot_iso = "2026-05-01";
  flat.r = r;
  const auto a = load_opra_cbbo_parquet(flat);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();

  // (B) A single-pillar "curve" at the same rate. Must reduce to flat r.
  OpraLoadSpec one;
  one.path = path;
  one.underlying = "XOM";
  one.snapshot_iso = "2026-05-01";
  one.r = 0.99; // ignored: the sole pillar's rate is the flat rate
  one.yc_pillar_t = {1.0};
  one.yc_pillar_r = {r};
  const auto b = load_opra_cbbo_parquet(one);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();

  // Bit-for-bit identical implied spot: a 1-pillar curve is NOT routed through
  // the (non-flat-interpolating) YieldCurve; it uses the scalar rate directly.
  EXPECT_DOUBLE_EQ(a->implied_spot, b->implied_spot);

  // (B) stamped the flat rate onto the expiry cell; (A) left it untouched.
  const auto *cell_b = atx::vol::find_expiry_inputs(b->frame, "XOM", "2026-09-18");
  ASSERT_NE(cell_b, nullptr);
  EXPECT_DOUBLE_EQ(cell_b->rate, r);
  EXPECT_TRUE(atx::vol::has_flag(cell_b->completeness, atx::vol::ExpiryInputField::Rate));
  const std::vector<double> one_t = {1.0};
  const std::vector<double> one_r = {r};
  EXPECT_EQ(b->frame.yc_pillar_t, one_t);
  EXPECT_EQ(b->frame.yc_pillar_r, one_r);

  const auto *cell_a = atx::vol::find_expiry_inputs(a->frame, "XOM", "2026-09-18");
  ASSERT_NE(cell_a, nullptr);
  EXPECT_FALSE(atx::vol::has_flag(cell_a->completeness, atx::vol::ExpiryInputField::Rate));
}

// ── OpraLoadSpec::time (I3: opt-in VolTime T convention) ──────────────────

// spec.time reaches all three opra_panel.cpp T computations: the PCP
// spot-implication forward T (line 181), the 0DTE drop filter (line 399, not
// independently asserted here -- it shares the identical
// time_to_expiry_years(.., spec.time) expression as the other two), and the
// per-expiry rate_source stamping against the term curve (line 492).
TEST(OpraPanel, VolTimeConvention_ChangesPcpSpotAndTermRateStamping) {
  const std::string snap = "2026-05-01";
  const std::string path = write_slice("term_curve_voltime.parquet", xom_two_expiry_rows());
  const std::vector<double> pt = {0.1, 2.0};
  const std::vector<double> pr = {0.02, 0.06};

  OpraLoadSpec cal_spec;
  cal_spec.path = path;
  cal_spec.underlying = "XOM";
  cal_spec.snapshot_iso = snap;
  cal_spec.yc_pillar_t = pt;
  cal_spec.yc_pillar_r = pr;
  // cal_spec.time left at its default: TimeConvention::Calendar365.

  OpraLoadSpec vol_spec = cal_spec;
  vol_spec.time.convention = TimeConvention::VolTime;

  const auto cal = load_opra_cbbo_parquet(cal_spec);
  const auto vol = load_opra_cbbo_parquet(vol_spec);
  ASSERT_TRUE(cal.has_value()) << cal.error().to_string();
  ASSERT_TRUE(vol.has_value()) << vol.error().to_string();

  // (1) Per-expiry rate stamping (line 492): independently re-derive the
  // VolTime-routed rate and check it matches bit-for-bit, AND differs from the
  // Calendar365 run -- proving spec.time actually reaches this call site
  // rather than being silently ignored.
  const auto yc = YieldCurve::create(std::span<const double>(pt), std::span<const double>(pr));
  ASSERT_TRUE(yc.has_value()) << yc.error().to_string();
  const std::int64_t now_ns = iso_to_ns(snap);
  // The loader routes the TRUE 16:00 ET PM expiry instant (G1) through VolTime.
  const std::int64_t short_ns = expiry_instant_ns("2026-08-01", SettlementSession::Pm);
  const std::int64_t long_ns = expiry_instant_ns("2027-11-01", SettlementSession::Pm);
  const auto& us_cal = VolTimeCalendar::us_default();
  const auto vt_short_res = vol_time_years(now_ns, short_ns, VolTimeParams{}, us_cal);
  const auto vt_long_res = vol_time_years(now_ns, long_ns, VolTimeParams{}, us_cal);
  ASSERT_TRUE(vt_short_res.has_value()) << vt_short_res.error().to_string();
  ASSERT_TRUE(vt_long_res.has_value()) << vt_long_res.error().to_string();
  const double vt_short = *vt_short_res;
  const double vt_long = *vt_long_res;

  const auto *vol_short = atx::vol::find_expiry_inputs(vol->frame, "XOM", "2026-08-01");
  const auto *vol_long = atx::vol::find_expiry_inputs(vol->frame, "XOM", "2027-11-01");
  const auto *cal_short = atx::vol::find_expiry_inputs(cal->frame, "XOM", "2026-08-01");
  const auto *cal_long = atx::vol::find_expiry_inputs(cal->frame, "XOM", "2027-11-01");
  ASSERT_NE(vol_short, nullptr);
  ASSERT_NE(vol_long, nullptr);
  ASSERT_NE(cal_short, nullptr);
  ASSERT_NE(cal_long, nullptr);

  EXPECT_DOUBLE_EQ(vol_short->rate, yc->zero(vt_short));
  EXPECT_DOUBLE_EQ(vol_long->rate, yc->zero(vt_long));
  EXPECT_NE(vol_short->rate, cal_short->rate);
  EXPECT_NE(vol_long->rate, cal_long->rate);

  // (2) PCP spot implication (line 181): the front-expiry query T differs
  // between conventions, so the implied spot (which discounts/back-solves off
  // that T and its own rate_at(T)) differs too.
  EXPECT_NE(vol->implied_spot, cal->implied_spot);
}

// The misuse the frame-carried TimeSpec makes impossible: a VolTime panel
// handed to the PLAIN `data_install(u, panel.frame)` call every production
// consumer already makes (no extra threading) must install Chain::T under
// VolTime -- the loader stamped the convention onto the frame itself, so the
// install cannot silently fall back to Calendar365 and produce a
// mixed-convention universe.
TEST(OpraPanel, VolTimePanelInstallsConsistentChainT_NoThreadingRequired) {
  const std::string snap = "2026-05-01";
  const std::string path = write_slice("voltime_handoff.parquet", xom_two_expiry_rows());

  OpraLoadSpec vol_spec;
  vol_spec.path = path;
  vol_spec.underlying = "XOM";
  vol_spec.snapshot_iso = snap;
  vol_spec.r = 0.04;
  vol_spec.time.convention = TimeConvention::VolTime;

  const auto panel = load_opra_cbbo_parquet(vol_spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  // The loader stamped the convention onto the frame (authoritative) and the
  // panel mirror.
  EXPECT_TRUE(panel->frame.time == vol_spec.time);
  EXPECT_TRUE(panel->time == vol_spec.time);

  // Plain 2-arg install -- the exact call pattern spy_diag / spy_oos_check /
  // opra_parity_bench et al. use, with NO TimeSpec threading anywhere.
  atx::vol::Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{2});

  // Every installed Chain::T equals the independently-computed vol-time T for
  // its expiry -- the same convention the loader's own PCP/rate math used.
  const std::int64_t now_ns = iso_to_ns(snap);
  const auto &us_cal = VolTimeCalendar::us_default();
  const char *expiries[] = {"2026-08-01", "2027-11-01"};
  for (std::size_t i = 0; i < 2; ++i) {
    // Chain::T is now the VolTime year-fraction to the TRUE 16:00 ET expiry
    // instant (G1) — the same instant the loader's own PCP/rate math used.
    const auto expected_T = vol_time_years(
        now_ns, expiry_instant_ns(expiries[i], SettlementSession::Pm), VolTimeParams{}, us_cal);
    ASSERT_TRUE(expected_T.has_value()) << expected_T.error().to_string();
    EXPECT_DOUBLE_EQ((*under)->chains[i].T, *expected_T) << expiries[i];
    EXPECT_NE((*under)->chains[i].T, year_fraction(snap, expiries[i])) << expiries[i];
  }
}

// ── G1: true expiry instants (16:00 ET PM / 09:30 ET AM) ────────────────────

// expiry_instant_ns maps an OSI/listed expiry DATE to the correct UTC ns of the
// 16:00 ET PM settlement across the modern-DST calendar 2024-2028: monthly
// (3rd-Fri), quarterly, weekly, and half-day-session dates. Cross-checked against
// iso_to_ns of the explicit-UTC datetime (EDT = 20:00Z summer, EST = 21:00Z
// winter) — an independent conversion path from expiry_instant_ns's
// days_since_epoch + settlement_instant_ns.
TEST(OpraPanel, ExpiryInstant_PmSettle_UtcNsAcrossDstAndSessions) {
  struct Case {
    const char *date; // OSI expiry date
    const char *utc;  // expected 16:00 ET, as an explicit-UTC datetime
    const char *note;
  };
  const Case cases[] = {
      {"2024-01-19", "2024-01-19T21:00:00Z", "monthly, EST"},
      {"2024-03-15", "2024-03-15T20:00:00Z", "monthly, EDT (DST began 03-10)"},
      {"2024-06-21", "2024-06-21T20:00:00Z", "quarterly, EDT"},
      {"2024-11-29", "2024-11-29T21:00:00Z",
       "half-day after Thanksgiving, EST (early close NOT modelled)"},
      {"2025-03-07", "2025-03-07T21:00:00Z", "weekly, EST (Fri before DST 03-09)"},
      {"2025-03-21", "2025-03-21T20:00:00Z", "weekly, EDT"},
      {"2026-07-17", "2026-07-17T20:00:00Z", "0DTE session fixture, EDT"},
      {"2026-12-18", "2026-12-18T21:00:00Z", "quarterly, EST"},
      {"2027-11-05", "2027-11-05T20:00:00Z", "weekly, EDT (DST ends 11-07)"},
      {"2028-03-17", "2028-03-17T20:00:00Z", "weekly, EDT (DST began 03-12)"},
  };
  for (const Case &c : cases) {
    EXPECT_EQ(expiry_instant_ns(c.date, SettlementSession::Pm), iso_to_ns(c.utc))
        << c.date << " (" << c.note << ")";
  }
  // AM-settled hook: 09:30 ET (EDT = 13:30Z / EST = 14:30Z).
  EXPECT_EQ(expiry_instant_ns("2026-07-17", SettlementSession::Am),
            iso_to_ns("2026-07-17T13:30:00Z"));
  EXPECT_EQ(expiry_instant_ns("2026-12-18", SettlementSession::Am),
            iso_to_ns("2026-12-18T14:30:00Z"));
  // Default is PM.
  EXPECT_EQ(expiry_instant_ns("2026-07-17"),
            expiry_instant_ns("2026-07-17", SettlementSession::Pm));
  // settlement_instant_ns and expiry_instant_ns agree for the same civil day.
  EXPECT_EQ(expiry_instant_ns("2026-07-17", SettlementSession::Pm),
            settlement_instant_ns(
                static_cast<std::int32_t>(iso_to_ns("2026-07-17") / (86400LL * 1000000000LL)),
                SettlementSession::Pm));
  // Unparseable date -> 0 sentinel (matches iso_to_ns).
  EXPECT_EQ(expiry_instant_ns("not-a-date"), 0);
  // DST-boundary exactness: the Fri before spring-forward is EST, the Fri after
  // is EDT — the UTC step is one hour LESS than the seven-day calendar gap.
  const std::int64_t before = expiry_instant_ns("2025-03-07", SettlementSession::Pm); // 21:00Z
  const std::int64_t after = expiry_instant_ns("2025-03-14", SettlementSession::Pm);  // 20:00Z
  EXPECT_EQ(after - before, iso_to_ns("2025-03-14T20:00:00Z") - iso_to_ns("2025-03-07T21:00:00Z"));
}

// G1 0DTE ingest: a same-session (0DTE) expiry survives ingest carrying a small
// POSITIVE intraday T, and that T shrinks monotonically toward 0 as the snapshot
// marches through the session to the 16:00 ET settle. Previously EVERY same-day
// contract was hard-dropped at the midnight-UTC parse.
TEST(OpraPanel, Ingest0DTE_SurvivesWithPositiveIntradayT_MonotoneThroughSession) {
  // 0DTE co-terminal pairs near F≈600 (C - P = F - K) plus one far expiry so the
  // board is a realistic front+back chain; spot is pinned so the test isolates
  // 0DTE survival from PCP conditioning.
  std::vector<RawRow> rows;
  for (const double k : {595.0, 600.0, 605.0}) {
    rows.push_back({"SPY", osi_sym("SPY", "260717", 'C', k), 0.5 + std::max(0.0, 600.0 - k),
                    0.6 + std::max(0.0, 600.0 - k)});
    rows.push_back({"SPY", osi_sym("SPY", "260717", 'P', k), 0.5 + std::max(0.0, k - 600.0),
                    0.6 + std::max(0.0, k - 600.0)});
  }
  rows.push_back({"SPY", osi_sym("SPY", "261218", 'C', 600.0), 20.0, 20.2});
  rows.push_back({"SPY", osi_sym("SPY", "261218", 'P', 600.0), 18.0, 18.2});

  // Snapshots marching toward the 16:00 ET (20:00Z) settle on the expiry day.
  // One slice per snapshot, each stamped with ITS OWN `ts` -- three successive
  // cbbo-1m minutes of the same session, which is what the feed actually
  // delivers. (Before FIX-C-1 this loop re-read ONE file at three unrelated
  // instants; the loader now takes the instant from the file, so a shared
  // fixture would have to lie about two of the three.)
  const std::vector<std::string> snaps = {
      "2026-07-17T13:35:00Z", // 09:35 ET  (~6.4h to settle)
      "2026-07-17T18:00:00Z", // 14:00 ET  (~2h)
      "2026-07-17T19:55:00Z", // 15:55 ET  (~5min)
  };
  const std::int64_t zdte_instant = expiry_instant_ns("2026-07-17", SettlementSession::Pm);

  double prev_T = std::numeric_limits<double>::infinity();
  for (const std::string &snap : snaps) {
    SCOPED_TRACE(snap);
    const std::string path =
        write_slice("zero_dte_" + snap.substr(11, 2) + snap.substr(14, 2) + ".parquet", rows, true,
                    {}, iso_to_ns(snap));
    OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = "SPY";
    spec.snapshot_iso = snap;
    spec.r = 0.043;
    spec.spot_override = 600.0;

    const auto loaded = load_opra_cbbo_parquet(spec);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
    // The 0DTE expiry SURVIVES: all 6 near legs + 2 far legs kept, none dropped.
    EXPECT_EQ(loaded->n_dropped, std::size_t{0});
    EXPECT_EQ(loaded->n_contracts, std::size_t{8});
    std::size_t n_zdte = 0;
    for (const auto &row : loaded->frame.rows) {
      if (row.expiry_iso == "2026-07-17") {
        ++n_zdte;
        EXPECT_EQ(row.expiry_ns, zdte_instant); // TRUE 16:00 ET instant stamped
      }
    }
    EXPECT_EQ(n_zdte, std::size_t{6});

    // Install and confirm the 0DTE chain carries a small POSITIVE intraday T.
    atx::vol::Universe u;
    const auto uid = atx::vol::data_install(u, loaded->frame);
    ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
    const auto under = u.get_underlying(*uid);
    ASSERT_TRUE(under.has_value());
    const atx::vol::Chain *zdte = nullptr;
    for (const atx::vol::Chain &ch : (*under)->chains) {
      if (ch.expiry_ns == zdte_instant) {
        zdte = &ch;
        break;
      }
    }
    ASSERT_NE(zdte, nullptr) << "0DTE chain not installed";
    EXPECT_GT(zdte->T, 0.0);
    EXPECT_LT(zdte->T, 0.01); // hours, not days
    EXPECT_TRUE(std::isfinite(zdte->T));
    // T strictly shrinks toward 0 as the session advances.
    EXPECT_LT(zdte->T, prev_T);
    prev_T = zdte->T;
  }
  // Final snapshot (15:55 ET) leaves ~5 min: T ≈ 300s / (365.25*86400) ≈ 9.5e-6.
  EXPECT_NEAR(prev_T, 9.5e-6, 2.0e-6);

  const fs::path dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::remove_all(dir);
}

TEST(OpraPanel, ExplicitIndexSemanticsStampEuropeanAmContracts) {
  const std::vector<RawRow> rows = {
      {"SPX", osi_sym("SPX", "190920", 'C', 2870.0), 54.5, 55.0},
      {"SPX", osi_sym("SPX", "190920", 'P', 2870.0), 55.0, 55.5},
  };
  const std::string snapshot = "2019-08-26T19:30:00Z";
  const std::string path =
      write_slice("spx_am_european.parquet", rows, true, {}, iso_to_ns(snapshot));
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPX";
  spec.snapshot_iso = snapshot;
  spec.r = 0.02;
  spec.spot_override = 2869.0;
  spec.expiry_close = ExpiryCloseConvention::UsIndexAmOpen;
  spec.exercise_style = ExerciseStyle::European;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  ASSERT_EQ(loaded->frame.rows.size(), std::size_t{2});
  const std::int64_t expected = expiry_instant_ns("2019-09-20", SettlementSession::Am);
  for (const auto &row : loaded->frame.rows) {
    EXPECT_EQ(row.expiry_ns, expected);
    EXPECT_EQ(row.settle, SettlementSession::Am);
    EXPECT_EQ(row.exercise_style, ExerciseStyle::European);
  }

  atx::vol::Universe u;
  const auto uid = atx::vol::data_install(u, loaded->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{1});
  EXPECT_EQ((*under)->chains.front().exercise_style, ExerciseStyle::European);
  EXPECT_EQ((*under)->chains.front().expiry_ns, expected);

  const fs::path dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::remove_all(dir);
}

// ── B4 row 2: instrument conventions come from the INSTRUMENT ───────────────
//
// `OpraLoadSpec` used to carry PM-close / American as plain DEFAULT VALUES and no
// production caller assigned either (only two files under examples/ did). So on
// every real board `chain.exercise_style` was American, the European branches
// downstream — deamer.cpp:814, prepared_fitting.cpp:247, parity.cpp:123 — could
// not execute, and a cash-settled index board ran the American early-exercise
// machinery on options that cannot be exercised early. These pin the resolution,
// both directions of the override, and the equity no-op.

TEST(OpraPanel, UsListedConventionsSeparatesClassNotIndex) {
  using atx::vol::InstrumentConventions;
  using atx::vol::us_listed_conventions;
  constexpr InstrumentConventions kEquity{ExpiryCloseConvention::UsEquityPmClose,
                                          ExerciseStyle::American};

  // Unlisted => the historical equity default, so nothing in today's universe moves.
  for (const char *equity : {"AAPL", "SPY", "QQQ", "VXX", "BRKB", "", "SPXQ"}) {
    EXPECT_EQ(us_listed_conventions(equity), kEquity) << equity;
  }
  // Third-Friday index class: AM-settled, European.
  EXPECT_EQ(us_listed_conventions("SPX"),
            (InstrumentConventions{ExpiryCloseConvention::UsIndexAmOpen, ExerciseStyle::European}));
  // Its weekly sibling on the SAME index is a different class and settles PM.
  EXPECT_EQ(
      us_listed_conventions("SPXW"),
      (InstrumentConventions{ExpiryCloseConvention::UsEquityPmClose, ExerciseStyle::European}));
  // The pair that forbids "index implies European": OEX is cash-settled and
  // AMERICAN, XEO is the European contract on the same index.
  EXPECT_EQ(us_listed_conventions("OEX").exercise_style, ExerciseStyle::American);
  EXPECT_EQ(us_listed_conventions("XEO").exercise_style, ExerciseStyle::European);
  // Case-sensitive: both namespaces are upper-case by construction, and a
  // lower-case key must not silently acquire index semantics.
  EXPECT_EQ(us_listed_conventions("spx"), kEquity);
}

TEST(OpraPanel, DefaultSpecLoadsACashIndexBoardAsEuropeanAmSettled) {
  const std::vector<RawRow> rows = {
      {"SPX", osi_sym("SPX", "190920", 'C', 2870.0), 54.5, 55.0},
      {"SPX", osi_sym("SPX", "190920", 'P', 2870.0), 55.0, 55.5},
  };
  const std::string snapshot = "2019-08-26T19:30:00Z";
  const std::string path =
      write_slice("spx_default_spec.parquet", rows, true, {}, iso_to_ns(snapshot));

  // A DEFAULT spec — exactly what every production caller builds. Nothing here
  // names a convention.
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPX";
  spec.snapshot_iso = snapshot;
  spec.r = 0.02;
  spec.spot_override = 2869.0;
  EXPECT_FALSE(spec.expiry_close.has_value());
  EXPECT_FALSE(spec.exercise_style.has_value());

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  ASSERT_EQ(loaded->frame.rows.size(), std::size_t{2});
  const std::int64_t am_instant = expiry_instant_ns("2019-09-20", SettlementSession::Am);
  for (const auto &row : loaded->frame.rows) {
    EXPECT_EQ(row.exercise_style, ExerciseStyle::European);
    EXPECT_EQ(row.settle, SettlementSession::Am);
    EXPECT_EQ(row.expiry_ns, am_instant);
  }
  atx::vol::Universe u;
  const auto uid = atx::vol::data_install(u, loaded->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{1});
  // The reachability claim as an assertion: the European consumers downstream
  // read exactly this field off the chain.
  EXPECT_EQ((*under)->chains.front().exercise_style, ExerciseStyle::European);

  const fs::path spx_dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::remove_all(spx_dir);
}

TEST(OpraPanel, DefaultSpecLeavesAnEquityBoardAmericanPmSettled) {
  const std::vector<RawRow> rows = {
      {"AAPL", osi_sym("AAPL", "260918", 'C', 250.0), 12.0, 12.2},
      {"AAPL", osi_sym("AAPL", "260918", 'P', 250.0), 10.0, 10.2},
  };
  const std::string snapshot = "2026-08-03T19:55:00Z";
  const std::string path =
      write_slice("aapl_default_spec.parquet", rows, true, {}, iso_to_ns(snapshot));
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "AAPL";
  spec.snapshot_iso = snapshot;
  spec.r = 0.043;
  spec.spot_override = 252.0;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  ASSERT_EQ(loaded->frame.rows.size(), std::size_t{2});
  const std::int64_t pm_instant = expiry_instant_ns("2026-09-18", SettlementSession::Pm);
  for (const auto &row : loaded->frame.rows) {
    EXPECT_EQ(row.exercise_style, ExerciseStyle::American);
    EXPECT_EQ(row.settle, SettlementSession::Pm);
    EXPECT_EQ(row.expiry_ns, pm_instant);
  }

  const fs::path equity_dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::remove_all(equity_dir);
}

TEST(OpraPanel, ExplicitSpecConventionsStillOverrideTheRegistry) {
  const std::vector<RawRow> rows = {
      {"SPX", osi_sym("SPX", "190920", 'C', 2870.0), 54.5, 55.0},
      {"SPX", osi_sym("SPX", "190920", 'P', 2870.0), 55.0, 55.5},
  };
  const std::string snapshot = "2019-08-26T19:30:00Z";
  const std::string path = write_slice("spx_override.parquet", rows, true, {}, iso_to_ns(snapshot));

  // Force an index board back onto the equity conventions: an explicit value
  // wins, which is what keeps a caller that already states its own convention
  // bit-identical.
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPX";
  spec.snapshot_iso = snapshot;
  spec.r = 0.02;
  spec.spot_override = 2869.0;
  spec.expiry_close = ExpiryCloseConvention::UsEquityPmClose;
  spec.exercise_style = ExerciseStyle::American;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  const std::int64_t pm_instant = expiry_instant_ns("2019-09-20", SettlementSession::Pm);
  for (const auto &row : loaded->frame.rows) {
    EXPECT_EQ(row.exercise_style, ExerciseStyle::American);
    EXPECT_EQ(row.settle, SettlementSession::Pm);
    EXPECT_EQ(row.expiry_ns, pm_instant);
  }

  // MidnightUtc is a historical spelling that now resolves to the same PM close;
  // pinned so the header's correction cannot drift from the code.
  spec.expiry_close = ExpiryCloseConvention::MidnightUtc;
  const auto midnight = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(midnight.has_value()) << midnight.error().to_string();
  for (const auto &row : midnight->frame.rows) {
    EXPECT_EQ(row.settle, SettlementSession::Pm);
    EXPECT_EQ(row.expiry_ns, pm_instant);
  }

  const fs::path override_dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::remove_all(override_dir);
}

// An unfiltered load has no `underlying` to key on, so the conventions must come
// off the rows' own OSI root — otherwise a whole-file index load silently reverts
// to equity semantics.
TEST(OpraPanel, UnfilteredLoadResolvesConventionsFromTheOsiRoot) {
  const std::vector<RawRow> rows = {
      {"SPX", osi_sym("SPX", "190920", 'C', 2870.0), 54.5, 55.0},
      {"SPX", osi_sym("SPX", "190920", 'P', 2870.0), 55.0, 55.5},
  };
  const std::string snapshot = "2019-08-26T19:30:00Z";
  const std::string path =
      write_slice("spx_unfiltered.parquet", rows, false, {}, iso_to_ns(snapshot));
  OpraLoadSpec spec;
  spec.path = path; // no `underlying` filter, no `underlying` column
  spec.snapshot_iso = snapshot;
  spec.r = 0.02;
  spec.spot_override = 2869.0;

  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  ASSERT_EQ(loaded->frame.rows.size(), std::size_t{2});
  for (const auto &row : loaded->frame.rows) {
    EXPECT_EQ(row.exercise_style, ExerciseStyle::European);
    EXPECT_EQ(row.settle, SettlementSession::Am);
  }

  const fs::path unfiltered_dir = fs::temp_directory_path() / "atx_opra_p2_test";
  fs::remove_all(unfiltered_dir);
}

} // namespace
