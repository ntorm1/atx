// mag7_dispersion_backtest gate tests.
//
// This is the acceptance-example gate: it drives the SAME library pipeline
// the example composes (SurfaceDb -> Clock::from_surface_db ->
// make_dispersion_strangle_spec -> DeclarativeStrategy -> run_backtest ->
// the run_report emitters), NOT the example binary. Fixture: a synthetic
// SurfaceDb with 8 symbols (7 fake MAG7 names + "SPY"), 12 daily partitions,
// distinct per-symbol vol bumps/spots, TEST-scale strategy config
// (tenor_days=6, close_dte_days=2.5, theta_per_name_daily=10, min_names=4).
//
//   1. EndToEnd_DbToEmittedFiles  — the full pipeline into a temp out dir;
//      every emitted file exists, meta lines parse, series.csv has 12 rows.
//   2. FortyDeltaOnDbSurfaces     — every leg resolved off the FIRST db
//      snapshot reprices to |delta| ~ 0.40.
//   3. CohortMechanics            — n_open_lots ramps 16/day (8 symbols x 2
//      legs) to a 4-cohort/64-lot plateau; pnl_settlement stays 0 (roll-close
//      only, never engine settlement).
//   4. VegaFlatAtEntry            — net entry-cohort vega ~ 0 on every date.
//   5. DeterminismAcrossThreads   — n_threads 1 vs 4 -> bit-identical result.
//
// Plus DISABLED_PersistFixtureDbForDriverGoldens — not a test: a fixture
// emitter that persists the db above at $ATX_MAG7_FIXTURE_DB for the driver
// output-byte goldens (Wave C T2). DISABLED_, so the suite never runs it.

#include <gtest/gtest.h>

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
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"            // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"            // Clock, MarketSnapshot, run_backtest, RunConfig
#include "atx/vol/dispersion.hpp"          // MissingNamePolicy, MissingNameSpec
#include "atx/vol/dispersion_strangle.hpp" // DispersionStrangleConfig, make_dispersion_strangle_spec
#include "atx/vol/priced_surface.hpp"      // PricedSurface, PricingContext
#include "atx/vol/tools/run_report.hpp"          // MetaKv, write_* emitters
#include "atx/vol/strategy.hpp"            // DeclarativeStrategy, resolve_spec_with_policy
#include "atx/vol/surface_archive.hpp"     // SurfaceArchiveItem
#include "atx/vol/surface_db.hpp"          // SurfaceDb
#include "atx/vol/surface_parity.hpp"      // SliceContext
#include "atx/vol/tools/tearsheet.hpp"           // TearSheet, tearsheet
#include "atx/vol/types.hpp"               // Result, ErrorCode
#include "atx/vol/vol_curve.hpp"           // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"         // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

// A synthetic eSSVI PricedSurface (flat forward == spot, genuine American
// premium via q_eff=0.02), 7 slices T in [0.05, 1.0]. Copied verbatim from
// surface_db_backtest_test.cpp's make_surface (Task 1's fixture pattern).
[[nodiscard]] PricedSurface make_surface(double S, std::int64_t now_ts, double vol_bump,
                                         std::uint32_t uid) {
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
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

// Fresh per-test temp dir under the system temp root, self-cleaning at start.
// Copied from surface_db_test.cpp:150-153 (via surface_db_backtest_test.cpp).
[[nodiscard]] fs::path test_root(std::string_view name) {
  auto p = fs::temp_directory_path() / ("atx_surface_db_" + std::string(name));
  fs::remove_all(p);
  return p;
}

[[nodiscard]] fs::path out_root(std::string_view name) {
  auto p = fs::temp_directory_path() / ("atx_mag7_dispersion_out_" + std::string(name));
  fs::remove_all(p);
  return p;
}

// 7 fake MAG7 names + the SPY index, distinct spots/vol bumps so the fixture
// is non-degenerate (per-name resolution genuinely differs).
const std::vector<std::string> kNames = {"AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "META", "TSLA"};
const std::string kIndexSym = "SPY";
constexpr double kBaseSpot[] = {195.0, 410.0, 175.0, 185.0, 120.0, 480.0, 250.0};
constexpr double kVolBump[] = {0.00, 0.01, 0.02, 0.03, 0.04, 0.05, 0.06};
constexpr double kIndexSpot = 560.0;
constexpr double kIndexVolBump = 0.0;
constexpr int kNumDates = 12;

// Build a fresh SurfaceDb at a self-cleaning temp root: kNumDates daily
// partitions, each holding all 7 names + SPY (distinct uids 1..8, gentle
// per-date spot drift so PnL is non-degenerate). Returns the db root path.
[[nodiscard]] fs::path build_fixture_db_at(const fs::path &root) {
  auto db = SurfaceDb::create(root.string());
  EXPECT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const std::int64_t base_ts = 1'700'000'000'000'000'000LL;
  for (int d = 0; d < kNumDates; ++d) {
    char date[11];
    std::snprintf(date, sizeof date, "2026-03-%02d", d + 1);
    const std::int64_t ts = base_ts + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(kNames.size() + 1);
    for (std::size_t i = 0; i < kNames.size(); ++i) {
      const double spot = kBaseSpot[i] * (1.0 + 0.0015 * static_cast<double>(d));
      surfaces.push_back(make_surface(spot, ts, kVolBump[i], static_cast<std::uint32_t>(i + 1)));
    }
    surfaces.push_back(make_surface(kIndexSpot * (1.0 + 0.001 * static_cast<double>(d)), ts,
                                    kIndexVolBump, static_cast<std::uint32_t>(kNames.size() + 1)));
    std::vector<SurfaceArchiveItem> items;
    items.reserve(surfaces.size());
    for (std::size_t i = 0; i < kNames.size(); ++i) {
      items.push_back(SurfaceArchiveItem{kNames[i], &surfaces[i]});
    }
    items.push_back(SurfaceArchiveItem{kIndexSym, &surfaces.back()});
    EXPECT_TRUE(db->write_partition(date, items).has_value());
  }
  return root;
}

// Same content, at a self-cleaning per-test temp root.
[[nodiscard]] fs::path build_fixture_db(std::string_view tag) {
  return build_fixture_db_at(test_root(tag));
}

// TEST-scale strategy config, shared across every test in this file.
[[nodiscard]] DispersionStrangleConfig test_cfg() {
  DispersionStrangleConfig cfg;
  cfg.names = kNames;
  cfg.index_symbol = kIndexSym;
  cfg.tenor_days = 6.0;
  cfg.close_dte_days = 2.5;
  cfg.theta_per_name_daily = 10.0;
  cfg.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 4};
  return cfg;
}

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// Copied from spy_strangle_backtest_test.cpp:158's expect_result_bit_identical
// (same column set).
void expect_result_bit_identical(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  const std::vector<std::pair<const std::vector<double> *, const std::vector<double> *>> cols = {
      {&a.pnl_total, &b.pnl_total},     {&a.pnl_theta, &b.pnl_theta},
      {&a.pnl_gamma, &b.pnl_gamma},     {&a.pnl_vega, &b.pnl_vega},
      {&a.nav, &b.nav},                 {&a.gross_vega, &b.gross_vega},
      {&a.gross_theta, &b.gross_theta}, {&a.n_open_lots, &b.n_open_lots},
      {&a.n_unpriced_lots, &b.n_unpriced_lots},
      {&a.n_unpriced_greeks, &b.n_unpriced_greeks}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    for (const auto &[va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
  }
}

// ATX_MAG7_FIXTURE_DB, or "" when unset/empty. Read with _dupenv_s under
// MSVC/clang-cl: plain std::getenv trips /WX (-Wdeprecated-declarations); same
// pattern as spy_fit_corpus_test.cpp:37-51.
[[nodiscard]] std::string fixture_db_env() {
#if defined(_MSC_VER)
  char *e = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&e, &n, "ATX_MAG7_FIXTURE_DB") != 0 || e == nullptr) {
    return {};
  }
  std::string out(e);
  std::free(e);
  return out;
#else
  const char *e = std::getenv("ATX_MAG7_FIXTURE_DB");
  return (e == nullptr) ? std::string{} : std::string(e);
#endif
}

[[nodiscard]] std::string read_file(const std::string &path) {
  std::ifstream is(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
}

} // namespace

// ── 1. End-to-end: db -> clock -> spec -> strategy -> run -> all emitters ──
TEST(Mag7DispersionBacktest, EndToEnd_DbToEmittedFiles) {
  const fs::path db_root = build_fixture_db("end_to_end");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();

  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), static_cast<std::size_t>(kNumDates));

  auto spec = make_dispersion_strangle_spec(test_cfg());
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  DeclarativeStrategy strat(*spec);
  RunConfig rc;
  rc.snapshot_cache = std::make_shared<SnapshotCache>();
  rc.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  auto r = run_backtest(*clock, strat, rc);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->size(), static_cast<std::size_t>(kNumDates));

  const TearSheet ts = tearsheet(*r);
  EngineRunStats stats;
  stats.wall_clock_ms = 1.0;
  stats.n_steps = r->size();
  stats.cache = rc.snapshot_cache->stats();

  const fs::path out = out_root("end_to_end");
  std::error_code ec;
  fs::create_directories(out, ec);
  const MetaKv meta = {{"strategy", "mag7_dispersion_strangle"}, {"names", "AAPL,MSFT,GOOGL"}};

  const std::string series_path = (out / "series.csv").string();
  ASSERT_TRUE(write_backtest_series_csv(*r, meta, series_path).has_value());

  MetaKv strat_rows = strategy_metrics(ts);
  const MetaKv summary_rows = result_summary_metrics(*r);
  strat_rows.insert(strat_rows.end(), summary_rows.begin(), summary_rows.end());
  const std::string strat_path = (out / "strategy_metrics.csv").string();
  ASSERT_TRUE(write_metrics_csv(meta, strat_rows, strat_path).has_value());

  const std::string engine_path = (out / "engine_metrics.csv").string();
  ASSERT_TRUE(write_metrics_csv(meta, engine_metrics(stats), engine_path).has_value());

  const std::string db_stats_path = (out / "db_stats.csv").string();
  ASSERT_TRUE(write_surface_db_stats_csv(*db, meta, db_stats_path).has_value());

  // Every file exists; every meta line ("# key=value") parses.
  for (const std::string &p : {series_path, strat_path, engine_path, db_stats_path}) {
    ASSERT_TRUE(fs::exists(p)) << p;
    std::istringstream iss(read_file(p));
    std::string line;
    int n_meta = 0;
    while (std::getline(iss, line)) {
      if (line.rfind("# ", 0) == 0) {
        EXPECT_NE(line.find('='), std::string::npos) << p << ": " << line;
        ++n_meta;
      }
    }
    // write_surface_db_stats_csv appends its own 5 db-inventory meta entries
    // after the caller's 2 (pinned/tested in run_report_test.cpp); every
    // other writer here emits exactly the caller's 2. >= 2 covers both.
    EXPECT_GE(n_meta, 2) << p;
  }

  // series.csv: 2 meta + 1 header + 12 data rows == 15 lines.
  std::istringstream series_iss(read_file(series_path));
  int n_lines = 0;
  std::string l;
  while (std::getline(series_iss, l)) {
    ++n_lines;
  }
  EXPECT_EQ(n_lines, 2 + 1 + kNumDates);

  fs::remove_all(db_root, ec);
  fs::remove_all(out, ec);
}

// ── 2. Every leg resolved off the FIRST db snapshot reprices to |delta|~0.40 ──
TEST(Mag7DispersionBacktest, FortyDeltaOnDbSurfaces) {
  const fs::path db_root = build_fixture_db("forty_delta");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(test_cfg());
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  auto snap = MarketSnapshot::load(clock->refs()[0].archive_path);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();
  auto legs = resolve_spec_with_policy(*snap, *spec, nullptr);
  ASSERT_TRUE(legs.has_value()) << legs.error().to_string();
  ASSERT_EQ(legs->size(), 16u); // 8 symbols x {call, put}

  for (const auto &sl : *legs) {
    const SurfaceRef surf = snap->find(sl.leg.uid);
    ASSERT_NE(surf, nullptr);
    auto d = surf->delta(sl.leg.K, sl.leg.T, sl.leg.side);
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(std::abs(*d), 0.40, 1e-3);
    const double F = surf->forward_at(sl.leg.T);
    if (sl.leg.side == Side::Call) {
      EXPECT_GT(sl.leg.K, F);
    } else {
      EXPECT_LT(sl.leg.K, F);
    }
  }
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 3. Cohort mechanics: 16/day ramp to a 4-cohort/64-lot plateau ───────────
TEST(Mag7DispersionBacktest, CohortMechanics) {
  const fs::path db_root = build_fixture_db("cohort_mechanics");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(test_cfg());
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  DeclarativeStrategy strat(*spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->size(), static_cast<std::size_t>(kNumDates));

  // tenor 6d, close below 2.5d -> a cohort lives ages 0..3 (4 live cohorts at
  // steady state x 16 lots/cohort (8 symbols x {call,put}) -> plateau 64.
  const double expect[kNumDates] = {16, 32, 48, 64, 64, 64, 64, 64, 64, 64, 64, 64};
  for (std::size_t i = 0; i < static_cast<std::size_t>(kNumDates); ++i) {
    EXPECT_EQ(r->n_open_lots[i], expect[i]) << i;
    EXPECT_EQ(r->pnl_settlement[i], 0.0) << i; // roll-close only, never engine settlement
  }
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 4. Net entry-cohort vega ~ 0 on every date ──────────────────────────────
TEST(Mag7DispersionBacktest, VegaFlatAtEntry) {
  const fs::path db_root = build_fixture_db("vega_flat");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(test_cfg());
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  for (const SnapshotRef &ref : clock->refs()) {
    auto snap = MarketSnapshot::load(ref.archive_path);
    ASSERT_TRUE(snap.has_value()) << ref.date;
    auto legs = resolve_spec_with_policy(*snap, *spec, nullptr);
    ASSERT_TRUE(legs.has_value()) << ref.date << ": " << legs.error().to_string();
    double net_vega = 0.0;
    double gross_vega = 0.0;
    for (const auto &sl : *legs) {
      net_vega += sl.qty * sl.leg.vega * sl.multiplier;
      gross_vega += std::abs(sl.qty * sl.leg.vega * sl.multiplier);
    }
    EXPECT_LE(std::abs(net_vega), 1e-9 * gross_vega) << ref.date;
  }
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 5. Determinism across thread counts ─────────────────────────────────────
TEST(Mag7DispersionBacktest, DeterminismAcrossThreads) {
  const fs::path db_root = build_fixture_db("determinism");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(test_cfg());
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  DeclarativeStrategy s1(*spec);
  DeclarativeStrategy s4(*spec);
  RunConfig cfg1;
  cfg1.price.n_threads = 1;
  RunConfig cfg4;
  cfg4.price.n_threads = 4;
  auto r1 = run_backtest(*clock, s1, cfg1);
  auto r4 = run_backtest(*clock, s4, cfg4);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();
  expect_result_bit_identical(*r1, *r4);

  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── Fixture emitter (NOT a test; DISABLED_ so the suite never runs it) ───────
//
// Persists this file's deterministic fixture db (12 daily partitions
// 2026-03-01..12, 8 symbols = the 7 kNames + SPY, uids 1..8, fixed
// base_ts = 1'700'000'000'000'000'000, fixed kBaseSpot/kVolBump) at the path in
// the ATX_MAG7_FIXTURE_DB environment variable and does NOT delete it, so the
// example-driver binaries (mag7_dispersion_backtest, spy_dispersion_pnl) can be
// run against a stable db to capture output-byte goldens.
//
// The db's CONTENT is reproducible; its per-partition `file_size` and
// `created_ts_ns` (which db_stats.csv reports) are NOT reproducible across
// rebuilds. So it is built ONCE per wave and reused unchanged; any golden that
// covers db_stats.csv is a same-db golden only.
//
// Run explicitly:
//   $env:ATX_MAG7_FIXTURE_DB = "<dir>"
//   atx-vol-tests.exe --gtest_also_run_disabled_tests \
//     --gtest_filter=Mag7DispersionBacktest.DISABLED_PersistFixtureDbForDriverGoldens
TEST(Mag7DispersionBacktest, DISABLED_PersistFixtureDbForDriverGoldens) {
  const std::string dest = fixture_db_env();
  if (dest.empty()) {
    GTEST_SKIP() << "ATX_MAG7_FIXTURE_DB unset";
  }
  const fs::path root(dest);
  std::error_code ec;
  fs::remove_all(root, ec); // fresh build; the caller owns the path
  const fs::path built = build_fixture_db_at(root);
  ASSERT_EQ(built, root);

  auto db = SurfaceDb::open(root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  const auto parts = db->partitions();
  EXPECT_EQ(parts.size(), static_cast<std::size_t>(kNumDates));
  // The 8 symbols live INSIDE each partition archive. `write_partition` only
  // refreshes provenance on symbols already in the manifest symbol table
  // (src/surface_db.cpp:1122-1135) and never adds any, so a db built purely by
  // write_partition has an EMPTY manifest symbol table — `db->symbols()` is 0
  // by construction, not by defect. The per-partition surface_count is the
  // check that actually witnesses the 8 symbols.
  EXPECT_TRUE(db->symbols().empty());
  for (const auto &p : parts) {
    EXPECT_EQ(p.surface_count, static_cast<std::uint32_t>(kNames.size() + 1)) << p.key;
  }
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  std::printf("persisted fixture db: %s (%zu partitions, surface_count=%u each)\n",
              root.string().c_str(), parts.size(),
              parts.empty() ? 0U : parts.front().surface_count);
  // Deliberately NOT removed.
}
