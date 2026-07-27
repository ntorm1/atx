#include "atx/options/option_pretrade_risk.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace atx::options::risk {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
constexpr std::size_t kScenarioMetricCount = 5U;

[[nodiscard]] Result<std::size_t> checked_add_size(std::size_t left, std::size_t right) {
  if (left > (std::numeric_limits<std::size_t>::max)() - right) {
    return Err(ErrorCode::OutOfRange, "option risk size addition overflow");
  }
  return Ok(left + right);
}

[[nodiscard]] Result<std::size_t> checked_mul_size(std::size_t left, std::size_t right) {
  if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
    return Err(ErrorCode::OutOfRange, "option risk size multiplication overflow");
  }
  return Ok(left * right);
}

[[nodiscard]] Result<std::int64_t> checked_add_i64(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > (std::numeric_limits<std::int64_t>::max)() - right) ||
      (right < 0 && left < (std::numeric_limits<std::int64_t>::min)() - right)) {
    return Err(ErrorCode::OutOfRange, "option risk contract quantity overflow");
  }
  return Ok(left + right);
}

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool populated(const atx::vol::ArchiveContentIdentity &identity) noexcept {
  return identity.file_size != 0U;
}

[[nodiscard]] bool populated(const OptionRiskContentDigest &digest) noexcept {
  return std::any_of(digest.bytes.begin(), digest.bytes.end(),
                     [](std::uint8_t byte) noexcept { return byte != 0U; });
}

[[nodiscard]] bool valid_status(OptionRiskRowStatus status) noexcept {
  switch (status) {
  case OptionRiskRowStatus::Ok:
  case OptionRiskRowStatus::MissingMarket:
  case OptionRiskRowStatus::StaleMarket:
  case OptionRiskRowStatus::ModelUnavailable:
  case OptionRiskRowStatus::UnsupportedContract:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_side(atx::vol::Side side) noexcept {
  switch (side) {
  case atx::vol::Side::Call:
  case atx::vol::Side::Put:
    return true;
  }
  return false;
}

[[nodiscard]] std::uint64_t fold_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
    hash ^= value & 0xFFU;
    hash *= kFnvPrime;
    value >>= 8U;
  }
  return hash;
}

[[nodiscard]] std::uint64_t fold_i64(std::uint64_t hash, std::int64_t value) noexcept {
  return fold_u64(hash, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t fold_double(std::uint64_t hash, double value) noexcept {
  return fold_u64(hash, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t
fold_identity(std::uint64_t hash, const atx::vol::ArchiveContentIdentity &identity) noexcept {
  hash = fold_u64(hash, identity.file_size);
  hash = fold_u64(hash, identity.created_ts_ns);
  hash = fold_u64(hash, identity.header_crc32c);
  return fold_u64(hash, identity.metadata_crc32c);
}

[[nodiscard]] std::uint64_t fold_digest(std::uint64_t hash,
                                        const OptionRiskContentDigest &digest) noexcept {
  for (std::uint8_t byte : digest.bytes) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
  return hash;
}

[[nodiscard]] Result<void> validate_contract_row(const OptionRiskContractRow &row) {
  if (row.decision_ts_ns == 0 || row.contract_id == 0U || row.engine_id.id == 0U ||
      row.underlier_uid == 0U || row.observed_ts_ns > row.available_ts_ns ||
      row.available_ts_ns > row.decision_ts_ns || row.expiry_ts_ns <= row.decision_ts_ns ||
      !finite(row.strike) || row.strike <= 0.0 || !valid_side(row.side) ||
      !finite(row.multiplier) || row.multiplier <= 0.0 ||
      !populated(row.definition_source_identity) || !valid_status(row.status) ||
      (row.status == OptionRiskRowStatus::Ok && !row.standard_deliverable) ||
      !finite(row.spot_delta_cash_per_contract) || !finite(row.spot_gamma_cash_per_contract) ||
      !finite(row.vega_cash_per_vol_point_per_contract) ||
      !finite(row.theta_cash_per_day_per_contract) ||
      !finite(row.vanna_cash_per_return_vol_point_per_contract) ||
      !finite(row.volga_cash_per_vol_point_squared_per_contract) ||
      !finite(row.premium_cash_notional_per_contract) ||
      row.premium_cash_notional_per_contract <= 0.0 || !populated(row.risk_source_identity) ||
      !populated(row.surface_source_identity)) {
    return Err(ErrorCode::InvalidArgument, "option risk contract row is invalid");
  }
  return Ok();
}

[[nodiscard]] Result<void> validate_scenario(const OptionRiskScenario &scenario) {
  if (scenario.scenario_id == 0U || !populated(scenario.source_identity)) {
    return Err(ErrorCode::InvalidArgument, "option risk scenario is invalid");
  }
  return Ok();
}

[[nodiscard]] Result<void> validate_pnl_row(const OptionRiskScenarioPnlRow &row) {
  if (row.decision_ts_ns == 0 || row.contract_id == 0U || row.scenario_id == 0U ||
      row.observed_ts_ns > row.available_ts_ns || row.available_ts_ns > row.decision_ts_ns ||
      !finite(row.pnl_per_long_contract) || !populated(row.source_identity)) {
    return Err(ErrorCode::InvalidArgument, "option risk scenario PnL row is invalid");
  }
  return Ok();
}

[[nodiscard]] Result<std::size_t> panel_required_bytes(std::size_t dates, std::size_t contracts,
                                                       std::size_t scenarios) {
  ATX_TRY(std::size_t contract_cells, checked_mul_size(dates, contracts));
  ATX_TRY(std::size_t scenario_cells, checked_mul_size(contract_cells, scenarios));
  std::size_t bytes = 0U;
  const auto add = [&bytes](std::size_t count, std::size_t width) -> Result<void> {
    ATX_TRY(std::size_t block, checked_mul_size(count, width));
    ATX_TRY(bytes, checked_add_size(bytes, block));
    return Ok();
  };
  ATX_TRY_VOID(add(dates, sizeof(std::int64_t)));
  ATX_TRY_VOID(add(contracts, sizeof(std::uint64_t)));
  ATX_TRY_VOID(add(contracts, sizeof(atx::engine::InstrumentId)));
  ATX_TRY_VOID(add(contracts, sizeof(std::uint32_t)));
  ATX_TRY_VOID(add(scenarios, sizeof(std::uint64_t)));
  ATX_TRY_VOID(add(contract_cells, sizeof(OptionRiskContractRow)));
  ATX_TRY_VOID(add(scenario_cells, sizeof(double)));
  return Ok(bytes);
}

[[nodiscard]] std::uint64_t hash_panel(std::span<const std::int64_t> dates,
                                       std::span<const std::uint64_t> contract_ids,
                                       std::span<const std::uint32_t> underlier_uids,
                                       std::span<const OptionRiskScenario> scenarios,
                                       std::span<const OptionRiskContractRow> rows,
                                       std::span<const OptionRiskScenarioPnlRow> pnl_rows,
                                       const OptionRiskPanelProvenance &provenance) noexcept {
  std::uint64_t hash = fold_u64(kFnvOffset, kOptionPreTradeRiskModelVersion);
  hash = fold_u64(hash, kOptionPreTradeRiskOrderingVersion);
  hash = fold_u64(hash, provenance.pricer_model_version);
  hash = fold_u64(hash, provenance.greek_convention_version);
  hash = fold_digest(hash, provenance.risk_snapshot_digest);
  hash = fold_digest(hash, provenance.scenario_manifest_digest);
  hash = fold_u64(hash, dates.size());
  hash = fold_u64(hash, contract_ids.size());
  hash = fold_u64(hash, scenarios.size());
  for (std::int64_t date : dates) {
    hash = fold_i64(hash, date);
  }
  for (std::size_t index = 0; index < contract_ids.size(); ++index) {
    hash = fold_u64(hash, contract_ids[index]);
    hash = fold_u64(hash, underlier_uids[index]);
  }
  for (const OptionRiskScenario &scenario : scenarios) {
    hash = fold_u64(hash, scenario.scenario_id);
    hash = fold_identity(hash, scenario.source_identity);
  }
  for (const OptionRiskContractRow &row : rows) {
    hash = fold_i64(hash, row.decision_ts_ns);
    hash = fold_u64(hash, row.contract_id);
    hash = fold_u64(hash, row.engine_id.id);
    hash = fold_u64(hash, row.underlier_uid);
    hash = fold_i64(hash, row.observed_ts_ns);
    hash = fold_i64(hash, row.available_ts_ns);
    hash = fold_i64(hash, row.expiry_ts_ns);
    hash = fold_double(hash, row.strike);
    hash = fold_u64(hash, static_cast<std::uint64_t>(row.side));
    hash = fold_double(hash, row.multiplier);
    hash = fold_u64(hash, row.standard_deliverable ? 1U : 0U);
    hash = fold_identity(hash, row.definition_source_identity);
    hash = fold_double(hash, row.spot_delta_cash_per_contract);
    hash = fold_double(hash, row.spot_gamma_cash_per_contract);
    hash = fold_double(hash, row.vega_cash_per_vol_point_per_contract);
    hash = fold_double(hash, row.theta_cash_per_day_per_contract);
    hash = fold_double(hash, row.vanna_cash_per_return_vol_point_per_contract);
    hash = fold_double(hash, row.volga_cash_per_vol_point_squared_per_contract);
    hash = fold_double(hash, row.premium_cash_notional_per_contract);
    hash = fold_u64(hash, static_cast<std::uint64_t>(row.status));
    hash = fold_identity(hash, row.risk_source_identity);
    hash = fold_identity(hash, row.surface_source_identity);
  }
  for (const OptionRiskScenarioPnlRow &row : pnl_rows) {
    hash = fold_i64(hash, row.decision_ts_ns);
    hash = fold_u64(hash, row.contract_id);
    hash = fold_u64(hash, row.scenario_id);
    hash = fold_i64(hash, row.observed_ts_ns);
    hash = fold_i64(hash, row.available_ts_ns);
    hash = fold_double(hash, row.pnl_per_long_contract);
    hash = fold_identity(hash, row.source_identity);
  }
  return hash;
}

[[nodiscard]] Result<double> checked_product(double left, double right) {
  const double value = left * right;
  if (!finite(value)) {
    return Err(ErrorCode::OutOfRange, "option risk product exceeds finite range");
  }
  return Ok(value);
}

[[nodiscard]] Result<void> checked_accumulate(double &total, double value) {
  const double next = total + value;
  if (!finite(next)) {
    return Err(ErrorCode::OutOfRange, "option risk aggregate exceeds finite range");
  }
  total = next;
  return Ok();
}

[[nodiscard]] Result<std::uint64_t> magnitude(std::int64_t value) {
  if (value == (std::numeric_limits<std::int64_t>::min)()) {
    return Err(ErrorCode::OutOfRange, "option risk quantity magnitude exceeds uint64 range");
  }
  return Ok(static_cast<std::uint64_t>(value < 0 ? -value : value));
}

[[nodiscard]] Result<void> validate_exact_quantity(std::int64_t value) {
  ATX_TRY(std::uint64_t absolute, magnitude(value));
  constexpr std::uint64_t kMaxExactDoubleInteger = std::uint64_t{1}
                                                   << std::numeric_limits<double>::digits;
  if (absolute > kMaxExactDoubleInteger) {
    return Err(ErrorCode::OutOfRange,
               "option risk quantity is not exactly representable as a double");
  }
  return Ok();
}

[[nodiscard]] bool valid_limits(const OptionRiskHardLimits &limits) noexcept {
  const std::array values{
      limits.max_abs_spot_delta_cash,
      limits.max_abs_spot_gamma_cash,
      limits.max_abs_vega_cash_per_vol_point,
      limits.max_abs_theta_cash_per_day,
      limits.max_abs_vanna_cash_per_return_vol_point,
      limits.max_abs_volga_cash_per_vol_point_squared,
      limits.max_gross_spot_gamma_cash,
      limits.max_gross_vega_cash_per_vol_point,
      limits.max_gross_vanna_cash_per_return_vol_point,
      limits.max_gross_volga_cash_per_vol_point_squared,
      limits.max_gross_premium_cash_notional,
      limits.max_scenario_loss,
      limits.max_single_underlier_scenario_loss,
  };
  return std::all_of(values.begin(), values.end(),
                     [](double value) noexcept { return finite(value) && value >= 0.0; });
}

[[nodiscard]] std::uint32_t breach_bit(OptionRiskBreach breach) noexcept {
  return static_cast<std::uint32_t>(breach);
}

[[nodiscard]] std::uint32_t breach_mask(const OptionRiskWorstFillMetrics &risk,
                                        const OptionRiskHardLimits &limits) noexcept {
  std::uint32_t mask = 0U;
  const auto add = [&mask](bool breached, OptionRiskBreach breach) noexcept {
    if (breached) {
      mask |= breach_bit(breach);
    }
  };
  add(risk.open_order_contracts > limits.max_open_order_contracts,
      OptionRiskBreach::OpenOrderContracts);
  add(risk.max_abs_spot_delta_cash > limits.max_abs_spot_delta_cash, OptionRiskBreach::SpotDelta);
  add(risk.max_abs_spot_gamma_cash > limits.max_abs_spot_gamma_cash, OptionRiskBreach::SpotGamma);
  add(risk.max_abs_vega_cash_per_vol_point > limits.max_abs_vega_cash_per_vol_point,
      OptionRiskBreach::Vega);
  add(risk.max_abs_theta_cash_per_day > limits.max_abs_theta_cash_per_day, OptionRiskBreach::Theta);
  add(risk.max_abs_vanna_cash_per_return_vol_point > limits.max_abs_vanna_cash_per_return_vol_point,
      OptionRiskBreach::Vanna);
  add(risk.max_abs_volga_cash_per_vol_point_squared >
          limits.max_abs_volga_cash_per_vol_point_squared,
      OptionRiskBreach::Volga);
  add(risk.max_gross_spot_gamma_cash > limits.max_gross_spot_gamma_cash,
      OptionRiskBreach::GrossGamma);
  add(risk.max_gross_vega_cash_per_vol_point > limits.max_gross_vega_cash_per_vol_point,
      OptionRiskBreach::GrossVega);
  add(risk.max_gross_vanna_cash_per_return_vol_point >
          limits.max_gross_vanna_cash_per_return_vol_point,
      OptionRiskBreach::GrossVanna);
  add(risk.max_gross_volga_cash_per_vol_point_squared >
          limits.max_gross_volga_cash_per_vol_point_squared,
      OptionRiskBreach::GrossVolga);
  add(risk.max_gross_premium_cash_notional > limits.max_gross_premium_cash_notional,
      OptionRiskBreach::GrossPremium);
  add(risk.scenario_loss > limits.max_scenario_loss, OptionRiskBreach::ScenarioLoss);
  add(risk.max_single_underlier_scenario_loss > limits.max_single_underlier_scenario_loss,
      OptionRiskBreach::UnderlierScenarioLoss);
  return mask;
}

[[nodiscard]] double max_abs(double low, double high) noexcept {
  return (std::max)(std::abs(low), std::abs(high));
}

} // namespace

OptionRiskPanel::OptionRiskPanel(
    std::vector<std::int64_t> dates, std::vector<std::uint64_t> contract_ids,
    std::vector<atx::engine::InstrumentId> engine_ids, std::vector<std::uint32_t> underlier_uids,
    std::vector<std::uint64_t> scenario_ids, std::vector<OptionRiskContractRow> contract_rows,
    std::vector<double> scenario_pnl, OptionRiskPanelProvenance provenance,
    std::uint64_t definition_hash) noexcept
    : dates_{std::move(dates)}, contract_ids_{std::move(contract_ids)},
      engine_ids_{std::move(engine_ids)}, underlier_uids_{std::move(underlier_uids)},
      scenario_ids_{std::move(scenario_ids)}, contract_rows_{std::move(contract_rows)},
      scenario_pnl_{std::move(scenario_pnl)}, provenance_{provenance},
      definition_hash_{definition_hash} {}

Result<OptionRiskPanel>
OptionRiskPanel::create(std::span<const OptionRiskContractRow> contract_rows,
                        std::span<const OptionRiskScenario> scenarios,
                        std::span<const OptionRiskScenarioPnlRow> scenario_pnl_rows,
                        const OptionRiskPanelProvenance &provenance, OptionRiskPanelLimits limits) {
  if (contract_rows.empty() || scenarios.empty() || limits.max_scenarios == 0U ||
      contract_rows.size() > limits.max_contract_rows || scenarios.size() > limits.max_scenarios ||
      scenario_pnl_rows.size() > limits.max_scenario_rows ||
      provenance.pricer_model_version == 0U || provenance.greek_convention_version == 0U ||
      !populated(provenance.risk_snapshot_digest) ||
      !populated(provenance.scenario_manifest_digest)) {
    return Err(ErrorCode::InvalidArgument, "option risk panel inputs or provenance are invalid");
  }
  ATX_TRY(std::size_t contract_row_bytes,
          checked_mul_size(contract_rows.size(), sizeof(OptionRiskContractRow)));
  ATX_TRY(std::size_t scenario_id_bytes, checked_mul_size(scenarios.size(), sizeof(std::uint64_t)));
  ATX_TRY(std::size_t scenario_pnl_bytes,
          checked_mul_size(scenario_pnl_rows.size(), sizeof(double)));
  ATX_TRY(std::size_t retained_lower_bound,
          checked_add_size(contract_row_bytes, scenario_id_bytes));
  ATX_TRY(retained_lower_bound, checked_add_size(retained_lower_bound, scenario_pnl_bytes));
  if (retained_lower_bound > limits.max_workspace_bytes) {
    return Err(ErrorCode::OutOfRange,
               "option risk panel retained payload lower bound exceeds workspace limit");
  }
  try {
    std::vector<OptionRiskContractRow> canonical_rows(contract_rows.begin(), contract_rows.end());
    for (const OptionRiskContractRow &row : canonical_rows) {
      ATX_TRY_VOID(validate_contract_row(row));
    }
    std::sort(canonical_rows.begin(), canonical_rows.end(),
              [](const OptionRiskContractRow &left, const OptionRiskContractRow &right) noexcept {
                return std::tie(left.decision_ts_ns, left.contract_id) <
                       std::tie(right.decision_ts_ns, right.contract_id);
              });
    const auto duplicate = std::adjacent_find(
        canonical_rows.begin(), canonical_rows.end(),
        [](const OptionRiskContractRow &left, const OptionRiskContractRow &right) noexcept {
          return left.decision_ts_ns == right.decision_ts_ns &&
                 left.contract_id == right.contract_id;
        });
    if (duplicate != canonical_rows.end()) {
      return Err(ErrorCode::AlreadyExists, "option risk panel contains a duplicate contract row");
    }

    std::vector<std::int64_t> dates;
    for (const OptionRiskContractRow &row : canonical_rows) {
      if (dates.empty() || dates.back() != row.decision_ts_ns) {
        dates.push_back(row.decision_ts_ns);
      }
    }
    if (canonical_rows.size() % dates.size() != 0U) {
      return Err(ErrorCode::InvalidArgument, "option risk panel contract grid is incomplete");
    }
    const std::size_t contract_count = canonical_rows.size() / dates.size();
    if (contract_count == 0U) {
      return Err(ErrorCode::InvalidArgument, "option risk panel contract grid is empty");
    }
    std::vector<std::uint64_t> contract_ids;
    std::vector<atx::engine::InstrumentId> engine_ids;
    std::vector<std::uint32_t> underlier_uids;
    contract_ids.reserve(contract_count);
    engine_ids.reserve(contract_count);
    underlier_uids.reserve(contract_count);
    for (std::size_t index = 0; index < contract_count; ++index) {
      contract_ids.push_back(canonical_rows[index].contract_id);
      engine_ids.push_back(canonical_rows[index].engine_id);
      underlier_uids.push_back(canonical_rows[index].underlier_uid);
    }
    for (std::size_t date_index = 0; date_index < dates.size(); ++date_index) {
      for (std::size_t contract_index = 0; contract_index < contract_count; ++contract_index) {
        const OptionRiskContractRow &row =
            canonical_rows[date_index * contract_count + contract_index];
        if (row.decision_ts_ns != dates[date_index] ||
            row.contract_id != contract_ids[contract_index] ||
            row.engine_id != engine_ids[contract_index] ||
            row.underlier_uid != underlier_uids[contract_index] ||
            row.expiry_ts_ns != canonical_rows[contract_index].expiry_ts_ns ||
            row.strike != canonical_rows[contract_index].strike ||
            row.side != canonical_rows[contract_index].side ||
            row.multiplier != canonical_rows[contract_index].multiplier ||
            row.standard_deliverable != canonical_rows[contract_index].standard_deliverable) {
          return Err(ErrorCode::InvalidArgument,
                     "option risk panel contract grid or catalog changes across dates");
        }
      }
    }

    std::vector<OptionRiskScenario> canonical_scenarios(scenarios.begin(), scenarios.end());
    for (const OptionRiskScenario &scenario : canonical_scenarios) {
      ATX_TRY_VOID(validate_scenario(scenario));
    }
    std::sort(canonical_scenarios.begin(), canonical_scenarios.end(),
              [](const OptionRiskScenario &left, const OptionRiskScenario &right) noexcept {
                return left.scenario_id < right.scenario_id;
              });
    if (std::adjacent_find(
            canonical_scenarios.begin(), canonical_scenarios.end(),
            [](const OptionRiskScenario &left, const OptionRiskScenario &right) noexcept {
              return left.scenario_id == right.scenario_id;
            }) != canonical_scenarios.end()) {
      return Err(ErrorCode::AlreadyExists, "option risk panel contains a duplicate scenario");
    }
    std::vector<std::uint64_t> scenario_ids;
    scenario_ids.reserve(canonical_scenarios.size());
    for (const OptionRiskScenario &scenario : canonical_scenarios) {
      scenario_ids.push_back(scenario.scenario_id);
    }

    ATX_TRY(std::size_t contract_cells, checked_mul_size(dates.size(), contract_count));
    ATX_TRY(std::size_t expected_pnl_rows, checked_mul_size(contract_cells, scenario_ids.size()));
    if (scenario_pnl_rows.size() != expected_pnl_rows) {
      return Err(ErrorCode::InvalidArgument, "option risk panel scenario grid is incomplete");
    }
    ATX_TRY(std::size_t required,
            panel_required_bytes(dates.size(), contract_count, scenario_ids.size()));
    if (required > limits.max_workspace_bytes) {
      return Err(ErrorCode::OutOfRange, "option risk panel workspace limit is exceeded");
    }

    std::vector<OptionRiskScenarioPnlRow> canonical_pnl(scenario_pnl_rows.begin(),
                                                        scenario_pnl_rows.end());
    for (const OptionRiskScenarioPnlRow &row : canonical_pnl) {
      ATX_TRY_VOID(validate_pnl_row(row));
    }
    std::sort(
        canonical_pnl.begin(), canonical_pnl.end(),
        [](const OptionRiskScenarioPnlRow &left, const OptionRiskScenarioPnlRow &right) noexcept {
          return std::tie(left.decision_ts_ns, left.scenario_id, left.contract_id) <
                 std::tie(right.decision_ts_ns, right.scenario_id, right.contract_id);
        });
    std::vector<double> pnl;
    pnl.reserve(expected_pnl_rows);
    std::size_t input_index = 0U;
    for (std::int64_t date : dates) {
      for (std::uint64_t scenario_id : scenario_ids) {
        for (std::uint64_t contract_id : contract_ids) {
          const OptionRiskScenarioPnlRow &row = canonical_pnl[input_index];
          if (row.decision_ts_ns != date || row.scenario_id != scenario_id ||
              row.contract_id != contract_id) {
            return Err(ErrorCode::InvalidArgument,
                       "option risk panel scenario keys do not cover the canonical grid");
          }
          pnl.push_back(row.pnl_per_long_contract);
          ++input_index;
        }
      }
    }
    const std::uint64_t definition_hash =
        hash_panel(dates, contract_ids, underlier_uids, canonical_scenarios, canonical_rows,
                   canonical_pnl, provenance);
    return Ok(OptionRiskPanel{std::move(dates), std::move(contract_ids), std::move(engine_ids),
                              std::move(underlier_uids), std::move(scenario_ids),
                              std::move(canonical_rows), std::move(pnl), provenance,
                              definition_hash});
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "option risk panel allocation failed");
  } catch (const std::length_error &) {
    return Err(ErrorCode::OutOfRange, "option risk panel capacity exceeds vector limits");
  }
}

struct OptionPreTradeRiskEngine::Impl {
  explicit Impl(OptionRiskEngineLimits configured_limits) : limits{configured_limits} {}

  OptionRiskEngineLimits limits{};
  std::vector<std::int64_t> baseline_min;
  std::vector<std::int64_t> baseline_max;
  std::vector<std::int64_t> baseline_projected;
  std::vector<std::int64_t> candidate_min;
  std::vector<std::int64_t> candidate_max;
  std::vector<std::int64_t> candidate_projected;
  std::vector<std::uint32_t> bound_underlier_uids;
  std::vector<std::uint32_t> underliers;
  std::vector<std::size_t> contract_root_indices;
  std::vector<double> root_pnl;

  [[nodiscard]] Result<void> prepare_quantities(std::span<const std::int64_t> filled,
                                                std::span<const OptionRiskLeaf> live,
                                                std::span<const OptionRiskLeaf> candidates,
                                                std::uint64_t &baseline_open,
                                                std::uint64_t &candidate_open) {
    baseline_min.assign(filled.begin(), filled.end());
    baseline_max.assign(filled.begin(), filled.end());
    baseline_projected.assign(filled.begin(), filled.end());
    const auto add_leaf = [count = filled.size()](std::span<const OptionRiskLeaf> leaves,
                                                  std::vector<std::int64_t> &low,
                                                  std::vector<std::int64_t> &high,
                                                  std::vector<std::int64_t> &projected,
                                                  std::uint64_t &open) -> Result<void> {
      for (const OptionRiskLeaf &leaf : leaves) {
        if (leaf.contract_index >= count || leaf.remaining_contracts == 0) {
          return Err(ErrorCode::InvalidArgument, "option risk leaf is invalid");
        }
        ATX_TRY(std::uint64_t absolute, magnitude(leaf.remaining_contracts));
        if (absolute > (std::numeric_limits<std::uint64_t>::max)() - open) {
          return Err(ErrorCode::OutOfRange, "option risk open-contract total overflows");
        }
        open += absolute;
        ATX_TRY(projected[leaf.contract_index],
                checked_add_i64(projected[leaf.contract_index], leaf.remaining_contracts));
        if (leaf.remaining_contracts < 0) {
          ATX_TRY(low[leaf.contract_index],
                  checked_add_i64(low[leaf.contract_index], leaf.remaining_contracts));
        } else {
          ATX_TRY(high[leaf.contract_index],
                  checked_add_i64(high[leaf.contract_index], leaf.remaining_contracts));
        }
      }
      return Ok();
    };
    baseline_open = 0U;
    ATX_TRY_VOID(add_leaf(live, baseline_min, baseline_max, baseline_projected, baseline_open));
    candidate_min.assign(baseline_min.begin(), baseline_min.end());
    candidate_max.assign(baseline_max.begin(), baseline_max.end());
    candidate_projected.assign(baseline_projected.begin(), baseline_projected.end());
    candidate_open = baseline_open;
    ATX_TRY_VOID(
        add_leaf(candidates, candidate_min, candidate_max, candidate_projected, candidate_open));
    for (std::size_t index = 0; index < filled.size(); ++index) {
      ATX_TRY_VOID(validate_exact_quantity(filled[index]));
      ATX_TRY_VOID(validate_exact_quantity(baseline_min[index]));
      ATX_TRY_VOID(validate_exact_quantity(baseline_max[index]));
      ATX_TRY_VOID(validate_exact_quantity(baseline_projected[index]));
      ATX_TRY_VOID(validate_exact_quantity(candidate_min[index]));
      ATX_TRY_VOID(validate_exact_quantity(candidate_max[index]));
      ATX_TRY_VOID(validate_exact_quantity(candidate_projected[index]));
    }
    return Ok();
  }

  [[nodiscard]] Result<void> prepare_underliers(const OptionRiskPanel &panel) {
    if (bound_underlier_uids.size() == panel.underlier_uids().size() &&
        std::equal(bound_underlier_uids.begin(), bound_underlier_uids.end(),
                   panel.underlier_uids().begin())) {
      return Ok();
    }
    // Invalidate the cache key before mutating any dependent scratch. If a
    // capacity check below fails, the next evaluation must rebuild rather than
    // reusing partially updated state with the prior panel's key.
    bound_underlier_uids.clear();
    underliers.assign(panel.underlier_uids().begin(), panel.underlier_uids().end());
    std::sort(underliers.begin(), underliers.end());
    underliers.erase(std::unique(underliers.begin(), underliers.end()), underliers.end());
    if (underliers.empty() || underliers.size() > limits.max_underliers) {
      return Err(ErrorCode::OutOfRange, "option risk underlier capacity is exceeded");
    }
    contract_root_indices.resize(panel.contract_count());
    for (std::size_t contract = 0; contract < panel.contract_count(); ++contract) {
      contract_root_indices[contract] = static_cast<std::size_t>(
          std::lower_bound(underliers.begin(), underliers.end(), panel.underlier_uids()[contract]) -
          underliers.begin());
    }
    ATX_TRY(std::size_t root_cells, checked_mul_size(underliers.size(), kScenarioMetricCount));
    root_pnl.resize(root_cells);
    bound_underlier_uids.assign(panel.underlier_uids().begin(), panel.underlier_uids().end());
    return Ok();
  }

  [[nodiscard]] Result<void> accumulate_point_greeks(const OptionRiskPanel &panel,
                                                     std::size_t date_index,
                                                     std::span<const std::int64_t> quantities,
                                                     OptionRiskPointMetrics &out) const {
    for (std::size_t index = 0; index < quantities.size(); ++index) {
      const OptionRiskContractRow &row = panel.contract_row(date_index, index);
      const double quantity = static_cast<double>(quantities[index]);
      ATX_TRY(double delta, checked_product(quantity, row.spot_delta_cash_per_contract));
      ATX_TRY(double gamma, checked_product(quantity, row.spot_gamma_cash_per_contract));
      ATX_TRY(double vega, checked_product(quantity, row.vega_cash_per_vol_point_per_contract));
      ATX_TRY(double theta, checked_product(quantity, row.theta_cash_per_day_per_contract));
      ATX_TRY(double vanna,
              checked_product(quantity, row.vanna_cash_per_return_vol_point_per_contract));
      ATX_TRY(double volga,
              checked_product(quantity, row.volga_cash_per_vol_point_squared_per_contract));
      ATX_TRY_VOID(checked_accumulate(out.spot_delta_cash, delta));
      ATX_TRY_VOID(checked_accumulate(out.spot_gamma_cash, gamma));
      ATX_TRY_VOID(checked_accumulate(out.vega_cash_per_vol_point, vega));
      ATX_TRY_VOID(checked_accumulate(out.theta_cash_per_day, theta));
      ATX_TRY_VOID(checked_accumulate(out.vanna_cash_per_return_vol_point, vanna));
      ATX_TRY_VOID(checked_accumulate(out.volga_cash_per_vol_point_squared, volga));
      ATX_TRY_VOID(checked_accumulate(out.gross_spot_gamma_cash, std::abs(gamma)));
      ATX_TRY_VOID(checked_accumulate(out.gross_vega_cash_per_vol_point, std::abs(vega)));
      ATX_TRY_VOID(checked_accumulate(out.gross_vanna_cash_per_return_vol_point, std::abs(vanna)));
      ATX_TRY_VOID(checked_accumulate(out.gross_volga_cash_per_vol_point_squared, std::abs(volga)));
      ATX_TRY(double premium,
              checked_product(std::abs(quantity), row.premium_cash_notional_per_contract));
      ATX_TRY_VOID(checked_accumulate(out.gross_premium_cash_notional, premium));
    }
    return Ok();
  }

  [[nodiscard]] Result<OptionRiskPointMetrics>
  point_greek_metrics(const OptionRiskPanel &panel, std::size_t date_index,
                      std::span<const std::int64_t> quantities) {
    OptionRiskPointMetrics out;
    ATX_TRY_VOID(accumulate_point_greeks(panel, date_index, quantities, out));
    return Ok(out);
  }

  [[nodiscard]] Result<void> accumulate_worst_greek(std::int64_t low_quantity,
                                                    std::int64_t high_quantity, double coefficient,
                                                    double &low_total, double &high_total,
                                                    double &gross_total) const {
    ATX_TRY(double low, checked_product(static_cast<double>(low_quantity), coefficient));
    ATX_TRY(double high, checked_product(static_cast<double>(high_quantity), coefficient));
    if (low > high) {
      std::swap(low, high);
    }
    ATX_TRY_VOID(checked_accumulate(low_total, low));
    ATX_TRY_VOID(checked_accumulate(high_total, high));
    ATX_TRY_VOID(checked_accumulate(gross_total, (std::max)(std::abs(low), std::abs(high))));
    return Ok();
  }

  [[nodiscard]] Result<OptionRiskWorstFillMetrics>
  worst_greek_metrics(const OptionRiskPanel &panel, std::size_t date_index,
                      std::span<const std::int64_t> low, std::span<const std::int64_t> high,
                      std::uint64_t open_order_contracts) {
    OptionRiskWorstFillMetrics out;
    out.open_order_contracts = open_order_contracts;
    double delta_low = 0.0;
    double delta_high = 0.0;
    double delta_gross = 0.0;
    double gamma_low = 0.0;
    double gamma_high = 0.0;
    double vega_low = 0.0;
    double vega_high = 0.0;
    double theta_low = 0.0;
    double theta_high = 0.0;
    double theta_gross = 0.0;
    double vanna_low = 0.0;
    double vanna_high = 0.0;
    double volga_low = 0.0;
    double volga_high = 0.0;
    for (std::size_t index = 0; index < low.size(); ++index) {
      const OptionRiskContractRow &row = panel.contract_row(date_index, index);
      ATX_TRY_VOID(accumulate_worst_greek(low[index], high[index], row.spot_delta_cash_per_contract,
                                          delta_low, delta_high, delta_gross));
      ATX_TRY_VOID(accumulate_worst_greek(low[index], high[index], row.spot_gamma_cash_per_contract,
                                          gamma_low, gamma_high, out.max_gross_spot_gamma_cash));
      ATX_TRY_VOID(accumulate_worst_greek(low[index], high[index],
                                          row.vega_cash_per_vol_point_per_contract, vega_low,
                                          vega_high, out.max_gross_vega_cash_per_vol_point));
      ATX_TRY_VOID(accumulate_worst_greek(low[index], high[index],
                                          row.theta_cash_per_day_per_contract, theta_low,
                                          theta_high, theta_gross));
      ATX_TRY_VOID(accumulate_worst_greek(
          low[index], high[index], row.vanna_cash_per_return_vol_point_per_contract, vanna_low,
          vanna_high, out.max_gross_vanna_cash_per_return_vol_point));
      ATX_TRY_VOID(accumulate_worst_greek(
          low[index], high[index], row.volga_cash_per_vol_point_squared_per_contract, volga_low,
          volga_high, out.max_gross_volga_cash_per_vol_point_squared));
      ATX_TRY(std::uint64_t low_abs, magnitude(low[index]));
      ATX_TRY(std::uint64_t high_abs, magnitude(high[index]));
      ATX_TRY(double premium, checked_product(static_cast<double>((std::max)(low_abs, high_abs)),
                                              row.premium_cash_notional_per_contract));
      ATX_TRY_VOID(checked_accumulate(out.max_gross_premium_cash_notional, premium));
    }
    out.max_abs_spot_delta_cash = max_abs(delta_low, delta_high);
    out.max_abs_spot_gamma_cash = max_abs(gamma_low, gamma_high);
    out.max_abs_vega_cash_per_vol_point = max_abs(vega_low, vega_high);
    out.max_abs_theta_cash_per_day = max_abs(theta_low, theta_high);
    out.max_abs_vanna_cash_per_return_vol_point = max_abs(vanna_low, vanna_high);
    out.max_abs_volga_cash_per_vol_point_squared = max_abs(volga_low, volga_high);
    return Ok(out);
  }

  template <typename Metrics>
  [[nodiscard]] Result<void> update_scenario_extrema(Metrics &out, std::uint64_t scenario_id,
                                                     std::span<const double> root_values) const {
    double scenario_loss = 0.0;
    double max_root_loss = 0.0;
    std::uint32_t max_root_uid = underliers.front();
    for (std::size_t root = 0; root < root_values.size(); ++root) {
      const double loss = (std::max)(0.0, -root_values[root]);
      ATX_TRY_VOID(checked_accumulate(scenario_loss, loss));
      if (loss > max_root_loss || (loss == max_root_loss && underliers[root] < max_root_uid)) {
        max_root_loss = loss;
        max_root_uid = underliers[root];
      }
    }
    if (scenario_loss > out.scenario_loss ||
        (scenario_loss == out.scenario_loss && scenario_id < out.worst_scenario_id)) {
      out.scenario_loss = scenario_loss;
      out.worst_scenario_id = scenario_id;
    }
    if (max_root_loss > out.max_single_underlier_scenario_loss ||
        (max_root_loss == out.max_single_underlier_scenario_loss &&
         (scenario_id < out.worst_underlier_scenario_id ||
          (scenario_id == out.worst_underlier_scenario_id &&
           max_root_uid < out.worst_underlier_uid)))) {
      out.max_single_underlier_scenario_loss = max_root_loss;
      out.worst_underlier_scenario_id = scenario_id;
      out.worst_underlier_uid = max_root_uid;
    }
    return Ok();
  }

  template <typename Metrics>
  void initialize_scenario_extrema(const OptionRiskPanel &panel, Metrics &out) const noexcept {
    out.worst_scenario_id = panel.scenario_ids().front();
    out.worst_underlier_scenario_id = panel.scenario_ids().front();
    out.worst_underlier_uid = underliers.front();
  }

  [[nodiscard]] std::span<double> root_segment(std::size_t metric_index) noexcept {
    return {root_pnl.data() + metric_index * underliers.size(), underliers.size()};
  }

  [[nodiscard]] Result<void> accumulate_all_scenarios(
      const OptionRiskPanel &panel, std::size_t date_index, std::span<const std::int64_t> filled,
      std::span<const std::int64_t> baseline_projected, std::span<const std::int64_t> baseline_low,
      std::span<const std::int64_t> baseline_high,
      std::span<const std::int64_t> candidate_projected,
      std::span<const std::int64_t> candidate_low, std::span<const std::int64_t> candidate_high,
      OptionRiskPointMetrics &filled_metrics, OptionRiskPointMetrics &baseline_projected_metrics,
      OptionRiskWorstFillMetrics &baseline_worst_metrics,
      OptionRiskPointMetrics &candidate_projected_metrics,
      OptionRiskWorstFillMetrics &candidate_worst_metrics) {
    initialize_scenario_extrema(panel, filled_metrics);
    initialize_scenario_extrema(panel, baseline_projected_metrics);
    initialize_scenario_extrema(panel, baseline_worst_metrics);
    initialize_scenario_extrema(panel, candidate_projected_metrics);
    initialize_scenario_extrema(panel, candidate_worst_metrics);

    for (std::size_t scenario = 0; scenario < panel.scenario_count(); ++scenario) {
      std::fill(root_pnl.begin(), root_pnl.end(), 0.0);
      for (std::size_t contract = 0; contract < filled.size(); ++contract) {
        const double pnl = panel.scenario_pnl(date_index, scenario, contract);
        ATX_TRY(double filled_pnl, checked_product(static_cast<double>(filled[contract]), pnl));
        ATX_TRY(double baseline_projected_pnl,
                checked_product(static_cast<double>(baseline_projected[contract]), pnl));
        ATX_TRY(double baseline_low_pnl,
                checked_product(static_cast<double>(baseline_low[contract]), pnl));
        ATX_TRY(double baseline_high_pnl,
                checked_product(static_cast<double>(baseline_high[contract]), pnl));
        ATX_TRY(double candidate_projected_pnl,
                checked_product(static_cast<double>(candidate_projected[contract]), pnl));
        ATX_TRY(double candidate_low_pnl,
                checked_product(static_cast<double>(candidate_low[contract]), pnl));
        ATX_TRY(double candidate_high_pnl,
                checked_product(static_cast<double>(candidate_high[contract]), pnl));
        const std::size_t root = contract_root_indices[contract];
        ATX_TRY_VOID(checked_accumulate(root_segment(0U)[root], filled_pnl));
        ATX_TRY_VOID(checked_accumulate(root_segment(1U)[root], baseline_projected_pnl));
        ATX_TRY_VOID(checked_accumulate(
            root_segment(2U)[root], (std::min)({filled_pnl, baseline_low_pnl, baseline_high_pnl})));
        ATX_TRY_VOID(checked_accumulate(root_segment(3U)[root], candidate_projected_pnl));
        ATX_TRY_VOID(
            checked_accumulate(root_segment(4U)[root],
                               (std::min)({filled_pnl, candidate_low_pnl, candidate_high_pnl})));
      }
      const std::uint64_t scenario_id = panel.scenario_ids()[scenario];
      ATX_TRY_VOID(update_scenario_extrema(filled_metrics, scenario_id, root_segment(0U)));
      ATX_TRY_VOID(
          update_scenario_extrema(baseline_projected_metrics, scenario_id, root_segment(1U)));
      ATX_TRY_VOID(update_scenario_extrema(baseline_worst_metrics, scenario_id, root_segment(2U)));
      ATX_TRY_VOID(
          update_scenario_extrema(candidate_projected_metrics, scenario_id, root_segment(3U)));
      ATX_TRY_VOID(update_scenario_extrema(candidate_worst_metrics, scenario_id, root_segment(4U)));
    }
    return Ok();
  }
};

Result<std::size_t>
option_pretrade_risk_required_workspace_bytes(const OptionRiskEngineLimits &limits) {
  if (limits.max_contracts == 0U || limits.max_live_leaves == 0U ||
      limits.max_candidate_leaves == 0U || limits.max_scenarios == 0U ||
      limits.max_underliers == 0U) {
    return Err(ErrorCode::InvalidArgument, "option risk engine limits must be positive");
  }
  ATX_TRY(std::size_t quantity_slots, checked_mul_size(limits.max_contracts, 6U));
  ATX_TRY(std::size_t quantity_bytes, checked_mul_size(quantity_slots, sizeof(std::int64_t)));
  ATX_TRY(std::size_t underlier_bytes,
          checked_mul_size(limits.max_contracts, sizeof(std::uint32_t)));
  ATX_TRY(std::size_t bound_underlier_bytes,
          checked_mul_size(limits.max_contracts, sizeof(std::uint32_t)));
  ATX_TRY(std::size_t root_index_bytes,
          checked_mul_size(limits.max_contracts, sizeof(std::size_t)));
  ATX_TRY(std::size_t root_slots, checked_mul_size(limits.max_underliers, kScenarioMetricCount));
  ATX_TRY(std::size_t root_bytes, checked_mul_size(root_slots, sizeof(double)));
  ATX_TRY(std::size_t bytes, checked_add_size(quantity_bytes, underlier_bytes));
  ATX_TRY(bytes, checked_add_size(bytes, bound_underlier_bytes));
  ATX_TRY(bytes, checked_add_size(bytes, root_index_bytes));
  ATX_TRY(bytes, checked_add_size(bytes, root_bytes));
  return Ok(bytes);
}

Result<OptionPreTradeRiskEngine> OptionPreTradeRiskEngine::create(OptionRiskEngineLimits limits) {
  ATX_TRY(std::size_t required, option_pretrade_risk_required_workspace_bytes(limits));
  if (required > limits.max_workspace_bytes) {
    return Err(ErrorCode::OutOfRange, "option risk engine workspace limit is exceeded");
  }
  try {
    auto impl = std::make_unique<Impl>(limits);
    impl->baseline_min.reserve(limits.max_contracts);
    impl->baseline_max.reserve(limits.max_contracts);
    impl->baseline_projected.reserve(limits.max_contracts);
    impl->candidate_min.reserve(limits.max_contracts);
    impl->candidate_max.reserve(limits.max_contracts);
    impl->candidate_projected.reserve(limits.max_contracts);
    impl->bound_underlier_uids.reserve(limits.max_contracts);
    impl->underliers.reserve(limits.max_contracts);
    impl->contract_root_indices.reserve(limits.max_contracts);
    ATX_TRY(std::size_t root_slots, checked_mul_size(limits.max_underliers, kScenarioMetricCount));
    impl->root_pnl.reserve(root_slots);
    return Ok(OptionPreTradeRiskEngine{std::move(impl)});
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "option risk engine allocation failed");
  } catch (const std::length_error &) {
    return Err(ErrorCode::OutOfRange, "option risk engine capacity exceeds vector limits");
  }
}

OptionPreTradeRiskEngine::OptionPreTradeRiskEngine(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

OptionPreTradeRiskEngine::~OptionPreTradeRiskEngine() = default;
OptionPreTradeRiskEngine::OptionPreTradeRiskEngine(OptionPreTradeRiskEngine &&) noexcept = default;
OptionPreTradeRiskEngine &
OptionPreTradeRiskEngine::operator=(OptionPreTradeRiskEngine &&) noexcept = default;

Result<OptionPreTradeRiskEvaluation> OptionPreTradeRiskEngine::evaluate(
    const OptionRiskPanel &risk_panel, std::size_t date_index,
    std::span<const research::OptionInstrument> contract_catalog,
    std::span<const std::int64_t> filled_contracts, std::span<const OptionRiskLeaf> live_leaves,
    std::span<const OptionRiskLeaf> candidate_leaves, const OptionRiskHardLimits &hard_limits) {
  if (impl_ == nullptr) {
    return Err(ErrorCode::Internal, "option risk engine has no implementation");
  }
  Impl &state = *impl_;
  const std::size_t count = contract_catalog.size();
  if (count == 0U || count > state.limits.max_contracts || count != risk_panel.contract_count() ||
      count != filled_contracts.size() || date_index >= risk_panel.dates().size() ||
      risk_panel.scenario_count() > state.limits.max_scenarios ||
      live_leaves.size() > state.limits.max_live_leaves ||
      candidate_leaves.size() > state.limits.max_candidate_leaves || !valid_limits(hard_limits)) {
    return Err(ErrorCode::InvalidArgument, "option risk evaluation inputs exceed limits");
  }
  for (std::size_t index = 0; index < count; ++index) {
    const research::OptionInstrument &instrument = contract_catalog[index];
    const OptionRiskContractRow &row = risk_panel.contract_row(date_index, index);
    if (instrument.contract_id != risk_panel.contract_ids()[index] ||
        instrument.engine_id != risk_panel.engine_ids()[index] ||
        instrument.underlier_uid != risk_panel.underlier_uids()[index] ||
        instrument.expiry_ts_ns != row.expiry_ts_ns || instrument.strike != row.strike ||
        instrument.side != row.side || instrument.multiplier != row.multiplier ||
        instrument.standard_deliverable != row.standard_deliverable ||
        row.decision_ts_ns != risk_panel.dates()[date_index] ||
        row.status != OptionRiskRowStatus::Ok) {
      return Err(ErrorCode::InvalidArgument,
                 "option risk panel does not align with the decision catalog");
    }
  }
  ATX_TRY_VOID(state.prepare_underliers(risk_panel));
  std::uint64_t baseline_open = 0U;
  std::uint64_t candidate_open = 0U;
  ATX_TRY_VOID(state.prepare_quantities(filled_contracts, live_leaves, candidate_leaves,
                                        baseline_open, candidate_open));

  OptionPreTradeRiskEvaluation out;
  ATX_TRY(out.filled, state.point_greek_metrics(risk_panel, date_index, filled_contracts));
  ATX_TRY(out.baseline_projected,
          state.point_greek_metrics(risk_panel, date_index, state.baseline_projected));
  ATX_TRY(out.baseline_worst_fill,
          state.worst_greek_metrics(risk_panel, date_index, state.baseline_min, state.baseline_max,
                                    baseline_open));
  ATX_TRY(out.candidate_projected,
          state.point_greek_metrics(risk_panel, date_index, state.candidate_projected));
  ATX_TRY(out.candidate_worst_fill,
          state.worst_greek_metrics(risk_panel, date_index, state.candidate_min,
                                    state.candidate_max, candidate_open));
  ATX_TRY_VOID(state.accumulate_all_scenarios(
      risk_panel, date_index, filled_contracts, state.baseline_projected, state.baseline_min,
      state.baseline_max, state.candidate_projected, state.candidate_min, state.candidate_max,
      out.filled, out.baseline_projected, out.baseline_worst_fill, out.candidate_projected,
      out.candidate_worst_fill));
  out.baseline_breach_mask = breach_mask(out.baseline_worst_fill, hard_limits);
  out.candidate_breach_mask = breach_mask(out.candidate_worst_fill, hard_limits);

  if (candidate_leaves.empty()) {
    out.disposition = out.baseline_breach_mask == 0U ? OptionRiskDisposition::Accept
                                                     : OptionRiskDisposition::CancelOnly;
  } else if (out.baseline_breach_mask == 0U) {
    out.disposition = out.candidate_breach_mask == 0U ? OptionRiskDisposition::Accept
                                                      : OptionRiskDisposition::RejectNewOrders;
  } else {
    const bool no_new_breach = (out.candidate_breach_mask & ~out.baseline_breach_mask) == 0U;
    const auto no_worse = [&out](OptionRiskBreach breach, double baseline,
                                 double candidate) noexcept {
      return (out.baseline_breach_mask & breach_bit(breach)) == 0U || candidate <= baseline;
    };
    const bool open_order_contracts_nonworsening =
        (out.baseline_breach_mask & breach_bit(OptionRiskBreach::OpenOrderContracts)) == 0U ||
        out.candidate_worst_fill.open_order_contracts <=
            out.baseline_worst_fill.open_order_contracts;
    bool nonworsening =
        open_order_contracts_nonworsening &&
        no_worse(OptionRiskBreach::SpotDelta, out.baseline_worst_fill.max_abs_spot_delta_cash,
                 out.candidate_worst_fill.max_abs_spot_delta_cash) &&
        no_worse(OptionRiskBreach::SpotGamma, out.baseline_worst_fill.max_abs_spot_gamma_cash,
                 out.candidate_worst_fill.max_abs_spot_gamma_cash) &&
        no_worse(OptionRiskBreach::Vega, out.baseline_worst_fill.max_abs_vega_cash_per_vol_point,
                 out.candidate_worst_fill.max_abs_vega_cash_per_vol_point) &&
        no_worse(OptionRiskBreach::Theta, out.baseline_worst_fill.max_abs_theta_cash_per_day,
                 out.candidate_worst_fill.max_abs_theta_cash_per_day) &&
        no_worse(OptionRiskBreach::Vanna,
                 out.baseline_worst_fill.max_abs_vanna_cash_per_return_vol_point,
                 out.candidate_worst_fill.max_abs_vanna_cash_per_return_vol_point) &&
        no_worse(OptionRiskBreach::Volga,
                 out.baseline_worst_fill.max_abs_volga_cash_per_vol_point_squared,
                 out.candidate_worst_fill.max_abs_volga_cash_per_vol_point_squared) &&
        no_worse(OptionRiskBreach::GrossGamma, out.baseline_worst_fill.max_gross_spot_gamma_cash,
                 out.candidate_worst_fill.max_gross_spot_gamma_cash) &&
        no_worse(OptionRiskBreach::GrossVega,
                 out.baseline_worst_fill.max_gross_vega_cash_per_vol_point,
                 out.candidate_worst_fill.max_gross_vega_cash_per_vol_point) &&
        no_worse(OptionRiskBreach::GrossVanna,
                 out.baseline_worst_fill.max_gross_vanna_cash_per_return_vol_point,
                 out.candidate_worst_fill.max_gross_vanna_cash_per_return_vol_point) &&
        no_worse(OptionRiskBreach::GrossVolga,
                 out.baseline_worst_fill.max_gross_volga_cash_per_vol_point_squared,
                 out.candidate_worst_fill.max_gross_volga_cash_per_vol_point_squared) &&
        no_worse(OptionRiskBreach::GrossPremium,
                 out.baseline_worst_fill.max_gross_premium_cash_notional,
                 out.candidate_worst_fill.max_gross_premium_cash_notional) &&
        no_worse(OptionRiskBreach::ScenarioLoss, out.baseline_worst_fill.scenario_loss,
                 out.candidate_worst_fill.scenario_loss) &&
        no_worse(OptionRiskBreach::UnderlierScenarioLoss,
                 out.baseline_worst_fill.max_single_underlier_scenario_loss,
                 out.candidate_worst_fill.max_single_underlier_scenario_loss);
    const auto improved = [&out](OptionRiskBreach breach, double baseline,
                                 double candidate) noexcept {
      return (out.baseline_breach_mask & breach_bit(breach)) != 0U && candidate < baseline;
    };
    const bool projected_improvement =
        improved(OptionRiskBreach::SpotDelta, std::abs(out.baseline_projected.spot_delta_cash),
                 std::abs(out.candidate_projected.spot_delta_cash)) ||
        improved(OptionRiskBreach::SpotGamma, std::abs(out.baseline_projected.spot_gamma_cash),
                 std::abs(out.candidate_projected.spot_gamma_cash)) ||
        improved(OptionRiskBreach::Vega, std::abs(out.baseline_projected.vega_cash_per_vol_point),
                 std::abs(out.candidate_projected.vega_cash_per_vol_point)) ||
        improved(OptionRiskBreach::Theta, std::abs(out.baseline_projected.theta_cash_per_day),
                 std::abs(out.candidate_projected.theta_cash_per_day)) ||
        improved(OptionRiskBreach::Vanna,
                 std::abs(out.baseline_projected.vanna_cash_per_return_vol_point),
                 std::abs(out.candidate_projected.vanna_cash_per_return_vol_point)) ||
        improved(OptionRiskBreach::Volga,
                 std::abs(out.baseline_projected.volga_cash_per_vol_point_squared),
                 std::abs(out.candidate_projected.volga_cash_per_vol_point_squared)) ||
        improved(OptionRiskBreach::GrossGamma, out.baseline_projected.gross_spot_gamma_cash,
                 out.candidate_projected.gross_spot_gamma_cash) ||
        improved(OptionRiskBreach::GrossVega, out.baseline_projected.gross_vega_cash_per_vol_point,
                 out.candidate_projected.gross_vega_cash_per_vol_point) ||
        improved(OptionRiskBreach::GrossVanna,
                 out.baseline_projected.gross_vanna_cash_per_return_vol_point,
                 out.candidate_projected.gross_vanna_cash_per_return_vol_point) ||
        improved(OptionRiskBreach::GrossVolga,
                 out.baseline_projected.gross_volga_cash_per_vol_point_squared,
                 out.candidate_projected.gross_volga_cash_per_vol_point_squared) ||
        improved(OptionRiskBreach::GrossPremium, out.baseline_projected.gross_premium_cash_notional,
                 out.candidate_projected.gross_premium_cash_notional) ||
        improved(OptionRiskBreach::ScenarioLoss, out.baseline_projected.scenario_loss,
                 out.candidate_projected.scenario_loss) ||
        improved(OptionRiskBreach::UnderlierScenarioLoss,
                 out.baseline_projected.max_single_underlier_scenario_loss,
                 out.candidate_projected.max_single_underlier_scenario_loss);
    nonworsening = nonworsening && no_new_breach;
    out.disposition = nonworsening && projected_improvement
                          ? OptionRiskDisposition::ReduceOnlyAccept
                          : OptionRiskDisposition::CancelOnly;
  }

  std::uint64_t hash = fold_u64(kFnvOffset, kOptionPreTradeRiskModelVersion);
  hash = fold_u64(hash, kOptionPreTradeRiskOrderingVersion);
  hash = fold_u64(hash, risk_panel.definition_hash());
  hash = fold_u64(hash, date_index);
  hash = fold_u64(hash, hard_limits.max_open_order_contracts);
  hash = fold_double(hash, hard_limits.max_abs_spot_delta_cash);
  hash = fold_double(hash, hard_limits.max_abs_spot_gamma_cash);
  hash = fold_double(hash, hard_limits.max_abs_vega_cash_per_vol_point);
  hash = fold_double(hash, hard_limits.max_abs_theta_cash_per_day);
  hash = fold_double(hash, hard_limits.max_abs_vanna_cash_per_return_vol_point);
  hash = fold_double(hash, hard_limits.max_abs_volga_cash_per_vol_point_squared);
  hash = fold_double(hash, hard_limits.max_gross_spot_gamma_cash);
  hash = fold_double(hash, hard_limits.max_gross_vega_cash_per_vol_point);
  hash = fold_double(hash, hard_limits.max_gross_vanna_cash_per_return_vol_point);
  hash = fold_double(hash, hard_limits.max_gross_volga_cash_per_vol_point_squared);
  hash = fold_double(hash, hard_limits.max_gross_premium_cash_notional);
  hash = fold_double(hash, hard_limits.max_scenario_loss);
  hash = fold_double(hash, hard_limits.max_single_underlier_scenario_loss);
  hash = fold_u64(hash, count);
  for (std::size_t index = 0; index < count; ++index) {
    hash = fold_i64(hash, state.baseline_min[index]);
    hash = fold_i64(hash, state.baseline_max[index]);
    hash = fold_i64(hash, state.baseline_projected[index]);
    hash = fold_i64(hash, state.candidate_min[index]);
    hash = fold_i64(hash, state.candidate_max[index]);
    hash = fold_i64(hash, state.candidate_projected[index]);
  }
  hash = fold_u64(hash, baseline_open);
  hash = fold_u64(hash, candidate_open);
  hash = fold_u64(hash, out.baseline_breach_mask);
  hash = fold_u64(hash, out.candidate_breach_mask);
  out.input_hash = fold_u64(hash, static_cast<std::uint64_t>(out.disposition));
  return Ok(out);
}

} // namespace atx::options::risk
