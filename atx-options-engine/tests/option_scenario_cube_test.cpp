#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/options/option_scenario_cube.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/greeks.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "atx/vol/api/fitting/surface_parity.hpp"
#include "atx/vol/api/fitting/surface_policy.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"

namespace {

namespace fs = std::filesystem;

using atx::options::research::OptionPanelRow;
using atx::options::research::OptionResearchPanel;
using atx::options::risk::compile_option_scenario_cube;
using atx::options::risk::OptionScenarioActiveContract;
using atx::options::risk::OptionScenarioContractRole;
using atx::options::risk::OptionScenarioCube;
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
constexpr double kNsPerYear = 365.25 * 86'400.0 * 1.0e9;
constexpr std::uint32_t kUid = 17U;
constexpr double kSpot = 100.0;
constexpr double kRate = 0.04;
constexpr double kYield = 0.015;

static_assert(!std::is_copy_constructible_v<OptionScenarioCube>);
static_assert(!std::is_copy_assignable_v<OptionScenarioCube>);
static_assert(std::is_move_constructible_v<OptionScenarioCube>);

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{10'000U + seed, 20'000U + seed,
                                static_cast<std::uint32_t>(30'000U + seed),
                                static_cast<std::uint32_t>(40'000U + seed)};
}

class TempDir {
public:
  explicit TempDir(const char *tag) {
    static std::atomic<std::uint64_t> next{1U};
    path_ = fs::temp_directory_path() /
            (std::string{"atx-option-scenario-"} + tag + "-" + std::to_string(next.fetch_add(1U)));
    std::error_code error;
    fs::remove_all(path_, error);
    fs::create_directories(path_, error);
  }

  ~TempDir() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_{};
};

[[nodiscard]] atx::vol::PricedSurface
make_surface(std::uint32_t uid = kUid, double spot = kSpot,
             atx::vol::AmericanMethod method = atx::vol::AmericanMethod::AndersenLake,
             double rate = kRate, double yield = kYield) {
  atx::vol::CurveSurface curves;
  std::vector<atx::vol::SliceContext> contexts;
  constexpr double tenors[]{0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  for (std::size_t index = 0; index < std::size(tenors); ++index) {
    const double tenor = tenors[index];
    const double forward = spot * std::exp((rate - yield) * tenor);
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
    curves.push(std::make_unique<atx::vol::EssviCurve>(params, std::exp(-rate * tenor)));
    contexts.push_back(atx::vol::SliceContext{tenor, forward, 0.0, yield, 250, 7});
  }
  atx::vol::PricingContext pricing;
  pricing.S = spot;
  pricing.r = rate;
  pricing.now_ts_ns = kNow;
  pricing.method = method;
  pricing.al_opts = atx::vol::al_fast_opts();
  pricing.uid = uid;
  auto result = atx::vol::PricedSurface::create(std::move(curves), std::move(contexts), pricing);
  EXPECT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  return std::move(*result);
}

[[nodiscard]] atx::vol::SurfaceProvenance healthy_provenance() {
  atx::vol::SurfaceProvenance provenance;
  provenance.purpose = atx::vol::SurfacePurpose::Risk;
  provenance.quality_mode = atx::vol::FitQualityMode::Balanced;
  provenance.state = atx::vol::SurfaceState::Healthy;
  provenance.validation.validation_id = 77U;
  provenance.source_generation = 3U;
  provenance.served_generation = 3U;
  return provenance;
}

[[nodiscard]] std::string
write_archive(const fs::path &directory, const atx::vol::PricedSurface &surface,
              atx::vol::SurfaceProvenance provenance = healthy_provenance()) {
  const std::string path = (directory / "surface.atxvsa").string();
  const atx::vol::SurfaceArchiveItem item{"IDX", &surface, provenance};
  const atx::vol::Status status =
      atx::vol::write_surface_archive_v2_file(path, std::span{&item, 1U});
  EXPECT_TRUE(status) << (status ? std::string{} : status.error().to_string());
  return path;
}

[[nodiscard]] OptionPanelRow panel_row(std::uint64_t contract_id, ExerciseStyle exercise_style,
                                       Side side, double strike, std::uint32_t uid = kUid) {
  constexpr std::int64_t decision = kNow + 60 * kSecondNs;
  OptionPanelRow row;
  row.observation.uid = uid;
  row.observation.observed_ts_ns = kNow + 5 * kSecondNs;
  row.observation.available_ts_ns = kNow + 10 * kSecondNs;
  row.observation.decision_ts_ns = decision;
  row.observation.execution_ts_ns = decision + kSecondNs;
  row.observation.label_end_ts_ns = decision + kDayNs;
  row.observation.signal = contract_id == 101U ? 1.0 : -1.0;
  row.observation.forward_pnl = 5.0;
  row.observation.lagged_capital = 1'000'000.0;
  row.observation.source_identity = identity(contract_id + 1U);
  row.contract_id = contract_id;
  row.engine_id.id = static_cast<std::uint32_t>(contract_id);
  row.definition_available_ts_ns = kNow;
  row.quote_event_ts_ns = kNow + 20 * kSecondNs;
  row.quote_available_ts_ns = kNow + 25 * kSecondNs;
  row.expiry_ts_ns = decision + 90 * kDayNs;
  row.strike = strike;
  row.side = side;
  row.exercise_style = exercise_style;
  row.multiplier = 100.0;
  row.standard_deliverable = true;
  row.mark = 8.0;
  row.bid = 7.9;
  row.ask = 8.1;
  row.bid_size_contracts = 100.0;
  row.ask_size_contracts = 100.0;
  row.interval_volume_contracts = 1'000.0;
  row.lagged_open_interest_contracts = 10'000.0;
  row.adv_contracts = 20'000.0;
  row.return_sigma = 0.20;
  row.vega_per_contract = 20.0;
  row.initial_margin_per_contract = 1'000.0;
  row.maintenance_margin_per_contract = 800.0;
  row.definition_source_identity = identity(contract_id + 2U);
  row.feature_source_identity = identity(contract_id + 3U);
  row.execution_source_identity = identity(contract_id + 4U);
  return row;
}

[[nodiscard]] OptionResearchPanel make_panel(bool reverse = false) {
  std::vector<OptionPanelRow> rows{
      panel_row(101U, ExerciseStyle::American, Side::Put, 105.0),
      panel_row(202U, ExerciseStyle::European, Side::Call, 100.0),
  };
  if (reverse) {
    std::reverse(rows.begin(), rows.end());
  }
  auto panel = OptionResearchPanel::create(rows);
  EXPECT_TRUE(panel) << (panel ? std::string{} : panel.error().to_string());
  return std::move(*panel);
}

[[nodiscard]] std::vector<OptionScenarioActiveContract>
active_catalog(const OptionResearchPanel &panel) {
  std::vector<OptionScenarioActiveContract> result;
  result.reserve(panel.instruments().size());
  const std::span<const double> marks =
      panel.row(atx::options::research::OptionPanelField::Mark, 0U);
  for (std::size_t index = 0U; index < panel.instruments().size(); ++index) {
    const OptionScenarioContractRole role =
        index == 0U
            ? OptionScenarioContractRole::Candidate | OptionScenarioContractRole::FilledPosition
            : OptionScenarioContractRole::WorkingOrder | OptionScenarioContractRole::PendingCancel;
    result.push_back(OptionScenarioActiveContract{
        panel.instruments()[index], panel.decision_audit()[index], marks[index], role});
  }
  return result;
}

[[nodiscard]] OptionScenarioManifest manifest(bool reverse = false) {
  OptionScenarioManifest result;
  result.minimum_implied_vol = 0.005;
  result.observed_ts_ns = kNow + 15 * kSecondNs;
  result.available_ts_ns = kNow + 35 * kSecondNs;
  result.effective_ts_ns = kNow;
  result.artifact_identity = identity(900U);
  result.scenarios = {
      OptionScenarioDefinition{11U, 0, 0.0},
      OptionScenarioDefinition{22U, 0, 0.01},
      OptionScenarioDefinition{33U, 120 * kDayNs, 0.0},
  };
  result.underlier_shocks = {
      OptionScenarioUnderlierShock{11U, kUid, 0.0, 0.0},
      OptionScenarioUnderlierShock{22U, kUid, -0.20, 0.10},
      OptionScenarioUnderlierShock{33U, kUid, 0.10, -0.02},
  };
  if (reverse) {
    std::reverse(result.scenarios.begin(), result.scenarios.end());
    std::reverse(result.underlier_shocks.begin(), result.underlier_shocks.end());
  }
  return result;
}

[[nodiscard]] OptionScenarioCubeBuildSpec build_spec(unsigned threads = 1U) {
  OptionScenarioCubeBuildSpec spec;
  spec.surface_available_ts_ns = kNow + 30 * kSecondNs;
  spec.n_threads = threads;
  spec.active_set_attestation.observed_ts_ns = kNow + 26 * kSecondNs;
  spec.active_set_attestation.available_ts_ns = kNow + 27 * kSecondNs;
  spec.active_set_attestation.source_identity = identity(800U);
  spec.active_set_attestation.candidate_contract_count = 1U;
  spec.active_set_attestation.filled_position_contract_count = 1U;
  spec.active_set_attestation.working_order_contract_count = 1U;
  spec.active_set_attestation.pending_cancel_contract_count = 1U;
  spec.limits.max_surface_age_ns = 5 * 60 * kSecondNs;
  spec.limits.max_market_age_ns = 5 * 60 * kSecondNs;
  return spec;
}

struct Fixture {
  explicit Fixture(const char *tag, atx::vol::SurfaceProvenance provenance = healthy_provenance())
      : directory{tag}, surface{make_surface()} {
    const std::string path = write_archive(directory.path(), surface, provenance);
    auto loaded = atx::vol::MarketSnapshot::load(path, atx::vol::QueryPricingTier::ColdReference);
    EXPECT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().to_string());
    snapshot.emplace(std::move(*loaded));
  }

  TempDir directory;
  atx::vol::PricedSurface surface;
  std::optional<atx::vol::MarketSnapshot> snapshot;
};

TEST(OptionScenarioCube, FullRepricesAmericanAndEuropeanAndCrossesExpiry) {
  Fixture fixture{"exact"};
  OptionResearchPanel panel = make_panel();
  auto cube = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot, manifest(),
                                           build_spec());
  ASSERT_TRUE(cube) << (cube ? std::string{} : cube.error().to_string());

  EXPECT_EQ(cube->report.contract_count, 2U);
  EXPECT_EQ(cube->report.underlier_count, 1U);
  EXPECT_EQ(cube->report.scenario_count, 3U);
  EXPECT_EQ(cube->report.american_contract_count, 1U);
  EXPECT_EQ(cube->report.european_contract_count, 1U);
  EXPECT_EQ(cube->report.candidate_contract_count, 1U);
  EXPECT_EQ(cube->report.filled_position_contract_count, 1U);
  EXPECT_EQ(cube->report.working_order_contract_count, 1U);
  EXPECT_EQ(cube->report.pending_cancel_contract_count, 1U);
  ASSERT_EQ(cube->panel.scenario_ids().size(), 3U);
  EXPECT_EQ(cube->panel.scenario_ids()[0], 11U);
  EXPECT_EQ(cube->panel.scenario_pnl(0U, 0U, 0U), 0.0);
  EXPECT_EQ(cube->panel.scenario_pnl(0U, 0U, 1U), 0.0);
  EXPECT_EQ(cube->panel.contract_row(0U, 0U).exercise_style, ExerciseStyle::American);
  EXPECT_EQ(cube->panel.contract_row(0U, 1U).exercise_style, ExerciseStyle::European);

  const auto &american = panel.instruments()[0];
  const double tenor =
      static_cast<double>(american.expiry_ts_ns - panel.dataset().dates()[0]) / kNsPerYear;
  const atx::vol::SurfaceRef surface = fixture.snapshot->find(kUid);
  const auto point = surface->resolve(american.strike, tenor);
  ASSERT_TRUE(point.valid);
  auto base = atx::vol::american_price(kSpot, american.strike, tenor, point.sigma, point.rate,
                                       point.q_eff, american.side, surface->pricing().method,
                                       std::optional<atx::vol::AlOpts>{surface->pricing().al_opts});
  auto shocked = atx::vol::american_price(
      kSpot * 0.80, american.strike, tenor, point.sigma + 0.10, point.rate + 0.01, point.q_eff,
      american.side, surface->pricing().method,
      std::optional<atx::vol::AlOpts>{surface->pricing().al_opts});
  ASSERT_TRUE(base);
  ASSERT_TRUE(shocked);
  const double expected = (*shocked - *base) * american.multiplier;
  EXPECT_DOUBLE_EQ(cube->panel.scenario_pnl(0U, 1U, 0U), expected);
  auto american_greeks = surface->greeks(american.strike, tenor, american.side,
                                         atx::vol::QueryExecution::ColdReference);
  ASSERT_TRUE(american_greeks);
  const auto &american_risk = cube->panel.contract_row(0U, 0U);
  EXPECT_DOUBLE_EQ(american_risk.premium_cash_notional_per_contract, 8.0 * american.multiplier);
  EXPECT_EQ(american_risk.market_observed_ts_ns, panel.decision_audit()[0].quote_event_ts_ns);
  EXPECT_EQ(american_risk.market_available_ts_ns, panel.decision_audit()[0].quote_available_ts_ns);
  EXPECT_EQ(american_risk.market_source_identity,
            panel.decision_audit()[0].execution_source_identity);
  EXPECT_DOUBLE_EQ(american_risk.spot_delta_cash_per_contract,
                   american_greeks->delta * kSpot * american.multiplier);
  EXPECT_DOUBLE_EQ(american_risk.spot_gamma_cash_per_contract,
                   american_greeks->gamma * kSpot * kSpot * american.multiplier);
  EXPECT_DOUBLE_EQ(american_risk.vega_cash_per_vol_point_per_contract,
                   american_greeks->vega * 0.01 * american.multiplier);
  EXPECT_DOUBLE_EQ(american_risk.theta_cash_per_day_per_contract,
                   american_greeks->theta / 365.25 * american.multiplier);
  EXPECT_DOUBLE_EQ(american_risk.vanna_cash_per_return_vol_point_per_contract,
                   american_greeks->vanna * kSpot * 0.01 * american.multiplier);
  EXPECT_DOUBLE_EQ(american_risk.volga_cash_per_vol_point_squared_per_contract,
                   american_greeks->volga * 0.0001 * american.multiplier);

  const auto &european = panel.instruments()[1];
  const double european_tenor =
      static_cast<double>(european.expiry_ts_ns - panel.dataset().dates()[0]) / kNsPerYear;
  const auto european_point = surface->resolve(european.strike, european_tenor);
  ASSERT_TRUE(european_point.valid);
  const double european_forward = surface->forward_at(european_tenor);
  const double european_discount = std::exp(-european_point.rate * european_tenor);
  const atx::vol::Black76Greeks european_base = atx::vol::black76_greeks(
      european_forward, european.strike, european_tenor, european_point.sigma, european_point.rate,
      european_discount, european.side);
  const double shocked_forward =
      kSpot * 0.80 * std::exp((european_point.rate + 0.01 - european_point.q_eff) * european_tenor);
  const double european_shocked = atx::vol::black76_price(
      shocked_forward, european.strike, european_tenor, european_point.sigma + 0.10,
      std::exp(-(european_point.rate + 0.01) * european_tenor), european.side);
  EXPECT_DOUBLE_EQ(cube->panel.scenario_pnl(0U, 1U, 1U),
                   (european_shocked - european_base.price) * european.multiplier);
  const auto &european_risk = cube->panel.contract_row(0U, 1U);
  EXPECT_DOUBLE_EQ(european_risk.spot_delta_cash_per_contract,
                   european_base.greeks.delta * european_forward * european.multiplier);
  EXPECT_DOUBLE_EQ(european_risk.spot_gamma_cash_per_contract,
                   european_base.greeks.gamma * european_forward * european_forward *
                       european.multiplier);
  EXPECT_DOUBLE_EQ(european_risk.vega_cash_per_vol_point_per_contract,
                   european_base.greeks.vega * 0.01 * european.multiplier);
  const double european_spot_theta =
      european_base.greeks.theta -
      (european_point.rate - european_point.q_eff) * european_forward * european_base.greeks.delta;
  EXPECT_DOUBLE_EQ(european_risk.theta_cash_per_day_per_contract,
                   european_spot_theta / 365.25 * european.multiplier);
  EXPECT_DOUBLE_EQ(european_risk.vanna_cash_per_return_vol_point_per_contract,
                   european_base.greeks.vanna * european_forward * 0.01 * european.multiplier);
  EXPECT_DOUBLE_EQ(european_risk.volga_cash_per_vol_point_squared_per_contract,
                   european_base.greeks.volga * 0.0001 * european.multiplier);

  const double american_expiry = (std::max)(kSpot * 1.10 - american.strike, 0.0);
  const double expected_expiry =
      (american.side == Side::Call ? american_expiry
                                   : (std::max)(american.strike - kSpot * 1.10, 0.0)) *
      american.multiplier;
  EXPECT_DOUBLE_EQ(cube->panel.scenario_pnl(0U, 2U, 0U),
                   expected_expiry - *base * american.multiplier);
}

TEST(OptionScenarioCube, PreservesArchivedBawAmericanMethod) {
  TempDir directory{"baw"};
  atx::vol::PricedSurface surface = make_surface(kUid, kSpot, atx::vol::AmericanMethod::Baw);
  const std::string path = write_archive(directory.path(), surface);
  auto snapshot = atx::vol::MarketSnapshot::load(path, atx::vol::QueryPricingTier::ColdReference);
  ASSERT_TRUE(snapshot);
  OptionResearchPanel panel = make_panel();
  auto cube =
      compile_option_scenario_cube(active_catalog(panel), *snapshot, manifest(), build_spec());
  ASSERT_TRUE(cube) << (cube ? std::string{} : cube.error().to_string());

  const auto &instrument = panel.instruments()[0];
  const double tenor =
      static_cast<double>(instrument.expiry_ts_ns - panel.dataset().dates()[0]) / kNsPerYear;
  const auto point = snapshot->find(kUid)->resolve(instrument.strike, tenor);
  ASSERT_TRUE(point.valid);
  auto base = atx::vol::american_price(kSpot, instrument.strike, tenor, point.sigma, point.rate,
                                       point.q_eff, instrument.side, atx::vol::AmericanMethod::Baw);
  auto shocked = atx::vol::american_price(kSpot * 0.80, instrument.strike, tenor,
                                          point.sigma + 0.10, point.rate + 0.01, point.q_eff,
                                          instrument.side, atx::vol::AmericanMethod::Baw);
  ASSERT_TRUE(base);
  ASSERT_TRUE(shocked);
  EXPECT_DOUBLE_EQ(cube->panel.scenario_pnl(0U, 1U, 0U),
                   (*shocked - *base) * instrument.multiplier);
}

TEST(OptionScenarioCube, FailsWholeCubeWhenBaseAmericanGreeksAreUnavailable) {
  TempDir directory{"base-failure"};
  atx::vol::PricedSurface surface =
      make_surface(kUid, kSpot, atx::vol::AmericanMethod::AndersenLake, -0.005, -0.02);
  const std::string path = write_archive(directory.path(), surface);
  auto snapshot = atx::vol::MarketSnapshot::load(path, atx::vol::QueryPricingTier::ColdReference);
  ASSERT_TRUE(snapshot);
  OptionResearchPanel panel = make_panel();

  auto result =
      compile_option_scenario_cube(active_catalog(panel), *snapshot, manifest(), build_spec());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_NE(result.error().message().find("base pricing"), std::string::npos);
}

TEST(OptionScenarioCube, AppliesExplicitSimultaneousUnderlierShocks) {
  constexpr std::uint32_t other_uid = kUid + 1U;
  TempDir directory{"multi-underlier"};
  atx::vol::PricedSurface first = make_surface(kUid, 100.0);
  atx::vol::PricedSurface second = make_surface(other_uid, 50.0);
  const atx::vol::SurfaceProvenance provenance = healthy_provenance();
  const std::array items{
      atx::vol::SurfaceArchiveItem{"AAA", &first, provenance},
      atx::vol::SurfaceArchiveItem{"BBB", &second, provenance},
  };
  const std::string path = (directory.path() / "surface.atxvsa").string();
  const atx::vol::Status written = atx::vol::write_surface_archive_v2_file(path, items);
  ASSERT_TRUE(written);
  auto snapshot = atx::vol::MarketSnapshot::load(path, atx::vol::QueryPricingTier::ColdReference);
  ASSERT_TRUE(snapshot);

  std::vector<OptionPanelRow> rows{
      panel_row(101U, ExerciseStyle::American, Side::Call, 100.0, kUid),
      panel_row(303U, ExerciseStyle::American, Side::Call, 50.0, other_uid),
  };
  auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel);
  OptionScenarioManifest simultaneous;
  simultaneous.minimum_implied_vol = 0.005;
  simultaneous.observed_ts_ns = kNow + 15 * kSecondNs;
  simultaneous.available_ts_ns = kNow + 35 * kSecondNs;
  simultaneous.effective_ts_ns = kNow;
  simultaneous.artifact_identity = identity(901U);
  simultaneous.scenarios = {OptionScenarioDefinition{44U, 0, 0.0}};
  simultaneous.underlier_shocks = {
      OptionScenarioUnderlierShock{44U, kUid, 0.10, 0.0},
      OptionScenarioUnderlierShock{44U, other_uid, -0.10, 0.0},
  };
  std::reverse(simultaneous.underlier_shocks.begin(), simultaneous.underlier_shocks.end());
  auto cube = compile_option_scenario_cube(active_catalog(*panel), *snapshot,
                                           std::move(simultaneous), build_spec());
  ASSERT_TRUE(cube) << (cube ? std::string{} : cube.error().to_string());
  EXPECT_GT(cube->panel.scenario_pnl(0U, 0U, 0U), 0.0);
  EXPECT_LT(cube->panel.scenario_pnl(0U, 0U, 1U), 0.0);
}

TEST(OptionScenarioCube, DenseFactoryMatchesExpandedPanelAndFeedsRiskEngine) {
  Fixture fixture{"dense"};
  OptionResearchPanel panel = make_panel();
  auto cube = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot, manifest(),
                                           build_spec());
  ASSERT_TRUE(cube);

  std::vector<atx::options::risk::OptionRiskContractRow> rows;
  for (std::size_t contract = 0; contract < cube->panel.contract_count(); ++contract) {
    rows.push_back(cube->panel.contract_row(0U, contract));
  }
  std::vector<atx::options::risk::OptionRiskScenario> scenarios;
  for (std::uint64_t scenario_id : cube->panel.scenario_ids()) {
    scenarios.push_back(atx::options::risk::OptionRiskScenario{scenario_id, identity(900U)});
  }
  std::vector<atx::options::risk::OptionRiskScenarioPnlRow> pnl_rows;
  for (std::size_t scenario = 0; scenario < cube->panel.scenario_count(); ++scenario) {
    for (std::size_t contract = 0; contract < cube->panel.contract_count(); ++contract) {
      pnl_rows.push_back(atx::options::risk::OptionRiskScenarioPnlRow{
          panel.dataset().dates()[0], panel.instruments()[contract].contract_id,
          cube->panel.scenario_ids()[scenario], manifest().observed_ts_ns,
          manifest().available_ts_ns, cube->panel.scenario_pnl(0U, scenario, contract),
          identity(900U)});
    }
  }
  auto expanded = atx::options::risk::OptionRiskPanel::create(rows, scenarios, pnl_rows,
                                                              cube->panel.provenance());
  ASSERT_TRUE(expanded) << (expanded ? std::string{} : expanded.error().to_string());
  EXPECT_EQ(expanded->definition_hash(), cube->panel.definition_hash());

  auto engine = atx::options::risk::OptionPreTradeRiskEngine::create();
  ASSERT_TRUE(engine);
  const std::vector<std::int64_t> filled{1, -1};
  const std::vector<atx::options::risk::OptionRiskLeaf> no_leaves;
  auto evaluation = engine->evaluate(cube->panel, 0U, panel.instruments(), filled, no_leaves,
                                     no_leaves, atx::options::risk::OptionRiskHardLimits{});
  ASSERT_TRUE(evaluation) << (evaluation ? std::string{} : evaluation.error().to_string());
  EXPECT_EQ(evaluation->disposition, atx::options::risk::OptionRiskDisposition::Accept);
}

TEST(OptionScenarioCube, DenseFactoryRejectsNoncanonicalInputs) {
  Fixture fixture{"dense-invalid"};
  OptionResearchPanel panel = make_panel();
  auto cube = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot, manifest(),
                                           build_spec());
  ASSERT_TRUE(cube);

  std::vector<atx::options::risk::OptionRiskContractRow> rows;
  for (std::size_t contract = 0; contract < cube->panel.contract_count(); ++contract) {
    rows.push_back(cube->panel.contract_row(0U, contract));
  }
  std::vector<atx::options::risk::OptionRiskScenario> scenarios;
  for (std::uint64_t scenario_id : cube->panel.scenario_ids()) {
    scenarios.push_back(atx::options::risk::OptionRiskScenario{scenario_id, identity(900U)});
  }
  std::vector<double> pnl;
  std::vector<atx::options::risk::OptionRiskGeneratedPnlLineage> lineage;
  for (std::size_t scenario = 0; scenario < cube->panel.scenario_count(); ++scenario) {
    lineage.push_back(atx::options::risk::OptionRiskGeneratedPnlLineage{
        panel.dataset().dates()[0], cube->panel.scenario_ids()[scenario], fixture.snapshot->ts_ns(),
        build_spec().surface_available_ts_ns, identity(900U)});
    for (std::size_t contract = 0; contract < cube->panel.contract_count(); ++contract) {
      pnl.push_back(cube->panel.scenario_pnl(0U, scenario, contract));
    }
  }

  std::vector<atx::options::risk::OptionRiskContractRow> reversed_rows = rows;
  std::reverse(reversed_rows.begin(), reversed_rows.end());
  auto bad_rows = atx::options::risk::OptionRiskPanel::create_generated_canonical(
      std::move(reversed_rows), scenarios, pnl, lineage, cube->panel.provenance());
  ASSERT_FALSE(bad_rows);
  EXPECT_EQ(bad_rows.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<atx::options::risk::OptionRiskScenario> reversed_scenarios = scenarios;
  std::reverse(reversed_scenarios.begin(), reversed_scenarios.end());
  auto bad_scenarios = atx::options::risk::OptionRiskPanel::create_generated_canonical(
      rows, std::move(reversed_scenarios), pnl, lineage, cube->panel.provenance());
  ASSERT_FALSE(bad_scenarios);
  EXPECT_EQ(bad_scenarios.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<double> nonfinite = pnl;
  nonfinite[0] = std::numeric_limits<double>::quiet_NaN();
  auto bad_pnl = atx::options::risk::OptionRiskPanel::create_generated_canonical(
      rows, scenarios, std::move(nonfinite), lineage, cube->panel.provenance());
  ASSERT_FALSE(bad_pnl);
  EXPECT_EQ(bad_pnl.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<atx::options::risk::OptionRiskGeneratedPnlLineage> bad_lineage = lineage;
  bad_lineage[0].scenario_id += 1U;
  auto bad_source = atx::options::risk::OptionRiskPanel::create_generated_canonical(
      rows, scenarios, pnl, std::move(bad_lineage), cube->panel.provenance());
  ASSERT_FALSE(bad_source);
  EXPECT_EQ(bad_source.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionScenarioCube, CanonicalizesPermutationsAndThreadCounts) {
  Fixture fixture{"determinism"};
  OptionResearchPanel forward_panel = make_panel();
  OptionResearchPanel reverse_panel = make_panel(true);
  auto one = compile_option_scenario_cube(active_catalog(forward_panel), *fixture.snapshot,
                                          manifest(), build_spec(1U));
  auto four = compile_option_scenario_cube(active_catalog(reverse_panel), *fixture.snapshot,
                                           manifest(true), build_spec(4U));
  auto automatic = compile_option_scenario_cube(active_catalog(forward_panel), *fixture.snapshot,
                                                manifest(), build_spec(0U));
  ASSERT_TRUE(one);
  ASSERT_TRUE(four);
  ASSERT_TRUE(automatic);
  EXPECT_EQ(one->report.scenario_manifest_digest, four->report.scenario_manifest_digest);
  EXPECT_EQ(one->report.risk_snapshot_digest, four->report.risk_snapshot_digest);
  EXPECT_EQ(one->panel.definition_hash(), four->panel.definition_hash());
  EXPECT_EQ(one->report.risk_snapshot_digest, automatic->report.risk_snapshot_digest);
  EXPECT_EQ(one->panel.definition_hash(), automatic->panel.definition_hash());
  for (std::size_t scenario = 0; scenario < one->panel.scenario_count(); ++scenario) {
    for (std::size_t contract = 0; contract < one->panel.contract_count(); ++contract) {
      EXPECT_DOUBLE_EQ(one->panel.scenario_pnl(0U, scenario, contract),
                       four->panel.scenario_pnl(0U, scenario, contract));
    }
  }
}

TEST(OptionScenarioCube, NormalizesSignedZeroInSemanticDigests) {
  Fixture fixture{"signed-zero"};
  OptionResearchPanel panel = make_panel();
  OptionScenarioManifest positive = manifest();
  OptionScenarioManifest negative = manifest();
  negative.scenarios[0].rate_shift = -0.0;
  negative.underlier_shocks[0].spot_return = -0.0;
  negative.underlier_shocks[0].vol_level_shift = -0.0;

  auto first = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot,
                                            std::move(positive), build_spec());
  auto second = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot,
                                             std::move(negative), build_spec());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->report.scenario_manifest_digest, second->report.scenario_manifest_digest);
  EXPECT_EQ(first->report.risk_snapshot_digest, second->report.risk_snapshot_digest);
  EXPECT_EQ(first->panel.definition_hash(), second->panel.definition_hash());
}

TEST(OptionScenarioCube, DigestsBindLifecycleRolesAndManifestEffectiveTime) {
  Fixture fixture{"digest-lineage"};
  OptionResearchPanel panel = make_panel();
  std::vector<OptionScenarioActiveContract> baseline_active = active_catalog(panel);
  auto baseline =
      compile_option_scenario_cube(baseline_active, *fixture.snapshot, manifest(), build_spec());
  ASSERT_TRUE(baseline);

  std::vector<OptionScenarioActiveContract> changed_roles = baseline_active;
  changed_roles[0].role_mask = OptionScenarioContractRole::Candidate;
  changed_roles[1].role_mask = OptionScenarioContractRole::FilledPosition |
                               OptionScenarioContractRole::WorkingOrder |
                               OptionScenarioContractRole::PendingCancel;
  auto roles =
      compile_option_scenario_cube(changed_roles, *fixture.snapshot, manifest(), build_spec());
  ASSERT_TRUE(roles);
  EXPECT_EQ(baseline->report.scenario_manifest_digest, roles->report.scenario_manifest_digest);
  EXPECT_NE(baseline->report.risk_snapshot_digest, roles->report.risk_snapshot_digest);
  EXPECT_NE(baseline->panel.definition_hash(), roles->panel.definition_hash());

  OptionScenarioManifest changed_effective = manifest();
  changed_effective.effective_ts_ns += kSecondNs;
  auto effective = compile_option_scenario_cube(baseline_active, *fixture.snapshot,
                                                std::move(changed_effective), build_spec());
  ASSERT_TRUE(effective);
  EXPECT_NE(baseline->report.scenario_manifest_digest, effective->report.scenario_manifest_digest);
  EXPECT_NE(baseline->report.risk_snapshot_digest, effective->report.risk_snapshot_digest);

  OptionScenarioCubeBuildSpec changed_attestation = build_spec();
  changed_attestation.active_set_attestation.observed_ts_ns += 1;
  changed_attestation.active_set_attestation.source_identity = identity(801U);
  auto attestation = compile_option_scenario_cube(baseline_active, *fixture.snapshot, manifest(),
                                                  changed_attestation);
  ASSERT_TRUE(attestation);
  EXPECT_EQ(baseline->report.scenario_manifest_digest,
            attestation->report.scenario_manifest_digest);
  EXPECT_NE(baseline->report.risk_snapshot_digest, attestation->report.risk_snapshot_digest);
  EXPECT_NE(baseline->panel.definition_hash(), attestation->panel.definition_hash());
}

TEST(OptionScenarioCube, AuditsAndBoundsVolatilityFloorHits) {
  Fixture fixture{"vol-floor"};
  OptionResearchPanel panel = make_panel();
  OptionScenarioManifest floored = manifest();
  floored.underlier_shocks[0].vol_level_shift = -10.0;

  auto rejected =
      compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot, floored, build_spec());
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioCubeBuildSpec allowed = build_spec();
  allowed.limits.max_vol_floor_hits = panel.instruments().size();
  auto admitted = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot,
                                               std::move(floored), allowed);
  ASSERT_TRUE(admitted) << (admitted ? std::string{} : admitted.error().to_string());
  EXPECT_EQ(admitted->report.vol_floor_hit_count, panel.instruments().size());
}

TEST(OptionScenarioCube, FailsWholeCubeOnNonfiniteShockedPrice) {
  Fixture fixture{"overflow"};
  OptionResearchPanel panel = make_panel();
  OptionScenarioManifest overflow = manifest();
  overflow.underlier_shocks[0].spot_return = (std::numeric_limits<double>::max)();

  auto result = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot,
                                             std::move(overflow), build_spec());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_NE(result.error().message().find("scenario 11 contract"), std::string::npos);
}

TEST(OptionScenarioCube, RejectsIncompleteOrNoncanonicalLifecycleCatalog) {
  Fixture fixture{"lifecycle"};
  OptionResearchPanel panel = make_panel();

  std::vector<OptionScenarioActiveContract> missing_role = active_catalog(panel);
  missing_role[0].role_mask = OptionScenarioContractRole::None;
  auto missing =
      compile_option_scenario_cube(missing_role, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<OptionScenarioActiveContract> reversed = active_catalog(panel);
  std::reverse(reversed.begin(), reversed.end());
  auto noncanonical =
      compile_option_scenario_cube(reversed, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(noncanonical);
  EXPECT_EQ(noncanonical.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<OptionScenarioActiveContract> duplicate = active_catalog(panel);
  duplicate[1] = duplicate[0];
  auto duplicate_result =
      compile_option_scenario_cube(duplicate, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(duplicate_result);
  EXPECT_EQ(duplicate_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<OptionScenarioActiveContract> unknown_role = active_catalog(panel);
  unknown_role[0].role_mask = static_cast<OptionScenarioContractRole>(0x80U);
  auto unknown_result =
      compile_option_scenario_cube(unknown_role, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(unknown_result);
  EXPECT_EQ(unknown_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<OptionScenarioActiveContract> future_definition = active_catalog(panel);
  future_definition[0].audit.definition_available_ts_ns =
      future_definition[0].audit.quote_event_ts_ns + 1;
  auto future_definition_result =
      compile_option_scenario_cube(future_definition, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(future_definition_result);
  EXPECT_EQ(future_definition_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  std::vector<OptionScenarioActiveContract> stale_status = active_catalog(panel);
  stale_status[0].audit.status = atx::options::research::OptionPanelStatus::StaleQuote;
  auto stale_status_result =
      compile_option_scenario_cube(stale_status, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(stale_status_result);
  EXPECT_EQ(stale_status_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioCubeBuildSpec stale_mark = build_spec();
  stale_mark.limits.max_market_age_ns = kSecondNs;
  auto stale_mark_result = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot,
                                                        manifest(), stale_mark);
  ASSERT_FALSE(stale_mark_result);
  EXPECT_EQ(stale_mark_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioCubeBuildSpec bad_attestation = build_spec();
  ++bad_attestation.active_set_attestation.candidate_contract_count;
  auto attestation_result = compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot,
                                                         manifest(), bad_attestation);
  ASSERT_FALSE(attestation_result);
  EXPECT_EQ(attestation_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  for (std::size_t mutation = 0U; mutation < 3U; ++mutation) {
    OptionScenarioCubeBuildSpec invalid_attestation = build_spec();
    if (mutation == 0U) {
      invalid_attestation.active_set_attestation.source_identity = {};
    } else if (mutation == 1U) {
      invalid_attestation.active_set_attestation.observed_ts_ns =
          invalid_attestation.active_set_attestation.available_ts_ns + 1;
    } else {
      invalid_attestation.active_set_attestation.available_ts_ns = panel.dataset().dates()[0] + 1;
    }
    auto invalid_attestation_result = compile_option_scenario_cube(
        active_catalog(panel), *fixture.snapshot, manifest(), invalid_attestation);
    ASSERT_FALSE(invalid_attestation_result) << "mutation=" << mutation;
    EXPECT_EQ(invalid_attestation_result.error().code(), atx::core::ErrorCode::InvalidArgument);
  }

  for (std::size_t mutation = 0U; mutation < 6U; ++mutation) {
    std::vector<OptionScenarioActiveContract> invalid_market = active_catalog(panel);
    if (mutation == 0U) {
      invalid_market[0].market_mark = 0.0;
    } else if (mutation == 1U) {
      invalid_market[0].market_mark = -1.0;
    } else if (mutation == 2U) {
      invalid_market[0].market_mark = std::numeric_limits<double>::quiet_NaN();
    } else if (mutation == 3U) {
      invalid_market[0].audit.quote_event_ts_ns = invalid_market[0].audit.quote_available_ts_ns + 1;
    } else if (mutation == 4U) {
      invalid_market[0].audit.quote_available_ts_ns = invalid_market[0].audit.decision_ts_ns + 1;
    } else {
      invalid_market[0].audit.execution_source_identity = {};
    }
    auto invalid_market_result =
        compile_option_scenario_cube(invalid_market, *fixture.snapshot, manifest(), build_spec());
    ASSERT_FALSE(invalid_market_result) << "mutation=" << mutation;
    EXPECT_EQ(invalid_market_result.error().code(), atx::core::ErrorCode::InvalidArgument);
  }

  std::vector<OptionScenarioActiveContract> adjusted = active_catalog(panel);
  adjusted[0].instrument.standard_deliverable = false;
  auto adjusted_result =
      compile_option_scenario_cube(adjusted, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(adjusted_result);
  EXPECT_EQ(adjusted_result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionScenarioCube, FailsClosedOnCoverageClocksAndProvenance) {
  Fixture healthy{"fail-closed"};
  OptionResearchPanel panel = make_panel();
  OptionScenarioManifest missing = manifest();
  missing.underlier_shocks.pop_back();
  auto missing_result = compile_option_scenario_cube(active_catalog(panel), *healthy.snapshot,
                                                     std::move(missing), build_spec());
  ASSERT_FALSE(missing_result);
  EXPECT_EQ(missing_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioCubeBuildSpec stale = build_spec();
  stale.limits.max_surface_age_ns = 10 * kSecondNs;
  auto stale_result =
      compile_option_scenario_cube(active_catalog(panel), *healthy.snapshot, manifest(), stale);
  ASSERT_FALSE(stale_result);
  EXPECT_EQ(stale_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioCubeBuildSpec future = build_spec();
  future.surface_available_ts_ns = panel.dataset().dates()[0] + 1;
  auto future_result =
      compile_option_scenario_cube(active_catalog(panel), *healthy.snapshot, manifest(), future);
  ASSERT_FALSE(future_result);
  EXPECT_EQ(future_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioManifest future_manifest = manifest();
  future_manifest.available_ts_ns = panel.dataset().dates()[0] + 1;
  auto future_manifest_result = compile_option_scenario_cube(
      active_catalog(panel), *healthy.snapshot, std::move(future_manifest), build_spec());
  ASSERT_FALSE(future_manifest_result);
  EXPECT_EQ(future_manifest_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioManifest inverted_manifest = manifest();
  inverted_manifest.observed_ts_ns = inverted_manifest.available_ts_ns + 1;
  auto inverted_manifest_result = compile_option_scenario_cube(
      active_catalog(panel), *healthy.snapshot, std::move(inverted_manifest), build_spec());
  ASSERT_FALSE(inverted_manifest_result);
  EXPECT_EQ(inverted_manifest_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioManifest future_effective = manifest();
  future_effective.effective_ts_ns = panel.dataset().dates()[0] + 1;
  auto effective_result = compile_option_scenario_cube(active_catalog(panel), *healthy.snapshot,
                                                       std::move(future_effective), build_spec());
  ASSERT_FALSE(effective_result);
  EXPECT_EQ(effective_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  OptionScenarioManifest invalid_shock = manifest();
  invalid_shock.underlier_shocks[0].spot_return = -1.0;
  auto invalid_result = compile_option_scenario_cube(active_catalog(panel), *healthy.snapshot,
                                                     std::move(invalid_shock), build_spec());
  ASSERT_FALSE(invalid_result);
  EXPECT_EQ(invalid_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  atx::vol::SurfaceProvenance degraded = healthy_provenance();
  degraded.state = atx::vol::SurfaceState::Degraded;
  degraded.validation.failures = atx::vol::ValidationFailure::CarryGap;
  Fixture bad{"bad-provenance", degraded};
  auto bad_result =
      compile_option_scenario_cube(active_catalog(panel), *bad.snapshot, manifest(), build_spec());
  ASSERT_FALSE(bad_result);
  EXPECT_EQ(bad_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  atx::vol::SurfaceProvenance empty_validation = healthy_provenance();
  empty_validation.validation.validation_id = 0U;
  Fixture no_evidence{"no-validation-evidence", empty_validation};
  auto evidence_result = compile_option_scenario_cube(active_catalog(panel), *no_evidence.snapshot,
                                                      manifest(), build_spec());
  ASSERT_FALSE(evidence_result);
  EXPECT_EQ(evidence_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  atx::vol::SurfaceProvenance mark_only = healthy_provenance();
  mark_only.purpose = atx::vol::SurfacePurpose::MarketMark;
  Fixture wrong_purpose{"wrong-purpose", mark_only};
  auto purpose_result = compile_option_scenario_cube(active_catalog(panel), *wrong_purpose.snapshot,
                                                     manifest(), build_spec());
  ASSERT_FALSE(purpose_result);
  EXPECT_EQ(purpose_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  atx::vol::SurfaceProvenance zero_source_generation = healthy_provenance();
  zero_source_generation.source_generation = 0U;
  Fixture no_source_generation{"no-source-generation", zero_source_generation};
  auto source_generation_result = compile_option_scenario_cube(
      active_catalog(panel), *no_source_generation.snapshot, manifest(), build_spec());
  ASSERT_FALSE(source_generation_result);
  EXPECT_EQ(source_generation_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  atx::vol::SurfaceProvenance zero_served_generation = healthy_provenance();
  zero_served_generation.served_generation = 0U;
  Fixture no_served_generation{"no-served-generation", zero_served_generation};
  auto served_generation_result = compile_option_scenario_cube(
      active_catalog(panel), *no_served_generation.snapshot, manifest(), build_spec());
  ASSERT_FALSE(served_generation_result);
  EXPECT_EQ(served_generation_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  atx::vol::SurfaceProvenance stale_surface = healthy_provenance();
  stale_surface.state = atx::vol::SurfaceState::Stale;
  Fixture stale_provenance{"stale-provenance", stale_surface};
  auto stale_provenance_result = compile_option_scenario_cube(
      active_catalog(panel), *stale_provenance.snapshot, manifest(), build_spec());
  ASSERT_FALSE(stale_provenance_result);
  EXPECT_EQ(stale_provenance_result.error().code(), atx::core::ErrorCode::InvalidArgument);

  constexpr std::uint32_t missing_uid = kUid + 1U;
  const std::vector<OptionPanelRow> missing_surface_rows{
      panel_row(101U, ExerciseStyle::American, Side::Put, 105.0, kUid),
      panel_row(303U, ExerciseStyle::European, Side::Call, 100.0, missing_uid),
  };
  auto missing_surface_panel = OptionResearchPanel::create(missing_surface_rows);
  ASSERT_TRUE(missing_surface_panel);
  OptionScenarioManifest missing_surface_manifest = manifest();
  for (const OptionScenarioDefinition &scenario : missing_surface_manifest.scenarios) {
    missing_surface_manifest.underlier_shocks.push_back(
        OptionScenarioUnderlierShock{scenario.scenario_id, missing_uid, 0.0, 0.0});
  }
  auto missing_surface_result =
      compile_option_scenario_cube(active_catalog(*missing_surface_panel), *healthy.snapshot,
                                   std::move(missing_surface_manifest), build_spec());
  ASSERT_FALSE(missing_surface_result);
  EXPECT_EQ(missing_surface_result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionScenarioCube, EnforcesPreallocationShapeBounds) {
  Fixture fixture{"bounds"};
  OptionResearchPanel panel = make_panel();
  OptionScenarioCubeBuildSpec cells = build_spec();
  cells.limits.max_scenario_cells = 5U;
  auto cells_result =
      compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot, manifest(), cells);
  ASSERT_FALSE(cells_result);
  EXPECT_EQ(cells_result.error().code(), atx::core::ErrorCode::OutOfRange);

  OptionScenarioCubeBuildSpec bytes = build_spec();
  bytes.limits.max_workspace_bytes = 1U;
  auto bytes_result =
      compile_option_scenario_cube(active_catalog(panel), *fixture.snapshot, manifest(), bytes);
  ASSERT_FALSE(bytes_result);
  EXPECT_EQ(bytes_result.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(OptionScenarioCube, RejectsMixedDecisionActiveCatalog) {
  Fixture fixture{"segmented"};
  OptionResearchPanel panel = make_panel();
  std::vector<OptionScenarioActiveContract> active = active_catalog(panel);
  active[1].audit.decision_ts_ns += kDayNs;
  auto result = compile_option_scenario_cube(active, *fixture.snapshot, manifest(), build_spec());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

} // namespace
