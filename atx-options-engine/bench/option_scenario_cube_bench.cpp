// End-to-end construction benchmark for the point-in-time full-reprice
// scenario compiler. Archive creation/loading and research-panel construction
// are fixture setup; each timed iteration builds a new authoritative cube.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/options/option_scenario_cube.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace {

namespace fs = std::filesystem;

using atx::options::research::OptionPanelField;
using atx::options::research::OptionPanelRow;
using atx::options::research::OptionResearchPanel;
using atx::options::risk::compile_option_scenario_cube;
using atx::options::risk::OptionScenarioActiveContract;
using atx::options::risk::OptionScenarioContractRole;
using atx::options::risk::OptionScenarioCubeBuildSpec;
using atx::options::risk::OptionScenarioDefinition;
using atx::options::risk::OptionScenarioManifest;
using atx::options::risk::OptionScenarioUnderlierShock;
using atx::vol::ArchiveContentIdentity;
using atx::vol::ExerciseStyle;
using atx::vol::Side;

constexpr std::int64_t kNow = 1'750'000'000'000'000'000LL;
constexpr std::int64_t kSecondNs = 1'000'000'000LL;
constexpr std::int64_t kDayNs = 86'400LL * kSecondNs;
constexpr double kRate = 0.04;
constexpr double kYield = 0.015;
constexpr std::size_t kMaximumFixtureUnderliers = 16U;
constexpr std::uint64_t kFirstScenarioId = 10'001U;

[[nodiscard]] std::size_t checked_product(std::size_t left, std::size_t right) {
  if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
    throw std::length_error{"scenario cube benchmark fixture size overflow"};
  }
  return left * right;
}

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{100'000U + seed, 200'000U + seed,
                                static_cast<std::uint32_t>(300'000U + seed),
                                static_cast<std::uint32_t>(400'000U + seed)};
}

template <typename T>
[[nodiscard]] T take_or_throw(atx::core::Result<T> result, std::string_view context) {
  if (!result) {
    throw std::runtime_error{std::string{context} + ": " + result.error().to_string()};
  }
  return std::move(*result);
}

void require_ok(const atx::core::Status &status, std::string_view context) {
  if (!status) {
    throw std::runtime_error{std::string{context} + ": " + status.error().to_string()};
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    const std::uint64_t suffix = next.fetch_add(1U, std::memory_order_relaxed);
    path_ =
        fs::temp_directory_path() / ("atx-option-scenario-cube-bench-" + std::to_string(suffix));
    std::error_code error;
    fs::remove_all(path_, error);
    if (error) {
      throw std::runtime_error{"scenario cube benchmark temp cleanup failed"};
    }
    fs::create_directories(path_, error);
    if (error) {
      throw std::runtime_error{"scenario cube benchmark temp creation failed"};
    }
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_{};
};

[[nodiscard]] std::uint32_t underlier_uid(std::size_t index) noexcept {
  return static_cast<std::uint32_t>(index + 1U);
}

[[nodiscard]] double underlier_spot(std::size_t index) noexcept {
  return 80.0 + 5.0 * static_cast<double>(index);
}

[[nodiscard]] atx::vol::PricedSurface make_surface(std::size_t underlier_index) {
  constexpr std::array tenors{0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  const double spot = underlier_spot(underlier_index);
  atx::vol::CurveSurface curves;
  std::vector<atx::vol::SliceContext> contexts;
  contexts.reserve(tenors.size());
  for (std::size_t index = 0U; index < tenors.size(); ++index) {
    const double tenor = tenors[index];
    const double forward = spot * std::exp((kRate - kYield) * tenor);
    atx::vol::EssviParams params{};
    params.theta = 0.035 + 0.004 * static_cast<double>(index);
    params.phi = 1.4 - 0.04 * static_cast<double>(index);
    params.rho = -0.35 + 0.015 * static_cast<double>(index);
    params.psi = 0.5;
    params.p = 0.5;
    params.lambda = 0.5;
    params.T = tenor;
    params.F = forward;
    params.expiry_id = static_cast<std::uint16_t>(index);
    curves.push(std::make_unique<atx::vol::EssviCurve>(params, std::exp(-kRate * tenor)));
    contexts.push_back(atx::vol::SliceContext{tenor, forward, 0.0, kYield, 250U, 7U});
  }

  atx::vol::PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = kNow;
  pricing.method = atx::vol::AmericanMethod::AndersenLake;
  pricing.al_opts = atx::vol::al_fast_opts();
  pricing.uid = underlier_uid(underlier_index);
  return take_or_throw(
      atx::vol::PricedSurface::create(std::move(curves), std::move(contexts), pricing),
      "create benchmark risk surface");
}

[[nodiscard]] atx::vol::SurfaceProvenance admitted_risk_provenance() {
  atx::vol::SurfaceProvenance provenance;
  provenance.purpose = atx::vol::SurfacePurpose::Risk;
  provenance.quality_mode = atx::vol::FitQualityMode::Balanced;
  provenance.state = atx::vol::SurfaceState::Healthy;
  provenance.validation.validation_id = 77U;
  provenance.validation.n_slices = 7U;
  provenance.validation.n_strike_samples = 1'001U;
  provenance.validation.n_calendar_samples = 1'001U;
  provenance.source_generation = 3U;
  provenance.served_generation = 3U;
  return provenance;
}

[[nodiscard]] std::string underlier_symbol(std::size_t index) {
  return "UND" + std::to_string(index + 1U);
}

[[nodiscard]] atx::vol::MarketSnapshot make_snapshot(const fs::path &directory,
                                                     std::size_t underlier_count) {
  std::vector<atx::vol::PricedSurface> surfaces;
  std::vector<std::string> symbols;
  surfaces.reserve(underlier_count);
  symbols.reserve(underlier_count);
  for (std::size_t index = 0U; index < underlier_count; ++index) {
    surfaces.push_back(make_surface(index));
    symbols.push_back(underlier_symbol(index));
  }

  const atx::vol::SurfaceProvenance provenance = admitted_risk_provenance();
  std::vector<atx::vol::SurfaceArchiveItem> items;
  items.reserve(underlier_count);
  for (std::size_t index = 0U; index < underlier_count; ++index) {
    items.push_back(atx::vol::SurfaceArchiveItem{symbols[index], &surfaces[index], provenance});
  }
  const std::string archive_path = (directory / "surfaces.atxvsa").string();
  require_ok(atx::vol::write_surface_archive_v2_file(archive_path, items),
             "write benchmark risk archive");
  return take_or_throw(
      atx::vol::MarketSnapshot::load(archive_path, atx::vol::QueryPricingTier::ColdReference),
      "load benchmark risk snapshot");
}

[[nodiscard]] OptionPanelRow make_panel_row(std::size_t contract_index,
                                            std::size_t underlier_count) {
  constexpr std::int64_t decision_ts_ns = kNow + 60LL * kSecondNs;
  const std::size_t underlier_index = contract_index % underlier_count;
  const std::uint64_t contract_id = static_cast<std::uint64_t>(contract_index + 1U);
  const double spot = underlier_spot(underlier_index);
  OptionPanelRow row;
  row.observation.uid = underlier_uid(underlier_index);
  row.observation.observed_ts_ns = kNow + 5LL * kSecondNs;
  row.observation.available_ts_ns = kNow + 10LL * kSecondNs;
  row.observation.decision_ts_ns = decision_ts_ns;
  row.observation.execution_ts_ns = decision_ts_ns + kSecondNs;
  row.observation.label_end_ts_ns = decision_ts_ns + kDayNs;
  row.observation.signal = contract_index % 2U == 0U ? 1.0 : -1.0;
  row.observation.forward_pnl = 5.0;
  row.observation.lagged_capital = 1'000'000.0;
  row.observation.source_identity = identity(1'000U + contract_id);
  row.contract_id = contract_id;
  row.engine_id.id = static_cast<std::uint32_t>(contract_id);
  row.definition_available_ts_ns = kNow;
  row.quote_event_ts_ns = kNow + 20LL * kSecondNs;
  row.quote_available_ts_ns = kNow + 25LL * kSecondNs;
  row.expiry_ts_ns =
      decision_ts_ns + static_cast<std::int64_t>(30U + contract_index % 330U) * kDayNs;
  row.strike = spot * (0.80 + 0.01 * static_cast<double>(contract_index % 41U));
  row.side = contract_index % 2U == 0U ? Side::Call : Side::Put;
  row.exercise_style =
      contract_index % 2U == 0U ? ExerciseStyle::American : ExerciseStyle::European;
  row.multiplier = 100.0;
  row.standard_deliverable = true;
  row.mark = 5.0;
  row.bid = 4.9;
  row.ask = 5.1;
  row.bid_size_contracts = 100.0;
  row.ask_size_contracts = 100.0;
  row.interval_volume_contracts = 1'000.0;
  row.lagged_open_interest_contracts = 10'000.0;
  row.adv_contracts = 20'000.0;
  row.return_sigma = 0.20;
  row.vega_per_contract = 20.0;
  row.initial_margin_per_contract = 1'000.0;
  row.maintenance_margin_per_contract = 800.0;
  row.definition_source_identity = identity(2'000U + contract_id);
  row.feature_source_identity = identity(3'000U + contract_id);
  row.execution_source_identity = identity(4'000U + contract_id);
  return row;
}

[[nodiscard]] OptionResearchPanel make_panel(std::size_t contract_count,
                                             std::size_t underlier_count) {
  std::vector<OptionPanelRow> rows;
  rows.reserve(contract_count);
  for (std::size_t index = 0U; index < contract_count; ++index) {
    rows.push_back(make_panel_row(index, underlier_count));
  }
  return take_or_throw(OptionResearchPanel::create(rows), "create benchmark research panel");
}

[[nodiscard]] OptionScenarioContractRole contract_role(std::size_t contract_index) noexcept {
  switch (contract_index % 4U) {
  case 0U:
    return OptionScenarioContractRole::Candidate;
  case 1U:
    return OptionScenarioContractRole::FilledPosition;
  case 2U:
    return OptionScenarioContractRole::WorkingOrder;
  case 3U:
    return OptionScenarioContractRole::FilledPosition | OptionScenarioContractRole::PendingCancel;
  }
  return OptionScenarioContractRole::None;
}

[[nodiscard]] std::vector<OptionScenarioActiveContract>
make_active_contracts(const OptionResearchPanel &panel) {
  const std::span<const atx::options::research::OptionInstrument> instruments = panel.instruments();
  const std::span<const atx::options::research::OptionDecisionAudit> audits =
      panel.decision_audit();
  const std::span<const double> marks = panel.row(OptionPanelField::Mark, 0U);
  if (instruments.size() != audits.size() || instruments.size() != marks.size()) {
    throw std::runtime_error{"benchmark research panel active catalog is misaligned"};
  }
  std::vector<OptionScenarioActiveContract> active_contracts;
  active_contracts.reserve(instruments.size());
  for (std::size_t index = 0U; index < instruments.size(); ++index) {
    active_contracts.push_back(OptionScenarioActiveContract{instruments[index], audits[index],
                                                            marks[index], contract_role(index)});
  }
  return active_contracts;
}

[[nodiscard]] OptionScenarioManifest make_manifest(std::size_t scenario_count,
                                                   std::size_t underlier_count) {
  OptionScenarioManifest manifest;
  manifest.minimum_implied_vol = 0.005;
  manifest.observed_ts_ns = kNow + 12LL * kSecondNs;
  manifest.available_ts_ns = kNow + 18LL * kSecondNs;
  manifest.effective_ts_ns = kNow;
  manifest.artifact_identity = identity(90'000U);
  manifest.scenarios.reserve(scenario_count);
  manifest.underlier_shocks.reserve(checked_product(scenario_count, underlier_count));
  for (std::size_t scenario_index = 0U; scenario_index < scenario_count; ++scenario_index) {
    const std::uint64_t scenario_id = kFirstScenarioId + static_cast<std::uint64_t>(scenario_index);
    const bool is_base = scenario_index == 0U;
    const double direction = scenario_index % 2U == 0U ? 1.0 : -1.0;
    manifest.scenarios.push_back(OptionScenarioDefinition{
        scenario_id, static_cast<std::int64_t>(scenario_index % 3U) * kDayNs,
        is_base ? 0.0 : direction * 0.0005});
    for (std::size_t underlier_index = 0U; underlier_index < underlier_count; ++underlier_index) {
      const double underlier_scale = 1.0 + 0.05 * static_cast<double>(underlier_index);
      manifest.underlier_shocks.push_back(OptionScenarioUnderlierShock{
          scenario_id, underlier_uid(underlier_index),
          is_base ? 0.0
                  : direction * (0.02 + 0.01 * static_cast<double>(scenario_index % 5U)) *
                        underlier_scale,
          is_base ? 0.0 : -direction * 0.01});
    }
  }
  return manifest;
}

[[nodiscard]] OptionScenarioCubeBuildSpec
make_build_spec(std::size_t contract_count, std::size_t underlier_count, std::size_t scenario_count,
                std::span<const OptionScenarioActiveContract> active_contracts,
                unsigned n_threads) {
  OptionScenarioCubeBuildSpec spec;
  spec.surface_available_ts_ns = kNow + 30LL * kSecondNs;
  spec.n_threads = n_threads;
  spec.active_set_attestation.observed_ts_ns = kNow + 26LL * kSecondNs;
  spec.active_set_attestation.available_ts_ns = kNow + 27LL * kSecondNs;
  spec.active_set_attestation.source_identity = identity(80'000U);
  for (const OptionScenarioActiveContract &active : active_contracts) {
    const auto has_role = [&active](OptionScenarioContractRole role) noexcept {
      return (static_cast<std::uint8_t>(active.role_mask) & static_cast<std::uint8_t>(role)) != 0U;
    };
    spec.active_set_attestation.candidate_contract_count +=
        has_role(OptionScenarioContractRole::Candidate) ? 1U : 0U;
    spec.active_set_attestation.filled_position_contract_count +=
        has_role(OptionScenarioContractRole::FilledPosition) ? 1U : 0U;
    spec.active_set_attestation.working_order_contract_count +=
        has_role(OptionScenarioContractRole::WorkingOrder) ? 1U : 0U;
    spec.active_set_attestation.pending_cancel_contract_count +=
        has_role(OptionScenarioContractRole::PendingCancel) ? 1U : 0U;
  }
  spec.limits.max_contracts = contract_count;
  spec.limits.max_underliers = underlier_count;
  spec.limits.max_scenarios = scenario_count;
  spec.limits.max_scenario_cells = checked_product(contract_count, scenario_count);
  spec.limits.max_surface_age_ns = 5LL * 60LL * kSecondNs;
  spec.limits.max_market_age_ns = 5LL * 60LL * kSecondNs;
  spec.limits.max_vol_floor_hits = 0U;
  return spec;
}

struct ScenarioCubeFixture {
  ScenarioCubeFixture(std::size_t contract_count, std::size_t scenario_count, unsigned n_threads)
      : underlier_count{(std::min)(contract_count, kMaximumFixtureUnderliers)}, directory{},
        panel{make_panel(contract_count, underlier_count)},
        active_contracts{make_active_contracts(panel)},
        snapshot{make_snapshot(directory.path(), underlier_count)},
        manifest{make_manifest(scenario_count, underlier_count)},
        spec{make_build_spec(contract_count, underlier_count, scenario_count, active_contracts,
                             n_threads)} {}

  std::size_t underlier_count;
  TempDirectory directory;
  OptionResearchPanel panel;
  std::vector<OptionScenarioActiveContract> active_contracts;
  atx::vol::MarketSnapshot snapshot;
  OptionScenarioManifest manifest;
  OptionScenarioCubeBuildSpec spec;
};

[[nodiscard]] std::size_t positive_size_argument(std::int64_t value, std::string_view name) {
  if (value <= 0) {
    throw std::invalid_argument{std::string{name} + " must be positive"};
  }
  return static_cast<std::size_t>(value);
}

void BM_CompileOptionScenarioCube(benchmark::State &state) {
  const std::size_t contract_count = positive_size_argument(state.range(0), "contract count");
  const std::size_t scenario_count = positive_size_argument(state.range(1), "scenario count");
  const std::size_t thread_count = positive_size_argument(state.range(2), "thread count");
  if (thread_count > static_cast<std::size_t>((std::numeric_limits<unsigned>::max)())) {
    state.SkipWithError("thread count exceeds unsigned");
    return;
  }
  ScenarioCubeFixture fixture{contract_count, scenario_count, static_cast<unsigned>(thread_count)};
  std::size_t last_vol_floor_hit_count = 0U;

  for (auto iteration : state) {
    (void)iteration;
    auto cube = compile_option_scenario_cube(fixture.active_contracts, fixture.snapshot,
                                             fixture.manifest, fixture.spec);
    if (!cube) {
      const std::string error = cube.error().to_string();
      state.SkipWithError(error.c_str());
      break;
    }
    last_vol_floor_hit_count = cube->report.vol_floor_hit_count;
    benchmark::DoNotOptimize(cube->panel.definition_hash());
    benchmark::ClobberMemory();
  }

  const double iterations = static_cast<double>(state.iterations());
  const double contracts = static_cast<double>(contract_count);
  const double cells = static_cast<double>(checked_product(contract_count, scenario_count));
  state.counters["compilations_per_second"] =
      benchmark::Counter(iterations, benchmark::Counter::kIsRate);
  state.counters["contracts_per_second"] =
      benchmark::Counter(iterations * contracts, benchmark::Counter::kIsRate);
  state.counters["scenario_cells_per_second"] =
      benchmark::Counter(iterations * cells, benchmark::Counter::kIsRate);
  state.counters["pnl_bytes_per_second"] = benchmark::Counter(
      iterations * cells * static_cast<double>(sizeof(double)), benchmark::Counter::kIsRate);
  state.counters["contracts"] = benchmark::Counter(contracts);
  state.counters["scenarios"] = benchmark::Counter(static_cast<double>(scenario_count));
  state.counters["underliers"] = benchmark::Counter(static_cast<double>(fixture.underlier_count));
  state.counters["compiler_threads"] = benchmark::Counter(static_cast<double>(thread_count));
  state.counters["vol_floor_hits"] =
      benchmark::Counter(static_cast<double>(last_vol_floor_hit_count));
  state.counters["max_vol_floor_hits"] =
      benchmark::Counter(static_cast<double>(fixture.spec.limits.max_vol_floor_hits));
}

BENCHMARK(BM_CompileOptionScenarioCube)
    ->Args({128, 8, 1})
    ->Args({512, 16, 1})
    ->Args({2'048, 32, 1})
    ->Args({2'048, 32, 4})
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

} // namespace
