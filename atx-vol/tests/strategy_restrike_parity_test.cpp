// atx-vol declarative swap-lane DSL — the DELETION LICENCE for the bespoke
// StrangleVsVarswapStrategy.
//
// Every gate here runs the old strategy and the equivalent declarative spec
// through the SAME engine on the SAME clock and asserts the tracks agree:
//
//   1. SyntheticCorpusTracksMatchTheOldStrategy — both lanes live (strangle +
//      equal-vega var swap) over a 7-session drifting corpus: per-row nav /
//      pnl_total / swap_pv / swap_pnl within 1e-9 relative, the final books
//      equivalent lot by lot (ids/cohorts/schedules exact; strikes/marks
//      within FP-solver noise), identical skip counters.
//   2. SyntheticOptionsOnlyKeepStrikesParity — a mid-cycle dark board under
//      ExcludeAndReport, options-only: the keep-strikes disposition and both
//      counters match, and so does the track.
//   3. Xom2026DbTrackMatchesTheOldStrategy — the real thing: the fixed
//      XOM 2026 surface db (fixture-gated; SKIPPED when the local db is
//      absent), full window, both legs.
//
// This file dies WITH the old class in the same commit — its assertions having
// been promoted into the ported behavioral tests — so nothing here may be
// load-bearing for anything else.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"         // Clock, run_backtest_incremental, RunConfig
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strangle_varswap.hpp" // StrangleVarswapConfig, StrangleVsVarswapStrategy
#include "atx/vol/strategy.hpp"         // DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveV2
#include "atx/vol/surface_db.hpp"       // SurfaceDb
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
constexpr std::int64_t kStepNs = 30LL * kDayNs;
constexpr std::uint32_t kUid = 11;
constexpr std::uint32_t kDarkUid = 4243;
constexpr std::size_t kSessions = 7;
constexpr double kDelta = 0.40;
constexpr double kTenorT = 0.25;
constexpr double kContracts = 100.0;
constexpr const char *kSymbol = "XOM";
constexpr const char *kFixtureDb = "C:/atx-data/surface-db/scratch-fitfix-2026";
constexpr double kSpots[kSessions] = {100.0, 106.0, 96.0, 103.0, 111.0, 94.0, 101.0};

// The strangle_varswap_test synthetic surface, verbatim.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, std::int64_t now_ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double term_forward = S * std::exp((kR - 0.02) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = term_forward;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, term_forward, 0.0, 0.02, 250, 7});
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

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::int64_t> sessions;
};

[[nodiscard]] Corpus make_corpus(const char *tag,
                                 std::size_t dark_at = static_cast<std::size_t>(-1)) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-restrike-parity-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  Corpus c;
  for (std::size_t d = 0; d < kSessions; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kStepNs;
    const bool dark = d == dark_at;
    const PricedSurface s = make_surface(dark ? kDarkUid : kUid, kSpots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(d) + 1);
    const std::string date = buf;
    const std::string path = (dir / (date + ".atxvsa")).string();
    const SurfaceArchiveItem item{dark ? "OTHER" : kSymbol, &s};
    const std::span<const SurfaceArchiveItem> items(&item, 1);
    const Status st = write_surface_archive_v2_file(path, items);
    EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
    c.manifest.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = kSymbol;
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    c.manifest.entries.push_back(std::move(e));
    c.sessions.push_back(now);
  }
  return c;
}

// The declarative spec equivalent to StrangleVarswapConfig{symbol, delta,
// tenor, contracts, sessions} + enable_swap_leg.
[[nodiscard]] StrategySpec equivalent_spec(const std::string &symbol, double delta, double tenor_T,
                                           double contracts, std::vector<std::int64_t> sessions,
                                           bool swap_leg) {
  StrategySpec spec;
  spec.name = "strangle-vs-varswap";
  LegSpec leg;
  leg.symbol = symbol;
  leg.tenor.target_T = tenor_T;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = {StrikeSelector::Kind::Delta, delta};
  leg.structure.put_leg = {StrikeSelector::Kind::Delta, delta};
  leg.size = {SizeSpec::Kind::FixedContracts, contracts, +1.0};
  spec.legs.push_back(std::move(leg));
  if (swap_leg) {
    SwapLegSpec swap;
    swap.symbol = symbol;
    swap.kind = DerivKind::VarSwap;
    swap.size.kind = SwapSizeSpec::Kind::MatchGroupVega; // empty group = ALL option legs
    spec.swap_legs.push_back(std::move(swap));
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::FixedExpiryRestrike;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  spec.session_ts = std::move(sessions);
  return spec;
}

// `reconcile` is off ONLY for the deliberately-dark synthetic corpus: the NAV
// audit liquidation-marks the whole book every row, which a dark session can
// never satisfy — the real driver never faces this because it DROPS dark
// sessions from the clock before running (probe_sessions).
[[nodiscard]] RunConfig parity_run_config(bool reconcile = true) {
  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  rc.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  rc.reconcile_nav = reconcile;
  return rc;
}

void expect_series_close(const std::vector<double> &a, const std::vector<double> &b,
                         const char *name, double rel = 1e-9) {
  ASSERT_EQ(a.size(), b.size()) << name;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double tol = rel * std::max(1.0, std::max(std::fabs(a[i]), std::fabs(b[i])));
    EXPECT_NEAR(a[i], b[i], tol) << name << " diverges at row " << i;
  }
}

void expect_tracks_match(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  expect_series_close(a.nav, b.nav, "nav");
  expect_series_close(a.pnl_total, b.pnl_total, "pnl_total");
  expect_series_close(a.swap_pv, b.swap_pv, "swap_pv");
  expect_series_close(a.swap_pnl, b.swap_pnl, "swap_pnl");
}

// Lot-by-lot book equivalence: identity, schedule and sizing EXACT; the
// solver-produced continuous coordinates (strike, tenor, entry mark) within
// FP-solver noise — the two paths re-derive the resolver's tenor through one
// extra anchor round-trip, which can move a bisection's last decision.
void expect_books_equivalent(const PortfolioState &a, const PortfolioState &b) {
  ASSERT_EQ(a.lots.size(), b.lots.size());
  for (std::size_t i = 0; i < a.lots.size(); ++i) {
    const Lot &x = a.lots[i];
    const Lot &y = b.lots[i];
    EXPECT_EQ(x.id, y.id);
    EXPECT_EQ(x.cohort, y.cohort);
    EXPECT_EQ(x.contract.uid, y.contract.uid);
    EXPECT_EQ(x.contract.side, y.contract.side);
    EXPECT_EQ(x.qty, y.qty);
    EXPECT_EQ(x.multiplier, y.multiplier);
    EXPECT_EQ(x.expiry_ts_ns, y.expiry_ts_ns);
    EXPECT_NEAR(x.contract.K, y.contract.K, 1e-9 * std::max(1.0, std::fabs(x.contract.K)));
    EXPECT_NEAR(x.contract.T, y.contract.T, 1e-12);
    EXPECT_NEAR(x.entry_price, y.entry_price, 1e-9 * std::max(1.0, std::fabs(x.entry_price)));
  }
  ASSERT_EQ(a.swap_lots.size(), b.swap_lots.size());
  for (std::size_t i = 0; i < a.swap_lots.size(); ++i) {
    const SwapLot &x = a.swap_lots[i];
    const SwapLot &y = b.swap_lots[i];
    EXPECT_EQ(x.id, y.id);
    EXPECT_EQ(x.uid, y.uid);
    EXPECT_EQ(x.kind, y.kind);
    EXPECT_EQ(x.notional, y.notional);
    EXPECT_EQ(x.start_ts_ns, y.start_ts_ns);
    EXPECT_EQ(x.expiry_ts_ns, y.expiry_ts_ns);
    EXPECT_EQ(x.n_obs_total, y.n_obs_total);
    EXPECT_EQ(x.annualization, y.annualization);
    EXPECT_NEAR(x.strike_dec, y.strike_dec, 1e-9 * std::max(1.0, std::fabs(x.strike_dec)));
    EXPECT_NEAR(x.qty, y.qty, 1e-9 * std::max(1.0, std::fabs(x.qty)));
  }
}

} // namespace

TEST(StrategyRestrikeParity, SyntheticCorpusTracksMatchTheOldStrategy) {
  const Corpus corpus = make_corpus("swap");
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrangleVarswapConfig cfg;
  cfg.symbol = kSymbol;
  cfg.target_abs_delta = kDelta;
  cfg.tenor_years = kTenorT;
  cfg.contracts = kContracts;
  cfg.session_ts = corpus.sessions;
  StrangleVsVarswapStrategy old_strat{cfg};

  DeclarativeStrategy new_strat{
      equivalent_spec(kSymbol, kDelta, kTenorT, kContracts, corpus.sessions, /*swap_leg=*/true)};

  auto old_run = run_backtest_incremental(*clock, old_strat, parity_run_config(), nullptr);
  ASSERT_TRUE(old_run.has_value()) << old_run.error().to_string();
  auto new_run = run_backtest_incremental(*clock, new_strat, parity_run_config(), nullptr);
  ASSERT_TRUE(new_run.has_value()) << new_run.error().to_string();

  // The old run carried both legs through at least two cycles — otherwise this
  // gate would vacuously bless an empty comparison. (The FINAL book is
  // legitimately empty: the tail cycle settles on the run's last session, so
  // life is proven on the rows and the id watermark, not the final book.)
  ASSERT_GT(old_run->checkpoint.next_lot_id, 5u); // > two wings + one swap issued
  double max_abs_swap_pv = 0.0;
  for (const double pv : old_run->rows.swap_pv) {
    max_abs_swap_pv = std::max(max_abs_swap_pv, std::fabs(pv));
  }
  ASSERT_GT(max_abs_swap_pv, 0.0) << "no swap ever carried a mark: vacuous comparison";

  expect_tracks_match(old_run->rows, new_run->rows);
  expect_books_equivalent(old_run->checkpoint.portfolio, new_run->checkpoint.portfolio);
  EXPECT_EQ(old_run->checkpoint.next_lot_id, new_run->checkpoint.next_lot_id);
  EXPECT_EQ(old_strat.skipped_restrikes(), new_strat.skipped_restrikes());
  EXPECT_EQ(old_strat.unopened_strangle_steps(), new_strat.unopened_entry_steps());
  EXPECT_EQ(old_strat.skipped_swap_cycles(), new_strat.skipped_swap_cycles());
}

TEST(StrategyRestrikeParity, SyntheticOptionsOnlyKeepStrikesParity) {
  // A dark board mid-cycle (session 3). Options-only on BOTH sides: a live
  // swap fails the whole run closed on a dark session by the engine's own
  // contract, so the keep-strikes disposition is an options-lane comparison.
  const Corpus corpus = make_corpus("dark", /*dark_at=*/3);
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrangleVarswapConfig cfg;
  cfg.symbol = kSymbol;
  cfg.target_abs_delta = kDelta;
  cfg.tenor_years = kTenorT;
  cfg.contracts = kContracts;
  cfg.session_ts = corpus.sessions;
  cfg.enable_swap_leg = false;
  StrangleVsVarswapStrategy old_strat{cfg};

  DeclarativeStrategy new_strat{
      equivalent_spec(kSymbol, kDelta, kTenorT, kContracts, corpus.sessions, /*swap_leg=*/false)};

  auto old_run =
      run_backtest_incremental(*clock, old_strat, parity_run_config(/*reconcile=*/false), nullptr);
  ASSERT_TRUE(old_run.has_value()) << old_run.error().to_string();
  auto new_run =
      run_backtest_incremental(*clock, new_strat, parity_run_config(/*reconcile=*/false), nullptr);
  ASSERT_TRUE(new_run.has_value()) << new_run.error().to_string();

  EXPECT_EQ(old_strat.skipped_restrikes(), 1u); // the gate must actually bite
  expect_tracks_match(old_run->rows, new_run->rows);
  expect_books_equivalent(old_run->checkpoint.portfolio, new_run->checkpoint.portfolio);
  EXPECT_EQ(old_strat.skipped_restrikes(), new_strat.skipped_restrikes());
  EXPECT_EQ(old_strat.unopened_strangle_steps(), new_strat.unopened_entry_steps());
}

TEST(StrategyRestrikeParity, Xom2026DbTrackMatchesTheOldStrategy) {
  if (!fs::exists(kFixtureDb)) {
    GTEST_SKIP() << "local fixture db absent: " << kFixtureDb;
  }
  auto db = SurfaceDb::open(kFixtureDb);
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto full = Clock::from_surface_db(*db);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();

  // The driver's probe, lifted minimally: keep the sessions whose partition
  // carries an XOM surface (a listed-but-absent FILE would be a broken db and
  // fails the test rather than the calendar).
  CorpusManifest live;
  std::vector<std::int64_t> sessions;
  for (const SnapshotRef &ref : full->refs()) {
    auto archive = SurfaceArchiveV2::open_mapped(ref.archive_path);
    ASSERT_TRUE(archive.has_value())
        << ref.date << ": " << archive.error().to_string() << " (inconsistent db)";
    auto surface = archive->map_symbol(kSymbol);
    if (!surface) {
      ASSERT_EQ(surface.error().code(), atx::core::ErrorCode::NotFound)
          << ref.date << ": " << surface.error().to_string();
      continue; // genuinely dark for this symbol
    }
    sessions.push_back(surface->pricing().now_ts_ns);
    live.dates.push_back(ref.date);
    CorpusEntry e;
    e.date = ref.date;
    e.symbol = "*";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = ref.archive_path;
    live.entries.push_back(std::move(e));
  }
  ASSERT_FALSE(live.dates.empty());
  ASSERT_TRUE(std::is_sorted(sessions.begin(), sessions.end()));
  auto clock = Clock::from_manifest(live);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrangleVarswapConfig cfg;
  cfg.symbol = kSymbol;
  cfg.target_abs_delta = kDelta;
  cfg.tenor_years = kTenorT;
  cfg.contracts = kContracts;
  cfg.session_ts = sessions;
  StrangleVsVarswapStrategy old_strat{cfg};

  DeclarativeStrategy new_strat{
      equivalent_spec(kSymbol, kDelta, kTenorT, kContracts, sessions, /*swap_leg=*/true)};

  auto old_run = run_backtest_incremental(*clock, old_strat, parity_run_config(), nullptr);
  ASSERT_TRUE(old_run.has_value()) << old_run.error().to_string();
  auto new_run = run_backtest_incremental(*clock, new_strat, parity_run_config(), nullptr);
  ASSERT_TRUE(new_run.has_value()) << new_run.error().to_string();

  ASSERT_GT(old_run->rows.size(), 100u); // the full 2026 window, not a stub
  expect_tracks_match(old_run->rows, new_run->rows);
  expect_books_equivalent(old_run->checkpoint.portfolio, new_run->checkpoint.portfolio);
  EXPECT_EQ(old_run->checkpoint.next_lot_id, new_run->checkpoint.next_lot_id);
  EXPECT_EQ(old_strat.skipped_restrikes(), new_strat.skipped_restrikes());
  EXPECT_EQ(old_strat.unopened_strangle_steps(), new_strat.unopened_entry_steps());
  EXPECT_EQ(old_strat.skipped_swap_cycles(), new_strat.skipped_swap_cycles());
}
