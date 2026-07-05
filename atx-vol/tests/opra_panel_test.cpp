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
#include "atx/vol/data.hpp" // year_fraction

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

} // namespace
