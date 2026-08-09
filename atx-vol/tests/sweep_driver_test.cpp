// sweep_driver.hpp -- cache-first, variant-parallel backtest sweeps over the
// track lakehouse (Task C3, backtest-production-lakehouse sprint).
//
// Only built when ATX_VOL_LAKEHOUSE is ON (tests/CMakeLists.txt) -- run_sweep
// needs Catalog::probe/register_staging/record_trial and
// TrackStore::write_staging actually compiled into atx-vol, mirroring
// track_store_test.cpp/catalog_test.cpp.
//
// The brief's Step 1 gates, verbatim:
//   (a)+(b) DuplicateVariantsCollapseAndRerunIsAllCacheHits -- 4 variants
//       where 2 are identical => 3 engine runs, 4 trial rows (one per
//       ORIGINAL variant -- trials count attempts, not unique configs), a
//       staging file + registered catalog row per unique key; rerun of the
//       SAME sweep => 0 engine runs, all 3 hits, 4 more trial rows appended
//       (8 total).
//   (c) SweepResultNavsMatchIndividualBaselinesUnderVariantParallelism --
//       sweep result NAVs bit-identical to individually-run baselines, under
//       REAL variant-level concurrency (4 outer workers) sharing one
//       SnapshotPool, plus the pool's own single-flight archive-open count.
// Plus a dedicated enumeration/dedupe determinism pin:
//   EnumerationOrderIsDeterministicAcrossFreshLakes -- the SAME grid,
//   enumerated against two independent cold lakes, produces the identical
//   ordered unique-key list both times.

#include "atx/vol/research/sweep_driver.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/backtest_template.hpp" // BacktestStrategyTemplate, ProjectedTemplateStrategy
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/research/snapshot_pool.hpp" // SnapshotPool
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUid = 7;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// A synthetic eSSVI PricedSurface -- mirrors backtest_exec_test.cpp's
// make_surface (proven pattern for a corpus ProjectedTemplateStrategy can
// project 40-delta 3-calendar-month legs against).
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd, std::int64_t now_ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double term_forward = fwd * std::exp((kR - 0.02) * T);
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

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-sweepdrv-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] std::string write_one(const fs::path &dir, const std::string &date, const std::string &symbol,
                                    const PricedSurface &s) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{symbol, &s};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

[[nodiscard]] CorpusManifest make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths,
                                           const std::string &symbol) {
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
  Clock clock;
};

// A single-underlying evolving corpus over `n_dates` calendar days starting
// 2026-08-01 -- the same date pattern backtest_exec_test.cpp's make_corpus
// uses (the engine walks the manifest's own dates; it does not itself
// validate them against an NYSE calendar -- only expiry-target resolution,
// which a 6-day corpus against a 3-calendar-month leg never reaches, does).
[[nodiscard]] Corpus make_corpus(const fs::path &dir, const std::string &symbol, int n_dates) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = 100.0 * (1.0 + 0.004 * static_cast<double>(d));
    const PricedSurface s = make_surface(kUid, S, S, now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_one(dir, date, symbol, s));
  }
  Corpus c;
  c.manifest = make_manifest(dp, symbol);
  auto clock = Clock::from_manifest(c.manifest);
  EXPECT_TRUE(clock.has_value()) << (clock.has_value() ? std::string{} : clock.error().to_string());
  c.clock = clock.has_value() ? std::move(*clock) : Clock{};
  return c;
}

[[nodiscard]] BacktestStrategyTemplate make_variant(double position_sign, unsigned entry_every_n) {
  auto made = make_40_delta_3_calendar_month_strangle_template(position_sign, entry_every_n);
  EXPECT_TRUE(made.has_value()) << (made.has_value() ? std::string{} : made.error().to_string());
  return made.has_value() ? std::move(*made) : BacktestStrategyTemplate{};
}

[[nodiscard]] std::array<std::uint8_t, 32> fixed_snapshot_id(std::uint8_t seed) {
  std::array<std::uint8_t, 32> id{};
  id.fill(seed);
  return id;
}

[[nodiscard]] std::size_t staged_file_count(const fs::path &lake_root) {
  std::error_code ec;
  std::size_t n = 0;
  for (const auto &entry : fs::directory_iterator(lake_root / "staging", ec)) {
    (void)entry;
    ++n;
  }
  return n;
}

} // namespace

// ── (a)+(b): duplicate collapse + staging/register on miss + cache-first rerun ──
TEST(SweepDriverTest, DuplicateVariantsCollapseAndRerunIsAllCacheHits) {
  const fs::path dir = fresh_dir("dup-rerun");
  const Corpus corpus = make_corpus(dir / "corpus", "SPX", 6);
  const fs::path lake_root = dir / "lake";

  SweepSpec spec;
  spec.variants = {
      make_variant(1.0, 100u),  // A
      make_variant(-1.0, 100u), // B
      make_variant(1.0, 100u),  // duplicate of A
      make_variant(1.0, 200u),  // C -- different entry cadence, distinct economics
  };
  spec.clock = corpus.clock;
  spec.uid = kUid;
  spec.meta = TrackMeta{"SPX", "strangle_sweep_test"};
  spec.data_snapshot_id = fixed_snapshot_id(0xAB);

  SweepConfig config;
  config.lake_root = lake_root.string();
  config.sweep_id = "sweep-dup-rerun";
  config.n_threads = 1;

  // ── cold run: 4 submitted, 3 unique, 3 engine runs, 4 trial rows ──────────
  auto first = run_sweep(spec, config);
  ASSERT_TRUE(first.has_value()) << (first.has_value() ? std::string{} : first.error().to_string());
  EXPECT_EQ(first->n_variants_submitted, 4u);
  ASSERT_EQ(first->variants.size(), 3u);
  EXPECT_EQ(first->engine_runs, 3u);
  EXPECT_EQ(first->cache_hits, 0u);
  for (const auto &outcome : first->variants) {
    EXPECT_TRUE(outcome.ran);
    EXPECT_FALSE(outcome.cache_hit);
    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_FALSE(outcome.result->nav.empty());
  }

  // The duplicate (index 2) maps to the SAME track_key as index 0 -- proven
  // directly, not just via the unique count.
  {
    const std::vector<std::uint8_t> canon_a = canonical_config_bytes(spec.variants[0], spec.base_config);
    const std::vector<std::uint8_t> canon_c = canonical_config_bytes(spec.variants[2], spec.base_config);
    const std::string engine_id = make_engine_id();
    const TrackKey key_a = make_track_key(canon_a, engine_id, spec.data_snapshot_id);
    const TrackKey key_c = make_track_key(canon_c, engine_id, spec.data_snapshot_id);
    EXPECT_EQ(key_a.hex(), key_c.hex());
  }

  // Every unique key landed exactly one staging file and one registered
  // catalog row (the miss -> write_staging -> register_staging flow).
  EXPECT_EQ(staged_file_count(lake_root), 3u);
  {
    auto catalog = Catalog::open(lake_root.string());
    ASSERT_TRUE(catalog.has_value()) << (catalog.has_value() ? std::string{} : catalog.error().to_string());
    for (const auto &outcome : first->variants) {
      auto row = catalog->probe(outcome.key);
      ASSERT_TRUE(row.has_value());
      ASSERT_TRUE(row->has_value()) << "a miss should have registered a tracks row";
      EXPECT_EQ((*row)->status, TrackStatus::Staging);
      EXPECT_EQ((*row)->underlier, "SPX");
      EXPECT_EQ((*row)->family, "strangle_sweep_test");
      EXPECT_EQ((*row)->economics_rev, kBacktestEconomicsRev);
    }
    auto stats = catalog->trial_stats(config.sweep_id);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->n_trials, 4u) << "trials count ATTEMPTS (4 original variants), not unique configs (3)";
  }

  // ── rerun, SAME sweep: 0 engine runs, all hits, 4 more trial rows ─────────
  auto second = run_sweep(spec, config);
  ASSERT_TRUE(second.has_value()) << (second.has_value() ? std::string{} : second.error().to_string());
  EXPECT_EQ(second->engine_runs, 0u);
  EXPECT_EQ(second->cache_hits, 3u);
  ASSERT_EQ(second->variants.size(), 3u);
  for (const auto &outcome : second->variants) {
    EXPECT_TRUE(outcome.cache_hit);
    EXPECT_FALSE(outcome.ran);
    EXPECT_FALSE(outcome.result.has_value());
  }
  {
    auto catalog = Catalog::open(lake_root.string());
    ASSERT_TRUE(catalog.has_value());
    auto stats2 = catalog->trial_stats(config.sweep_id);
    ASSERT_TRUE(stats2.has_value());
    EXPECT_EQ(stats2->n_trials, 8u) << "N grows across reruns -- trials count attempts, not unique configs";
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ── enumeration/dedupe determinism: same grid -> same ordered key list ─────
TEST(SweepDriverTest, EnumerationOrderIsDeterministicAcrossFreshLakes) {
  const fs::path dir = fresh_dir("enum-det");
  const Corpus corpus = make_corpus(dir / "corpus", "SPX", 4);

  SweepSpec spec;
  spec.variants = {
      make_variant(1.0, 100u),
      make_variant(-1.0, 100u),
      make_variant(1.0, 100u),  // duplicate of variants[0]
      make_variant(1.0, 200u),
      make_variant(-1.0, 100u), // duplicate of variants[1]
  };
  spec.clock = corpus.clock;
  spec.uid = kUid;
  spec.meta = TrackMeta{"SPX", "strangle_sweep_test"};
  spec.data_snapshot_id = fixed_snapshot_id(0xCD);

  SweepConfig config_a;
  config_a.lake_root = (dir / "lake-a").string();
  config_a.sweep_id = "sweep-enum-a";
  config_a.n_threads = 1;

  SweepConfig config_b = config_a;
  config_b.lake_root = (dir / "lake-b").string();
  config_b.sweep_id = "sweep-enum-b";

  auto result_a = run_sweep(spec, config_a);
  auto result_b = run_sweep(spec, config_b);
  ASSERT_TRUE(result_a.has_value()) << (result_a.has_value() ? std::string{} : result_a.error().to_string());
  ASSERT_TRUE(result_b.has_value()) << (result_b.has_value() ? std::string{} : result_b.error().to_string());

  ASSERT_EQ(result_a->variants.size(), 3u);
  ASSERT_EQ(result_b->variants.size(), 3u);
  EXPECT_EQ(result_a->engine_runs, 3u);
  EXPECT_EQ(result_b->engine_runs, 3u);
  for (std::size_t i = 0; i < result_a->variants.size(); ++i) {
    EXPECT_EQ(result_a->variants[i].key.hex(), result_b->variants[i].key.hex()) << "index " << i;
    EXPECT_EQ(result_a->variants[i].first_variant_index, result_b->variants[i].first_variant_index)
        << "index " << i;
  }
  // Expected first-occurrence order over the 5-element grid above: 0, 1, 3
  // (indices 2 and 4 are duplicates of 0 and 1, so they never introduce a new
  // unique entry).
  ASSERT_EQ(result_a->variants[0].first_variant_index, 0u);
  ASSERT_EQ(result_a->variants[1].first_variant_index, 1u);
  ASSERT_EQ(result_a->variants[2].first_variant_index, 3u);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ── (c): sweep NAVs are bit-identical to individually-run baselines, under
// real variant-level concurrency sharing one SnapshotPool ──────────────────
TEST(SweepDriverTest, SweepResultNavsMatchIndividualBaselinesUnderVariantParallelism) {
  const fs::path dir = fresh_dir("parallel-nav");
  const Corpus corpus = make_corpus(dir / "corpus", "SPX", 6);
  const fs::path lake_root = dir / "lake";

  SweepSpec spec;
  spec.variants = {
      make_variant(1.0, 100u),  make_variant(-1.0, 100u), make_variant(1.0, 150u),
      make_variant(-1.0, 150u), make_variant(1.0, 200u),  make_variant(-1.0, 200u),
  };
  spec.clock = corpus.clock;
  spec.uid = kUid;
  spec.meta = TrackMeta{"SPX", "strangle_sweep_test"};
  spec.data_snapshot_id = fixed_snapshot_id(0xEF);

  SnapshotPool pool;
  SweepConfig config;
  config.snapshot_pool = &pool;
  config.lake_root = lake_root.string();
  config.sweep_id = "sweep-parallel-nav";
  config.n_threads = 4; // real variant-level concurrency, all 6 misses distinct

  auto swept = run_sweep(spec, config);
  ASSERT_TRUE(swept.has_value()) << (swept.has_value() ? std::string{} : swept.error().to_string());
  ASSERT_EQ(swept->variants.size(), 6u);
  EXPECT_EQ(swept->engine_runs, 6u);

  // Single-flight across the concurrent variants: the 6-date corpus is opened
  // exactly once between all 6 racing variants (C2's own gate shape --
  // BacktestExec.SnapshotPoolConcurrentRunsMatchSerial).
  EXPECT_EQ(pool.stats().archive_opens, 6u);

  for (const auto &outcome : swept->variants) {
    ASSERT_TRUE(outcome.ran);
    ASSERT_TRUE(outcome.result.has_value());
    const BacktestStrategyTemplate &variant = spec.variants[outcome.first_variant_index];

    // Solo baseline: same template, same clock, same base_config economics,
    // its OWN private snapshot cache (no pool, no outer concurrency) -- the
    // determinism invariant (I1-I8) says the bytes must match regardless.
    auto baseline_strat = ProjectedTemplateStrategy::create(variant, spec.uid);
    ASSERT_TRUE(baseline_strat.has_value());
    RunConfig baseline_cfg = spec.base_config;
    baseline_cfg.price.n_threads = 1;
    auto baseline = run_backtest(spec.clock, *baseline_strat, baseline_cfg);
    ASSERT_TRUE(baseline.has_value()) << (baseline.has_value() ? std::string{} : baseline.error().to_string());

    ASSERT_EQ(outcome.result->nav.size(), baseline->nav.size());
    for (std::size_t i = 0; i < outcome.result->nav.size(); ++i) {
      EXPECT_TRUE(bits_equal(outcome.result->nav[i], baseline->nav[i]))
          << "variant " << outcome.first_variant_index << " nav row " << i;
    }
    ASSERT_EQ(outcome.result->cash.size(), baseline->cash.size());
    for (std::size_t i = 0; i < outcome.result->cash.size(); ++i) {
      EXPECT_TRUE(bits_equal(outcome.result->cash[i], baseline->cash[i]))
          << "variant " << outcome.first_variant_index << " cash row " << i;
    }
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}
