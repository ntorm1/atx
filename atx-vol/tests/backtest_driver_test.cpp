// atx-vol backtest_driver spine (Wave C T1) gate tests.
//
// `run_timed` is the one thing all five example drivers genuinely share: time the
// engine call, fold the result into a `TearSheet`, capture `EngineRunStats`. The
// drivers' emitted bytes depend on that seam being transparent, so these gates
// pin exactly the properties the byte goldens rest on:
//
//   1. `outcome.result` is the engine's return value MOVED, UNTRANSFORMED — all
//      25 F64 series columns (driven off `backtest_series_columns()`, the single
//      source both serializers iterate), plus `date`, `ts_ns`, `step_pnl_total`
//      and every signal series, bit-identical to an independently-invoked
//      `run_backtest` / `run_dispersion_backtest` over the same corpus. A
//      10-column subset check would pass while three emitted columns diverged.
//   2. `outcome.sheet == tearsheet(outcome.result)` in every one of the 27
//      `TearSheet` fields — `sizeof`-guarded, so a newly added field cannot
//      silently escape the comparison.
//   3. `outcome.stats` carries `n_steps == result.size()`, a positive wall clock,
//      and the shared `SnapshotCache`'s 8 counters — or a zeroed
//      `SnapshotCacheStats` when no cache is supplied (two of the five drivers
//      run cacheless, and that path must not crash).
//
// Fixtures are the local synthetic-eSSVI corpus trio (the `tearsheet_test.cpp`
// pattern: analytic surfaces, no fitting, no external data); the dispersion
// corpus mirrors `examples/dispersion_backtest.cpp:123-145` (IDX/NM0/NM1,
// weights 0.6/0.4, min_names=2, record_diagnostics=true) so the seam is pinned
// over the same shape the driver runs.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"                // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"                // Clock, run_backtest, RunConfig, SnapshotCache
#include "atx/vol/backtest_driver.hpp"         // RunOutcome, run_timed  (the seam under test)
#include "atx/vol/backtest_series_columns.hpp" // backtest_series_columns() (all 25 F64 columns)
#include "atx/vol/corpus.hpp"                  // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/dispersion.hpp"              // DispersionUniverse, DispersionMember
#include "atx/vol/dispersion_backtest.hpp"     // DispersionBacktestConfig, run_dispersion_backtest
#include "atx/vol/priced_surface.hpp"          // PricedSurface, PricingContext
#include "atx/vol/run_report.hpp"              // EngineRunStats
#include "atx/vol/strategy.hpp"                // DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp"         // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"          // SliceContext
#include "atx/vol/tearsheet.hpp"               // TearSheet, tearsheet
#include "atx/vol/types.hpp"                   // Side, Result, Status
#include "atx/vol/vol_curve.hpp"               // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"             // EssviParams

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

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors tearsheet_test's make_surface.
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
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-backtest-driver-") + tag);
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
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths,
              const std::string &symbol = "MKT") {
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

// A single-underlying evolving corpus (spot drifts, valuation advances a day).
[[nodiscard]] CorpusManifest single_name_corpus(const fs::path &dir, const std::string &symbol,
                                                int n_dates) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = 100.0 * (1.0 + 0.004 * static_cast<double>(d));
    const PricedSurface s = make_surface(kUid, S, S, now, 0.0008 * static_cast<double>(d));
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_archive(dir, date, {{symbol, &s}}));
  }
  return make_manifest(dp, symbol);
}

// The `examples/dispersion_backtest.cpp:123-135` corpus: an index + two
// constituents per date, 5 dates 5 calendar days apart.
[[nodiscard]] CorpusManifest dispersion_corpus(const fs::path &dir) {
  const std::vector<int> day_off = {0, 5, 10, 15, 20};
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < day_off.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(day_off[d]) * kDayNs;
    const double drift = 1.0 + 0.001 * static_cast<double>(day_off[d]);
    const PricedSurface idx = make_surface(1, 500.0 * drift, 500.0 * drift, now, 0.00);
    const PricedSurface n0 = make_surface(2, 100.0 * drift, 100.0 * drift, now, 0.02);
    const PricedSurface n1 = make_surface(3, 120.0 * drift, 120.0 * drift, now, 0.03);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-10-%02d", static_cast<int>(d + 1));
    dp.emplace_back(std::string(buf),
                    write_archive(dir, buf, {{"IDX", &idx}, {"NM0", &n0}, {"NM1", &n1}}));
  }
  return make_manifest(dp);
}

// The design's Worked Example A (tearsheet_test.cpp): 3m 25-delta put, a new
// clip every step, held to expiry, delta-hedged to zero daily.
[[nodiscard]] StrategySpec worked_example_a_spec() {
  StrategySpec spec;
  spec.name = "spy-3m-25d-put-daily-clip";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::Delta, 0.25};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  return spec;
}

[[nodiscard]] DispersionUniverse dispersion_universe() {
  DispersionUniverse u;
  u.index = DispersionMember{"IDX", 1, 0.0};
  u.names.push_back(DispersionMember{"NM0", 2, 0.6});
  u.names.push_back(DispersionMember{"NM1", 3, 0.4});
  return u;
}

// EVERY series column of two BacktestResults is bit-identical: `date`, `ts_ns`,
// all 25 F64 columns by name off the single-source table, the full-resolution
// `step_pnl_total` series, and every signal (name AND value, in order).
void expect_result_bit_identical(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  ASSERT_GT(a.size(), 1u) << "a vacuous (0/1-row) result would make this gate untestable";
  for (const BacktestSeriesColumn &col : backtest_series_columns()) {
    const std::vector<double> &va = a.*(col.member);
    const std::vector<double> &vb = b.*(col.member);
    ASSERT_EQ(va.size(), a.size()) << col.name;
    ASSERT_EQ(vb.size(), b.size()) << col.name;
    for (std::size_t i = 0; i < va.size(); ++i) {
      EXPECT_TRUE(bits_equal(va[i], vb[i])) << col.name << " row " << i;
    }
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    EXPECT_EQ(a.ts_ns[i], b.ts_ns[i]) << i;
  }
  ASSERT_EQ(a.step_pnl_total.size(), b.step_pnl_total.size());
  for (std::size_t k = 0; k < a.step_pnl_total.size(); ++k) {
    EXPECT_TRUE(bits_equal(a.step_pnl_total[k], b.step_pnl_total[k])) << "step_pnl_total " << k;
  }
  ASSERT_EQ(a.signals.size(), b.signals.size());
  for (std::size_t s = 0; s < a.signals.size(); ++s) {
    EXPECT_EQ(a.signals[s].first, b.signals[s].first) << "signal " << s;
    ASSERT_EQ(a.signals[s].second.size(), b.signals[s].second.size()) << a.signals[s].first;
    for (std::size_t i = 0; i < a.signals[s].second.size(); ++i) {
      EXPECT_TRUE(bits_equal(a.signals[s].second[i], b.signals[s].second[i]))
          << a.signals[s].first << " row " << i;
    }
  }
}

// Every TearSheet field, in declaration order. The sizeof guard below makes a
// newly added field a compile error rather than a silently unchecked one.
#define ATX_TEARSHEET_FIELDS(X)                                                                    \
  X(total_return) X(ann_return) X(ann_vol) X(sharpe) X(max_drawdown) X(hit_rate) X(avg_turnover)    \
      X(total_cost) X(total_financing) X(attr_delta) X(attr_gamma) X(attr_vega) X(attr_vanna)       \
          X(attr_volga) X(attr_theta) X(attr_rho) X(attr_charm) X(attr_unexplained)                \
              X(attr_settlement) X(attr_shares) X(attr_financing) X(attr_cost)                     \
                  X(return_on_gross_vega) X(vega_adj_sharpe) X(pnl_per_vega_traded)                \
                      X(avg_gross_vega) X(avg_gross_gamma)

static_assert(sizeof(TearSheet) == 27 * sizeof(double),
              "TearSheet gained/lost a field — update ATX_TEARSHEET_FIELDS or this gate stops "
              "covering the whole struct");

void expect_sheet_bit_identical(const TearSheet &a, const TearSheet &b) {
#define ATX_TEARSHEET_EXPECT(f) EXPECT_TRUE(bits_equal(a.f, b.f)) << #f;
  ATX_TEARSHEET_FIELDS(ATX_TEARSHEET_EXPECT)
#undef ATX_TEARSHEET_EXPECT
}

// Guards against a 0 == 0 pass: at least one field must be a real number.
[[nodiscard]] bool sheet_is_nondegenerate(const TearSheet &t) noexcept {
  bool any = false;
#define ATX_TEARSHEET_ANY(f) any = any || (t.f != 0.0);
  ATX_TEARSHEET_FIELDS(ATX_TEARSHEET_ANY)
#undef ATX_TEARSHEET_ANY
  return any;
}

// Whole-file bytes, opened binary so nothing normalises line endings.
[[nodiscard]] std::string read_file_bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void expect_cache_stats_equal(const SnapshotCacheStats &a, const SnapshotCacheStats &b) {
  // SnapshotCacheStats has no operator== (backtest.hpp) — compare all 8 fields.
  EXPECT_EQ(a.loads, b.loads);
  EXPECT_EQ(a.hits, b.hits);
  EXPECT_EQ(a.prefetches, b.prefetches);
  EXPECT_EQ(a.retained_entries, b.retained_entries);
  EXPECT_EQ(a.evictions, b.evictions);
  EXPECT_EQ(a.fast_build_loads, b.fast_build_loads);
  EXPECT_EQ(a.reuse_only_fast_hits, b.reuse_only_fast_hits);
  EXPECT_EQ(a.reuse_only_cold_resolutions, b.reuse_only_cold_resolutions);
}

} // namespace

// ── 1. The IStrategy overload forwards the engine's result untransformed ─────
TEST(BacktestDriver, RunTimed_ResultIsBitIdenticalToRunBacktest) {
  const fs::path dir = fresh_dir("bit-identity");
  const CorpusManifest manifest = single_name_corpus(dir, "SPY", 12);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const StrategySpec spec = worked_example_a_spec();
  DeclarativeStrategy s1{spec};
  DeclarativeStrategy s2{spec};
  RunConfig cfg;
  cfg.price.n_threads = 1;

  auto baseline = run_backtest(*clock, s1, cfg);   // the engine, called directly
  auto outcome = run_timed(*clock, s2, cfg);       // the same engine, through the spine
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();
  ASSERT_TRUE(outcome.has_value()) << outcome.error().to_string();

  expect_result_bit_identical(*baseline, outcome->result);
  std::printf("[backtest_driver] bit-identity: %zu rows x %zu F64 columns + date/ts_ns/step_pnl\n",
              outcome->result.size(), backtest_series_columns().size());
}

// ── 2. `sheet` is exactly tearsheet(result) ─────────────────────────────────
TEST(BacktestDriver, RunTimed_SheetEqualsTearsheetOfResult) {
  const fs::path dir = fresh_dir("sheet");
  const CorpusManifest manifest = single_name_corpus(dir, "SPY", 12);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy strat{worked_example_a_spec()};
  RunConfig cfg;
  cfg.price.n_threads = 1;
  auto outcome = run_timed(*clock, strat, cfg);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().to_string();
  ASSERT_GT(outcome->result.size(), 1u);

  EXPECT_TRUE(sheet_is_nondegenerate(outcome->sheet))
      << "an all-zero sheet would make this comparison vacuous";
  expect_sheet_bit_identical(outcome->sheet, tearsheet(outcome->result));
  std::printf("[backtest_driver] sheet: total_return=%.6f sharpe=%.6f avg_gross_vega=%.4f\n",
              outcome->sheet.total_return, outcome->sheet.sharpe, outcome->sheet.avg_gross_vega);
}

// ── 3. Stats capture: steps, a real wall clock, and the shared cache counters ─
TEST(BacktestDriver, RunTimed_StatsCaptureStepsAndCache) {
  const fs::path dir = fresh_dir("stats");
  const CorpusManifest manifest = single_name_corpus(dir, "SPY", 12);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy strat{worked_example_a_spec()};
  RunConfig cfg;
  cfg.price.n_threads = 1;
  cfg.snapshot_cache = std::make_shared<SnapshotCache>(); // mag7's shared-cache path
  auto outcome = run_timed(*clock, strat, cfg);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().to_string();

  EXPECT_EQ(outcome->stats.n_steps, static_cast<std::uint64_t>(outcome->result.size()));
  EXPECT_EQ(outcome->result.size(), manifest.dates.size());
  EXPECT_GT(outcome->stats.wall_clock_ms, 0.0);

  const SnapshotCacheStats end_of_run = cfg.snapshot_cache->stats();
  EXPECT_GT(end_of_run.loads, 0u) << "a cache with no traffic would make this gate vacuous";
  expect_cache_stats_equal(outcome->stats.cache, end_of_run);
  std::printf("[backtest_driver] stats: n_steps=%llu wall_ms=%.3f loads=%llu hits=%llu\n",
              static_cast<unsigned long long>(outcome->stats.n_steps),
              outcome->stats.wall_clock_ms,
              static_cast<unsigned long long>(outcome->stats.cache.loads),
              static_cast<unsigned long long>(outcome->stats.cache.hits));
}

// ── 4. The cacheless path (2 of 5 drivers): zeroed stats, no crash ──────────
TEST(BacktestDriver, RunTimed_NullCacheYieldsZeroedStats) {
  const fs::path dir = fresh_dir("null-cache");
  const CorpusManifest manifest = single_name_corpus(dir, "SPY", 12);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy strat{worked_example_a_spec()};
  const RunConfig cfg; // default => snapshot_cache == nullptr (a private per-run cache)
  ASSERT_EQ(cfg.snapshot_cache, nullptr);
  auto outcome = run_timed(*clock, strat, cfg);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().to_string();
  ASSERT_GT(outcome->result.size(), 1u);

  expect_cache_stats_equal(outcome->stats.cache, SnapshotCacheStats{});
  EXPECT_EQ(outcome->stats.n_steps, static_cast<std::uint64_t>(outcome->result.size()));
  EXPECT_GT(outcome->stats.wall_clock_ms, 0.0);
}

// ── 5. The dispersion overload: result AND signals survive the seam ─────────
TEST(BacktestDriver, RunTimedDispersion_ResultIsBitIdenticalToRunDispersionBacktest) {
  const fs::path dir = fresh_dir("dispersion");
  const CorpusManifest manifest = dispersion_corpus(dir);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionBacktestConfig config;
  config.min_names = 2u;
  config.record_diagnostics = true; // the driver's research output => signals
  config.run.price.n_threads = 1;

  auto baseline = run_dispersion_backtest(*clock, dispersion_universe(), config);
  auto outcome = run_timed(*clock, dispersion_universe(), config);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();
  ASSERT_TRUE(outcome.has_value()) << outcome.error().to_string();

  // `write_backtest_tsv` appends one column per signal, so a dropped, reordered
  // or copied-stale signal moves `dispersion.tsv`'s bytes.
  ASSERT_FALSE(baseline->signals.empty()) << "record_diagnostics=true must produce signals";
  ASSERT_FALSE(baseline->signals.front().second.empty());
  expect_result_bit_identical(*baseline, outcome->result);

  EXPECT_EQ(outcome->stats.n_steps, static_cast<std::uint64_t>(outcome->result.size()));
  EXPECT_GT(outcome->stats.wall_clock_ms, 0.0);
  expect_cache_stats_equal(outcome->stats.cache, SnapshotCacheStats{}); // config.run has no cache
  expect_sheet_bit_identical(outcome->sheet, tearsheet(outcome->result));
  std::printf("[backtest_driver] dispersion: %zu rows, %zu signal(s), first='%s'\n",
              outcome->result.size(), outcome->result.signals.size(),
              outcome->result.signals.front().first.c_str());
}

// ── 6. Wave C T3: the signals survive the seam all the way into the EMITTED
//      BYTES. `examples/dispersion_backtest.cpp` writes its golden artifact with
//      `write_backtest_tsv`, which appends one column per signal in
//      `r.signals` order — so a dropped, reordered or stale signal moves
//      `dispersion.tsv`. Test 5 above compares the in-memory doubles; this one
//      compares what actually reaches the file, which is what the T2 whole-file
//      golden `87DA84887A2793AE` is a hash of. Deliberately NOT a restatement:
//      it is the only gate here that runs the driver's serializer.
TEST(BacktestDriver, RunTimedDispersion_SignalsSurviveTheSeam) {
  const fs::path dir = fresh_dir("signals-tsv");
  const CorpusManifest manifest = dispersion_corpus(dir);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionBacktestConfig config; // the driver's config, verbatim
  config.min_names = 2u;
  config.record_diagnostics = true;
  config.run.price.n_threads = 1;

  auto baseline = run_dispersion_backtest(*clock, dispersion_universe(), config);
  auto outcome = run_timed(*clock, dispersion_universe(), config);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();
  ASSERT_TRUE(outcome.has_value()) << outcome.error().to_string();
  ASSERT_FALSE(outcome->result.signals.empty())
      << "no signals => this gate cannot see a signal regression";

  const std::string direct_path = (dir / "route_direct.tsv").string();
  const std::string seam_path = (dir / "route_run_timed.tsv").string();
  const Status st_direct = write_backtest_tsv(*baseline, direct_path);
  const Status st_seam = write_backtest_tsv(outcome->result, seam_path);
  ASSERT_TRUE(st_direct.has_value()) << st_direct.error().to_string();
  ASSERT_TRUE(st_seam.has_value()) << st_seam.error().to_string();

  const std::string direct_bytes = read_file_bytes(direct_path);
  const std::string seam_bytes = read_file_bytes(seam_path);
  ASSERT_GT(direct_bytes.size(), 0u);
  // Each signal must be a real column in the header, else "identical" would be
  // two files that both lost the same signal.
  const std::string header = direct_bytes.substr(0, direct_bytes.find('\n'));
  for (const auto &sig : outcome->result.signals) {
    EXPECT_NE(header.find(sig.first), std::string::npos) << "signal column: " << sig.first;
  }
  ASSERT_EQ(direct_bytes.size(), seam_bytes.size()) << "emitted TSV lengths differ";
  EXPECT_TRUE(direct_bytes == seam_bytes) << "run_timed's result serialises to different bytes";
  std::printf("[backtest_driver] emitted TSV: %zu bytes identical both routes, %zu signal col(s)\n",
              direct_bytes.size(), outcome->result.signals.size());
}
