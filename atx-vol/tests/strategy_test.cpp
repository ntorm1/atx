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
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod, AmericanGreeks
#include "atx/vol/backtest.hpp" // MarketSnapshot, Clock, run_backtest
#include "atx/vol/corpus.hpp"   // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/counters.hpp"
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

    const PricedSurface *surface = fast_snapshot->find(actual.leg.uid);
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
    const PricedSurface *surface = fast_snapshot->find(lot.contract.uid);
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

  const PricedSurface *resolved_surface = snapshot->find(kUid);
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
    EXPECT_EQ(sl.leg.model_price, expected->price);
    EXPECT_EQ(sl.leg.vega, expected->vega);
    EXPECT_EQ(sl.leg.theta, expected->theta);
    EXPECT_EQ(sl.leg.gamma, expected->gamma);
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

TEST(Strategy, RollAtHorizonNoTradeLeavesBookIdsAndLifecycleStateUnchanged) {
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

  const Status no_trade = strategy.on_step(*missing, 1, book, next_lot_id);
  ASSERT_TRUE(no_trade.has_value()) << no_trade.error().to_string();
  ASSERT_EQ(book.lots.size(), 1u);
  expect_lot_equal(book.lots.front(), original);
  EXPECT_EQ(next_lot_id, original_next_id);
  EXPECT_TRUE(strategy.entry_risk_seeds().empty());

  ASSERT_TRUE(strategy.on_step(*valid, 1, book, next_lot_id).has_value());
  ASSERT_EQ(book.lots.size(), 1u);
  EXPECT_EQ(book.lots.front().id, original_next_id);
  EXPECT_EQ(book.lots.front().cohort, original.cohort + 1u);
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
  const PricedSurface *resolved_surface = snapshot->find(kUid);
  ASSERT_NE(resolved_surface, nullptr);
  const auto expected = resolved_surface->greeks_analytic(sl.leg.K, sl.leg.T, sl.leg.side,
                                                          QueryExecution::ColdReference);
  ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
  EXPECT_EQ(sl.leg.model_price, expected->price);
  EXPECT_EQ(sl.leg.vega, expected->vega);
  EXPECT_EQ(sl.leg.theta, expected->theta);
  EXPECT_EQ(sl.leg.gamma, expected->gamma);
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
