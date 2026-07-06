// atx-vol backtest engine (Phase B0) gate tests.
//
// Drives `run_backtest` over synthetic single-underlying corpora built by
// writing hand-fitted eSSVI `PricedSurface`s (the pnl_greeks_consistency
// pattern) to one archive per date. No external data — runs everywhere.
//
// Six gates:
//   1. LoadOnce         — N-date run opens each archive exactly once.
//   2a. AgingSpotOnly   — spot-only step => unexplained tiny vs total.
//   2b. AgingTimeOnly   — time-only step => PnL isolates to theta.
//   3. AttributionCloses— axes + unexplained == pnl_total (non-settlement).
//   4. Determinism      — n_threads 1 vs 4 => BacktestResult bit-identical.
//   5. Granularity      — coarse recorded nav/attribution == fine at samples.
//   6. ExpirySettlement — a lot crossing expiry settles at intrinsic and drops.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/portfolio_pricer.hpp" // OptionContract
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, Result, Status
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUid = 7;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] fs::path fresh_dir(const char* tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-backtest-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// A synthetic eSSVI PricedSurface: flat forward `fwd`, genuine American premium
// (q_eff = 0.02), slices spanning T in [0.05, 1.0]. `vol_bump` shifts the whole
// term's ATM variance. Mirrors pnl_greeks_consistency's make_essvi.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts, double vol_bump = 0.0) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = fwd;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

// Write one surface as this date's archive; return its path (creating `dir`).
[[nodiscard]] std::string write_one(const fs::path& dir, const std::string& date,
                                    const std::string& symbol, const PricedSurface& s) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{symbol, &s};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows.
[[nodiscard]] CorpusManifest make_manifest(
    const std::vector<std::pair<std::string, std::string>>& date_paths, const std::string& symbol) {
  CorpusManifest m;
  for (const auto& [date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = symbol;
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

// An N-date corpus: spot drifts +0.4%/day, now advances 1 day, vol drifts up.
[[nodiscard]] CorpusManifest make_evolving_corpus(const fs::path& dir, const std::string& symbol,
                                                  int n_dates) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = 100.0 * (1.0 + 0.004 * static_cast<double>(d));
    const double vbump = 0.001 * static_cast<double>(d);
    const PricedSurface s = make_surface(kUid, S, S, now, vbump);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_one(dir, date, symbol, s));
  }
  return make_manifest(dp, symbol);
}

// A two-lot book (long call, short put) that survives past `expiry`.
[[nodiscard]] PortfolioState survivor_book(std::int64_t expiry) {
  PortfolioState st;
  st.lots.push_back(Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Call}, +5.0, 100.0, expiry, 0, 0.0});
  st.lots.push_back(Lot{2, OptionContract{kUid, 105.0, 0.0, Side::Put}, -3.0, 100.0, expiry, 0, 0.0});
  return st;
}

void expect_result_bit_identical(const BacktestResult& a, const BacktestResult& b) {
  ASSERT_EQ(a.size(), b.size());
  const std::vector<std::pair<const std::vector<double>*, const std::vector<double>*>> cols = {
      {&a.pnl_total, &b.pnl_total},   {&a.pnl_delta, &b.pnl_delta},
      {&a.pnl_gamma, &b.pnl_gamma},   {&a.pnl_vega, &b.pnl_vega},
      {&a.pnl_vanna, &b.pnl_vanna},   {&a.pnl_volga, &b.pnl_volga},
      {&a.pnl_theta, &b.pnl_theta},   {&a.pnl_rho, &b.pnl_rho},
      {&a.pnl_charm, &b.pnl_charm},   {&a.pnl_unexplained, &b.pnl_unexplained},
      {&a.pnl_settlement, &b.pnl_settlement}, {&a.nav, &b.nav},
      {&a.gross_delta, &b.gross_delta}, {&a.gross_gamma, &b.gross_gamma},
      {&a.gross_vega, &b.gross_vega}, {&a.gross_theta, &b.gross_theta},
      {&a.n_open_lots, &b.n_open_lots}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    EXPECT_EQ(a.ts_ns[i], b.ts_ns[i]) << i;
    for (const auto& [va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
  }
}

}  // namespace

// ── 1. Load-once ────────────────────────────────────────────────────────────
TEST(Backtest, LoadOnce) {
  const fs::path dir = fresh_dir("loadonce");
  const int n = 5;
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", n);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), static_cast<std::size_t>(n));

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;  // survives every date
  MarketSnapshot::reset_open_count();
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(MarketSnapshot::open_count(), static_cast<std::uint64_t>(n));
  EXPECT_EQ(res->size(), static_cast<std::size_t>(n));  // inception + (n-1) steps
}

// ── 2a. Aging: spot-only step reconstructs to a tiny residual ───────────────
TEST(Backtest, AgingSpotOnly) {
  const fs::path dir = fresh_dir("spot");
  const std::int64_t now = kBaseNow;  // identical valuation time both dates
  const double S0 = 100.0;
  const double ratio = 1.004;  // bump spot AND forward by the same ratio
  const PricedSurface d0 = make_surface(kUid, S0, S0, now);
  const PricedSurface d1 = make_surface(kUid, S0 * ratio, S0 * ratio, now);
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-08-02", "SPX", d1);
  auto clock = Clock::from_manifest(
      make_manifest({{"2026-08-01", p0}, {"2026-08-02", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), 2u);

  const double total = res->pnl_total[1];
  const double unexpl = res->pnl_unexplained[1];
  EXPECT_EQ(res->pnl_settlement[1], 0.0);
  EXPECT_GT(std::fabs(total), 0.0);
  EXPECT_LE(std::fabs(unexpl), 1e-3 * std::fabs(total) + 1e-6)
      << "unexplained=" << unexpl << " total=" << total;
}

// ── 2b. Aging: time-only step isolates to theta ─────────────────────────────
TEST(Backtest, AgingTimeOnly) {
  const fs::path dir = fresh_dir("time");
  const double S0 = 100.0;
  const PricedSurface d0 = make_surface(kUid, S0, S0, kBaseNow);
  const PricedSurface d1 = make_surface(kUid, S0, S0, kBaseNow + kDayNs);  // +1 day only
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-08-02", "SPX", d1);
  auto clock = Clock::from_manifest(
      make_manifest({{"2026-08-01", p0}, {"2026-08-02", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), 2u);

  // Pure time move: only theta lights up (dS = dvol = dr = 0 exactly).
  EXPECT_EQ(res->pnl_delta[1], 0.0);
  EXPECT_EQ(res->pnl_gamma[1], 0.0);
  EXPECT_EQ(res->pnl_vega[1], 0.0);
  EXPECT_EQ(res->pnl_vanna[1], 0.0);
  EXPECT_EQ(res->pnl_volga[1], 0.0);
  EXPECT_EQ(res->pnl_rho[1], 0.0);
  EXPECT_EQ(res->pnl_charm[1], 0.0);
  EXPECT_NE(res->pnl_theta[1], 0.0);
}

// ── 3. Attribution closes each step ─────────────────────────────────────────
TEST(Backtest, AttributionCloses) {
  const fs::path dir = fresh_dir("attrib");
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", 4);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
  ASSERT_GT(r.size(), 1u);

  for (std::size_t i = 0; i < r.size(); ++i) {
    const double axes = r.pnl_delta[i] + r.pnl_gamma[i] + r.pnl_vega[i] + r.pnl_vanna[i] +
                        r.pnl_volga[i] + r.pnl_theta[i] + r.pnl_rho[i] + r.pnl_charm[i] +
                        r.pnl_unexplained[i];
    const double nonsettle = r.pnl_total[i] - r.pnl_settlement[i];
    EXPECT_NEAR(axes, nonsettle, 1e-9 * (std::fabs(nonsettle) + 1.0)) << "row " << i;
  }
}

// ── 4. Determinism across thread counts ─────────────────────────────────────
TEST(Backtest, Determinism) {
  const fs::path dir = fresh_dir("determinism");
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", 4);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  RunConfig cfg1;
  cfg1.price.n_threads = 1;
  RunConfig cfg4;
  cfg4.price.n_threads = 4;

  auto r1 = run_backtest(*clock, survivor_book(expiry), cfg1);
  auto r4 = run_backtest(*clock, survivor_book(expiry), cfg4);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();
  expect_result_bit_identical(*r1, *r4);
}

// ── 5. Storage granularity ──────────────────────────────────────────────────
TEST(Backtest, Granularity) {
  const fs::path dir = fresh_dir("granularity");
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", 7);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  RunConfig fine;
  fine.record_every_n = 1;
  RunConfig coarse;
  coarse.record_every_n = 2;

  auto rf = run_backtest(*clock, survivor_book(expiry), fine);
  auto rc = run_backtest(*clock, survivor_book(expiry), coarse);
  ASSERT_TRUE(rf.has_value()) << rf.error().to_string();
  ASSERT_TRUE(rc.has_value()) << rc.error().to_string();
  EXPECT_LT(rc->size(), rf->size());  // coarse genuinely downsampled

  // Every coarse row matches the fine row at the same timestamp, bit-for-bit.
  for (std::size_t j = 0; j < rc->size(); ++j) {
    std::size_t fi = rf->size();
    for (std::size_t k = 0; k < rf->size(); ++k) {
      if (rf->ts_ns[k] == rc->ts_ns[j]) {
        fi = k;
        break;
      }
    }
    ASSERT_LT(fi, rf->size()) << "no fine row at coarse ts row " << j;
    EXPECT_TRUE(bits_equal(rc->nav[j], rf->nav[fi])) << j;
    EXPECT_TRUE(bits_equal(rc->pnl_total[j], rf->pnl_total[fi])) << j;
    EXPECT_TRUE(bits_equal(rc->pnl_delta[j], rf->pnl_delta[fi])) << j;
    EXPECT_TRUE(bits_equal(rc->pnl_theta[j], rf->pnl_theta[fi])) << j;
    EXPECT_TRUE(bits_equal(rc->gross_vega[j], rf->gross_vega[fi])) << j;
  }
}

// ── 6. Expiry settlement ────────────────────────────────────────────────────
TEST(Backtest, ExpirySettlement) {
  const fs::path dir = fresh_dir("expiry");
  const std::int64_t now0 = kBaseNow;
  const std::int64_t now1 = kBaseNow + 45 * kDayNs;
  const std::int64_t exp_expiring = kBaseNow + 30 * kDayNs;   // between the two dates
  const std::int64_t exp_survivor = kBaseNow + 200 * kDayNs;  // survives

  const PricedSurface d0 = make_surface(kUid, 100.0, 100.0, now0);
  const PricedSurface d1 = make_surface(kUid, 103.0, 103.0, now1);  // spot up to 103
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-09-15", "SPX", d1);
  auto clock = Clock::from_manifest(
      make_manifest({{"2026-08-01", p0}, {"2026-09-15", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  PortfolioState st;
  // Expiring long call K=95 (settles ITM at S=103) + surviving short put K=100.
  st.lots.push_back(
      Lot{1, OptionContract{kUid, 95.0, 0.0, Side::Call}, +5.0, 100.0, exp_expiring, 0, 0.0});
  st.lots.push_back(
      Lot{2, OptionContract{kUid, 100.0, 0.0, Side::Put}, -2.0, 100.0, exp_survivor, 0, 0.0});

  auto res = run_backtest(*clock, std::move(st));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
  ASSERT_EQ(r.size(), 2u);

  // Hand-compute the settlement: intrinsic(S_shifted) - base_mark on d0.
  const double T_base = (static_cast<double>(exp_expiring) - static_cast<double>(now0)) / kNsPerYear;
  auto mark = d0.fair_value(95.0, T_base, Side::Call);
  ASSERT_TRUE(mark.has_value()) << mark.error().to_string();
  const double intrinsic = std::max(0.0, 103.0 - 95.0);
  const double expected_settle = 5.0 * 100.0 * (intrinsic - *mark);

  EXPECT_NE(r.pnl_settlement[1], 0.0);
  EXPECT_NEAR(r.pnl_settlement[1], expected_settle, 1e-6 * (std::fabs(expected_settle) + 1.0));
  EXPECT_EQ(r.n_open_lots[0], 2.0);  // inception: both lots open
  EXPECT_EQ(r.n_open_lots[1], 1.0);  // after settlement: survivor only

  std::printf("[backtest] settlement=%.4f (intrinsic=%.2f base_mark=%.4f) survivors=%.0f\n",
              r.pnl_settlement[1], intrinsic, *mark, r.n_open_lots[1]);
}
