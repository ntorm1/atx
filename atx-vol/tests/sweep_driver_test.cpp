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
//
// Task D5 additions (economics-rev supersession lives entirely in
// catalog.cpp -- see catalog_test.cpp -- and needs no code here; these three
// gates are the integration legs the D5 brief/controller scope note asks
// for):
//   LastMissFailureLeavesEarlierMissesDurablyStagedAndSelfHeals -- INHERITED
//       OBLIGATION (C3 review): the last miss of a sweep fails mid-run;
//       earlier misses stay durably staged+registered despite the sweep as a
//       whole returning Err; a re-run treats them as cache hits and the
//       failed variant re-runs cleanly. Pins the write_staging/
//       register_staging two-step publish window's self-healing.
//   CompactMarkCompactedAndArrowReloadMatchesOriginalResultsToTheDouble --
//       sweep -> TrackStore::compact() -> Catalog::mark_compacted (per
//       CompactStats::placements) -> reload the REAL compacted Parquet batch
//       via Arrow C++ directly; every value matches the original in-memory
//       BacktestResult bit-for-bit.
//   WritesRealCxxLakeAndCsvSidecarForPythonReadCrossCheck -- INHERITED
//       OBLIGATION (D4 review): env-var-gated (ATX_VOL_D5_FIXTURE_LAKE;
//       GTEST_SKIP when unset) producer of a REAL C++-written lake + a CSV
//       side-car of the exact values this process computed, for
//       python/tests/test_tracks_cxx_fixture.py (skips when the fixture is
//       absent) to independently verify atxpy.tracks reads it correctly --
//       D4's own tests only ever read Python-built fixtures.

#include "atx/vol/sweep_driver.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h> // GetEnvironmentVariableA -- WritesRealCxxLakeAndCsvSidecarForPythonReadCrossCheck
#else
#include <cstdlib>
#endif

#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/backtest_template.hpp" // BacktestStrategyTemplate, ProjectedTemplateStrategy
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/snapshot_pool.hpp" // SnapshotPool
#include "atx/vol/track_key.hpp" // track_key_from_hex
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

// ── D5: compact/reload integration leg helpers -- reads a compacted Parquet
// batch back via Arrow C++ directly (track_store_test.cpp's own read_batch/
// dbl_at/str_at pattern, trimmed to what this file's reload gate needs).
[[nodiscard]] std::shared_ptr<arrow::Table> read_batch_table(const std::string &path) {
  auto in = arrow::io::ReadableFile::Open(path);
  EXPECT_TRUE(in.ok()) << (in.ok() ? std::string{} : in.status().ToString());
  auto reader_res = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
  EXPECT_TRUE(reader_res.ok()) << (reader_res.ok() ? std::string{} : reader_res.status().ToString());
  std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(reader_res);
  auto table_res = reader->ReadTable();
  EXPECT_TRUE(table_res.ok()) << (table_res.ok() ? std::string{} : table_res.status().ToString());
  auto combined = (*table_res)->CombineChunks(arrow::default_memory_pool());
  EXPECT_TRUE(combined.ok());
  return *combined;
}

[[nodiscard]] double dbl_at(const arrow::Table &table, const std::string &col, std::int64_t row) {
  const int idx = table.schema()->GetFieldIndex(col);
  EXPECT_GE(idx, 0) << col;
  const auto &arr = static_cast<const arrow::DoubleArray &>(*table.column(idx)->chunk(0));
  return arr.Value(row);
}

[[nodiscard]] std::string str_at(const arrow::Table &table, const std::string &col, std::int64_t row) {
  const int idx = table.schema()->GetFieldIndex(col);
  EXPECT_GE(idx, 0) << col;
  const auto &arr = static_cast<const arrow::StringArray &>(*table.column(idx)->chunk(0));
  return arr.GetString(row);
}

// Marks every track in `placements` compacted in `lake_root`'s catalog --
// the compact()->mark_compacted() glue track_compact.cpp's D5 change wires
// for real; exercised directly here (rather than shelling out to the CLI
// binary) so this file's tests stay self-contained gtest, matching the rest
// of this suite's convention of calling library entry points directly.
void mark_all_compacted(Catalog &catalog, const std::vector<CompactedTrackPlacement> &placements) {
  for (const auto &placement : placements) {
    auto key = track_key_from_hex(placement.track_key_hex);
    ASSERT_TRUE(key.has_value()) << (key.has_value() ? std::string{} : key.error().to_string());
    ASSERT_TRUE(catalog.mark_compacted(*key, placement.file, placement.row_group).has_value());
  }
}

// ── D5: env-var-gated fixture-lake producer (WritesRealCxxLakeAndCsvSidecar-
// ForPythonReadCrossCheck) -- see this file's top doc comment.
constexpr const char *kFixtureLakeEnv = "ATX_VOL_D5_FIXTURE_LAKE";

// Empty when unset. GetEnvironmentVariableA (not std::getenv, which this
// toolchain flags deprecated under /WX) -- same shape as catalog_test.cpp's
// stress_root_env()/process_scratch_test.cpp's probe_out().
[[nodiscard]] std::string fixture_lake_env() {
#ifdef _WIN32
  char buf[4096];
  const DWORD n = ::GetEnvironmentVariableA(kFixtureLakeEnv, buf, static_cast<DWORD>(sizeof buf));
  if (n == 0 || n >= sizeof buf) {
    return {};
  }
  return std::string(buf, static_cast<std::size_t>(n));
#else
  const char *v = std::getenv(kFixtureLakeEnv);
  return v == nullptr ? std::string{} : std::string{v};
#endif
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

// ── D5 INHERITED OBLIGATION (C3 review): failure-injection abort-recovery ──
//
// Pins the write_staging/register_staging two-step publish window's
// self-healing: if the LAST miss of a sweep fails mid-run, every EARLIER
// miss this call already published stays durably staged+registered even
// though the sweep as a whole reports Err, and a subsequent identical sweep
// treats those earlier successes as cache hits while the failed variant
// re-runs cleanly.
//
// Injection uses `RunConfig::step_observer` -- an EXISTING production hook
// ("Returning Err aborts the run with that error", backtest.hpp), reused
// exactly the way backtest_test.cpp already uses it to stop a run early, not
// a new test-only seam. `config.n_threads = 1` makes `detail::parallel_for`'s
// fan-out over the sweep's misses a PLAIN serial for-loop
// (parallel_for.hpp: `nt <= 1` is `for (i=0;i<n;++i) fn(i);`), so counting
// `step_observer`'s `step_index == 0` calls (fired once per `run_backtest`
// invocation, before that run touches anything else) deterministically
// identifies "the k-th run in this sweep" -- no dependency on real-time
// scheduling.
TEST(SweepDriverTest, LastMissFailureLeavesEarlierMissesDurablyStagedAndSelfHeals) {
  const fs::path dir = fresh_dir("abort-recovery");
  const Corpus corpus = make_corpus(dir / "corpus", "SPX", 6);
  const fs::path lake_root = dir / "lake";

  // 6 DISTINCT variants, no duplicates: cold-lake misses.size() == 6 and
  // unique_order[i] == i, so "the 6th run" IS spec.variants[5].
  SweepSpec spec;
  spec.variants = {
      make_variant(1.0, 100u),  make_variant(-1.0, 100u), make_variant(1.0, 150u),
      make_variant(-1.0, 150u), make_variant(1.0, 200u),  make_variant(-1.0, 200u),
  };
  spec.clock = corpus.clock;
  spec.uid = kUid;
  spec.meta = TrackMeta{"SPX", "strangle_sweep_test"};
  spec.data_snapshot_id = fixed_snapshot_id(0x77);

  std::size_t run_count = 0;
  spec.base_config.step_observer = [&run_count](const StepEvent &event) -> Status {
    if (event.step_index == 0) {
      ++run_count;
      if (run_count == 6) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "D5 injected failure: last miss of the sweep");
      }
    }
    return atx::core::Ok();
  };

  SweepConfig config;
  config.lake_root = lake_root.string();
  config.sweep_id = "sweep-abort-recovery";
  config.n_threads = 1;

  auto first = run_sweep(spec, config);
  ASSERT_FALSE(first.has_value()) << "the injected failure on the 6th miss must abort the whole sweep";
  EXPECT_NE(first.error().to_string().find("D5 injected failure"), std::string::npos)
      << first.error().to_string();
  EXPECT_EQ(run_count, 6u);

  const std::string engine_id = make_engine_id();
  std::vector<TrackKey> keys;
  for (const auto &variant : spec.variants) {
    const std::vector<std::uint8_t> canon = canonical_config_bytes(variant, spec.base_config);
    keys.push_back(make_track_key(canon, engine_id, spec.data_snapshot_id));
  }

  {
    auto catalog = Catalog::open(lake_root.string());
    ASSERT_TRUE(catalog.has_value());
    for (std::size_t i = 0; i < 5; ++i) {
      auto row = catalog->probe(keys[i]);
      ASSERT_TRUE(row.has_value());
      ASSERT_TRUE(row->has_value())
          << "variant " << i << " should have been durably registered before the abort";
      EXPECT_EQ((*row)->status, TrackStatus::Staging);
    }
    // The 6th (failed) variant never reached write_staging/register_staging
    // -- Phase 5 (sweep_driver.cpp) bails at the first Err it sees in
    // `misses` order, before doing anything for that index.
    auto failed_row = catalog->probe(keys[5]);
    ASSERT_TRUE(failed_row.has_value());
    EXPECT_FALSE(failed_row->has_value()) << "the failed variant must not have a partial/ghost catalog row";
  }
  EXPECT_EQ(staged_file_count(lake_root), 5u);

  // ── Re-run the SAME sweep: earlier 5 are cache hits, the previously-
  // failed 6th re-runs cleanly and lands its own staging+register row. ─────
  spec.base_config.step_observer = nullptr; // no more injected failures
  auto second = run_sweep(spec, config);
  ASSERT_TRUE(second.has_value()) << (second.has_value() ? std::string{} : second.error().to_string());
  EXPECT_EQ(second->engine_runs, 1u) << "only the previously-failed variant should re-run";
  EXPECT_EQ(second->cache_hits, 5u);
  ASSERT_EQ(second->variants.size(), 6u);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_TRUE(second->variants[i].cache_hit) << "variant " << i;
    EXPECT_FALSE(second->variants[i].ran) << "variant " << i;
  }
  EXPECT_TRUE(second->variants[5].ran);
  EXPECT_FALSE(second->variants[5].cache_hit);

  {
    auto catalog = Catalog::open(lake_root.string());
    ASSERT_TRUE(catalog.has_value());
    for (std::size_t i = 0; i < keys.size(); ++i) {
      auto row = catalog->probe(keys[i]);
      ASSERT_TRUE(row.has_value());
      ASSERT_TRUE(row->has_value()) << "variant " << i;
      EXPECT_EQ((*row)->status, TrackStatus::Staging) << "variant " << i;
    }
  }
  EXPECT_EQ(staged_file_count(lake_root), 6u);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ── D5 scope item 2: compact/reload integration leg ─────────────────────────
//
// sweep -> TrackStore::compact() -> Catalog::mark_compacted (per
// CompactStats::placements, the compact()<->catalog glue track_compact.cpp's
// D5 change wires for real) -> reload the REAL compacted Parquet batch via
// Arrow C++ directly. Every value matches the original in-memory
// BacktestResult bit-for-bit ("to the double").
TEST(SweepDriverTest, CompactMarkCompactedAndArrowReloadMatchesOriginalResultsToTheDouble) {
  const fs::path dir = fresh_dir("compact-reload");
  const Corpus corpus = make_corpus(dir / "corpus", "SPX", 5);
  const fs::path lake_root = dir / "lake";

  SweepSpec spec;
  spec.variants = {
      make_variant(1.0, 100u),
      make_variant(-1.0, 100u),
      make_variant(1.0, 150u),
  };
  spec.clock = corpus.clock;
  spec.uid = kUid;
  spec.meta = TrackMeta{"SPX", "strangle_compact_test"};
  spec.data_snapshot_id = fixed_snapshot_id(0x55);

  SweepConfig config;
  config.lake_root = lake_root.string();
  config.sweep_id = "sweep-compact-reload";
  config.n_threads = 1;

  auto swept = run_sweep(spec, config);
  ASSERT_TRUE(swept.has_value()) << (swept.has_value() ? std::string{} : swept.error().to_string());
  ASSERT_EQ(swept->engine_runs, 3u);

  // sweep -> compact(): D2's TrackStore::compact() over the SAME lake_root
  // run_sweep just staged into.
  auto compacted = compact(lake_root.string());
  ASSERT_TRUE(compacted.has_value()) << (compacted.has_value() ? std::string{} : compacted.error().to_string());
  EXPECT_EQ(compacted->tracks_compacted, 3u);
  ASSERT_EQ(compacted->placements.size(), 3u);

  // compact() -> Catalog::mark_compacted(): the integration leg no prior
  // task wired into production (track_compact.cpp's D5 doc comment) --
  // exercised here directly against the SAME catalog run_sweep populated.
  {
    auto catalog = Catalog::open(lake_root.string());
    ASSERT_TRUE(catalog.has_value());
    mark_all_compacted(*catalog, compacted->placements);
    for (const auto &outcome : swept->variants) {
      auto row = catalog->probe(outcome.key);
      ASSERT_TRUE(row.has_value());
      ASSERT_TRUE(row->has_value());
      EXPECT_EQ((*row)->status, TrackStatus::Compacted);
      ASSERT_TRUE((*row)->file.has_value());
      EXPECT_EQ(*(*row)->file,
               "tracks/underlier=SPX/family=strangle_compact_test/batch-000000.parquet");
      ASSERT_TRUE((*row)->row_group.has_value());
      EXPECT_EQ(*(*row)->row_group, 0);
    }
  }

  // Reload the REAL Parquet batch via Arrow C++ -- values match the
  // original in-memory BacktestResults to the double.
  const fs::path batch_path =
      lake_root / "tracks" / "underlier=SPX" / "family=strangle_compact_test" / "batch-000000.parquet";
  ASSERT_TRUE(fs::exists(batch_path)) << batch_path.string();
  const std::shared_ptr<arrow::Table> reloaded = read_batch_table(batch_path.string());
  ASSERT_TRUE(reloaded != nullptr);

  std::int64_t expected_rows = 0;
  for (const auto &outcome : swept->variants) {
    ASSERT_TRUE(outcome.result.has_value());
    expected_rows += static_cast<std::int64_t>(outcome.result->size());
  }
  ASSERT_EQ(reloaded->num_rows(), expected_rows);

  for (const auto &outcome : swept->variants) {
    const BacktestResult &original = *outcome.result;
    const std::string hex = outcome.key.hex();
    // Rows are sorted (track_key, date) (compact()'s own contract) -- find
    // this track's first row, then walk forward contiguously.
    std::int64_t first_row = -1;
    for (std::int64_t r = 0; r < reloaded->num_rows(); ++r) {
      if (str_at(*reloaded, "track_key", r) == hex) {
        first_row = r;
        break;
      }
    }
    ASSERT_GE(first_row, 0) << "track " << hex << " missing from the reloaded batch";
    for (std::size_t i = 0; i < original.nav.size(); ++i) {
      const std::int64_t row = first_row + static_cast<std::int64_t>(i);
      EXPECT_EQ(dbl_at(*reloaded, "nav", row), original.nav[i]) << "track " << hex << " row " << i;
      EXPECT_EQ(dbl_at(*reloaded, "cash", row), original.cash[i]) << "track " << hex << " row " << i;
      EXPECT_EQ(dbl_at(*reloaded, "pnl_total", row), original.pnl_total[i])
          << "track " << hex << " row " << i;
    }
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ── D5 INHERITED OBLIGATION (D4 review): real C++ writer -> Python reader ──
//
// D4's own tests only ever read Python-built fixture lakes (test_tracks.py's
// own doc comment: "No compiled C++ artifact that produces a lake exists in
// this worktree yet"). This test closes that gap from the WRITER side: it
// runs a small real sweep + compact() + mark_compacted() into a directory
// named by ATX_VOL_D5_FIXTURE_LAKE, plus a CSV side-car of the exact
// (track_key, date, nav) values this process computed in memory. Skipped
// (GTEST_SKIP, not failed) when the env var is unset -- this is a fixture
// PRODUCER for the paired pytest
// (python/tests/test_tracks_cxx_fixture.py), not a self-contained gate; see
// that file's own doc comment for how to run the pair manually.
TEST(SweepDriverTest, WritesRealCxxLakeAndCsvSidecarForPythonReadCrossCheck) {
  const std::string fixture_dir_str = fixture_lake_env();
  if (fixture_dir_str.empty()) {
    GTEST_SKIP() << "set " << kFixtureLakeEnv
                 << " to a directory to produce a real C++-written lake for "
                    "python/tests/test_tracks_cxx_fixture.py to read; skipped otherwise.";
  }
  const fs::path lake_root = fs::path(fixture_dir_str);
  std::error_code rm_ec;
  fs::remove_all(lake_root, rm_ec);

  const fs::path dir = fresh_dir("fixture-corpus");
  const Corpus corpus = make_corpus(dir / "corpus", "SPY", 5);

  SweepSpec spec;
  spec.variants = {
      make_variant(1.0, 100u),
      make_variant(-1.0, 150u),
  };
  spec.clock = corpus.clock;
  spec.uid = kUid;
  spec.meta = TrackMeta{"SPY", "d5_fixture"};
  spec.data_snapshot_id = fixed_snapshot_id(0x99);

  SweepConfig config;
  config.lake_root = lake_root.string();
  config.sweep_id = "sweep-d5-fixture";
  config.n_threads = 1;

  auto swept = run_sweep(spec, config);
  ASSERT_TRUE(swept.has_value()) << (swept.has_value() ? std::string{} : swept.error().to_string());
  ASSERT_EQ(swept->engine_runs, 2u);

  auto compacted = compact(lake_root.string());
  ASSERT_TRUE(compacted.has_value()) << (compacted.has_value() ? std::string{} : compacted.error().to_string());

  {
    auto catalog = Catalog::open(lake_root.string());
    ASSERT_TRUE(catalog.has_value());
    mark_all_compacted(*catalog, compacted->placements);
  }

  // CSV side-car: track_key,date,nav -- exactly what the pytest asserts
  // atxpy.tracks.load() returns for these tracks. setprecision(17) with the
  // default (neither fixed nor scientific) float format is the standard
  // round-trip-safe rendering of an IEEE754 binary64 -- "matches to the
  // double" on the Python side means an exact re-parse, not an approximation.
  const fs::path sidecar = lake_root / "expected_navs.csv";
  std::ofstream out(sidecar, std::ios::trunc);
  ASSERT_TRUE(out.good()) << sidecar.string();
  out << "track_key,date,nav\n";
  out << std::setprecision(17);
  for (const auto &outcome : swept->variants) {
    ASSERT_TRUE(outcome.result.has_value());
    const BacktestResult &r = *outcome.result;
    const std::string hex = outcome.key.hex();
    for (std::size_t i = 0; i < r.size(); ++i) {
      out << hex << "," << r.date[i] << "," << r.nav[i] << "\n";
    }
  }
  out.flush();
  ASSERT_TRUE(out.good()) << "failed writing " << sidecar.string();
  out.close();

  std::printf("D5 fixture lake written to: %s (sweep_id=%s, %llu track(s) compacted)\n",
              lake_root.string().c_str(), config.sweep_id.c_str(),
              static_cast<unsigned long long>(compacted->tracks_compacted));

  fs::remove_all(dir, rm_ec); // corpus scratch only -- lake_root is the deliverable, left in place
}
