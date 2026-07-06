#include "atx/vol/opra_panel.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/io/parquet_writer.hpp"
#include "atx/vol/curve.hpp" // YieldCurve
#include "atx/vol/data.hpp"  // year_fraction, find_expiry_inputs, ExpiryInputs

// Loader/parser coverage for the OPRA cbbo-1m (NBBO) Parquet ingestion path.
//
//   parse_osi_symbol          -> ParseOsi_* (unit)
//   load_opra_cbbo_parquet    -> Load_* (round-trip over a synthetic fixture)

namespace {

namespace io = atx::core::io;
namespace fs = std::filesystem;
using atx::i64;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::OpraLoadSpec;
using atx::vol::parse_osi_symbol;
using atx::vol::Side;
using atx::vol::year_fraction;
using atx::vol::YieldCurve;

// ── Shared fixture builders (P2-2 / P2-3) ──────────────────────────────────

// One raw NBBO row: its underlying tag, OSI symbol, and dollar bid/ask.
struct RawRow {
  std::string underlying;
  std::string symbol;
  double bid;
  double ask;
};

// Compose an OSI/OCC 21-char symbol: 6-char space-padded root + YYMMDD + C/P +
// 8-digit strike (price x 1000).
[[nodiscard]] std::string osi_sym(std::string root, const std::string& yymmdd, char cp,
                                  double strike) {
  root.resize(6, ' ');
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08lld",
                static_cast<long long>(std::llround(strike * 1000.0)));
  return root + yymmdd + std::string(1, cp) + std::string(buf);
}

// Write a cbbo-1m slice (ts/[underlying]/symbol/bid_px/ask_px/bid_sz/ask_sz)
// from raw rows and return its path. `with_underlying=false` omits the column.
[[nodiscard]] std::string write_slice(const std::string& name,
                                      const std::vector<RawRow>& rows,
                                      bool with_underlying = true) {
  const auto to_px = [](double d) { return static_cast<i64>(std::llround(d * 1e9)); };
  std::vector<i64> ts_col, bidpx, askpx, bidsz, asksz;
  std::vector<std::string> und_col, sym_col;
  for (const RawRow& rr : rows) {
    ts_col.push_back(1780000000000000000LL);
    und_col.push_back(rr.underlying);
    sym_col.push_back(rr.symbol);
    bidpx.push_back(to_px(rr.bid));
    askpx.push_back(to_px(rr.ask));
    bidsz.push_back(10);
    asksz.push_back(12);
  }
  std::vector<io::WriteColumn> cols;
  cols.push_back({"ts", std::span<const i64>(ts_col)});
  if (with_underlying) {
    cols.push_back({"underlying", std::span<const std::string>(und_col)});
  }
  cols.push_back({"symbol", std::span<const std::string>(sym_col)});
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
    std::snprintf(buf, sizeof(buf), "%08lld",
                  static_cast<long long>(std::llround(k * 1000.0)));
    return std::string(buf);
  };
  const auto to_px = [](double dollars) {
    return static_cast<i64>(std::llround(dollars * 1e9));
  };

  const i64 ts_val = 1780000000000000000LL;
  const auto add_row = [&](const std::string& sym, i64 bpx, i64 apx) {
    ts_col.push_back(ts_val);
    und_col.emplace_back("XOM");
    sym_col.push_back(sym);
    bidpx.push_back(bpx);
    askpx.push_back(apx);
    bidsz.push_back(10);
    asksz.push_back(12);
  };

  for (const Expiry& e : expiries) {
    const double t = year_fraction(snap, e.iso);
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
  // Dropped: unset-price sentinel on bid_px.
  add_row(pad_root("XOM") + "260619C00120000", std::numeric_limits<i64>::min(), to_px(1.0));
  // Dropped: crossed quote (bid > ask).
  add_row(pad_root("XOM") + "260619P00120000", to_px(5.0), to_px(4.0));

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

  EXPECT_EQ(loaded->n_contracts, std::size_t{12});
  EXPECT_EQ(loaded->n_expiries, std::size_t{2});
  EXPECT_EQ(loaded->n_dropped, std::size_t{2});
  EXPECT_EQ(loaded->frame.uid, "XOM");
  EXPECT_EQ(loaded->frame.rows.size(), std::size_t{12});

  // The dropped strike (120) must not have entered the frame.
  for (const auto& row : loaded->frame.rows) {
    EXPECT_NE(row.strike, 120.0);
  }

  // Front expiry forward is planted at 111.0; implied spot = F * exp(-r*T).
  const double t_front = year_fraction(snap, "2026-06-19");
  const double expected_spot = 111.0 * std::exp(-r * t_front);
  EXPECT_NEAR(loaded->implied_spot, expected_spot, 1e-3);
  EXPECT_DOUBLE_EQ(loaded->frame.spot, loaded->implied_spot);

  fs::remove_all(dir);
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

TEST(OpraPanel, Load_MissingFile_ReturnsInvalidArgument) {
  OpraLoadSpec spec;
  spec.path = (fs::temp_directory_path() / "atx_opra_does_not_exist.parquet").string();
  spec.underlying = "XOM";
  spec.snapshot_iso = "2026-05-01";
  const auto loaded = load_opra_cbbo_parquet(spec);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), atx::vol::ErrorCode::InvalidArgument);
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
  for (const auto& row : loaded->frame.rows) {
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
  const auto yc = YieldCurve::create(std::span<const double>(pt),
                                     std::span<const double>(pr));
  ASSERT_TRUE(yc.has_value()) << yc.error().to_string();
  const double t_short = year_fraction(snap, "2026-08-01");
  const double t_long = year_fraction(snap, "2027-11-01");

  const auto* cell_short =
      atx::vol::find_expiry_inputs(loaded->frame, "XOM", "2026-08-01");
  const auto* cell_long =
      atx::vol::find_expiry_inputs(loaded->frame, "XOM", "2027-11-01");
  ASSERT_NE(cell_short, nullptr);
  ASSERT_NE(cell_long, nullptr);

  // Hand-checked: the per-expiry source rate equals the monotone-Hermite curve
  // rate at each maturity, bit-for-bit.
  EXPECT_DOUBLE_EQ(cell_short->rate, yc->zero(t_short));
  EXPECT_DOUBLE_EQ(cell_long->rate, yc->zero(t_long));
  EXPECT_TRUE(atx::vol::has_flag(cell_short->completeness,
                                 atx::vol::ExpiryInputField::Rate));
  EXPECT_TRUE(atx::vol::has_flag(cell_long->completeness,
                                 atx::vol::ExpiryInputField::Rate));

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
  const auto* cell_b = atx::vol::find_expiry_inputs(b->frame, "XOM", "2026-09-18");
  ASSERT_NE(cell_b, nullptr);
  EXPECT_DOUBLE_EQ(cell_b->rate, r);
  EXPECT_TRUE(atx::vol::has_flag(cell_b->completeness, atx::vol::ExpiryInputField::Rate));
  const std::vector<double> one_t = {1.0};
  const std::vector<double> one_r = {r};
  EXPECT_EQ(b->frame.yc_pillar_t, one_t);
  EXPECT_EQ(b->frame.yc_pillar_r, one_r);

  const auto* cell_a = atx::vol::find_expiry_inputs(a->frame, "XOM", "2026-09-18");
  ASSERT_NE(cell_a, nullptr);
  EXPECT_FALSE(atx::vol::has_flag(cell_a->completeness, atx::vol::ExpiryInputField::Rate));
}

} // namespace
