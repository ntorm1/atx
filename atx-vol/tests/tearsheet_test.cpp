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

#include "atx/vol/american.hpp"          // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"          // Clock, run_backtest, RunConfig, BacktestResult
#include "atx/vol/corpus.hpp"            // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/dispersion.hpp"        // DispersionUniverse, DispersionConfig, DispersionMember
#include "atx/vol/portfolio_pricer.hpp"  // OptionContract, kNsPerYear
#include "atx/vol/priced_surface.hpp"    // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"          // DeclarativeStrategy, StrategySpec, DispersionStrategy
#include "atx/vol/surface_archive.hpp"   // write_surface_archive_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"    // SliceContext
#include "atx/vol/tearsheet.hpp"         // TearSheet, tearsheet, write_backtest_tsv
#include "atx/vol/types.hpp"             // Side, Result, Status
#include "atx/vol/vol_curve.hpp"         // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"       // EssviParams

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

[[nodiscard]] fs::path fresh_dir(const char* tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-tearsheet-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// Write `items` (symbol -> surface) as one date's archive; return its path.
[[nodiscard]] std::string write_archive(
    const fs::path& dir, const std::string& date,
    const std::vector<std::pair<std::string, const PricedSurface*>>& items) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  std::vector<SurfaceArchiveItem> its;
  its.reserve(items.size());
  for (const auto& [sym, ps] : items) {
    its.push_back(SurfaceArchiveItem{sym, ps});
  }
  const Status st = write_surface_archive_file(path, its);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows (one entry/date).
[[nodiscard]] CorpusManifest make_manifest(
    const std::vector<std::pair<std::string, std::string>>& date_paths,
    const std::string& symbol = "MKT") {
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

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp;  // (date, path), ascending
};

// A single-underlying evolving corpus: spot drifts, valuation advances one day.
[[nodiscard]] Corpus make_corpus(const fs::path& dir, const std::string& symbol, int n_dates,
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
[[nodiscard]] Corpus make_multi_corpus(const fs::path& dir, int n_dates) {
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
[[nodiscard]] double closure_sum(const TearSheet& t) noexcept {
  return t.attr_delta + t.attr_gamma + t.attr_vega + t.attr_vanna + t.attr_volga + t.attr_theta +
         t.attr_rho + t.attr_charm + t.attr_unexplained + t.attr_settlement + t.attr_shares +
         t.attr_financing - t.attr_cost;
}

// Every numeric column of two BacktestResults is bit-identical (determinism).
void expect_result_bit_identical(const BacktestResult& a, const BacktestResult& b) {
  ASSERT_EQ(a.size(), b.size());
  const std::vector<std::pair<const std::vector<double>*, const std::vector<double>*>> cols = {
      {&a.pnl_total, &b.pnl_total},         {&a.pnl_delta, &b.pnl_delta},
      {&a.pnl_gamma, &b.pnl_gamma},         {&a.pnl_vega, &b.pnl_vega},
      {&a.pnl_vanna, &b.pnl_vanna},         {&a.pnl_volga, &b.pnl_volga},
      {&a.pnl_theta, &b.pnl_theta},         {&a.pnl_rho, &b.pnl_rho},
      {&a.pnl_charm, &b.pnl_charm},         {&a.pnl_unexplained, &b.pnl_unexplained},
      {&a.pnl_settlement, &b.pnl_settlement}, {&a.pnl_shares, &b.pnl_shares},
      {&a.financing, &b.financing},         {&a.cost, &b.cost},
      {&a.nav, &b.nav},                     {&a.cash, &b.cash},
      {&a.gross_delta, &b.gross_delta},     {&a.gross_gamma, &b.gross_gamma},
      {&a.gross_vega, &b.gross_vega},       {&a.gross_theta, &b.gross_theta},
      {&a.turnover_notional, &b.turnover_notional}, {&a.turnover_vega, &b.turnover_vega},
      {&a.n_open_lots, &b.n_open_lots}, {&a.n_unpriced_lots, &b.n_unpriced_lots},
      {&a.n_unpriced_greeks, &b.n_unpriced_greeks}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    EXPECT_EQ(a.ts_ns[i], b.ts_ns[i]) << i;
    for (const auto& [va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
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
[[nodiscard]] LegSpec strangle_leg(std::uint32_t uid, double T, double sign, const char* group) {
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

}  // namespace

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
  expect_close(t.max_drawdown, 4.0);  // peak 12 at row 3, trough 10 at row 4 -> but 6->? see below
  expect_close(t.hit_rate, 0.5);      // 2 of 4 returns > 0
  expect_close(t.avg_turnover, 875.0);
  expect_close(t.total_cost, 1.75);
  expect_close(t.total_financing, 1.0);

  // Attribution totals (independent Σ).
  const auto sum = [&](const std::vector<double>& v) {
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

  std::printf(
      "[tearsheet] math: sharpe=%.6f mdd=%.2f hit=%.3f avg_turnover=%.1f "
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
  const double sumc = closure_sum(t) + res->cost.front();
  const double resid = std::fabs(t.total_return - sumc);
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0))
      << "total_return=" << t.total_return << " closure=" << sumc
      << " inception_cost=" << res->cost.front();
  std::printf("[tearsheet] attribution-close residual = %.3e (total_return=%.4f inception_cost=%.4f)\n",
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
  const DispersionConfig dcfg;
  DispersionStrategy strat{u, dcfg};

  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
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
        break;  // trailing content after the last '\n' (none expected)
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
  const std::vector<std::string>& header = table.front();
  ASSERT_EQ(table.size(), r.size() + 1);  // header + one row per step

  const auto col_index = [&](const std::string& name) -> std::size_t {
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
  const std::vector<std::pair<std::string, const std::vector<double>*>> dcols = {
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
  for (const auto& [name, col] : dcols) {
    const std::size_t ci = col_index(name);
    ASSERT_LT(ci, header.size()) << name;
    for (std::size_t i = 0; i < r.size(); ++i) {
      const double got = std::strtod(table[i + 1][ci].c_str(), nullptr);
      EXPECT_TRUE(bits_equal(got, (*col)[i])) << name << " row " << i;
      ++checked;
    }
  }

  // Signal columns bit-identical.
  for (const auto& sig : r.signals) {
    const std::size_t ci = col_index(sig.first);
    ASSERT_LT(ci, header.size()) << sig.first;
    for (std::size_t i = 0; i < r.size(); ++i) {
      const double got = std::strtod(table[i + 1][ci].c_str(), nullptr);
      EXPECT_TRUE(bits_equal(got, sig.second[i])) << sig.first << " row " << i;
    }
  }

  std::printf("[tearsheet] tsv round-trip: %zu rows, %zu numeric cells bit-exact, %zu signal(s)\n",
              r.size(), checked, r.signals.size());
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
  const double resid = std::fabs(t.total_return - (closure_sum(t) + r1->cost.front()));
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0));
  expect_result_bit_identical(*r1, *r4);

  std::printf(
      "[tearsheet] Example A: total_return=%.4f sharpe=%.4f mdd=%.4f "
      "avg_gross_vega=%.2f ret_on_vega=%.6f closure_resid=%.3e (det=OK)\n",
      t.total_return, t.sharpe, t.max_drawdown, t.avg_gross_vega, t.return_on_gross_vega, resid);
}

// ── 4b. Worked Example B: XOM 9m vs SPY 3m 40d strangle, flat vega, roll ─────
TEST(TearSheet, WorkedExampleB) {
  const fs::path dir = fresh_dir("exampleB");
  const Corpus c = make_multi_corpus(dir, 10);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.name = "xom9m-vs-spy3m-40d-strangle-flat-vega";
  spec.legs.push_back(strangle_leg(kXom, 0.75, +1.0, "a"));  // long XOM 9m
  spec.legs.push_back(strangle_leg(kSpy, 0.25, -1.0, "b"));  // short SPY 3m
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
  const double resid = std::fabs(t.total_return - (closure_sum(t) + r1->cost.front()));
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0));
  expect_result_bit_identical(*r1, *r4);

  std::printf(
      "[tearsheet] Example B: total_return=%.4f sharpe=%.4f avg_gross_vega=%.2f "
      "pnl_per_vega_traded=%.6f closure_resid=%.3e (det=OK)\n",
      t.total_return, t.sharpe, t.avg_gross_vega, t.pnl_per_vega_traded, resid);
}
