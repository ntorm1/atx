#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/vol/contract_projection.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/var.hpp"
#include "atx/vol/vol_curve.hpp"

namespace {

using namespace atx::vol;
namespace time = atx::core::time;

constexpr double kRate = 0.04;

std::int64_t timestamp(int year, unsigned month, unsigned day, unsigned hour = 19,
                       unsigned minute = 55) {
  return time::timestamp_from_utc(year, month, day, hour, minute, 0, 0).unix_nanos();
}

PricedSurface make_surface(std::uint32_t uid, double spot, std::int64_t now,
                           double variance_scale = 1.0) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.03, 0.08, 0.15, 0.25, 0.50, 1.00}) {
    EssviParams parameters{};
    parameters.theta = variance_scale * (0.035 + 0.012 * term);
    parameters.phi = 1.30;
    parameters.rho = -0.30;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = spot;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, spot, 0.0, kRate, 120, 7});
  }

  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = now;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto surface = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(surface) << (surface ? std::string{} : surface.error().to_string());
  return std::move(*surface);
}

class OneSurfaceSnapshot {
public:
  OneSurfaceSnapshot(std::uint32_t uid, double spot, std::int64_t ts, double variance_scale = 1.0)
      : surface_(make_surface(uid, spot, ts, variance_scale)), pointers_{&surface_},
        set_(SurfaceSet::create(pointers_)) {}

  [[nodiscard]] bool valid() const noexcept { return set_.has_value(); }
  [[nodiscard]] const SurfaceSet &set() const noexcept { return *set_; }
  [[nodiscard]] const PricedSurface &surface() const noexcept { return surface_; }

private:
  PricedSurface surface_;
  std::array<const PricedSurface *, 1> pointers_{};
  Result<SurfaceSet> set_;
};

VarPosition option(std::string underlier, Side side, double quantity,
                   double target_abs_delta = 0.40, double multiplier = 100.0) {
  return VarOptionPosition{std::move(underlier),
                           ProjectedMaturitySpec::months(3),
                           target_abs_delta,
                           side,
                           quantity,
                           multiplier};
}

VarPosition stock(std::string underlier, double shares) {
  return VarStockPosition{std::move(underlier), shares};
}

OptionProjectionSpec projection_spec(std::uint32_t uid, Side side, ProjectedMaturitySpec maturity,
                                     ProjectedStrikeSpec strike, double multiplier = 100.0) {
  OptionProjectionSpec result;
  result.uid = uid;
  result.maturity = maturity;
  result.strike = strike;
  result.side = side;
  result.multiplier = multiplier;
  return result;
}

VarEvaluationConfig evaluation_config(unsigned n_threads) {
  VarEvaluationConfig config;
  config.n_threads = n_threads;
  config.delta_tolerance = 1.0e-7;
  config.projection_execution = QueryExecution::ColdReference;
  config.valuation_execution = QueryExecution::ColdReference;
  return config;
}

std::uint64_t bits(double value) noexcept { return std::bit_cast<std::uint64_t>(value); }

void expect_bit_identical(const VarScenarioFrame &actual, const VarScenarioFrame &expected) {
  EXPECT_EQ(actual.base_ts_ns, expected.base_ts_ns);
  EXPECT_EQ(actual.shifted_ts_ns, expected.shifted_ts_ns);
  EXPECT_EQ(actual.status, expected.status);
  EXPECT_EQ(bits(actual.base_value), bits(expected.base_value));
  EXPECT_EQ(bits(actual.shifted_value), bits(expected.shifted_value));
  EXPECT_EQ(bits(actual.pnl), bits(expected.pnl));
  EXPECT_EQ(bits(actual.dollar_delta), bits(expected.dollar_delta));
  EXPECT_EQ(actual.n_ok, expected.n_ok);
  EXPECT_EQ(actual.n_failed, expected.n_failed);
  EXPECT_EQ(actual.definition_fingerprint, expected.definition_fingerprint);
}

void expect_economically_equal(const VarScenarioFrame &actual, const VarScenarioFrame &expected) {
  EXPECT_EQ(actual.base_ts_ns, expected.base_ts_ns);
  EXPECT_EQ(actual.shifted_ts_ns, expected.shifted_ts_ns);
  EXPECT_EQ(actual.status, expected.status);
  EXPECT_EQ(actual.n_ok, expected.n_ok);
  EXPECT_EQ(actual.n_failed, expected.n_failed);
  EXPECT_EQ(actual.definition_fingerprint, expected.definition_fingerprint);
  const auto close = [](double lhs, double rhs) {
    return std::fabs(lhs - rhs) <= 1.0e-10 * std::max(1.0, std::fabs(rhs));
  };
  EXPECT_TRUE(close(actual.base_value, expected.base_value));
  EXPECT_TRUE(close(actual.shifted_value, expected.shifted_value));
  EXPECT_TRUE(close(actual.pnl, expected.pnl));
  EXPECT_TRUE(close(actual.dollar_delta, expected.dollar_delta));
}

// Cross-route economic parity gate (CORRECTNESS GATE 2): independent Newton
// solves -- the engine's cross-sectional batch solver and the scalar
// project_option_contract oracle (OptionDeltaSolvePolicy::Direct) -- each
// land on a different, individually cold-confirmed root inside the
// delta_tolerance corridor (CORRECTNESS GATE 1). They are not required to be
// bit-identical; only economically equal at this relative gate.
bool close_cross_route(double actual, double expected) {
  return std::fabs(actual - expected) <= 1.0e-5 * std::max(1.0, std::fabs(expected));
}

void expect_bit_identical(const VarLegFrame &actual, const VarLegFrame &expected) {
  EXPECT_EQ(actual.kind, expected.kind);
  EXPECT_EQ(actual.status, expected.status);
  EXPECT_EQ(actual.uid, expected.uid);
  EXPECT_EQ(bits(actual.units), bits(expected.units));
  EXPECT_EQ(bits(actual.base_spot), bits(expected.base_spot));
  EXPECT_EQ(bits(actual.shifted_spot), bits(expected.shifted_spot));
  EXPECT_EQ(bits(actual.base_mark), bits(expected.base_mark));
  EXPECT_EQ(bits(actual.shifted_mark), bits(expected.shifted_mark));
  EXPECT_EQ(bits(actual.base_delta), bits(expected.base_delta));
  EXPECT_EQ(bits(actual.dollar_delta), bits(expected.dollar_delta));
  EXPECT_EQ(bits(actual.base_value), bits(expected.base_value));
  EXPECT_EQ(bits(actual.shifted_value), bits(expected.shifted_value));
  EXPECT_EQ(bits(actual.pnl), bits(expected.pnl));
  EXPECT_EQ(bits(actual.strike), bits(expected.strike));
  EXPECT_EQ(bits(actual.base_time_to_expiry), bits(expected.base_time_to_expiry));
  EXPECT_EQ(bits(actual.shifted_time_to_expiry), bits(expected.shifted_time_to_expiry));
  EXPECT_EQ(actual.definition_fingerprint, expected.definition_fingerprint);
}

class ScopedTempDirectory {
public:
  explicit ScopedTempDirectory(std::string_view label) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("atx_var_" + std::string(label) + "_" + std::to_string(tick) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ~ScopedTempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedTempDirectory(const ScopedTempDirectory &) = delete;
  ScopedTempDirectory &operator=(const ScopedTempDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_{};
};

TEST(Var, DefaultSolvePolicyIsCrossSectionalColdConfirm) {
  EXPECT_EQ(VarEvaluationConfig{}.projection_solve_policy,
            OptionDeltaSolvePolicy::CrossSectionalColdConfirm);
}

TEST(Var, ReferenceDeltaStrikeAnchorsForwardMoneynessAndDollarDelta) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const std::int64_t reference_ts = timestamp(2026, 1, 2);
  const OneSurfaceSnapshot reference(uid, 100.0, reference_ts);
  ASSERT_TRUE(reference.valid());

  const std::vector<VarPosition> positions = {option("spy", Side::Call, 2.0)};
  const VarEvaluationConfig config = evaluation_config(1);
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());
  ASSERT_EQ(prepared->size(), 1u);
  ASSERT_EQ(prepared->reference_legs().size(), 1u);

  OptionProjectionConfig projection_config;
  projection_config.output = OptionProjectionOutput::FullGreeks;
  projection_config.analytic_greeks = true;
  projection_config.delta_tolerance = config.delta_tolerance;
  projection_config.query_execution = config.projection_execution;
  auto expected =
      project_option_contract(reference.surface(),
                              projection_spec(uid, Side::Call, ProjectedMaturitySpec::months(3),
                                              ProjectedStrikeSpec::delta(0.40)),
                              projection_config);
  ASSERT_TRUE(expected) << (expected ? std::string{} : expected.error().to_string());

  const VarReferenceLeg &leg = prepared->reference_legs()[0];
  EXPECT_EQ(leg.kind, VarLegKind::Option);
  EXPECT_EQ(leg.uid, uid);
  EXPECT_EQ(leg.underlier, "SPY");
  EXPECT_DOUBLE_EQ(leg.reference_units, 2.0);
  EXPECT_DOUBLE_EQ(leg.reference_spot, 100.0);
  EXPECT_DOUBLE_EQ(leg.reference_mark, expected->model_mark);
  EXPECT_DOUBLE_EQ(leg.reference_delta, expected->greeks.delta);
  EXPECT_NEAR(std::fabs(leg.reference_delta), 0.40, config.delta_tolerance);
  EXPECT_DOUBLE_EQ(leg.target_abs_delta, 0.40);
  EXPECT_DOUBLE_EQ(leg.log_moneyness,
                   std::log(expected->definition.contract.K / expected->forward));
  EXPECT_DOUBLE_EQ(leg.target_dollar_delta, 2.0 * 100.0 * expected->greeks.delta * 100.0);
  EXPECT_DOUBLE_EQ(prepared->reference_value(), 2.0 * 100.0 * expected->model_mark);
  EXPECT_DOUBLE_EQ(prepared->reference_dollar_delta(), leg.target_dollar_delta);
  EXPECT_EQ(prepared->reference_ts_ns(), reference_ts);
}

TEST(Var, DollarDeltaSizingIsReusableSignSafeAndRejectsInvalidInputs) {
  auto long_call = resolve_var_sizing(VarSizingInput{4000.0, 100.0, 0.40, 100.0});
  ASSERT_TRUE(long_call);
  EXPECT_DOUBLE_EQ(long_call->units, 1.0);
  EXPECT_DOUBLE_EQ(long_call->achieved_dollar_delta, 4000.0);

  auto long_put = resolve_var_sizing(VarSizingInput{-4000.0, 100.0, -0.40, 100.0});
  ASSERT_TRUE(long_put);
  EXPECT_DOUBLE_EQ(long_put->units, 1.0);
  EXPECT_DOUBLE_EQ(long_put->achieved_dollar_delta, -4000.0);

  auto short_put = resolve_var_sizing(VarSizingInput{4000.0, 100.0, -0.40, 100.0});
  ASSERT_TRUE(short_put);
  EXPECT_DOUBLE_EQ(short_put->units, -1.0);
  EXPECT_DOUBLE_EQ(short_put->achieved_dollar_delta, 4000.0);

  EXPECT_FALSE(resolve_var_sizing(VarSizingInput{1.0, 0.0, 0.40, 100.0}));
  EXPECT_FALSE(resolve_var_sizing(VarSizingInput{1.0, 100.0, 0.0, 100.0}));
  EXPECT_FALSE(resolve_var_sizing(VarSizingInput{1.0, 100.0, 0.40, 0.0}));
}

TEST(Var, ReplayPreservesDeltaMoneynessTteAndDollarDeltaThenHoldsConcreteContract) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  const OneSurfaceSnapshot base(uid, 82.0, timestamp(2026, 1, 5), 2.0);
  const OneSurfaceSnapshot shifted(uid, 87.0, timestamp(2026, 1, 6), 1.5);
  ASSERT_TRUE(reference.valid() && base.valid() && shifted.valid());

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.75)};
  const VarEvaluationConfig config = evaluation_config(1);
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(prepared);
  const VarReferenceLeg &reference_leg = prepared->reference_legs()[0];

  const std::vector<VarScenario> scenarios = {
      {base.surface().pricing().now_ts_ns, &base.set(), shifted.surface().pricing().now_ts_ns,
       &shifted.set()},
  };
  std::vector<VarScenarioFrame> frames(1);
  std::vector<VarLegFrame> legs(1);
  ASSERT_TRUE(prepared->replay_into(scenarios, frames, legs, config));
  ASSERT_EQ(frames[0].status, VarScenarioStatus::Ok);
  ASSERT_EQ(legs[0].status, VarLegStatus::Ok);

  OptionProjectionConfig projection_config;
  projection_config.output = OptionProjectionOutput::FullGreeks;
  projection_config.analytic_greeks = true;
  projection_config.delta_tolerance = config.delta_tolerance;
  projection_config.query_execution = config.projection_execution;
  auto expected_base =
      project_option_contract(base.surface(),
                              projection_spec(uid, Side::Call, ProjectedMaturitySpec::months(3),
                                              ProjectedStrikeSpec::delta(0.40)),
                              projection_config);
  ASSERT_TRUE(expected_base);
  auto expected_shifted = project_option_contract(
      shifted.surface(),
      projection_spec(uid, Side::Call,
                      ProjectedMaturitySpec::absolute(expected_base->definition.expiry_ts_ns),
                      ProjectedStrikeSpec::absolute(expected_base->definition.contract.K)),
      projection_config);
  ASSERT_TRUE(expected_shifted);

  const double expected_units = reference_leg.target_dollar_delta /
                                (base.surface().pricing().S * expected_base->greeks.delta * 100.0);
  // The engine (CrossSectionalColdConfirm) and expected_base/expected_shifted
  // (scalar Direct oracle, see OptionProjectionConfig::delta_solve_policy
  // default) are independent solves; compare at the cross-route gate rather
  // than requiring a bit-identical root.
  EXPECT_TRUE(close_cross_route(legs[0].units, expected_units));
  EXPECT_TRUE(close_cross_route(legs[0].strike, expected_base->definition.contract.K));
  EXPECT_NE(std::log(legs[0].strike / expected_base->forward), reference_leg.log_moneyness);
  EXPECT_DOUBLE_EQ(legs[0].base_time_to_expiry, expected_base->definition.contract.T);
  EXPECT_DOUBLE_EQ(legs[0].shifted_time_to_expiry, expected_shifted->definition.contract.T);
  EXPECT_LT(legs[0].shifted_time_to_expiry, legs[0].base_time_to_expiry);
  EXPECT_TRUE(close_cross_route(legs[0].base_mark, expected_base->model_mark));
  EXPECT_TRUE(close_cross_route(legs[0].shifted_mark, expected_shifted->model_mark));
  // Both roots are independently cold-confirmed to delta_tolerance of the
  // same target, so their deltas cannot diverge by more than 2*delta_tolerance.
  EXPECT_LE(std::fabs(legs[0].base_delta - expected_base->greeks.delta),
            2.0 * config.delta_tolerance);
  EXPECT_NEAR(std::fabs(legs[0].base_delta), 0.40, config.delta_tolerance);
  EXPECT_NEAR(legs[0].dollar_delta, reference_leg.target_dollar_delta,
              std::fabs(reference_leg.target_dollar_delta) * 1.0e-13);
  EXPECT_TRUE(
      close_cross_route(legs[0].base_value, expected_units * 100.0 * expected_base->model_mark));
  EXPECT_TRUE(close_cross_route(legs[0].shifted_value,
                                expected_units * 100.0 * expected_shifted->model_mark));
  EXPECT_DOUBLE_EQ(legs[0].pnl, legs[0].shifted_value - legs[0].base_value);
  EXPECT_DOUBLE_EQ(frames[0].base_value, legs[0].base_value);
  EXPECT_DOUBLE_EQ(frames[0].shifted_value, legs[0].shifted_value);
  EXPECT_DOUBLE_EQ(frames[0].pnl, legs[0].pnl);
  EXPECT_DOUBLE_EQ(frames[0].dollar_delta, legs[0].dollar_delta);
  EXPECT_EQ(frames[0].n_ok, 1u);
  EXPECT_EQ(frames[0].n_failed, 0u);
  EXPECT_NE(legs[0].definition_fingerprint, 0u);
  EXPECT_NE(frames[0].definition_fingerprint, 0u);
}

TEST(Var, YearFractionReplayMatchesDirectContractProjection) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  const OneSurfaceSnapshot base(uid, 94.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 97.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(reference.valid() && base.valid() && shifted.valid());

  const ProjectedMaturitySpec maturity = ProjectedMaturitySpec::years(0.20);
  const std::vector<VarPosition> positions = {
      VarOptionPosition{"SPY", maturity, 0.35, Side::Put, -2.0, 100.0}};
  const VarEvaluationConfig config = evaluation_config(1u);
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(prepared);

  const std::vector<VarScenario> scenarios = {
      {base.surface().pricing().now_ts_ns, &base.set(), shifted.surface().pricing().now_ts_ns,
       &shifted.set()},
  };
  std::vector<VarScenarioFrame> frames(1u);
  std::vector<VarLegFrame> legs(1u);
  ASSERT_TRUE(prepared->replay_into(scenarios, frames, legs, config));
  ASSERT_EQ(legs[0].status, VarLegStatus::Ok);

  OptionProjectionConfig projection_config;
  projection_config.output = OptionProjectionOutput::FullGreeks;
  projection_config.analytic_greeks = true;
  projection_config.delta_tolerance = config.delta_tolerance;
  projection_config.query_execution = config.projection_execution;
  auto expected_base = project_option_contract(
      base.surface(), projection_spec(uid, Side::Put, maturity, ProjectedStrikeSpec::delta(0.35)),
      projection_config);
  ASSERT_TRUE(expected_base);
  auto expected_shifted = project_option_contract(
      shifted.surface(),
      projection_spec(uid, Side::Put,
                      ProjectedMaturitySpec::absolute(expected_base->definition.expiry_ts_ns),
                      ProjectedStrikeSpec::absolute(expected_base->definition.contract.K)),
      projection_config);
  ASSERT_TRUE(expected_shifted);

  // The engine (CrossSectionalColdConfirm) and expected_base/expected_shifted
  // (scalar Direct oracle, see OptionProjectionConfig::delta_solve_policy
  // default) are independent solves; compare strike/mark/delta at the
  // cross-route gate rather than requiring a bit-identical root.
  EXPECT_TRUE(close_cross_route(legs[0].strike, expected_base->definition.contract.K));
  EXPECT_DOUBLE_EQ(legs[0].base_time_to_expiry, expected_base->definition.contract.T);
  EXPECT_DOUBLE_EQ(legs[0].shifted_time_to_expiry, expected_shifted->definition.contract.T);
  EXPECT_TRUE(close_cross_route(legs[0].base_mark, expected_base->model_mark));
  EXPECT_TRUE(close_cross_route(legs[0].shifted_mark, expected_shifted->model_mark));
  EXPECT_LE(std::fabs(legs[0].base_delta - expected_base->greeks.delta),
            2.0 * config.delta_tolerance);

  // The fingerprint is a pure hash of (uid, K, T, side, valuation_ts_ns,
  // expiry_ts_ns, multiplier) -- see projected_definition_fingerprint
  // (contract_projection.cpp:448-460). It is legitimately route-dependent
  // through K and T (the solved strike/tenor), so it cannot match
  // expected_base's own fingerprint; but it must still be a bit-exact,
  // deterministic function of the engine's OWN resolved values. Reconstruct
  // the definition from the engine's leg output (K, T) plus the
  // route-independent inputs -- uid/side/multiplier/valuation_ts_ns from the
  // fixture, expiry_ts_ns from expected_base (expiry resolution depends only
  // on valuation_ts_ns and the maturity spec, not on which route solved the
  // strike) -- and assert bit-exact equality instead of a near-tautological
  // nonzero check.
  ProjectedOptionDefinition reconstructed;
  reconstructed.contract =
      OptionContract{uid, legs[0].strike, legs[0].base_time_to_expiry, Side::Put};
  reconstructed.valuation_ts_ns = base.surface().pricing().now_ts_ns;
  reconstructed.expiry_ts_ns = expected_base->definition.expiry_ts_ns;
  reconstructed.multiplier = 100.0;
  EXPECT_EQ(legs[0].definition_fingerprint, projected_definition_fingerprint(reconstructed));
}

TEST(Var, StockHedgeResizesAtBaseAndProducesNonzeroHeldPeriodPnl) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  const OneSurfaceSnapshot base(uid, 80.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 84.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(reference.valid() && base.valid() && shifted.valid());

  const std::vector<VarPosition> positions = {stock("SPY", 12.0)};
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), evaluation_config(1));
  ASSERT_TRUE(prepared);
  ASSERT_EQ(prepared->reference_legs().size(), 1u);
  EXPECT_EQ(prepared->reference_legs()[0].kind, VarLegKind::Stock);
  EXPECT_DOUBLE_EQ(prepared->reference_legs()[0].target_dollar_delta, 1200.0);

  const std::vector<VarScenario> scenarios = {
      {base.surface().pricing().now_ts_ns, &base.set(), shifted.surface().pricing().now_ts_ns,
       &shifted.set()},
  };
  std::vector<VarScenarioFrame> frames(1);
  std::vector<VarLegFrame> legs(1);
  ASSERT_TRUE(prepared->replay_into(scenarios, frames, legs, evaluation_config(1)));
  ASSERT_EQ(legs[0].status, VarLegStatus::Ok);
  EXPECT_EQ(legs[0].kind, VarLegKind::Stock);
  EXPECT_DOUBLE_EQ(legs[0].units, 15.0);
  EXPECT_DOUBLE_EQ(legs[0].base_delta, 1.0);
  EXPECT_DOUBLE_EQ(legs[0].dollar_delta, 1200.0);
  EXPECT_DOUBLE_EQ(legs[0].base_value, 1200.0);
  EXPECT_DOUBLE_EQ(legs[0].shifted_value, 1260.0);
  EXPECT_DOUBLE_EQ(legs[0].pnl, 60.0);
  EXPECT_DOUBLE_EQ(frames[0].pnl, 60.0);
  EXPECT_NE(frames[0].pnl, 0.0);
}

TEST(Var, LongAndShortCallAndPutSignsArePreservedByDollarDeltaScaling) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  const OneSurfaceSnapshot base(uid, 92.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 95.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(reference.valid() && base.valid() && shifted.valid());

  const std::vector<VarPosition> positions = {
      option("SPY", Side::Call, 2.0),
      option("SPY", Side::Call, -2.0),
      option("SPY", Side::Put, 3.0),
      option("SPY", Side::Put, -3.0),
  };
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), evaluation_config(1));
  ASSERT_TRUE(prepared);
  ASSERT_EQ(prepared->reference_legs().size(), 4u);
  const auto reference_legs = prepared->reference_legs();

  EXPECT_GT(reference_legs[0].reference_delta, 0.0);
  EXPECT_GT(reference_legs[0].target_dollar_delta, 0.0);
  EXPECT_GT(reference_legs[1].reference_delta, 0.0);
  EXPECT_LT(reference_legs[1].target_dollar_delta, 0.0);
  EXPECT_LT(reference_legs[2].reference_delta, 0.0);
  EXPECT_LT(reference_legs[2].target_dollar_delta, 0.0);
  EXPECT_LT(reference_legs[3].reference_delta, 0.0);
  EXPECT_GT(reference_legs[3].target_dollar_delta, 0.0);

  const std::vector<VarScenario> scenarios = {
      {base.surface().pricing().now_ts_ns, &base.set(), shifted.surface().pricing().now_ts_ns,
       &shifted.set()},
  };
  std::vector<VarScenarioFrame> frames(1);
  std::vector<VarLegFrame> legs(positions.size());
  ASSERT_TRUE(prepared->replay_into(scenarios, frames, legs, evaluation_config(1)));
  ASSERT_EQ(frames[0].status, VarScenarioStatus::Ok);
  EXPECT_GT(legs[0].units, 0.0);
  EXPECT_LT(legs[1].units, 0.0);
  EXPECT_GT(legs[2].units, 0.0);
  EXPECT_LT(legs[3].units, 0.0);
  for (std::size_t i = 0; i < legs.size(); ++i) {
    EXPECT_EQ(legs[i].status, VarLegStatus::Ok);
    EXPECT_EQ(std::signbit(legs[i].dollar_delta),
              std::signbit(reference_legs[i].target_dollar_delta));
  }
}

TEST(Var, ReplayIsBitInvariantAcrossThreadCountsAndLegOutputIsOptional) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());
  const std::vector<VarPosition> positions = {
      option("SPY", Side::Call, 1.25, 0.40),
      option("SPY", Side::Put, -0.75, 0.30),
      stock("SPY", 7.0),
  };
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), evaluation_config(1));
  ASSERT_TRUE(prepared);

  std::vector<std::unique_ptr<OneSurfaceSnapshot>> bases;
  std::vector<std::unique_ptr<OneSurfaceSnapshot>> shifts;
  const std::array<double, 4> base_spots{96.0, 103.0, 89.0, 111.0};
  const std::array<double, 4> shifted_spots{99.0, 101.0, 94.0, 107.0};
  for (std::size_t i = 0; i < base_spots.size(); ++i) {
    bases.push_back(std::make_unique<OneSurfaceSnapshot>(
        uid, base_spots[i], timestamp(2026, 1, 5 + static_cast<unsigned>(i))));
    shifts.push_back(std::make_unique<OneSurfaceSnapshot>(
        uid, shifted_spots[i], timestamp(2026, 1, 6 + static_cast<unsigned>(i))));
    ASSERT_TRUE(bases.back()->valid() && shifts.back()->valid());
  }

  std::vector<VarScenario> scenarios;
  scenarios.reserve(base_spots.size());
  for (std::size_t i = 0; i < base_spots.size(); ++i) {
    scenarios.push_back(VarScenario{bases[i]->surface().pricing().now_ts_ns, &bases[i]->set(),
                                    shifts[i]->surface().pricing().now_ts_ns, &shifts[i]->set()});
  }

  std::vector<VarScenarioFrame> serial_frames(scenarios.size());
  std::vector<VarScenarioFrame> parallel_frames(scenarios.size());
  std::vector<VarScenarioFrame> aggregate_only_frames(scenarios.size());
  std::vector<VarLegFrame> serial_legs(scenarios.size() * positions.size());
  std::vector<VarLegFrame> parallel_legs(scenarios.size() * positions.size());
  ASSERT_TRUE(prepared->replay_into(scenarios, serial_frames, serial_legs, evaluation_config(1)));
  ASSERT_TRUE(
      prepared->replay_into(scenarios, parallel_frames, parallel_legs, evaluation_config(4)));
  ASSERT_TRUE(prepared->replay_into(scenarios, aggregate_only_frames, std::span<VarLegFrame>{},
                                    evaluation_config(4)));

  for (std::size_t i = 0; i < scenarios.size(); ++i) {
    expect_bit_identical(parallel_frames[i], serial_frames[i]);
    expect_economically_equal(aggregate_only_frames[i], serial_frames[i]);
  }
  for (std::size_t i = 0; i < serial_legs.size(); ++i) {
    expect_bit_identical(parallel_legs[i], serial_legs[i]);
  }
}

TEST(Var, ScreenedBatchProjectionIsColdConfirmedAndMatchesRetainedLegOutput) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());

  PricedSurface base_source = make_surface(uid, 97.0, timestamp(2026, 1, 5), 1.15);
  PricedSurface shifted_source = make_surface(uid, 103.0, timestamp(2026, 1, 6), 0.90);
  auto base_fast = std::move(base_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  auto shifted_fast =
      std::move(shifted_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(base_fast && shifted_fast);
  const std::array<const PricedSurface *, 1> base_pointers{&*base_fast};
  const std::array<const PricedSurface *, 1> shifted_pointers{&*shifted_fast};
  auto base_set = SurfaceSet::create(base_pointers);
  auto shifted_set = SurfaceSet::create(shifted_pointers);
  ASSERT_TRUE(base_set && shifted_set);

  const std::array<double, 8> targets{0.10, 0.20, 0.30, 0.40, 0.55, 0.65, 0.75, 0.85};
  std::vector<VarPosition> positions;
  positions.reserve(targets.size());
  for (std::size_t index = 0u; index < targets.size(); ++index) {
    positions.push_back(
        option("SPY", index % 2u == 0u ? Side::Call : Side::Put, 1.0, targets[index]));
  }
  VarEvaluationConfig config = evaluation_config(1);
  config.projection_solve_policy = OptionDeltaSolvePolicy::FastScreenColdConfirm;
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(prepared);

  const std::vector<VarScenario> scenarios = {
      {base_fast->pricing().now_ts_ns, &*base_set, shifted_fast->pricing().now_ts_ns,
       &*shifted_set},
  };
  std::vector<VarScenarioFrame> retained_frames(1u);
  std::vector<VarScenarioFrame> aggregate_frames(1u);
  std::vector<VarLegFrame> legs(positions.size());
  ASSERT_TRUE(prepared->replay_into(scenarios, retained_frames, legs, config));
  ASSERT_TRUE(prepared->replay_into(scenarios, aggregate_frames, {}, config));
  EXPECT_EQ(aggregate_frames[0].status, retained_frames[0].status);
  EXPECT_EQ(aggregate_frames[0].n_ok, retained_frames[0].n_ok);
  EXPECT_EQ(aggregate_frames[0].n_failed, retained_frames[0].n_failed);
  const double value_scale = std::max(
      {1.0, std::fabs(retained_frames[0].base_value), std::fabs(retained_frames[0].shifted_value)});
  EXPECT_LE(std::fabs(aggregate_frames[0].base_value - retained_frames[0].base_value),
            1.0e-7 * value_scale);
  EXPECT_LE(std::fabs(aggregate_frames[0].shifted_value - retained_frames[0].shifted_value),
            1.0e-7 * value_scale);
  EXPECT_LE(std::fabs(aggregate_frames[0].pnl - retained_frames[0].pnl), 1.0e-7 * value_scale);
  EXPECT_LE(std::fabs(aggregate_frames[0].dollar_delta - retained_frames[0].dollar_delta),
            1.0e-9 * std::max(1.0, std::fabs(retained_frames[0].dollar_delta)));
  ASSERT_EQ(retained_frames[0].status, VarScenarioStatus::Ok);
  for (std::size_t index = 0u; index < legs.size(); ++index) {
    ASSERT_EQ(legs[index].status, VarLegStatus::Ok);
    EXPECT_NEAR(std::fabs(legs[index].base_delta), targets[index], config.delta_tolerance);
  }
}

TEST(Var, CrossSectionalAggregateReplayMatchesDirectColdOracle) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());

  PricedSurface base_source = make_surface(uid, 97.0, timestamp(2026, 1, 5), 1.15);
  PricedSurface shifted_source = make_surface(uid, 103.0, timestamp(2026, 1, 6), 0.90);
  auto base_fast = std::move(base_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  auto shifted_fast =
      std::move(shifted_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(base_fast && shifted_fast);
  const std::array<const PricedSurface *, 1> base_pointers{&*base_fast};
  const std::array<const PricedSurface *, 1> shifted_pointers{&*shifted_fast};
  auto base_set = SurfaceSet::create(base_pointers);
  auto shifted_set = SurfaceSet::create(shifted_pointers);
  ASSERT_TRUE(base_set && shifted_set);

  const std::array<double, 8> targets{0.10, 0.20, 0.30, 0.40, 0.55, 0.65, 0.75, 0.85};
  std::vector<VarPosition> positions;
  positions.reserve(targets.size());
  for (std::size_t index = 0u; index < targets.size(); ++index) {
    positions.push_back(
        option("SPY", index % 2u == 0u ? Side::Call : Side::Put, 1.0, targets[index]));
  }

  // The new engine is exercised end to end (reference AND scenario
  // resolution) under CrossSectionalColdConfirm; the comparator portfolio
  // runs entirely on the scalar Direct oracle -- a genuine cross-route
  // comparison per CORRECTNESS GATE 2, not merely two solve paths sharing one
  // portfolio's pre-solved reference legs.
  VarEvaluationConfig cross_config = evaluation_config(1);
  cross_config.projection_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  auto cross_prepared = PreparedVarPortfolio::create(positions, reference.set(), cross_config);
  ASSERT_TRUE(cross_prepared) << (cross_prepared ? std::string{}
                                                 : cross_prepared.error().to_string());

  VarEvaluationConfig direct_config = evaluation_config(1);
  direct_config.projection_solve_policy = OptionDeltaSolvePolicy::Direct;
  auto direct_prepared = PreparedVarPortfolio::create(positions, reference.set(), direct_config);
  ASSERT_TRUE(direct_prepared) << (direct_prepared ? std::string{}
                                                   : direct_prepared.error().to_string());

  const std::vector<VarScenario> scenarios = {
      {base_fast->pricing().now_ts_ns, &*base_set, shifted_fast->pricing().now_ts_ns,
       &*shifted_set},
  };
  std::vector<VarScenarioFrame> cross_frames(1u);
  ASSERT_TRUE(cross_prepared->replay_into(scenarios, cross_frames, {}, cross_config));

  std::vector<VarScenarioFrame> direct_frames(1u);
  std::vector<VarLegFrame> direct_legs(positions.size());
  ASSERT_TRUE(direct_prepared->replay_into(scenarios, direct_frames, direct_legs, direct_config));

  EXPECT_EQ(cross_frames[0].status, direct_frames[0].status);
  EXPECT_EQ(cross_frames[0].n_ok, direct_frames[0].n_ok);
  EXPECT_EQ(cross_frames[0].n_failed, direct_frames[0].n_failed);
  ASSERT_EQ(cross_frames[0].status, VarScenarioStatus::Ok);
  ASSERT_EQ(direct_frames[0].status, VarScenarioStatus::Ok);
  for (const VarLegFrame &leg : direct_legs) {
    ASSERT_EQ(leg.status, VarLegStatus::Ok);
  }

  const double value_scale = std::max(
      {1.0, std::fabs(direct_frames[0].base_value), std::fabs(direct_frames[0].shifted_value)});
  EXPECT_LE(std::fabs(cross_frames[0].base_value - direct_frames[0].base_value),
            1.0e-5 * value_scale);
  EXPECT_LE(std::fabs(cross_frames[0].shifted_value - direct_frames[0].shifted_value),
            1.0e-5 * value_scale);
  EXPECT_LE(std::fabs(cross_frames[0].pnl - direct_frames[0].pnl), 1.0e-5 * value_scale);
  EXPECT_LE(std::fabs(cross_frames[0].dollar_delta - direct_frames[0].dollar_delta),
            1.0e-6 * std::max(1.0, std::fabs(direct_frames[0].dollar_delta)));
}

TEST(Var, ValidationRejectsAbsoluteExpiryAndReportsMissingMarketsAndTimestamps) {
  const std::uint32_t spy_uid = uid_for_symbol("SPY");
  const std::uint32_t aapl_uid = uid_for_symbol("AAPL");
  const std::int64_t reference_ts = timestamp(2026, 1, 2);
  const OneSurfaceSnapshot reference(spy_uid, 100.0, reference_ts);
  ASSERT_TRUE(reference.valid());

  const std::vector<VarPosition> absolute_expiry = {VarOptionPosition{
      "SPY", ProjectedMaturitySpec::absolute(timestamp(2026, 4, 2)), 0.40, Side::Call, 1.0, 100.0}};
  EXPECT_FALSE(
      PreparedVarPortfolio::create(absolute_expiry, reference.set(), evaluation_config(1)));

  const std::vector<VarPosition> valid_configuration = {option("SPY", Side::Call, 1.0)};
  VarEvaluationConfig invalid_projection = evaluation_config(1);
  invalid_projection.projection_execution = static_cast<QueryExecution>(0xff);
  EXPECT_FALSE(
      PreparedVarPortfolio::create(valid_configuration, reference.set(), invalid_projection));

  VarEvaluationConfig invalid_valuation = evaluation_config(1);
  invalid_valuation.valuation_execution = static_cast<QueryExecution>(0xff);
  EXPECT_FALSE(
      PreparedVarPortfolio::create(valid_configuration, reference.set(), invalid_valuation));

  VarEvaluationConfig invalid_solve_policy = evaluation_config(1);
  invalid_solve_policy.projection_solve_policy = static_cast<OptionDeltaSolvePolicy>(0xff);
  EXPECT_FALSE(
      PreparedVarPortfolio::create(valid_configuration, reference.set(), invalid_solve_policy));

  const std::vector<VarPosition> unavailable_reference = {option("AAPL", Side::Call, 1.0)};
  EXPECT_FALSE(
      PreparedVarPortfolio::create(unavailable_reference, reference.set(), evaluation_config(1)));

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0)};
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), evaluation_config(1));
  ASSERT_TRUE(prepared);

  const OneSurfaceSnapshot wrong_base(aapl_uid, 190.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot wrong_shifted(aapl_uid, 191.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(wrong_base.valid() && wrong_shifted.valid());
  const std::vector<VarScenario> missing_scenarios = {
      {wrong_base.surface().pricing().now_ts_ns, &wrong_base.set(),
       wrong_shifted.surface().pricing().now_ts_ns, &wrong_shifted.set()}};
  std::vector<VarScenarioFrame> missing_frames(1);
  std::vector<VarLegFrame> missing_legs(1);
  ASSERT_TRUE(
      prepared->replay_into(missing_scenarios, missing_frames, missing_legs, evaluation_config(1)));
  EXPECT_EQ(missing_frames[0].status, VarScenarioStatus::MarketUnavailable);
  EXPECT_EQ(missing_frames[0].n_ok, 0u);
  EXPECT_EQ(missing_frames[0].n_failed, 1u);
  EXPECT_EQ(missing_legs[0].status, VarLegStatus::SurfaceUnavailable);

  const OneSurfaceSnapshot base(spy_uid, 99.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(spy_uid, 101.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(base.valid() && shifted.valid());
  const std::vector<VarScenario> mismatched_scenarios = {
      {base.surface().pricing().now_ts_ns + 1, &base.set(), shifted.surface().pricing().now_ts_ns,
       &shifted.set()}};
  std::vector<VarScenarioFrame> mismatched_frames(1);
  std::vector<VarLegFrame> mismatched_legs(1);
  ASSERT_TRUE(prepared->replay_into(mismatched_scenarios, mismatched_frames, mismatched_legs,
                                    evaluation_config(1)));
  EXPECT_EQ(mismatched_frames[0].status, VarScenarioStatus::TimestampMismatch);
  EXPECT_EQ(mismatched_frames[0].n_failed, 1u);
  EXPECT_EQ(mismatched_legs[0].status, VarLegStatus::TimestampMismatch);
}

TEST(Var, CrossSectionalPolicyIsAcceptedByConfigValidation) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0)};
  VarEvaluationConfig cross_config = evaluation_config(1);
  cross_config.projection_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), cross_config);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());

  const OneSurfaceSnapshot base(uid, 99.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 101.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(base.valid() && shifted.valid());
  const std::vector<VarScenario> scenarios = {{base.surface().pricing().now_ts_ns, &base.set(),
                                               shifted.surface().pricing().now_ts_ns,
                                               &shifted.set()}};

  std::vector<VarScenarioFrame> retained_frames(1u);
  std::vector<VarLegFrame> legs(1u);
  ASSERT_TRUE(prepared->replay_into(scenarios, retained_frames, legs, cross_config));
  EXPECT_EQ(retained_frames[0].status, VarScenarioStatus::Ok);
  ASSERT_EQ(legs[0].status, VarLegStatus::Ok);

  std::vector<VarScenarioFrame> aggregate_frames(1u);
  ASSERT_TRUE(prepared->replay_into(scenarios, aggregate_frames, {}, cross_config));
  EXPECT_EQ(aggregate_frames[0].status, VarScenarioStatus::Ok);

  VarEvaluationConfig invalid_solve_policy = evaluation_config(1);
  invalid_solve_policy.projection_solve_policy = static_cast<OptionDeltaSolvePolicy>(0xff);
  EXPECT_FALSE(PreparedVarPortfolio::create(positions, reference.set(), invalid_solve_policy));
  EXPECT_FALSE(prepared->replay_into(scenarios, retained_frames, legs, invalid_solve_policy));
}

TEST(Var, ReplayReportsOptionExpiryBeforeScenarioEndExplicitly) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  const OneSurfaceSnapshot base(uid, 99.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 101.0, timestamp(2026, 1, 7));
  ASSERT_TRUE(reference.valid() && base.valid() && shifted.valid());

  const std::vector<VarPosition> positions = {VarOptionPosition{
      "SPY", ProjectedMaturitySpec::years(1.0 / 365.0), 0.40, Side::Call, 1.0, 100.0}};
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), evaluation_config(1));
  ASSERT_TRUE(prepared);

  const std::vector<VarScenario> scenarios = {{base.surface().pricing().now_ts_ns, &base.set(),
                                               shifted.surface().pricing().now_ts_ns,
                                               &shifted.set()}};
  std::vector<VarScenarioFrame> frames(1u);
  std::vector<VarLegFrame> legs(1u);
  ASSERT_TRUE(prepared->replay_into(scenarios, frames, legs, evaluation_config(1)));
  EXPECT_EQ(legs[0].status, VarLegStatus::ExpiredBeforeShift);
  EXPECT_EQ(frames[0].status, VarScenarioStatus::LegFailure);
  EXPECT_EQ(frames[0].n_failed, 1u);
}

TEST(Var, CrossSectionalRowFailureFallsBackToScalarLegStatuses) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  const OneSurfaceSnapshot base(uid, 99.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 101.0, timestamp(2026, 1, 7));
  ASSERT_TRUE(reference.valid() && base.valid() && shifted.valid());

  const std::vector<VarPosition> positions = {VarOptionPosition{
      "SPY", ProjectedMaturitySpec::years(1.0 / 365.0), 0.40, Side::Call, 1.0, 100.0}};
  const std::vector<VarScenario> scenarios = {{base.surface().pricing().now_ts_ns, &base.set(),
                                               shifted.surface().pricing().now_ts_ns,
                                               &shifted.set()}};

  // Reference behavior: FastScreenColdConfirm's scalar retained-leg route
  // (mirrors Var.ReplayReportsOptionExpiryBeforeScenarioEndExplicitly) --
  // what the fallback's downgraded config is expected to reproduce.
  auto fast_prepared =
      PreparedVarPortfolio::create(positions, reference.set(), evaluation_config(1));
  ASSERT_TRUE(fast_prepared);
  std::vector<VarScenarioFrame> fast_retained_frames(1u);
  std::vector<VarLegFrame> fast_legs(1u);
  ASSERT_TRUE(
      fast_prepared->replay_into(scenarios, fast_retained_frames, fast_legs, evaluation_config(1)));
  ASSERT_EQ(fast_legs[0].status, VarLegStatus::ExpiredBeforeShift);
  ASSERT_EQ(fast_retained_frames[0].status, VarScenarioStatus::LegFailure);
  ASSERT_EQ(fast_retained_frames[0].n_failed, 1u);

  std::vector<VarScenarioFrame> fast_aggregate_frames(1u);
  ASSERT_TRUE(
      fast_prepared->replay_into(scenarios, fast_aggregate_frames, {}, evaluation_config(1)));

  // New route: the group resolver fails this row (expired before the
  // shifted date), so evaluate_scenario_batched must fall back to the whole-
  // scenario scalar path and report the identical scenario-level status.
  VarEvaluationConfig cross_config = evaluation_config(1);
  cross_config.projection_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  auto cross_prepared = PreparedVarPortfolio::create(positions, reference.set(), cross_config);
  ASSERT_TRUE(cross_prepared);
  std::vector<VarScenarioFrame> cross_aggregate_frames(1u);
  ASSERT_TRUE(cross_prepared->replay_into(scenarios, cross_aggregate_frames, {}, cross_config));

  EXPECT_EQ(cross_aggregate_frames[0].status, VarScenarioStatus::LegFailure);
  EXPECT_EQ(cross_aggregate_frames[0].n_failed, 1u);
  EXPECT_EQ(cross_aggregate_frames[0].n_ok, 0u);
  EXPECT_EQ(cross_aggregate_frames[0].status, fast_aggregate_frames[0].status);
  EXPECT_EQ(cross_aggregate_frames[0].n_failed, fast_aggregate_frames[0].n_failed);
  EXPECT_EQ(cross_aggregate_frames[0].n_ok, fast_aggregate_frames[0].n_ok);
}

TEST(Var, CrossSectionalReplayIsBitInvariantAcrossThreadCountsAndLegOutputIsOptional) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());
  const std::vector<VarPosition> positions = {
      option("SPY", Side::Call, 1.25, 0.40),
      option("SPY", Side::Put, -0.75, 0.30),
      stock("SPY", 7.0),
  };
  const auto cross_config = [](unsigned n_threads) {
    VarEvaluationConfig config = evaluation_config(n_threads);
    config.projection_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
    return config;
  };
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), cross_config(1));
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());

  std::vector<std::unique_ptr<OneSurfaceSnapshot>> bases;
  std::vector<std::unique_ptr<OneSurfaceSnapshot>> shifts;
  const std::array<double, 4> base_spots{96.0, 103.0, 89.0, 111.0};
  const std::array<double, 4> shifted_spots{99.0, 101.0, 94.0, 107.0};
  for (std::size_t i = 0; i < base_spots.size(); ++i) {
    bases.push_back(std::make_unique<OneSurfaceSnapshot>(
        uid, base_spots[i], timestamp(2026, 1, 5 + static_cast<unsigned>(i))));
    shifts.push_back(std::make_unique<OneSurfaceSnapshot>(
        uid, shifted_spots[i], timestamp(2026, 1, 6 + static_cast<unsigned>(i))));
    ASSERT_TRUE(bases.back()->valid() && shifts.back()->valid());
  }

  std::vector<VarScenario> scenarios;
  scenarios.reserve(base_spots.size());
  for (std::size_t i = 0; i < base_spots.size(); ++i) {
    scenarios.push_back(VarScenario{bases[i]->surface().pricing().now_ts_ns, &bases[i]->set(),
                                    shifts[i]->surface().pricing().now_ts_ns, &shifts[i]->set()});
  }

  std::vector<VarScenarioFrame> serial_frames(scenarios.size());
  std::vector<VarScenarioFrame> parallel_frames(scenarios.size());
  std::vector<VarScenarioFrame> aggregate_only_frames(scenarios.size());
  std::vector<VarLegFrame> serial_legs(scenarios.size() * positions.size());
  std::vector<VarLegFrame> parallel_legs(scenarios.size() * positions.size());
  ASSERT_TRUE(prepared->replay_into(scenarios, serial_frames, serial_legs, cross_config(1)));
  ASSERT_TRUE(prepared->replay_into(scenarios, parallel_frames, parallel_legs, cross_config(4)));
  ASSERT_TRUE(prepared->replay_into(scenarios, aggregate_only_frames, std::span<VarLegFrame>{},
                                    cross_config(4)));

  for (std::size_t i = 0; i < scenarios.size(); ++i) {
    expect_bit_identical(parallel_frames[i], serial_frames[i]);
    expect_economically_equal(aggregate_only_frames[i], serial_frames[i]);
  }
  for (std::size_t i = 0; i < serial_legs.size(); ++i) {
    expect_bit_identical(parallel_legs[i], serial_legs[i]);
  }
}

TEST(Var, CrossSectionalAggregateMatchesRetainedLegOutput) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());

  PricedSurface base_source = make_surface(uid, 97.0, timestamp(2026, 1, 5), 1.15);
  PricedSurface shifted_source = make_surface(uid, 103.0, timestamp(2026, 1, 6), 0.90);
  auto base_fast = std::move(base_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  auto shifted_fast =
      std::move(shifted_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(base_fast && shifted_fast);
  const std::array<const PricedSurface *, 1> base_pointers{&*base_fast};
  const std::array<const PricedSurface *, 1> shifted_pointers{&*shifted_fast};
  auto base_set = SurfaceSet::create(base_pointers);
  auto shifted_set = SurfaceSet::create(shifted_pointers);
  ASSERT_TRUE(base_set && shifted_set);

  const std::array<double, 8> targets{0.10, 0.20, 0.30, 0.40, 0.55, 0.65, 0.75, 0.85};
  std::vector<VarPosition> positions;
  positions.reserve(targets.size());
  for (std::size_t index = 0u; index < targets.size(); ++index) {
    positions.push_back(
        option("SPY", index % 2u == 0u ? Side::Call : Side::Put, 1.0, targets[index]));
  }
  VarEvaluationConfig config = evaluation_config(1);
  config.projection_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());

  const std::vector<VarScenario> scenarios = {
      {base_fast->pricing().now_ts_ns, &*base_set, shifted_fast->pricing().now_ts_ns,
       &*shifted_set},
  };
  std::vector<VarScenarioFrame> retained_frames(1u);
  std::vector<VarScenarioFrame> aggregate_frames(1u);
  std::vector<VarLegFrame> legs(positions.size());
  ASSERT_TRUE(prepared->replay_into(scenarios, retained_frames, legs, config));
  ASSERT_TRUE(prepared->replay_into(scenarios, aggregate_frames, {}, config));
  EXPECT_EQ(aggregate_frames[0].status, retained_frames[0].status);
  EXPECT_EQ(aggregate_frames[0].n_ok, retained_frames[0].n_ok);
  EXPECT_EQ(aggregate_frames[0].n_failed, retained_frames[0].n_failed);
  const double value_scale = std::max(
      {1.0, std::fabs(retained_frames[0].base_value), std::fabs(retained_frames[0].shifted_value)});
  EXPECT_LE(std::fabs(aggregate_frames[0].base_value - retained_frames[0].base_value),
            1.0e-7 * value_scale);
  EXPECT_LE(std::fabs(aggregate_frames[0].shifted_value - retained_frames[0].shifted_value),
            1.0e-7 * value_scale);
  EXPECT_LE(std::fabs(aggregate_frames[0].pnl - retained_frames[0].pnl), 1.0e-7 * value_scale);
  EXPECT_LE(std::fabs(aggregate_frames[0].dollar_delta - retained_frames[0].dollar_delta),
            1.0e-9 * std::max(1.0, std::fabs(retained_frames[0].dollar_delta)));
  ASSERT_EQ(retained_frames[0].status, VarScenarioStatus::Ok);
  for (std::size_t index = 0u; index < legs.size(); ++index) {
    ASSERT_EQ(legs[index].status, VarLegStatus::Ok);
    EXPECT_NEAR(std::fabs(legs[index].base_delta), targets[index], config.delta_tolerance);
  }
}

TEST(Var, CrossSectionalRetainedLegsAreColdConfirmedPerLeg) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());

  PricedSurface base_source = make_surface(uid, 97.0, timestamp(2026, 1, 5), 1.15);
  PricedSurface shifted_source = make_surface(uid, 103.0, timestamp(2026, 1, 6), 0.90);
  auto base_fast = std::move(base_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  auto shifted_fast =
      std::move(shifted_source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(base_fast && shifted_fast);
  const std::array<const PricedSurface *, 1> base_pointers{&*base_fast};
  const std::array<const PricedSurface *, 1> shifted_pointers{&*shifted_fast};
  auto base_set = SurfaceSet::create(base_pointers);
  auto shifted_set = SurfaceSet::create(shifted_pointers);
  ASSERT_TRUE(base_set && shifted_set);

  const std::array<double, 8> targets{0.10, 0.20, 0.30, 0.40, 0.55, 0.65, 0.75, 0.85};
  std::vector<Side> sides;
  std::vector<VarPosition> positions;
  positions.reserve(targets.size());
  sides.reserve(targets.size());
  for (std::size_t index = 0u; index < targets.size(); ++index) {
    const Side side = index % 2u == 0u ? Side::Call : Side::Put;
    sides.push_back(side);
    positions.push_back(option("SPY", side, 1.0, targets[index]));
  }
  VarEvaluationConfig config = evaluation_config(1);
  config.projection_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());

  const std::vector<VarScenario> scenarios = {
      {base_fast->pricing().now_ts_ns, &*base_set, shifted_fast->pricing().now_ts_ns,
       &*shifted_set},
  };
  std::vector<VarScenarioFrame> retained_frames(1u);
  std::vector<VarLegFrame> legs(positions.size());
  ASSERT_TRUE(prepared->replay_into(scenarios, retained_frames, legs, config));
  ASSERT_EQ(retained_frames[0].status, VarScenarioStatus::Ok);

  for (std::size_t index = 0u; index < legs.size(); ++index) {
    ASSERT_EQ(legs[index].status, VarLegStatus::Ok);
    const auto cold_delta = base_fast->delta(legs[index].strike, legs[index].base_time_to_expiry,
                                             sides[index], QueryExecution::ColdReference);
    ASSERT_TRUE(cold_delta) << (cold_delta ? std::string{} : cold_delta.error().to_string());
    EXPECT_LE(std::fabs(std::fabs(*cold_delta) - targets[index]), config.delta_tolerance);
  }
}

// Not brief-named; added to close a coverage gap found during self-review.
// The three brief tests above only exercise the CrossSectionalColdConfirm
// retained-leg route on happy paths -- none reaches evaluate_scenario's new
// group-level market/timestamp guard or its downgrade-to-scalar fallback.
// Without that guard running before evaluate_option_leg_resolved, a missing
// surface would dereference a null SurfaceRef instead of reporting
// SurfaceUnavailable; this pins the guard/fallback wiring itself, mirroring
// CrossSectionalRowFailureFallsBackToScalarLegStatuses's aggregate-route
// coverage for the retained-leg route.
TEST(Var, CrossSectionalRetainedLegFallbackHandlesMarketUnavailable) {
  const std::uint32_t spy_uid = uid_for_symbol("SPY");
  const std::uint32_t aapl_uid = uid_for_symbol("AAPL");
  const OneSurfaceSnapshot reference(spy_uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0)};
  VarEvaluationConfig cross_config = evaluation_config(1);
  cross_config.projection_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  auto prepared = PreparedVarPortfolio::create(positions, reference.set(), cross_config);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());

  const OneSurfaceSnapshot wrong_base(aapl_uid, 190.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot wrong_shifted(aapl_uid, 191.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(wrong_base.valid() && wrong_shifted.valid());
  const std::vector<VarScenario> scenarios = {
      {wrong_base.surface().pricing().now_ts_ns, &wrong_base.set(),
       wrong_shifted.surface().pricing().now_ts_ns, &wrong_shifted.set()}};
  std::vector<VarScenarioFrame> frames(1u);
  std::vector<VarLegFrame> legs(1u);
  ASSERT_TRUE(prepared->replay_into(scenarios, frames, legs, cross_config));
  EXPECT_EQ(frames[0].status, VarScenarioStatus::MarketUnavailable);
  EXPECT_EQ(frames[0].n_ok, 0u);
  EXPECT_EQ(frames[0].n_failed, 1u);
  EXPECT_EQ(legs[0].status, VarLegStatus::SurfaceUnavailable);
}

// Mirrors the SP100 bench fixture's replay-exclusion flow at unit scale
// (var_bench.cpp prepare_replayable_portfolio): replay a book whose short-dated
// leg expires inside one scenario's base->shifted window, exclude the failed
// position, rebuild the portfolio, and require the aggregate (no-legs) route to
// match a FRESH retained-leg replay of the SAME rebuilt book at the bench's
// structural + 1e-9 same-route gate. The excluded row's presence flips the
// whole-scenario CrossSectionalColdConfirm fallback decision for every other
// leg in that scenario, so retained frames replayed on the SUPERSET book are
// not a valid oracle for the filtered book -- only a fresh same-book replay is.
TEST(Var, CrossSectionalAggregateMatchesRetainedAfterExclusionRebuild) {
  const std::uint32_t spy_uid = uid_for_symbol("SPY");
  const std::uint32_t aapl_uid = uid_for_symbol("AAPL");

  class TwoSurfaceSnapshot {
  public:
    TwoSurfaceSnapshot(std::uint32_t spy_uid, std::uint32_t aapl_uid, double spy_spot,
                       double aapl_spot, std::int64_t ts, double variance_scale = 1.0)
        : spy_(make_surface(spy_uid, spy_spot, ts, variance_scale)),
          aapl_(make_surface(aapl_uid, aapl_spot, ts, variance_scale)), pointers_{&spy_, &aapl_},
          set_(SurfaceSet::create(pointers_)) {}
    [[nodiscard]] bool valid() const noexcept { return set_.has_value(); }
    [[nodiscard]] const SurfaceSet &set() const noexcept { return *set_; }
    [[nodiscard]] std::int64_t ts() const noexcept { return spy_.pricing().now_ts_ns; }

  private:
    PricedSurface spy_;
    PricedSurface aapl_;
    std::array<const PricedSurface *, 2> pointers_{};
    Result<SurfaceSet> set_;
  };

  const TwoSurfaceSnapshot reference(spy_uid, aapl_uid, 100.0, 190.0, timestamp(2026, 1, 2));
  const TwoSurfaceSnapshot t0(spy_uid, aapl_uid, 99.0, 188.0, timestamp(2026, 1, 5), 1.10);
  const TwoSurfaceSnapshot t1(spy_uid, aapl_uid, 101.0, 191.0, timestamp(2026, 1, 6), 0.95);
  const TwoSurfaceSnapshot t2(spy_uid, aapl_uid, 103.0, 195.0, timestamp(2026, 2, 2), 1.05);
  const TwoSurfaceSnapshot t3(spy_uid, aapl_uid, 102.0, 193.0, timestamp(2026, 2, 3), 1.00);
  ASSERT_TRUE(reference.valid() && t0.valid() && t1.valid() && t2.valid() && t3.valid());

  // Poison row: ~20 calendar days to expiry at the reference anchor. Scenario 1
  // spans Jan 6 -> Feb 2 (27 days), so its replayed expiry (base + offset)
  // falls strictly inside the window and the SPY group's cross-sectional
  // resolution fails there, downgrading the WHOLE scenario (both groups) to
  // the scalar FastScreenColdConfirm fallback. Scenarios 0/2 span one day and
  // keep the row alive.
  std::vector<VarPosition> positions = {
      option("SPY", Side::Call, 1.25, 0.25),
      option("SPY", Side::Put, -0.75, 0.40),
      option("SPY", Side::Call, 2.0, 0.55),
      option("SPY", Side::Call, 3.0, 0.25), // duplicate anchor of position 0
      VarOptionPosition{"SPY", ProjectedMaturitySpec::years(20.0 / 365.25), 0.40, Side::Call, 1.0,
                        100.0},
      stock("SPY", 7.0),
      option("AAPL", Side::Call, 1.0, 0.30),
      option("AAPL", Side::Put, -1.0, 0.60),
  };
  const std::size_t poison_index = 4u;

  const VarEvaluationConfig config = evaluation_config(1);
  ASSERT_EQ(config.projection_solve_policy, OptionDeltaSolvePolicy::CrossSectionalColdConfirm);
  auto full = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(full) << (full ? std::string{} : full.error().to_string());

  const std::vector<VarScenario> scenarios = {
      {t0.ts(), &t0.set(), t1.ts(), &t1.set()},
      {t1.ts(), &t1.set(), t2.ts(), &t2.set()},
      {t2.ts(), &t2.set(), t3.ts(), &t3.set()},
  };
  std::vector<VarScenarioFrame> full_frames(scenarios.size());
  std::vector<VarLegFrame> full_legs(scenarios.size() * positions.size());
  ASSERT_TRUE(full->replay_into(scenarios, full_frames, full_legs, config));

  // The poison row must fail exactly scenario 1 (expired before the shifted
  // date) and every other leg must succeed on every scenario, so exclusion
  // removes exactly one position and scenario 1's fallback decision flips.
  EXPECT_EQ(full_frames[1].status, VarScenarioStatus::LegFailure);
  EXPECT_EQ(full_frames[1].n_failed, 1u);
  for (std::size_t scenario = 0u; scenario < scenarios.size(); ++scenario) {
    for (std::size_t position = 0u; position < positions.size(); ++position) {
      const VarLegFrame &leg = full_legs[scenario * positions.size() + position];
      if (scenario == 1u && position == poison_index) {
        EXPECT_EQ(leg.status, VarLegStatus::ExpiredBeforeShift);
      } else {
        ASSERT_EQ(leg.status, VarLegStatus::Ok)
            << "scenario " << scenario << " position " << position << ": " << to_string(leg.status);
      }
    }
  }

  positions.erase(positions.begin() + static_cast<std::ptrdiff_t>(poison_index));
  auto filtered = PreparedVarPortfolio::create(positions, reference.set(), config);
  ASSERT_TRUE(filtered) << (filtered ? std::string{} : filtered.error().to_string());

  std::vector<VarScenarioFrame> retained_frames(scenarios.size());
  std::vector<VarLegFrame> retained_legs(scenarios.size() * positions.size());
  ASSERT_TRUE(filtered->replay_into(scenarios, retained_frames, retained_legs, config));
  for (const VarLegFrame &leg : retained_legs) {
    ASSERT_EQ(leg.status, VarLegStatus::Ok) << to_string(leg.status);
  }

  std::vector<VarScenarioFrame> aggregate_frames(scenarios.size());
  ASSERT_TRUE(filtered->replay_into(scenarios, aggregate_frames, {}, config));

  for (std::size_t scenario = 0u; scenario < scenarios.size(); ++scenario) {
    const VarScenarioFrame &oracle = retained_frames[scenario];
    const VarScenarioFrame &aggregate = aggregate_frames[scenario];
    ASSERT_EQ(oracle.status, VarScenarioStatus::Ok);
    EXPECT_EQ(aggregate.status, oracle.status) << "scenario " << scenario;
    EXPECT_EQ(aggregate.n_ok, oracle.n_ok) << "scenario " << scenario;
    EXPECT_EQ(aggregate.n_failed, oracle.n_failed) << "scenario " << scenario;
    EXPECT_EQ(aggregate.definition_fingerprint, oracle.definition_fingerprint)
        << "scenario " << scenario;
    const double value_scale =
        std::max({1.0, std::fabs(oracle.base_value), std::fabs(oracle.shifted_value)});
    EXPECT_LE(std::fabs(aggregate.base_value - oracle.base_value), 1.0e-9 * value_scale)
        << "scenario " << scenario;
    EXPECT_LE(std::fabs(aggregate.shifted_value - oracle.shifted_value), 1.0e-9 * value_scale)
        << "scenario " << scenario;
    EXPECT_LE(std::fabs(aggregate.pnl - oracle.pnl), 1.0e-9 * value_scale)
        << "scenario " << scenario;
    EXPECT_LE(std::fabs(aggregate.dollar_delta - oracle.dollar_delta),
              1.0e-9 * std::max(1.0, std::fabs(oracle.dollar_delta)))
        << "scenario " << scenario;
  }
}

TEST(Var, HistoricalStatisticsUseNearestRankLossAndInclusiveExpectedShortfall) {
  const std::array<double, 5> pnl{10.0, 5.0, 0.0, -5.0, -10.0};
  std::vector<VarScenarioFrame> frames(pnl.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    frames[i].base_ts_ns = static_cast<std::int64_t>(i + 1u);
    frames[i].shifted_ts_ns = static_cast<std::int64_t>(i + 2u);
    frames[i].status = VarScenarioStatus::Ok;
    frames[i].base_value = 100.0;
    frames[i].shifted_value = 100.0 + pnl[i];
    frames[i].pnl = pnl[i];
    frames[i].dollar_delta = 1000.0;
    frames[i].n_ok = 1u;
    frames[i].definition_fingerprint = i + 1u;
  }

  auto risk = historical_var_statistics(frames, 0.80);
  ASSERT_TRUE(risk) << (risk ? std::string{} : risk.error().to_string());
  EXPECT_DOUBLE_EQ(risk->confidence, 0.80);
  EXPECT_DOUBLE_EQ(risk->value_at_risk, 5.0);
  EXPECT_DOUBLE_EQ(risk->expected_shortfall, 7.5);
  EXPECT_EQ(risk->n_scenarios, 5u);
  EXPECT_FALSE(historical_var_statistics(frames, 0.0));
  EXPECT_FALSE(historical_var_statistics(frames, 1.0));
}

TEST(Var, SurfaceDbRunSortsThreeDatesAndProducesAdjacentScenariosEndToEnd) {
  const ScopedTempDirectory root("surface_db_range");
  auto db = SurfaceDb::create(root.path().string());
  ASSERT_TRUE(db) << (db ? std::string{} : db.error().to_string());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}));

  const std::uint32_t uid = uid_for_symbol("SPY");
  const PricedSurface jan2 = make_surface(uid, 100.0, timestamp(2026, 1, 2));
  const PricedSurface jan5 = make_surface(uid, 103.0, timestamp(2026, 1, 5));
  const PricedSurface jan6 = make_surface(uid, 98.0, timestamp(2026, 1, 6));

  // Deliberately publish out of order: the runner must use the database Clock's
  // ascending order and form Jan-02 -> Jan-05 -> Jan-06 adjacent observations.
  const std::array<SurfaceArchiveItem, 1> items_jan6{{{"SPY", &jan6}}};
  const std::array<SurfaceArchiveItem, 1> items_jan2{{{"SPY", &jan2}}};
  const std::array<SurfaceArchiveItem, 1> items_jan5{{{"SPY", &jan5}}};
  ASSERT_TRUE(db->write_partition("2026-01-06", items_jan6));
  ASSERT_TRUE(db->write_partition("2026-01-02", items_jan2));
  ASSERT_TRUE(db->write_partition("2026-01-05", items_jan5));

  const std::vector<VarPosition> positions = {
      option("SPY", Side::Call, 1.0),
      stock("SPY", -20.0),
  };
  VarRunConfig config;
  config.reference_date = "2026-01-06";
  config.date_begin = "2026-01-02";
  config.date_end = "2026-01-06";
  config.confidence = 0.50;
  config.evaluation = evaluation_config(2);
  config.failure_policy = VarScenarioFailurePolicy::RejectRun;
  config.retain_leg_frames = true;
  config.archive_backing = ArchiveBacking::Mutable;
  config.query_pricing_tier = QueryPricingTier::ColdReference;
  config.provenance_policy = SurfaceProvenancePolicy::Compatibility;

  auto result = run_historical_var(*db, positions, config);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_EQ(result->reference_date, "2026-01-06");
  EXPECT_EQ(result->reference_ts_ns, timestamp(2026, 1, 6));
  EXPECT_TRUE(std::isfinite(result->reference_value));
  EXPECT_TRUE(std::isfinite(result->reference_dollar_delta));
  EXPECT_EQ(result->n_legs, positions.size());
  EXPECT_EQ(result->base_dates, (std::vector<std::string>{"2026-01-02", "2026-01-05"}));
  EXPECT_EQ(result->shifted_dates, (std::vector<std::string>{"2026-01-05", "2026-01-06"}));
  ASSERT_EQ(result->frames.size(), 2u);
  ASSERT_EQ(result->leg_frames.size(), result->frames.size() * result->n_legs);
  for (const VarScenarioFrame &frame : result->frames) {
    EXPECT_EQ(frame.status, VarScenarioStatus::Ok);
    EXPECT_EQ(frame.n_ok, positions.size());
    EXPECT_EQ(frame.n_failed, 0u);
    EXPECT_TRUE(std::isfinite(frame.pnl));
  }
  for (const VarLegFrame &leg : result->leg_frames) {
    EXPECT_EQ(leg.status, VarLegStatus::Ok);
  }
  EXPECT_DOUBLE_EQ(result->risk.confidence, config.confidence);
  EXPECT_EQ(result->risk.n_scenarios, result->frames.size());
  auto independently_computed = historical_var_statistics(result->frames, config.confidence);
  ASSERT_TRUE(independently_computed);
  EXPECT_EQ(result->risk, *independently_computed);

  // Explicit-policy variant: run the full SurfaceDb pipeline (reference AND
  // every scenario) once under the cross-sectional default and once under
  // the scalar Direct oracle. This is a genuine cross-route comparison per
  // CORRECTNESS GATE 2 -- not merely two solve paths sharing one prepared
  // portfolio -- exercised end to end through run_historical_var.
  VarRunConfig cross_run_config = config;
  cross_run_config.retain_leg_frames = false;
  cross_run_config.evaluation.projection_solve_policy =
      OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
  auto cross_result = run_historical_var(*db, positions, cross_run_config);
  ASSERT_TRUE(cross_result) << (cross_result ? std::string{} : cross_result.error().to_string());

  VarRunConfig direct_run_config = config;
  direct_run_config.retain_leg_frames = false;
  direct_run_config.evaluation.projection_solve_policy = OptionDeltaSolvePolicy::Direct;
  auto direct_result = run_historical_var(*db, positions, direct_run_config);
  ASSERT_TRUE(direct_result) << (direct_result ? std::string{} : direct_result.error().to_string());

  ASSERT_EQ(cross_result->frames.size(), direct_result->frames.size());
  for (std::size_t index = 0u; index < cross_result->frames.size(); ++index) {
    const VarScenarioFrame &cross_frame = cross_result->frames[index];
    const VarScenarioFrame &direct_frame = direct_result->frames[index];
    EXPECT_EQ(cross_frame.status, direct_frame.status);
    EXPECT_EQ(cross_frame.n_ok, direct_frame.n_ok);
    EXPECT_EQ(cross_frame.n_failed, direct_frame.n_failed);
    ASSERT_EQ(cross_frame.status, VarScenarioStatus::Ok);
    const double value_scale =
        std::max({1.0, std::fabs(direct_frame.base_value), std::fabs(direct_frame.shifted_value)});
    EXPECT_LE(std::fabs(cross_frame.base_value - direct_frame.base_value), 1.0e-5 * value_scale);
    EXPECT_LE(std::fabs(cross_frame.shifted_value - direct_frame.shifted_value),
              1.0e-5 * value_scale);
    EXPECT_LE(std::fabs(cross_frame.pnl - direct_frame.pnl), 1.0e-5 * value_scale);
  }

  const double var_scale = std::max(1.0, std::fabs(direct_result->risk.value_at_risk));
  EXPECT_LE(std::fabs(cross_result->risk.value_at_risk - direct_result->risk.value_at_risk),
            1.0e-5 * var_scale);
  const double es_scale = std::max(1.0, std::fabs(direct_result->risk.expected_shortfall));
  EXPECT_LE(
      std::fabs(cross_result->risk.expected_shortfall - direct_result->risk.expected_shortfall),
      1.0e-5 * es_scale);

  config.retain_leg_frames = false;
  auto aggregate_only = run_historical_var(*db, positions, config);
  ASSERT_TRUE(aggregate_only) << (aggregate_only ? std::string{}
                                                 : aggregate_only.error().to_string());
  EXPECT_TRUE(aggregate_only->leg_frames.empty());
  ASSERT_EQ(aggregate_only->frames.size(), result->frames.size());
  for (std::size_t index = 0u; index < result->frames.size(); ++index) {
    expect_economically_equal(aggregate_only->frames[index], result->frames[index]);
  }
}

// [solver] F4: run_historical_var's load_snapshot (var.cpp) collapsed every
// MarketSnapshot::load failure -- a genuinely absent surface, a corrupt or
// truncated archive, or a plain I/O error -- into the same
// VarLegStatus::SurfaceUnavailable / VarScenarioStatus::MarketUnavailable
// pair, discarding the underlying error. Under
// VarScenarioFailurePolicy::ExcludeFromDistribution that made a corrupt
// archive economically indistinguishable from an ordinary missing-surface
// day: the scenario is silently dropped from the loss distribution rather
// than failing the run. This test corrupts one archive partition (truncated
// below the ATXVSA header, the shape of a partial/interrupted write) that
// sits strictly between two GOOD scenarios in the window, so that without
// the new ArchiveError classification and its explicit
// ExcludeFromDistribution override, historical_var_statistics would still
// have one Ok scenario to compute from and run_historical_var would return
// Ok -- silently masking the corruption.
TEST(Var, CorruptArchiveReportsArchiveErrorAndFailsRunUnderExcludeFromDistribution) {
  const ScopedTempDirectory root("corrupt_archive");
  auto db = SurfaceDb::create(root.path().string());
  ASSERT_TRUE(db) << (db ? std::string{} : db.error().to_string());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}));

  const std::uint32_t uid = uid_for_symbol("SPY");
  const PricedSurface ref_surface = make_surface(uid, 100.0, timestamp(2026, 1, 2));
  const PricedSurface d05 = make_surface(uid, 100.0, timestamp(2026, 1, 5));
  const PricedSurface d06 = make_surface(uid, 100.0, timestamp(2026, 1, 6));
  const PricedSurface d07 = make_surface(uid, 100.0, timestamp(2026, 1, 7));
  const PricedSurface d08 = make_surface(uid, 100.0, timestamp(2026, 1, 8));

  const std::array<SurfaceArchiveItem, 1> items_ref{{{"SPY", &ref_surface}}};
  const std::array<SurfaceArchiveItem, 1> items_05{{{"SPY", &d05}}};
  const std::array<SurfaceArchiveItem, 1> items_06{{{"SPY", &d06}}};
  const std::array<SurfaceArchiveItem, 1> items_07{{{"SPY", &d07}}};
  const std::array<SurfaceArchiveItem, 1> items_08{{{"SPY", &d08}}};
  ASSERT_TRUE(db->write_partition("2026-01-02", items_ref));
  ASSERT_TRUE(db->write_partition("2026-01-05", items_05));
  ASSERT_TRUE(db->write_partition("2026-01-06", items_06));
  ASSERT_TRUE(db->write_partition("2026-01-07", items_07));
  ASSERT_TRUE(db->write_partition("2026-01-08", items_08));

  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock) << (clock ? std::string{} : clock.error().to_string());
  const auto corrupt_ref =
      std::find_if(clock->refs().begin(), clock->refs().end(),
                   [](const SnapshotRef &ref) { return ref.date == "2026-01-06"; });
  ASSERT_NE(corrupt_ref, clock->refs().end());
  {
    std::error_code ec;
    std::filesystem::resize_file(corrupt_ref->archive_path, 8, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0)};
  VarRunConfig config;
  config.reference_date = "2026-01-02";
  config.date_begin = "2026-01-05";
  config.date_end = "2026-01-08";
  config.confidence = 0.50;
  config.evaluation = evaluation_config(1);
  config.failure_policy = VarScenarioFailurePolicy::ExcludeFromDistribution;
  config.archive_backing = ArchiveBacking::Mutable;
  config.query_pricing_tier = QueryPricingTier::ColdReference;
  config.provenance_policy = SurfaceProvenancePolicy::Compatibility;

  auto result = run_historical_var(*db, positions, config);
  ASSERT_FALSE(result) << "corrupt-archive scenarios must fail the run even under "
                          "ExcludeFromDistribution, not be silently excluded";
  EXPECT_NE(result.error().to_string().find("ArchiveError"), std::string::npos)
      << result.error().to_string();
}

// [solver] F5: run_historical_var's per-scenario retry (var.cpp, the block
// that calls PreparedVarPortfolio::replay_into and, on failure, poisons the
// scenario/leg frames) labeled EVERY replay_into failure
// VarLegStatus::InvalidValue -- including the specific case of two adjacent
// archive partitions whose embedded valuation timestamps are non-monotone
// (shifted_ts_ns <= base_ts_ns), which fails replay_into's own scenario
// validation. That is a structural TimestampMismatch, not a generic
// InvalidValue. This test builds a partition "2026-01-06" whose surface
// embeds an EARLIER now_ts_ns than "2026-01-05" -- e.g. a corrupted or
// misattributed archive write -- so partition KEYS still sort correctly
// while the embedded timestamps do not: the 05 -> 06 transition is
// non-monotone even though the 06 -> 07 transition (embedded Jan-04 ->
// Jan-07) is not, isolating the failure to exactly one scenario.
TEST(Var, NonMonotoneArchiveTimestampsReportTimestampMismatch) {
  const ScopedTempDirectory root("nonmonotone_ts");
  auto db = SurfaceDb::create(root.path().string());
  ASSERT_TRUE(db) << (db ? std::string{} : db.error().to_string());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}));

  const std::uint32_t uid = uid_for_symbol("SPY");
  const PricedSurface ref_surface = make_surface(uid, 100.0, timestamp(2026, 1, 2));
  const PricedSurface d05 = make_surface(uid, 100.0, timestamp(2026, 1, 5));
  // Deliberately regressed: "2026-01-06"'s partition embeds Jan-04, earlier
  // than "2026-01-05"'s own embedded timestamp.
  const PricedSurface d06 = make_surface(uid, 100.0, timestamp(2026, 1, 4));
  const PricedSurface d07 = make_surface(uid, 100.0, timestamp(2026, 1, 7));

  const std::array<SurfaceArchiveItem, 1> items_ref{{{"SPY", &ref_surface}}};
  const std::array<SurfaceArchiveItem, 1> items_05{{{"SPY", &d05}}};
  const std::array<SurfaceArchiveItem, 1> items_06{{{"SPY", &d06}}};
  const std::array<SurfaceArchiveItem, 1> items_07{{{"SPY", &d07}}};
  ASSERT_TRUE(db->write_partition("2026-01-02", items_ref));
  ASSERT_TRUE(db->write_partition("2026-01-05", items_05));
  ASSERT_TRUE(db->write_partition("2026-01-06", items_06));
  ASSERT_TRUE(db->write_partition("2026-01-07", items_07));

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0)};
  VarRunConfig config;
  config.reference_date = "2026-01-02";
  config.date_begin = "2026-01-05";
  config.date_end = "2026-01-07";
  config.confidence = 0.50;
  config.evaluation = evaluation_config(1);
  config.failure_policy = VarScenarioFailurePolicy::ExcludeFromDistribution;
  config.retain_leg_frames = true;
  config.archive_backing = ArchiveBacking::Mutable;
  config.query_pricing_tier = QueryPricingTier::ColdReference;
  config.provenance_policy = SurfaceProvenancePolicy::Compatibility;

  auto result = run_historical_var(*db, positions, config);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  ASSERT_EQ(result->frames.size(), 2u);
  EXPECT_EQ(result->base_dates[0], "2026-01-05");
  EXPECT_EQ(result->shifted_dates[0], "2026-01-06");
  EXPECT_EQ(result->frames[0].status, VarScenarioStatus::TimestampMismatch);
  EXPECT_EQ(result->frames[0].n_failed, positions.size());
  ASSERT_EQ(result->leg_frames.size(), result->frames.size() * result->n_legs);
  EXPECT_EQ(result->leg_frames[0].status, VarLegStatus::TimestampMismatch);
  // The second (06 -> 07) transition (embedded Jan-04 -> Jan-07) is monotone
  // and unaffected by the corrupted 05 -> 06 transition.
  EXPECT_EQ(result->frames[1].status, VarScenarioStatus::Ok);
}

// [proj] I1: a wholly missing run of partitions (a fit failure day, an
// ingestion hole) must not bridge silently into a single multi-session
// observation. Fixture: 01-02 -> 01-05 (3 calendar days, an ordinary
// weekend) -> 01-15 (10 calendar days, an induced hole) -> 01-16 (1 day).
TEST(Var, GapPolicySkipsAndCountsTransitionsBeyondMaxSessionGapDays) {
  const ScopedTempDirectory root("gap_policy");
  auto db = SurfaceDb::create(root.path().string());
  ASSERT_TRUE(db) << (db ? std::string{} : db.error().to_string());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}));

  const std::uint32_t uid = uid_for_symbol("SPY");
  const PricedSurface d02 = make_surface(uid, 100.0, timestamp(2026, 1, 2));
  const PricedSurface d05 = make_surface(uid, 101.0, timestamp(2026, 1, 5));
  const PricedSurface d15 = make_surface(uid, 104.0, timestamp(2026, 1, 15));
  const PricedSurface d16 = make_surface(uid, 103.0, timestamp(2026, 1, 16));

  const std::array<SurfaceArchiveItem, 1> items_02{{{"SPY", &d02}}};
  const std::array<SurfaceArchiveItem, 1> items_05{{{"SPY", &d05}}};
  const std::array<SurfaceArchiveItem, 1> items_15{{{"SPY", &d15}}};
  const std::array<SurfaceArchiveItem, 1> items_16{{{"SPY", &d16}}};
  ASSERT_TRUE(db->write_partition("2026-01-02", items_02));
  ASSERT_TRUE(db->write_partition("2026-01-05", items_05));
  ASSERT_TRUE(db->write_partition("2026-01-15", items_15));
  ASSERT_TRUE(db->write_partition("2026-01-16", items_16));

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0)};
  VarRunConfig base_config;
  base_config.reference_date = "2026-01-16";
  base_config.date_begin = "2026-01-02";
  base_config.date_end = "2026-01-16";
  base_config.confidence = 0.50;
  base_config.evaluation = evaluation_config(1);
  base_config.archive_backing = ArchiveBacking::Mutable;
  base_config.query_pricing_tier = QueryPricingTier::ColdReference;
  base_config.provenance_policy = SurfaceProvenancePolicy::Compatibility;

  // gap 0 (default): the guard is disabled -- every adjacent partition
  // bridges, current behavior preserved.
  auto bridged = run_historical_var(*db, positions, base_config);
  ASSERT_TRUE(bridged) << (bridged ? std::string{} : bridged.error().to_string());
  EXPECT_EQ(bridged->n_gap_skipped, 0u);
  ASSERT_EQ(bridged->frames.size(), 3u);
  EXPECT_EQ(bridged->base_dates,
            (std::vector<std::string>{"2026-01-02", "2026-01-05", "2026-01-15"}));
  EXPECT_EQ(bridged->shifted_dates,
            (std::vector<std::string>{"2026-01-05", "2026-01-15", "2026-01-16"}));

  // gap 5: the 01-05 -> 01-15 (10 calendar day) transition must be skipped
  // and counted; the two normal-gap transitions survive.
  VarRunConfig gapped_config = base_config;
  gapped_config.max_session_gap_days = 5;
  auto gapped = run_historical_var(*db, positions, gapped_config);
  ASSERT_TRUE(gapped) << (gapped ? std::string{} : gapped.error().to_string());
  EXPECT_EQ(gapped->n_gap_skipped, 1u);
  ASSERT_EQ(gapped->frames.size(), 2u);
  EXPECT_EQ(gapped->base_dates, (std::vector<std::string>{"2026-01-02", "2026-01-15"}));
  EXPECT_EQ(gapped->shifted_dates, (std::vector<std::string>{"2026-01-05", "2026-01-16"}));
  for (const VarScenarioFrame &frame : gapped->frames) {
    EXPECT_EQ(frame.status, VarScenarioStatus::Ok);
  }
}

// [proj] I5: ExcludeFromDistribution shrinks the sample silently today.
// Reuses NonMonotoneArchiveTimestampsReportTimestampMismatch's fixture shape
// (one deliberately non-monotone embedded timestamp) to induce exactly one
// TimestampMismatch scenario alongside one Ok scenario.
TEST(Var, ExclusionAccountingCountsFailedScenariosAndMaxExcludedFractionCanFailRun) {
  const ScopedTempDirectory root("exclusion_accounting");
  auto db = SurfaceDb::create(root.path().string());
  ASSERT_TRUE(db) << (db ? std::string{} : db.error().to_string());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}));

  const std::uint32_t uid = uid_for_symbol("SPY");
  const PricedSurface ref_surface = make_surface(uid, 100.0, timestamp(2026, 1, 2));
  const PricedSurface d05 = make_surface(uid, 100.0, timestamp(2026, 1, 5));
  // Deliberately regressed embedded timestamp (earlier than d05's own),
  // inducing a non-monotone base -> shifted pair for exactly one scenario.
  const PricedSurface d06 = make_surface(uid, 100.0, timestamp(2026, 1, 4));
  const PricedSurface d07 = make_surface(uid, 100.0, timestamp(2026, 1, 7));

  const std::array<SurfaceArchiveItem, 1> items_ref{{{"SPY", &ref_surface}}};
  const std::array<SurfaceArchiveItem, 1> items_05{{{"SPY", &d05}}};
  const std::array<SurfaceArchiveItem, 1> items_06{{{"SPY", &d06}}};
  const std::array<SurfaceArchiveItem, 1> items_07{{{"SPY", &d07}}};
  ASSERT_TRUE(db->write_partition("2026-01-02", items_ref));
  ASSERT_TRUE(db->write_partition("2026-01-05", items_05));
  ASSERT_TRUE(db->write_partition("2026-01-06", items_06));
  ASSERT_TRUE(db->write_partition("2026-01-07", items_07));

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0)};
  VarRunConfig config;
  config.reference_date = "2026-01-02";
  config.date_begin = "2026-01-05";
  config.date_end = "2026-01-07";
  config.confidence = 0.50;
  config.evaluation = evaluation_config(1);
  config.failure_policy = VarScenarioFailurePolicy::ExcludeFromDistribution;
  config.archive_backing = ArchiveBacking::Mutable;
  config.query_pricing_tier = QueryPricingTier::ColdReference;
  config.provenance_policy = SurfaceProvenancePolicy::Compatibility;

  auto excluded_ok = run_historical_var(*db, positions, config);
  ASSERT_TRUE(excluded_ok) << (excluded_ok ? std::string{} : excluded_ok.error().to_string());
  ASSERT_EQ(excluded_ok->frames.size(), 2u);
  EXPECT_EQ(excluded_ok->frames[0].status, VarScenarioStatus::TimestampMismatch);
  EXPECT_EQ(excluded_ok->frames[1].status, VarScenarioStatus::Ok);
  EXPECT_EQ(excluded_ok->n_excluded_from_distribution, 1u);

  // max_excluded_fraction = 0.0 disallows ANY exclusion: the 1-of-2 (50%)
  // excluded fraction here must fail the run outright.
  config.max_excluded_fraction = 0.0;
  auto rejected = run_historical_var(*db, positions, config);
  ASSERT_FALSE(rejected)
      << "excluded fraction 0.5 must fail the run when max_excluded_fraction=0.0";
  EXPECT_NE(rejected.error().to_string().find("max_excluded_fraction"), std::string::npos)
      << rejected.error().to_string();
}

// [proj] I4: a restrike root this far into the wing is pure parametric
// extrapolation -- no quote has ever existed there. A deliberately tight
// bound (0.001) is exceeded by ANY non-ATM restrike, so this reliably
// exercises the reject path without depending on exact vol calibration.
TEST(Var, RestrikeBeyondWingBoundFailsWithInvalidDelta) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());
  const OneSurfaceSnapshot base(uid, 100.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 101.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(base.valid() && shifted.valid());

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0, 0.35)};
  const std::vector<VarScenario> scenarios = {{base.surface().pricing().now_ts_ns, &base.set(),
                                               shifted.surface().pricing().now_ts_ns,
                                               &shifted.set()}};

  VarEvaluationConfig tight_config = evaluation_config(1);
  tight_config.max_restrike_abs_log_moneyness = 0.001;
  auto tight_prepared = PreparedVarPortfolio::create(positions, reference.set(), tight_config);
  ASSERT_TRUE(tight_prepared) << (tight_prepared ? std::string{}
                                                 : tight_prepared.error().to_string());

  std::vector<VarScenarioFrame> retained_frames(1u);
  std::vector<VarLegFrame> legs(1u);
  ASSERT_TRUE(tight_prepared->replay_into(scenarios, retained_frames, legs, tight_config));
  ASSERT_EQ(legs[0].status, VarLegStatus::InvalidDelta) << to_string(legs[0].status);
  EXPECT_EQ(retained_frames[0].status, VarScenarioStatus::LegFailure);

  // Aggregate route must reject the SAME leg for parity on the scenario totals.
  std::vector<VarScenarioFrame> aggregate_frames(1u);
  ASSERT_TRUE(tight_prepared->replay_into(scenarios, aggregate_frames, {}, tight_config));
  EXPECT_EQ(aggregate_frames[0].status, VarScenarioStatus::LegFailure);
  EXPECT_EQ(aggregate_frames[0].n_failed, 1u);

  // The default bound (5.0, today's implicit behavior) must accept the SAME
  // restrike normally.
  const VarEvaluationConfig wide_config = evaluation_config(1);
  auto wide_prepared = PreparedVarPortfolio::create(positions, reference.set(), wide_config);
  ASSERT_TRUE(wide_prepared);
  std::vector<VarScenarioFrame> wide_frames(1u);
  std::vector<VarLegFrame> wide_legs(1u);
  ASSERT_TRUE(wide_prepared->replay_into(scenarios, wide_frames, wide_legs, wide_config));
  EXPECT_EQ(wide_legs[0].status, VarLegStatus::Ok) << to_string(wide_legs[0].status);
}

// [proj] I4: the (0.8x, 1.0x] band below the bound is an early-warning flag,
// not a rejection. Derive a bound from the wide-open run's own resolved
// strike so the test needs no fragile numeric assumptions about calibration.
TEST(Var, RestrikeNearWingBoundSetsEarlyWarningFlagWithoutFailingTheLeg) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());
  const OneSurfaceSnapshot base(uid, 100.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 101.0, timestamp(2026, 1, 6));
  ASSERT_TRUE(base.valid() && shifted.valid());

  const std::vector<VarPosition> positions = {option("SPY", Side::Call, 1.0, 0.35)};
  const std::vector<VarScenario> scenarios = {{base.surface().pricing().now_ts_ns, &base.set(),
                                               shifted.surface().pricing().now_ts_ns,
                                               &shifted.set()}};

  const VarEvaluationConfig wide_config = evaluation_config(1);
  auto wide_prepared = PreparedVarPortfolio::create(positions, reference.set(), wide_config);
  ASSERT_TRUE(wide_prepared);
  std::vector<VarScenarioFrame> wide_frames(1u);
  std::vector<VarLegFrame> wide_legs(1u);
  ASSERT_TRUE(wide_prepared->replay_into(scenarios, wide_frames, wide_legs, wide_config));
  ASSERT_EQ(wide_legs[0].status, VarLegStatus::Ok);
  EXPECT_EQ(wide_legs[0].diagnostic_flags & 0x4u, 0u);

  const double forward = base.surface().forward_at(wide_legs[0].base_time_to_expiry);
  ASSERT_TRUE(std::isfinite(forward) && forward > 0.0);
  const double log_moneyness = std::log(wide_legs[0].strike / forward);
  ASSERT_GT(std::fabs(log_moneyness), 0.0);

  VarEvaluationConfig near_config = evaluation_config(1);
  near_config.max_restrike_abs_log_moneyness = std::fabs(log_moneyness) / 0.9;
  auto near_prepared = PreparedVarPortfolio::create(positions, reference.set(), near_config);
  ASSERT_TRUE(near_prepared);
  std::vector<VarScenarioFrame> near_frames(1u);
  std::vector<VarLegFrame> near_legs(1u);
  ASSERT_TRUE(near_prepared->replay_into(scenarios, near_frames, near_legs, near_config));
  ASSERT_EQ(near_legs[0].status, VarLegStatus::Ok)
      << "0.9x the bound must still be accepted -- only beyond the bound rejects";
  EXPECT_NE(near_legs[0].diagnostic_flags & 0x4u, 0u)
      << "0.9x the bound is inside the (0.8x, 1.0x] early-warning band";
}

// [proj] I3: plumb extrapolates_tenor into VarLegFrame::diagnostic_flags,
// telemetry only (no status/value change on the fixture paths).
TEST(Var, DiagnosticFlagsRecordTenorExtrapolationOnBothSidesAndStayClearInsideTheFittedDomain) {
  const std::uint32_t uid = uid_for_symbol("SPY");
  const OneSurfaceSnapshot reference(uid, 100.0, timestamp(2026, 1, 2));
  ASSERT_TRUE(reference.valid());
  const OneSurfaceSnapshot base(uid, 100.0, timestamp(2026, 1, 5));
  const OneSurfaceSnapshot shifted(uid, 100.5, timestamp(2026, 1, 6));
  ASSERT_TRUE(base.valid() && shifted.valid());

  const VarEvaluationConfig config = evaluation_config(1);
  const std::vector<VarScenario> scenarios = {{base.surface().pricing().now_ts_ns, &base.set(),
                                               shifted.surface().pricing().now_ts_ns,
                                               &shifted.set()}};

  // Short (2 calendar day) template: base_time (~2/365.25y) and shifted_time
  // (~1/365.25y) both fall below make_surface's front pillar (0.03y), so
  // BOTH sides extrapolate.
  const std::vector<VarPosition> short_positions = {
      VarOptionPosition{"SPY", ProjectedMaturitySpec::days(2), 0.40, Side::Call, 1.0, 100.0}};
  auto short_prepared = PreparedVarPortfolio::create(short_positions, reference.set(), config);
  ASSERT_TRUE(short_prepared) << (short_prepared ? std::string{}
                                                 : short_prepared.error().to_string());
  std::vector<VarScenarioFrame> short_frames(1u);
  std::vector<VarLegFrame> short_legs(1u);
  ASSERT_TRUE(short_prepared->replay_into(scenarios, short_frames, short_legs, config));
  ASSERT_EQ(short_legs[0].status, VarLegStatus::Ok) << to_string(short_legs[0].status);
  EXPECT_EQ(short_legs[0].diagnostic_flags, 0x3u)
      << "bits 0 and 1 (only) must be set; bit 2 stays clear at the default 5.0 bound";

  // The aggregate route has no per-leg VarLegFrame output, but the scenario
  // totals it DOES produce must still match the retained-leg route exactly.
  std::vector<VarScenarioFrame> short_aggregate_frames(1u);
  ASSERT_TRUE(short_prepared->replay_into(scenarios, short_aggregate_frames, {}, config));
  EXPECT_EQ(short_aggregate_frames[0].status, short_frames[0].status);
  EXPECT_EQ(short_aggregate_frames[0].n_ok, short_frames[0].n_ok);
  EXPECT_EQ(short_aggregate_frames[0].n_failed, short_frames[0].n_failed);

  // Normal 3-month template stays inside [0.03, 1.00] on both dates: no bits.
  const std::vector<VarPosition> normal_positions = {option("SPY", Side::Call, 1.0)};
  auto normal_prepared = PreparedVarPortfolio::create(normal_positions, reference.set(), config);
  ASSERT_TRUE(normal_prepared);
  std::vector<VarScenarioFrame> normal_frames(1u);
  std::vector<VarLegFrame> normal_legs(1u);
  ASSERT_TRUE(normal_prepared->replay_into(scenarios, normal_frames, normal_legs, config));
  ASSERT_EQ(normal_legs[0].status, VarLegStatus::Ok);
  EXPECT_EQ(normal_legs[0].diagnostic_flags, 0u);
}

// [proj] I3: HistoricalVarResult::n_tenor_extrapolated_legs aggregates
// diagnostic_flags bits 0/1 across leg_frames -- only meaningful when
// VarRunConfig::retain_leg_frames is true, since it is a leg_frames rollup.
TEST(Var, NTenorExtrapolatedLegsAggregatesFlaggedLegFramesEndToEnd) {
  const ScopedTempDirectory root("tenor_extrapolation_count");
  auto db = SurfaceDb::create(root.path().string());
  ASSERT_TRUE(db) << (db ? std::string{} : db.error().to_string());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}));

  const std::uint32_t uid = uid_for_symbol("SPY");
  const PricedSurface d02 = make_surface(uid, 100.0, timestamp(2026, 1, 2));
  const PricedSurface d05 = make_surface(uid, 100.0, timestamp(2026, 1, 5));
  const PricedSurface d06 = make_surface(uid, 100.5, timestamp(2026, 1, 6));
  ASSERT_TRUE(
      db->write_partition("2026-01-02", std::array<SurfaceArchiveItem, 1>{{{"SPY", &d02}}}));
  ASSERT_TRUE(
      db->write_partition("2026-01-05", std::array<SurfaceArchiveItem, 1>{{{"SPY", &d05}}}));
  ASSERT_TRUE(
      db->write_partition("2026-01-06", std::array<SurfaceArchiveItem, 1>{{{"SPY", &d06}}}));

  const std::vector<VarPosition> positions = {
      VarOptionPosition{"SPY", ProjectedMaturitySpec::days(2), 0.40, Side::Call, 1.0, 100.0}};
  VarRunConfig config;
  config.reference_date = "2026-01-02";
  config.date_begin = "2026-01-05";
  config.date_end = "2026-01-06";
  config.confidence = 0.50;
  config.evaluation = evaluation_config(1);
  config.archive_backing = ArchiveBacking::Mutable;
  config.query_pricing_tier = QueryPricingTier::ColdReference;
  config.provenance_policy = SurfaceProvenancePolicy::Compatibility;

  // Not retained: the counter must stay 0 -- it is a leg_frames rollup and
  // leg_frames itself is empty here.
  config.retain_leg_frames = false;
  auto not_retained = run_historical_var(*db, positions, config);
  ASSERT_TRUE(not_retained) << (not_retained ? std::string{} : not_retained.error().to_string());
  EXPECT_EQ(not_retained->n_tenor_extrapolated_legs, 0u);
  EXPECT_TRUE(not_retained->leg_frames.empty());

  config.retain_leg_frames = true;
  auto retained = run_historical_var(*db, positions, config);
  ASSERT_TRUE(retained) << (retained ? std::string{} : retained.error().to_string());
  ASSERT_EQ(retained->leg_frames.size(), 1u);
  EXPECT_EQ(retained->leg_frames[0].status, VarLegStatus::Ok);
  EXPECT_EQ(retained->n_tenor_extrapolated_legs, 1u);
}

} // namespace
