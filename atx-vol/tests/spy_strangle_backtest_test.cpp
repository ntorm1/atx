// atx-vol backtest — SPY short 40-delta 6m strangle, restriked daily: the
// correctness GATE for the examples/spy_strangle_backtest.cpp deliverable.
//
// Self-contained (synthetic eSSVI surfaces via the make_surface pattern; no fit,
// no data dependency). Pins the four properties that make the deliverable
// "correct":
//
//   1. Restrike invariant — a SINGLE cohort restruck every step: every recorded
//      row carries EXACTLY 2 open lots (a call + a put), never accumulating (the
//      RollAtHorizon-with-roll_at_T-above-tenor mechanic), and the strikes MOVE
//      across dates as spot drifts (it is genuinely restruck, not held).
//   2. 40-delta entry — the resolved call/put strikes reprice to |delta| ~ 0.40.
//   3. Short sign — the book is short vega/gamma every row (short a strangle).
//   4. Closure + determinism — the tearsheet attribution closes and the run is
//      bit-identical at n_threads 1 vs 4.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // al_fast_opts, AmericanMethod, AmericanGreeks
#include "atx/vol/backtest.hpp"         // Clock, run_backtest, RunConfig, BacktestResult, MarketSnapshot
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"         // DeclarativeStrategy, StrategySpec, resolve_strike_by_delta
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/tearsheet.hpp"        // TearSheet, tearsheet
#include "atx/vol/types.hpp"            // Side, Result, Status
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kSpy = 42;
constexpr double kTenorT = 0.5;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] PricedSurface make_surface(double S, std::int64_t now_ts, double vol_bump) {
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
    e.F = S;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, S, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = kSpy;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

[[nodiscard]] std::string write_archive(const fs::path& dir, const std::string& date,
                                        const PricedSurface& spy) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{"SPY", &spy};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp;
  std::vector<double> spots;
};

// A short evolving SPY corpus: spot drifts up, valuation advances one day.
[[nodiscard]] Corpus make_corpus(const fs::path& dir, int n_dates) {
  Corpus c;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = 600.0 * (1.0 + 0.004 * static_cast<double>(d));
    const double vb = 0.002 * static_cast<double>(d);
    const PricedSurface spy = make_surface(S, now, vb);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-03-%02d", d + 1);
    const std::string date = buf;
    c.dp.emplace_back(date, write_archive(dir, date, spy));
    c.spots.push_back(S);
    c.manifest.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "SPY";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = c.dp.back().second;
    c.manifest.entries.push_back(std::move(e));
  }
  return c;
}

[[nodiscard]] StrategySpec make_spec() {
  StrategySpec spec;
  spec.name = "spy-short-40d-6m-strangle-daily-restrike";
  LegSpec leg;
  leg.uid = kSpy;
  leg.tenor.target_T = kTenorT;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, -1.0};  // SHORT
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 1.0;  // > tenor => restrike every step
  spec.hedge = HedgeSpec{HedgeSpec::Kind::None, HedgeSpec::Cadence::Daily, 0.0};
  return spec;
}

[[nodiscard]] double closure_sum(const TearSheet& t) noexcept {
  return t.attr_delta + t.attr_gamma + t.attr_vega + t.attr_vanna + t.attr_volga + t.attr_theta +
         t.attr_rho + t.attr_charm + t.attr_unexplained + t.attr_settlement + t.attr_shares +
         t.attr_financing - t.attr_cost;
}

void expect_result_bit_identical(const BacktestResult& a, const BacktestResult& b) {
  ASSERT_EQ(a.size(), b.size());
  const std::vector<std::pair<const std::vector<double>*, const std::vector<double>*>> cols = {
      {&a.pnl_total, &b.pnl_total},   {&a.pnl_theta, &b.pnl_theta},
      {&a.pnl_gamma, &b.pnl_gamma},   {&a.pnl_vega, &b.pnl_vega},
      {&a.nav, &b.nav},               {&a.gross_vega, &b.gross_vega},
      {&a.gross_theta, &b.gross_theta}, {&a.n_open_lots, &b.n_open_lots},
      {&a.n_unpriced_lots, &b.n_unpriced_lots},
      {&a.n_unpriced_greeks, &b.n_unpriced_greeks}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    for (const auto& [va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
  }
}

}  // namespace

// Per-eval timing relocated to bench/strangle_solver_bench.cpp.

// ── 1. Restrike invariant: single cohort (2 lots) every row, strikes MOVE ────
TEST(SpyStrangleBacktest, RestrikeSingleCohortStrikesMove) {
  const fs::path dir = fs::temp_directory_path() / "atx-spy-strangle-restrike";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const Corpus c = make_corpus(dir, 12);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy strat{make_spec()};
  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
  ASSERT_EQ(r.size(), 12u);

  // Exactly two open lots (call + put) on every recorded row — a single cohort
  // restruck each step, NOT an accumulating ladder.
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_EQ(r.n_open_lots[i], 2.0) << "row " << i;
  }

  // The strikes are genuinely restruck: reprice the 40-delta call/put on the
  // first and last snapshots — spot drifted, so the strikes must differ.
  const auto strike_on = [&](const std::string& path, Side side) -> double {
    auto snap = MarketSnapshot::load(path);
    EXPECT_TRUE(snap.has_value()) << (snap.has_value() ? std::string{} : snap.error().to_string());
    const SurfaceRef surf = snap->find(kSpy);
    EXPECT_NE(surf, nullptr);
    const Result<double> K = resolve_strike_by_delta(*surf, kTenorT, side, 0.40);
    EXPECT_TRUE(K.has_value()) << (K.has_value() ? std::string{} : K.error().to_string());
    return K.value_or(0.0);
  };
  const double kc0 = strike_on(c.dp.front().second, Side::Call);
  const double kc1 = strike_on(c.dp.back().second, Side::Call);
  const double kp0 = strike_on(c.dp.front().second, Side::Put);
  const double kp1 = strike_on(c.dp.back().second, Side::Put);
  EXPECT_GT(std::fabs(kc1 - kc0), 1.0) << "call strike must move as spot drifts";
  EXPECT_GT(std::fabs(kp1 - kp0), 1.0) << "put strike must move as spot drifts";
  EXPECT_GT(kc0, kp0) << "strangle: OTM call strike above OTM put strike";

  std::printf("[spy-strangle] restrike: n_lots=2 all rows; call K %.2f->%.2f put K %.2f->%.2f\n",
              kc0, kc1, kp0, kp1);
}

// ── 2. 40-delta entry: resolved strikes reprice to |delta| ~ 0.40 ────────────
TEST(SpyStrangleBacktest, FortyDeltaEntry) {
  const fs::path dir = fs::temp_directory_path() / "atx-spy-strangle-40d";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const Corpus c = make_corpus(dir, 3);
  auto snap = MarketSnapshot::load(c.dp.front().second);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();
  const SurfaceRef surf = snap->find(kSpy);
  ASSERT_NE(surf, nullptr);

  for (const Side side : {Side::Call, Side::Put}) {
    const Result<double> K = resolve_strike_by_delta(*surf, kTenorT, side, 0.40);
    ASSERT_TRUE(K.has_value()) << K.error().to_string();
    const Result<AmericanGreeks> gr = surf->greeks(*K, kTenorT, side);
    ASSERT_TRUE(gr.has_value()) << gr.error().to_string();
    EXPECT_NEAR(std::fabs(gr->delta), 0.40, 1e-3) << (side == Side::Call ? "call" : "put");
    const double F = surf->forward_at(kTenorT);
    if (side == Side::Call) {
      EXPECT_GT(*K, F) << "40d call is OTM (above forward)";
    } else {
      EXPECT_LT(*K, F) << "40d put is OTM (below forward)";
    }
  }
}

// ── 5. Greek-target sizing: pin book theta constant while restriking daily ───
TEST(SpyStrangleBacktest, TargetThetaHoldsConstant) {
  const fs::path dir = fs::temp_directory_path() / "atx-spy-strangle-theta";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const Corpus c = make_corpus(dir, 15);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Short 40d 6m strangle, restriked daily, sized so |book theta| == $50/DAY. The
  // recorded gross_theta is annualized (the greek is dP/dt in years), so it pins at
  // 50 * 365.25 == $18,262.5/yr.
  constexpr double kTargetPerDay = 50.0;
  constexpr double kTargetAnnual = kTargetPerDay * 365.25;
  StrategySpec spec = make_spec();
  spec.legs[0].size = SizeSpec{SizeSpec::Kind::TargetTheta, kTargetPerDay, -1.0};
  DeclarativeStrategy strat{spec};
  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;

  // Book theta is pinned near +$18.26k/yr (== $50/day; short => positive theta). The
  // small wobble is the sizing greek (FD theta at entry) vs the recorded book greek
  // (analytic PDE theta) — the same axis, methods agreeing to ~sub-%, so a 3% band.
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_EQ(r.n_open_lots[i], 2.0) << "row " << i;
    EXPECT_NEAR(r.gross_theta[i], kTargetAnnual, 0.03 * kTargetAnnual) << "row " << i;
    EXPECT_LT(r.gross_vega[i], 0.0) << "row " << i << " (short vega)";
  }

  // The unit count genuinely FLOATS: with theta pinned, the book vega (which rides
  // the same units) must vary across the run — otherwise sizing was effectively
  // fixed. Compare an early vs a late row's gross vega.
  double vmin = r.gross_vega[1];
  double vmax = r.gross_vega[1];
  for (std::size_t i = 1; i < r.size(); ++i) {
    vmin = std::min(vmin, r.gross_vega[i]);
    vmax = std::max(vmax, r.gross_vega[i]);
  }
  EXPECT_GT(std::fabs(vmax - vmin), 1.0) << "units did not float (vega constant)";

  std::printf("[spy-strangle] target-theta: gross_theta[0]=%.1f gross_theta[last]=%.1f "
              "(target $%.0f/day = %.0f/yr); gross_vega in [%.0f, %.0f]\n",
              r.gross_theta.front(), r.gross_theta.back(), kTargetPerDay, kTargetAnnual, vmin,
              vmax);
}

// ── 3./4. Short sign + closure identity + thread determinism ─────────────────
TEST(SpyStrangleBacktest, ShortSignClosureAndDeterminism) {
  const fs::path dir = fs::temp_directory_path() / "atx-spy-strangle-close";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const Corpus c = make_corpus(dir, 15);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy s1{make_spec()};
  DeclarativeStrategy s4{make_spec()};
  RunConfig cfg1;
  cfg1.price.n_threads = 1;
  RunConfig cfg4;
  cfg4.price.n_threads = 4;
  auto r1 = run_backtest(*clock, s1, cfg1);
  auto r4 = run_backtest(*clock, s4, cfg4);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();

  const BacktestResult& r = *r1;
  // Short a strangle: the book is short vega and short gamma on every row.
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_LT(r.gross_vega[i], 0.0) << "row " << i << " (short vega)";
    EXPECT_LT(r.gross_gamma[i], 0.0) << "row " << i << " (short gamma)";
    EXPECT_GT(r.gross_theta[i], 0.0) << "row " << i << " (short options earn theta)";
  }

  // Closure identity (frictionless => cost.front()==0, plain identity).
  const TearSheet t = tearsheet(r);
  const double resid = std::fabs(t.total_return - (closure_sum(t) + r.cost.front()));
  EXPECT_LE(resid, 1e-6 * (std::fabs(t.total_return) + 1.0))
      << "total_return=" << t.total_return << " closure=" << (closure_sum(t) + r.cost.front());

  // Determinism across thread counts.
  expect_result_bit_identical(*r1, *r4);

  std::printf("[spy-strangle] short-strangle: total_return=%.2f theta=%.2f gamma=%.2f vega=%.2f "
              "closure_resid=%.3e det=OK\n",
              t.total_return, t.attr_theta, t.attr_gamma, t.attr_vega, resid);
}
