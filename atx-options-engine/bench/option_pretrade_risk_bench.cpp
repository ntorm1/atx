// Pure-kernel projected/worst-fill pre-trade risk benchmarks.
//
// Treat Debug and Release results as separate regression series. Any retained
// result must include build configuration, compiler/toolchain version, CPU,
// benchmark command, and git commit; no result is a portable capacity claim.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/options/option_pretrade_risk.hpp"

namespace {

using atx::options::research::OptionInstrument;
using atx::options::risk::OptionPreTradeRiskEngine;
using atx::options::risk::OptionPreTradeRiskEvaluation;
using atx::options::risk::OptionRiskContentDigest;
using atx::options::risk::OptionRiskContractRow;
using atx::options::risk::OptionRiskDisposition;
using atx::options::risk::OptionRiskEngineLimits;
using atx::options::risk::OptionRiskHardLimits;
using atx::options::risk::OptionRiskLeaf;
using atx::options::risk::OptionRiskPanel;
using atx::options::risk::OptionRiskPanelLimits;
using atx::options::risk::OptionRiskPanelProvenance;
using atx::options::risk::OptionRiskPointMetrics;
using atx::options::risk::OptionRiskRowStatus;
using atx::options::risk::OptionRiskScenario;
using atx::options::risk::OptionRiskScenarioPnlRow;
using atx::options::risk::OptionRiskWorstFillMetrics;
using atx::vol::ArchiveContentIdentity;

constexpr std::int64_t kDecisionTsNs = 100;
constexpr std::int64_t kExpiryTsNs = 1'000'000;
constexpr std::uint64_t kFirstScenarioId = 1'001U;
constexpr double kScenarioSweepsPerEvaluation = 1.0;

[[nodiscard]] std::size_t checked_product(std::size_t left, std::size_t right) {
  if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
    throw std::length_error{"pre-trade risk benchmark fixture size overflow"};
  }
  return left * right;
}

[[nodiscard]] std::int64_t checked_add(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > (std::numeric_limits<std::int64_t>::max)() - right) ||
      (right < 0 && left < (std::numeric_limits<std::int64_t>::min)() - right)) {
    throw std::overflow_error{"pre-trade risk benchmark quantity overflow"};
  }
  return left + right;
}

[[nodiscard]] std::uint64_t magnitude(std::int64_t value) {
  if (value == (std::numeric_limits<std::int64_t>::min)()) {
    throw std::overflow_error{"pre-trade risk benchmark quantity magnitude overflow"};
  }
  return static_cast<std::uint64_t>(value < 0 ? -value : value);
}

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{100'000U + seed, 200'000U + seed,
                                static_cast<std::uint32_t>(300'000U + seed),
                                static_cast<std::uint32_t>(400'000U + seed)};
}

[[nodiscard]] OptionRiskContentDigest digest(std::uint8_t seed) noexcept {
  OptionRiskContentDigest out;
  for (std::size_t index = 0; index < out.bytes.size(); ++index) {
    out.bytes[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
  }
  return out;
}

[[nodiscard]] std::uint32_t underlier_uid(std::size_t contract_index,
                                          std::size_t underlier_count) noexcept {
  return static_cast<std::uint32_t>((contract_index % underlier_count) + 1U);
}

[[nodiscard]] double strike(std::size_t contract_index) noexcept {
  return 80.0 + static_cast<double>(contract_index % 41U);
}

[[nodiscard]] atx::vol::Side side(std::size_t contract_index) noexcept {
  return contract_index % 2U == 0U ? atx::vol::Side::Call : atx::vol::Side::Put;
}

[[nodiscard]] double signed_scale(std::size_t contract_index, std::size_t modulus,
                                  double offset) noexcept {
  const double scale = offset + static_cast<double>(contract_index % modulus);
  return contract_index % 2U == 0U ? scale : -scale;
}

[[nodiscard]] double scenario_pnl(std::size_t contract_index, std::size_t scenario_index) noexcept {
  const double amount =
      1.0 + static_cast<double>((contract_index * 3U + scenario_index * 5U) % 11U);
  return (contract_index + scenario_index * 2U) % 5U < 3U ? -amount : amount;
}

[[nodiscard]] OptionRiskContractRow make_contract_row(std::size_t contract_index,
                                                      std::size_t underlier_count) {
  const std::uint64_t contract_id = static_cast<std::uint64_t>(contract_index + 1U);
  OptionRiskContractRow row;
  row.decision_ts_ns = kDecisionTsNs;
  row.contract_id = contract_id;
  row.engine_id.id = static_cast<std::uint32_t>(contract_id);
  row.underlier_uid = underlier_uid(contract_index, underlier_count);
  row.observed_ts_ns = kDecisionTsNs - 20;
  row.available_ts_ns = kDecisionTsNs - 10;
  row.market_observed_ts_ns = kDecisionTsNs - 5;
  row.market_available_ts_ns = kDecisionTsNs;
  row.definition_available_ts_ns = kDecisionTsNs - 10;
  row.expiry_ts_ns = kExpiryTsNs;
  row.strike = strike(contract_index);
  row.side = side(contract_index);
  row.multiplier = 100.0;
  row.standard_deliverable = true;
  row.definition_source_identity = identity(5'000U + contract_id);
  row.spot_delta_cash_per_contract = signed_scale(contract_index, 7U, 1.0);
  row.spot_gamma_cash_per_contract = 1.0 + static_cast<double>(contract_index % 5U);
  row.vega_cash_per_vol_point_per_contract = 2.0 + static_cast<double>((contract_index * 2U) % 7U);
  row.theta_cash_per_day_per_contract = -(1.0 + static_cast<double>((contract_index * 3U) % 5U));
  row.vanna_cash_per_return_vol_point_per_contract = signed_scale(contract_index + 1U, 6U, 1.0);
  row.volga_cash_per_vol_point_squared_per_contract =
      contract_index % 3U == 0U ? -2.0 : 1.0 + static_cast<double>(contract_index % 4U);
  row.premium_cash_notional_per_contract = 10.0 + static_cast<double>((contract_index * 7U) % 13U);
  row.status = OptionRiskRowStatus::Ok;
  row.risk_source_identity = identity(10'000U + contract_id);
  row.surface_source_identity = identity(20'000U + contract_id);
  row.market_source_identity = identity(30'000U + contract_id);
  return row;
}

[[nodiscard]] OptionRiskPanel make_panel(std::size_t contract_count, std::size_t scenario_count,
                                         std::size_t configured_underliers) {
  const std::size_t underlier_count = (std::min)(contract_count, configured_underliers);
  const std::size_t scenario_cell_count = checked_product(contract_count, scenario_count);
  std::vector<OptionRiskContractRow> contract_rows;
  contract_rows.reserve(contract_count);
  for (std::size_t contract_index = 0; contract_index < contract_count; ++contract_index) {
    contract_rows.push_back(make_contract_row(contract_index, underlier_count));
  }

  std::vector<OptionRiskScenario> scenarios;
  scenarios.reserve(scenario_count);
  for (std::size_t scenario_index = 0; scenario_index < scenario_count; ++scenario_index) {
    scenarios.push_back(
        OptionRiskScenario{kFirstScenarioId + static_cast<std::uint64_t>(scenario_index),
                           identity(30'000U + static_cast<std::uint64_t>(scenario_index))});
  }

  std::vector<OptionRiskScenarioPnlRow> scenario_rows;
  scenario_rows.reserve(scenario_cell_count);
  for (std::size_t scenario_index = 0; scenario_index < scenario_count; ++scenario_index) {
    const std::uint64_t scenario_id = kFirstScenarioId + static_cast<std::uint64_t>(scenario_index);
    for (std::size_t contract_index = 0; contract_index < contract_count; ++contract_index) {
      const std::uint64_t contract_id = static_cast<std::uint64_t>(contract_index + 1U);
      scenario_rows.push_back(
          OptionRiskScenarioPnlRow{kDecisionTsNs, contract_id, scenario_id, kDecisionTsNs - 20,
                                   kDecisionTsNs - 10, scenario_pnl(contract_index, scenario_index),
                                   identity(40'000U + static_cast<std::uint64_t>(scenario_index))});
    }
  }

  OptionRiskPanelProvenance provenance;
  provenance.pricer_model_version = 1U;
  provenance.greek_convention_version = 1U;
  provenance.risk_snapshot_digest = digest(1U);
  provenance.scenario_manifest_digest = digest(101U);

  OptionRiskPanelLimits limits;
  limits.max_contract_rows = contract_count;
  limits.max_scenarios = scenario_count;
  limits.max_scenario_rows = scenario_cell_count;
  limits.max_workspace_bytes = 1'073'741'824U;
  auto created =
      OptionRiskPanel::create(contract_rows, scenarios, scenario_rows, provenance, limits);
  if (!created) {
    throw std::runtime_error{created.error().to_string()};
  }
  return std::move(*created);
}

[[nodiscard]] OptionPreTradeRiskEngine
make_engine(std::size_t contract_count, std::size_t scenario_count, std::size_t live_leaf_count,
            std::size_t candidate_leaf_count, std::size_t underlier_count) {
  OptionRiskEngineLimits limits;
  limits.max_contracts = contract_count;
  limits.max_live_leaves = (std::max)(live_leaf_count, std::size_t{1U});
  limits.max_candidate_leaves = (std::max)(candidate_leaf_count, std::size_t{1U});
  limits.max_scenarios = scenario_count;
  limits.max_underliers = underlier_count;
  limits.max_workspace_bytes = 1'073'741'824U;
  auto created = OptionPreTradeRiskEngine::create(limits);
  if (!created) {
    throw std::runtime_error{created.error().to_string()};
  }
  return std::move(*created);
}

[[nodiscard]] std::vector<OptionInstrument> make_catalog(std::size_t contract_count,
                                                         std::size_t configured_underliers) {
  const std::size_t underlier_count = (std::min)(contract_count, configured_underliers);
  std::vector<OptionInstrument> catalog;
  catalog.reserve(contract_count);
  for (std::size_t contract_index = 0; contract_index < contract_count; ++contract_index) {
    const std::uint64_t contract_id = static_cast<std::uint64_t>(contract_index + 1U);
    OptionInstrument instrument;
    instrument.contract_id = contract_id;
    instrument.underlier_uid = underlier_uid(contract_index, underlier_count);
    instrument.expiry_ts_ns = kExpiryTsNs;
    instrument.strike = strike(contract_index);
    instrument.side = side(contract_index);
    instrument.multiplier = 100.0;
    instrument.standard_deliverable = true;
    instrument.engine_id.id = static_cast<std::uint32_t>(contract_id);
    catalog.push_back(instrument);
  }
  return catalog;
}

[[nodiscard]] std::vector<std::uint32_t> root_uids(const OptionRiskPanel &panel) {
  std::vector<std::uint32_t> roots(panel.underlier_uids().begin(), panel.underlier_uids().end());
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

[[nodiscard]] std::size_t root_index(std::span<const std::uint32_t> roots,
                                     std::uint32_t uid) noexcept {
  return static_cast<std::size_t>(std::lower_bound(roots.begin(), roots.end(), uid) -
                                  roots.begin());
}

void update_point_scenario_extrema(OptionRiskPointMetrics &out, std::uint64_t scenario_id,
                                   std::span<const std::uint32_t> roots,
                                   std::span<const double> root_pnl) {
  double scenario_loss = 0.0;
  double max_root_loss = 0.0;
  std::uint32_t max_root_uid = roots.front();
  for (std::size_t root = 0; root < roots.size(); ++root) {
    const double loss = (std::max)(0.0, -root_pnl[root]);
    scenario_loss += loss;
    if (loss > max_root_loss || (loss == max_root_loss && roots[root] < max_root_uid)) {
      max_root_loss = loss;
      max_root_uid = roots[root];
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
}

void update_worst_scenario_extrema(OptionRiskWorstFillMetrics &out, std::uint64_t scenario_id,
                                   std::span<const std::uint32_t> roots,
                                   std::span<const double> root_pnl) {
  double scenario_loss = 0.0;
  double max_root_loss = 0.0;
  std::uint32_t max_root_uid = roots.front();
  for (std::size_t root = 0; root < roots.size(); ++root) {
    const double loss = (std::max)(0.0, -root_pnl[root]);
    scenario_loss += loss;
    if (loss > max_root_loss || (loss == max_root_loss && roots[root] < max_root_uid)) {
      max_root_loss = loss;
      max_root_uid = roots[root];
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
}

[[nodiscard]] OptionRiskPointMetrics reference_point(const OptionRiskPanel &panel,
                                                     std::span<const std::int64_t> quantities) {
  OptionRiskPointMetrics out;
  for (std::size_t contract = 0; contract < quantities.size(); ++contract) {
    const OptionRiskContractRow &row = panel.contract_row(0U, contract);
    const double quantity = static_cast<double>(quantities[contract]);
    const double gamma = quantity * row.spot_gamma_cash_per_contract;
    const double vega = quantity * row.vega_cash_per_vol_point_per_contract;
    const double vanna = quantity * row.vanna_cash_per_return_vol_point_per_contract;
    const double volga = quantity * row.volga_cash_per_vol_point_squared_per_contract;
    out.spot_delta_cash += quantity * row.spot_delta_cash_per_contract;
    out.spot_gamma_cash += gamma;
    out.vega_cash_per_vol_point += vega;
    out.theta_cash_per_day += quantity * row.theta_cash_per_day_per_contract;
    out.vanna_cash_per_return_vol_point += vanna;
    out.volga_cash_per_vol_point_squared += volga;
    out.gross_spot_gamma_cash += std::abs(gamma);
    out.gross_vega_cash_per_vol_point += std::abs(vega);
    out.gross_vanna_cash_per_return_vol_point += std::abs(vanna);
    out.gross_volga_cash_per_vol_point_squared += std::abs(volga);
    out.gross_premium_cash_notional += std::abs(quantity) * row.premium_cash_notional_per_contract;
  }

  const std::vector<std::uint32_t> roots = root_uids(panel);
  std::vector<double> root_pnl(roots.size());
  out.worst_scenario_id = panel.scenario_ids().front();
  out.worst_underlier_scenario_id = panel.scenario_ids().front();
  out.worst_underlier_uid = roots.front();
  for (std::size_t scenario = 0; scenario < panel.scenario_count(); ++scenario) {
    std::fill(root_pnl.begin(), root_pnl.end(), 0.0);
    for (std::size_t contract = 0; contract < quantities.size(); ++contract) {
      root_pnl[root_index(roots, panel.underlier_uids()[contract])] +=
          static_cast<double>(quantities[contract]) * panel.scenario_pnl(0U, scenario, contract);
    }
    update_point_scenario_extrema(out, panel.scenario_ids()[scenario], roots, root_pnl);
  }
  return out;
}

struct QuantityEnvelope {
  std::vector<std::int64_t> low;
  std::vector<std::int64_t> high;
  std::vector<std::int64_t> projected;
  std::uint64_t open_order_contracts{0};
};

void add_leaves(QuantityEnvelope &out, std::span<const OptionRiskLeaf> leaves) {
  for (const OptionRiskLeaf &leaf : leaves) {
    out.open_order_contracts += magnitude(leaf.remaining_contracts);
    out.projected[leaf.contract_index] =
        checked_add(out.projected[leaf.contract_index], leaf.remaining_contracts);
    if (leaf.remaining_contracts < 0) {
      out.low[leaf.contract_index] =
          checked_add(out.low[leaf.contract_index], leaf.remaining_contracts);
    } else {
      out.high[leaf.contract_index] =
          checked_add(out.high[leaf.contract_index], leaf.remaining_contracts);
    }
  }
}

[[nodiscard]] QuantityEnvelope make_envelope(std::span<const std::int64_t> filled,
                                             std::span<const OptionRiskLeaf> leaves) {
  QuantityEnvelope out;
  out.low.assign(filled.begin(), filled.end());
  out.high.assign(filled.begin(), filled.end());
  out.projected.assign(filled.begin(), filled.end());
  add_leaves(out, leaves);
  return out;
}

void accumulate_worst_greek(std::int64_t low_quantity, std::int64_t high_quantity,
                            double coefficient, double &low_total, double &high_total,
                            double &gross_total) {
  double low = static_cast<double>(low_quantity) * coefficient;
  double high = static_cast<double>(high_quantity) * coefficient;
  if (low > high) {
    std::swap(low, high);
  }
  low_total += low;
  high_total += high;
  gross_total += (std::max)(std::abs(low), std::abs(high));
}

[[nodiscard]] OptionRiskWorstFillMetrics reference_worst(const OptionRiskPanel &panel,
                                                         std::span<const std::int64_t> filled,
                                                         const QuantityEnvelope &envelope) {
  OptionRiskWorstFillMetrics out;
  out.open_order_contracts = envelope.open_order_contracts;
  double delta_low = 0.0;
  double delta_high = 0.0;
  double ignored_delta_gross = 0.0;
  double gamma_low = 0.0;
  double gamma_high = 0.0;
  double vega_low = 0.0;
  double vega_high = 0.0;
  double theta_low = 0.0;
  double theta_high = 0.0;
  double ignored_theta_gross = 0.0;
  double vanna_low = 0.0;
  double vanna_high = 0.0;
  double volga_low = 0.0;
  double volga_high = 0.0;
  for (std::size_t contract = 0; contract < filled.size(); ++contract) {
    const OptionRiskContractRow &row = panel.contract_row(0U, contract);
    accumulate_worst_greek(envelope.low[contract], envelope.high[contract],
                           row.spot_delta_cash_per_contract, delta_low, delta_high,
                           ignored_delta_gross);
    accumulate_worst_greek(envelope.low[contract], envelope.high[contract],
                           row.spot_gamma_cash_per_contract, gamma_low, gamma_high,
                           out.max_gross_spot_gamma_cash);
    accumulate_worst_greek(envelope.low[contract], envelope.high[contract],
                           row.vega_cash_per_vol_point_per_contract, vega_low, vega_high,
                           out.max_gross_vega_cash_per_vol_point);
    accumulate_worst_greek(envelope.low[contract], envelope.high[contract],
                           row.theta_cash_per_day_per_contract, theta_low, theta_high,
                           ignored_theta_gross);
    accumulate_worst_greek(envelope.low[contract], envelope.high[contract],
                           row.vanna_cash_per_return_vol_point_per_contract, vanna_low, vanna_high,
                           out.max_gross_vanna_cash_per_return_vol_point);
    accumulate_worst_greek(envelope.low[contract], envelope.high[contract],
                           row.volga_cash_per_vol_point_squared_per_contract, volga_low, volga_high,
                           out.max_gross_volga_cash_per_vol_point_squared);
    const std::uint64_t largest_quantity =
        (std::max)(magnitude(envelope.low[contract]), magnitude(envelope.high[contract]));
    out.max_gross_premium_cash_notional +=
        static_cast<double>(largest_quantity) * row.premium_cash_notional_per_contract;
  }
  out.max_abs_spot_delta_cash = (std::max)(std::abs(delta_low), std::abs(delta_high));
  out.max_abs_spot_gamma_cash = (std::max)(std::abs(gamma_low), std::abs(gamma_high));
  out.max_abs_vega_cash_per_vol_point = (std::max)(std::abs(vega_low), std::abs(vega_high));
  out.max_abs_theta_cash_per_day = (std::max)(std::abs(theta_low), std::abs(theta_high));
  out.max_abs_vanna_cash_per_return_vol_point =
      (std::max)(std::abs(vanna_low), std::abs(vanna_high));
  out.max_abs_volga_cash_per_vol_point_squared =
      (std::max)(std::abs(volga_low), std::abs(volga_high));

  const std::vector<std::uint32_t> roots = root_uids(panel);
  std::vector<double> root_pnl(roots.size());
  out.worst_scenario_id = panel.scenario_ids().front();
  out.worst_underlier_scenario_id = panel.scenario_ids().front();
  out.worst_underlier_uid = roots.front();
  for (std::size_t scenario = 0; scenario < panel.scenario_count(); ++scenario) {
    std::fill(root_pnl.begin(), root_pnl.end(), 0.0);
    for (std::size_t contract = 0; contract < filled.size(); ++contract) {
      const double pnl = panel.scenario_pnl(0U, scenario, contract);
      const double no_fill = static_cast<double>(filled[contract]) * pnl;
      const double low_fill = static_cast<double>(envelope.low[contract]) * pnl;
      const double high_fill = static_cast<double>(envelope.high[contract]) * pnl;
      root_pnl[root_index(roots, panel.underlier_uids()[contract])] +=
          (std::min)({no_fill, low_fill, high_fill});
    }
    update_worst_scenario_extrema(out, panel.scenario_ids()[scenario], roots, root_pnl);
  }
  return out;
}

struct RiskFixture {
  std::size_t contract_count{0};
  std::size_t scenario_count{0};
  std::size_t underlier_count{0};
  OptionRiskPanel panel;
  OptionPreTradeRiskEngine engine;
  std::vector<OptionInstrument> catalog;
  std::vector<std::int64_t> filled_contracts;
  std::vector<OptionRiskLeaf> live_leaves;
  std::vector<OptionRiskLeaf> candidate_leaves;
  OptionRiskHardLimits hard_limits{};
  OptionRiskPointMetrics expected_filled{};
  OptionRiskPointMetrics expected_baseline_projected{};
  OptionRiskWorstFillMetrics expected_baseline_worst{};
  OptionRiskPointMetrics expected_candidate_projected{};
  OptionRiskWorstFillMetrics expected_candidate_worst{};

  RiskFixture(std::size_t configured_contracts, std::size_t configured_scenarios,
              std::size_t live_leaves_per_contract, std::size_t candidate_leaves_per_contract,
              std::size_t configured_underliers)
      : contract_count{configured_contracts}, scenario_count{configured_scenarios},
        underlier_count{(std::min)(configured_contracts, configured_underliers)},
        panel{make_panel(configured_contracts, configured_scenarios, configured_underliers)},
        engine{make_engine(configured_contracts, configured_scenarios,
                           checked_product(configured_contracts, live_leaves_per_contract),
                           checked_product(configured_contracts, candidate_leaves_per_contract),
                           underlier_count)},
        catalog{make_catalog(configured_contracts, configured_underliers)},
        filled_contracts(configured_contracts, 1) {
    live_leaves.reserve(checked_product(contract_count, live_leaves_per_contract));
    candidate_leaves.reserve(checked_product(contract_count, candidate_leaves_per_contract));
    for (std::size_t contract = 0; contract < contract_count; ++contract) {
      for (std::size_t leaf = 0; leaf < live_leaves_per_contract; ++leaf) {
        const std::int64_t quantity = (contract + leaf) % 2U == 0U ? 1 : -1;
        live_leaves.push_back(OptionRiskLeaf{contract, quantity});
      }
      for (std::size_t leaf = 0; leaf < candidate_leaves_per_contract; ++leaf) {
        const std::int64_t quantity = (contract + leaf + 1U) % 2U == 0U ? 1 : -1;
        candidate_leaves.push_back(OptionRiskLeaf{contract, quantity});
      }
    }

    const QuantityEnvelope baseline = make_envelope(filled_contracts, live_leaves);
    QuantityEnvelope candidate = baseline;
    add_leaves(candidate, candidate_leaves);
    expected_filled = reference_point(panel, filled_contracts);
    expected_baseline_projected = reference_point(panel, baseline.projected);
    expected_baseline_worst = reference_worst(panel, filled_contracts, baseline);
    expected_candidate_projected = reference_point(panel, candidate.projected);
    expected_candidate_worst = reference_worst(panel, filled_contracts, candidate);
  }

  [[nodiscard]] atx::core::Result<OptionPreTradeRiskEvaluation> evaluate() {
    return engine.evaluate(panel, 0U, catalog, filled_contracts, live_leaves, candidate_leaves,
                           hard_limits);
  }

  [[nodiscard]] bool valid(const OptionPreTradeRiskEvaluation &evaluation) const noexcept {
    return evaluation.disposition == OptionRiskDisposition::Accept &&
           evaluation.baseline_breach_mask == 0U && evaluation.candidate_breach_mask == 0U &&
           evaluation.filled == expected_filled &&
           evaluation.baseline_projected == expected_baseline_projected &&
           evaluation.baseline_worst_fill == expected_baseline_worst &&
           evaluation.candidate_projected == expected_candidate_projected &&
           evaluation.candidate_worst_fill == expected_candidate_worst;
  }
};

void run_risk_benchmark(benchmark::State &state, std::size_t contract_count,
                        std::size_t scenario_count, std::size_t live_leaves_per_contract,
                        std::size_t candidate_leaves_per_contract, std::size_t underlier_count) {
  RiskFixture fixture{contract_count, scenario_count, live_leaves_per_contract,
                      candidate_leaves_per_contract, underlier_count};
  const auto verified = fixture.evaluate();
  if (!verified || !fixture.valid(*verified)) {
    const std::string error = verified ? "pre-trade risk benchmark metrics/disposition mismatch"
                                       : verified.error().to_string();
    state.SkipWithError(error.c_str());
    return;
  }

  for (auto _ : state) {
    static_cast<void>(_);
    auto result = fixture.evaluate();
    if (!result) {
      state.SkipWithError(result.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(*result);
    benchmark::ClobberMemory();
  }

  if (!state.skipped()) {
    const auto repeated = fixture.evaluate();
    if (!repeated || !fixture.valid(*repeated) || repeated->input_hash != verified->input_hash) {
      const std::string error = repeated ? "pre-trade risk benchmark repeat-state validation failed"
                                         : repeated.error().to_string();
      state.SkipWithError(error.c_str());
    }
  }

  const double iterations = static_cast<double>(state.iterations());
  const double scenario_contract_cells = iterations * kScenarioSweepsPerEvaluation *
                                         static_cast<double>(contract_count) *
                                         static_cast<double>(scenario_count);
  const double active_leaves =
      static_cast<double>(fixture.live_leaves.size() + fixture.candidate_leaves.size());
  state.counters["risk_evaluations_per_second"] =
      benchmark::Counter(iterations, benchmark::Counter::kIsRate);
  state.counters["scenario_contract_cells_per_second"] =
      benchmark::Counter(scenario_contract_cells, benchmark::Counter::kIsRate);
  state.counters["active_leaves_per_second"] =
      benchmark::Counter(iterations * active_leaves, benchmark::Counter::kIsRate);
  state.counters["contracts"] = benchmark::Counter(static_cast<double>(contract_count));
  state.counters["scenarios"] = benchmark::Counter(static_cast<double>(scenario_count));
  state.counters["underliers"] = benchmark::Counter(static_cast<double>(fixture.underlier_count));
  state.counters["live_leaves"] =
      benchmark::Counter(static_cast<double>(fixture.live_leaves.size()));
  state.counters["candidate_leaves"] =
      benchmark::Counter(static_cast<double>(fixture.candidate_leaves.size()));
}

void BM_OptionPreTradeRiskScenarioFanOut(benchmark::State &state) {
  run_risk_benchmark(state, 512U, static_cast<std::size_t>(state.range(0)), 1U, 1U, 64U);
}

BENCHMARK(BM_OptionPreTradeRiskScenarioFanOut)->Arg(1)->Arg(8)->Arg(32)->Arg(128);

void BM_OptionPreTradeRiskCatalogFanOut(benchmark::State &state) {
  const std::size_t contracts = static_cast<std::size_t>(state.range(0));
  run_risk_benchmark(state, contracts, 16U, 1U, 1U, (std::min)(contracts, std::size_t{257U}));
}

BENCHMARK(BM_OptionPreTradeRiskCatalogFanOut)->Arg(127)->Arg(1'023)->Arg(4'097);

void BM_OptionPreTradeRiskActiveLeafFanOut(benchmark::State &state) {
  const std::size_t leaves_per_side = static_cast<std::size_t>(state.range(0));
  run_risk_benchmark(state, 512U, 16U, leaves_per_side, leaves_per_side, 64U);
}

BENCHMARK(BM_OptionPreTradeRiskActiveLeafFanOut)->Arg(1)->Arg(4)->Arg(16);

void BM_OptionPreTradeRiskRootFanOut(benchmark::State &state) {
  run_risk_benchmark(state, 2'048U, 32U, 1U, 1U, static_cast<std::size_t>(state.range(0)));
}

BENCHMARK(BM_OptionPreTradeRiskRootFanOut)->Arg(1)->Arg(16)->Arg(256)->Arg(2'048);

void BM_OptionPreTradeRiskMemoryBandwidth(benchmark::State &state) {
  run_risk_benchmark(state, static_cast<std::size_t>(state.range(0)),
                     static_cast<std::size_t>(state.range(1)), 1U, 1U, 1'024U);
}

// The larger P&L cube is 32 MiB, deliberately exceeding a typical desktop LLC
// while remaining far below the panel's bounded workspace budget.
BENCHMARK(BM_OptionPreTradeRiskMemoryBandwidth)->Args({4'096, 256})->Args({8'192, 512});

} // namespace
