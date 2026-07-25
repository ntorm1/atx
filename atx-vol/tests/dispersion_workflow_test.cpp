// WS-C correctness-cluster regression suite for the SPY-dispersion workflow.
//
// Covers, one test per fix:
//   C1 — point-in-time universe: DispersionStrategy re-resolves its basket per
//        step, so a mid-backtest reconstitution changes the SERVED basket instead
//        of freezing day-1 membership for the whole run.
//   C3 — removals: each `effective_date` block is a FULL point-in-time snapshot,
//        so a name absent from a later block actually LEAVES the basket.
//   M2 — read_universe rejects duplicate (effective_date, symbol) keys and sorts
//        stably (deterministic weights).
//   M4 — the index symbol is a parameter (default "SPY"), not hardcoded.
//
// The C1 integration test builds real (fast, non-fitted) eSSVI boards, archives
// them, and drives DispersionStrategy::on_step — so it exercises the production
// symbol->uid resolve + vega-flat sizing path, not a mock.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp" // MarketSnapshot, PortfolioState, Lot, Clock
#include "atx/vol/corpus.hpp"   // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_run.hpp" // run_dispersion_surface_backtest (the seam)
#include "atx/vol/dispersion_workflow.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/strategy.hpp" // DispersionStrategy
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 0.04;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

// A universe row with as_of pinned to the effective date (the reader requires
// as_of <= effective_date).
[[nodiscard]] UniverseRow row(std::string effective_date, std::string symbol, double weight) {
  UniverseRow r;
  r.effective_date = effective_date;
  r.symbol = std::move(symbol);
  r.raw_weight = weight;
  r.source = "test";
  r.as_of = std::move(effective_date);
  return r;
}

[[nodiscard]] std::vector<std::string> names_of(const DispersionUniverse &universe) {
  std::vector<std::string> out;
  out.reserve(universe.names.size());
  for (const DispersionMember &m : universe.names) {
    out.push_back(m.symbol);
  }
  std::sort(out.begin(), out.end());
  return out;
}

[[nodiscard]] fs::path fresh_dir(const char *leaf) {
  const fs::path path = fs::temp_directory_path() / leaf;
  std::error_code error;
  fs::remove_all(path, error);
  fs::create_directories(path, error);
  return path;
}

[[nodiscard]] std::string write_universe_tsv(const fs::path &dir, const std::string &name,
                                             const std::string &body) {
  const fs::path path = dir / name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "effective_date\tsymbol\traw_weight\tsource\tas_of\n" << body;
  out.close();
  return path.string();
}

// A small, deterministic eSSVI board built directly (no fitting) — the fast
// surface pattern used by the listed-reconciliation suite.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double spot, std::int64_t now) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.05, 0.10, 0.20, 0.50}) {
    const double forward = spot * std::exp((kRate - 0.02) * term);
    EssviParams parameters{};
    parameters.theta = 0.04 + 0.01 * term;
    parameters.phi = 1.4;
    parameters.rho = -0.35;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = forward;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, forward, 0.0, 0.02, 100, 7});
  }
  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = now;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto result = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  return std::move(*result);
}

// Index (uid 1) + three candidate names (uid 2,3,4). BOTH dates carry all four
// boards, so a name leaving the basket is a MEMBERSHIP change, never a
// missing-surface drop.
[[nodiscard]] std::vector<PricedSurface> pit_surfaces(std::int64_t now, double shift) {
  std::vector<PricedSurface> out;
  out.push_back(make_surface(1u, 500.0 + shift, now));       // SPY
  out.push_back(make_surface(2u, 100.0 + 0.3 * shift, now)); // N0
  out.push_back(make_surface(3u, 200.0 - 0.2 * shift, now)); // N1
  out.push_back(make_surface(4u, 150.0 + 0.1 * shift, now)); // N2
  return out;
}

[[nodiscard]] std::string write_pit_archive(const fs::path &dir, const std::string &date,
                                            const std::vector<PricedSurface> &surfaces) {
  const std::vector<SurfaceArchiveItem> items = {
      {"SPY", &surfaces[0]}, {"N0", &surfaces[1]}, {"N1", &surfaces[2]}, {"N2", &surfaces[3]}};
  const std::string path = (dir / (date + ".atxvsa")).string();
  const Status status = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(status) << (status ? std::string{} : status.error().to_string());
  return path;
}

} // namespace

// ── C3: each effective_date block is a full PIT snapshot (removals work) ─────

TEST(DispersionWorkflow, UniverseAtTreatsEachBlockAsFullPitSnapshotSoNamesCanLeave) {
  const std::vector<UniverseRow> rows = {
      row("2026-01-02", "AAA", 0.5), row("2026-01-02", "BBB", 0.3),
      row("2026-01-02", "CCC", 0.2), // present in the first block only
      row("2026-06-01", "AAA", 0.6), row("2026-06-01", "BBB", 0.4),
  };

  // Inside the first block: all three constituents.
  auto early = universe_at(rows, "2026-03-15");
  ASSERT_TRUE(early) << early.error().to_string();
  EXPECT_EQ(names_of(*early), (std::vector<std::string>{"AAA", "BBB", "CCC"}));

  // On/after the reconstitution: CCC has LEFT the basket. Pre-C3 the cumulative
  // latest-row-per-symbol map kept CCC forever (the basket could only grow).
  auto late = universe_at(rows, "2026-07-01");
  ASSERT_TRUE(late) << late.error().to_string();
  EXPECT_EQ(names_of(*late), (std::vector<std::string>{"AAA", "BBB"}));

  // The later block's weights are the ones served.
  ASSERT_EQ(late->names.size(), 2u);
  for (const DispersionMember &m : late->names) {
    EXPECT_DOUBLE_EQ(m.weight, m.symbol == "AAA" ? 0.6 : 0.4);
  }

  // Exactly on the reconstitution date the new block is already effective.
  auto on_date = universe_at(rows, "2026-06-01");
  ASSERT_TRUE(on_date) << on_date.error().to_string();
  EXPECT_EQ(names_of(*on_date), (std::vector<std::string>{"AAA", "BBB"}));

  // Before any block is effective there is no basket.
  EXPECT_FALSE(universe_at(rows, "2025-12-31"));
}

// ── M4: the index symbol is a parameter, not a hardcoded "SPY" ───────────────

TEST(DispersionWorkflow, IndexSymbolIsParameterizedAndNeverAConstituent) {
  const std::vector<UniverseRow> rows = {
      row("2026-01-02", "AAA", 0.5),
      row("2026-01-02", "NDX", 0.3), // would be a constituent under the default index
      row("2026-01-02", "SPY", 0.2),
  };

  // Default index stays "SPY": SPY is the index leg, NDX is a constituent.
  auto spy = universe_at(rows, "2026-02-01");
  ASSERT_TRUE(spy) << spy.error().to_string();
  EXPECT_EQ(spy->index.symbol, "SPY");
  EXPECT_EQ(names_of(*spy), (std::vector<std::string>{"AAA", "NDX"}));

  // Overriding the index makes NDX the index leg and SPY an ordinary constituent.
  auto ndx = universe_at(rows, "2026-02-01", "NDX");
  ASSERT_TRUE(ndx) << ndx.error().to_string();
  EXPECT_EQ(ndx->index.symbol, "NDX");
  EXPECT_EQ(names_of(*ndx), (std::vector<std::string>{"AAA", "SPY"}));

  // all_symbols seeds with the configured index, not a hardcoded one.
  EXPECT_EQ(all_symbols(rows), (std::vector<std::string>{"AAA", "NDX", "SPY"}));
  const std::vector<std::string> with_qqq = all_symbols(rows, "QQQ");
  EXPECT_NE(std::find(with_qqq.begin(), with_qqq.end(), "QQQ"), with_qqq.end());
}

// ── M2: duplicate-key rejection + deterministic (stable) ordering ────────────

TEST(DispersionWorkflow, ReadUniverseRejectsDuplicateEffectiveDateSymbolKeys) {
  const fs::path dir = fresh_dir("atx-disp-universe-dup");

  // Same (effective_date, symbol) twice with DIFFERENT weights: previously the
  // unstable sort made the surviving weight nondeterministic. Now it is a hard
  // error rather than a silent last-writer-wins.
  const std::string dup = write_universe_tsv(dir, "dup.tsv",
                                             "2026-01-02\tAAA\t0.5\ttest\t2026-01-02\n"
                                             "2026-01-02\tAAA\t0.9\ttest\t2026-01-02\n"
                                             "2026-01-02\tBBB\t0.3\ttest\t2026-01-02\n");
  auto duplicated = read_universe(dup);
  EXPECT_FALSE(duplicated);
  if (!duplicated) {
    EXPECT_EQ(duplicated.error().code(), atx::core::ErrorCode::AlreadyExists);
  }

  // The same symbol in DIFFERENT blocks is legitimate (that is a reweight).
  const std::string ok = write_universe_tsv(dir, "ok.tsv",
                                            "2026-06-01\tAAA\t0.9\ttest\t2026-06-01\n"
                                            "2026-01-02\tBBB\t0.3\ttest\t2026-01-02\n"
                                            "2026-01-02\tAAA\t0.5\ttest\t2026-01-02\n");
  auto rows = read_universe(ok);
  ASSERT_TRUE(rows) << rows.error().to_string();
  ASSERT_EQ(rows->size(), 3u);
  // Deterministically ordered by (effective_date, symbol) regardless of file order.
  EXPECT_EQ((*rows)[0].effective_date, "2026-01-02");
  EXPECT_EQ((*rows)[0].symbol, "AAA");
  EXPECT_EQ((*rows)[1].symbol, "BBB");
  EXPECT_EQ((*rows)[2].effective_date, "2026-06-01");

  std::error_code error;
  fs::remove_all(dir, error);
}

// ── C1: UTC date basis + point-in-time resolver ─────────────────────────────

TEST(DispersionWorkflow, UtcDateFromNsIsCivilCalendarExact) {
  EXPECT_EQ(utc_date_from_ns(0), "1970-01-01");
  EXPECT_EQ(utc_date_from_ns(1'700'000'000'000'000'000LL), "2023-11-14");
  // Leap day and the instant before/after a UTC midnight boundary.
  EXPECT_EQ(utc_date_from_ns(1'709'164'800'000'000'000LL), "2024-02-29");
  EXPECT_EQ(utc_date_from_ns(1'709'164'800'000'000'000LL - 1), "2024-02-28");
}

TEST(DispersionWorkflow, PitResolverTracksReconstitutionAcrossTimestamps) {
  const std::int64_t ts_early = 1'700'000'000'000'000'000LL; // 2023-11-14
  const std::int64_t ts_late = ts_early + 30 * kDayNs;       // 2023-12-14
  const std::vector<UniverseRow> rows = {
      row(utc_date_from_ns(ts_early), "AAA", 0.5),
      row(utc_date_from_ns(ts_early), "BBB", 0.3),
      row(utc_date_from_ns(ts_early), "CCC", 0.2),
      row(utc_date_from_ns(ts_late), "AAA", 0.6),
      row(utc_date_from_ns(ts_late), "BBB", 0.4),
  };

  const auto resolve = make_pit_universe_resolver(rows, "SPY");
  auto early = resolve(ts_early);
  auto late = resolve(ts_late);
  ASSERT_TRUE(early) << early.error().to_string();
  ASSERT_TRUE(late) << late.error().to_string();
  EXPECT_EQ(names_of(*early), (std::vector<std::string>{"AAA", "BBB", "CCC"}));
  EXPECT_EQ(names_of(*late), (std::vector<std::string>{"AAA", "BBB"}));
  // Before the first block the resolver reports "no basket" rather than guessing.
  EXPECT_FALSE(resolve(ts_early - 30 * kDayNs));
}

// ── C1 (flagship): a mid-backtest reconstitution changes the SERVED basket ──

TEST(DispersionWorkflow, PitReconstitutionChangesServedBasket) {
  const fs::path dir = fresh_dir("atx-disp-pit-basket");
  const std::int64_t ts0 = 1'700'000'000'000'000'000LL;
  const std::int64_t ts1 = ts0 + 3 * kDayNs;
  const std::string d0 = utc_date_from_ns(ts0);
  const std::string d1 = utc_date_from_ns(ts1);
  ASSERT_LT(d0, d1);

  const std::vector<PricedSurface> day0 = pit_surfaces(ts0, 0.0);
  const std::vector<PricedSurface> day1 = pit_surfaces(ts1, 1.5);
  const std::string archive0 = write_pit_archive(dir, d0, day0);
  const std::string archive1 = write_pit_archive(dir, d1, day1);

  // N2 is in the day-0 block and ABSENT from the day-1 block => it is removed at
  // the reconstitution, even though its board is still archived on day 1.
  const std::vector<UniverseRow> schedule = {
      row(d0, "N0", 0.5), row(d0, "N1", 0.3), row(d0, "N2", 0.2),
      row(d1, "N0", 0.6), row(d1, "N1", 0.4),
  };

  auto snap0 = MarketSnapshot::load(archive0);
  auto snap1 = MarketSnapshot::load(archive1);
  ASSERT_TRUE(snap0) << snap0.error().to_string();
  ASSERT_TRUE(snap1) << snap1.error().to_string();

  DispersionConfig cfg;
  cfg.target_T = 0.10;
  cfg.target_vega = 1000.0;
  cfg.side = DispersionSide::ShortIndexLongNames;
  cfg.multiplier = 100.0;
  cfg.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 2};

  // Open a fresh cohort on `snap` through the production on_step path, with the
  // point-in-time resolver installed. Each call starts from an empty book so the
  // result is purely "what basket does this date serve".
  const auto open_basket = [&](const MarketSnapshot &snap) {
    auto seed = universe_at(schedule, d0, "SPY");
    EXPECT_TRUE(seed);
    DispersionStrategy strategy{*seed, cfg, LifecycleSpec{}, HedgeSpec{},
                                make_pit_universe_resolver(schedule, "SPY")};
    PortfolioState book;
    std::uint64_t next_id = 1;
    const Status status = strategy.on_step(snap, 0, book, next_id);
    EXPECT_TRUE(status) << (status ? std::string{} : status.error().to_string());
    return book.lots;
  };

  const std::vector<Lot> lots0 = open_basket(*snap0);
  const std::vector<Lot> lots1 = open_basket(*snap1);

  const auto n2 = snap1->uid_of("N2");
  ASSERT_TRUE(n2.has_value());
  const auto holds_n2 = [&](const std::vector<Lot> &lots) {
    return std::any_of(lots.begin(), lots.end(),
                       [&](const Lot &lot) { return lot.contract.uid == *n2; });
  };

  // Day 0 serves the 3-name basket including N2 ...
  EXPECT_TRUE(holds_n2(lots0));
  // ... and after the reconstitution N2 is genuinely gone. Pre-C1 the strategy
  // froze day-1 membership for the whole run, so this basket still held N2.
  EXPECT_FALSE(holds_n2(lots1));

  // index + 3 names, one straddle (call+put) each => 8 lots; then index + 2 => 6.
  EXPECT_EQ(lots0.size(), 8u);
  EXPECT_EQ(lots1.size(), 6u);

  std::error_code error;
  fs::remove_all(dir, error);
}

// ── Preserved invariant: the served basket stays vega-flat after a removal ───

TEST(DispersionWorkflow, PitBasketRemainsVegaFlatAfterReconstitution) {
  const fs::path dir = fresh_dir("atx-disp-pit-vega");
  const std::int64_t ts1 = 1'700'000'000'000'000'000LL + 3 * kDayNs;
  const std::string d0 = utc_date_from_ns(1'700'000'000'000'000'000LL);
  const std::string d1 = utc_date_from_ns(ts1);

  const std::vector<PricedSurface> day1 = pit_surfaces(ts1, 1.5);
  const std::string archive1 = write_pit_archive(dir, d1, day1);
  const std::vector<UniverseRow> schedule = {
      row(d0, "N0", 0.5), row(d0, "N1", 0.3), row(d0, "N2", 0.2),
      row(d1, "N0", 0.6), row(d1, "N1", 0.4),
  };

  auto snap1 = MarketSnapshot::load(archive1);
  ASSERT_TRUE(snap1) << snap1.error().to_string();

  DispersionConfig cfg;
  cfg.target_T = 0.10;
  cfg.target_vega = 1000.0;
  cfg.side = DispersionSide::ShortIndexLongNames;
  cfg.multiplier = 100.0;
  cfg.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 2};

  auto seed = universe_at(schedule, d0, "SPY");
  ASSERT_TRUE(seed) << seed.error().to_string();
  DispersionStrategy strategy{*seed, cfg, LifecycleSpec{}, HedgeSpec{},
                              make_pit_universe_resolver(schedule, "SPY")};
  PortfolioState book;
  std::uint64_t next_id = 1;
  ASSERT_TRUE(strategy.on_step(*snap1, 0, book, next_id));

  // The post-reconstitution book must still be the P4-1 vega-neutral sizing:
  // index straddle gross vega == basket gross vega (weights renormalized over the
  // SURVIVING two names). Read it off the authoritative builder.
  auto built = strategy.build_book(*snap1);
  ASSERT_TRUE(built) << built.error().to_string();
  ASSERT_EQ(built->name_legs.size(), 2u);

  const double index_vega =
      std::fabs(built->index_leg.straddle_vega * built->index_leg.straddle_qty);
  double basket_vega = 0.0;
  for (const DispersionLeg &leg : built->name_legs) {
    basket_vega += std::fabs(leg.straddle_vega * leg.straddle_qty);
  }
  ASSERT_GT(index_vega, 0.0);
  EXPECT_NEAR(basket_vega, index_vega, 1.0e-9 * index_vega);

  std::error_code error;
  fs::remove_all(dir, error);
}

// ── C1-ACTIVATE: the SEAM entry point, not just the strategy ─────────────────
//
// The tests above prove DispersionStrategy is PIT-CAPABLE when a resolver is
// handed to its constructor. They do NOT prove the shipping path uses one: WS-C
// left the resolver defaulted-empty and `dispersion_run_surface_backtest` still
// resolved the universe once and passed a frozen `DispersionUniverse`, so the
// flagship surface backtest was capable-but-inert. These two tests pin the
// activation itself at `run_dispersion_surface_backtest`'s schedule overload.

namespace {

// Hand-build an Ok-only manifest over (date, archive_path), one entry per
// (date, symbol) — the shape Clock::from_manifest consumes.
[[nodiscard]] CorpusManifest pit_manifest(
    const std::vector<std::pair<std::string, std::string>> &date_paths) {
  CorpusManifest manifest;
  for (const auto &[date, path] : date_paths) {
    manifest.dates.push_back(date);
    for (const char *symbol : {"SPY", "N0", "N1", "N2"}) {
      CorpusEntry entry;
      entry.date = date;
      entry.symbol = symbol;
      entry.status = CorpusFitStatus::Ok;
      entry.archive_path = path;
      manifest.entries.push_back(std::move(entry));
    }
  }
  return manifest;
}

struct PitFixture {
  fs::path dir;
  Clock clock;
  std::string d0;
  std::string d1;
  std::string d2;
};

// THREE sessions, deliberately. With only two, the reconstituted basket is opened
// on the final step and never held across a P&L interval, so NAV stays identical
// and only lot COMPOSITION diverges. The third session lets the post-
// reconstitution basket actually earn/lose, which is the economically meaningful
// consequence of activation.
[[nodiscard]] PitFixture make_pit_fixture(const char *leaf) {
  const fs::path dir = fresh_dir(leaf);
  const std::int64_t ts0 = 1'700'000'000'000'000'000LL;
  const std::int64_t ts1 = ts0 + 3 * kDayNs;
  const std::int64_t ts2 = ts0 + 6 * kDayNs;
  const std::string d0 = utc_date_from_ns(ts0);
  const std::string d1 = utc_date_from_ns(ts1);
  const std::string d2 = utc_date_from_ns(ts2);
  const std::string a0 = write_pit_archive(dir, d0, pit_surfaces(ts0, 0.0));
  const std::string a1 = write_pit_archive(dir, d1, pit_surfaces(ts1, 1.5));
  const std::string a2 = write_pit_archive(dir, d2, pit_surfaces(ts2, 3.0));
  auto clock = Clock::from_manifest(pit_manifest({{d0, a0}, {d1, a1}, {d2, a2}}));
  EXPECT_TRUE(clock) << (clock ? std::string{} : clock.error().to_string());
  return PitFixture{dir, clock ? std::move(*clock) : Clock{}, d0, d1, d2};
}

[[nodiscard]] DispersionBacktestConfig pit_backtest_config() {
  DispersionBacktestConfig config;
  config.target_dte_days = 36.525; // 0.10y, matching the boards' 0.05-0.50 terms
  config.gross_index_vega = 1000.0;
  config.min_names = 2;
  config.entry_every_n = 1;
  config.project_to_calendar_expiry = false;
  // A PIT membership change only reaches the BOOK at the next roll (the resolver
  // updates the basket every step; the lots are rebuilt when the front cohort is
  // rolled). The fixture's sessions are 3 days apart, so the roll horizon must sit
  // just inside the entry tenor for a roll to fire on the reconstitution date —
  // with the production 7-day default no roll happens in a 3-session window and
  // the test would prove nothing.
  config.roll_dte_days = 34.0; // residual at d1 = 36.525 - 3 = 33.525 < 34 => roll
  return config;
}

} // namespace

// A schedule that reconstitutes mid-window must produce a DIFFERENT track than
// the same schedule's frozen day-1 basket. Before C1-ACTIVATE both paths went
// through the frozen overload and this difference was unobservable.
TEST(DispersionWorkflow, SurfaceBacktestSeamHonorsMidWindowReconstitution) {
  const PitFixture fixture = make_pit_fixture("atx-disp-seam-pit");
  ASSERT_EQ(fixture.clock.size(), 3u);

  // N2 leaves at d1 (its board is still archived, so this is membership, not a
  // missing-surface drop).
  const std::vector<UniverseRow> schedule = {
      row(fixture.d0, "N0", 0.5), row(fixture.d0, "N1", 0.3), row(fixture.d0, "N2", 0.2),
      row(fixture.d1, "N0", 0.6), row(fixture.d1, "N1", 0.4),
  };
  const DispersionBacktestConfig config = pit_backtest_config();

  auto frozen_universe = universe_at(schedule, fixture.d0, "SPY");
  ASSERT_TRUE(frozen_universe) << frozen_universe.error().to_string();
  ASSERT_EQ(frozen_universe->names.size(), 3u);

  auto frozen = run_dispersion_surface_backtest(fixture.clock, *frozen_universe, config);
  auto pit = run_dispersion_surface_backtest(fixture.clock, schedule, config, "SPY");
  ASSERT_TRUE(frozen) << frozen.error().to_string();
  ASSERT_TRUE(pit) << pit.error().to_string();
  ASSERT_EQ(frozen->track.size(), 3u);
  ASSERT_EQ(pit->track.size(), 3u);

  // Day 0 is the same 3-name basket under both paths (index + 3 names, one
  // straddle each => 8 lots) ...
  EXPECT_EQ(pit->track.n_open_lots.front(), 8u);
  EXPECT_EQ(frozen->track.n_open_lots.front(), 8u);
  // ... and from the reconstitution onward the PIT path has genuinely dropped N2
  // (index + 2 names => 6 lots) while the frozen path still carries all three.
  EXPECT_EQ(frozen->track.n_open_lots.back(), 8u);
  EXPECT_EQ(pit->track.n_open_lots.back(), 6u);
  // The dropped name stops contributing P&L, so the tracks diverge economically —
  // not merely in composition. This is the assertion that fails if the seam is
  // reverted to the frozen overload.
  EXPECT_NE(pit->track.nav.back(), frozen->track.nav.back());

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// GOLDEN GUARD. The pinned 82-session fixture's schedule has a SINGLE
// effective_date block, so activation must be a no-op there. This pins that
// property structurally (single-block schedule => bit-identical to frozen) so a
// future resolver change cannot silently move the reproducibility pin.
TEST(DispersionWorkflow, SingleBlockScheduleIsBitIdenticalToTheFrozenPath) {
  const PitFixture fixture = make_pit_fixture("atx-disp-seam-single");
  ASSERT_EQ(fixture.clock.size(), 3u);

  const std::vector<UniverseRow> schedule = {
      row(fixture.d0, "N0", 0.5), row(fixture.d0, "N1", 0.3), row(fixture.d0, "N2", 0.2)};
  const DispersionBacktestConfig config = pit_backtest_config();

  auto frozen_universe = universe_at(schedule, fixture.d0, "SPY");
  ASSERT_TRUE(frozen_universe) << frozen_universe.error().to_string();

  auto frozen = run_dispersion_surface_backtest(fixture.clock, *frozen_universe, config);
  auto pit = run_dispersion_surface_backtest(fixture.clock, schedule, config, "SPY");
  ASSERT_TRUE(frozen) << frozen.error().to_string();
  ASSERT_TRUE(pit) << pit.error().to_string();
  ASSERT_EQ(frozen->track.size(), pit->track.size());

  for (std::size_t i = 0; i < frozen->track.size(); ++i) {
    EXPECT_EQ(pit->track.nav[i], frozen->track.nav[i]) << "nav diverged at step " << i;
    EXPECT_EQ(pit->track.pnl_total[i], frozen->track.pnl_total[i]) << "pnl diverged at step " << i;
    EXPECT_EQ(pit->track.n_open_lots[i], frozen->track.n_open_lots[i]);
  }

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// ── WS-X integration: frictions (X2) and risk limits (X3) at the seam ────────
//
// These ride the same PIT fixture as the C1-ACTIVATE tests because they need a
// real (small) surface backtest, not a mock: the point is that the knobs reach
// the ENGINE, which only a real run can demonstrate.

namespace {

[[nodiscard]] const std::vector<double> *signal_series(const BacktestResult &track,
                                                       std::string_view name) {
  for (const auto &entry : track.signals) {
    if (entry.first == name) {
      return &entry.second;
    }
  }
  return nullptr;
}

[[nodiscard]] std::vector<UniverseRow> static_schedule(const std::string &d0) {
  return {row(d0, "N0", 0.5), row(d0, "N1", 0.3), row(d0, "N2", 0.2)};
}

[[nodiscard]] double sum_of(const std::vector<double> &values) {
  double total = 0.0;
  for (const double v : values) {
    total += v;
  }
  return total;
}

// The quantity `DispersionRiskLimits::max_gross_vega` is actually compared
// against: the risk probe's GROSS vega, Σ|straddle_vega × straddle_qty| over the
// index leg AND every basket leg (`dispersion_strategy.cpp`, `risk_probe`).
//
// The X3 limit tests used to read `track.gross_vega.front()` instead, which is
// the NET book vega — a vega-neutral dispersion book drives that to zero by
// construction, so "0.25 × natural_vega" was 0.25 of a floating-point residual.
// It happened to be a denormal-ish nonzero, so the tests passed; E1 (AN-P1-1)
// rescaled the book and the residual now cancels to EXACTLY 0.0, which turned
// the limit into 0.0 == "unlimited" and unmasked the latent defect. Measuring
// the probe's own quantity from the strategy's own builder makes these tests
// independent of that cancellation.
[[nodiscard]] double natural_gross_vega(const PitFixture &fixture,
                                        const std::vector<UniverseRow> &schedule,
                                        const DispersionBacktestConfig &config) {
  auto snap = MarketSnapshot::load(fixture.clock.refs().front().archive_path);
  EXPECT_TRUE(snap.has_value());
  if (!snap.has_value()) {
    return 0.0;
  }
  DispersionStrategy strategy = make_dispersion_backtest_strategy(schedule, config, "SPY");
  auto book = strategy.build_book(*snap);
  EXPECT_TRUE(book.has_value());
  if (!book.has_value()) {
    return 0.0;
  }
  double gross = std::fabs(book->index_leg.straddle_vega * book->index_leg.straddle_qty);
  for (const DispersionLeg &leg : book->name_legs) {
    gross += std::fabs(leg.straddle_vega * leg.straddle_qty);
  }
  return gross;
}

} // namespace

// X2. Frictions were never wired into the dispersion path — RunConfig carried
// them, the config assembly never set them, so every fill was a frictionless
// mid. Assert the SIGN and rough MAGNITUDE of the cost, not a new golden.
TEST(DispersionWorkflow, FrictionedRunCostsTheSpreadOnTradedNotional) {
  const PitFixture fixture = make_pit_fixture("atx-disp-x2-frictions");
  const std::vector<UniverseRow> schedule = static_schedule(fixture.d0);

  const DispersionBacktestConfig frictionless = pit_backtest_config();
  auto base = run_dispersion_surface_backtest(fixture.clock, schedule, frictionless, "SPY");
  ASSERT_TRUE(base) << base.error().to_string();
  // The pinned default really is frictionless: not one cent of cost.
  EXPECT_EQ(sum_of(base->track.cost), 0.0);

  constexpr double kHalfSpreadBps = 50.0;
  DispersionBacktestConfig costed = pit_backtest_config();
  costed.run.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  costed.run.frictions.half_spread_bps = kHalfSpreadBps;
  auto with_costs = run_dispersion_surface_backtest(fixture.clock, schedule, costed, "SPY");
  ASSERT_TRUE(with_costs) << with_costs.error().to_string();

  const double total_cost = sum_of(with_costs->track.cost);
  const double traded = sum_of(with_costs->track.turnover_notional);
  ASSERT_GT(traded, 0.0) << "the fixture must actually trade for this to mean anything";

  // SIGN: costs are strictly positive and strictly reduce NAV.
  EXPECT_GT(total_cost, 0.0);
  EXPECT_LT(with_costs->track.nav.back(), base->track.nav.back());

  // MAGNITUDE: a half-spread of B bps charged on traded notional. Allow a wide
  // band — turnover accounting and the hedge leg differ in detail — but pin the
  // order of magnitude so a mis-scaled bps (a factor of 100) cannot pass.
  const double expected = traded * kHalfSpreadBps / 1.0e4;
  EXPECT_GT(total_cost, 0.2 * expected);
  EXPECT_LT(total_cost, 5.0 * expected);

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// X3. A deliberately tight gross-vega limit must clamp the book down to the
// limit and RECORD why — a silent clamp would be worse than no limit at all.
TEST(DispersionWorkflow, TightGrossVegaLimitClampsTheBookAndRecordsTheReason) {
  const PitFixture fixture = make_pit_fixture("atx-disp-x3-clamp");
  const std::vector<UniverseRow> schedule = static_schedule(fixture.d0);

  auto unlimited =
      run_dispersion_surface_backtest(fixture.clock, schedule, pit_backtest_config(), "SPY");
  ASSERT_TRUE(unlimited) << unlimited.error().to_string();
  const double natural_vega = natural_gross_vega(fixture, schedule, pit_backtest_config());
  ASSERT_GT(natural_vega, 0.0);
  // Default limits are unlimited => no risk telemetry columns at all, which is
  // what keeps the pinned golden's schema unchanged.
  EXPECT_EQ(signal_series(unlimited->track, "risk_clamp_scale"), nullptr);

  DispersionBacktestConfig limited = pit_backtest_config();
  limited.limits.max_gross_vega = 0.25 * natural_vega; // deliberately tight
  limited.limits.action = RiskBreachAction::Clamp;
  auto clamped = run_dispersion_surface_backtest(fixture.clock, schedule, limited, "SPY");
  ASSERT_TRUE(clamped) << clamped.error().to_string();

  // The book is scaled down toward the limit, not left oversized.
  //
  // FIX-5/M5: this used to read
  //     EXPECT_LT(fabs(clamped->track.gross_vega.front()), 0.5 * natural_vega);
  // which compared the NET book vega (the track column, x multiplier) against the
  // probe's GROSS quantity. `natural_gross_vega`'s own comment above explains that a
  // vega-neutral dispersion book drives the NET column to ~0 BY CONSTRUCTION, so that
  // assertion held whether or not the clamp fired — near-vacuous by the sprint's own
  // definition. 2a7321c fixed the LIMIT computation and left the ASSERTION.
  //
  // The net-vega column cannot carry this property at all, so the size claim is made
  // on a column that genuinely scales with book size, against the unlimited run as its
  // own control. Both runs share a fixture, a schedule and a config modulo the limit,
  // so the only thing that can move this is the clamp.
  ASSERT_FALSE(unlimited->track.turnover_notional.empty());
  ASSERT_FALSE(clamped->track.turnover_notional.empty());
  EXPECT_GT(unlimited->track.turnover_notional.front(), 0.0)
      << "control run must actually open a book, else the comparison is vacuous";
  EXPECT_LT(clamped->track.turnover_notional.front(),
            unlimited->track.turnover_notional.front())
      << "a clamped book must trade less notional than the unclamped one";
  // Still trading — a clamp is not a halt.
  EXPECT_GT(clamped->track.n_open_lots.front(), 0u);

  // The reason is in the output.
  const std::vector<double> *scale = signal_series(clamped->track, "risk_clamp_scale");
  const std::vector<double> *reason = signal_series(clamped->track, "risk_breach_reason");
  ASSERT_NE(scale, nullptr) << "a configured limit must emit its telemetry";
  ASSERT_NE(reason, nullptr);
  ASSERT_FALSE(scale->empty());
  EXPECT_LT(scale->front(), 1.0) << "the clamp factor must be recorded";
  EXPECT_GT(scale->front(), 0.0);
  // And it is the RIGHT factor: limit / requested == 0.25 by construction, now
  // that `natural_vega` measures the quantity the probe actually compares.
  EXPECT_NEAR(scale->front(), 0.25, 1.0e-9) << "clamp scale must equal limit/requested";
  EXPECT_EQ(reason->front(), static_cast<double>(RiskBreachReason::GrossVega));
  EXPECT_EQ(to_string(RiskBreachReason::GrossVega), "max_gross_vega");

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// X3. The same breach under the Halt action must open NO risk and say so.
TEST(DispersionWorkflow, HaltActionOpensNoRiskAndIsNeverSilent) {
  const PitFixture fixture = make_pit_fixture("atx-disp-x3-halt");
  const std::vector<UniverseRow> schedule = static_schedule(fixture.d0);

  auto unlimited =
      run_dispersion_surface_backtest(fixture.clock, schedule, pit_backtest_config(), "SPY");
  ASSERT_TRUE(unlimited) << unlimited.error().to_string();
  const double natural_vega = natural_gross_vega(fixture, schedule, pit_backtest_config());
  ASSERT_GT(natural_vega, 0.0);

  DispersionBacktestConfig halting = pit_backtest_config();
  halting.limits.max_gross_vega = 0.25 * natural_vega;
  halting.limits.action = RiskBreachAction::Halt;
  auto halted = run_dispersion_surface_backtest(fixture.clock, schedule, halting, "SPY");
  ASSERT_TRUE(halted) << halted.error().to_string();

  // Nothing was ever opened, so the run stays flat throughout.
  for (const std::uint32_t lots : halted->track.n_open_lots) {
    EXPECT_EQ(lots, 0u);
  }

  const std::vector<double> *scale = signal_series(halted->track, "risk_clamp_scale");
  const std::vector<double> *reason = signal_series(halted->track, "risk_breach_reason");
  ASSERT_NE(scale, nullptr);
  ASSERT_NE(reason, nullptr);
  EXPECT_EQ(scale->front(), 0.0) << "a halt records scale 0, not a missing row";
  EXPECT_EQ(reason->front(), static_cast<double>(RiskBreachReason::GrossVega));

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// X3. Unlimited (default) limits must leave the track bit-identical — this is
// the property that protects the 82-session reproducibility pin.
TEST(DispersionWorkflow, DefaultRiskLimitsLeaveTheTrackBitIdentical) {
  const PitFixture fixture = make_pit_fixture("atx-disp-x3-default");
  const std::vector<UniverseRow> schedule = static_schedule(fixture.d0);

  auto ungated =
      run_dispersion_surface_backtest(fixture.clock, schedule, pit_backtest_config(), "SPY");
  DispersionBacktestConfig explicit_unlimited = pit_backtest_config();
  explicit_unlimited.limits = DispersionRiskLimits{}; // all zero == unlimited
  auto gated = run_dispersion_surface_backtest(fixture.clock, schedule, explicit_unlimited, "SPY");
  ASSERT_TRUE(ungated) << ungated.error().to_string();
  ASSERT_TRUE(gated) << gated.error().to_string();
  ASSERT_EQ(ungated->track.size(), gated->track.size());

  for (std::size_t i = 0; i < ungated->track.size(); ++i) {
    EXPECT_EQ(gated->track.nav[i], ungated->track.nav[i]) << "step " << i;
    EXPECT_EQ(gated->track.gross_vega[i], ungated->track.gross_vega[i]) << "step " << i;
  }
  EXPECT_EQ(signal_series(gated->track, "risk_clamp_scale"), nullptr);

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// X3. The drawdown stop is enforced at the SEAM (the engine never shows a
// strategy its NAV). A stop tight enough to bind must halt trading and record it.
TEST(DispersionWorkflow, DrawdownStopHaltsTradingAndRecordsTheReason) {
  const PitFixture fixture = make_pit_fixture("atx-disp-x3-drawdown");
  const std::vector<UniverseRow> schedule = static_schedule(fixture.d0);

  auto unstopped =
      run_dispersion_surface_backtest(fixture.clock, schedule, pit_backtest_config(), "SPY");
  ASSERT_TRUE(unstopped) << unstopped.error().to_string();

  // The stop is measured in CURRENCY against a capital base (NAV is cumulative
  // P&L from zero, not an equity curve). Find the fixture's worst peak-to-trough
  // loss and put the stop strictly inside it so it is guaranteed to bind.
  double peak = 0.0;
  double worst_loss = 0.0;
  for (const double nav : unstopped->track.nav) {
    peak = std::max(peak, nav);
    worst_loss = std::max(worst_loss, peak - nav);
  }
  ASSERT_GT(worst_loss, 0.0) << "fixture must lose money somewhere for a stop to bind";

  DispersionBacktestConfig stopped = pit_backtest_config();
  // Capital high enough that the OUTLAY limit cannot bind — this test is about
  // the drawdown stop alone, not about capital-based sizing.
  stopped.limits.capital = 1.0e12;
  stopped.limits.drawdown_stop = 0.5 * worst_loss / stopped.limits.capital;
  auto result = run_dispersion_surface_backtest(fixture.clock, schedule, stopped, "SPY");
  ASSERT_TRUE(result) << result.error().to_string();

  const std::vector<double> *reason = signal_series(result->track, "risk_breach_reason");
  ASSERT_NE(reason, nullptr) << "a configured drawdown stop must emit telemetry";
  const bool recorded = std::any_of(reason->begin(), reason->end(), [](double value) {
    return value == static_cast<double>(RiskBreachReason::DrawdownStop);
  });
  EXPECT_TRUE(recorded) << "the halt must name drawdown_stop in the output";
  EXPECT_EQ(to_string(RiskBreachReason::DrawdownStop), "drawdown_stop");

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}
