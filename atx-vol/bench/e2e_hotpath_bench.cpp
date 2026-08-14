// Canonical real-OPRA fit -> value -> snapshot and archive -> backtest benches.
// Fixture I/O is preloaded; the fit rows time the public production facade from
// OptionChain::from_frame onward. Their value stage requests production model
// marks only; full Prices|Bands is a separate accuracy diagnostic because three
// market-IV inversions per quote are not part of the backtest path. Backtest rows
// always publish cold-oracle economic deltas beside speed so an approximate route
// cannot win on time alone.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/backtest/backtest.hpp"
#include "atx/vol/api/core/chain.hpp"
#include "atx/vol/api/marketdata/corpus.hpp"
#include "fitting/counters.hpp"       // counters::timing — stage attribution (M3)
#include "atx/vol/api/marketdata/data.hpp"           // year_fraction (synthetic attribution panel)
#include "atx/vol/api/backtest/panel.hpp"          // SynthPanelSpec, make_synthetic_american_panel
#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface (deserialized price stage)
#include "atx/vol/api/fitting/pricer_fitter.hpp"
#include "atx/vol/api/backtest/query_pricing.hpp"
#include "atx/vol/api/fitting/s3.hpp"             // S3Params (synthetic truth)
#include "atx/vol/api/backtest/strategy.hpp"
#include "atx/vol/api/storage/surface_archive.hpp" // write_surface_archive_v2 / SurfaceArchiveV2
#include "atx/vol/api/core/types.hpp"           // Side

#include "bench_util.hpp"
#include "support/opra_fixture.hpp"
#include "support/spy_fit_fixture.hpp"

namespace atx::vol::bench {
namespace {

namespace fs = std::filesystem;
using StageClock = std::chrono::steady_clock;

constexpr double kRate = 0.043;
constexpr std::string_view kUniverseSnapshot = "2026-07-01T19:55:00Z";

[[nodiscard]] double elapsed_ms(StageClock::time_point start) noexcept {
  return std::chrono::duration<double, std::milli>(StageClock::now() - start).count();
}

// A0 (WS-3) measurement knob — NOT a production default. The de-Am prepass
// (curve_fit.cpp:run_deam_prepass / essvi_calib.cpp) fans the per-expiry work
// across `PricerConfig::fit_workers`; the canonical gate pins it to 1 (serial),
// which is what the 347 ms SPY / 272.9 ms-per-board de-Am number was measured at.
// This env override lets the wall-vs-CPU disambiguation sweep fit_workers>1
// WITHOUT touching the production/gate default (unset => 1, byte-unchanged gate).
// 0 => hardware_concurrency (matches the fit_workers=0 auto semantics).
// The value is read with _dupenv_s under MSVC/clang-cl: plain std::getenv trips
// /WX (-Wdeprecated-declarations) here — matches parallel_for.hpp's env-read.
[[nodiscard]] unsigned bench_fit_workers() noexcept {
  constexpr unsigned kGateDefault = 1u; // canonical gate — production unchanged.
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, "ATX_BENCH_FIT_WORKERS") != 0 || raw == nullptr) {
    return kGateDefault;
  }
#else
  const char *raw = std::getenv("ATX_BENCH_FIT_WORKERS");
  if (raw == nullptr || raw[0] == '\0') {
    return kGateDefault;
  }
#endif
  const char *end = raw;
  while (*end != '\0') {
    ++end;
  }
  unsigned value = 0u;
  const auto res = std::from_chars(raw, end, value);
  // Accept 0 (=> hardware_concurrency auto) through 1024; else fall back.
  const bool ok = (res.ec == std::errc{}) && (res.ptr == end) && (value <= 1024u);
#if defined(_MSC_VER)
  std::free(raw);
#endif
  return ok ? value : kGateDefault;
}

[[nodiscard]] fs::path first_existing(std::initializer_list<fs::path> candidates) {
  for (const fs::path &candidate : candidates) {
    std::error_code error;
    if (fs::exists(candidate, error) && !error) {
      return candidate;
    }
  }
  return {};
}

struct FitCorpus {
  std::vector<testkit::OpraBoard> boards{};
  std::size_t declared_boards{0u};
  std::size_t fixture_failures{0u};
  std::string error{};
};

[[nodiscard]] FitCorpus load_spy_fit_corpus() {
  FitCorpus corpus;
  corpus.declared_boards = 1u;
  std::optional<testkit::OpraBoard> board =
      testkit::load_spy_fit_fixture(testkit::kSpyFitFixtures[0], kRate);
  if (!board.has_value()) {
    corpus.fixture_failures = 1u;
    corpus.error = "real SPY fit fixture is unavailable";
    return corpus;
  }
  corpus.boards.push_back(std::move(*board));
  return corpus;
}

[[nodiscard]] FitCorpus load_universe_fit_corpus() {
  FitCorpus corpus;
  const fs::path list_path =
      first_existing({"data/universe/smoke100.txt", "../data/universe/smoke100.txt",
                      "../../data/universe/smoke100.txt", "C:/atx/data/universe/smoke100.txt"});
  if (list_path.empty()) {
    corpus.error = "100-name universe list is unavailable";
    return corpus;
  }

  std::ifstream symbols{list_path};
  std::string symbol;
  while (std::getline(symbols, symbol)) {
    if (symbol.empty()) {
      continue;
    }
    ++corpus.declared_boards;
    const fs::path parquet =
        first_existing({fs::path{"data/opra_universe"} / symbol / "2026-07-01.parquet",
                        fs::path{"../data/opra_universe"} / symbol / "2026-07-01.parquet",
                        fs::path{"../../data/opra_universe"} / symbol / "2026-07-01.parquet",
                        fs::path{"C:/atx/data/opra_universe"} / symbol / "2026-07-01.parquet"});
    std::optional<testkit::OpraBoard> board = testkit::load_opra_board_path(
        parquet.generic_string(), symbol, std::string{kUniverseSnapshot}, kRate);
    if (!board.has_value()) {
      ++corpus.fixture_failures;
      continue;
    }
    corpus.boards.push_back(std::move(*board));
  }
  if (corpus.declared_boards != 100u) {
    corpus.error = "smoke100.txt did not declare exactly 100 symbols";
  } else if (corpus.boards.empty()) {
    corpus.error = "no real 100-name OPRA fixtures could be loaded";
  }
  return corpus;
}

struct FitIterationReport {
  SurfaceFitStageTimings internal{};
  double chain_install_ms{0.0};
  double fit_attempt_ms{0.0};
  double value_ms{0.0};
  double snapshot_ms{0.0};
  std::size_t attempted{0u};
  std::size_t fitted{0u};
  std::size_t failed{0u};
  std::size_t quotes{0u};
  std::size_t requested_model_prices{0u};
  std::size_t auto_value_worker_boards{0u};
  std::size_t legacy_compatible_boards{0u};
  std::size_t stored_query_cache_boards{0u};
  std::size_t published_essvi_boards{0u};
  std::size_t published_override_boards{0u};
  std::size_t legacy_override_cold_boards{0u};
};

void add_stage_timings(SurfaceFitStageTimings &sum, const SurfaceFitStageTimings &sample) noexcept {
  sum.carry_solve_ms += sample.carry_solve_ms;
  sum.observation_deam_ms += sample.observation_deam_ms;
  sum.slice_fit_ms += sample.slice_fit_ms;
  sum.audit_ms += sample.audit_ms;
  sum.calendar_validation_ms += sample.calendar_validation_ms;
  // V2: the two previously-invisible session-level fit costs.
  sum.correction_cache_ms += sample.correction_cache_ms;
  sum.input_diagnostics_ms += sample.input_diagnostics_ms;
  sum.correction_cache_solves += sample.correction_cache_solves;
  sum.input_diagnostics_solves += sample.input_diagnostics_solves;
  sum.total_wall_ms += sample.total_wall_ms;
  sum.collected = sum.collected || sample.collected;
}

void record_value_contract(FitIterationReport &report, const FittedSurface &surface,
                           std::size_t quote_count) noexcept {
  report.requested_model_prices += quote_count;
  ++report.auto_value_worker_boards;

  const VolaSession &session = surface.session();
  const SessionInputs &inputs = session.inputs();
  const bool legacy = inputs.query_pricing_tier == QueryPricingTier::LegacyCompatible;
  const bool override_curve = inputs.curve.kind != VolCurveKind::Essvi;
  report.legacy_compatible_boards += legacy ? 1u : 0u;
  report.published_essvi_boards += override_curve ? 0u : 1u;
  report.published_override_boards += override_curve ? 1u : 0u;
  report.legacy_override_cold_boards += legacy && override_curve ? 1u : 0u;

  const auto caches = session.correction_caches();
  report.stored_query_cache_boards += caches.call != nullptr || caches.put != nullptr ? 1u : 0u;
}

[[nodiscard]] FitIterationReport run_fit_iteration(const FitCorpus &corpus) {
  FitIterationReport report;
  for (const testkit::OpraBoard &board : corpus.boards) {
    ++report.attempted;
    const StageClock::time_point chain_start = StageClock::now();
    Result<OptionChain> chain = OptionChain::from_frame(board.panel.frame, board.env());
    report.chain_install_ms += elapsed_ms(chain_start);
    if (!chain.has_value()) {
      ++report.failed;
      continue;
    }

    PricerConfig config;
    // Match the production/accuracy-panel baseline: Robust owns the routing
    // budget, while the board's stamped event/session context selects the
    // actual profile and curve family.
    config.preset = FitPreset::Robust;
    config.context = board.panel.fit_context;
    // A0 measurement: fit_workers is 1 by default (canonical gate, unchanged),
    // overridable via ATX_BENCH_FIT_WORKERS for the wall-vs-CPU sweep only.
    config.fit_workers = bench_fit_workers();
    config.n_threads = 0u;
    config.collect_stage_timings = true;
    PricerFitter fitter{config};
    const StageClock::time_point fit_start = StageClock::now();
    const Status fitted = fitter.fit(*chain);
    report.fit_attempt_ms += elapsed_ms(fit_start);
    const FittedSurface *surface = fitter.surface();
    if (!fitted.has_value() || surface == nullptr) {
      ++report.failed;
      continue;
    }
    add_stage_timings(report.internal, surface->diagnostics().fit_timings);
    record_value_contract(report, *surface, chain->size());

    const StageClock::time_point value_start = StageClock::now();
    Result<ChainValuation> valued = fitter.value_chain(*chain, OutputField::Prices, 0u);
    report.value_ms += elapsed_ms(value_start);
    if (!valued.has_value()) {
      ++report.failed;
      continue;
    }

    const StageClock::time_point snapshot_start = StageClock::now();
    Result<PricedSurface> snapshot = surface->session().to_priced_surface();
    report.snapshot_ms += elapsed_ms(snapshot_start);
    if (!snapshot.has_value()) {
      ++report.failed;
      continue;
    }
    ++report.fitted;
    report.quotes += chain->size();
    benchmark::DoNotOptimize(valued->model_price.data());
    benchmark::DoNotOptimize(snapshot->n_slices());
    benchmark::ClobberMemory();
  }
  return report;
}

void publish_fit_counters(benchmark::State &state, const FitCorpus &corpus,
                          const FitIterationReport &report) {
  const double denominator = static_cast<double>(std::max<std::size_t>(report.fitted, 1u));
  state.counters["declared_boards"] = static_cast<double>(corpus.declared_boards);
  state.counters["fixture_failures"] = static_cast<double>(corpus.fixture_failures);
  state.counters["fit_failures"] = static_cast<double>(report.failed);
  state.counters["fitted_boards"] = static_cast<double>(report.fitted);
  state.counters["quotes"] = static_cast<double>(report.quotes);
  state.counters["requested_model_prices"] = static_cast<double>(report.requested_model_prices);
  state.counters["requested_band_inversions"] = 0.0;
  state.counters["configured_value_workers"] = 0.0;
  state.counters["value_worker_policy_auto"] = 1.0;
  state.counters["configured_fit_workers"] = static_cast<double>(bench_fit_workers());
  state.counters["auto_value_worker_boards"] = static_cast<double>(report.auto_value_worker_boards);
  state.counters["legacy_compatible_boards"] = static_cast<double>(report.legacy_compatible_boards);
  state.counters["stored_query_cache_boards"] =
      static_cast<double>(report.stored_query_cache_boards);
  state.counters["published_essvi_boards"] = static_cast<double>(report.published_essvi_boards);
  state.counters["published_override_boards"] =
      static_cast<double>(report.published_override_boards);
  state.counters["legacy_override_cold_boards"] =
      static_cast<double>(report.legacy_override_cold_boards);
  const double attempted_denominator =
      static_cast<double>(std::max<std::size_t>(report.attempted, 1u));
  state.counters["chain_install_ms_per_attempt"] = report.chain_install_ms / attempted_denominator;
  state.counters["fit_attempt_wall_ms_total"] = report.fit_attempt_ms;
  state.counters["fit_attempt_wall_ms_per_attempt"] = report.fit_attempt_ms / attempted_denominator;
  state.counters["reported_success_fit_wall_ms_total"] = report.internal.total_wall_ms;
  state.counters["unreported_or_failed_fit_wall_ms"] =
      std::fmax(0.0, report.fit_attempt_ms - report.internal.total_wall_ms);
  state.counters["carry_cpu_ms_per_board"] = report.internal.carry_solve_ms / denominator;
  state.counters["observation_deam_cpu_ms_per_board"] =
      report.internal.observation_deam_ms / denominator;
  state.counters["slice_fit_ms_per_board"] = report.internal.slice_fit_ms / denominator;
  state.counters["audit_ms_per_board"] = report.internal.audit_ms / denominator;
  state.counters["calendar_validation_ms_per_board"] =
      report.internal.calendar_validation_ms / denominator;
  // V2 blind-spot closure: the two previously-invisible fit costs, per board, in
  // both wall-ms and AL-boundary-solve terms (solve counts exact under this bench's
  // serial per-board fit loop).
  state.counters["correction_cache_ms_per_board"] =
      report.internal.correction_cache_ms / denominator;
  state.counters["input_diagnostics_ms_per_board"] =
      report.internal.input_diagnostics_ms / denominator;
  state.counters["correction_cache_solves_per_board"] =
      static_cast<double>(report.internal.correction_cache_solves) / denominator;
  state.counters["input_diagnostics_solves_per_board"] =
      static_cast<double>(report.internal.input_diagnostics_solves) / denominator;
  // Fit fraction decomposed into carry / de-Am / cache / calib / diag / parity —
  // the six attributable stages of a board fit. Fractions of the summed stage CPU
  // (audit rolls into parity), so the C-lane wins (C1 kills diag, C2/C5 kill
  // cache+carry, C3 shrinks calib+de-Am) are now individually attributable.
  const double carry = report.internal.carry_solve_ms;
  const double deam = report.internal.observation_deam_ms;
  const double cache = report.internal.correction_cache_ms;
  const double calib = report.internal.slice_fit_ms;
  const double diag = report.internal.input_diagnostics_ms;
  const double parity = report.internal.calendar_validation_ms + report.internal.audit_ms;
  const double stage_sum = carry + deam + cache + calib + diag + parity;
  const double frac_denom = stage_sum > 0.0 ? stage_sum : 1.0;
  state.counters["fit_frac_carry"] = carry / frac_denom;
  state.counters["fit_frac_deam"] = deam / frac_denom;
  state.counters["fit_frac_cache"] = cache / frac_denom;
  state.counters["fit_frac_calib"] = calib / frac_denom;
  state.counters["fit_frac_diag"] = diag / frac_denom;
  state.counters["fit_frac_parity"] = parity / frac_denom;
  state.counters["fit_wall_ms_per_board"] = report.internal.total_wall_ms / denominator;
  state.counters["value_ms_per_board"] = report.value_ms / denominator;
  state.counters["snapshot_ms_per_board"] = report.snapshot_ms / denominator;
}

template <FitCorpus (*LoadCorpus)()> void BM_FitE2e(benchmark::State &state) {
  const FitCorpus corpus = LoadCorpus();
  if (!corpus.error.empty()) {
    state.SkipWithError(corpus.error.c_str());
    return;
  }

  FitIterationReport last;
  for (auto _ : state) {
    last = run_fit_iteration(corpus);
    if (last.fitted == 0u) {
      state.SkipWithError("real OPRA facade produced no complete fit/value/snapshot result");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(last.fitted));
  publish_fit_counters(state, corpus, last);
}

[[nodiscard]] StrategySpec make_strangle_spec() {
  StrategySpec spec;
  spec.name = "e2e-real-spy-short-40d-6m-strangle";
  LegSpec leg;
  leg.symbol = "SPY";
  leg.tenor.target_T = 0.5;
  leg.tenor.snap_to_listed = false;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.size = SizeSpec{SizeSpec::Kind::TargetTheta, 10'000.0, -1.0};
  spec.legs.push_back(std::move(leg));
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 1.0;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::None, HedgeSpec::Cadence::Daily, 0.0};
  return spec;
}

struct BacktestFixture {
  std::optional<Clock> clock{};
  StrategySpec strategy{};
  std::optional<BacktestResult> cold_oracle{};
  std::shared_ptr<SnapshotCache> representative_warm_cache{};
  std::string error{};
};

[[nodiscard]] RunConfig cold_config() {
  RunConfig config;
  config.query_pricing_tier = QueryPricingTier::ColdReference;
  return config;
}

[[nodiscard]] BacktestFixture load_backtest_fixture() {
  BacktestFixture fixture;
  const fs::path manifest_path = first_existing(
      {"data/spy_ytd/archives/manifest.tsv", "../data/spy_ytd/archives/manifest.tsv",
       "../../data/spy_ytd/archives/manifest.tsv", "C:/atx/data/spy_ytd/archives/manifest.tsv"});
  if (manifest_path.empty()) {
    fixture.error = "real SPY backtest manifest is unavailable";
    return fixture;
  }
  Result<CorpusManifest> manifest = read_manifest_file(manifest_path.generic_string());
  if (!manifest.has_value()) {
    fixture.error = manifest.error().to_string();
    return fixture;
  }
  Result<Clock> clock = Clock::from_manifest(*manifest);
  if (!clock.has_value()) {
    fixture.error = clock.error().to_string();
    return fixture;
  }
  fixture.clock.emplace(std::move(*clock));
  fixture.strategy = make_strangle_spec();

  DeclarativeStrategy cold_strategy{fixture.strategy};
  Result<BacktestResult> cold = run_backtest(*fixture.clock, cold_strategy, cold_config());
  if (!cold.has_value()) {
    fixture.error = cold.error().to_string();
    return fixture;
  }
  fixture.cold_oracle.emplace(std::move(*cold));

  fixture.representative_warm_cache = std::make_shared<SnapshotCache>();
  for (const SnapshotRef &ref : fixture.clock->refs()) {
    Result<std::shared_ptr<const MarketSnapshot>> loaded = fixture.representative_warm_cache->load(
        ref.archive_path, QueryPricingTier::RepresentativeFast);
    if (!loaded.has_value()) {
      fixture.error = loaded.error().to_string();
      fixture.representative_warm_cache.reset();
      return fixture;
    }
  }
  return fixture;
}

[[nodiscard]] const BacktestFixture &backtest_fixture() {
  static const BacktestFixture fixture = load_backtest_fixture();
  return fixture;
}

[[nodiscard]] double max_abs_delta(const std::vector<double> &left,
                                   const std::vector<double> &right) noexcept {
  if (left.size() != right.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double maximum = 0.0;
  for (std::size_t index = 0u; index < left.size(); ++index) {
    maximum = std::max(maximum, std::abs(left[index] - right[index]));
  }
  return maximum;
}

struct BacktestOutcome {
  double final_nav{0.0};
  double final_nav_delta{0.0};
  double max_nav_delta{0.0};
  double max_gross_vega_delta{0.0};
  double max_gross_gamma_delta{0.0};
  double max_gross_theta_delta{0.0};
};

[[nodiscard]] BacktestOutcome compare_outcome(const BacktestResult &result,
                                              const BacktestResult &cold) noexcept {
  BacktestOutcome outcome;
  if (!result.nav.empty()) {
    outcome.final_nav = result.nav.back();
  }
  const double cold_final = cold.nav.empty() ? 0.0 : cold.nav.back();
  outcome.final_nav_delta = outcome.final_nav - cold_final;
  outcome.max_nav_delta = max_abs_delta(result.nav, cold.nav);
  outcome.max_gross_vega_delta = max_abs_delta(result.gross_vega, cold.gross_vega);
  outcome.max_gross_gamma_delta = max_abs_delta(result.gross_gamma, cold.gross_gamma);
  outcome.max_gross_theta_delta = max_abs_delta(result.gross_theta, cold.gross_theta);
  return outcome;
}

enum class BacktestMode : std::uint8_t {
  Cold = 0,
  RepresentativeEager = 1,
  RepresentativeWarm = 2,
  RepresentativeScreenColdConfirm = 3,
};

[[nodiscard]] RunConfig backtest_config(BacktestMode mode, const BacktestFixture &fixture) {
  if (mode == BacktestMode::Cold) {
    RunConfig config = cold_config();
    config.snapshot_cache = std::make_shared<SnapshotCache>(3u);
    return config;
  }
  RunConfig config;
  config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  if (mode == BacktestMode::RepresentativeEager) {
    config.snapshot_cache = std::make_shared<SnapshotCache>(3u);
  } else {
    config.snapshot_cache = fixture.representative_warm_cache;
    config.prefetch_snapshots = false;
    if (mode == BacktestMode::RepresentativeScreenColdConfirm) {
      // The approximation proposes only. Every accepted strike and every
      // economic price/Greek is recomputed on the cold reference route.
      config.price.query_execution = QueryExecution::ColdReference;
      config.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
    }
  }
  return config;
}

void publish_backtest_counters(benchmark::State &state, const BacktestFixture &fixture,
                               const RunConfig &config, const BacktestOutcome &outcome,
                               const SnapshotCacheStats &before) {
  const SnapshotCacheStats after = config.snapshot_cache->stats();
  const BacktestResult &cold = *fixture.cold_oracle;
  state.counters["cold_final_nav"] = cold.nav.empty() ? 0.0 : cold.nav.back();
  state.counters["final_nav"] = outcome.final_nav;
  state.counters["final_nav_delta"] = outcome.final_nav_delta;
  state.counters["max_nav_delta"] = outcome.max_nav_delta;
  state.counters["max_gross_vega_delta"] = outcome.max_gross_vega_delta;
  state.counters["max_gross_gamma_delta"] = outcome.max_gross_gamma_delta;
  state.counters["max_gross_theta_delta"] = outcome.max_gross_theta_delta;
  state.counters["cache_loads"] = static_cast<double>(after.loads - before.loads);
  state.counters["cache_hits"] = static_cast<double>(after.hits - before.hits);
  state.counters["fast_build_loads"] =
      static_cast<double>(after.fast_build_loads - before.fast_build_loads);
  state.counters["steps"] = static_cast<double>(cold.size());
}

template <BacktestMode Mode> void BM_BacktestReal(benchmark::State &state) {
  const BacktestFixture &fixture = backtest_fixture();
  if (!fixture.error.empty() || !fixture.clock.has_value() || !fixture.cold_oracle.has_value()) {
    state.SkipWithError(fixture.error.empty() ? "real backtest fixture failed"
                                              : fixture.error.c_str());
    return;
  }

  BacktestOutcome last;
  SnapshotCacheStats stats_before{};
  RunConfig last_config;
  for (auto _ : state) {
    RunConfig config = backtest_config(Mode, fixture);
    stats_before = config.snapshot_cache->stats();
    StrategySpec strategy_spec = fixture.strategy;
    if constexpr (Mode == BacktestMode::RepresentativeScreenColdConfirm) {
      strategy_spec.resolution.fast_screen_cold_confirm = true;
    }
    DeclarativeStrategy strategy{std::move(strategy_spec)};
    Result<BacktestResult> result = run_backtest(*fixture.clock, strategy, config);
    if (!result.has_value()) {
      const std::string error = result.error().to_string();
      state.SkipWithError(error.c_str());
      break;
    }
    last = compare_outcome(*result, *fixture.cold_oracle);
    benchmark::DoNotOptimize(result->nav.data());
    benchmark::ClobberMemory();
    last_config = std::move(config);
  }
  if (last_config.snapshot_cache != nullptr) {
    publish_backtest_counters(state, fixture, last_config, last, stats_before);
  }
  const std::int64_t priced_steps = static_cast<std::int64_t>(
      fixture.cold_oracle->size() > 0u ? fixture.cold_oracle->size() - 1u : 0u);
  state.SetItemsProcessed(state.iterations() * priced_steps);
}

benchmark::internal::Benchmark *register_corpus_scale(const char *name,
                                                      void (*function)(benchmark::State &)) {
  // A corpus operation can already contain 100 admitted boards. Process-level
  // best-of-3 owns baseline noise control; adaptive warmup and in-process repeats
  // would multiply cold fitting work and make the canonical gate impractical.
  //
  // Because this is Iterations(1) with no repeats, Google Benchmark emits NO
  // aggregate row (no median/stddev/cv) for these rows — only a single
  // `iteration` row. compare_baseline.py therefore treats them as CV-UNGUARDED:
  // it falls back to the iteration real_time to gate a CRASH (a vanished/errored
  // row → MISSING → hard fail), while a ratio move alone is advisory (a lone
  // iteration carries no CV). Keep this Iterations(1) contract in sync with that
  // fallback if the corpus scale registration ever changes.
  return benchmark::RegisterBenchmark(name, function)
      ->Iterations(1)
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
}

// ── M3: per-stage attribution harness (class: tooling) ───────────────────────
//
// Attributes the end-to-end pipeline wall — the north-star loop fit -> serialize ->
// deserialize -> price/greeks — to its four stages, so effort lands on the real
// critical path (and a 5%-of-wall stage is never "optimized"). The stage BOUNDARIES
// are timed HARNESS-SIDE via counters::timing::ScopedStageTimer (no foreign TU is
// instrumented — WS-M owns only the harness). It runs on a SELF-CONTAINED synthetic
// board so it always produces a number under the quiet-window protocol, independent
// of whether the real-OPRA fixtures the fit/backtest rows above need are on disk.
//
// Stage definitions (each timed from the driver's vantage):
//   fit         — PricerFitter::fit(chain): OptionChain -> FittedSurface.
//   serialize   — snapshot (VolaSession::to_priced_surface) + write_surface_archive_v2:
//                 the fit output packed to on-disk bytes (memcpy-bound).
//   deserialize — SurfaceArchiveV2::open + reconstruct_all_with_provenance: bytes ->
//                 an owned ready-to-price PricedSurface (framing + per-record parse).
//   price       — american price+greeks over a (K,T,side) grid on the deserialized
//                 surface (the pricing/greeks the backtest hot loop pays per step).
//
// The reported fractions are stage/total. NOTE for the reader: in the BACKTEST hot
// loop only deserialize+price recur (fit+serialize are the offline populate); the
// per-stage ms let M4 read both the full-pipeline split AND the deser:price ratio
// that governs the in-loop budget.

namespace timing = atx::vol::counters::timing;

// A few synthetic liquid boards (distinct spots) — a genuine cold fit each, not a
// hand-rolled surface. Mirrors surface_v2_bench's make_liquid_fixture.
[[nodiscard]] SynthPanelSpec make_attribution_spec(double spot) {
  SynthPanelSpec spec;
  spec.uid = "SYNTHATTR";
  spec.snapshot_iso = "2026-06-19";
  spec.spot = spot;
  spec.r = kRate;
  spec.borrow = 0.0;
  spec.half_spread_frac = 0.01;
  spec.min_half_spread = 0.02;
  constexpr std::string_view expiries[]{"2026-07-17", "2026-08-21", "2026-09-18", "2026-12-18",
                                        "2027-03-19"};
  for (const std::string_view expiry : expiries) {
    SynthExpiry slice;
    slice.expiry_iso = std::string(expiry);
    slice.T = year_fraction(spec.snapshot_iso, expiry);
    slice.truth = S3Params{0.30, 0.0, 0.0};
    spec.expiries.push_back(std::move(slice));
  }
  constexpr std::size_t kStrikeCount = 65;
  spec.strikes.reserve(kStrikeCount);
  for (std::size_t i = 0; i < kStrikeCount; ++i) {
    const double fraction = static_cast<double>(i) / static_cast<double>(kStrikeCount - 1u);
    const double log_moneyness = -0.80 + 1.60 * fraction;
    spec.strikes.push_back(spec.spot * std::exp(log_moneyness));
  }
  return spec;
}

struct AttributionCorpus {
  std::vector<OptionChain> chains;
  std::vector<double> spots;
  std::string error;
};

[[nodiscard]] AttributionCorpus load_attribution_corpus() {
  AttributionCorpus corpus;
  const double spots[] = {90.0, 100.0, 110.0, 125.0};
  for (const double spot : spots) {
    Result<SynthPanel> panel = make_synthetic_american_panel(make_attribution_spec(spot));
    if (!panel.has_value()) {
      corpus.error = "synthetic attribution panel construction failed: " + panel.error().to_string();
      return corpus;
    }
    Result<OptionChain> chain = OptionChain::from_frame(panel->frame, kRate, spot);
    if (!chain.has_value()) {
      corpus.error = "synthetic attribution chain install failed: " + chain.error().to_string();
      return corpus;
    }
    corpus.chains.push_back(std::move(*chain));
    corpus.spots.push_back(spot);
  }
  return corpus;
}

struct AttributionReport {
  timing::StageAccumulator acc;
  std::size_t boards{0};
  std::size_t priced_points{0};
  bool ok{false};
};

// One full pass: fit -> serialize -> deserialize -> price for every board, charging
// each stage's wall to `report.acc`.
[[nodiscard]] AttributionReport run_attribution_iteration(const AttributionCorpus &corpus) {
  AttributionReport report;
  // A representative price grid: 5 OTM/ITM strikes × 2 maturities × both sides.
  const double moneyness[] = {0.85, 0.93, 1.00, 1.08, 1.20};
  const double Ts[] = {0.10, 0.25};

  for (std::size_t b = 0; b < corpus.chains.size(); ++b) {
    const OptionChain &chain = corpus.chains[b];
    const double spot = corpus.spots[b];

    PricerConfig config;
    config.preset = FitPreset::Robust; // production routing (published curve family)

    PricerFitter fitter{config};
    {
      timing::ScopedStageTimer t(report.acc, timing::Stage::Fit);
      const Status fitted = fitter.fit(chain);
      if (!fitted.has_value()) {
        return report; // ok stays false
      }
    }
    const FittedSurface *surface = fitter.surface();
    if (surface == nullptr) {
      return report;
    }

    std::vector<std::byte> bytes;
    {
      timing::ScopedStageTimer t(report.acc, timing::Stage::Serialize);
      Result<PricedSurface> snapshot = surface->session().to_priced_surface();
      if (!snapshot.has_value()) {
        return report;
      }
      SurfaceArchiveItem item;
      item.symbol = "SYNTH";
      item.surface = &*snapshot;
      Result<std::vector<std::byte>> serialized =
          write_surface_archive_v2(std::span<const SurfaceArchiveItem>(&item, 1));
      if (!serialized.has_value()) {
        return report;
      }
      bytes = std::move(*serialized);
    }

    std::optional<PricedSurface> ready;
    {
      timing::ScopedStageTimer t(report.acc, timing::Stage::Deserialize);
      Result<SurfaceArchiveV2> archive = SurfaceArchiveV2::open(std::move(bytes));
      if (!archive.has_value()) {
        return report;
      }
      // reconstruct_*, not map_*: the stage must yield an OWNED PricedSurface that
      // outlives this scope's archive (a PricedSurfaceView borrows the archive's
      // bytes). Same measured work as the v1 whole-board reconstruct it replaces.
      Result<std::vector<ArchivedSurface>> all = archive->reconstruct_all_with_provenance();
      if (!all.has_value() || all->empty()) {
        return report;
      }
      ready.emplace(std::move(all->front().surface));
    }

    {
      timing::ScopedStageTimer t(report.acc, timing::Stage::Price);
      for (const double m : moneyness) {
        const double K = spot * m;
        for (const double T : Ts) {
          for (const Side side : {Side::Call, Side::Put}) {
            Result<AmericanGreeks> g = ready->greeks(K, T, side);
            if (g.has_value()) {
              benchmark::DoNotOptimize(g->price);
              ++report.priced_points;
            }
          }
        }
      }
    }
    ++report.boards;
  }
  report.ok = report.boards == corpus.chains.size();
  return report;
}

void BM_PipelineAttribution(benchmark::State &state) {
  const AttributionCorpus corpus = load_attribution_corpus();
  if (!corpus.error.empty()) {
    state.SkipWithError(corpus.error.c_str());
    return;
  }

  // Manual warm-up — apply_common chains MinWarmUpTime(0.5), which Google Benchmark
  // 1.9.0 forbids alongside this row's explicit Iterations() (Benchmark::Iterations
  // asserts IsZero(min_warmup_time_); the combination aborts the binary at
  // static-init). The registration clears the GB warm-up with ->MinWarmUpTime(0.0)
  // and the "brief turbo prime" is re-supplied here: one untimed attribution pass
  // before the timed reps. Keeps the review's Iterations(3) x Repetitions(15)
  // sampling (finding #2) verbatim; only the warm-up delivery moved (GB -> in-body).
  // Runs once per repetition (this body is invoked once per rep), matching
  // MinWarmUpTime's per-rep cadence. The corpus is self-contained synthetic, so the
  // warm pass always completes.
  {
    AttributionReport warm = run_attribution_iteration(corpus);
    benchmark::DoNotOptimize(warm.priced_points);  // non-const: mutable-ref DoNotOptimize overload
  }

  AttributionReport last;
  for (auto _ : state) {
    last = run_attribution_iteration(corpus);
    if (!last.ok) {
      state.SkipWithError("synthetic pipeline attribution did not complete every stage");
      break;
    }
    benchmark::ClobberMemory();
  }
  if (!last.ok) {
    return;
  }
  using timing::Stage;
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(last.boards));
  state.counters["boards"] = static_cast<double>(last.boards);
  state.counters["priced_points"] = static_cast<double>(last.priced_points);
  // Per-stage wall (ms, summed across boards in one iteration) and its fraction of
  // the attributed total — the headline attribution numbers M4 ratifies.
  state.counters["fit_ms"] = last.acc.ms(Stage::Fit);
  state.counters["serialize_ms"] = last.acc.ms(Stage::Serialize);
  state.counters["deserialize_ms"] = last.acc.ms(Stage::Deserialize);
  state.counters["price_ms"] = last.acc.ms(Stage::Price);
  state.counters["attributed_total_ms"] = last.acc.total_ns() * 1.0e-6;
  state.counters["fit_frac"] = last.acc.fraction(Stage::Fit);
  state.counters["serialize_frac"] = last.acc.fraction(Stage::Serialize);
  state.counters["deserialize_frac"] = last.acc.fraction(Stage::Deserialize);
  state.counters["price_frac"] = last.acc.fraction(Stage::Price);
}

const int kRegistered = [] {
  register_corpus_scale("fit/e2e/spy_real", BM_FitE2e<load_spy_fit_corpus>);
  register_corpus_scale("fit/e2e/100name", BM_FitE2e<load_universe_fit_corpus>);
  // Sample-count override (review fix, finding #2): the attribution pass is a heavy
  // ~0.66 s op, so apply_common's default (5 reps × ~1 auto iteration) gives only ~5
  // timed executions and a single slow rep swings the wall CV. Fix 3 iterations/rep
  // (each rep-mean averages 3 full passes) and raise to 15 reps for a stable CV
  // before M4 reads the stage fractions. The fit/price SPLIT was already stable
  // (fit_frac CV ~0.4%); this stabilizes the absolute wall too.
  //
  // Warm-up reconciliation (shape (a)): apply_common chains MinWarmUpTime(0.5), but
  // Google Benchmark 1.9.0's Benchmark::Iterations() asserts IsZero(min_warmup_time_),
  // so apply_common(...)->Iterations(3) aborts the binary at static-init
  // (benchmark_register.cc:369). Clear the GB warm-up on THIS row only with
  // ->MinWarmUpTime(0.0) (legal here: iterations_ is still 0 at that chain point);
  // BM_PipelineAttribution re-supplies the warm-up in-body. Iterations(3) x
  // Repetitions(15) is kept verbatim (finding #2 needs deterministic fixed work/rep
  // for a meaningful CV; MinTime auto-iteration would undo it). apply_common (shared
  // by ~20 bench files) is untouched; the other rows in this binary are unaffected.
  apply_common(benchmark::RegisterBenchmark("attribution/pipeline/synth_fit_ser_deser_price",
                                            BM_PipelineAttribution))
      ->MinWarmUpTime(0.0)  // clear apply_common's GB warm-up (illegal with Iterations); warm-up is in-body
      ->Unit(benchmark::kMillisecond)
      ->Iterations(3)
      ->Repetitions(15)
      ->UseRealTime();
  apply_common(benchmark::RegisterBenchmark("price/backtest/spy_real/cold",
                                            BM_BacktestReal<BacktestMode::Cold>))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  apply_common(benchmark::RegisterBenchmark("price/backtest/spy_real/representative_eager",
                                            BM_BacktestReal<BacktestMode::RepresentativeEager>))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  apply_common(benchmark::RegisterBenchmark("price/backtest/spy_real/representative_warm",
                                            BM_BacktestReal<BacktestMode::RepresentativeWarm>))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  apply_common(
      benchmark::RegisterBenchmark("price/backtest/spy_real/representative_screen_cold_confirm",
                                   BM_BacktestReal<BacktestMode::RepresentativeScreenColdConfirm>))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
