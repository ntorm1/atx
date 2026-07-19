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
