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
//   6. DispersionLifecycle — run_dispersion_backtest honours the lifecycle fields
//                         on DispersionBacktestConfig: the ladder shape
//                         (EveryStep+HoldToExpiry) accumulates a cohort per step
//                         while the default rolling shape stays at one clip.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp" // al_fast_opts, AmericanMethod, AmericanGreeks
#include "atx/vol/api/backtest/backtest.hpp" // MarketSnapshot, Clock, run_backtest
#include "atx/vol/api/marketdata/corpus.hpp"   // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "fitting/counters.hpp"
#include "atx/vol/api/backtest/dispersion.hpp" // DispersionUniverse, dispersion_signal
#include "atx/vol/research/dispersion_backtest.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // Portfolio, SurfaceSet, PortfolioPricer, Position
#include "atx/vol/api/backtest/priced_surface.hpp"   // PricedSurface, PricingContext
#include "pricing/deriv_ref_bridge.hpp" // detail::deriv_greeks_on_ref (swap-lane oracle)
#include "atx/vol/api/backtest/strategy.hpp"  // the DSL + DeclarativeStrategy/DispersionStrategy
#include "atx/vol/api/pricing/swap_leg.hpp"  // swap_contract_for_lot
#include "atx/vol/api/storage/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/api/fitting/surface_parity.hpp"   // SliceContext
#include "atx/vol/api/core/types.hpp"            // Side, Result, Status, ErrorCode
#include "atx/vol/api/fitting/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"      // EssviParams

#include "support/isa_golden_tol.hpp" // laned_greeks_close (WS-P1a route band)

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
  const Status st = write_surface_archive_v2_file(path, its);
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

// A ready-to-clock manifest over `n_dates` consecutive DAILY snapshots, one
// symbol "SPY" (uid `kUid`) per date, for the CloseAtHorizon lifecycle tests
// (which need a real multi-date corpus, unlike the single-snapshot DSL tests
// above). `tag` picks the fresh_dir so distinct call sites never collide.
struct Corpus {
  CorpusManifest manifest;
};

[[nodiscard]] Corpus make_corpus(int n_dates, const char *tag) {
  const fs::path dir = fresh_dir(tag);
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const PricedSurface s = make_surface(kUid, 100.0, 100.0, now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-11-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_archive(dir, date, {{"SPY", &s}}));
  }
  return Corpus{make_manifest(dp)};
}

// A single-date MarketSnapshot over `items` (symbol -> surface) via
// write_archive + MarketSnapshot::load, for tests that only need one snapshot
// resolved by symbol (not a full clocked corpus).
[[nodiscard]] Result<MarketSnapshot>
snapshot_of(const std::vector<std::pair<std::string, const PricedSurface *>> &items,
            const char *tag) {
  const fs::path dir = fresh_dir(tag);
  const std::string path = write_archive(dir, "2026-12-01", items);
  return MarketSnapshot::load(path);
}

void expect_lot_equal(const Lot &actual, const Lot &expected) {
  EXPECT_EQ(actual.id, expected.id);
  EXPECT_EQ(actual.contract.uid, expected.contract.uid);
  EXPECT_EQ(actual.contract.K, expected.contract.K);
  EXPECT_EQ(actual.contract.T, expected.contract.T);
  EXPECT_EQ(actual.contract.side, expected.contract.side);
  EXPECT_EQ(actual.qty, expected.qty);
  EXPECT_EQ(actual.multiplier, expected.multiplier);
  EXPECT_EQ(actual.expiry_ts_ns, expected.expiry_ts_ns);
  EXPECT_EQ(actual.cohort, expected.cohort);
  EXPECT_EQ(actual.entry_price, expected.entry_price);
}

[[nodiscard]] LegSpec fixed_call(std::uint32_t uid, double T = 0.25) {
  LegSpec leg;
  leg.uid = uid;
  leg.tenor.target_T = T;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Call;
  leg.strike = {StrikeSelector::Kind::AbsStrike, 100.0};
  leg.size = {SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  return leg;
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

TEST(Strategy, StrikeFromDeltaDoesNotRepriceTerminalCandidate) {
  namespace ledger = counters::ledger;
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow);

  ledger::reset();
  const Result<double> strike = resolve_strike_by_delta(s, 0.50, Side::Put, 0.25);
  ASSERT_TRUE(strike.has_value()) << strike.error().to_string();
  const ledger::Counts measured = ledger::snapshot();

  EXPECT_EQ(measured.get(ledger::Solve::AlBoundarySolves), 10u)
      << "the terminal root evaluation must serve validation without an eleventh solve";
}

TEST(Strategy, AdaptiveStrikeResolutionAlwaysColdConfirmsAcrossGrid) {
  PricedSurface source = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  const PricedSurface fast = std::move(*prepared);
  const PricedSurface cold = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const ResolutionOptions options{/*fast_screen_cold_confirm=*/true};

  for (const double T : {0.10, 0.25, 0.50, 0.75}) {
    for (const auto &[side, target] : std::vector<std::pair<Side, double>>{
             {Side::Call, 0.25}, {Side::Call, 0.40}, {Side::Put, 0.25}, {Side::Put, 0.40}}) {
      const auto adaptive = resolve_strike_by_delta(fast, T, side, target, options);
      const auto reference = resolve_strike_by_delta(cold, T, side, target);
      ASSERT_TRUE(adaptive.has_value()) << adaptive.error().to_string();
      ASSERT_TRUE(reference.has_value()) << reference.error().to_string();

      const auto achieved = fast.delta(*adaptive, T, side, QueryExecution::ColdReference);
      ASSERT_TRUE(achieved.has_value()) << achieved.error().to_string();
      EXPECT_LE(std::fabs(std::fabs(*achieved) - target), options.cold_delta_tolerance)
          << "T=" << T << " side=" << static_cast<int>(side) << " target=" << target;
      EXPECT_NEAR(*adaptive, *reference, 2.0e-4 * cold.forward_at(T))
          << "T=" << T << " side=" << static_cast<int>(side) << " target=" << target;
    }
  }
}

TEST(Strategy, AdaptiveResolutionColdSizesEverySelectorAndStoresColdEntryMark) {
  const fs::path dir = fresh_dir("adaptive-resolution");
  const PricedSurface archived = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_archive(dir, "2026-08-01", {{"SPY", &archived}});
  auto fast_snapshot = MarketSnapshot::load(path, QueryPricingTier::RepresentativeFast);
  auto cold_snapshot = MarketSnapshot::load(path, QueryPricingTier::ColdReference);
  ASSERT_TRUE(fast_snapshot.has_value()) << fast_snapshot.error().to_string();
  ASSERT_TRUE(cold_snapshot.has_value()) << cold_snapshot.error().to_string();

  StrategySpec adaptive_spec;
  adaptive_spec.resolution.fast_screen_cold_confirm = true;
  LegSpec strangle;
  strangle.uid = kUid;
  strangle.tenor.target_T = 0.50;
  strangle.structure.kind = StructureSpec::Kind::Strangle;
  strangle.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  strangle.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  strangle.size = {SizeSpec::Kind::TargetTheta, 10.0, -1.0};
  adaptive_spec.legs.push_back(strangle);
  LegSpec absolute;
  absolute.uid = kUid;
  absolute.tenor.target_T = 0.25;
  absolute.structure.kind = StructureSpec::Kind::Single;
  absolute.structure.single_side = Side::Put;
  absolute.strike = {StrikeSelector::Kind::AbsStrike, 95.0};
  absolute.size = {SizeSpec::Kind::FixedContracts, 2.0, +1.0};
  adaptive_spec.legs.push_back(absolute);

  StrategySpec cold_spec = adaptive_spec;
  cold_spec.resolution = {};
  const auto adaptive = resolve_spec(*fast_snapshot, adaptive_spec);
  const auto reference = resolve_spec(*cold_snapshot, cold_spec);
  ASSERT_TRUE(adaptive.has_value()) << adaptive.error().to_string();
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();
  ASSERT_EQ(adaptive->size(), reference->size());
  for (std::size_t i = 0; i < adaptive->size(); ++i) {
    const SizedLeg &actual = (*adaptive)[i];
    const SizedLeg &expected = (*reference)[i];
    EXPECT_EQ(actual.leg.uid, expected.leg.uid) << i;
    EXPECT_EQ(actual.leg.side, expected.leg.side) << i;
    EXPECT_NEAR(actual.leg.K, expected.leg.K, 2.0e-4 * 100.0) << i;
    EXPECT_NEAR(actual.leg.vega, expected.leg.vega, 1.0e-3 * (1.0 + std::fabs(expected.leg.vega)))
        << i;
    EXPECT_NEAR(actual.leg.theta, expected.leg.theta,
                1.0e-3 * (1.0 + std::fabs(expected.leg.theta)))
        << i;
    EXPECT_NEAR(actual.leg.gamma, expected.leg.gamma,
                1.0e-3 * (1.0 + std::fabs(expected.leg.gamma)))
        << i;
    EXPECT_NEAR(actual.qty, expected.qty, 1.0e-3 * (1.0 + std::fabs(expected.qty))) << i;

    const SurfaceRef surface = fast_snapshot->find(actual.leg.uid);
    ASSERT_NE(surface, nullptr);
    const auto cold_greeks =
        surface->greeks(actual.leg.K, actual.leg.T, actual.leg.side, QueryExecution::ColdReference);
    ASSERT_TRUE(cold_greeks.has_value()) << cold_greeks.error().to_string();
    EXPECT_EQ(actual.leg.vega, cold_greeks->vega) << i;
    EXPECT_EQ(actual.leg.theta, cold_greeks->theta) << i;
    EXPECT_EQ(actual.leg.gamma, cold_greeks->gamma) << i;
  }

  DeclarativeStrategy strategy{adaptive_spec};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  const Status opened = strategy.on_step(*fast_snapshot, 0, book, next_lot_id);
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  ASSERT_EQ(book.lots.size(), adaptive->size());
  for (const Lot &lot : book.lots) {
    const SurfaceRef surface = fast_snapshot->find(lot.contract.uid);
    ASSERT_NE(surface, nullptr);
    const auto cold_mark = surface->fair_value(lot.contract.K, lot.contract.T, lot.contract.side,
                                               QueryExecution::ColdReference);
    ASSERT_TRUE(cold_mark.has_value()) << cold_mark.error().to_string();
    EXPECT_EQ(lot.entry_price, *cold_mark);
  }
}

TEST(Strategy, PriceOptionsForceColdForNonadaptiveStrikeSizingAndEntryMark) {
  const fs::path dir = fresh_dir("price-options-forced-cold");
  const PricedSurface archived = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_archive(dir, "2026-08-01", {{"SPY", &archived}});
  auto fast_snapshot = MarketSnapshot::load(path, QueryPricingTier::RepresentativeFast);
  auto cold_snapshot = MarketSnapshot::load(path, QueryPricingTier::ColdReference);
  ASSERT_TRUE(fast_snapshot.has_value()) << fast_snapshot.error().to_string();
  ASSERT_TRUE(cold_snapshot.has_value()) << cold_snapshot.error().to_string();

  StrategySpec spec;
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.50;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::TargetTheta, 10.0, -1.0};
  spec.legs.push_back(leg);

  PriceOptions options;
  options.query_execution = QueryExecution::ColdReference;
  const auto actual = resolve_spec(*fast_snapshot, spec, options);
  const auto reference = resolve_spec(*cold_snapshot, spec);
  ASSERT_TRUE(actual.has_value()) << actual.error().to_string();
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();
  ASSERT_EQ(actual->size(), reference->size());
  ASSERT_EQ(actual->size(), 1u);
  const SizedLeg &a = actual->front();
  const SizedLeg &r = reference->front();
  EXPECT_EQ(a.leg.K, r.leg.K);
  EXPECT_EQ(a.leg.model_price, r.leg.model_price);
  EXPECT_EQ(a.leg.vega, r.leg.vega);
  EXPECT_EQ(a.leg.theta, r.leg.theta);
  EXPECT_EQ(a.leg.gamma, r.leg.gamma);
  EXPECT_EQ(a.qty, r.qty);
}

TEST(Strategy, PriceOptionsAnalyticGreeksDriveTargetThetaSizing) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "price-options-analytic-theta");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  StrategySpec spec;
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.50;
  leg.structure.kind = StructureSpec::Kind::Straddle;
  leg.strike = {StrikeSelector::Kind::AbsStrike, 100.0};
  leg.size = {SizeSpec::Kind::TargetTheta, 12.5, -1.0};
  spec.legs.push_back(leg);

  PriceOptions options;
  options.analytic_greeks = true;
  options.query_execution = QueryExecution::ColdReference;
  const auto sized = resolve_spec(*snapshot, spec, options);
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 2u);

  const SurfaceRef resolved_surface = snapshot->find(kUid);
  ASSERT_NE(resolved_surface, nullptr);
  double book_theta = 0.0;
  double structure_theta = 0.0;
  for (const SizedLeg &sl : *sized) {
    ASSERT_TRUE(sl.leg.full_greek_seed.has_value());
    EXPECT_EQ(sl.leg.expiry_ts_ns,
              kBaseNow + static_cast<std::int64_t>(std::round(0.50 * kNsPerYear)));
    const FullGreekSeed &seed = *sl.leg.full_greek_seed;
    EXPECT_EQ(seed.surface_instance_id(), resolved_surface->instance_id());
    EXPECT_TRUE(seed.analytic_greeks());
    EXPECT_EQ(seed.query_execution(), QueryExecution::ColdReference);
    EXPECT_EQ(seed.K(), sl.leg.K);
    EXPECT_EQ(seed.T(), sl.leg.T);
    EXPECT_EQ(seed.side(), sl.leg.side);
    const auto expected = resolved_surface->greeks_analytic(sl.leg.K, sl.leg.T, sl.leg.side,
                                                            QueryExecution::ColdReference);
    ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
    // Route parity, not a value pin: sl.leg.* comes from resolve_spec's BATCHED
    // path (laned AVX2 greeks under Auto ISA since WS-P1a) while `expected` is a
    // single-contract re-query that can land on the scalar oracle. Relative
    // agreement is the invariant; see support/isa_golden_tol.hpp. These are the
    // call sites carrying the band's measured worst cases (gamma 1.34e-10,
    // theta 1.23e-10 relative).
    using atx::vol::test::laned_greeks_close;
    EXPECT_TRUE(laned_greeks_close(sl.leg.model_price, expected->price));
    EXPECT_TRUE(laned_greeks_close(sl.leg.vega, expected->vega));
    EXPECT_TRUE(laned_greeks_close(sl.leg.theta, expected->theta));
    EXPECT_TRUE(laned_greeks_close(sl.leg.gamma, expected->gamma));
    structure_theta += seed.greeks().theta;
    book_theta += sl.qty * sl.multiplier * sl.leg.theta;
  }
  const double expected_qty = -12.5 * 365.25 / (std::fabs(structure_theta) * 100.0);
  EXPECT_EQ(sized->front().qty, expected_qty);
  EXPECT_EQ(sized->back().qty, expected_qty);
  EXPECT_NEAR(std::fabs(book_theta), 12.5 * 365.25, 1.0e-11 * std::fabs(book_theta));
}

TEST(Strategy, NonGridTenorCanonicalizesToExactExpiryBeforeSeededSizing) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "canonical-non-grid-tenor");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  constexpr double kRequestedT = 0.123456789012345;
  StrategySpec spec;
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = kRequestedT;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Call;
  leg.strike = {StrikeSelector::Kind::AbsStrike, 101.0};
  leg.size = {SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);

  PriceOptions options;
  options.analytic_greeks = true;
  options.query_execution = QueryExecution::ColdReference;
  const auto sized = resolve_spec(*snapshot, spec, options);
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 1u);
  const ResolvedLeg &resolved = sized->front().leg;
  const auto tenor_ns = static_cast<std::int64_t>(std::round(kRequestedT * kNsPerYear));
  const std::int64_t expected_expiry = kBaseNow + tenor_ns;
  const double expected_T = static_cast<double>(tenor_ns) / kNsPerYear;
  EXPECT_EQ(resolved.expiry_ts_ns, expected_expiry);
  EXPECT_EQ(resolved.T, expected_T);
  ASSERT_TRUE(resolved.full_greek_seed.has_value());
  const FullGreekSeed &seed = *resolved.full_greek_seed;
  EXPECT_EQ(seed.T(), expected_T);
  EXPECT_EQ(seed.K(), resolved.K);
  EXPECT_EQ(seed.side(), resolved.side);
  EXPECT_EQ(seed.surface_instance_id(), snapshot->find(kUid)->instance_id());
  EXPECT_TRUE(seed.analytic_greeks());
  EXPECT_EQ(seed.query_execution(), QueryExecution::ColdReference);

  DeclarativeStrategy strategy{spec};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  const Status opened = strategy.on_step(*snapshot, 0, book, next_lot_id, options);
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  ASSERT_EQ(book.lots.size(), 1u);
  EXPECT_EQ(book.lots.front().expiry_ts_ns, expected_expiry);
  EXPECT_EQ(book.lots.front().contract.T, expected_T);
  ASSERT_EQ(strategy.entry_risk_seeds().size(), 1u);
  EXPECT_EQ(strategy.entry_risk_seeds().front().T(), expected_T);
}

TEST(Strategy, EntryRiskSeedsClearOnEveryStepWithoutANewEntry) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "entry-seed-stale-clear");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  StrategySpec spec;
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.entry_every_n = 2;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = {StrikeSelector::Kind::AbsStrike, 95.0};
  leg.size = {SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);

  DeclarativeStrategy strategy{spec};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  PriceOptions options;
  options.analytic_greeks = true;
  ASSERT_TRUE(strategy.on_step(*snapshot, 0, book, next_lot_id, options).has_value());
  ASSERT_EQ(strategy.entry_risk_seeds().size(), 1u);
  ASSERT_TRUE(strategy.on_step(*snapshot, 1, book, next_lot_id, options).has_value());
  EXPECT_TRUE(strategy.entry_risk_seeds().empty());
  EXPECT_EQ(book.lots.size(), 1u);
}

TEST(Strategy, TenorCanonicalizationRejectsExpiryOverflow) {
  constexpr std::int64_t kNearMaxTs = std::numeric_limits<std::int64_t>::max() - 10;
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kNearMaxTs);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "canonical-tenor-overflow");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 1.0 / 365.25;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.strike = {StrikeSelector::Kind::AbsStrike, 100.0};
  const auto expanded = expand_leg(*snapshot, leg, ResolutionOptions{}, PriceOptions{});
  ASSERT_FALSE(expanded.has_value());
  EXPECT_EQ(expanded.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(expanded.error().message().find("expiry"), std::string::npos);
}

TEST(Strategy, NegativeValuationTimestampCanonicalizesToExactFutureExpiry) {
  constexpr std::int64_t kNegativeNow = -100 * kDayNs;
  constexpr double kRequestedT = 0.25;
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kNegativeNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "canonical-tenor-negative-now");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  const auto expanded =
      expand_leg(*snapshot, fixed_call(kUid, kRequestedT), ResolutionOptions{}, PriceOptions{});
  ASSERT_TRUE(expanded.has_value()) << expanded.error().to_string();
  ASSERT_EQ(expanded->size(), 1u);
  const std::int64_t tenor_ns = static_cast<std::int64_t>(std::round(kRequestedT * kNsPerYear));
  EXPECT_EQ(expanded->front().expiry_ts_ns, kNegativeNow + tenor_ns);
  EXPECT_EQ(expanded->front().T, static_cast<double>(tenor_ns) / kNsPerYear);
}

TEST(Strategy, CloseAtHorizonHandlesExtremeSignedTimestampDistances) {
  const PricedSurface min_surface =
      make_surface(kUid, 100.0, 100.0, std::numeric_limits<std::int64_t>::min());
  const PricedSurface max_surface =
      make_surface(kUid, 100.0, 100.0, std::numeric_limits<std::int64_t>::max());
  auto min_snapshot = snapshot_of({{"SPY", &min_surface}}, "close-horizon-int64-min");
  auto max_snapshot = snapshot_of({{"SPY", &max_surface}}, "close-horizon-int64-max");
  ASSERT_TRUE(min_snapshot.has_value()) << min_snapshot.error().to_string();
  ASSERT_TRUE(max_snapshot.has_value()) << max_snapshot.error().to_string();

  StrategySpec spec;
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.entry_every_n = 2;
  spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = 0.0;

  Lot future;
  future.id = 1;
  future.contract = OptionContract{kUid, 100.0, 0.25, Side::Call};
  future.qty = 1.0;
  future.expiry_ts_ns = std::numeric_limits<std::int64_t>::max();
  PortfolioState future_book;
  future_book.lots.push_back(future);
  DeclarativeStrategy future_strategy{spec};
  std::uint64_t future_next_id = 2;
  ASSERT_TRUE(future_strategy.on_step(*min_snapshot, 1, future_book, future_next_id).has_value());
  ASSERT_EQ(future_book.lots.size(), 1u);
  expect_lot_equal(future_book.lots.front(), future);

  Lot expired = future;
  expired.expiry_ts_ns = std::numeric_limits<std::int64_t>::min();
  PortfolioState expired_book;
  expired_book.lots.push_back(expired);
  DeclarativeStrategy expired_strategy{spec};
  std::uint64_t expired_next_id = 2;
  ASSERT_TRUE(
      expired_strategy.on_step(*max_snapshot, 1, expired_book, expired_next_id).has_value());
  EXPECT_TRUE(expired_book.lots.empty());
}

TEST(Strategy, RollAtHorizonFailureLeavesBookIdsAndLifecycleStateUnchanged) {
  const PricedSurface valid_surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const PricedSurface unrelated_surface = make_surface(kUid + 1, 100.0, 100.0, kBaseNow);
  auto valid = snapshot_of({{"SPY", &valid_surface}}, "roll-transaction-valid");
  auto missing = snapshot_of({{"OTHER", &unrelated_surface}}, "roll-transaction-missing");
  ASSERT_TRUE(valid.has_value()) << valid.error().to_string();
  ASSERT_TRUE(missing.has_value()) << missing.error().to_string();

  StrategySpec spec;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 0.30;
  spec.legs.push_back(fixed_call(kUid));
  DeclarativeStrategy strategy{spec};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strategy.on_step(*valid, 0, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  ASSERT_EQ(strategy.entry_risk_seeds().size(), 1u);
  const Lot original = book.lots.front();
  const std::uint64_t original_next_id = next_lot_id;

  const Status failed = strategy.on_step(*missing, 1, book, next_lot_id);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), ErrorCode::NotFound);
  ASSERT_EQ(book.lots.size(), 1u);
  expect_lot_equal(book.lots.front(), original);
  EXPECT_EQ(next_lot_id, original_next_id);
  EXPECT_TRUE(strategy.entry_risk_seeds().empty());

  ASSERT_TRUE(strategy.on_step(*valid, 1, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  EXPECT_EQ(book.lots.front().id, original_next_id);
  EXPECT_EQ(book.lots.front().cohort, original.cohort + 1u);
}

// Task A2: a no-trade step is NOT a no-close step for RollAtHorizon either. The
// single live cohort is AT its horizon on step 1 (residual T == the T=0.25
// entry leg's tenor, unchanged since `valid`/`missing` share `kBaseNow`, which
// is < roll_at_T=0.30), so `lifecycle_decide` sets `d.clear`. Before the A2
// fix, `d.clear` was only ever consumed on the success path, so the aged
// cohort here rode on unchanged; the close is now unconditional at the
// horizon, exactly like CloseAtHorizon's own no-trade close
// (Strategy.CloseAtHorizonNoTradeStillClosesLotsAtTheHorizon).
TEST(Strategy, RollAtHorizonNoTradeStillClosesTheAgedCohortAtItsHorizon) {
  const PricedSurface valid_surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const PricedSurface unrelated_surface = make_surface(kUid + 1, 100.0, 100.0, kBaseNow);
  auto valid = snapshot_of({{"SPY", &valid_surface}}, "roll-no-trade-valid");
  auto missing = snapshot_of({{"OTHER", &unrelated_surface}}, "roll-no-trade-missing");
  ASSERT_TRUE(valid.has_value()) << valid.error().to_string();
  ASSERT_TRUE(missing.has_value()) << missing.error().to_string();

  StrategySpec spec;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 0.30;
  spec.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 1};
  spec.legs.push_back(fixed_call(kUid));
  DeclarativeStrategy strategy{spec};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strategy.on_step(*valid, 0, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  const Lot original = book.lots.front();
  const std::uint64_t original_next_id = next_lot_id;
  EXPECT_EQ(strategy.n_steps_entry_skipped(), 0u);

  const Status no_trade = strategy.on_step(*missing, 1, book, next_lot_id);
  ASSERT_TRUE(no_trade.has_value()) << no_trade.error().to_string();
  EXPECT_TRUE(book.lots.empty());               // A2: the horizon close must still commit
  EXPECT_EQ(next_lot_id, original_next_id);      // no entry => no lot ids consumed
  EXPECT_TRUE(strategy.entry_risk_seeds().empty());
  EXPECT_EQ(strategy.n_steps_entry_skipped(), 1u);

  ASSERT_TRUE(strategy.on_step(*valid, 1, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  EXPECT_EQ(book.lots.front().id, original_next_id);
  EXPECT_EQ(book.lots.front().cohort, original.cohort + 1u);
  EXPECT_EQ(strategy.n_steps_entry_skipped(), 1u); // the successful reopen does not move it
}

TEST(Strategy, CloseAtHorizonFailedReopenDoesNotCommitStagedCloses) {
  const PricedSurface valid_surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const PricedSurface unrelated_surface = make_surface(kUid + 1, 100.0, 100.0, kBaseNow);
  auto valid = snapshot_of({{"SPY", &valid_surface}}, "close-transaction-valid");
  auto missing = snapshot_of({{"OTHER", &unrelated_surface}}, "close-transaction-missing");
  ASSERT_TRUE(valid.has_value()) << valid.error().to_string();
  ASSERT_TRUE(missing.has_value()) << missing.error().to_string();

  StrategySpec spec;
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = 0.30;
  spec.legs.push_back(fixed_call(kUid));
  DeclarativeStrategy strategy{spec};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strategy.on_step(*valid, 0, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  const Lot original = book.lots.front();
  const std::uint64_t original_next_id = next_lot_id;

  const Status failed = strategy.on_step(*missing, 1, book, next_lot_id);
  ASSERT_FALSE(failed.has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  expect_lot_equal(book.lots.front(), original);
  EXPECT_EQ(next_lot_id, original_next_id);
  EXPECT_TRUE(strategy.entry_risk_seeds().empty());

  ASSERT_TRUE(strategy.on_step(*valid, 1, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  EXPECT_EQ(book.lots.front().id, original_next_id);
  EXPECT_EQ(book.lots.front().cohort, original.cohort + 1u);
}

TEST(Strategy, CloseAtHorizonNoTradeStillClosesLotsAtTheHorizon) {
  // A no-trade step is NOT a no-close step. Under DropRenormalize an unbuildable
  // entry is an Ok/no-trade step and the RUN CONTINUES, so a lot left in the book
  // here rides past its close horizon all the way to expiry and settles
  // intrinsically -- the exact economics CloseAtHorizon exists to avoid. The
  // horizon close is an unconditional risk rule, not a leg of the entry.
  const PricedSurface valid_surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const PricedSurface unrelated_surface = make_surface(kUid + 1, 100.0, 100.0, kBaseNow);
  auto valid = snapshot_of({{"SPY", &valid_surface}}, "close-horizon-no-trade-valid");
  auto missing = snapshot_of({{"OTHER", &unrelated_surface}}, "close-horizon-no-trade-missing");
  ASSERT_TRUE(valid.has_value()) << valid.error().to_string();
  ASSERT_TRUE(missing.has_value()) << missing.error().to_string();

  StrategySpec spec;
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = 0.30; // the 0.25y lot entered below sits AT its horizon
  spec.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 1};
  spec.legs.push_back(fixed_call(kUid));
  DeclarativeStrategy strategy{spec};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strategy.on_step(*valid, 0, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  const std::uint64_t next_id_after_entry = next_lot_id;

  // Step 1: the only leg's uid is absent, so under DropRenormalize the cohort
  // build yields a NO-TRADE (Ok) step rather than an error.
  const Status no_trade = strategy.on_step(*missing, 1, book, next_lot_id);
  ASSERT_TRUE(no_trade.has_value()) << no_trade.error().to_string();
  EXPECT_TRUE(book.lots.empty());              // the horizon close must still have committed
  EXPECT_EQ(next_lot_id, next_id_after_entry); // no entry => no lot ids consumed
  EXPECT_TRUE(strategy.entry_risk_seeds().empty());
}

// Review fix round 1 (I-1). Committing the horizon close turns the lot into a
// roll-close, and `execute` fails closed when a roll-close lot has no surface on
// the step ("run_backtest: no surface for roll-close lot", src/backtest.cpp). This
// pins that outcome for the no-trade branch AND shows it is not a new class of
// failure: the pre-existing !d.open (off-tick) branch commits the same closes and
// aborts identically on the same corpus. Only observable under
// UnpricedLotPolicy::ExcludeAndReport — the default Error policy aborts on the
// unpriced held lot earlier in the same iteration.
TEST(Strategy, CloseAtHorizonClosingALotWithNoSurfaceFailsClosedInTheEngine) {
  // Date 0 holds SPY (the entry, hence the lot); date 1 holds only an unrelated
  // name, so on the step that closes the lot at its horizon it has no surface.
  const fs::path dir = fresh_dir("close-horizon-no-surface");
  const PricedSurface spy = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const PricedSurface other = make_surface(kUid + 1, 100.0, 100.0, kBaseNow + kDayNs);
  const std::string d0 = write_archive(dir, "2026-11-01", {{"SPY", &spy}});
  const std::string d1 = write_archive(dir, "2026-11-02", {{"OTHER", &other}});
  auto clock = Clock::from_manifest(make_manifest({{"2026-11-01", d0}, {"2026-11-02", d1}}));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  RunConfig cfg;
  cfg.unpriced = UnpricedLotPolicy::ExcludeAndReport;

  const auto run = [&](LifecycleSpec::Entry entry, unsigned every_n) {
    StrategySpec spec;
    spec.lifecycle.entry = entry;
    spec.lifecycle.entry_every_n = every_n;
    spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
    spec.lifecycle.roll_at_T = 0.30; // the 0.25y lot is at its horizon on date 1
    spec.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 1};
    spec.legs.push_back(fixed_call(kUid));
    DeclarativeStrategy strat(spec);
    return run_backtest(*clock, strat, cfg);
  };

  // (a) The no-trade branch this task changed: EveryStep ticks on date 1, the
  //     entry cannot be built (SPY absent + DropRenormalize -> no-trade), and the
  //     staged close commits.
  const auto no_trade = run(LifecycleSpec::Entry::EveryStep, 1);
  ASSERT_FALSE(no_trade.has_value()) << "closing a lot with no surface must fail closed";
  EXPECT_EQ(no_trade.error().code(), ErrorCode::NotFound);
  EXPECT_NE(no_trade.error().to_string().find("no surface for roll-close lot"), std::string::npos)
      << no_trade.error().to_string();

  // (b) The pre-existing off-tick branch, untouched by this task: EveryNDays(2)
  //     makes date 1 a non-entry step, so `!d.open` commits the very same close and
  //     meets the very same engine contract. Identical failure => the abort is a
  //     property of CloseAtHorizon over a dataless day, not of the no-trade fix.
  const auto off_tick = run(LifecycleSpec::Entry::EveryNDays, 2);
  ASSERT_FALSE(off_tick.has_value());
  EXPECT_EQ(off_tick.error().code(), ErrorCode::NotFound);
  EXPECT_NE(off_tick.error().to_string().find("no surface for roll-close lot"), std::string::npos)
      << off_tick.error().to_string();
}

TEST(Strategy, DropRenormalizePropagatesInvalidAndOverflowingTenors) {
  constexpr std::int64_t kNearMaxTs = std::numeric_limits<std::int64_t>::max() - 100 * kDayNs;
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kNearMaxTs);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "drop-invalid-tenor");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  const auto expect_invalid = [&](double invalid_T) {
    StrategySpec spec;
    spec.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 1};
    spec.legs.push_back(fixed_call(kUid, 20.0 / 365.25));
    spec.legs.push_back(fixed_call(kUid, invalid_T));
    std::vector<ResolveDrop> dropped;
    const auto resolved = resolve_spec_with_policy(*snapshot, spec, &dropped);
    ASSERT_FALSE(resolved.has_value());
    EXPECT_EQ(resolved.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(dropped.empty());
  };
  expect_invalid(std::numeric_limits<double>::infinity());
  expect_invalid(120.0 / 365.25); // valid model tenor, but absolute expiry overflows int64
}

TEST(Strategy, DropRenormalizeStillDropsMissingUid) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "drop-missing-uid-only");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  StrategySpec spec;
  spec.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 1};
  spec.legs.push_back(fixed_call(kUid));
  spec.legs.push_back(fixed_call(kUid + 100));
  std::vector<ResolveDrop> dropped;
  const auto resolved = resolve_spec_with_policy(*snapshot, spec, &dropped);
  ASSERT_TRUE(resolved.has_value()) << resolved.error().to_string();
  EXPECT_EQ(resolved->size(), 1u);
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_NE(dropped.front().detail.find("no surface"), std::string::npos);
}

TEST(Strategy, AdaptivePriceOptionsKeepFinalAnalyticSizingCold) {
  const fs::path dir = fresh_dir("adaptive-price-options-analytic");
  const PricedSurface archived = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_archive(dir, "2026-08-01", {{"SPY", &archived}});
  auto snapshot = MarketSnapshot::load(path, QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  StrategySpec spec;
  spec.resolution.fast_screen_cold_confirm = true;
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.50;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Call;
  leg.strike = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::TargetTheta, 10.0, -1.0};
  spec.legs.push_back(leg);

  PriceOptions options;
  options.analytic_greeks = true;
  options.query_execution = QueryExecution::Configured;
  const auto sized = resolve_spec(*snapshot, spec, options);
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 1u);
  const SizedLeg &sl = sized->front();
  const SurfaceRef resolved_surface = snapshot->find(kUid);
  ASSERT_NE(resolved_surface, nullptr);
  const auto expected = resolved_surface->greeks_analytic(sl.leg.K, sl.leg.T, sl.leg.side,
                                                          QueryExecution::ColdReference);
  ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
  // Route parity (batched laned-AVX2 vs single-contract re-query) — see above.
  using atx::vol::test::laned_greeks_close;
  EXPECT_TRUE(laned_greeks_close(sl.leg.model_price, expected->price));
  EXPECT_TRUE(laned_greeks_close(sl.leg.vega, expected->vega));
  EXPECT_TRUE(laned_greeks_close(sl.leg.theta, expected->theta));
  EXPECT_TRUE(laned_greeks_close(sl.leg.gamma, expected->gamma));
}

TEST(Strategy, AdaptiveColdSeedReusesUnderConfiguredLegacyAndColdSurfaceTiers) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  for (const QueryPricingTier tier :
       {QueryPricingTier::LegacyCompatible, QueryPricingTier::ColdReference}) {
    const char *const tag = tier == QueryPricingTier::LegacyCompatible
                                ? "adaptive-seed-legacy-alias"
                                : "adaptive-seed-cold-alias";
    const fs::path dir = fresh_dir(tag);
    const PricedSurface archived = make_surface(kUid, 100.0, 100.0, kBaseNow);
    const std::string path = write_archive(dir, "2026-08-01", {{"SPY", &archived}});
    auto snapshot = MarketSnapshot::load(path, tier);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

    StrategySpec spec;
    spec.resolution.fast_screen_cold_confirm = true;
    LegSpec leg = fixed_call(kUid, 0.25);
    leg.strike = {StrikeSelector::Kind::Delta, 0.40};
    spec.legs.push_back(leg);
    DeclarativeStrategy strategy{spec};
    PortfolioState book;
    std::uint64_t next_lot_id = 1;
    PriceOptions options;
    options.n_threads = 1;
    options.analytic_greeks = true;
    options.query_execution = QueryExecution::Configured;
    ASSERT_TRUE(strategy.on_step(*snapshot, 0, book, next_lot_id, options).has_value());
    ASSERT_EQ(book.lots.size(), 1u);
    ASSERT_EQ(strategy.entry_risk_seeds().size(), 1u);
    EXPECT_EQ(strategy.entry_risk_seeds().front().query_execution(), QueryExecution::ColdReference);

    const Lot &lot = book.lots.front();
    const std::vector<Position> positions{Position{lot.id, lot.contract, lot.qty, lot.multiplier}};
    auto portfolio = Portfolio::create(positions);
    ASSERT_TRUE(portfolio.has_value());
    const PortfolioPricer pricer{std::move(*portfolio)};
    PortfolioWorkspace workspace;
    PriceFrame frame;
    frame.id.resize(1);
    frame.uid.resize(1);
    frame.pv.resize(1);
    frame.price.resize(1);
    frame.iv.resize(1);
    frame.delta.resize(1);
    frame.gamma.resize(1);
    frame.vega.resize(1);
    frame.theta.resize(1);
    frame.rho.resize(1);
    frame.vanna.resize(1);
    frame.volga.resize(1);
    frame.charm.resize(1);
    frame.status.resize(1);
    const PriceFrameView view{frame.id,    frame.uid,   frame.pv,    frame.price,  frame.iv,
                              frame.delta, frame.gamma, frame.vega,  frame.theta,  frame.rho,
                              frame.vanna, frame.volga, frame.charm, frame.status, &frame.total};
    if constexpr (counters_enabled()) {
      atx::vol::counters::reset();
    }
    ASSERT_TRUE(pricer
                    .price_into(snapshot->set(), PriceFieldMask::FullGreeks, view, workspace,
                                options, strategy.entry_risk_seeds())
                    .has_value());
    EXPECT_EQ(frame.status.front(), PriceStatus::Ok);
    EXPECT_EQ(frame.price.front(), strategy.entry_risk_seeds().front().greeks().price);
    if constexpr (counters_enabled()) {
      const auto measured = atx::vol::counters::snapshot();
      EXPECT_EQ(measured.get(Counter::FullGreekSeedReuseLanes), 1u);
      EXPECT_EQ(measured.get(Counter::FullGreekSeedRejectedCandidates), 0u);
      EXPECT_EQ(measured.get(Counter::SurfaceFullGreekRoutes), 0u);
      EXPECT_EQ(measured.get(Counter::BoundarySolves), 0u);
    }
  }
}

TEST(Strategy, AdaptiveStrikeResolutionFallsBackToColdSolverWhenRefinementIsDisabled) {
  PricedSurface source = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::CarryBank);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  const PricedSurface fast = std::move(*prepared);
  ResolutionOptions options{/*fast_screen_cold_confirm=*/true};
  options.cold_delta_tolerance = 1.0e-8;
  options.max_refine_iterations = 0;

  const auto strike = resolve_strike_by_delta(fast, 0.50, Side::Put, 0.25, options);
  ASSERT_TRUE(strike.has_value()) << strike.error().to_string();
  const auto achieved = fast.delta(*strike, 0.50, Side::Put, QueryExecution::ColdReference);
  ASSERT_TRUE(achieved.has_value()) << achieved.error().to_string();
  EXPECT_LE(std::fabs(std::fabs(*achieved) - 0.25), options.cold_delta_tolerance);
}

// ── 2. Structures: a 40d strangle => OTM call + OTM put ─────────────────────
TEST(Strategy, InvalidAdaptiveOptionsFailBeforeAnyMissingNameDrop) {
  const fs::path dir = fresh_dir("adaptive-invalid-options");
  const PricedSurface archived = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_archive(dir, "2026-08-01", {{"SPY", &archived}});
  auto snapshot = MarketSnapshot::load(path, QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  for (const StrikeSelector::Kind selector_kind :
       {StrikeSelector::Kind::AbsStrike, StrikeSelector::Kind::Delta}) {
    StrategySpec spec;
    spec.resolution.fast_screen_cold_confirm = true;
    spec.resolution.cold_delta_tolerance = 0.0;
    spec.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 0u};
    LegSpec leg;
    leg.uid = kUid;
    leg.tenor.target_T = 0.50;
    leg.structure.kind = StructureSpec::Kind::Single;
    leg.structure.single_side = Side::Call;
    leg.strike =
        StrikeSelector{selector_kind, selector_kind == StrikeSelector::Kind::Delta ? 0.40 : 100.0};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
    std::vector<ResolveDrop> dropped;

    const auto result = resolve_spec_with_policy(*snapshot, spec, &dropped);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(dropped.empty()) << "invalid configuration is not missing market data";
  }
}

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

TEST(Strategy, SnapToListedFailsClosedAtStrikeResolution) {
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow);
  TenorSpec tenor;
  tenor.target_T = 0.25;
  tenor.snap_to_listed = true;

  const auto strike =
      resolve_strike(s, tenor, Side::Call, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0});

  ASSERT_FALSE(strike.has_value());
  EXPECT_EQ(strike.error().code(), ErrorCode::NotImplemented);
  EXPECT_NE(strike.error().message().find("model-on-model"), std::string::npos);
  EXPECT_NE(strike.error().message().find("listed OPRA workflow"), std::string::npos);
}

TEST(Strategy, SnapToListedCannotBeDroppedByMissingNamePolicy) {
  const fs::path dir = fresh_dir("snap-to-listed-policy");
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string p = write_archive(dir, "2026-08-01", {{"SPX", &s}});
  auto snap = MarketSnapshot::load(p);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  LegSpec listed;
  listed.uid = kUid;
  listed.symbol = "SPX";
  listed.tenor.target_T = 0.25;
  listed.tenor.snap_to_listed = true;
  listed.structure.kind = StructureSpec::Kind::Single;
  listed.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};

  LegSpec model = listed;
  model.tenor.snap_to_listed = false;

  StrategySpec spec;
  spec.legs = {listed, model};
  spec.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 1};
  std::vector<ResolveDrop> dropped;

  const auto sized = resolve_spec_with_policy(*snap, spec, &dropped);

  ASSERT_FALSE(sized.has_value());
  EXPECT_EQ(sized.error().code(), ErrorCode::NotImplemented);
  EXPECT_NE(sized.error().message().find("listed OPRA workflow"), std::string::npos);
  EXPECT_TRUE(dropped.empty());
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
  const std::vector<int> day_off = {0, 5, 10, 15, 20, 25, 30};
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
  // Exactly 20 days so every cohort's engine-owned settlement has an exact
  // snapshot observation; a later snapshot spot is not a settlement price.
  leg.tenor.target_T = (20.0 * static_cast<double>(kDayNs)) / kNsPerYear;
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

  // Monotonic +1 growth before the first exact expiry observation (rows 0..3).
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(r.n_open_lots[i], static_cast<double>(i + 1)) << "row " << i;
  }
  // Thereafter one cohort settles and one opens at each five-day step.
  double tot_settle = 0.0;
  for (const double x : r.pnl_settlement) {
    tot_settle += std::fabs(x);
  }
  EXPECT_GT(tot_settle, 0.0);
  EXPECT_EQ(r.n_open_lots[4], 4.0);
  EXPECT_EQ(r.n_open_lots.back(), 4.0);

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

// ── FIX-E C-1: the correlation telemetry must describe the book actually built
//
// `corr_vega` / `corr_gamma` are PERSISTED track columns (they become series in
// the run artifact), so a unit error in them outlives the run. Both are
// `correlation_{vega,gamma}(signal, index_signed_vega)`, and the derivative they
// multiply — `d sigma_idx / d rho` — is per UNIT vol, so `index_signed_vega`
// must be the index leg's dollar vega per UNIT vol.
//
// Since E1 `DispersionConfig::target_vega` is read as dollars per VOL POINT, so
// the leg the sizing builds carries `target_vega / kVegaPerVolPoint` = 100x that
// number. `signals()` passed `target_vega` RAW, understating both columns by
// exactly 100x, and nothing asserted the wiring.
//
// This test closes that: the expectation is derived from the BUILT BOOK
// (`straddle_vega * straddle_qty * multiplier`, cross-checked against the priced
// portfolio's own vega column) and the signal's own derivative fields — never
// from the expression under test. It therefore asserts the WIRING, not the
// arithmetic, which `DispersionX4.CorrelationGamma_MatchesHandComputedDerivatives`
// already covers.
TEST(Strategy, DispersionCorrelationTelemetryMatchesTheBuiltBook) {
  const fs::path dir = fresh_dir("dispersion-corr-telemetry");
  const PricedSurface idx = make_surface(1, 500.0, 500.0, kBaseNow, 0.00);
  const PricedSurface n0 = make_surface(2, 100.0, 100.0, kBaseNow, 0.02);
  const PricedSurface n1 = make_surface(3, 120.0, 120.0, kBaseNow, 0.03);
  const std::string path =
      write_archive(dir, "2026-10-01", {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}});
  auto snap = MarketSnapshot::load(path);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"IDX", 1, 0.0};
  u.names.push_back(DispersionMember{"NM0", 2, 0.6});
  u.names.push_back(DispersionMember{"NM1", 3, 0.4});
  DispersionConfig cfg; // ShortIndexLongNames, target_vega = 10000 $/vol-pt, mult 100
  cfg.record_diagnostics = true;
  const DispersionStrategy strat{u, cfg};

  // ── what the strategy actually emitted ───────────────────────────────────
  const std::vector<std::pair<std::string, double>> rows = strat.signals(*snap);
  const auto row = [&rows](const char *key) -> double {
    for (const auto &kv : rows) {
      if (kv.first == key) {
        return kv.second;
      }
    }
    ADD_FAILURE() << "signal row '" << key << "' was not emitted";
    return std::numeric_limits<double>::quiet_NaN();
  };

  // ── the expectation, DERIVED FROM THE BOOK ───────────────────────────────
  auto book = strat.build_book(*snap);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  // `straddle_qty` already carries the side's sign, so this product is the index
  // leg's SIGNED dollar vega per unit vol.
  const double book_index_signed_vega =
      book->index_leg.straddle_vega * book->index_leg.straddle_qty * cfg.multiplier;
  ASSERT_LT(book_index_signed_vega, 0.0) << "a short-index book must carry negative index vega";

  // Tie that leg quantity to the PRICED book, so the derivation is anchored on
  // the portfolio the strategy opens rather than on the sizing struct alone.
  {
    auto pf = Portfolio::create(book->positions);
    ASSERT_TRUE(pf.has_value());
    const PortfolioPricer pricer{std::move(*pf)};
    auto frame = pricer.price(snap->set());
    ASSERT_TRUE(frame.has_value()) << frame.error().to_string();
    double priced_index_vega = 0.0;
    for (std::size_t i = 0; i < frame->size(); ++i) {
      if (frame->uid[i] == 1) {
        priced_index_vega += frame->vega[i];
      }
    }
    EXPECT_NEAR(priced_index_vega, book_index_signed_vega,
                5.0e-3 * std::fabs(book_index_signed_vega))
        << "the leg's straddle_vega*qty*mult is not the book's priced index vega";
  }

  auto sig = dispersion_signal(u, snap->set(), cfg.target_T);
  ASSERT_TRUE(sig.has_value()) << sig.error().to_string();
  ASSERT_GT(std::fabs(sig->d_sigma_d_rho), 0.0); // else both assertions are vacuous

  //   dP/drho   = v * dsigma/drho
  //   d2P/drho2 = v * [ (-sigma*T/4)*(dsigma/drho)^2 + d2sigma/drho2 ]
  const double expect_corr_vega = book_index_signed_vega * sig->d_sigma_d_rho;
  const double expect_corr_gamma =
      book_index_signed_vega *
      (-0.25 * sig->sigma_index * sig->T_used * sig->d_sigma_d_rho * sig->d_sigma_d_rho +
       sig->d2_sigma_d_rho2);

  EXPECT_NEAR(row("corr_vega"), expect_corr_vega, 1.0e-9 * std::fabs(expect_corr_vega))
      << "corr_vega does not describe the index leg the strategy builds";
  EXPECT_NEAR(row("corr_gamma"), expect_corr_gamma, 1.0e-9 * std::fabs(expect_corr_gamma))
      << "corr_gamma does not describe the index leg the strategy builds";

  // ── NON-VACUITY ──────────────────────────────────────────────────────────
  // The retired expression passed `-target_vega` ($ per VOL POINT). Pin that it
  // is exactly 1/kVegaPerVolPoint = 100x away from the book's per-unit-vol vega,
  // so the assertions above cannot both hold under the old wiring.
  const double retired_index_signed_vega = -cfg.target_vega;
  EXPECT_NEAR(book_index_signed_vega, retired_index_signed_vega / kVegaPerVolPoint,
              1.0e-9 * std::fabs(book_index_signed_vega));
  EXPECT_GT(std::fabs(row("corr_vega")),
            50.0 * std::fabs(correlation_vega(*sig, retired_index_signed_vega)));

  std::printf("[strategy] corr_vega=%.6f corr_gamma=%.6f book_index_vega=%.2f\n", row("corr_vega"),
              row("corr_gamma"), book_index_signed_vega);
}

// ── C-2 (pipeline-m production review): the X3 gross-vega LIMIT is DOLLARS PER
//    VOL POINT, at every multiplier — not just at the historical 100 ──────────
//
// `DispersionRiskLimits::max_gross_vega` is documented in strategy.hpp as a
// dollar cap denominated per VOL POINT. The quantity it is compared against is
// the X3 risk probe's gross vega, and `DispersionLeg::straddle_vega` is a
// PER-SHARE dP/dsigma per UNIT vol (dispersion.hpp), so a leg's contribution is
//
//     |straddle_vega * straddle_qty| * multiplier * kVegaPerVolPoint
//
// `multiplier` became a real typed run-spec field in this branch
// (dispersion_run.cpp binds `multiplier`; dispersion_backtest.cpp routes it into
// `DispersionConfig::multiplier`), so a spec at 250 or 1000 is now reachable
// from production.
//
// THE ORACLE IS HAND-DERIVED FROM THE CONTRACT, NOT FROM THE CODE. Under the
// default VegaNeutral scheme the index leg is sized to carry exactly
// `target_vega` dollars per vol point, and the basket is sized to match it leg
// by normalized weight — Σ ŵ_k = 1 — so the whole book's GROSS vega is exactly
//
//     2 * target_vega     dollars per vol point, for ANY multiplier.
//
// That is why this test is an independent oracle where `natural_gross_vega`
// (dispersion_workflow_test.cpp) is not: that helper re-evaluates the production
// expression and therefore cannot detect a wrong one.
TEST(Strategy, DispersionGrossVegaLimitIsDollarsPerVolPointAtNonHistoricalMultiplier) {
  const fs::path dir = fresh_dir("dispersion-gross-vega-unit");
  const PricedSurface idx = make_surface(1, 500.0, 500.0, kBaseNow, 0.00);
  const PricedSurface n0 = make_surface(2, 100.0, 100.0, kBaseNow, 0.02);
  const PricedSurface n1 = make_surface(3, 120.0, 120.0, kBaseNow, 0.03);
  const std::string path =
      write_archive(dir, "2026-10-01", {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}});
  auto snap = MarketSnapshot::load(path);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"IDX", 1, 0.0};
  u.names.push_back(DispersionMember{"NM0", 2, 0.6});
  u.names.push_back(DispersionMember{"NM1", 3, 0.4});

  constexpr double kMultiplier = 250.0;   // deliberately NOT the historical 100
  constexpr double kTargetVega = 10000.0; // $ of index-leg vega per ONE vol point

  DispersionConfig cfg;
  cfg.target_vega = kTargetVega;
  cfg.multiplier = kMultiplier;

  DispersionStrategy strategy{u, cfg};
  DispersionRiskLimits limits;
  // Any binding cap will do: the assertion reads the RECORDED `requested`, which
  // is the probe's own measurement of the book it was handed.
  limits.max_gross_vega = 1.0;
  limits.action = RiskBreachAction::Clamp;
  strategy.set_risk_limits(limits);

  PortfolioState book;
  std::uint64_t next_lot_id = 1u;
  ASSERT_TRUE(strategy.on_step(*snap, 0u, book, next_lot_id).has_value());
  ASSERT_EQ(strategy.risk_events().size(), 1u) << "the tight cap must bind and be recorded";
  ASSERT_EQ(strategy.risk_events().front().reason, RiskBreachReason::GrossVega);
  const double measured = strategy.risk_events().front().requested;

  const double expected_usd_per_vol_point = 2.0 * kTargetVega;
  EXPECT_NEAR(measured, expected_usd_per_vol_point, 1.0e-9 * expected_usd_per_vol_point)
      << "the gross-vega limit is not being compared in dollars per vol point";

  // NON-VACUITY. The retired expression `Σ|straddle_vega × straddle_qty|` drops
  // BOTH the multiplier and the vol-point scale, so it equals the correct
  // quantity times 100/multiplier — 0.4x here. Pin that gap so the assertion
  // above cannot be satisfied by the expression it replaced.
  const auto built = strategy.build_book(*snap);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  double retired = std::fabs(built->index_leg.straddle_vega * built->index_leg.straddle_qty);
  for (const DispersionLeg &leg : built->name_legs) {
    retired += std::fabs(leg.straddle_vega * leg.straddle_qty);
  }
  EXPECT_NEAR(retired, expected_usd_per_vol_point * 100.0 / kMultiplier,
              1.0e-9 * expected_usd_per_vol_point)
      << "the retired expression's known 100/multiplier error is not reproduced";
  EXPECT_LT(retired, 0.5 * expected_usd_per_vol_point)
      << "at multiplier 250 the two quantities must be far apart, else this is vacuous";

  std::printf("[strategy] gross_vega_per_vol_point=%.6f retired=%.6f multiplier=%.0f\n", measured,
              retired, kMultiplier);
}

// ── 6. Dispersion lifecycle is a config parameter, not a constant ───────────
// `make_dispersion_backtest_strategy` used to hardcode EveryNDays/RollAtHorizon,
// so the library orchestration could express exactly ONE book shape. This gate
// pins BOTH shapes through the same `run_dispersion_backtest` entry point and
// asserts they differ in the way the lifecycle says they should:
//
//   default (EveryNDays/RollAtHorizon) — one clip; the book returns to clip size
//     after each roll, so n_open_lots never exceeds one cohort.
//   ladder  (EveryStep/HoldToExpiry)   — a fresh cohort per step, none closed at
//     a horizon, so n_open_lots is exactly (step+1) cohorts for a tenor long
//     enough that nothing has expired yet.
//
// The default-config half is the NEGATIVE CONTROL: it is the behaviour the old
// hardcoded strategy had, and it must still hold, so a regression that made the
// lifecycle fields inert would fail the ladder half while leaving this one green
// (and one that wired them backwards fails this one). Neither assertion alone
// distinguishes "configurable" from "hardcoded to the other value".
TEST(Strategy, DispersionLifecycleIsConfigurableAndLadderAccumulatesCohorts) {
  constexpr std::size_t kLadderDates = 6;
  constexpr std::size_t kLadderNames = 2;
  // One lot per straddle leg: (index + names) * 2. Fixed by build_dispersion_book.
  constexpr double kClipLots = static_cast<double>(1 + kLadderNames) * 2.0;
  // 90 calendar days at one day per step: with only 6 steps nothing reaches
  // expiry, so every entered cohort is still open on the last row. That is what
  // makes the expected count exact rather than a lower bound.
  constexpr double kTenorDays = 90.0;

  const fs::path dir = fresh_dir("dispersion-ladder");
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < kLadderDates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double drift = 1.0 + 0.002 * static_cast<double>(d);
    const PricedSurface idx = make_surface(1, 500.0 * drift, 500.0 * drift, now, 0.00);
    const PricedSurface n0 = make_surface(2, 100.0 * drift, 100.0 * drift, now, 0.02);
    const PricedSurface n1 = make_surface(3, 120.0 * drift, 120.0 * drift, now, 0.03);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-11-%02d", static_cast<int>(d + 1));
    const std::string date = buf;
    dp.emplace_back(date, write_archive(dir, date, {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}}));
  }
  auto clock = Clock::from_manifest(make_manifest(dp));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"IDX", 1, 0.0};
  u.names.push_back(DispersionMember{"NM0", 2, 0.5});
  u.names.push_back(DispersionMember{"NM1", 3, 0.5});

  // The helper is the documented way to build the ladder shape; assert it sets
  // what it claims rather than trusting the run to imply it, so a helper that
  // silently stopped setting the lifecycle would be caught here and not only via
  // the (slower, more indirect) book-count assertion below.
  const DispersionBacktestConfig ladder =
      make_dispersion_ladder_config(kTenorDays, 10'000.0, kLadderNames);
  EXPECT_EQ(ladder.entry, LifecycleSpec::Entry::EveryStep);
  EXPECT_EQ(ladder.holding, LifecycleSpec::Holding::HoldToExpiry);
  EXPECT_EQ(ladder.target_dte_days, kTenorDays);

  DispersionBacktestConfig rolling; // inherited defaults = the old hardcoded pair
  rolling.min_names = kLadderNames;
  rolling.target_dte_days = kTenorDays;
  ASSERT_EQ(rolling.entry, LifecycleSpec::Entry::EveryNDays);
  ASSERT_EQ(rolling.holding, LifecycleSpec::Holding::RollAtHorizon);

  auto ladder_run = run_dispersion_backtest(*clock, u, ladder);
  ASSERT_TRUE(ladder_run.has_value()) << ladder_run.error().to_string();
  auto rolling_run = run_dispersion_backtest(*clock, u, rolling);
  ASSERT_TRUE(rolling_run.has_value()) << rolling_run.error().to_string();
  ASSERT_EQ(ladder_run->size(), kLadderDates);
  ASSERT_EQ(rolling_run->size(), kLadderDates);

  for (std::size_t i = 0; i < kLadderDates; ++i) {
    EXPECT_EQ(ladder_run->n_open_lots[i], static_cast<double>(i + 1) * kClipLots)
        << "ladder row " << i;
    EXPECT_LE(rolling_run->n_open_lots[i], kClipLots) << "rolling row " << i;
  }
  // The whole point of the parameter: the shapes must actually diverge. Without
  // this, both loops above would still pass if the ladder collapsed to one clip
  // on a one-date clock.
  EXPECT_GT(ladder_run->n_open_lots.back(), rolling_run->n_open_lots.back());

  std::printf("[strategy] ladder n_open_lots back=%.0f vs rolling=%.0f (clip=%.0f)\n",
              ladder_run->n_open_lots.back(), rolling_run->n_open_lots.back(), kClipLots);
}

TEST(Strategy, DispersionHonorsPriceOptionsAndPublishesExactEntrySeeds) {
  const fs::path dir = fresh_dir("dispersion-price-options-seeds");
  const PricedSurface idx = make_surface(1, 500.0, 500.0, kBaseNow, 0.00);
  const PricedSurface n0 = make_surface(2, 100.0, 100.0, kBaseNow, 0.02);
  const PricedSurface n1 = make_surface(3, 120.0, 120.0, kBaseNow, 0.03);
  const std::string path =
      write_archive(dir, "2026-10-01", {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}});
  auto snapshot = MarketSnapshot::load(path, QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  DispersionUniverse universe;
  universe.index = DispersionMember{"IDX", 1, 0.0};
  universe.names.push_back(DispersionMember{"NM0", 2, 0.6});
  universe.names.push_back(DispersionMember{"NM1", 3, 0.4});
  PriceOptions options;
  options.analytic_greeks = true;
  options.query_execution = QueryExecution::ColdReference;

  for (const bool projected : {false, true}) {
    DispersionConfig config;
    config.target_T = 0.123456789012345;
    if (projected) {
      config.projected_maturity = ProjectedMaturitySpec::days(30);
    }
    const auto seeded_book = build_dispersion_book(universe, snapshot->set(), config, options);
    ASSERT_TRUE(seeded_book.has_value()) << seeded_book.error().to_string();
    ASSERT_EQ(seeded_book->entry_risk_seeds.size(), seeded_book->positions.size());
    ASSERT_GE(seeded_book->entry_risk_seeds.size(), 2u);
    EXPECT_EQ(seeded_book->index_leg.sigma, seeded_book->entry_risk_seeds[0].iv());
    ASSERT_EQ(seeded_book->name_legs.size(), universe.names.size());
    for (std::size_t name_index = 0; name_index < seeded_book->name_legs.size(); ++name_index) {
      EXPECT_EQ(seeded_book->name_legs[name_index].sigma,
                seeded_book->entry_risk_seeds[2u + 2u * name_index].iv());
    }

    DispersionStrategy strategy{universe, config};
    PortfolioState book;
    std::uint64_t next_lot_id = 1;
    ASSERT_TRUE(strategy.on_step(*snapshot, 0, book, next_lot_id, options).has_value());
    ASSERT_EQ(book.lots.size(), 6u);
    ASSERT_EQ(strategy.entry_risk_seeds().size(), book.lots.size());
    for (std::size_t i = 0; i < book.lots.size(); ++i) {
      const Lot &lot = book.lots[i];
      const FullGreekSeed &seed = strategy.entry_risk_seeds()[i];
      EXPECT_EQ(seed.uid(), lot.contract.uid) << i;
      EXPECT_EQ(seed.K(), lot.contract.K) << i;
      EXPECT_EQ(seed.T(), lot.contract.T) << i;
      EXPECT_EQ(seed.side(), lot.contract.side) << i;
      EXPECT_TRUE(seed.analytic_greeks()) << i;
      EXPECT_EQ(seed.query_execution(), QueryExecution::ColdReference) << i;
      EXPECT_EQ(seed.greeks().price, lot.entry_price) << i;
    }

    ASSERT_TRUE(strategy.on_step(*snapshot, 1, book, next_lot_id, options).has_value());
    EXPECT_TRUE(strategy.entry_risk_seeds().empty());
  }
}

// ── 6. CloseAtHorizon: overlapping cohorts, each closed at ITS OWN DTE ───────
TEST(Strategy, DispersionFourArgEntryMatchesLegacyBuildBookExactly) {
  const fs::path dir = fresh_dir("dispersion-legacy-four-arg-parity");
  const PricedSurface idx = make_surface(1, 500.0, 500.0, kBaseNow, 0.00);
  const PricedSurface n0 = make_surface(2, 100.0, 100.0, kBaseNow, 0.02);
  const PricedSurface n1 = make_surface(3, 120.0, 120.0, kBaseNow, 0.03);
  const std::string path =
      write_archive(dir, "2026-10-01", {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}});
  auto snapshot = MarketSnapshot::load(path);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  DispersionUniverse universe;
  universe.index = DispersionMember{"IDX", 1, 0.0};
  universe.names.push_back(DispersionMember{"NM0", 2, 0.6});
  universe.names.push_back(DispersionMember{"NM1", 3, 0.4});
  for (const bool projected : {false, true}) {
    DispersionConfig config;
    config.target_T = 0.123456789012345;
    if (projected) {
      config.projected_maturity = ProjectedMaturitySpec::days(30);
    }
    DispersionStrategy strategy{universe, config};
    const auto expected = strategy.build_book(*snapshot);
    ASSERT_TRUE(expected.has_value()) << expected.error().to_string();

    PortfolioState actual;
    std::uint64_t next_lot_id = 1u;
    ASSERT_TRUE(strategy.on_step(*snapshot, 0u, actual, next_lot_id).has_value());
    ASSERT_EQ(actual.lots.size(), expected->positions.size());
    ASSERT_EQ(expected->entry_marks.size(), expected->positions.size());
    const std::int64_t expected_expiry =
        expected->index_leg.call_definition.expiry_ts_ns != 0
            ? expected->index_leg.call_definition.expiry_ts_ns
            : snapshot->ts_ns() + std::llround(config.target_T * kNsPerYear);
    for (std::size_t i = 0; i < actual.lots.size(); ++i) {
      const Lot &lot = actual.lots[i];
      const Position &position = expected->positions[i];
      EXPECT_EQ(lot.contract.uid, position.contract.uid) << i;
      EXPECT_EQ(lot.contract.K, position.contract.K) << i;
      EXPECT_EQ(lot.contract.T, position.contract.T) << i;
      EXPECT_EQ(lot.contract.side, position.contract.side) << i;
      EXPECT_EQ(lot.qty, position.qty) << i;
      EXPECT_EQ(lot.multiplier, position.multiplier) << i;
      EXPECT_EQ(lot.entry_price, expected->entry_marks[i]) << i;
      EXPECT_EQ(lot.expiry_ts_ns, expected_expiry) << i;
    }
    EXPECT_TRUE(strategy.entry_risk_seeds().empty());
  }
}

TEST(Strategy, CloseAtHorizonOverlappingCohorts) {
  // 10 consecutive daily snapshots. Tenor 6 calendar days, close below 2.5
  // days residual (half-day margin keeps the comparison off exact-boundary
  // floating point). A cohort opened on day d has expiry d+6; residual at age
  // 3 is 3d (alive), at age 4 is 2d (< 2.5 -> close), so each cohort lives
  // ages 0..3 = 4 days. Steady state: 4 live cohorts x 2 strangle lots = 8.
  auto corpus = make_corpus(/*n_dates=*/10, "close-horizon");
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.name = "close-at-horizon";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 6.0 / 365.25;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = 2.5 / 365.25;

  DeclarativeStrategy strat(spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->size(), 10u);
  // Ramp 2,4,6,8 then plateau at 8 (close of oldest exactly offsets the new entry).
  const unsigned expect[] = {2, 4, 6, 8, 8, 8, 8, 8, 8, 8};
  for (std::size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(r->n_open_lots[i], static_cast<double>(expect[i])) << i;
  }
  // Closes are roll-closes at marks, never engine settlement.
  for (std::size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(r->pnl_settlement[i], 0.0) << i;
  }

  std::printf("[strategy] close-at-horizon n_open_lots ="
              " {%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f}\n",
              r->n_open_lots[0], r->n_open_lots[1], r->n_open_lots[2], r->n_open_lots[3],
              r->n_open_lots[4], r->n_open_lots[5], r->n_open_lots[6], r->n_open_lots[7],
              r->n_open_lots[8], r->n_open_lots[9]);
}

// ── 7. Missing-name policy: DropRenormalize survives, floors, protects the
//        hedge leg, and Error preserves the pre-S1-3/T2 hard fail ───────────
TEST(Strategy, MissingNameDropRenormalize) {
  // Snapshot holds SPY + XOM only; spec asks for SPY-index vs {XOM, FAKE} basket.
  const PricedSurface spy = make_surface(/*uid=*/1, 500.0, 500.0, kBaseNow, 0.00);
  const PricedSurface xom = make_surface(/*uid=*/2, 110.0, 110.0, kBaseNow, 0.05);
  auto snap = snapshot_of({{"SPY", &spy}, {"XOM", &xom}}, "missing-name");
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  StrategySpec spec;
  const auto name_leg = [](std::string sym) {
    LegSpec l;
    l.symbol = std::move(sym);
    l.tenor.target_T = 0.25;
    l.structure.kind = StructureSpec::Kind::Strangle;
    l.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
    l.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
    l.size = {SizeSpec::Kind::TargetTheta, 10.0, +1.0};
    l.group = "basket";
    return l;
  };
  spec.legs.push_back(name_leg("XOM"));
  spec.legs.push_back(name_leg("FAKE"));
  LegSpec idx = name_leg("SPY");
  idx.size = {SizeSpec::Kind::TargetVega, 10000.0, -1.0};
  idx.group = "index";
  spec.legs.push_back(idx);
  spec.constraint = {CrossLegConstraint::Kind::FlatVega, "basket", "index"};
  spec.missing = {MissingNamePolicy::DropRenormalize, /*min_names=*/1};

  std::vector<ResolveDrop> dropped;
  auto legs = resolve_spec_with_policy(*snap, spec, &dropped);
  ASSERT_TRUE(legs.has_value()) << legs.error().to_string();
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].symbol, "FAKE");
  // Survivors: XOM strangle (2) + SPY strangle (2); constraint held on survivors.
  ASSERT_EQ(legs->size(), 4u);
  double net_vega = 0.0;
  double gross_vega = 0.0;
  for (const auto &sl : *legs) {
    net_vega += sl.qty * sl.leg.vega * sl.multiplier;
    gross_vega += std::fabs(sl.qty * sl.leg.vega * sl.multiplier);
  }
  EXPECT_LE(std::fabs(net_vega), 1e-9 * gross_vega);

  // min_names floor: requiring 2 surviving basket names -> Unavailable.
  spec.missing.min_names = 2;
  auto floored = resolve_spec_with_policy(*snap, spec, nullptr);
  ASSERT_FALSE(floored.has_value());
  EXPECT_EQ(floored.error().code(), ErrorCode::Unavailable);

  // Missing HEDGE leg (group_b) is never droppable.
  spec.missing.min_names = 1;
  spec.legs[2].symbol = "NOPE";
  auto no_hedge = resolve_spec_with_policy(*snap, spec, nullptr);
  ASSERT_FALSE(no_hedge.has_value());
  EXPECT_EQ(no_hedge.error().code(), ErrorCode::Unavailable);

  // Error policy preserves today's hard fail.
  spec.legs[2].symbol = "SPY";
  spec.missing = {MissingNamePolicy::Error, 2};
  auto hard = resolve_spec_with_policy(*snap, spec, nullptr);
  ASSERT_FALSE(hard.has_value());
  EXPECT_EQ(hard.error().code(), ErrorCode::NotFound);
}

// ── 8. CloseAtHorizon + an unbuildable entry (missing hedge every date) is a
//        no-trade step, never a hard error ──────────────────────────────────
TEST(Strategy, CloseAtHorizonNoTradeOnMissingEntry) {
  // Under DropRenormalize with an unbuildable entry (hedge symbol absent from
  // EVERY snapshot), DeclarativeStrategy no-trades instead of erroring, and
  // the run completes with an empty book throughout.
  auto corpus = make_corpus(/*n_dates=*/4, "close-horizon-notrade"); // archives contain SPY only
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  LegSpec l;
  l.symbol = "SPY";
  l.tenor.target_T = 0.25;
  l.structure.kind = StructureSpec::Kind::Strangle;
  l.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  l.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  l.size = {SizeSpec::Kind::TargetTheta, 10.0, +1.0};
  l.group = "basket";
  spec.legs.push_back(l);
  LegSpec idx = l;
  idx.symbol = "MISSING_INDEX";
  idx.group = "index";
  idx.size = {SizeSpec::Kind::TargetVega, 10000.0, -1.0};
  spec.legs.push_back(idx);
  spec.constraint = {CrossLegConstraint::Kind::FlatVega, "basket", "index"};
  spec.missing = {MissingNamePolicy::DropRenormalize, 1};
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = 2.0 / 365.25;

  DeclarativeStrategy strat(spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  for (std::size_t i = 0; i < r->size(); ++i) {
    EXPECT_EQ(r->n_open_lots[i], 0.0) << i;
  }
  // The DeclarativeStrategy accessor documents WHY: the index leg never
  // resolved (it's not a "drop" -- it's the hedge leg, which is fatal-to-build
  // and never reaches the drop bookkeeping), so dropped_on_last_entry() stays
  // empty while the run silently no-trades every step.
  EXPECT_TRUE(strat.dropped_on_last_entry().empty());
}

// ── 9. dropped_on_last_entry(): the per-name-failure hook, positive path ────
TEST(Strategy, DeclarativeDroppedOnLastEntryAccessor) {
  // DropRenormalize with one droppable (non-hedge) name: DeclarativeStrategy
  // opens the survivor book and documents the drop via dropped_on_last_entry().
  const PricedSurface spy = make_surface(/*uid=*/1, 500.0, 500.0, kBaseNow, 0.00);
  const PricedSurface xom = make_surface(/*uid=*/2, 110.0, 110.0, kBaseNow, 0.05);
  auto snap = snapshot_of({{"SPY", &spy}, {"XOM", &xom}}, "dropped-accessor");
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  StrategySpec spec;
  const auto name_leg = [](std::string sym) {
    LegSpec l;
    l.symbol = std::move(sym);
    l.tenor.target_T = 0.25;
    l.structure.kind = StructureSpec::Kind::Strangle;
    l.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
    l.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
    l.size = {SizeSpec::Kind::TargetTheta, 10.0, +1.0};
    l.group = "basket";
    return l;
  };
  spec.legs.push_back(name_leg("XOM"));
  spec.legs.push_back(name_leg("FAKE"));
  LegSpec idx = name_leg("SPY");
  idx.size = {SizeSpec::Kind::TargetVega, 10000.0, -1.0};
  idx.group = "index";
  spec.legs.push_back(idx);
  spec.constraint = {CrossLegConstraint::Kind::FlatVega, "basket", "index"};
  spec.missing = {MissingNamePolicy::DropRenormalize, /*min_names=*/1};
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;

  DeclarativeStrategy strat(spec);
  EXPECT_TRUE(strat.dropped_on_last_entry().empty()); // nothing attempted yet

  PortfolioState book;
  std::uint64_t next_id = 1;
  const Status st = strat.on_step(*snap, 0, book, next_id);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  ASSERT_EQ(book.lots.size(), 4u); // XOM strangle + SPY strangle survivors

  const auto dropped = strat.dropped_on_last_entry();
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].symbol, "FAKE");
}

// ── 6. RunConfig::step_observer — the L10 observation hook ───────────────────
//
// The divergence collector Wave D builds on this hook replaces a shadow replay
// loop that re-walked the clock and re-loaded every archive purely to read
// strategy state after `on_step`. These gates pin the four properties that
// substitution rests on: the observer fires once per clock step in order, it
// fires on EVERY step regardless of `record_every_n`, its `Err` aborts the run
// verbatim and immediately, and an absent observer leaves the recorded series
// bit-identical.

namespace {

// The `Strategy.OverlappingClips` corpus: 7 dates, 5 calendar days apart, one
// "SPX" surface per date at uid kUid, spot drifting +0.2%/day. Returned with the
// day offsets so a test can predict each step's snapshot timestamp independently
// of the engine.
struct ObserverCorpus {
  std::vector<int> day_off{0, 5, 10, 15, 20, 25, 30};
  std::vector<std::pair<std::string, std::string>> date_paths;
};

[[nodiscard]] ObserverCorpus make_observer_corpus(const char *tag) {
  ObserverCorpus c;
  const fs::path dir = fresh_dir(tag);
  for (std::size_t d = 0; d < c.day_off.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(c.day_off[d]) * kDayNs;
    const double S = 100.0 * (1.0 + 0.002 * static_cast<double>(c.day_off[d]));
    const PricedSurface s = make_surface(kUid, S, S, now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-09-%02d", static_cast<int>(d + 1));
    const std::string date = buf;
    c.date_paths.emplace_back(date, write_archive(dir, date, {{"SPX", &s}}));
  }
  return c;
}

// The OverlappingClips spec: a daily ATM-forward put clip held to expiry, whose
// 20-day tenor lands every cohort's settlement on an exact snapshot observation.
[[nodiscard]] StrategySpec observer_spec() {
  StrategySpec spec;
  spec.name = "spy-25d-put-daily-clip";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = (20.0 * static_cast<double>(kDayNs)) / kNsPerYear;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  return spec;
}

} // namespace

TEST(Strategy, StepObserverFiresOncePerStepInOrder) {
  const ObserverCorpus corpus = make_observer_corpus("observer-order");
  auto clock = Clock::from_manifest(make_manifest(corpus.date_paths));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  struct Seen {
    std::size_t step_index;
    std::string date;
    std::int64_t ts_ns;
    const IStrategy *strategy;
  };
  std::vector<Seen> seen;

  DeclarativeStrategy strat{observer_spec()};
  RunConfig cfg;
  cfg.step_observer = [&](const StepEvent &e) {
    seen.push_back(Seen{e.step_index, e.ref.date, e.snapshot.ts_ns(), &e.strategy});
    return atx::core::Ok();
  };
  auto res = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  // One event per clock ref, INCLUDING inception. Asserted before any indexing so
  // an observer that never fired fails here rather than vacuously passing below.
  ASSERT_EQ(seen.size(), corpus.day_off.size());
  std::int64_t prev_ts = std::numeric_limits<std::int64_t>::min();
  for (std::size_t i = 0; i < seen.size(); ++i) {
    EXPECT_EQ(seen[i].step_index, i) << "event " << i;
    // The ref is the RIGHT ref, not merely some ref: its date is the manifest's
    // date at the same index.
    EXPECT_EQ(seen[i].date, corpus.date_paths[i].first) << "event " << i;
    // The snapshot is the base the strategy stepped on: its archived timestamp is
    // the corpus's own day offset, which the engine had to round-trip through the
    // archive bytes to reproduce.
    const std::int64_t want_ts =
        kBaseNow + static_cast<std::int64_t>(corpus.day_off[i]) * kDayNs;
    EXPECT_EQ(seen[i].ts_ns, want_ts) << "event " << i;
    EXPECT_GT(seen[i].ts_ns, prev_ts) << "event " << i; // strictly increasing
    prev_ts = seen[i].ts_ns;
    // The observed strategy is the caller's own instance, not a copy.
    EXPECT_EQ(seen[i].strategy, static_cast<const IStrategy *>(&strat)) << "event " << i;
  }

  std::printf("[strategy] step_observer fired %zu times, indices 0..%zu, ts %lld..%lld\n",
              seen.size(), seen.size() - 1, static_cast<long long>(seen.front().ts_ns),
              static_cast<long long>(seen.back().ts_ns));
}

TEST(Strategy, StepObserverFiresEveryStepAtStride) {
  const ObserverCorpus corpus = make_observer_corpus("observer-stride");
  auto clock = Clock::from_manifest(make_manifest(corpus.date_paths));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  std::vector<std::size_t> indices;
  DeclarativeStrategy strat{observer_spec()};
  RunConfig cfg;
  cfg.record_every_n = 3;
  cfg.step_observer = [&](const StepEvent &e) {
    indices.push_back(e.step_index);
    return atx::core::Ok();
  };
  auto res = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  // The stride downsamples RECORDED ROWS, never events. This is what makes
  // step_index load-bearing: it is the only way to correlate an event with the
  // (fewer) rows the run persisted.
  ASSERT_EQ(indices.size(), corpus.day_off.size());
  for (std::size_t i = 0; i < indices.size(); ++i) {
    EXPECT_EQ(indices[i], i) << "event " << i;
  }
  EXPECT_LT(res->size(), corpus.day_off.size())
      << "record_every_n=3 must record fewer rows than there are steps";

  std::printf("[strategy] step_observer at stride 3: %zu events vs %zu recorded rows\n",
              indices.size(), res->size());
}

TEST(Strategy, StepObserverErrPropagatesAndStopsTheRun) {
  const ObserverCorpus corpus = make_observer_corpus("observer-err");
  auto clock = Clock::from_manifest(make_manifest(corpus.date_paths));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  std::size_t fired = 0;
  DeclarativeStrategy strat{observer_spec()};
  RunConfig cfg;
  cfg.step_observer = [&](const StepEvent &e) -> Status {
    ++fired;
    if (e.step_index == 2) {
      return atx::core::Err(ErrorCode::InvalidArgument, "observer stop");
    }
    return atx::core::Ok();
  };
  auto res = run_backtest(*clock, strat, cfg);

  ASSERT_FALSE(res.has_value()) << "an observer Err must abort the run";
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  // Verbatim propagation: the engine must not wrap or re-message the observer's
  // error, or an observer's own invariant failure becomes unattributable.
  EXPECT_NE(res.error().message().find("observer stop"), std::string::npos)
      << "message was: " << res.error().message();
  // The abort is IMMEDIATE, not deferred to the end of the run: steps 0,1,2 only.
  EXPECT_EQ(fired, 3u);

  std::printf("[strategy] step_observer Err after %zu events: %s\n", fired,
              res.error().to_string().c_str());
}

TEST(Strategy, StepObserverAbsentIsBitIdentical) {
  const ObserverCorpus corpus = make_observer_corpus("observer-identity");
  auto clock = Clock::from_manifest(make_manifest(corpus.date_paths));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy bare_strat{observer_spec()};
  auto bare = run_backtest(*clock, bare_strat, RunConfig{});
  ASSERT_TRUE(bare.has_value()) << bare.error().to_string();

  std::size_t fired = 0;
  DeclarativeStrategy observed_strat{observer_spec()}; // fresh lifecycle state
  RunConfig cfg;
  cfg.step_observer = [&fired](const StepEvent &) {
    ++fired;
    return atx::core::Ok();
  };
  auto observed = run_backtest(*clock, observed_strat, cfg);
  ASSERT_TRUE(observed.has_value()) << observed.error().to_string();

  // Anti-vacuity: both runs must be the real, fully-shaped series before any
  // column comparison can mean anything, and the observer must actually have run.
  ASSERT_EQ(bare->size(), corpus.day_off.size());
  ASSERT_EQ(observed->size(), corpus.day_off.size());
  ASSERT_EQ(fired, corpus.day_off.size());
  ASSERT_FALSE(bare->step_pnl_total.empty());

  // EXPECT_EQ on raw doubles == bit equality (no NaN is produced by this corpus,
  // which the finiteness check below pins).
  const auto same = [](const char *name, const std::vector<double> &a,
                       const std::vector<double> &b) {
    ASSERT_EQ(a.size(), b.size()) << name;
    for (std::size_t i = 0; i < a.size(); ++i) {
      ASSERT_TRUE(std::isfinite(a[i])) << name << " row " << i;
      EXPECT_EQ(a[i], b[i]) << name << " row " << i;
    }
  };
  same("pnl_total", bare->pnl_total, observed->pnl_total);
  same("pnl_delta", bare->pnl_delta, observed->pnl_delta);
  same("pnl_gamma", bare->pnl_gamma, observed->pnl_gamma);
  same("pnl_vega", bare->pnl_vega, observed->pnl_vega);
  same("pnl_vanna", bare->pnl_vanna, observed->pnl_vanna);
  same("pnl_volga", bare->pnl_volga, observed->pnl_volga);
  same("pnl_theta", bare->pnl_theta, observed->pnl_theta);
  same("pnl_rho", bare->pnl_rho, observed->pnl_rho);
  same("pnl_charm", bare->pnl_charm, observed->pnl_charm);
  same("pnl_unexplained", bare->pnl_unexplained, observed->pnl_unexplained);
  same("pnl_settlement", bare->pnl_settlement, observed->pnl_settlement);
  same("pnl_shares", bare->pnl_shares, observed->pnl_shares);
  same("financing", bare->financing, observed->financing);
  same("cost", bare->cost, observed->cost);
  same("nav", bare->nav, observed->nav);
  same("cash", bare->cash, observed->cash);
  same("gross_delta", bare->gross_delta, observed->gross_delta);
  same("gross_gamma", bare->gross_gamma, observed->gross_gamma);
  same("gross_vega", bare->gross_vega, observed->gross_vega);
  same("gross_theta", bare->gross_theta, observed->gross_theta);
  same("turnover_notional", bare->turnover_notional, observed->turnover_notional);
  same("turnover_vega", bare->turnover_vega, observed->turnover_vega);
  same("n_open_lots", bare->n_open_lots, observed->n_open_lots);
  same("n_unpriced_lots", bare->n_unpriced_lots, observed->n_unpriced_lots);
  same("n_unpriced_greeks", bare->n_unpriced_greeks, observed->n_unpriced_greeks);
  same("step_pnl_total", bare->step_pnl_total, observed->step_pnl_total);
  EXPECT_EQ(bare->date, observed->date);
  EXPECT_EQ(bare->ts_ns, observed->ts_ns);
  ASSERT_EQ(bare->signals.size(), observed->signals.size());
  for (std::size_t s = 0; s < bare->signals.size(); ++s) {
    EXPECT_EQ(bare->signals[s].first, observed->signals[s].first);
    same(bare->signals[s].first.c_str(), bare->signals[s].second, observed->signals[s].second);
  }

  std::printf("[strategy] observer-absent vs observer-present nav.back() = %.17g vs %.17g\n",
              bare->nav.back(), observed->nav.back());
}

TEST(Backtest, FixedBookRejectsStepObserver) {
  const ObserverCorpus corpus = make_observer_corpus("observer-fixed-book");
  auto clock = Clock::from_manifest(make_manifest(corpus.date_paths));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // One held call that never crosses expiry inside the clock, so the fixed-book
  // run is a genuinely valid run and the rejection below cannot be some other
  // failure wearing the observer's name.
  const auto make_book = []() {
    Lot lot;
    lot.id = 1;
    lot.contract = OptionContract{kUid, 100.0, (60.0 * static_cast<double>(kDayNs)) / kNsPerYear,
                                  Side::Call};
    lot.qty = 1.0;
    lot.multiplier = 100.0;
    lot.expiry_ts_ns = kBaseNow + 60 * kDayNs;
    PortfolioState book;
    book.lots.push_back(lot);
    return book;
  };

  // Control: without an observer this exact call succeeds. Without this the
  // rejection assertion below could pass against a run that failed for any reason.
  auto control = run_backtest(*clock, make_book(), RunConfig{});
  ASSERT_TRUE(control.has_value()) << control.error().to_string();
  ASSERT_EQ(control->size(), corpus.day_off.size());

  RunConfig cfg;
  cfg.step_observer = [](const StepEvent &) { return atx::core::Ok(); };
  auto res = run_backtest(*clock, make_book(), cfg);

  // Fail closed. A silently dropped observer would mean a silently dropped
  // divergence capture — precisely the Wave B failure class.
  ASSERT_FALSE(res.has_value()) << "the fixed-book overload has no on_step to observe";
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(res.error().message().find("step_observer"), std::string::npos)
      << "message was: " << res.error().message();

  std::printf("[backtest] fixed-book + step_observer -> %s\n", res.error().to_string().c_str());
}

// ── Session-snapped synthetic expiry (TenorSpec::snap_to_sessions) ──────────
// The synthetic expiry `valuation + round(T * kNsPerYear)` can land on a day the
// run's clock never observes (a weekend/holiday), which makes a held-to-expiry
// cohort unsettleable. With `snap_to_sessions` the canonical expiry is pulled
// back onto the greatest session at or before the raw expiry, and T is recomputed
// from the snapped anchor so the contract key and the greeks agree.

namespace {

// A 5-on/2-off weekday-shaped session grid: `n_days` calendar days from `start`,
// keeping day d only when (d % 7) < 5.
[[nodiscard]] std::vector<std::int64_t> weekday_sessions(std::int64_t start, int n_days) {
  std::vector<std::int64_t> out;
  for (int d = 0; d < n_days; ++d) {
    if (d % 7 < 5) {
      out.push_back(start + static_cast<std::int64_t>(d) * kDayNs);
    }
  }
  return out;
}

// The raw (unsnapped) expiry offset canonicalize_tenor computes for `T`.
[[nodiscard]] std::int64_t raw_tenor_offset(double T) {
  return static_cast<std::int64_t>(std::round(T * kNsPerYear));
}

// A one-leg spec (AbsStrike call at `T`) carrying `sessions` as the run's grid.
[[nodiscard]] StrategySpec snap_spec(double T, bool snap, std::vector<std::int64_t> sessions) {
  StrategySpec spec;
  LegSpec leg = fixed_call(kUid, T);
  leg.tenor.snap_to_sessions = snap;
  spec.legs.push_back(std::move(leg));
  spec.session_ts = std::move(sessions);
  return spec;
}

} // namespace

TEST(TenorSnap, SnapsToGreatestSessionAtOrBeforeRawExpiry) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "tenor-snap-gap");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  // 33 calendar days lands in a weekend gap (33 % 7 == 5); the greatest session
  // at or before it is day 32.
  constexpr double kT = 33.0 / 365.25;
  ASSERT_EQ(raw_tenor_offset(kT), 33 * kDayNs) << "fixture assumption: raw expiry is day 33";

  const auto sized = resolve_spec(*snapshot, snap_spec(kT, /*snap=*/true,
                                                       weekday_sessions(kBaseNow, 60)));
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 1u);
  EXPECT_EQ(sized->front().leg.expiry_ts_ns, kBaseNow + 32 * kDayNs);
  EXPECT_EQ(sized->front().leg.T, static_cast<double>(32 * kDayNs) / kNsPerYear);
}

TEST(TenorSnap, ExactSessionHitIsUnchanged) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "tenor-snap-exact");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  // 32 calendar days IS a session (32 % 7 == 4): snapping is the identity.
  constexpr double kT = 32.0 / 365.25;
  const std::int64_t raw = raw_tenor_offset(kT);
  ASSERT_EQ(raw, 32 * kDayNs) << "fixture assumption: raw expiry is day 32";

  const auto sized = resolve_spec(*snapshot, snap_spec(kT, /*snap=*/true,
                                                       weekday_sessions(kBaseNow, 60)));
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 1u);
  EXPECT_EQ(sized->front().leg.expiry_ts_ns, kBaseNow + raw);
  EXPECT_EQ(sized->front().leg.T, static_cast<double>(raw) / kNsPerYear);
}

TEST(TenorSnap, BeyondCalendarStaysUnsnapped) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "tenor-snap-beyond");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  // The grid ends at day 18; a 90-day tenor out-lives the corpus, so the expiry
  // stays raw (that cohort is liquidation-marked at run end, never settled).
  const std::vector<std::int64_t> sessions = weekday_sessions(kBaseNow, 21);
  ASSERT_EQ(sessions.back(), kBaseNow + 18 * kDayNs);
  constexpr double kT = 90.0 / 365.25;
  const std::int64_t raw = raw_tenor_offset(kT);

  const auto sized = resolve_spec(*snapshot, snap_spec(kT, /*snap=*/true, sessions));
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 1u);
  EXPECT_EQ(sized->front().leg.expiry_ts_ns, kBaseNow + raw);
  EXPECT_EQ(sized->front().leg.T, static_cast<double>(raw) / kNsPerYear);
}

TEST(TenorSnap, SnappedAtOrBeforeValuationIsInvalidArgument) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "tenor-snap-underflow");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  // Raw expiry is day 1, inside the grid's span, but the only session at or
  // before it is the valuation itself -> there is no future expiry to book.
  const std::vector<std::int64_t> sessions = {kBaseNow, kBaseNow + 3 * kDayNs};
  const auto sized = resolve_spec(*snapshot, snap_spec(1.0 / 365.25, /*snap=*/true, sessions));
  ASSERT_FALSE(sized.has_value());
  EXPECT_EQ(sized.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(sized.error().message().find("snapped expiry is not after valuation"),
            std::string::npos)
      << "message was: " << sized.error().message();
}

TEST(TenorSnap, SnapWithoutSessionsIsInvalidArgument) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "tenor-snap-no-sessions");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  // Never a silent no-op: an unsatisfiable snap request is a configuration error.
  const auto sized = resolve_spec(*snapshot, snap_spec(33.0 / 365.25, /*snap=*/true, {}));
  ASSERT_FALSE(sized.has_value());
  EXPECT_EQ(sized.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(sized.error().message().find("session_ts"), std::string::npos)
      << "message was: " << sized.error().message();
}

TEST(TenorSnap, UnsortedSessionsIsInvalidArgument) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "tenor-snap-unsorted");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  constexpr double kT = 33.0 / 365.25;
  std::vector<std::int64_t> sessions = weekday_sessions(kBaseNow, 60);
  // The RIGHT timestamps in the WRONG order. The binary search would still
  // return an anchor, silently — a wrong expiry, a wrong T and therefore a wrong
  // strike, with no error anywhere. That is the one silent-wrong-answer hole in
  // a fail-closed calendar feature, so it must be rejected up front.
  std::swap(sessions[3], sessions[9]);

  const auto sized = resolve_spec(*snapshot, snap_spec(kT, /*snap=*/true, sessions));
  ASSERT_FALSE(sized.has_value()) << "an unsorted grid must never resolve";
  EXPECT_EQ(sized.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(sized.error().message().find("session_ts"), std::string::npos)
      << "message was: " << sized.error().message();

  // A leg that never reads the grid is not this guard's business: the same
  // unsorted vector resolves normally when nothing snaps.
  const auto not_snapping = resolve_spec(*snapshot, snap_spec(kT, /*snap=*/false, sessions));
  ASSERT_TRUE(not_snapping.has_value()) << not_snapping.error().to_string();

  // Control: the guard is about ORDER, not contents — sorted, the same grid
  // resolves to the expected snapped anchor.
  std::sort(sessions.begin(), sessions.end());
  const auto ok = resolve_spec(*snapshot, snap_spec(kT, /*snap=*/true, sessions));
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  ASSERT_EQ(ok->size(), 1u);
  EXPECT_EQ(ok->front().leg.expiry_ts_ns, kBaseNow + 32 * kDayNs);
}

TEST(TenorSnap, DefaultOffIsBitIdentical) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snapshot = snapshot_of({{"SPY", &surface}}, "tenor-snap-default-off");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  // snap_to_sessions defaults false: a populated session grid must not move the
  // expiry off the hand-computed raw offset (day 33, a non-session).
  constexpr double kT = 33.0 / 365.25;
  const std::int64_t raw = raw_tenor_offset(kT);
  ASSERT_FALSE(TenorSpec{}.snap_to_sessions) << "snapping must be opt-in";

  const auto sized =
      resolve_spec(*snapshot, snap_spec(kT, /*snap=*/false, weekday_sessions(kBaseNow, 60)));
  ASSERT_TRUE(sized.has_value()) << sized.error().to_string();
  ASSERT_EQ(sized->size(), 1u);
  EXPECT_EQ(sized->front().leg.expiry_ts_ns, kBaseNow + raw);
  EXPECT_EQ(sized->front().leg.T, static_cast<double>(raw) / kNsPerYear);
}

// ── Declarative swap-lane DSL: grammar + validation gates ───────────────────
//
// FixedExpiryRestrike + swap_legs validation fires on the FIRST on_step (the
// Status channel a constructor does not have), before any resolution work — so
// these gates drive a real strategy against a real snapshot and assert the
// exact refusal.

namespace {

[[nodiscard]] StrategySpec restrike_spec(std::vector<std::int64_t> sessions) {
  StrategySpec spec;
  spec.name = "restrike";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::FixedContracts, 100.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::FixedExpiryRestrike;
  spec.session_ts = std::move(sessions);
  return spec;
}

[[nodiscard]] SwapLegSpec var_swap_leg() {
  SwapLegSpec leg;
  leg.uid = kUid;
  leg.kind = DerivKind::VarSwap;
  leg.size.kind = SwapSizeSpec::Kind::MatchGroupVega;
  return leg;
}

// Drive one on_step and hand back its Status; the book and watermark are
// scratch (a validation refusal must not touch either, and these tests only
// read the code).
[[nodiscard]] Status first_step_status(StrategySpec spec, const MarketSnapshot &snap) {
  DeclarativeStrategy strat{std::move(spec)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  return strat.on_step(snap, 0, book, next_lot_id, PriceOptions{});
}

} // namespace

TEST(SelectFixedCycleExpiry, CeilSnapsFallsBackToLastAndExhausts) {
  const std::int64_t s[] = {100, 200, 300};
  EXPECT_EQ(select_fixed_cycle_expiry(s, 90, 100), 200);  // ceil: anchor 190 -> 200
  EXPECT_EQ(select_fixed_cycle_expiry(s, 90, 250), 300);  // anchor 340 past end -> last
  EXPECT_EQ(select_fixed_cycle_expiry(s, 100, 100), 200); // exact anchor hit -> itself
  EXPECT_EQ(select_fixed_cycle_expiry(s, 300, 100), 0);   // nothing after base
  EXPECT_EQ(select_fixed_cycle_expiry(s, 500, 100), 0);   // base past the grid
}

TEST(StrategyRestrikeValidation, RejectsMismatchedLegTenors) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snap = snapshot_of({{"SPY", &surface}}, "restrike-val-tenor");
  ASSERT_TRUE(snap.has_value());

  StrategySpec spec = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  LegSpec second = spec.legs.front();
  second.tenor.target_T = 0.50; // the cycle has ONE tenor; two is a config error
  spec.legs.push_back(second);
  const Status st = first_step_status(std::move(spec), *snap);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(StrategyRestrikeValidation, RejectsLegSnapFlagsAndEmptyGrid) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snap = snapshot_of({{"SPY", &surface}}, "restrike-val-snap");
  ASSERT_TRUE(snap.has_value());

  // The lifecycle owns expiry snapping in this mode; a leg asking for its own
  // snap is rejected, never silently ignored.
  StrategySpec with_snap = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  with_snap.legs.front().tenor.snap_to_sessions = true;
  const Status snap_st = first_step_status(std::move(with_snap), *snap);
  ASSERT_FALSE(snap_st.has_value());
  EXPECT_EQ(snap_st.error().code(), atx::core::ErrorCode::InvalidArgument);

  // No session grid, no cycle expiry to fix.
  const Status grid_st = first_step_status(restrike_spec({}), *snap);
  ASSERT_FALSE(grid_st.has_value());
  EXPECT_EQ(grid_st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(StrategyRestrikeValidation, RejectsSwapLegsOutsideRestrikeMode) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snap = snapshot_of({{"SPY", &surface}}, "restrike-val-mode");
  ASSERT_TRUE(snap.has_value());

  // A swap lot can never be erased (the lane is held to expiry), so only the
  // cycle lifecycle can carry one; anything else is an honest NotImplemented.
  StrategySpec spec = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.swap_legs.push_back(var_swap_leg());
  const Status st = first_step_status(std::move(spec), *snap);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::NotImplemented);
}

TEST(StrategyRestrikeValidation, RejectsCappedKindWithoutCap) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snap = snapshot_of({{"SPY", &surface}}, "restrike-val-cap");
  ASSERT_TRUE(snap.has_value());

  StrategySpec spec = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  SwapLegSpec leg = var_swap_leg();
  leg.kind = DerivKind::CappedVarSwap;
  leg.cap_dec = 0.0; // a capped kind with no cap is a config error, not a 0 cap
  spec.swap_legs.push_back(leg);
  const Status capped_st = first_step_status(std::move(spec), *snap);
  ASSERT_FALSE(capped_st.has_value());
  EXPECT_EQ(capped_st.error().code(), atx::core::ErrorCode::InvalidArgument);

  // ... and the mirror image: a cap on an UNCAPPED kind is equally meaningless.
  StrategySpec mirror = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  SwapLegSpec uncapped = var_swap_leg();
  uncapped.cap_dec = 0.5;
  mirror.swap_legs.push_back(uncapped);
  const Status uncapped_st = first_step_status(std::move(mirror), *snap);
  ASSERT_FALSE(uncapped_st.has_value());
  EXPECT_EQ(uncapped_st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(StrategyRestrikeValidation, RejectsUnknownMatchGroup) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snap = snapshot_of({{"SPY", &surface}}, "restrike-val-group");
  ASSERT_TRUE(snap.has_value());

  StrategySpec spec = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  SwapLegSpec leg = var_swap_leg();
  leg.size.group = "no-such-group"; // no option leg carries this tag
  spec.swap_legs.push_back(leg);
  const Status st = first_step_status(std::move(spec), *snap);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(StrategyRestrikeValidation, RejectsWeightSizingAndCrossLegConstraints) {
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  auto snap = snapshot_of({{"SPY", &surface}}, "restrike-val-weight");
  ASSERT_TRUE(snap.has_value());

  // Weight sizing is meaningful only under a cross-leg constraint, and the
  // constraint machinery is not part of the restrike mode's v1 scope — both
  // are refused rather than silently resolved to something else.
  StrategySpec weight = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  weight.legs.front().size.kind = SizeSpec::Kind::Weight;
  const Status weight_st = first_step_status(std::move(weight), *snap);
  ASSERT_FALSE(weight_st.has_value());
  EXPECT_EQ(weight_st.error().code(), atx::core::ErrorCode::InvalidArgument);

  StrategySpec constrained = restrike_spec({kBaseNow, kBaseNow + kDayNs});
  constrained.constraint = CrossLegConstraint{CrossLegConstraint::Kind::FlatVega, "a", "b"};
  const Status constraint_st = first_step_status(std::move(constrained), *snap);
  ASSERT_FALSE(constraint_st.has_value());
  EXPECT_EQ(constraint_st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── Declarative swap-lane DSL: the restrike option lane ─────────────────────
//
// These gates drive DeclarativeStrategy::on_step by hand over hand-built
// consecutive snapshots (30 calendar days apart, so a 0.25y cycle spans ~3
// sessions and every residual tenor stays inside the synthetic surface's
// fitted pillar range [0.05, 1.0]).

namespace {

constexpr std::int64_t kStep30Ns = 30LL * kDayNs;
constexpr std::uint32_t kRestrikeDarkUid = 4243;

// Eight 30-day sessions starting at kBaseNow.
[[nodiscard]] std::vector<std::int64_t> restrike_sessions() {
  std::vector<std::int64_t> s;
  for (int i = 0; i < 8; ++i) {
    s.push_back(kBaseNow + i * kStep30Ns);
  }
  return s;
}

// Session `day_index`'s snapshot: uid kUid at a drifting spot — or, when
// `dark`, a board written under a DIFFERENT symbol/uid so the strategy's name
// is simply absent that session.
[[nodiscard]] Result<MarketSnapshot> restrike_snap(int day_index, const char *tag,
                                                   bool dark = false) {
  const std::int64_t now = kBaseNow + day_index * kStep30Ns;
  const double spot = 100.0 * (1.0 + 0.03 * day_index);
  const PricedSurface s =
      make_surface(dark ? kRestrikeDarkUid : kUid, spot, spot, now);
  const fs::path dir = fresh_dir((std::string("restrike-") + tag).c_str());
  char date[16];
  std::snprintf(date, sizeof date, "2026-10-%02d", day_index + 1);
  const std::string path = write_archive(dir, date, {{dark ? "OTHER" : "SPY", &s}});
  return MarketSnapshot::load(path);
}

} // namespace

TEST(StrategyRestrike, FixesOneExpiryAndRestrikesDailyAtIt) {
  const std::vector<std::int64_t> sessions = restrike_sessions();
  auto snap0 = restrike_snap(0, "fix0");
  auto snap1 = restrike_snap(1, "fix1");
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());

  DeclarativeStrategy strat{restrike_spec(sessions)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;

  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  const std::int64_t tenor_ns =
      static_cast<std::int64_t>(std::round(0.25 * kNsPerYear));
  const std::int64_t expected_expiry =
      select_fixed_cycle_expiry(sessions, sessions.front(), tenor_ns);
  ASSERT_GT(expected_expiry, sessions.front());
  EXPECT_EQ(book.lots[0].id, 1u);
  EXPECT_EQ(book.lots[1].id, 2u);
  for (const Lot &lot : book.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, expected_expiry); // the cycle's ONE expiry, exact int64
    EXPECT_EQ(lot.cohort, 1u);                    // cohort counts CYCLES
    EXPECT_EQ(lot.qty, 100.0);
    EXPECT_GT(lot.entry_price, 0.0);
  }
  EXPECT_NE(book.lots[0].contract.K, book.lots[1].contract.K); // a strangle, not a straddle
  EXPECT_EQ(strat.entry_risk_seeds().size(), 2u);
  const std::array<double, 2> day0_K{book.lots[0].contract.K, book.lots[1].contract.K};

  ASSERT_TRUE(strat.on_step(*snap1, 1, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u); // restruck, never accumulated
  EXPECT_EQ(book.lots[0].id, 3u);  // fresh lots each restrike...
  EXPECT_EQ(book.lots[1].id, 4u);
  for (const Lot &lot : book.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, expected_expiry); // ...at the SAME fixed expiry
    EXPECT_EQ(lot.cohort, 1u);                    // still cycle 1
  }
  EXPECT_NE(book.lots[0].contract.K, day0_K[0]); // strikes moved with the spot
  EXPECT_NE(book.lots[1].contract.K, day0_K[1]);
  EXPECT_EQ(strat.skipped_restrikes(), 0u);
  EXPECT_EQ(strat.unopened_entry_steps(), 0u);
}

TEST(StrategyRestrike, KeepsLiveStrikesWhenTheSurfaceCannotServeTheDelta) {
  const std::vector<std::int64_t> sessions = restrike_sessions();
  auto snap0 = restrike_snap(0, "keep0");
  auto snap1 = restrike_snap(1, "keep1", /*dark=*/true);
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());

  DeclarativeStrategy strat{restrike_spec(sessions)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  const PortfolioState held = book;

  // The board goes dark mid-cycle: the live strikes are KEPT — no fabricated
  // strike, no flattening for a data gap — and the hole is on the record.
  ASSERT_TRUE(strat.on_step(*snap1, 1, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  EXPECT_EQ(book.lots[0], held.lots[0]);
  EXPECT_EQ(book.lots[1], held.lots[1]);
  EXPECT_EQ(strat.skipped_restrikes(), 1u);
  EXPECT_EQ(strat.unopened_entry_steps(), 0u);
}

TEST(StrategyRestrike, CountsUnopenedStepsWhenNothingWasHeld) {
  const std::vector<std::int64_t> sessions = restrike_sessions();
  auto snap0 = restrike_snap(0, "unopened0", /*dark=*/true);
  auto snap1 = restrike_snap(1, "unopened1");
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());

  DeclarativeStrategy strat{restrike_spec(sessions)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;

  // Inception on a dark board: nothing to keep, so this is an UNOPENED step —
  // a different fact about the position than a kept restrike, counted apart.
  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());
  EXPECT_TRUE(book.lots.empty());
  EXPECT_EQ(strat.unopened_entry_steps(), 1u);
  EXPECT_EQ(strat.skipped_restrikes(), 0u);

  // The board returns: the cycle was fixed off the calendar regardless, and
  // the strangle opens at THIS session's strikes.
  ASSERT_TRUE(strat.on_step(*snap1, 1, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  EXPECT_EQ(strat.unopened_entry_steps(), 1u);
}

TEST(StrategyRestrike, EveryNDaysHoldsBetweenRestrikeTicks) {
  const std::vector<std::int64_t> sessions = restrike_sessions();
  auto snap0 = restrike_snap(0, "cad0");
  auto snap1 = restrike_snap(1, "cad1");
  auto snap2 = restrike_snap(2, "cad2");
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());
  ASSERT_TRUE(snap2.has_value());

  StrategySpec spec = restrike_spec(sessions);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.entry_every_n = 2;
  DeclarativeStrategy strat{std::move(spec)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;

  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  const std::uint64_t day0_ids[2] = {book.lots[0].id, book.lots[1].id};

  // Step 1 is off-tick: the book is HELD, not restruck — and holding is not a
  // skipped restrike (nothing failed; the cadence simply says hold).
  ASSERT_TRUE(strat.on_step(*snap1, 1, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  EXPECT_EQ(book.lots[0].id, day0_ids[0]);
  EXPECT_EQ(book.lots[1].id, day0_ids[1]);
  EXPECT_EQ(strat.skipped_restrikes(), 0u);

  // Step 2 is a tick: fresh strikes, fresh ids, same cycle expiry.
  ASSERT_TRUE(strat.on_step(*snap2, 2, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  EXPECT_NE(book.lots[0].id, day0_ids[0]);
  EXPECT_EQ(book.lots[0].cohort, 1u);
}

// ── Declarative swap-lane DSL: the swap lane + comparison signals ───────────

namespace {

[[nodiscard]] double signal_of(const std::vector<std::pair<std::string, double>> &signals,
                               const char *name) {
  for (const auto &[key, value] : signals) {
    if (key == name) {
      return value;
    }
  }
  ADD_FAILURE() << "signal '" << name << "' is missing";
  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace

TEST(StrategyRestrikeSwap, OpensOneFairStruckEqualVegaSwapPerCycle) {
  const std::vector<std::int64_t> sessions = restrike_sessions();
  auto snap0 = restrike_snap(0, "swap0");
  auto snap1 = restrike_snap(1, "swap1");
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());

  StrategySpec spec = restrike_spec(sessions);
  SwapLegSpec swap = var_swap_leg(); // MatchGroupVega, empty group = ALL option legs
  spec.swap_legs.push_back(swap);
  DeclarativeStrategy strat{std::move(spec)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;

  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u);
  ASSERT_EQ(book.swap_lots.size(), 1u);
  const SwapLot &lot = book.swap_lots.front();
  EXPECT_EQ(lot.id, 3u); // after the two wings: one shared id watermark
  EXPECT_EQ(lot.uid, kUid);
  EXPECT_GT(lot.strike_dec, 0.0);
  EXPECT_EQ(lot.expiry_ts_ns, book.lots.front().expiry_ts_ns); // the cycle's expiry
  EXPECT_GT(lot.n_obs_total, 0u);

  // EQUAL VEGA, independently: qty x the swap's own entry vega reproduces the
  // option book's entry dollar vega (per-share vega x qty x multiplier).
  const SurfaceRef surface = snap0->find(kUid);
  ASSERT_NE(surface, nullptr);
  RealizedVarianceSpec staged{};
  staged.annualization = lot.annualization;
  staged.n_obs_total = lot.n_obs_total;
  const Result<DerivGreeks> g = detail::deriv_greeks_on_ref(
      surface, swap_contract_for_lot(lot, lot.start_ts_ns, staged), DerivConfig{},
      DerivGreekBumps{});
  ASSERT_TRUE(g.has_value());
  const double options_vega = signal_of(strat.signals(*snap0), "options_vega");
  ASSERT_TRUE(std::isfinite(options_vega));
  EXPECT_GT(options_vega, 0.0);
  EXPECT_NEAR(lot.qty * g->vega, options_vega, 1e-6 * options_vega);

  // A restrike step inside the cycle opens NO second swap: the leg is
  // per-CYCLE, and the engine's lane is append-only and held to expiry.
  ASSERT_TRUE(strat.on_step(*snap1, 1, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.swap_lots.size(), 1u);
  EXPECT_EQ(book.swap_lots.front().id, 3u);
  EXPECT_EQ(strat.skipped_swap_cycles(), 0u);
}

TEST(StrategyRestrikeSwap, SignalsCarryTheEightColumnsWithNaNDiscipline) {
  const std::vector<std::int64_t> sessions = restrike_sessions();
  auto snap0 = restrike_snap(0, "sig0");
  auto snap1 = restrike_snap(1, "sig1");
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());

  StrategySpec spec = restrike_spec(sessions);
  spec.swap_legs.push_back(var_swap_leg());
  DeclarativeStrategy strat{std::move(spec)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());

  // As-of the stepped snapshot: all eight columns, the greeks + options_vega
  // finite (the probe adopted the lot this step; a mid-flight accrual is a
  // valid contract state), the counters genuinely 0.0.
  const auto signals = strat.signals(*snap0);
  ASSERT_EQ(signals.size(), 8u);
  EXPECT_TRUE(std::isfinite(signal_of(signals, "swap_delta")));
  EXPECT_TRUE(std::isfinite(signal_of(signals, "swap_gamma")));
  EXPECT_TRUE(std::isfinite(signal_of(signals, "swap_vega")));
  EXPECT_TRUE(std::isfinite(signal_of(signals, "swap_rho")));
  EXPECT_TRUE(std::isfinite(signal_of(signals, "options_vega")));
  EXPECT_EQ(signal_of(signals, "skipped_restrikes"), 0.0);
  EXPECT_EQ(signal_of(signals, "skipped_swaps"), 0.0);

  // Handed any OTHER snapshot the cached state measures nothing: NaN, never a
  // confident number against someone else's market. Counters stay counters.
  const auto wrong = strat.signals(*snap1);
  ASSERT_EQ(wrong.size(), 8u);
  EXPECT_TRUE(std::isnan(signal_of(wrong, "swap_vega")));
  EXPECT_TRUE(std::isnan(signal_of(wrong, "options_vega")));
  EXPECT_EQ(signal_of(wrong, "skipped_swaps"), 0.0);
}

TEST(StrategyRestrikeSwap, OneLeggedCycleCountsSkippedSwaps) {
  // A grid of TWO sessions and a 0.25y tenor: the expiry falls back to the
  // last session, whose fixing window holds a single session — too short to
  // observe one return. The cycle runs options-only, and says so.
  auto snap0 = restrike_snap(0, "oneleg0");
  ASSERT_TRUE(snap0.has_value());
  const std::vector<std::int64_t> sessions{kBaseNow, kBaseNow + kStep30Ns};

  StrategySpec spec = restrike_spec(sessions);
  spec.swap_legs.push_back(var_swap_leg());
  DeclarativeStrategy strat{std::move(spec)};
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());
  ASSERT_EQ(book.lots.size(), 2u); // the options leg still runs
  EXPECT_TRUE(book.swap_lots.empty());
  EXPECT_EQ(strat.skipped_swap_cycles(), 1u);
  EXPECT_EQ(signal_of(strat.signals(*snap0), "skipped_swaps"), 1.0);
}

TEST(StrategyRestrikeSwap, EmptySwapLegsEmitsNoSignals) {
  const std::vector<std::int64_t> sessions = restrike_sessions();
  auto snap0 = restrike_snap(0, "nosig0");
  ASSERT_TRUE(snap0.has_value());

  DeclarativeStrategy strat{restrike_spec(sessions)}; // no swap legs
  PortfolioState book;
  std::uint64_t next_lot_id = 1;
  ASSERT_TRUE(strat.on_step(*snap0, 0, book, next_lot_id, PriceOptions{}).has_value());
  // No swap lane, no signal columns — existing specs keep their empty default.
  EXPECT_TRUE(strat.signals(*snap0).empty());
}

// ── Ported from the retired strangle_varswap suite ──────────────────────────
//
// The two gates whose coverage lived only in the bespoke strategy's tests:
// the ENGINE-driven cycle roll (settle at the fixed expiry, refix on that same
// step, exhaust the grid) with an independent delta oracle on the resolved
// strikes, and the swap greek signals matched against an independently
// hand-staged accrual + deriv_greeks_on_ref — never read off the strategy.

namespace {

// `n` DAILY sessions (make_corpus's grid) starting at kBaseNow.
[[nodiscard]] std::vector<std::int64_t> daily_sessions(int n) {
  std::vector<std::int64_t> s;
  for (int d = 0; d < n; ++d) {
    s.push_back(kBaseNow + static_cast<std::int64_t>(d) * kDayNs);
  }
  return s;
}

// A 5-session, 30-day-step corpus on an EXPLICIT drifting spot path (the swap
// oracle hand-computes the engine's accrual off these very numbers).
constexpr double kOracleSpots[5] = {100.0, 106.0, 96.0, 103.0, 111.0};

// Task A1: the 30-CALENDAR-day step above is ~21 NYSE weekday sessions,
// coarser than a swap lot's implicitly-daily fixing schedule, so
// SignalsMatchIndependentSwapGreeksOracle below opts into
// `SwapFixingCadence::AcceptClockAsSchedule` so the PnL/settlement accrual
// survives the gap instead of refusing the run (see that test's own comment
// for why the oracle's hand-computed `n_obs_done` does NOT change).

struct OracleCorpus {
  CorpusManifest manifest;
  std::vector<std::int64_t> sessions;
  std::vector<std::string> archives; // per-session path, for the oracle reload
};

[[nodiscard]] OracleCorpus make_oracle_corpus(const char *tag) {
  const fs::path dir = fresh_dir(tag);
  OracleCorpus c;
  for (int d = 0; d < 5; ++d) {
    const std::int64_t now = kBaseNow + d * kStep30Ns;
    const PricedSurface s = make_surface(kUid, kOracleSpots[d], kOracleSpots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-09-%02d", d + 1);
    const std::string date = buf;
    const std::string path = write_archive(dir, date, {{"SPY", &s}});
    c.manifest.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "SPY";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    c.manifest.entries.push_back(std::move(e));
    c.sessions.push_back(now);
    c.archives.push_back(path);
  }
  return c;
}

[[nodiscard]] const std::vector<double> *signal_series(const BacktestResult &r, const char *name) {
  for (const auto &sig : r.signals) {
    if (sig.first == name) {
      return &sig.second;
    }
  }
  return nullptr;
}

} // namespace

TEST(StrategyRestrike, EngineRollsIntoNewCycleAtExpiryAndServesTheTargetDelta) {
  const Corpus corpus = make_corpus(7, "restrike-roll");
  const std::vector<std::int64_t> sessions = daily_sessions(7);
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // ~3.5-day cycles on a 7-day grid: cycle 1 fixes day 4 (ceil-snap), cycle 2
  // fixes on day 4 and, its anchor outrunning the grid, takes the LAST session.
  const auto spec_of = [&sessions]() {
    StrategySpec spec = restrike_spec(sessions);
    spec.legs.front().tenor.target_T = 3.5 / 365.25;
    return spec;
  };
  RunConfig rcfg; // Error policy: every session is bright, nothing to exclude

  // Mid-cycle prefix (day 3): cycle 1's book, restruck daily at ONE expiry.
  {
    DeclarativeStrategy strat{spec_of()};
    auto prefix = clock->between(corpus.manifest.dates[0], corpus.manifest.dates[3]);
    ASSERT_TRUE(prefix.has_value());
    auto run = run_backtest_incremental(*prefix, strat, rcfg, nullptr);
    ASSERT_TRUE(run.has_value()) << run.error().to_string();
    const PortfolioState &book = run->checkpoint.portfolio;
    ASSERT_EQ(book.lots.size(), 2u);
    for (const Lot &lot : book.lots) {
      EXPECT_EQ(lot.expiry_ts_ns, sessions[4]);
      EXPECT_EQ(lot.cohort, 1u);
    }
    // INDEPENDENT delta oracle: reload day 3's archive and ask the very
    // surface the engine priced against what these strikes' deltas are.
    auto snap = MarketSnapshot::load(corpus.manifest.entries[3].archive_path);
    ASSERT_TRUE(snap.has_value());
    const SurfaceRef s = snap->find(kUid);
    ASSERT_NE(s, nullptr);
    for (const Lot &lot : book.lots) {
      const Result<double> d = s->delta(lot.contract.K, lot.contract.T, lot.contract.side);
      ASSERT_TRUE(d.has_value());
      EXPECT_NEAR(std::fabs(*d), 0.40, 1e-3);
    }
  }

  // Second-cycle prefix (day 5): the engine settled cycle 1 on day 4, the
  // strategy refixed on that same step, and the new book carries a strictly
  // LATER fixed expiry.
  {
    DeclarativeStrategy strat{spec_of()};
    auto prefix = clock->between(corpus.manifest.dates[0], corpus.manifest.dates[5]);
    ASSERT_TRUE(prefix.has_value());
    auto run = run_backtest_incremental(*prefix, strat, rcfg, nullptr);
    ASSERT_TRUE(run.has_value()) << run.error().to_string();
    const PortfolioState &book = run->checkpoint.portfolio;
    ASSERT_EQ(book.lots.size(), 2u);
    for (const Lot &lot : book.lots) {
      EXPECT_EQ(lot.expiry_ts_ns, sessions[6]); // the grid ends before the anchor
      EXPECT_EQ(lot.cohort, 2u);
    }
  }

  // Full run: the tail cycle settles on the last session and the exhausted
  // grid opens nothing after it. 12 lots were issued over the run's 6 open
  // steps (2 wings x {day0..day3 in cycle 1, day4..day5 in cycle 2}).
  {
    DeclarativeStrategy strat{spec_of()};
    auto run = run_backtest_incremental(*clock, strat, rcfg, nullptr);
    ASSERT_TRUE(run.has_value()) << run.error().to_string();
    EXPECT_TRUE(run->checkpoint.portfolio.lots.empty());
    EXPECT_EQ(run->checkpoint.next_lot_id, 13u);
    EXPECT_EQ(strat.skipped_restrikes(), 0u);
    EXPECT_EQ(strat.unopened_entry_steps(), 0u);
  }
}

TEST(StrategyRestrikeSwap, SignalsMatchIndependentSwapGreeksOracle) {
  const OracleCorpus corpus = make_oracle_corpus("restrike-swaporacle");
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec = restrike_spec(corpus.sessions); // 0.25y -> expiry = session 4
  spec.swap_legs.push_back(var_swap_leg());
  RunConfig rcfg;
  // Task A1: the 30-calendar-day step is ~21 NYSE weekday sessions, coarser
  // than this lot's implicitly-daily schedule -- opt in (see the
  // `elapsed_weekdays` comment near `kOracleSpots` above).
  rcfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;

  // The lot's terms, off a prefix the engine itself validated (never off the
  // strategy): opened day 0, expiring session 4, 3 observable returns.
  SwapLot lot;
  {
    DeclarativeStrategy strat{spec};
    auto prefix = clock->between(corpus.manifest.dates[0], corpus.manifest.dates[2]);
    ASSERT_TRUE(prefix.has_value());
    auto run = run_backtest_incremental(*prefix, strat, rcfg, nullptr);
    ASSERT_TRUE(run.has_value()) << run.error().to_string();
    ASSERT_EQ(run->checkpoint.portfolio.swap_lots.size(), 1u);
    lot = run->checkpoint.portfolio.swap_lots.front();
    EXPECT_EQ(lot.expiry_ts_ns, corpus.sessions[4]);
    EXPECT_EQ(lot.n_obs_total, 3u);
  }

  // Full run, rows recorded every step; the oracle targets row 2 (day 2).
  DeclarativeStrategy strat{spec};
  auto run = run_backtest_incremental(*clock, strat, rcfg, nullptr);
  ASSERT_TRUE(run.has_value()) << run.error().to_string();
  const BacktestResult &rows = run->rows;
  ASSERT_GE(rows.size(), 3u);
  ASSERT_EQ(rows.date[2], corpus.manifest.dates[2]);
  const std::vector<double> *vega_col = signal_series(rows, "swap_vega");
  const std::vector<double> *delta_col = signal_series(rows, "swap_delta");
  ASSERT_NE(vega_col, nullptr);
  ASSERT_NE(delta_col, nullptr);
  ASSERT_EQ(vega_col->size(), rows.size());

  // The accrual as of day 2, hand-computed from the EXPLICIT spot path: the
  // engine seeds on day 1 (accrues nothing) and takes its first return on
  // day 2 — r = ln(S2/S1) — exactly one observation done. Task A1 note: the
  // swap_vega/swap_delta SIGNAL columns are computed off the strategy-level
  // greeks PROBE (swap_leg.hpp/.cpp), a SEPARATE realized-variance tracker
  // from `SwapAccrual`/`step_swap_lots` (backtest.cpp) that Task A1's cadence
  // guard does not touch -- the probe keeps its pre-existing "+1 fixing per
  // step" counting regardless of `RunConfig::swap_fixing_cadence`. Only the
  // PnL/settlement accrual needed the `AcceptClockAsSchedule` opt-in above
  // (to survive the ~21-weekday-session gap instead of refusing the run); the
  // oracle here still hand-computes n_obs_done = 1, unchanged, to match what
  // the probe actually reports.
  RealizedVarianceSpec rv{};
  rv.annualization = lot.annualization;
  rv.n_obs_total = lot.n_obs_total;
  const double r1 = std::log(kOracleSpots[2] / kOracleSpots[1]);
  rv.sum_sq_log_returns_done = r1 * r1;
  rv.n_obs_done = 1;
  rv.rv_done_dec = rv.annualization * rv.sum_sq_log_returns_done / 1.0;

  auto snap = MarketSnapshot::load(corpus.archives[2]);
  ASSERT_TRUE(snap.has_value());
  const SurfaceRef s = snap->find(kUid);
  ASSERT_NE(s, nullptr);
  const Result<DerivGreeks> g = detail::deriv_greeks_on_ref(
      s, swap_contract_for_lot(lot, corpus.sessions[2], rv), DerivConfig{}, DerivGreekBumps{});
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_NEAR((*vega_col)[2], lot.qty * g->vega,
              1e-9 * std::max(1.0, std::fabs(lot.qty * g->vega)));
  EXPECT_NEAR((*delta_col)[2], lot.qty * g->delta,
              1e-9 * std::max(1.0, std::fabs(lot.qty * g->delta)));
}
