// atx-vol strategy DSL (Phase B1) gate tests.
//
// Exercises the declarative interpreter on synthetic eSSVI PricedSurfaces (the
// backtest_test make_surface pattern — analytic, no fitting, runs everywhere):
//
//   1. StrikeFromDelta  — resolve_strike_by_delta reprices to |delta|=target
//                         (call K>F, put K<F) across tenors; unreachable/out-of-
//                         range targets -> InvalidArgument.
//   2. Structures       — a 40d strangle expands to one OTM call + one OTM put.
//   3. FlatVega         — the design's example B (long XOM 9m strangle vs short
//                         SPY 3m strangle, FlatVega) prices to ~0 net book vega.
//   4. OverlappingClips — EveryStep+HoldToExpiry accumulates a cohort per step and
//                         drops cohorts as they cross expiry.
//   5. DispersionParity — a DispersionStrategy opens a vega-neutral book and its
//                         recorded implied_corr matches dispersion_signal.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"   // al_fast_opts, AmericanMethod, AmericanGreeks
#include "atx/vol/backtest.hpp"   // MarketSnapshot, Clock, run_backtest
#include "atx/vol/corpus.hpp"     // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/dispersion.hpp" // DispersionUniverse, dispersion_signal
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/portfolio_pricer.hpp" // Portfolio, SurfaceSet, PortfolioPricer, Position
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"         // the DSL + DeclarativeStrategy/DispersionStrategy
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, Result, Status, ErrorCode
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUid = 7;

[[nodiscard]] bool close(double a, double b, double rel = 1e-9) noexcept {
  return std::fabs(a - b) <= rel * (std::fabs(a) + std::fabs(b) + 1e-300);
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
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-strategy-") + tag);
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
  const Status st = write_surface_archive_file(path, its);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows (one entry/date).
[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths) {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "MKT";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

} // namespace

// ── 1. Strike-from-delta reprices to the target ─────────────────────────────
TEST(Strategy, StrikeFromDelta) {
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow);

  double worst_err = 0.0;
  for (const double T : {0.10, 0.50}) {
    const double F = s.forward_at(T);
    ASSERT_GT(F, 0.0);

    // 40-delta call: OTM => K > F.
    auto kc = resolve_strike_by_delta(s, T, Side::Call, 0.40);
    ASSERT_TRUE(kc.has_value()) << kc.error().to_string();
    EXPECT_GT(*kc, F) << "40d call should sit above the forward";
    const auto gc = s.greeks(*kc, T, Side::Call);
    ASSERT_TRUE(gc.has_value());
    worst_err = std::max(worst_err, std::fabs(std::fabs(gc->delta) - 0.40));
    EXPECT_NEAR(std::fabs(gc->delta), 0.40, 1e-4) << "T=" << T;

    // 25-delta put: OTM => K < F.
    auto kp = resolve_strike_by_delta(s, T, Side::Put, 0.25);
    ASSERT_TRUE(kp.has_value()) << kp.error().to_string();
    EXPECT_LT(*kp, F) << "25d put should sit below the forward";
    const auto gp = s.greeks(*kp, T, Side::Put);
    ASSERT_TRUE(gp.has_value());
    worst_err = std::max(worst_err, std::fabs(std::fabs(gp->delta) - 0.25));
    EXPECT_NEAR(std::fabs(gp->delta), 0.25, 1e-4) << "T=" << T;
  }

  // Deep interior targets are reachable: the solver's [-5,5] log-moneyness bracket
  // spans essentially all of (0,1) (a deep-ITM American |delta| grazes 1.0), so even
  // a 0.90 call / 0.85 put resolve and reprice to the target. There is no interior
  // |delta| a well-behaved surface cannot bracket; the unreachable contract is the
  // (0,1) guard band below.
  for (const auto &[side, tgt] :
       std::vector<std::pair<Side, double>>{{Side::Call, 0.90}, {Side::Put, 0.85}}) {
    auto kk = resolve_strike_by_delta(s, 0.25, side, tgt);
    ASSERT_TRUE(kk.has_value()) << kk.error().to_string();
    const auto g = s.greeks(*kk, 0.25, side);
    ASSERT_TRUE(g.has_value());
    EXPECT_NEAR(std::fabs(g->delta), tgt, 1e-4);
  }
  // Out of the (0,1) guard band.
  for (const double bad : {0.0, 1.5, -0.2}) {
    auto r = resolve_strike_by_delta(s, 0.25, Side::Put, bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }

  std::printf("[strategy] strike-from-delta worst |delta| error = %.2e\n", worst_err);
}

// ── 2. Structures: a 40d strangle => OTM call + OTM put ─────────────────────
TEST(Strategy, Structures) {
  const fs::path dir = fresh_dir("structures");
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string p = write_archive(dir, "2026-08-01", {{"SPX", &s}});
  auto snap = MarketSnapshot::load(p);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};

  auto legs = expand_leg(*snap, leg);
  ASSERT_TRUE(legs.has_value()) << legs.error().to_string();
  ASSERT_EQ(legs->size(), 2u);

  const ResolvedLeg &call = (*legs)[0];
  const ResolvedLeg &put = (*legs)[1];
  const double F = snap->find(kUid)->forward_at(0.25);

  EXPECT_EQ(call.side, Side::Call);
  EXPECT_EQ(put.side, Side::Put);
  EXPECT_GT(call.K, F);
  EXPECT_LT(put.K, F);
  EXPECT_GT(call.vega, 0.0);
  EXPECT_GT(put.vega, 0.0);

  const auto gc = snap->find(kUid)->greeks(call.K, 0.25, Side::Call);
  const auto gp = snap->find(kUid)->greeks(put.K, 0.25, Side::Put);
  ASSERT_TRUE(gc.has_value() && gp.has_value());
  EXPECT_NEAR(std::fabs(gc->delta), 0.40, 1e-4);
  EXPECT_NEAR(std::fabs(gp->delta), 0.40, 1e-4);
}

// ── 3. Flat-vega cross-strangle nets to ~0 book vega ────────────────────────
TEST(Strategy, FlatVega) {
  const fs::path dir = fresh_dir("flatvega");
  constexpr std::uint32_t kXom = 10;
  constexpr std::uint32_t kSpy = 20;
  const PricedSurface xom = make_surface(kXom, 110.0, 110.0, kBaseNow, 0.03);
  const PricedSurface spy = make_surface(kSpy, 450.0, 450.0, kBaseNow, 0.00);
  const std::string p = write_archive(dir, "2026-08-01", {{"XOM", &xom}, {"SPY", &spy}});
  auto snap = MarketSnapshot::load(p);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  const auto strangle = [](std::uint32_t uid, double T, double sign, const char *group) {
    LegSpec leg;
    leg.uid = uid;
    leg.tenor.target_T = T;
    leg.structure.kind = StructureSpec::Kind::Strangle;
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::Weight, 1.0, sign};
    leg.group = group;
    return leg;
  };

  StrategySpec spec;
  spec.name = "xom9m-vs-spy3m-40d-strangle-flat-vega";
  spec.legs.push_back(strangle(kXom, 0.75, +1.0, "a")); // long XOM 9m
  spec.legs.push_back(strangle(kSpy, 0.25, -1.0, "b")); // short SPY 3m
  spec.constraint = CrossLegConstraint{CrossLegConstraint::Kind::FlatVega, "a", "b"};

  auto sized = resolve_spec(*snap, spec);
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 4u);

  std::vector<Position> ps;
  for (std::size_t i = 0; i < sized->size(); ++i) {
    const SizedLeg &sl = (*sized)[i];
    ps.push_back(Position{i + 1, OptionContract{sl.leg.uid, sl.leg.K, sl.leg.T, sl.leg.side},
                          sl.qty, sl.multiplier});
  }
  auto pf = Portfolio::create(ps);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer{std::move(*pf)};
  auto frame = pricer.price(snap->set());
  ASSERT_TRUE(frame.has_value()) << frame.error().to_string();

  double net = 0.0;
  double gross = 0.0;
  for (std::size_t i = 0; i < frame->size(); ++i) {
    net += frame->vega[i];
    gross += std::fabs(frame->vega[i]);
  }
  EXPECT_GT(gross, 0.0);
  EXPECT_LE(std::fabs(net), 1e-6 * gross) << "net=" << net << " gross=" << gross;

  std::printf("[strategy] flat-vega net book vega = %.6e (gross %.2f, residual %.2e of gross)\n",
              net, gross, std::fabs(net) / gross);
}

// ── 4. Overlapping clips: a cohort per step, dropped as they expire ─────────
TEST(Strategy, OverlappingClips) {
  const fs::path dir = fresh_dir("clips");
  const std::vector<int> day_off = {0, 5, 10, 15, 20, 25, 125}; // final = big time-jump
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < day_off.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(day_off[d]) * kDayNs;
    const double S = 100.0 * (1.0 + 0.002 * static_cast<double>(day_off[d]));
    const PricedSurface s = make_surface(kUid, S, S, now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-09-%02d", static_cast<int>(d + 1));
    const std::string date = buf;
    dp.emplace_back(date, write_archive(dir, date, {{"SPX", &s}}));
  }
  auto clock = Clock::from_manifest(make_manifest(dp));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.name = "spy-25d-put-daily-clip";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.06; // ~22 days: cohorts expire within the corpus
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  // ATM-forward strike keeps pricing in the surface core (variance > 0) as each
  // cohort ages down to a tiny residual T just before expiry.
  leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;

  DeclarativeStrategy strat{spec};
  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult &r = *res;
  ASSERT_EQ(r.size(), day_off.size());

  // Monotonic +1 growth while no cohort has yet crossed expiry (rows 0..4).
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(r.n_open_lots[i], static_cast<double>(i + 1)) << "row " << i;
  }
  // Cohorts genuinely expire (settlement fires) and the count drops.
  double tot_settle = 0.0;
  for (const double x : r.pnl_settlement) {
    tot_settle += std::fabs(x);
  }
  EXPECT_GT(tot_settle, 0.0);
  EXPECT_LT(r.n_open_lots.back(), r.n_open_lots[4]); // final jump expires the backlog
  EXPECT_EQ(r.n_open_lots.back(), 1.0);              // only the fresh clip remains

  std::printf("[strategy] overlapping-clip n_open_lots ="
              " {%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f}\n",
              r.n_open_lots[0], r.n_open_lots[1], r.n_open_lots[2], r.n_open_lots[3],
              r.n_open_lots[4], r.n_open_lots[5], r.n_open_lots[6]);
}

// ── 5. Dispersion parity: vega-neutral book + recorded implied_corr ─────────
TEST(Strategy, DispersionParity) {
  const fs::path dir = fresh_dir("dispersion");
  const std::vector<int> day_off = {0, 5};
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
  DispersionConfig cfg; // 30d, 10000 target vega, short-index, mult 100
  cfg.record_diagnostics = true;

  DispersionStrategy strat{u, cfg};
  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  // The recorded implied_corr series exists and matches the closed-form signal.
  const std::vector<double> *corr = nullptr;
  for (const auto &sig_series : res->signals) {
    if (sig_series.first == "implied_corr") {
      corr = &sig_series.second;
    }
  }
  ASSERT_NE(corr, nullptr) << "implied_corr signal not recorded";
  ASSERT_EQ(corr->size(), res->size());

  auto base = MarketSnapshot::load(clock->refs()[0].archive_path);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  DispersionBacktestConfig fast_config;
  fast_config.min_names = 2u;
  auto fast_run = run_dispersion_backtest(*clock, u, fast_config);
  ASSERT_TRUE(fast_run.has_value()) << fast_run.error().to_string();
  EXPECT_TRUE(fast_run->signals.empty());
  auto sig = dispersion_signal(u, base->set(), cfg.target_T);
  ASSERT_TRUE(sig.has_value()) << sig.error().to_string();
  EXPECT_TRUE(close((*corr)[0], sig->implied_corr, 1e-9))
      << (*corr)[0] << " vs " << sig->implied_corr;

  // The opened book is vega-neutral (index gross vega == -basket gross vega).
  auto book = strat.build_book(*base);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  auto pf = Portfolio::create(book->positions);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer{std::move(*pf)};
  auto frame = pricer.price(base->set());
  ASSERT_TRUE(frame.has_value()) << frame.error().to_string();
  double v_index = 0.0;
  double v_names = 0.0;
  for (std::size_t i = 0; i < frame->size(); ++i) {
    if (frame->uid[i] == 1) {
      v_index += frame->vega[i];
    } else {
      v_names += frame->vega[i];
    }
  }
  EXPECT_TRUE(close(v_index, -v_names)) << v_index << " vs " << -v_names;
  EXPECT_LT(v_index, 0.0); // short index

  std::printf("[strategy] dispersion implied_corr=%.6f book_vega_idx=%.2f book_vega_names=%.2f\n",
              sig->implied_corr, v_index, v_names);
}
