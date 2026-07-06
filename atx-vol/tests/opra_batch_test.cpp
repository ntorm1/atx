#include "atx/vol/opra_batch.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/io/parquet_writer.hpp"
#include "atx/vol/curve.hpp"        // YieldCurve
#include "atx/vol/data.hpp"         // QuoteFrame
#include "atx/vol/market_env.hpp"   // MarketEnv

// Coverage for the P2-4 date-range batch loader (`load_opra_daterange`) and the
// term-curve -> MarketEnv bridge (`market_env_from_frame`).

namespace {

namespace io = atx::core::io;
namespace fs = std::filesystem;
using atx::i64;
using atx::vol::load_opra_daterange;
using atx::vol::market_env_from_frame;
using atx::vol::MarketEnv;
using atx::vol::OpraBatchEntry;
using atx::vol::OpraBatchResult;
using atx::vol::OpraBatchSpec;
using atx::vol::QuoteFrame;
using atx::vol::YieldCurve;

// ── In-test parquet fixture ────────────────────────────────────────────────

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

// Write a single-symbol co-terminal call/put pair (one strike, one expiry) to an
// explicit parquet path so the loader can imply the spot via put-call parity.
// The mids are planted so C - P = fwd - strike (r = 0): the implied forward, and
// thus the implied spot (df = 1), is `fwd`.
void write_pair(const fs::path& path, const std::string& symbol, const std::string& yymmdd,
                double strike, double fwd) {
  const double put_mid = 5.0;
  const double call_mid = put_mid + (fwd - strike);
  const auto to_px = [](double d) { return static_cast<i64>(std::llround(d * 1e9)); };

  std::vector<i64> ts_col = {1780000000000000000LL, 1780000000000000000LL};
  std::vector<std::string> und_col = {symbol, symbol};
  std::vector<std::string> sym_col = {osi_sym(symbol, yymmdd, 'C', strike),
                                      osi_sym(symbol, yymmdd, 'P', strike)};
  std::vector<i64> bidpx = {to_px(call_mid - 0.05), to_px(put_mid - 0.05)};
  std::vector<i64> askpx = {to_px(call_mid + 0.05), to_px(put_mid + 0.05)};
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
  fs::create_directories(path.parent_path());
  fs::remove(path);
  ASSERT_TRUE(io::write_parquet(cols, path.string()).has_value());
}

// The two symbols' planted forwards (implied spot with r = 0).
constexpr double kXomFwd = 111.0;
constexpr double kAaplFwd = 252.0;

// Find the entry for a (symbol, date) in the batch result.
[[nodiscard]] const OpraBatchEntry* find_entry(const OpraBatchResult& r,
                                               const std::string& symbol,
                                               const std::string& date) {
  for (const OpraBatchEntry& e : r.entries) {
    if (e.symbol == symbol && e.date == date) {
      return &e;
    }
  }
  return nullptr;
}

// ── load_opra_daterange: counts, panels, missing handling ──────────────────

TEST(OpraBatch, DateRange_CountsPanelsAndMissing) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_counts";
  fs::remove_all(root);

  const std::vector<std::string> dates = {"2026-06-01", "2026-06-02", "2026-06-03"};
  // Expiry ~110 days out (well-conditioned for the PCP spot back-out).
  const std::string yymmdd = "260918";
  const std::string exp_iso = "2026-09-18";
  const double strike = 110.0;

  // 2 symbols x 3 dates = 6 cells, with (AAPL, 2026-06-02) DELIBERATELY absent.
  for (const std::string& d : dates) {
    write_pair(root / "XOM" / (d + ".parquet"), "XOM", yymmdd, strike, kXomFwd);
    if (d != "2026-06-02") {
      write_pair(root / "AAPL" / (d + ".parquet"), "AAPL", yymmdd, strike, kAaplFwd);
    }
  }

  OpraBatchSpec spec;
  spec.symbols = {"XOM", "AAPL"};
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-03";
  spec.root_dir = root.string();
  spec.r = 0.0; // r = 0 => implied spot == planted forward exactly

  const auto res = load_opra_daterange(spec);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  EXPECT_EQ(res->n_total, std::size_t{6});
  EXPECT_EQ(res->n_loaded, std::size_t{5});
  EXPECT_EQ(res->n_missing, std::size_t{1});
  EXPECT_EQ(res->n_error, std::size_t{0});
  EXPECT_EQ(res->entries.size(), std::size_t{6});

  // Every Ok entry's panel: single co-terminal pair (2 contracts) and the right
  // symbol-specific implied spot.
  for (const OpraBatchEntry& e : res->entries) {
    if (e.symbol == "AAPL" && e.date == "2026-06-02") {
      continue; // the missing one
    }
    ASSERT_TRUE(e.panel.has_value()) << e.symbol << " " << e.date << ": "
                                     << e.panel.error().to_string();
    EXPECT_EQ(e.panel->n_contracts, std::size_t{2});
    EXPECT_EQ(e.panel->n_expiries, std::size_t{1});
    EXPECT_EQ(e.panel->frame.uid, e.symbol);
    const double expected = (e.symbol == "XOM") ? kXomFwd : kAaplFwd;
    EXPECT_NEAR(e.panel->implied_spot, expected, 1e-3) << e.symbol << " " << e.date;
  }

  // The missing (AAPL, 2026-06-02) cell is Err(NotFound).
  const OpraBatchEntry* missing = find_entry(*res, "AAPL", "2026-06-02");
  ASSERT_NE(missing, nullptr);
  ASSERT_FALSE(missing->panel.has_value());
  EXPECT_EQ(missing->panel.error().code(), atx::vol::ErrorCode::NotFound);

  fs::remove_all(root);
}

TEST(OpraBatch, DateRange_ProgressFiresPerCellMonotonic) {
  const fs::path root = fs::temp_directory_path() / "atx_opra_batch_progress";
  fs::remove_all(root);

  const std::vector<std::string> dates = {"2026-06-01", "2026-06-02", "2026-06-03"};
  for (const std::string& d : dates) {
    write_pair(root / "XOM" / (d + ".parquet"), "XOM", "260918", 110.0, kXomFwd);
    if (d != "2026-06-02") {
      write_pair(root / "AAPL" / (d + ".parquet"), "AAPL", "260918", 110.0, kAaplFwd);
    }
  }

  OpraBatchSpec spec;
  spec.symbols = {"XOM", "AAPL"};
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-03";
  spec.root_dir = root.string();

  std::size_t n_calls = 0;
  std::size_t last_done = 0;
  std::size_t last_total = 0;
  bool monotonic = true;
  const auto progress = [&](std::size_t done, std::size_t total,
                            const OpraBatchEntry& /*entry*/) {
    ++n_calls;
    if (done != last_done + 1) {
      monotonic = false;
    }
    last_done = done;
    last_total = total;
  };

  const auto res = load_opra_daterange(spec, progress);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  EXPECT_EQ(n_calls, res->n_total);           // fired exactly n_total times
  EXPECT_EQ(n_calls, std::size_t{6});
  EXPECT_TRUE(monotonic);                      // done stepped 1,2,...,n_total
  EXPECT_EQ(last_done, res->n_total);
  EXPECT_EQ(last_total, res->n_total);

  fs::remove_all(root);
}

// ── Malformed spec -> top-level Err ─────────────────────────────────────────

TEST(OpraBatch, MalformedSpec_EmptySymbols_Rejected) {
  OpraBatchSpec spec;
  spec.date_lo = "2026-06-01";
  spec.date_hi = "2026-06-03";
  spec.root_dir = fs::temp_directory_path().string();
  const auto res = load_opra_daterange(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(OpraBatch, MalformedSpec_ReversedDates_Rejected) {
  OpraBatchSpec spec;
  spec.symbols = {"XOM"};
  spec.date_lo = "2026-06-03";
  spec.date_hi = "2026-06-01"; // hi < lo
  spec.root_dir = fs::temp_directory_path().string();
  const auto res = load_opra_daterange(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(OpraBatch, MalformedSpec_UnparseableDate_Rejected) {
  OpraBatchSpec spec;
  spec.symbols = {"XOM"};
  spec.date_lo = "2026/06/01"; // wrong format
  spec.date_hi = "2026-06-03";
  spec.root_dir = fs::temp_directory_path().string();
  const auto res = load_opra_daterange(spec);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── market_env_from_frame: the term-curve bridge ────────────────────────────

TEST(OpraBatch, MarketEnvFromFrame_TermCurveInterpolatesShortRate) {
  // A materially-sloped curve bracketing a short front maturity.
  const std::vector<double> pt = {0.1, 2.0};
  const std::vector<double> pr = {0.02, 0.06};

  QuoteFrame frame;
  frame.spot = 100.0;
  frame.snapshot_ts_ns = 1780000000000000000LL;
  frame.yc_pillar_t = pt;
  frame.yc_pillar_r = pr;

  const MarketEnv env = market_env_from_frame(frame);
  EXPECT_DOUBLE_EQ(env.spot, 100.0);
  EXPECT_EQ(env.now_ns, frame.snapshot_ts_ns);
  ASSERT_GT(env.yield.size(), std::size_t{0}); // a real term curve was built

  // Independently-built reference curve: the env's rate_at must equal the
  // monotone-Hermite zero rate at each maturity, bit-for-bit.
  const auto yc = YieldCurve::create(std::span<const double>(pt), std::span<const double>(pr));
  ASSERT_TRUE(yc.has_value()) << yc.error().to_string();

  const double front_T = 0.25; // a single date's front expiry, ~3 months out
  EXPECT_DOUBLE_EQ(env.rate_at(front_T), yc->zero(front_T));

  // The interpolated short rate genuinely sits between the pillars (term
  // structure is live), NOT collapsed to a flat number.
  EXPECT_GT(env.rate_at(front_T), 0.02);
  EXPECT_LT(env.rate_at(front_T), 0.06);
}

TEST(OpraBatch, MarketEnvFromFrame_NoOrSinglePillarIsFlat) {
  // (A) No pillars -> flat 0.
  QuoteFrame f0;
  f0.spot = 100.0;
  f0.snapshot_ts_ns = 1780000000000000000LL;
  const MarketEnv e0 = market_env_from_frame(f0);
  EXPECT_EQ(e0.yield.size(), std::size_t{0}); // no curve
  EXPECT_DOUBLE_EQ(e0.rate_at(0.25), 0.0);
  EXPECT_DOUBLE_EQ(e0.spot, 100.0);

  // (B) A single pillar -> flat at that pillar's rate (a 1-pillar curve does NOT
  // interpolate flat, so it must be treated as the flat rate).
  QuoteFrame f1;
  f1.spot = 100.0;
  f1.snapshot_ts_ns = 1780000000000000000LL;
  f1.yc_pillar_t = {1.0};
  f1.yc_pillar_r = {0.05};
  const MarketEnv e1 = market_env_from_frame(f1);
  EXPECT_EQ(e1.yield.size(), std::size_t{0}); // still no curve
  EXPECT_DOUBLE_EQ(e1.rate_at(0.25), 0.05);   // the flat rate at any T
  EXPECT_DOUBLE_EQ(e1.rate_at(5.0), 0.05);
}

} // namespace
