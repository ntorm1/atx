// atx-vol backtest analytics (Phase B3) gate tests.
//
// Exercises `tearsheet` (standard + attribution + vega-scaled/per-unit-risk) and
// `write_backtest_tsv` (deterministic, bit-exact double round-trip) over both a
// hand-built BacktestResult (to pin the formulas independent of the engine) and
// real strategy runs on synthetic eSSVI corpora (the backtest_test/strategy_test
// make_surface pattern — analytic, no fitting, runs everywhere).
//
// Four gates:
//   1. Math          — every TearSheet field == its hand-computed value (1e-9).
//   2. AttributionCloses — a delta-hedged put with frictions+financing ON:
//      total_return == Σ attr axes + settlement + shares + financing - cost.
//   3. TsvRoundTrip  — write_backtest_tsv -> parse back -> every numeric column
//      (incl. signals + ts_ns) is bit-identical to the source BacktestResult.
//   4. WorkedExamples — the design's Example A (3m 25d put, delta-hedged daily,
//      new clip each day) and Example B (XOM 9m vs SPY 3m 40d strangle, flat
//      vega, roll-at-horizon): each runs end-to-end, its tearsheet attribution
//      closes, and the run is bit-identical at n_threads 1 vs 4.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/api/backtest/backtest.hpp"         // Clock, run_backtest, RunConfig, BacktestResult
#include "atx/vol/api/marketdata/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/api/backtest/dispersion.hpp"       // DispersionUniverse, DispersionConfig, DispersionMember
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // OptionContract, kNsPerYear
#include "atx/vol/api/backtest/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/api/backtest/strategy.hpp"         // DeclarativeStrategy, StrategySpec, DispersionStrategy
#include "atx/vol/api/storage/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/api/fitting/surface_parity.hpp"   // SliceContext
#include "atx/vol/tools/tearsheet.hpp"        // TearSheet, tearsheet, write_backtest_tsv
#include "atx/vol/api/core/types.hpp"            // Side, Result, Status
#include "atx/vol/api/fitting/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUid = 7;
constexpr std::uint32_t kXom = 10;
constexpr std::uint32_t kSpy = 20;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_test's make_surface.
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

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-tearsheet-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// Write `items` (symbol -> surface) as one date's archive; return its path.
[[nodiscard]] std::string
write_archive(const fs::path &dir, const std::string &date,
              const std::vector<std::pair<std::string, const PricedSurface *>> &items) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  std::vector<SurfaceArchiveItem> its;
  its.reserve(items.size());
  for (const auto &[sym, ps] : items) {
    its.push_back(SurfaceArchiveItem{sym, ps});
  }
  const Status st = write_surface_archive_v2_file(path, its);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows (one entry/date).
[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths,
              const std::string &symbol = "MKT") {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
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

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp; // (date, path), ascending
};

// A single-underlying evolving corpus: spot drifts, valuation advances one day.
[[nodiscard]] Corpus make_corpus(const fs::path &dir, const std::string &symbol, int n_dates,
                                 double s0 = 100.0, double drift = 0.004, double vdrift = 0.001) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = s0 * (1.0 + drift * static_cast<double>(d));
    const double vb = vdrift * static_cast<double>(d);
    const PricedSurface s = make_surface(kUid, S, S, now, vb);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_archive(dir, date, {{symbol, &s}}));
  }
  Corpus c;
  c.dp = std::move(dp);
  c.manifest = make_manifest(c.dp, symbol);
  return c;
}

// A two-underlying corpus (XOM + SPY in each date's archive), distinct uids.
[[nodiscard]] Corpus make_multi_corpus(const fs::path &dir, int n_dates) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double sx = 110.0 * (1.0 + 0.003 * static_cast<double>(d));
    const double sy = 450.0 * (1.0 + 0.002 * static_cast<double>(d));
    const PricedSurface xom = make_surface(kXom, sx, sx, now, 0.03);
    const PricedSurface spy = make_surface(kSpy, sy, sy, now, 0.00);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_archive(dir, date, {{"XOM", &xom}, {"SPY", &spy}}));
  }
  Corpus c;
  c.dp = std::move(dp);
  c.manifest = make_manifest(c.dp);
  return c;
}

// The closure sum the design pins as a gate.
[[nodiscard]] double closure_sum(const TearSheet &t) noexcept {
  return t.attr_delta + t.attr_gamma + t.attr_vega + t.attr_vanna + t.attr_volga + t.attr_theta +
         t.attr_rho + t.attr_charm + t.attr_unexplained + t.attr_settlement + t.attr_shares +
         t.attr_financing - t.attr_cost;
}

// Every numeric column of two BacktestResults is bit-identical (determinism).
void expect_result_bit_identical(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  const std::vector<std::pair<const std::vector<double> *, const std::vector<double> *>> cols = {
      {&a.pnl_total, &b.pnl_total},
      {&a.pnl_delta, &b.pnl_delta},
      {&a.pnl_gamma, &b.pnl_gamma},
      {&a.pnl_vega, &b.pnl_vega},
      {&a.pnl_vanna, &b.pnl_vanna},
      {&a.pnl_volga, &b.pnl_volga},
      {&a.pnl_theta, &b.pnl_theta},
      {&a.pnl_rho, &b.pnl_rho},
      {&a.pnl_charm, &b.pnl_charm},
      {&a.pnl_unexplained, &b.pnl_unexplained},
      {&a.pnl_settlement, &b.pnl_settlement},
      {&a.pnl_shares, &b.pnl_shares},
      {&a.financing, &b.financing},
      {&a.cost, &b.cost},
      {&a.nav, &b.nav},
      {&a.cash, &b.cash},
      {&a.gross_delta, &b.gross_delta},
      {&a.gross_gamma, &b.gross_gamma},
      {&a.gross_vega, &b.gross_vega},
      {&a.gross_theta, &b.gross_theta},
      {&a.turnover_notional, &b.turnover_notional},
      {&a.turnover_vega, &b.turnover_vega},
      {&a.n_open_lots, &b.n_open_lots},
      {&a.n_unpriced_lots, &b.n_unpriced_lots},
      {&a.n_unpriced_greeks, &b.n_unpriced_greeks}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    EXPECT_EQ(a.ts_ns[i], b.ts_ns[i]) << i;
    for (const auto &[va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
  }
  ASSERT_EQ(a.step_pnl_total.size(), b.step_pnl_total.size());
  for (std::size_t k = 0; k < a.step_pnl_total.size(); ++k) {
    EXPECT_TRUE(bits_equal(a.step_pnl_total[k], b.step_pnl_total[k])) << k;
  }
  ASSERT_EQ(a.signals.size(), b.signals.size());
  for (std::size_t s = 0; s < a.signals.size(); ++s) {
    EXPECT_EQ(a.signals[s].first, b.signals[s].first);
    ASSERT_EQ(a.signals[s].second.size(), b.signals[s].second.size());
    for (std::size_t i = 0; i < a.signals[s].second.size(); ++i) {
      EXPECT_TRUE(bits_equal(a.signals[s].second[i], b.signals[s].second[i])) << s << "," << i;
    }
  }
}

// A 40-delta strangle leg-template on one underlier (mirrors strategy_test).
[[nodiscard]] LegSpec strangle_leg(std::uint32_t uid, double T, double sign, const char *group) {
  LegSpec leg;
  leg.uid = uid;
  leg.tenor.target_T = T;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.size = SizeSpec{SizeSpec::Kind::Weight, 1.0, sign};
  leg.group = group;
  return leg;
}

} // namespace

// ── 1. Tearsheet math on a hand-built series ────────────────────────────────
TEST(TearSheet, Math) {
  // A 5-row BacktestResult with hand-chosen columns (row 0 = inception).
  BacktestResult r;
  const std::size_t n = 5;
  for (std::size_t i = 0; i < n; ++i) {
    r.date.push_back("d" + std::to_string(i));
    r.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(i) * kDayNs);
  }
  r.pnl_total = {0.0, 10.0, -4.0, 6.0, -2.0};
  r.nav = {0.0, 10.0, 6.0, 12.0, 10.0};
  r.gross_vega = {100.0, 50.0, 200.0, 80.0, 40.0};
  r.gross_gamma = {1.0, 2.0, 3.0, 4.0, 5.0};
  r.turnover_notional = {0.0, 1000.0, 500.0, 2000.0, 0.0};
  r.turnover_vega = {0.0, 5.0, 3.0, 8.0, 0.0};
  r.cost = {0.0, 0.5, 0.25, 1.0, 0.0};
  r.financing = {0.0, 0.1, 0.2, 0.3, 0.4};
  r.pnl_delta = {0.0, 1.0, 2.0, 3.0, 4.0};
  r.pnl_gamma = {0.0, 0.5, 0.5, 0.5, 0.5};
  r.pnl_vega = {0.0, -1.0, -2.0, -3.0, -4.0};
  r.pnl_vanna = {0.0, 0.1, 0.1, 0.1, 0.1};
  r.pnl_volga = {0.0, 0.2, 0.2, 0.2, 0.2};
  r.pnl_theta = {0.0, -0.3, -0.3, -0.3, -0.3};
  r.pnl_rho = {0.0, 0.05, 0.05, 0.05, 0.05};
  r.pnl_charm = {0.0, -0.02, -0.02, -0.02, -0.02};
  r.pnl_unexplained = {0.0, 0.01, 0.01, 0.01, 0.01};
  r.pnl_settlement = {0.0, 0.0, 0.0, 0.0, 3.0};
  r.pnl_shares = {0.0, -5.0, -5.0, -5.0, -5.0};
  r.gross_delta = std::vector<double>(n, 0.0);
  r.gross_theta = std::vector<double>(n, 0.0);
  r.cash = std::vector<double>(n, 0.0);
  r.n_open_lots = {1.0, 1.0, 1.0, 1.0, 1.0};
  ASSERT_EQ(r.size(), n);

  const double ppy = 252.0;
  const TearSheet t = tearsheet(r, ppy);

  const auto expect_close = [](double a, double b, double tol = 1e-9) {
    EXPECT_NEAR(a, b, tol * (std::fabs(b) + 1.0));
  };

  // Independent hand computation of the return-series statistics (rows 1..4).
  const std::vector<double> rets = {10.0, -4.0, 6.0, -2.0};
  double rmean = 0.0;
  for (const double v : rets) {
    rmean += v;
  }
  rmean /= static_cast<double>(rets.size());
  double rss = 0.0;
  for (const double v : rets) {
    rss += (v - rmean) * (v - rmean);
  }
  const double rsd = std::sqrt(rss / static_cast<double>(rets.size() - 1));

  // Standard block.
  expect_close(t.total_return, 10.0);
  expect_close(t.ann_return, rmean * ppy);
  expect_close(t.ann_return, 630.0);
  expect_close(t.ann_vol, rsd * std::sqrt(ppy));
  expect_close(t.sharpe, (rmean * ppy) / (rsd * std::sqrt(ppy)));
  expect_close(t.max_drawdown, 4.0); // peak 12 at row 3, trough 10 at row 4 -> but 6->? see below
  expect_close(t.hit_rate, 0.5);     // 2 of 4 returns > 0
  expect_close(t.avg_turnover, 875.0);
  expect_close(t.total_cost, 1.75);
  expect_close(t.total_financing, 1.0);

  // Attribution totals (independent Σ).
  const auto sum = [&](const std::vector<double> &v) {
    double s = 0.0;
    for (const double x : v) {
      s += x;
    }
    return s;
  };
  expect_close(t.attr_delta, sum(r.pnl_delta));
  expect_close(t.attr_gamma, sum(r.pnl_gamma));
  expect_close(t.attr_vega, sum(r.pnl_vega));
  expect_close(t.attr_vanna, sum(r.pnl_vanna));
  expect_close(t.attr_volga, sum(r.pnl_volga));
  expect_close(t.attr_theta, sum(r.pnl_theta));
  expect_close(t.attr_rho, sum(r.pnl_rho));
  expect_close(t.attr_charm, sum(r.pnl_charm));
  expect_close(t.attr_unexplained, sum(r.pnl_unexplained));
  expect_close(t.attr_settlement, sum(r.pnl_settlement));
  expect_close(t.attr_shares, sum(r.pnl_shares));
  expect_close(t.attr_financing, sum(r.financing));
  expect_close(t.attr_cost, sum(r.cost));

  // Vega-scaled / per-unit-risk.
  expect_close(t.avg_gross_vega, 94.0);
  expect_close(t.avg_gross_gamma, 3.0);
  expect_close(t.return_on_gross_vega, 10.0 / 94.0);
  expect_close(t.pnl_per_vega_traded, 10.0 / 16.0);

  const std::vector<double> xs = {10.0 / 100.0, -4.0 / 50.0, 6.0 / 200.0, -2.0 / 80.0};
  double xmean = 0.0;
  for (const double v : xs) {
    xmean += v;
  }
  xmean /= static_cast<double>(xs.size());
  double xss = 0.0;
  for (const double v : xs) {
    xss += (v - xmean) * (v - xmean);
  }
  const double xsd = std::sqrt(xss / static_cast<double>(xs.size() - 1));
  expect_close(t.vega_adj_sharpe, (xmean / xsd) * std::sqrt(ppy));

  std::printf("[tearsheet] math: sharpe=%.6f mdd=%.2f hit=%.3f avg_turnover=%.1f "
              "ret_on_vega=%.6f vega_adj_sharpe=%.6f pnl_per_vega=%.6f\n",
              t.sharpe, t.max_drawdown, t.hit_rate, t.avg_turnover, t.return_on_gross_vega,
              t.vega_adj_sharpe, t.pnl_per_vega_traded);
}

// ── 2. Attribution closes on a real (frictions + financing ON) run ──────────
TEST(TearSheet, AttributionCloses) {
  const fs::path dir = fresh_dir("attrib");
  const Corpus c = make_corpus(dir, "SPX", 7);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // A 25-delta put, delta-hedged daily, held to expiry, WITH frictions + financing.
  StrategySpec spec;
  spec.name = "delta-hedged-put";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.5;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::Delta, 0.25};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 1e-6};
  DeclarativeStrategy strat{spec};

  RunConfig cfg;
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  cfg.frictions.half_spread_bps = 10.0;
  cfg.frictions.per_contract_cost = 0.5;
  cfg.frictions.hedge_slippage_bps = 2.0;
  cfg.financing.finance_premium = true;
  cfg.financing.borrow_rate = 0.02;
  cfg.financing.initial_cash = 1'000'000.0;

  auto res = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const TearSheet t = tearsheet(*res);

  // Closure: every dollar of NAV change (priced rows i>=1) is attributed to an
  // axis. The engine books INCEPTION-row (row 0) trade frictions to CASH — row 0
  // carries zero PnL by construction (nav[0]==0), and the tearsheet sums cost over
  // ALL rows (per spec), so attr_cost includes the sunk inception cost that
  // total_return does not. Adding cost[0] back makes the identity exact. (For a
  // frictionless run cost[0]==0 and this reduces to the plain closure.)
  const double sumc = closure_sum(t);
  const double resid = std::fabs(t.total_return - sumc);
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0))
      << "total_return=" << t.total_return << " closure=" << sumc
      << " inception_cost=" << res->cost.front();
  std::printf(
      "[tearsheet] attribution-close residual = %.3e (total_return=%.4f inception_cost=%.4f)\n",
      resid, t.total_return, res->cost.front());
}

// ── 3. TSV round-trips bit-exactly (incl. signals + ts_ns) ──────────────────
TEST(TearSheet, TsvRoundTrip) {
  const fs::path dir = fresh_dir("tsv");
  // Dispersion run so the result carries a signal column (implied_corr).
  const std::vector<int> day_off = {0, 5, 10};
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < day_off.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(day_off[d]) * kDayNs;
    const PricedSurface idx = make_surface(1, 500.0, 500.0, now, 0.00);
    const PricedSurface n0 = make_surface(2, 100.0, 100.0, now, 0.02);
    const PricedSurface n1 = make_surface(3, 120.0, 120.0, now, 0.03);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-10-%02d", static_cast<int>(d + 1));
    const std::string date = buf;
    dp.emplace_back(date, write_archive(dir, date, {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}}));
  }
  auto clock = Clock::from_manifest(make_manifest(dp));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"IDX", 1, 0.0};
  u.names.push_back(DispersionMember{"NM0", 2, 0.6});
  u.names.push_back(DispersionMember{"NM1", 3, 0.4});
  DispersionConfig dcfg;
  dcfg.record_diagnostics = true;
  DispersionStrategy strat{u, dcfg};

  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult &r = *res;
  ASSERT_GT(r.signals.size(), 0u) << "expected a signal column to exercise the signal TSV path";

  const std::string path = (dir / "run.tsv").string();
  const Status st = write_backtest_tsv(r, path);
  ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());

  // Parse the TSV back: split into rows of tab-separated cells.
  std::ifstream is(path, std::ios::binary);
  ASSERT_TRUE(is.good());
  std::string content((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  std::vector<std::vector<std::string>> table;
  {
    std::size_t start = 0;
    while (start <= content.size()) {
      const std::size_t nl = content.find('\n', start);
      if (nl == std::string::npos) {
        break; // trailing content after the last '\n' (none expected)
      }
      const std::string line = content.substr(start, nl - start);
      std::vector<std::string> cells;
      std::size_t cs = 0;
      while (true) {
        const std::size_t tab = line.find('\t', cs);
        if (tab == std::string::npos) {
          cells.push_back(line.substr(cs));
          break;
        }
        cells.push_back(line.substr(cs, tab - cs));
        cs = tab + 1;
      }
      table.push_back(std::move(cells));
      start = nl + 1;
    }
  }
  ASSERT_GE(table.size(), 1u);
  const std::vector<std::string> &header = table.front();
  ASSERT_EQ(table.size(), r.size() + 1); // header + one row per step

  const auto col_index = [&](const std::string &name) -> std::size_t {
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (header[i] == name) {
        return i;
      }
    }
    ADD_FAILURE() << "column not found: " << name;
    return header.size();
  };

  // ts_ns integer column bit-identical.
  {
    const std::size_t ci = col_index("ts_ns");
    ASSERT_LT(ci, header.size());
    for (std::size_t i = 0; i < r.size(); ++i) {
      const long long got = std::strtoll(table[i + 1][ci].c_str(), nullptr, 10);
      EXPECT_EQ(got, static_cast<long long>(r.ts_ns[i])) << "row " << i;
    }
  }

  // Every double column bit-identical after strtod.
  const std::vector<std::pair<std::string, const std::vector<double> *>> dcols = {
      {"pnl_total", &r.pnl_total},
      {"pnl_delta", &r.pnl_delta},
      {"pnl_gamma", &r.pnl_gamma},
      {"pnl_vega", &r.pnl_vega},
      {"pnl_vanna", &r.pnl_vanna},
      {"pnl_volga", &r.pnl_volga},
      {"pnl_theta", &r.pnl_theta},
      {"pnl_rho", &r.pnl_rho},
      {"pnl_charm", &r.pnl_charm},
      {"pnl_unexplained", &r.pnl_unexplained},
      {"pnl_settlement", &r.pnl_settlement},
      {"pnl_shares", &r.pnl_shares},
      {"financing", &r.financing},
      {"cost", &r.cost},
      {"nav", &r.nav},
      {"cash", &r.cash},
      {"gross_delta", &r.gross_delta},
      {"gross_gamma", &r.gross_gamma},
      {"gross_vega", &r.gross_vega},
      {"gross_theta", &r.gross_theta},
      {"turnover_notional", &r.turnover_notional},
      {"turnover_vega", &r.turnover_vega},
      {"n_open_lots", &r.n_open_lots},
      {"n_unpriced_lots", &r.n_unpriced_lots},
      {"n_unpriced_greeks", &r.n_unpriced_greeks},
  };
  std::size_t checked = 0;
  for (const auto &[name, col] : dcols) {
    const std::size_t ci = col_index(name);
    ASSERT_LT(ci, header.size()) << name;
    for (std::size_t i = 0; i < r.size(); ++i) {
      const double got = std::strtod(table[i + 1][ci].c_str(), nullptr);
      EXPECT_TRUE(bits_equal(got, (*col)[i])) << name << " row " << i;
      ++checked;
    }
  }

  // Signal columns bit-identical.
  for (const auto &sig : r.signals) {
    const std::size_t ci = col_index(sig.first);
    ASSERT_LT(ci, header.size()) << sig.first;
    for (std::size_t i = 0; i < r.size(); ++i) {
      const double got = std::strtod(table[i + 1][ci].c_str(), nullptr);
      EXPECT_TRUE(bits_equal(got, sig.second[i])) << sig.first << " row " << i;
    }
  }

  std::printf("[tearsheet] tsv round-trip: %zu rows, %zu numeric cells bit-exact, %zu signal(s)\n",
              r.size(), checked, r.signals.size());

  // Plan 4.6: the same writer over a SKEWED copy is a reported shape error, not
  // an out-of-range read. `append_backtest_series_tsv` walks the columns in row
  // lockstep, so this is the seam the invariant protects. Nothing is written.
  BacktestResult skewed = r;
  skewed.gross_vega.pop_back();
  const std::string skew_path = (dir / "skewed.tsv").string();
  const Status skew_st = write_backtest_tsv(skewed, skew_path);
  ASSERT_FALSE(skew_st.has_value()) << "a skewed result must not be written";
  EXPECT_EQ(skew_st.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(skew_st.error().message().find("gross_vega"), std::string::npos)
      << skew_st.error().message();
  EXPECT_FALSE(fs::exists(skew_path));

  // A skewed SIGNAL series is caught on the same path (signals are dynamic
  // columns of the same TSV).
  BacktestResult skewed_sig = r;
  skewed_sig.signals.front().second.pop_back();
  EXPECT_FALSE(write_backtest_tsv(skewed_sig, (dir / "skewed_sig.tsv").string()).has_value());
}

// ── 4a. Worked Example A: 3m 25d put, delta-hedged daily, new clip each day ──
TEST(TearSheet, WorkedExampleA) {
  const fs::path dir = fresh_dir("exampleA");
  const Corpus c = make_corpus(dir, "SPY", 12, 100.0, 0.004, 0.0008);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.name = "spy-3m-25d-put-daily-clip";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::Delta, 0.25};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};

  // (a) runs; (b) attribution closes; (c) deterministic across thread counts.
  DeclarativeStrategy s1{spec};
  DeclarativeStrategy s4{spec};
  RunConfig cfg1;
  cfg1.price.n_threads = 1;
  RunConfig cfg4;
  cfg4.price.n_threads = 4;
  auto r1 = run_backtest(*clock, s1, cfg1);
  auto r4 = run_backtest(*clock, s4, cfg4);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();
  ASSERT_GT(r1->size(), 1u);

  const TearSheet t = tearsheet(*r1);
  // Inception-cost-aware closure (see AttributionCloses); r1 is frictionless so
  // cost[0]==0 and this is the plain identity.
  const double resid = std::fabs(t.total_return - closure_sum(t));
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0));
  expect_result_bit_identical(*r1, *r4);

  std::printf("[tearsheet] Example A: total_return=%.4f sharpe=%.4f mdd=%.4f "
              "avg_gross_vega=%.2f ret_on_vega=%.6f closure_resid=%.3e (det=OK)\n",
              t.total_return, t.sharpe, t.max_drawdown, t.avg_gross_vega, t.return_on_gross_vega,
              resid);
}

// ── 4b. Worked Example B: XOM 9m vs SPY 3m 40d strangle, flat vega, roll ─────
TEST(TearSheet, WorkedExampleB) {
  const fs::path dir = fresh_dir("exampleB");
  const Corpus c = make_multi_corpus(dir, 10);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.name = "xom9m-vs-spy3m-40d-strangle-flat-vega";
  spec.legs.push_back(strangle_leg(kXom, 0.75, +1.0, "a")); // long XOM 9m
  spec.legs.push_back(strangle_leg(kSpy, 0.25, -1.0, "b")); // short SPY 3m
  spec.constraint = CrossLegConstraint{CrossLegConstraint::Kind::FlatVega, "a", "b"};
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.entry_every_n = 21;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::None, HedgeSpec::Cadence::Daily, 0.0};

  DeclarativeStrategy s1{spec};
  DeclarativeStrategy s4{spec};
  RunConfig cfg1;
  cfg1.price.n_threads = 1;
  RunConfig cfg4;
  cfg4.price.n_threads = 4;
  auto r1 = run_backtest(*clock, s1, cfg1);
  auto r4 = run_backtest(*clock, s4, cfg4);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();
  ASSERT_GT(r1->size(), 1u);

  const TearSheet t = tearsheet(*r1);
  // Inception-cost-aware closure (see AttributionCloses); r1 is frictionless so
  // cost[0]==0 and this is the plain identity.
  const double resid = std::fabs(t.total_return - closure_sum(t));
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0));
  expect_result_bit_identical(*r1, *r4);

  std::printf("[tearsheet] Example B: total_return=%.4f sharpe=%.4f avg_gross_vega=%.2f "
              "pnl_per_vega_traded=%.6f closure_resid=%.3e (det=OK)\n",
              t.total_return, t.sharpe, t.avg_gross_vega, t.pnl_per_vega_traded, resid);
}

// ── 5. record_every_n stride-invariance of annualized statistics (C1 regression) ─
//
// The corruption: with record_every_n>1 each recorded row stored only its OWN
// step's pnl_*, so the tearsheet computed Sharpe / ann_return / ann_vol / hit_rate
// and the attribution totals off a 1-in-stride SAMPLE — silently wrong, while
// `nav` stayed correct. The fix retains the TRUE per-step series (`step_pnl_total`)
// for the risk statistics and block-sums the flow columns into the recorded rows
// for the attribution totals. This test proves a run's headline statistics are
// IDENTICAL across record strides (the property the old code violated).
TEST(TearSheet, StrideInvariantAnnualizedStats) {
  const fs::path dir = fresh_dir("stride-invariance");
  const Corpus c = make_corpus(dir, "SPX", 16, 100.0, 0.004, 0.0008);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // A 25-delta put, delta-hedged daily, WITH frictions + financing so the shares /
  // financing / cost flow columns are all exercised through the block-sum path.
  StrategySpec spec;
  spec.name = "stride-invariance-put";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.5;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::Delta, 0.25};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 1e-6};

  const auto run_stride = [&](unsigned k) {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.record_every_n = k;
    cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
    cfg.frictions.half_spread_bps = 10.0;
    cfg.frictions.per_contract_cost = 0.5;
    cfg.financing.finance_premium = true;
    cfg.financing.borrow_rate = 0.02;
    cfg.financing.initial_cash = 1'000'000.0;
    return run_backtest(*clock, strat, cfg);
  };

  auto r1 = run_stride(1);
  auto r2 = run_stride(2);
  auto r3 = run_stride(3);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r2.has_value()) << r2.error().to_string();
  ASSERT_TRUE(r3.has_value()) << r3.error().to_string();
  EXPECT_LT(r2->size(), r1->size()); // downsampling really happened
  EXPECT_LT(r3->size(), r1->size());
  // The full-resolution per-step series is stride-independent.
  ASSERT_EQ(r1->step_pnl_total.size(), r2->step_pnl_total.size());
  ASSERT_EQ(r1->step_pnl_total.size(), r3->step_pnl_total.size());
  for (std::size_t k = 0; k < r1->step_pnl_total.size(); ++k) {
    EXPECT_TRUE(bits_equal(r2->step_pnl_total[k], r1->step_pnl_total[k])) << k;
    EXPECT_TRUE(bits_equal(r3->step_pnl_total[k], r1->step_pnl_total[k])) << k;
  }

  const TearSheet t1 = tearsheet(*r1);
  const TearSheet t2 = tearsheet(*r2);
  const TearSheet t3 = tearsheet(*r3);

  // Headline annualized statistics: computed off the TRUE per-step series, hence
  // bit-for-bit identical regardless of the record stride. (Pre-fix these were a
  // 1-in-stride sample and diverged.)
  for (const TearSheet *tk : {&t2, &t3}) {
    EXPECT_TRUE(bits_equal(tk->ann_return, t1.ann_return));
    EXPECT_TRUE(bits_equal(tk->ann_vol, t1.ann_vol));
    EXPECT_TRUE(bits_equal(tk->sharpe, t1.sharpe));
    EXPECT_TRUE(bits_equal(tk->hit_rate, t1.hit_rate));
    EXPECT_TRUE(bits_equal(tk->total_return, t1.total_return));
  }

  // Attribution + cost/financing TOTALS: block-summed columns, so equal to the
  // stride-1 totals within a tight tolerance (block grouping reorders the sum).
  const double tol = 1e-9;
  const auto near = [&](double a, double b) { EXPECT_NEAR(a, b, tol * (std::fabs(b) + 1.0)); };
  for (const TearSheet *tk : {&t2, &t3}) {
    near(tk->attr_delta, t1.attr_delta);
    near(tk->attr_gamma, t1.attr_gamma);
    near(tk->attr_vega, t1.attr_vega);
    near(tk->attr_theta, t1.attr_theta);
    near(tk->attr_settlement, t1.attr_settlement);
    near(tk->attr_shares, t1.attr_shares);
    near(tk->attr_financing, t1.attr_financing);
    near(tk->attr_cost, t1.attr_cost);
    near(tk->total_cost, t1.total_cost);
    near(tk->total_financing, t1.total_financing);
  }

  std::printf("[tearsheet] stride-invariance: sharpe(s1/s2/s3)=%.10f/%.10f/%.10f "
              "ann_return=%.6f ann_vol=%.6f hit=%.4f (rows %zu/%zu/%zu)\n",
              t1.sharpe, t2.sharpe, t3.sharpe, t1.ann_return, t1.ann_vol, t1.hit_rate, r1->size(),
              r2->size(), r3->size());
}

// ── B4: rigor tearsheet — PSR / DSR / MinTRL + attribution residual alarm ──
//
// Every pinned constant below was computed OFFLINE in Python, using the
// standard-library `statistics.NormalDist` (an INDEPENDENT normal CDF/PPF
// implementation from this file's `atx::core::norm_cdf` / this TU's Acklam-
// based `norm_ppf` detail function) — not this library, so the expected
// values are not tautological. Full derivations are in each test's comment.

// 1. PSR reference value.
TEST(TearSheet, PsrReferenceValue) {
  // Python (statistics.NormalDist):
  //   sr, skew, kurt, T, benchmark = 0.08, -0.3, 4.5, 60, 0.0
  //   var_term = 1 - skew*sr + ((kurt-1)/4)*sr^2
  //            = 1 - (-0.3)(0.08) + (3.5/4)(0.0064)
  //            = 1 + 0.024 + 0.0056 = 1.0296
  //   z = (sr-benchmark)*sqrt(T-1)/sqrt(var_term)
  //     = 0.08*sqrt(59)/sqrt(1.0296) = 0.605594226148472
  //   PSR = Phi(z) = 0.7276078813197
  const double p = psr(0.08, -0.3, 4.5, 60, 0.0);
  EXPECT_NEAR(p, 0.7276078813197, 1e-9);
}

// 2. PSR domain guards.
TEST(TearSheet, PsrDomainGuards) {
  // T < 2: sqrt(T-1) is 0 (or undefined below 1), refused rather than
  // silently returning Phi(0)==0.5 for every input.
  EXPECT_TRUE(std::isnan(psr(0.10, 0.0, 3.0, 1, 0.0)));
  EXPECT_TRUE(std::isnan(psr(0.10, 0.0, 3.0, 0, 0.0)));

  // Degenerate variance term: sr=10, skew=10, kurt=1 ->
  //   1 - 10*10 + ((1-1)/4)*100 = 1 - 100 + 0 = -99 <= 0.
  EXPECT_TRUE(std::isnan(psr(10.0, 10.0, 1.0, 60, 0.0)));

  // sr == benchmark is NOT degenerate: PSR == 0.5 exactly (Phi(0)).
  EXPECT_NEAR(psr(0.10, -0.2, 3.5, 100, 0.10), 0.5, 1e-12);
}

// 3. DSR reference value — the brief's own worked example.
TEST(TearSheet, DsrReferenceValue_BriefExample) {
  // Python (statistics.NormalDist):
  //   sr, skew, kurt, T, N = 1.0, -1.0, 5.0, 252, 100
  //   var_term = 1 - (-1.0)(1.0) + ((5.0-1)/4)(1.0)^2 = 1 + 1 + 1 = 3.0
  //   V_hat    = var_term / T = 3.0 / 252 = 0.011904761904761904
  //     (the single-stream Sharpe-variance estimator implied by the same
  //     skew/kurt/T -- a representative TrialStats.sr_variance when the
  //     trial catalog's own cross-trial variance is unavailable; the same
  //     convention atx-engine/eval/deflated_sharpe.hpp falls back to.)
  //   gammaE = 0.5772156649015329
  //   q_hi = Phi^-1(1 - 1/N)     = Phi^-1(0.99)             = 2.3263478740408408
  //   q_lo = Phi^-1(1 - 1/(N*e)) = Phi^-1(0.9963212055882856) = 2.680210444966887
  //   max_z = (1-gammaE)*q_hi + gammaE*q_lo = 2.5306028932016846
  //   SR0   = sqrt(V_hat) * max_z = 0.27611141218978497
  //   z     = (sr-SR0)*sqrt(T-1)/sqrt(var_term) = 6.621371624722196
  //   DSR   = Phi(z) = 0.999999999982206
  const TrialStats trials{100, 0.011904761904761904};
  const double d = dsr(1.0, -1.0, 5.0, 252, trials);
  EXPECT_NEAR(d, 0.999999999982206, 1e-9);
}

// 4. DSR reference value — a second, less-saturated pin (DsrReferenceValue_
// BriefExample lands PSR at ~1.0, which cannot discriminate a sign/formula
// error in the selection-benchmark term as sharply as a mid-range value can).
TEST(TearSheet, DsrReferenceValue_Discriminating) {
  // Python (statistics.NormalDist):
  //   sr, skew, kurt, T, N = 0.10, -0.4, 4.2, 120, 50
  //   var_term = 1 - (-0.4)(0.10) + ((4.2-1)/4)(0.10)^2
  //            = 1 + 0.04 + 0.008 = 1.048
  //   V_hat    = 1.048 / 120 = 0.008733333333333334
  //   q_hi = Phi^-1(1 - 1/50)      = Phi^-1(0.98)              = 2.053748910631822
  //   q_lo = Phi^-1(1 - 1/(50*e))  = Phi^-1(0.9926424111765713) = 2.4393139538578943
  //   max_z = (1-gammaE)*q_hi + gammaE*q_lo = 2.276303093420348
  //   SR0   = sqrt(V_hat) * max_z = 0.2127257712452147
  //   z     = (0.10-0.2127257712452147)*sqrt(119)/sqrt(1.048) = -1.2012020223543558
  //   DSR   = Phi(z) = 0.1148364225606856
  const TrialStats trials{50, 0.008733333333333334};
  const double d = dsr(0.10, -0.4, 4.2, 120, trials);
  EXPECT_NEAR(d, 0.1148364225606856, 1e-9);
}

// 5. N<=1 ("no selection") collapses SR0 to 0, so DSR == PSR(sr,...,T,0.0).
// This is a structural identity between two calls of THIS library (not an
// offline pin), complementing the hand-computed constants above.
TEST(TearSheet, DsrNoSelectionCollapsesToPsrAtZero) {
  const double baseline = psr(0.12, -0.2, 3.8, 500, 0.0);
  EXPECT_NEAR(dsr(0.12, -0.2, 3.8, 500, TrialStats{1, 0.05}), baseline, 1e-12);
  EXPECT_NEAR(dsr(0.12, -0.2, 3.8, 500, TrialStats{0, 0.05}), baseline, 1e-12);
  // sr_variance is irrelevant at N<=1 -- the Z^-1(1-1/N) divergence guard
  // fires regardless of what V[SR] the caller supplied.
  EXPECT_NEAR(dsr(0.12, -0.2, 3.8, 500, TrialStats{1, 999.0}), baseline, 1e-12);
}

// 6. DSR domain guards.
TEST(TearSheet, DsrDomainGuards) {
  // A negative cross-trial variance is a caller/catalog bug, not a "no
  // selection" case -- refused rather than silently sqrt'd into a complex
  // (NaN'd) SR0.
  EXPECT_TRUE(std::isnan(dsr(0.10, 0.0, 3.0, 100, TrialStats{50, -1.0})));
  // The underlying psr() guard (T < 2) still applies through dsr().
  EXPECT_TRUE(std::isnan(dsr(0.10, 0.0, 3.0, 1, TrialStats{50, 0.01})));
  // ...and so does psr()'s degenerate-variance-term guard: dsr's own var_term
  // is computed off the OBSERVED sr/skew/kurt (not SR0), so a pathological
  // combination there (sr=10, skew=10, kurt=1 -> var_term=-99, see
  // PsrDomainGuards) poisons dsr the same way regardless of TrialStats.
  EXPECT_TRUE(std::isnan(dsr(10.0, 10.0, 1.0, 60, TrialStats{50, 0.01})));
}

// 7. MinTRL reference value, with a round-trip consistency check against psr.
TEST(TearSheet, MinTrlReferenceValue) {
  // Python (statistics.NormalDist):
  //   sr, skew, kurt, benchmark, alpha = 0.2, 0.1, 3.5, 0.0, 0.95
  //   var_term = 1 - 0.1*0.2 + ((3.5-1)/4)*0.2^2 = 1 - 0.02 + 0.025 = 1.005
  //   z_alpha  = Phi^-1(0.95) = 1.6448536269514715
  //   diff     = sr - benchmark = 0.2
  //   ratio    = z_alpha / diff = 8.224268134757358
  //   MinTRL   = 1 + var_term*ratio^2 = 68.97677928414718
  //   (round-trip check done in Python too: Phi(z at T=MinTRL) == 0.95 exactly)
  const double trl = min_trl(0.2, 0.1, 3.5, 0.0, 0.95);
  EXPECT_NEAR(trl, 68.97677928414718, 1e-6);

  // Round-trip: plugging MinTRL back into the PSR formula (by hand, since
  // psr() only accepts an integer T) must reproduce alpha.
  const double var_term = 1.0 - 0.1 * 0.2 + ((3.5 - 1.0) / 4.0) * 0.2 * 0.2;
  const double z = (0.2 - 0.0) * std::sqrt(trl - 1.0) / std::sqrt(var_term);
  EXPECT_NEAR(0.5 * std::erfc(-z / std::sqrt(2.0)), 0.95, 1e-9);
}

// 8. MinTRL domain guards.
TEST(TearSheet, MinTrlDomainGuards) {
  EXPECT_TRUE(std::isnan(min_trl(0.10, 0.0, 3.0, 0.0, 0.0)));  // alpha <= 0
  EXPECT_TRUE(std::isnan(min_trl(0.10, 0.0, 3.0, 0.0, 1.0)));  // alpha >= 1
  EXPECT_TRUE(std::isnan(min_trl(10.0, 10.0, 1.0, 0.0, 0.95)));  // var_term <= 0

  // sr == benchmark: a zero effect size needs an infinite track record at any
  // nontrivial confidence -- +infinity is the CORRECT answer, not a NaN.
  const double trl = min_trl(0.10, 0.0, 3.0, 0.10, 0.95);
  EXPECT_TRUE(std::isinf(trl));
  EXPECT_GT(trl, 0.0);
}

namespace {

// A 21-row hand-built BacktestResult (row 0 = inception, rows 1..20 traded)
// with UNIFORM per-row greek P&L, used by the residual-alarm tests below.
// gross(row) = |1.0|+|0.5|+|1.0|+|0.1|+|0.2|+|0.3|+|0.05|+|0.02| = 3.17 per
// row; `unexplained_bump` overrides `pnl_unexplained` on rows
// [bump_start, bump_start+bump_len) (1-indexed into the 20 traded rows).
[[nodiscard]] BacktestResult make_residual_fixture(std::size_t bump_start, std::size_t bump_len,
                                                    double bumped_unexplained) {
  BacktestResult r;
  constexpr std::size_t n = 21;
  for (std::size_t i = 0; i < n; ++i) {
    r.date.push_back("d" + std::to_string(i));
    r.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(i) * kDayNs);
  }
  r.pnl_delta.assign(n, 1.0);
  r.pnl_gamma.assign(n, 0.5);
  r.pnl_vega.assign(n, -1.0);
  r.pnl_vanna.assign(n, 0.1);
  r.pnl_volga.assign(n, 0.2);
  r.pnl_theta.assign(n, -0.3);
  r.pnl_rho.assign(n, 0.05);
  r.pnl_charm.assign(n, -0.02);
  r.pnl_unexplained.assign(n, 0.01);
  r.pnl_delta[0] = r.pnl_gamma[0] = r.pnl_vega[0] = r.pnl_vanna[0] = r.pnl_volga[0] =
      r.pnl_theta[0] = r.pnl_rho[0] = r.pnl_charm[0] = r.pnl_unexplained[0] = 0.0;  // inception
  for (std::size_t k = 0; k < bump_len; ++k) {
    r.pnl_unexplained[bump_start + k] = bumped_unexplained;
  }
  EXPECT_EQ(r.size(), n);  // ASSERT_* cannot be used in a non-void helper
  return r;
}

}  // namespace

// 9. A clean book (uniform, tiny unexplained residual) never trips.
TEST(TearSheet, UnexplainedAlarmCleanBookDoesNotTrip) {
  const BacktestResult r = make_residual_fixture(/*bump_start=*/0, /*bump_len=*/0, 0.0);
  const ResidualAlarm alarm = unexplained_alarm(r, /*window=*/5, /*tolerance=*/0.10);
  EXPECT_FALSE(alarm.tripped);
  // Every window is identical: ratio == 0.01 / 3.17.
  EXPECT_NEAR(alarm.worst_ratio, 0.01 / 3.17, 1e-9);
}

// 10. A deliberately mis-marked greek (5 consecutive rows whose
// pnl_unexplained is bumped from 0.01 to 2.0, simulating a mismarked greek
// whose Taylor residual blew up) trips the alarm, and the worst window is
// exactly the one fully covering the mismark.
TEST(TearSheet, UnexplainedAlarmTripsOnMismarkedGreek) {
  const BacktestResult r = make_residual_fixture(/*bump_start=*/11, /*bump_len=*/5, 2.0);
  const ResidualAlarm alarm = unexplained_alarm(r, /*window=*/5, /*tolerance=*/0.10);
  EXPECT_TRUE(alarm.tripped);
  // Fully-overlapping window: unexpl_sum = 5*2.0 = 10.0, gross_sum = 5*3.17 =
  // 15.85, ratio = 10.0/15.85 = 0.6309148580968...
  EXPECT_NEAR(alarm.worst_ratio, 10.0 / 15.85, 1e-9);
  EXPECT_EQ(alarm.worst_window_start, 11u);
}

// 11. Degenerate inputs form no window and report the zero-initialized,
// non-tripped result rather than crash or divide by zero.
TEST(TearSheet, UnexplainedAlarmDegenerateInputsFormNoWindow) {
  const BacktestResult r = make_residual_fixture(0, 0, 0.0);

  // window == 0: cannot form a window.
  {
    const ResidualAlarm alarm = unexplained_alarm(r, /*window=*/0, /*tolerance=*/0.10);
    EXPECT_FALSE(alarm.tripped);
    EXPECT_EQ(alarm.worst_ratio, 0.0);
  }
  // window > row count: cannot form a window.
  {
    const ResidualAlarm alarm = unexplained_alarm(r, /*window=*/100, /*tolerance=*/0.10);
    EXPECT_FALSE(alarm.tripped);
    EXPECT_EQ(alarm.worst_ratio, 0.0);
  }
  // Size-mismatched columns (a partially hand-built result): must not read
  // out of bounds, must fail safe to the empty result.
  {
    BacktestResult bad;
    bad.date = {"d0", "d1", "d2"};
    bad.ts_ns = {kBaseNow, kBaseNow + kDayNs, kBaseNow + 2 * kDayNs};
    bad.pnl_delta = {0.0, 1.0};  // short by one row
    const ResidualAlarm alarm = unexplained_alarm(bad, /*window=*/2, /*tolerance=*/0.10);
    EXPECT_FALSE(alarm.tripped);
    EXPECT_EQ(alarm.worst_ratio, 0.0);
  }
}
