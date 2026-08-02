#include "atx/options/option_scenario_cube.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/detail/pricing_executor.hpp"
#include "atx/vol/surface_policy.hpp"

namespace atx::options::risk {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr double kNsPerYear = 365.25 * 86'400.0 * 1.0e9;

struct BaseContract {
  atx::vol::SurfaceRef surface{};
  double spot{0.0};
  double strike{0.0};
  double tenor{0.0};
  double sigma{0.0};
  double rate{0.0};
  double q_eff{0.0};
  double base_price{0.0};
  double multiplier{0.0};
  std::int64_t remaining_ns{0};
  std::size_t underlier_index{0};
  atx::vol::Side side{atx::vol::Side::Call};
  atx::vol::ExerciseStyle exercise_style{atx::vol::ExerciseStyle::American};
  atx::vol::AmericanMethod american_method{atx::vol::AmericanMethod::AndersenLake};
  atx::vol::AlOpts al_opts{};
};

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool populated(const atx::vol::ArchiveContentIdentity &identity) noexcept {
  return identity.file_size != 0U;
}

[[nodiscard]] bool valid_dynamics(OptionScenarioSurfaceDynamics dynamics) noexcept {
  return dynamics == OptionScenarioSurfaceDynamics::FrozenStickyStrike;
}

[[nodiscard]] bool valid_exercise_style(atx::vol::ExerciseStyle style) noexcept {
  return style == atx::vol::ExerciseStyle::American || style == atx::vol::ExerciseStyle::European;
}

constexpr std::uint8_t kKnownContractRoles =
    static_cast<std::uint8_t>(OptionScenarioContractRole::Candidate) |
    static_cast<std::uint8_t>(OptionScenarioContractRole::FilledPosition) |
    static_cast<std::uint8_t>(OptionScenarioContractRole::WorkingOrder) |
    static_cast<std::uint8_t>(OptionScenarioContractRole::PendingCancel);

[[nodiscard]] bool valid_roles(OptionScenarioContractRole roles) noexcept {
  const std::uint8_t mask = static_cast<std::uint8_t>(roles);
  return mask != 0U && (mask & static_cast<std::uint8_t>(~kKnownContractRoles)) == 0U;
}

[[nodiscard]] bool has_role(OptionScenarioContractRole roles,
                            OptionScenarioContractRole role) noexcept {
  return (static_cast<std::uint8_t>(roles) & static_cast<std::uint8_t>(role)) != 0U;
}

void normalize_zero(double &value) noexcept {
  if (value == 0.0) {
    value = 0.0;
  }
}

[[nodiscard]] Result<std::size_t> checked_mul(std::size_t left, std::size_t right) {
  if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
    return Err(ErrorCode::OutOfRange, "option scenario cube size multiplication overflow");
  }
  return Ok(left * right);
}

[[nodiscard]] Result<std::size_t> checked_add(std::size_t left, std::size_t right) {
  if (left > (std::numeric_limits<std::size_t>::max)() - right) {
    return Err(ErrorCode::OutOfRange, "option scenario cube size addition overflow");
  }
  return Ok(left + right);
}

[[nodiscard]] Result<std::int64_t> positive_difference(std::int64_t later, std::int64_t earlier) {
  if (earlier < 0 || later <= earlier) {
    return Err(ErrorCode::OutOfRange, "option scenario cube timestamp difference is invalid");
  }
  return Ok(later - earlier);
}

[[nodiscard]] Result<std::int64_t> nonnegative_difference(std::int64_t later,
                                                          std::int64_t earlier) {
  if (earlier < 0 || later < earlier) {
    return Err(ErrorCode::OutOfRange, "option scenario cube timestamp difference is invalid");
  }
  return Ok(later - earlier);
}

[[nodiscard]] double intrinsic(double spot, double strike, atx::vol::Side side) noexcept {
  return side == atx::vol::Side::Call ? (std::max)(spot - strike, 0.0)
                                      : (std::max)(strike - spot, 0.0);
}

class DigestWriter {
public:
  [[nodiscard]] Status u64(std::uint64_t value) {
    std::array<std::byte, sizeof(value)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::byte>(value & 0xFFU);
      value >>= 8U;
    }
    return digest_.update(bytes);
  }

  [[nodiscard]] Status i64(std::int64_t value) { return u64(std::bit_cast<std::uint64_t>(value)); }

  [[nodiscard]] Status f64(double value) { return u64(std::bit_cast<std::uint64_t>(value)); }

  [[nodiscard]] Status identity(const atx::vol::ArchiveContentIdentity &value) {
    ATX_TRY_VOID(u64(value.file_size));
    ATX_TRY_VOID(u64(value.created_ts_ns));
    ATX_TRY_VOID(u64(value.header_crc32c));
    return u64(value.metadata_crc32c);
  }

  [[nodiscard]] Status digest(const OptionRiskContentDigest &value) {
    return digest_.update(std::as_bytes(std::span{value.bytes.data(), value.bytes.size()}));
  }

  [[nodiscard]] Result<OptionRiskContentDigest> finish() {
    ATX_TRY(auto bytes, digest_.finalize());
    OptionRiskContentDigest result{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      result.bytes[index] = std::to_integer<std::uint8_t>(bytes[index]);
    }
    return Ok(result);
  }

private:
  atx::core::Sha256 digest_{};
};

[[nodiscard]] Result<OptionRiskContentDigest>
manifest_digest(const OptionScenarioManifest &manifest) {
  DigestWriter writer;
  ATX_TRY_VOID(writer.u64(kOptionScenarioCubeSchemaVersion));
  ATX_TRY_VOID(writer.u64(static_cast<std::uint64_t>(manifest.surface_dynamics)));
  ATX_TRY_VOID(writer.f64(manifest.minimum_implied_vol));
  ATX_TRY_VOID(writer.i64(manifest.observed_ts_ns));
  ATX_TRY_VOID(writer.i64(manifest.available_ts_ns));
  ATX_TRY_VOID(writer.i64(manifest.effective_ts_ns));
  ATX_TRY_VOID(writer.identity(manifest.artifact_identity));
  ATX_TRY_VOID(writer.u64(manifest.scenarios.size()));
  for (const OptionScenarioDefinition &scenario : manifest.scenarios) {
    ATX_TRY_VOID(writer.u64(scenario.scenario_id));
    ATX_TRY_VOID(writer.i64(scenario.horizon_ns));
    ATX_TRY_VOID(writer.f64(scenario.rate_shift));
  }
  ATX_TRY_VOID(writer.u64(manifest.underlier_shocks.size()));
  for (const OptionScenarioUnderlierShock &shock : manifest.underlier_shocks) {
    ATX_TRY_VOID(writer.u64(shock.scenario_id));
    ATX_TRY_VOID(writer.u64(shock.underlier_uid));
    ATX_TRY_VOID(writer.f64(shock.spot_return));
    ATX_TRY_VOID(writer.f64(shock.vol_level_shift));
  }
  return writer.finish();
}

[[nodiscard]] Status digest_contract_row(DigestWriter &writer, const OptionRiskContractRow &row) {
  ATX_TRY_VOID(writer.i64(row.decision_ts_ns));
  ATX_TRY_VOID(writer.u64(row.contract_id));
  ATX_TRY_VOID(writer.u64(row.engine_id.id));
  ATX_TRY_VOID(writer.u64(row.underlier_uid));
  ATX_TRY_VOID(writer.i64(row.observed_ts_ns));
  ATX_TRY_VOID(writer.i64(row.available_ts_ns));
  ATX_TRY_VOID(writer.i64(row.market_observed_ts_ns));
  ATX_TRY_VOID(writer.i64(row.market_available_ts_ns));
  ATX_TRY_VOID(writer.i64(row.definition_available_ts_ns));
  ATX_TRY_VOID(writer.i64(row.expiry_ts_ns));
  ATX_TRY_VOID(writer.f64(row.strike));
  ATX_TRY_VOID(writer.u64(static_cast<std::uint64_t>(row.side)));
  ATX_TRY_VOID(writer.u64(static_cast<std::uint64_t>(row.exercise_style)));
  ATX_TRY_VOID(writer.f64(row.multiplier));
  ATX_TRY_VOID(writer.u64(row.standard_deliverable ? 1U : 0U));
  ATX_TRY_VOID(writer.identity(row.definition_source_identity));
  ATX_TRY_VOID(writer.f64(row.spot_delta_cash_per_contract));
  ATX_TRY_VOID(writer.f64(row.spot_gamma_cash_per_contract));
  ATX_TRY_VOID(writer.f64(row.vega_cash_per_vol_point_per_contract));
  ATX_TRY_VOID(writer.f64(row.theta_cash_per_day_per_contract));
  ATX_TRY_VOID(writer.f64(row.vanna_cash_per_return_vol_point_per_contract));
  ATX_TRY_VOID(writer.f64(row.volga_cash_per_vol_point_squared_per_contract));
  ATX_TRY_VOID(writer.f64(row.premium_cash_notional_per_contract));
  ATX_TRY_VOID(writer.u64(static_cast<std::uint64_t>(row.status)));
  ATX_TRY_VOID(writer.identity(row.risk_source_identity));
  ATX_TRY_VOID(writer.identity(row.surface_source_identity));
  return writer.identity(row.market_source_identity);
}

[[nodiscard]] Result<OptionRiskContentDigest>
risk_digest(std::span<const OptionRiskContractRow> rows,
            std::span<const OptionScenarioActiveContract> active_contracts,
            std::span<const OptionRiskScenario> scenarios, std::span<const double> pnl,
            std::span<const OptionRiskGeneratedPnlLineage> pnl_lineage,
            const OptionRiskContentDigest &scenario_digest, const OptionScenarioCubeBuildSpec &spec,
            const atx::vol::ArchiveContentIdentity &surface_identity,
            std::size_t vol_floor_hit_count) {
  DigestWriter writer;
  ATX_TRY_VOID(writer.u64(kOptionScenarioCubeSchemaVersion));
  ATX_TRY_VOID(writer.u64(kOptionScenarioCubePricerModelVersion));
  ATX_TRY_VOID(writer.u64(kOptionScenarioCubeGreekConventionVersion));
  ATX_TRY_VOID(writer.i64(spec.surface_available_ts_ns));
  ATX_TRY_VOID(writer.i64(spec.limits.max_surface_age_ns));
  ATX_TRY_VOID(writer.i64(spec.limits.max_market_age_ns));
  ATX_TRY_VOID(writer.u64(spec.limits.max_vol_floor_hits));
  ATX_TRY_VOID(writer.u64(vol_floor_hit_count));
  ATX_TRY_VOID(writer.i64(spec.active_set_attestation.observed_ts_ns));
  ATX_TRY_VOID(writer.i64(spec.active_set_attestation.available_ts_ns));
  ATX_TRY_VOID(writer.identity(spec.active_set_attestation.source_identity));
  ATX_TRY_VOID(writer.u64(spec.active_set_attestation.candidate_contract_count));
  ATX_TRY_VOID(writer.u64(spec.active_set_attestation.filled_position_contract_count));
  ATX_TRY_VOID(writer.u64(spec.active_set_attestation.working_order_contract_count));
  ATX_TRY_VOID(writer.u64(spec.active_set_attestation.pending_cancel_contract_count));
  ATX_TRY_VOID(writer.identity(surface_identity));
  ATX_TRY_VOID(writer.digest(scenario_digest));
  ATX_TRY_VOID(writer.u64(rows.size()));
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    ATX_TRY_VOID(writer.u64(static_cast<std::uint64_t>(active_contracts[index].role_mask)));
    ATX_TRY_VOID(writer.u64(static_cast<std::uint64_t>(active_contracts[index].audit.status)));
    ATX_TRY_VOID(digest_contract_row(writer, rows[index]));
  }
  ATX_TRY_VOID(writer.u64(pnl.size()));
  std::size_t cell = 0U;
  for (std::size_t scenario_index = 0U; scenario_index < scenarios.size(); ++scenario_index) {
    const OptionRiskGeneratedPnlLineage &lineage = pnl_lineage[scenario_index];
    for (const OptionRiskContractRow &row : rows) {
      ATX_TRY_VOID(writer.i64(row.decision_ts_ns));
      ATX_TRY_VOID(writer.u64(row.contract_id));
      ATX_TRY_VOID(writer.u64(scenarios[scenario_index].scenario_id));
      ATX_TRY_VOID(writer.i64(lineage.observed_ts_ns));
      ATX_TRY_VOID(writer.i64(lineage.available_ts_ns));
      ATX_TRY_VOID(writer.f64(pnl[cell++]));
      ATX_TRY_VOID(writer.identity(lineage.source_identity));
    }
  }
  return writer.finish();
}

[[nodiscard]] Result<std::vector<std::uint32_t>>
active_underliers(std::span<const OptionScenarioActiveContract> active_contracts,
                  std::size_t max_underliers) {
  std::vector<std::uint32_t> result;
  result.reserve((std::min)(active_contracts.size(), max_underliers));
  for (const OptionScenarioActiveContract &active : active_contracts) {
    const research::OptionInstrument &instrument = active.instrument;
    if (instrument.underlier_uid == 0U || !valid_exercise_style(instrument.exercise_style)) {
      return Err(ErrorCode::InvalidArgument, "option scenario cube contract definition is invalid");
    }
    result.push_back(instrument.underlier_uid);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  if (result.empty() || result.size() > max_underliers) {
    return Err(ErrorCode::OutOfRange, "option scenario cube active underlier limit is exceeded");
  }
  return Ok(std::move(result));
}

[[nodiscard]] Result<std::int64_t>
validate_active_catalog(std::span<const OptionScenarioActiveContract> active_contracts,
                        const OptionScenarioCubeBuildSpec &spec) {
  if (active_contracts.empty() || active_contracts.size() > spec.limits.max_contracts ||
      spec.limits.max_market_age_ns <= 0) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario cube requires a bounded nonempty active catalog");
  }
  const std::int64_t decision_ts_ns = active_contracts.front().audit.decision_ts_ns;
  const OptionScenarioActiveSetAttestation &attestation = spec.active_set_attestation;
  if (decision_ts_ns <= 0 || attestation.observed_ts_ns <= 0 ||
      attestation.observed_ts_ns > attestation.available_ts_ns ||
      attestation.available_ts_ns > decision_ts_ns || !populated(attestation.source_identity)) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario active-set attestation is not point-in-time");
  }
  std::uint64_t previous_contract_id = 0U;
  std::size_t candidate_count = 0U;
  std::size_t filled_count = 0U;
  std::size_t working_count = 0U;
  std::size_t pending_cancel_count = 0U;
  for (const OptionScenarioActiveContract &active : active_contracts) {
    const research::OptionInstrument &instrument = active.instrument;
    const research::OptionDecisionAudit &audit = active.audit;
    if (instrument.contract_id == 0U || instrument.contract_id <= previous_contract_id ||
        audit.contract_id != instrument.contract_id || audit.decision_ts_ns != decision_ts_ns ||
        audit.definition_available_ts_ns <= 0 ||
        audit.definition_available_ts_ns > audit.quote_event_ts_ns ||
        audit.quote_event_ts_ns <= 0 || audit.quote_event_ts_ns > audit.quote_available_ts_ns ||
        audit.quote_available_ts_ns > decision_ts_ns ||
        audit.status != research::OptionPanelStatus::Tradable ||
        !populated(audit.definition_source_identity) ||
        !populated(audit.execution_source_identity) || !finite(active.market_mark) ||
        active.market_mark <= 0.0 || !valid_roles(active.role_mask) ||
        !instrument.standard_deliverable) {
      return Err(ErrorCode::InvalidArgument,
                 "option scenario active catalog is invalid or noncanonical");
    }
    ATX_TRY(std::int64_t market_age,
            nonnegative_difference(decision_ts_ns, audit.quote_event_ts_ns));
    if (market_age > spec.limits.max_market_age_ns) {
      return Err(ErrorCode::InvalidArgument, "option scenario active market mark is stale");
    }
    candidate_count += has_role(active.role_mask, OptionScenarioContractRole::Candidate) ? 1U : 0U;
    filled_count +=
        has_role(active.role_mask, OptionScenarioContractRole::FilledPosition) ? 1U : 0U;
    working_count += has_role(active.role_mask, OptionScenarioContractRole::WorkingOrder) ? 1U : 0U;
    pending_cancel_count +=
        has_role(active.role_mask, OptionScenarioContractRole::PendingCancel) ? 1U : 0U;
    previous_contract_id = instrument.contract_id;
  }
  if (candidate_count != attestation.candidate_contract_count ||
      filled_count != attestation.filled_position_contract_count ||
      working_count != attestation.working_order_contract_count ||
      pending_cancel_count != attestation.pending_cancel_contract_count) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario active catalog does not match its lifecycle attestation");
  }
  return Ok(decision_ts_ns);
}

[[nodiscard]] Status canonicalize_manifest(OptionScenarioManifest &manifest,
                                           std::span<const std::uint32_t> underliers,
                                           std::int64_t decision_ts_ns,
                                           const OptionScenarioCubeLimits &limits) {
  if (!valid_dynamics(manifest.surface_dynamics) || !finite(manifest.minimum_implied_vol) ||
      manifest.minimum_implied_vol <= 0.0 || !populated(manifest.artifact_identity) ||
      manifest.observed_ts_ns <= 0 || manifest.available_ts_ns < manifest.observed_ts_ns ||
      manifest.available_ts_ns > decision_ts_ns || manifest.effective_ts_ns <= 0 ||
      manifest.effective_ts_ns > decision_ts_ns || manifest.scenarios.empty() ||
      manifest.scenarios.size() > limits.max_scenarios) {
    return Err(ErrorCode::InvalidArgument, "option scenario manifest is invalid");
  }
  for (OptionScenarioDefinition &scenario : manifest.scenarios) {
    if (scenario.scenario_id == 0U || scenario.horizon_ns < 0 || !finite(scenario.rate_shift)) {
      return Err(ErrorCode::InvalidArgument, "option scenario definition is invalid");
    }
    normalize_zero(scenario.rate_shift);
  }
  std::sort(
      manifest.scenarios.begin(), manifest.scenarios.end(),
      [](const OptionScenarioDefinition &left, const OptionScenarioDefinition &right) noexcept {
        return left.scenario_id < right.scenario_id;
      });
  if (std::adjacent_find(
          manifest.scenarios.begin(), manifest.scenarios.end(),
          [](const OptionScenarioDefinition &left, const OptionScenarioDefinition &right) noexcept {
            return left.scenario_id == right.scenario_id;
          }) != manifest.scenarios.end()) {
    return Err(ErrorCode::AlreadyExists,
               "option scenario manifest contains a duplicate scenario id");
  }

  ATX_TRY(std::size_t expected_shocks, checked_mul(manifest.scenarios.size(), underliers.size()));
  if (manifest.underlier_shocks.size() != expected_shocks) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario manifest does not cover every active underlier");
  }
  for (OptionScenarioUnderlierShock &shock : manifest.underlier_shocks) {
    if (shock.scenario_id == 0U || shock.underlier_uid == 0U || !finite(shock.spot_return) ||
        shock.spot_return <= -1.0 || !finite(shock.vol_level_shift)) {
      return Err(ErrorCode::InvalidArgument, "option scenario underlier shock is invalid");
    }
    normalize_zero(shock.spot_return);
    normalize_zero(shock.vol_level_shift);
  }
  std::sort(manifest.underlier_shocks.begin(), manifest.underlier_shocks.end(),
            [](const OptionScenarioUnderlierShock &left,
               const OptionScenarioUnderlierShock &right) noexcept {
              return std::tie(left.scenario_id, left.underlier_uid) <
                     std::tie(right.scenario_id, right.underlier_uid);
            });
  std::size_t index = 0U;
  for (const OptionScenarioDefinition &scenario : manifest.scenarios) {
    for (std::uint32_t underlier_uid : underliers) {
      const OptionScenarioUnderlierShock &shock = manifest.underlier_shocks[index++];
      if (shock.scenario_id != scenario.scenario_id || shock.underlier_uid != underlier_uid) {
        return Err(ErrorCode::InvalidArgument,
                   "option scenario manifest has missing, duplicate, or unknown coverage");
      }
    }
  }
  return Ok();
}

[[nodiscard]] Status validate_snapshot_clocks(const atx::vol::MarketSnapshot &snapshot,
                                              std::int64_t decision_ts_ns,
                                              const OptionScenarioCubeBuildSpec &spec) {
  if (spec.limits.max_surface_age_ns <= 0 || snapshot.ts_ns() <= 0 ||
      spec.surface_available_ts_ns < snapshot.ts_ns() ||
      spec.surface_available_ts_ns > decision_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario cube surface clocks are not point-in-time");
  }
  ATX_TRY(std::int64_t age, nonnegative_difference(decision_ts_ns, snapshot.ts_ns()));
  if (age > spec.limits.max_surface_age_ns) {
    return Err(ErrorCode::InvalidArgument, "option scenario cube surface is stale");
  }
  if (!populated(snapshot.source_identity())) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario cube surface archive identity is unavailable");
  }
  return Ok();
}

[[nodiscard]] Status validate_surface(const atx::vol::MarketSnapshot &snapshot, std::uint32_t uid) {
  const atx::vol::SurfaceRef surface = snapshot.find(uid);
  const atx::vol::SurfaceProvenance *provenance = snapshot.provenance(uid);
  if (surface == nullptr || provenance == nullptr || provenance->legacy_format ||
      provenance->purpose != atx::vol::SurfacePurpose::Risk ||
      provenance->state != atx::vol::SurfaceState::Healthy || provenance->served_generation == 0U ||
      provenance->source_generation == 0U || provenance->validation.validation_id == 0U ||
      surface->n_slices() == 0U || !provenance->validation.admitted()) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario cube requires a healthy admitted risk surface");
  }
  return Ok();
}

struct CashGreeks {
  double delta{0.0};
  double gamma{0.0};
  double vega_01{0.0};
  double theta_day{0.0};
  double vanna{0.0};
  double volga{0.0};
  double price{0.0};
};

[[nodiscard]] Result<CashGreeks> american_cash_greeks(const BaseContract &base) {
  ATX_TRY(atx::vol::AmericanGreeks greeks,
          base.surface->greeks(base.strike, base.tenor, base.side,
                               atx::vol::QueryExecution::ColdReference));
  CashGreeks result;
  result.delta = greeks.delta * base.spot * base.multiplier;
  result.gamma = greeks.gamma * base.spot * base.spot * base.multiplier;
  result.vega_01 = greeks.vega * 0.01 * base.multiplier;
  result.theta_day = greeks.theta / 365.25 * base.multiplier;
  result.vanna = greeks.vanna * base.spot * 0.01 * base.multiplier;
  result.volga = greeks.volga * 0.0001 * base.multiplier;
  result.price = greeks.price;
  return Ok(result);
}

[[nodiscard]] Result<CashGreeks> european_cash_greeks(const BaseContract &base) {
  const double forward = base.surface->forward_at(base.tenor);
  const double discount = std::exp(-base.rate * base.tenor);
  if (!finite(forward) || forward <= 0.0 || !finite(discount) || discount <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "option scenario cube European forward or discount is invalid");
  }
  const atx::vol::Black76Greeks solved = atx::vol::black76_greeks(
      forward, base.strike, base.tenor, base.sigma, base.rate, discount, base.side);
  const double forward_per_spot = forward / base.spot;
  const double carry_rate = base.rate - base.q_eff;
  CashGreeks result;
  result.delta = solved.greeks.delta * forward_per_spot * base.spot * base.multiplier;
  result.gamma = solved.greeks.gamma * forward_per_spot * forward_per_spot * base.spot * base.spot *
                 base.multiplier;
  result.vega_01 = solved.greeks.vega * 0.01 * base.multiplier;
  const double spot_theta = solved.greeks.theta - carry_rate * forward * solved.greeks.delta;
  result.theta_day = spot_theta / 365.25 * base.multiplier;
  result.vanna = solved.greeks.vanna * forward_per_spot * base.spot * 0.01 * base.multiplier;
  result.volga = solved.greeks.volga * 0.0001 * base.multiplier;
  result.price = solved.price;
  return Ok(result);
}

[[nodiscard]] Result<BaseContract> resolve_base(const research::OptionInstrument &instrument,
                                                const atx::vol::MarketSnapshot &snapshot,
                                                std::int64_t decision_ts_ns,
                                                std::size_t underlier_index) {
  ATX_TRY(std::int64_t remaining_ns, positive_difference(instrument.expiry_ts_ns, decision_ts_ns));
  const double tenor = static_cast<double>(remaining_ns) / kNsPerYear;
  const atx::vol::SurfaceRef surface = snapshot.find(instrument.underlier_uid);
  if (surface == nullptr || !(tenor > 0.0) || !finite(tenor)) {
    return Err(ErrorCode::InvalidArgument, "option scenario cube cannot resolve the base contract");
  }
  const atx::vol::PricedSurface::ResolvedSurfacePoint point =
      surface->resolve(instrument.strike, tenor);
  const atx::vol::PricingContext &pricing = surface->pricing();
  if (!point.valid || !finite(point.sigma) || point.sigma <= 0.0 || !finite(point.rate) ||
      !finite(point.q_eff) || !finite(pricing.S) || pricing.S <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "option scenario cube base surface point is invalid");
  }
  return Ok(BaseContract{surface, pricing.S, instrument.strike, tenor, point.sigma, point.rate,
                         point.q_eff, 0.0, instrument.multiplier, remaining_ns, underlier_index,
                         instrument.side, instrument.exercise_style, pricing.method,
                         pricing.al_opts});
}

[[nodiscard]] Result<OptionRiskContractRow> make_contract_row(
    const research::OptionInstrument &instrument, const research::OptionDecisionAudit &audit,
    const atx::vol::ArchiveContentIdentity &surface_identity, std::int64_t observed_ts_ns,
    std::int64_t available_ts_ns, double market_mark, BaseContract &base) {
  ATX_TRY(CashGreeks greeks, base.exercise_style == atx::vol::ExerciseStyle::American
                                 ? american_cash_greeks(base)
                                 : european_cash_greeks(base));
  base.base_price = greeks.price;
  const std::array values{greeks.delta, greeks.gamma, greeks.vega_01, greeks.theta_day,
                          greeks.vanna, greeks.volga, greeks.price};
  if (!std::all_of(values.begin(), values.end(), finite) || greeks.price < 0.0) {
    return Err(ErrorCode::InvalidArgument, "option scenario cube base price or Greeks are invalid");
  }
  const double premium = std::abs(market_mark) * base.multiplier;
  if (!finite(premium) || premium <= 0.0) {
    return Err(ErrorCode::OutOfRange, "option scenario cube premium cash notional is invalid");
  }
  OptionRiskContractRow row;
  row.decision_ts_ns = audit.decision_ts_ns;
  row.contract_id = instrument.contract_id;
  row.engine_id = instrument.engine_id;
  row.underlier_uid = instrument.underlier_uid;
  row.observed_ts_ns = observed_ts_ns;
  row.available_ts_ns = available_ts_ns;
  row.market_observed_ts_ns = audit.quote_event_ts_ns;
  row.market_available_ts_ns = audit.quote_available_ts_ns;
  row.definition_available_ts_ns = audit.definition_available_ts_ns;
  row.expiry_ts_ns = instrument.expiry_ts_ns;
  row.strike = instrument.strike;
  row.side = instrument.side;
  row.exercise_style = instrument.exercise_style;
  row.multiplier = instrument.multiplier;
  row.standard_deliverable = instrument.standard_deliverable;
  row.definition_source_identity = audit.definition_source_identity;
  row.spot_delta_cash_per_contract = greeks.delta;
  row.spot_gamma_cash_per_contract = greeks.gamma;
  row.vega_cash_per_vol_point_per_contract = greeks.vega_01;
  row.theta_cash_per_day_per_contract = greeks.theta_day;
  row.vanna_cash_per_return_vol_point_per_contract = greeks.vanna;
  row.volga_cash_per_vol_point_squared_per_contract = greeks.volga;
  row.premium_cash_notional_per_contract = premium;
  row.status = OptionRiskRowStatus::Ok;
  row.risk_source_identity = surface_identity;
  row.surface_source_identity = surface_identity;
  row.market_source_identity = audit.execution_source_identity;
  return Ok(row);
}

[[nodiscard]] double shocked_price(const BaseContract &base,
                                   const OptionScenarioDefinition &scenario,
                                   const OptionScenarioUnderlierShock &shock,
                                   double minimum_implied_vol, bool &vol_floor_hit) noexcept {
  const double shocked_spot = base.spot * (1.0 + shock.spot_return);
  const std::int64_t shocked_remaining_ns =
      scenario.horizon_ns >= base.remaining_ns ? 0 : base.remaining_ns - scenario.horizon_ns;
  const double tenor = static_cast<double>(shocked_remaining_ns) / kNsPerYear;
  const double raw_sigma = base.sigma + shock.vol_level_shift;
  vol_floor_hit = shocked_remaining_ns != 0 && raw_sigma < minimum_implied_vol;
  const double sigma = (std::max)(minimum_implied_vol, raw_sigma);
  const double rate = base.rate + scenario.rate_shift;
  if (!finite(shocked_spot) || shocked_spot <= 0.0 || !finite(tenor) || !finite(sigma) ||
      sigma <= 0.0 || !finite(rate)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (shocked_remaining_ns == 0) {
    return intrinsic(shocked_spot, base.strike, base.side);
  }
  if (base.exercise_style == atx::vol::ExerciseStyle::European) {
    const double forward = shocked_spot * std::exp((rate - base.q_eff) * tenor);
    const double discount = std::exp(-rate * tenor);
    if (!finite(forward) || forward <= 0.0 || !finite(discount) || discount <= 0.0) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    return atx::vol::black76_price(forward, base.strike, tenor, sigma, discount, base.side);
  }
  const Result<double> price =
      atx::vol::american_price(shocked_spot, base.strike, tenor, sigma, rate, base.q_eff, base.side,
                               base.american_method, std::optional<atx::vol::AlOpts>{base.al_opts});
  return price && finite(*price) ? *price : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] Result<std::size_t>
required_workspace_bytes(std::size_t contracts, std::size_t underliers, std::size_t scenarios) {
  ATX_TRY(std::size_t cells, checked_mul(contracts, scenarios));
  ATX_TRY(std::size_t shocks, checked_mul(underliers, scenarios));
  std::size_t bytes = 0U;
  const auto add = [&bytes](std::size_t count, std::size_t width) -> Status {
    ATX_TRY(std::size_t block, checked_mul(count, width));
    ATX_TRY(bytes, checked_add(bytes, block));
    return Ok();
  };
  ATX_TRY_VOID(add(contracts, sizeof(BaseContract)));
  ATX_TRY_VOID(add(contracts, sizeof(OptionRiskContractRow)));
  ATX_TRY_VOID(add(scenarios, sizeof(OptionRiskScenario)));
  ATX_TRY_VOID(add(cells, sizeof(double)));
  ATX_TRY_VOID(add(scenarios, sizeof(OptionRiskGeneratedPnlLineage)));
  ATX_TRY_VOID(add(underliers, sizeof(std::uint32_t)));
  ATX_TRY_VOID(add(scenarios, sizeof(OptionScenarioDefinition)));
  ATX_TRY_VOID(add(shocks, sizeof(OptionScenarioUnderlierShock)));
  ATX_TRY_VOID(add(1U, sizeof(std::int64_t)));
  ATX_TRY_VOID(add(contracts, sizeof(std::uint64_t)));
  ATX_TRY_VOID(add(contracts, sizeof(atx::engine::InstrumentId)));
  ATX_TRY_VOID(add(contracts, sizeof(std::uint32_t)));
  ATX_TRY_VOID(add(scenarios, sizeof(std::uint64_t)));
  return Ok(bytes);
}

} // namespace

Result<OptionScenarioCube>
compile_option_scenario_cube(std::span<const OptionScenarioActiveContract> active_contracts,
                             const atx::vol::MarketSnapshot &snapshot,
                             OptionScenarioManifest manifest,
                             const OptionScenarioCubeBuildSpec &spec) {
  const std::size_t contract_count = active_contracts.size();
  ATX_TRY(std::int64_t decision_ts_ns, validate_active_catalog(active_contracts, spec));
  ATX_TRY_VOID(validate_snapshot_clocks(snapshot, decision_ts_ns, spec));
  ATX_TRY(std::vector<std::uint32_t> underliers,
          active_underliers(active_contracts, spec.limits.max_underliers));
  ATX_TRY_VOID(canonicalize_manifest(manifest, underliers, decision_ts_ns, spec.limits));
  ATX_TRY(std::size_t scenario_cells, checked_mul(contract_count, manifest.scenarios.size()));
  if (scenario_cells > spec.limits.max_scenario_cells) {
    return Err(ErrorCode::OutOfRange, "option scenario cube cell limit is exceeded");
  }
  ATX_TRY(std::size_t workspace,
          required_workspace_bytes(contract_count, underliers.size(), manifest.scenarios.size()));
  if (workspace > spec.limits.max_workspace_bytes) {
    return Err(ErrorCode::OutOfRange, "option scenario cube workspace limit is exceeded");
  }
  for (std::uint32_t uid : underliers) {
    ATX_TRY_VOID(validate_surface(snapshot, uid));
  }

  try {
    std::vector<BaseContract> bases(contract_count);
    std::vector<OptionRiskContractRow> contract_rows(contract_count);
    std::size_t american_count = 0U;
    std::size_t european_count = 0U;
    const atx::vol::ArchiveContentIdentity surface_identity = snapshot.source_identity();
    for (std::size_t index = 0; index < contract_count; ++index) {
      const research::OptionInstrument &instrument = active_contracts[index].instrument;
      const auto underlier =
          std::lower_bound(underliers.begin(), underliers.end(), instrument.underlier_uid);
      if (underlier == underliers.end() || *underlier != instrument.underlier_uid) {
        return Err(ErrorCode::Internal, "option scenario cube active underlier lookup failed");
      }
      ATX_TRY(bases[index], resolve_base(instrument, snapshot, decision_ts_ns,
                                         static_cast<std::size_t>(underlier - underliers.begin())));
      if (instrument.exercise_style == atx::vol::ExerciseStyle::American) {
        ++american_count;
      } else {
        ++european_count;
      }
    }
    std::atomic<bool> base_pricing_failed{false};
    std::atomic<std::size_t> first_base_failed{(std::numeric_limits<std::size_t>::max)()};
    atx::vol::pricing_executor().run_blocks(
        contract_count, spec.n_threads, [&](std::size_t index) noexcept {
          const OptionScenarioActiveContract &active = active_contracts[index];
          auto row =
              make_contract_row(active.instrument, active.audit, surface_identity, snapshot.ts_ns(),
                                spec.surface_available_ts_ns, active.market_mark, bases[index]);
          if (!row) {
            base_pricing_failed.store(true, std::memory_order_relaxed);
            std::size_t current = first_base_failed.load(std::memory_order_relaxed);
            while (index < current &&
                   !first_base_failed.compare_exchange_weak(
                       current, index, std::memory_order_relaxed, std::memory_order_relaxed)) {
            }
            return;
          }
          contract_rows[index] = std::move(*row);
        });
    if (base_pricing_failed.load(std::memory_order_relaxed)) {
      const std::size_t index = first_base_failed.load(std::memory_order_relaxed);
      return Err(ErrorCode::Unavailable,
                 "option scenario cube base pricing failed for contract " +
                     std::to_string(active_contracts[index].instrument.contract_id));
    }

    std::vector<OptionRiskScenario> risk_scenarios;
    risk_scenarios.reserve(manifest.scenarios.size());
    for (const OptionScenarioDefinition &scenario : manifest.scenarios) {
      risk_scenarios.push_back(
          OptionRiskScenario{scenario.scenario_id, manifest.artifact_identity});
    }

    std::vector<double> pnl(scenario_cells, 0.0);
    std::vector<OptionRiskGeneratedPnlLineage> pnl_lineage;
    pnl_lineage.reserve(manifest.scenarios.size());
    const std::int64_t pnl_observed_ts_ns = (std::max)(snapshot.ts_ns(), manifest.observed_ts_ns);
    const std::int64_t pnl_available_ts_ns =
        (std::max)(spec.surface_available_ts_ns, manifest.available_ts_ns);
    for (const OptionScenarioDefinition &scenario : manifest.scenarios) {
      pnl_lineage.push_back(OptionRiskGeneratedPnlLineage{decision_ts_ns, scenario.scenario_id,
                                                          pnl_observed_ts_ns, pnl_available_ts_ns,
                                                          manifest.artifact_identity});
    }
    std::atomic<bool> pricing_failed{false};
    std::atomic<std::size_t> first_failed_cell{(std::numeric_limits<std::size_t>::max)()};
    std::atomic<std::size_t> vol_floor_hits{0U};
    std::atomic<std::size_t> first_vol_floor_cell{(std::numeric_limits<std::size_t>::max)()};
    atx::vol::pricing_executor().run_blocks(
        scenario_cells, spec.n_threads, [&](std::size_t cell) noexcept {
          const std::size_t scenario_index = cell / contract_count;
          const std::size_t contract_index = cell % contract_count;
          const OptionScenarioDefinition &scenario = manifest.scenarios[scenario_index];
          const BaseContract &base = bases[contract_index];
          const OptionScenarioUnderlierShock &shock =
              manifest.underlier_shocks[scenario_index * underliers.size() + base.underlier_index];
          bool vol_floor_hit = false;
          const double price =
              shocked_price(base, scenario, shock, manifest.minimum_implied_vol, vol_floor_hit);
          if (vol_floor_hit) {
            vol_floor_hits.fetch_add(1U, std::memory_order_relaxed);
            std::size_t current = first_vol_floor_cell.load(std::memory_order_relaxed);
            while (cell < current &&
                   !first_vol_floor_cell.compare_exchange_weak(
                       current, cell, std::memory_order_relaxed, std::memory_order_relaxed)) {
            }
          }
          const double cell_pnl = (price - base.base_price) * base.multiplier;
          if (!finite(price) || !finite(cell_pnl)) {
            pricing_failed.store(true, std::memory_order_relaxed);
            std::size_t current = first_failed_cell.load(std::memory_order_relaxed);
            while (cell < current &&
                   !first_failed_cell.compare_exchange_weak(
                       current, cell, std::memory_order_relaxed, std::memory_order_relaxed)) {
            }
            return;
          }
          pnl[cell] = cell_pnl;
        });
    if (pricing_failed.load(std::memory_order_relaxed)) {
      const std::size_t cell = first_failed_cell.load(std::memory_order_relaxed);
      const std::size_t scenario_index = cell / contract_count;
      const std::size_t contract_index = cell % contract_count;
      return Err(ErrorCode::Unavailable,
                 "option scenario cube full repricing failed for scenario " +
                     std::to_string(manifest.scenarios[scenario_index].scenario_id) + " contract " +
                     std::to_string(active_contracts[contract_index].instrument.contract_id));
    }
    const std::size_t vol_floor_hit_count = vol_floor_hits.load(std::memory_order_relaxed);
    if (vol_floor_hit_count > spec.limits.max_vol_floor_hits) {
      const std::size_t cell = first_vol_floor_cell.load(std::memory_order_relaxed);
      const std::size_t scenario_index = cell / contract_count;
      const std::size_t contract_index = cell % contract_count;
      return Err(ErrorCode::InvalidArgument,
                 "option scenario cube volatility floor hits " +
                     std::to_string(vol_floor_hit_count) + " exceed allowance " +
                     std::to_string(spec.limits.max_vol_floor_hits) + " at scenario " +
                     std::to_string(manifest.scenarios[scenario_index].scenario_id) + " contract " +
                     std::to_string(active_contracts[contract_index].instrument.contract_id));
    }

    ATX_TRY(OptionRiskContentDigest scenario_sha, manifest_digest(manifest));
    ATX_TRY(OptionRiskContentDigest snapshot_sha,
            risk_digest(contract_rows, active_contracts, risk_scenarios, pnl, pnl_lineage,
                        scenario_sha, spec, surface_identity, vol_floor_hit_count));
    const OptionRiskPanelProvenance provenance{
        kOptionScenarioCubePricerModelVersion,
        kOptionScenarioCubeGreekConventionVersion,
        snapshot_sha,
        scenario_sha,
    };
    OptionRiskPanelLimits panel_limits;
    panel_limits.max_contract_rows = spec.limits.max_contracts;
    panel_limits.max_scenarios = spec.limits.max_scenarios;
    panel_limits.max_scenario_rows = spec.limits.max_scenario_cells;
    panel_limits.max_workspace_bytes = spec.limits.max_workspace_bytes;
    ATX_TRY(OptionRiskPanel risk_panel,
            OptionRiskPanel::create_generated_canonical(
                std::move(contract_rows), std::move(risk_scenarios), std::move(pnl),
                std::move(pnl_lineage), provenance, panel_limits));

    OptionScenarioCubeBuildReport report;
    report.contract_count = contract_count;
    report.underlier_count = underliers.size();
    report.scenario_count = manifest.scenarios.size();
    report.scenario_cell_count = scenario_cells;
    report.american_contract_count = american_count;
    report.european_contract_count = european_count;
    for (const OptionScenarioActiveContract &active : active_contracts) {
      report.candidate_contract_count +=
          has_role(active.role_mask, OptionScenarioContractRole::Candidate) ? 1U : 0U;
      report.filled_position_contract_count +=
          has_role(active.role_mask, OptionScenarioContractRole::FilledPosition) ? 1U : 0U;
      report.working_order_contract_count +=
          has_role(active.role_mask, OptionScenarioContractRole::WorkingOrder) ? 1U : 0U;
      report.pending_cancel_contract_count +=
          has_role(active.role_mask, OptionScenarioContractRole::PendingCancel) ? 1U : 0U;
    }
    report.vol_floor_hit_count = vol_floor_hit_count;
    report.scenario_manifest_digest = scenario_sha;
    report.risk_snapshot_digest = snapshot_sha;
    return Ok(OptionScenarioCube{std::move(risk_panel), std::move(manifest), report});
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "option scenario cube allocation failed");
  } catch (const std::length_error &) {
    return Err(ErrorCode::OutOfRange, "option scenario cube capacity exceeds vector limits");
  } catch (const std::system_error &) {
    return Err(ErrorCode::Unavailable, "option scenario cube pricing executor is unavailable");
  }
}

} // namespace atx::options::risk
