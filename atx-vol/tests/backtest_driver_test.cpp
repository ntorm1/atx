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

#include <algorithm>
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

#include "atx/vol/api/pricing/american.hpp"                // al_fast_opts, AmericanMethod
#include "atx/vol/api/backtest/backtest.hpp"                // Clock, run_backtest, RunConfig, SnapshotCache
#include "atx/vol/research/backtest_driver.hpp"         // RunOutcome, run_timed  (the seam under test)
#include "backtest/backtest_series_columns.hpp" // backtest_series_columns() (all 25 F64 columns)
#include "atx/vol/api/marketdata/corpus.hpp"                  // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/api/backtest/dispersion.hpp"              // DispersionUniverse, DispersionMember
#include "atx/vol/research/dispersion_backtest.hpp"     // DispersionBacktestConfig, run_dispersion_backtest
#include "atx/vol/api/backtest/priced_surface.hpp"          // PricedSurface, PricingContext
#include "atx/vol/tools/run_report.hpp"              // EngineRunStats
#include "atx/vol/api/backtest/strategy.hpp"                // DeclarativeStrategy, StrategySpec
#include "atx/vol/api/storage/surface_archive.hpp"         // write_surface_archive_v2_file
#include "atx/vol/api/fitting/surface_parity.hpp"          // SliceContext
#include "atx/vol/tools/tearsheet.hpp"               // TearSheet, tearsheet
#include "atx/vol/api/core/types.hpp"                   // Side, Result, Status
#include "atx/vol/api/fitting/vol_curve.hpp"               // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"             // EssviParams

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
                      X(avg_gross_vega) X(avg_gross_gamma) X(return_on_margin)                     \
                          X(margin_utilization_peak)

// The trunk's WS-X5 added a nested benchmark-relative block to TearSheet, which
// this guard correctly caught at the main->pipeline-m merge (it fired as
// "expression evaluates to '280 == 216'"). Its six doubles are enumerated
// separately because the two macros above are double-only: `has_benchmark`
// (bool) and `n_obs` (size_t) are compared explicitly in the comparator below
// rather than being folded in and silently converted.
#define ATX_TEARSHEET_BENCHMARK_FIELDS(X)                                                          \
  X(benchmark.beta) X(benchmark.alpha) X(benchmark.active_return)                                  \
      X(benchmark.tracking_error) X(benchmark.information_ratio) X(benchmark.correlation)

// Task B2 (backtest-lakehouse sprint) added `return_on_margin` and
// `margin_utilization_peak`, so this grew 27 -> 29 plain doubles (232 B) +
// BenchmarkStats (bool + pad + size_t + 6 doubles = 64 B) = 296 B = 37
// doubles. Update BOTH field lists, not just this number, or the gate stops
// covering the whole struct.
static_assert(sizeof(TearSheet) == 37 * sizeof(double),
              "TearSheet gained/lost a field — update ATX_TEARSHEET_FIELDS / "
              "ATX_TEARSHEET_BENCHMARK_FIELDS or this gate stops covering the whole struct");
static_assert(sizeof(BenchmarkStats) == 8 * sizeof(double),
              "BenchmarkStats gained/lost a field — update ATX_TEARSHEET_BENCHMARK_FIELDS");

void expect_sheet_bit_identical(const TearSheet &a, const TearSheet &b) {
#define ATX_TEARSHEET_EXPECT(f) EXPECT_TRUE(bits_equal(a.f, b.f)) << #f;
  ATX_TEARSHEET_FIELDS(ATX_TEARSHEET_EXPECT)
  ATX_TEARSHEET_BENCHMARK_FIELDS(ATX_TEARSHEET_EXPECT)
#undef ATX_TEARSHEET_EXPECT
  EXPECT_EQ(a.benchmark.has_benchmark, b.benchmark.has_benchmark) << "benchmark.has_benchmark";
  EXPECT_EQ(a.benchmark.n_obs, b.benchmark.n_obs) << "benchmark.n_obs";
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

// A COPY of `spy_dispersion_pnl.cpp:57-61`'s `fmt_num` — the rendering that
// reaches the artifact's meta prelude. A copy, not the driver's function: if the
// driver's `%.10g` ever changes, this helper does NOT follow and no assertion
// here notices (see test 7's "WHAT THIS TEST DOES *NOT* COVER").
[[nodiscard]] std::string fmt_num_10g(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

// A COPY (same caveat as `fmt_num_10g`) of the 11 result/tearsheet-derived meta
// keys `spy_dispersion_pnl.cpp:502,515-524` inlines into `pnl_track.tsv`, in the
// driver's order. The two further derived keys it embeds — `wall_clock_ms` and
// `steps_per_s` (`:525-526`) — are omitted here for the same reason T2's golden
// filters them: they are wall-clock and cannot be equal across two runs. The
// other 30 `Meta` keys are identity/config strings that do not touch the seam.
// This is a pure function of `r.size()`, `r.n_open_lots` and 9 `TearSheet`
// fields — every one of which tests 1 and 2 already pin bit-for-bit.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
pnl_meta_block(const BacktestResult &r, const TearSheet &ts) {
  const double peak_lots =
      r.size() ? *std::max_element(r.n_open_lots.begin(), r.n_open_lots.end()) : 0.0;
  return {
      {"n_steps", std::to_string(r.size())},
      {"total_return", fmt_num_10g(ts.total_return)},
      {"ann_return", fmt_num_10g(ts.ann_return)},
      {"ann_vol", fmt_num_10g(ts.ann_vol)},
      {"sharpe", fmt_num_10g(ts.sharpe)},
      {"max_drawdown", fmt_num_10g(ts.max_drawdown)},
      {"hit_rate", fmt_num_10g(ts.hit_rate)},
      {"avg_gross_vega", fmt_num_10g(ts.avg_gross_vega)},
      {"avg_gross_gamma", fmt_num_10g(ts.avg_gross_gamma)},
      {"return_on_gross_vega", fmt_num_10g(ts.return_on_gross_vega)},
      {"peak_open_lots", fmt_num_10g(peak_lots)},
  };
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

// ── 7. Wave C T5: `write_backtest_pnl_tsv`'s emitted bytes. TWO assertions of
//      VERY different strength — read the split before trusting this test.
//
//      (a) Two-route equality (`direct_bytes == seam_bytes`) is a REGRESSION
//          LOCK, and it is STRICTLY IMPLIED by tests 1+2.
//          `write_backtest_pnl_tsv` is `append_meta_header(meta)` +
//          `append_backtest_series_tsv(r)` (`src/tearsheet.cpp:281-288`), and the
//          series appender (`:190-233`) reads ONLY `r.date`, `r.ts_ns`, the 25
//          `backtest_series_columns()` doubles and `r.signals` — precisely the
//          set test 1 already compares bit-for-bit on this same fixture, spec and
//          `RunConfig`. `pnl_meta_block` is a pure function of `r.size()`,
//          `r.n_open_lots` and 9 `TearSheet` fields, pinned by tests 1+2. So (a)
//          is a theorem given tests 1+2, NOT new coverage. Kept because it is
//          the cheapest end-to-end statement of the seam contract and it fails
//          loudly if either premise is ever weakened.
//
//      (b) The PRELUDE FRAMING pin is this test's real, not-implied
//          contribution. A change to `append_meta_header`
//          (`src/tearsheet.cpp:244-256`) — `# k=v` becoming `#k=v` or `k,v`, a
//          sorted prelude, a blank line before the series header, or a tab in a
//          value surviving into the `\t` body — moves BOTH routes identically, so
//          (a) is blind to it, and `write_backtest_pnl_tsv` has no other C++ test
//          anywhere in the tree (only Python reads it). (b) pins that framing
//          against a literal.
//
//      WHAT THIS TEST DOES *NOT* COVER, stated plainly because an earlier
//      version of this comment over-claimed it: `fmt_num_10g` and
//      `pnl_meta_block` are COPIES of `spy_dispersion_pnl.cpp:57-61` and
//      `:485-527`, not the driver's own code. Change the driver's `fmt_num` to
//      `%.9g`, or delete `{"sharpe", fmt_num(ts.sharpe)}` from its `Meta`
//      literal, and the artifact convention this task exists to preserve changes
//      while this test still PASSES. The only thing that catches that is T2's
//      filtered hash `CC90B900A7116CC3`, which lives in session scratchpad and is
//      NOT committed. The driver-side `Meta` key set and its precision are
//      pinned by review plus that hash — not here.
//
//      Plan-name mapping: the plan asked for
//      `RunTimed_SheetFieldsAreBitEqualUnderFmtNum` = snprintf `outcome.sheet`
//      and `tearsheet(outcome.result)` at `%.10g` and compare the strings. That
//      is strictly implied by test 2 (`%.10g` of two bit-equal doubles is equal
//      by construction), so it was rescoped rather than written as a duplicate.
//
//      RESOLUTION LIMIT (measured; it corrects the task text): `%.10g` ABSORBS a
//      1-ULP tearsheet difference — `nextafter(387.1141627)` renders
//      byte-identically — so this prelude cannot witness a 1-ULP sheet change.
//      That guarantee comes from test 2's bit comparison, not from this artifact
//      (the `%.17g` series body carries no tearsheet value). A relative 1e-9
//      nudge of `outcome->sheet.total_return` DOES move the bytes and was run as
//      (a)'s negative control: `direct_bytes == seam_bytes` failed at equal
//      length (4895 both sides), i.e. the byte assertion and not the size
//      precheck is what catches it.
TEST(BacktestDriver, RunTimed_PnlTrackBytesIdenticalBothRoutes) {
  const fs::path dir = fresh_dir("pnl-meta");
  const CorpusManifest manifest = single_name_corpus(dir, "SPY", 12);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const StrategySpec spec = worked_example_a_spec();
  DeclarativeStrategy s1{spec};
  DeclarativeStrategy s2{spec};
  RunConfig cfg;
  cfg.price.n_threads = 1;

  auto baseline = run_backtest(*clock, s1, cfg); // the engine, called directly
  auto outcome = run_timed(*clock, s2, cfg);     // the same engine, through the spine
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();
  ASSERT_TRUE(outcome.has_value()) << outcome.error().to_string();
  EXPECT_TRUE(sheet_is_nondegenerate(outcome->sheet))
      << "an all-zero sheet would make this comparison vacuous";

  const auto meta_direct = pnl_meta_block(*baseline, tearsheet(*baseline));
  const auto meta_seam = pnl_meta_block(outcome->result, outcome->sheet);

  const std::string direct_path = (dir / "route_direct_pnl.tsv").string();
  const std::string seam_path = (dir / "route_run_timed_pnl.tsv").string();
  const Status st_direct = write_backtest_pnl_tsv(*baseline, meta_direct, direct_path);
  const Status st_seam = write_backtest_pnl_tsv(outcome->result, meta_seam, seam_path);
  ASSERT_TRUE(st_direct.has_value()) << st_direct.error().to_string();
  ASSERT_TRUE(st_seam.has_value()) << st_seam.error().to_string();

  const std::string direct_bytes = read_file_bytes(direct_path);
  const std::string seam_bytes = read_file_bytes(seam_path);
  ASSERT_GT(direct_bytes.size(), 0u);
  // Anti-vacuity: every key must really be in the prelude with its rendered
  // value, else two files that both dropped the same key compare "identical".
  for (const auto &kv : meta_direct) {
    EXPECT_NE(direct_bytes.find("# " + kv.first + "=" + kv.second + "\n"), std::string::npos)
        << "meta key missing from the emitted prelude: " << kv.first;
  }
  ASSERT_EQ(direct_bytes.size(), seam_bytes.size()) << "emitted PnL-track lengths differ";
  EXPECT_TRUE(direct_bytes == seam_bytes) << "the seam moved the PnL-track bytes";

  // ── (b) The prelude framing pin: the part of this test NOT implied by tests
  //    1+2. Keys are deliberately NOT alphabetical, because a sorted prelude
  //    would reorder the driver's 41 keys and move the artifact while (a) stayed
  //    green. One value is a `%.10g` render, so the artifact's
  //    10-significant-digit convention is asserted against a literal in a
  //    COMMITTED test rather than existing only inside the driver. One value
  //    carries a TAB, which `append_sanitized` (`src/tearsheet.cpp:238-242`) must
  //    turn into a space or a meta value could inject a spurious column into the
  //    `\t`-separated body that follows.
  const std::vector<std::pair<std::string, std::string>> framing_meta = {
      {"zeta", "1"},
      {"alpha", fmt_num_10g(1.0 / 3.0)},
      {"mid", "two\twords"},
  };
  const std::string kPrelude = "# zeta=1\n# alpha=0.3333333333\n# mid=two words\n";
  const std::string kSeriesHead = "date\tts_ns\t";
  const std::string framing_path = (dir / "framing_pnl.tsv").string();
  const Status st_framing = write_backtest_pnl_tsv(*baseline, framing_meta, framing_path);
  ASSERT_TRUE(st_framing.has_value()) << st_framing.error().to_string();
  const std::string framing_bytes = read_file_bytes(framing_path);
  ASSERT_GT(framing_bytes.size(), kPrelude.size() + kSeriesHead.size())
      << "no series body after the prelude — an empty file would make this pin vacuous";
  EXPECT_EQ(framing_bytes.compare(0, kPrelude.size(), kPrelude), 0)
      << "prelude framing moved. expected [" << kPrelude << "] got ["
      << framing_bytes.substr(0, kPrelude.size()) << "]";
  EXPECT_EQ(framing_bytes.compare(kPrelude.size(), kSeriesHead.size(), kSeriesHead), 0)
      << "the series header must start immediately after the last `# k=v` line, got ["
      << framing_bytes.substr(kPrelude.size(), kSeriesHead.size()) << "]";

  std::printf("[backtest_driver] pnl_track: %zu bytes identical both routes, %zu meta key(s), "
              "total_return=%s peak_open_lots=%s; prelude framing pinned (%zu bytes)\n",
              direct_bytes.size(), meta_seam.size(), meta_seam[1].second.c_str(),
              meta_seam.back().second.c_str(), kPrelude.size());
}
