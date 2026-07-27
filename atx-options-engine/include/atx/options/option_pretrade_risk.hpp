#pragma once

// Point-in-time full-reprice risk evidence and a bounded pre-trade options
// envelope. Scenario P&L is authoritative. Greek measures are separately
// unit-named limits and diagnostics; they are not used as a tail-repricing
// substitute.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/options/option_research_panel.hpp"
#include "atx/vol/research_validation.hpp"

namespace atx::options::risk {

inline constexpr std::uint64_t kOptionPreTradeRiskModelVersion =
    0x4154584F50520102ULL; // "ATXOPR", revision 1.2
inline constexpr std::uint64_t kOptionPreTradeRiskOrderingVersion = 2U;

// Caller-attested 256-bit artifact digest. OptionRiskPanel::create rejects an
// all-zero value but deliberately does not recompute or verify it; trusted
// loaders must verify source bytes before constructing the panel.
struct OptionRiskContentDigest {
  std::array<std::uint8_t, 32> bytes{};

  [[nodiscard]] bool operator==(const OptionRiskContentDigest &) const noexcept = default;
};

enum class OptionRiskRowStatus : std::uint8_t {
  Ok = 0,
  MissingMarket = 1,
  StaleMarket = 2,
  ModelUnavailable = 3,
  UnsupportedContract = 4,
};

// One point-in-time, per-long-contract Greek snapshot. Every cash Greek is
// already multiplied by the listed-contract multiplier.
struct OptionRiskContractRow {
  std::int64_t decision_ts_ns{0};
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::uint32_t underlier_uid{0};
  std::int64_t observed_ts_ns{0};
  std::int64_t available_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  atx::vol::Side side{atx::vol::Side::Call};
  double multiplier{0.0};
  bool standard_deliverable{true};
  atx::vol::ArchiveContentIdentity definition_source_identity{};

  // Dollars for a +100% proportional spot move under the local first- and
  // second-derivative conventions. Gamma excludes the one-half P&L factor.
  double spot_delta_cash_per_contract{0.0};
  double spot_gamma_cash_per_contract{0.0};
  // Dollars per one absolute implied-volatility percentage point and per
  // calendar day, respectively.
  double vega_cash_per_vol_point_per_contract{0.0};
  double theta_cash_per_day_per_contract{0.0};
  // Dollars for a 100% spot return crossed with one volatility point, and the
  // raw second volatility derivative per squared volatility point. Volga
  // excludes the one-half P&L factor.
  double vanna_cash_per_return_vol_point_per_contract{0.0};
  double volga_cash_per_vol_point_squared_per_contract{0.0};
  // Absolute base mark * multiplier for one listed contract.
  double premium_cash_notional_per_contract{0.0};

  OptionRiskRowStatus status{OptionRiskRowStatus::Ok};
  atx::vol::ArchiveContentIdentity risk_source_identity{};
  atx::vol::ArchiveContentIdentity surface_source_identity{};

  [[nodiscard]] bool operator==(const OptionRiskContractRow &) const noexcept = default;
};

struct OptionRiskScenario {
  std::uint64_t scenario_id{0};
  atx::vol::ArchiveContentIdentity source_identity{};

  [[nodiscard]] bool operator==(const OptionRiskScenario &) const noexcept = default;
};

// Authoritative shocked-value-minus-base-value dollars for one long listed
// contract. The upstream producer performs full repricing.
struct OptionRiskScenarioPnlRow {
  std::int64_t decision_ts_ns{0};
  std::uint64_t contract_id{0};
  std::uint64_t scenario_id{0};
  std::int64_t observed_ts_ns{0};
  std::int64_t available_ts_ns{0};
  double pnl_per_long_contract{0.0};
  atx::vol::ArchiveContentIdentity source_identity{};

  [[nodiscard]] bool operator==(const OptionRiskScenarioPnlRow &) const noexcept = default;
};

struct OptionRiskPanelProvenance {
  std::uint64_t pricer_model_version{0};
  std::uint64_t greek_convention_version{0};
  OptionRiskContentDigest risk_snapshot_digest{};
  OptionRiskContentDigest scenario_manifest_digest{};

  [[nodiscard]] bool operator==(const OptionRiskPanelProvenance &) const noexcept = default;
};

struct OptionRiskPanelLimits {
  std::size_t max_contract_rows{5'000'000};
  std::size_t max_scenarios{1'024};
  std::size_t max_scenario_rows{100'000'000};
  // Exact retained payload of canonical contract rows, scenario P&L values,
  // dates, contract IDs, engine IDs, underlier IDs, and scenario IDs.
  // Cold-path canonicalization scratch and container/allocator metadata are
  // excluded.
  std::size_t max_workspace_bytes{4'294'967'296ULL}; // 4 GiB
};

// Immutable canonical date x contract risk sidecar with a dense
// date x scenario x contract full-reprice P&L cube. Concurrent const reads are
// safe after create() returns.
class OptionRiskPanel {
public:
  // Canonicalizes input permutations and returns a typed error for invalid,
  // incomplete, nonfinite, over-capacity, or allocation-failing inputs.
  [[nodiscard]] static atx::core::Result<OptionRiskPanel>
  create(std::span<const OptionRiskContractRow> contract_rows,
         std::span<const OptionRiskScenario> scenarios,
         std::span<const OptionRiskScenarioPnlRow> scenario_pnl_rows,
         const OptionRiskPanelProvenance &provenance, OptionRiskPanelLimits limits = {});

  [[nodiscard]] std::span<const std::int64_t> dates() const noexcept { return dates_; }
  [[nodiscard]] std::span<const std::uint64_t> contract_ids() const noexcept {
    return contract_ids_;
  }
  [[nodiscard]] std::span<const atx::engine::InstrumentId> engine_ids() const noexcept {
    return engine_ids_;
  }
  [[nodiscard]] std::span<const std::uint32_t> underlier_uids() const noexcept {
    return underlier_uids_;
  }
  [[nodiscard]] std::span<const std::uint64_t> scenario_ids() const noexcept {
    return scenario_ids_;
  }
  [[nodiscard]] std::size_t contract_count() const noexcept { return contract_ids_.size(); }
  [[nodiscard]] std::size_t scenario_count() const noexcept { return scenario_ids_.size(); }
  // Preconditions: date_index < dates().size() and
  // contract_index < contract_count().
  [[nodiscard]] const OptionRiskContractRow &
  contract_row(std::size_t date_index, std::size_t contract_index) const noexcept {
    return contract_rows_[date_index * contract_count() + contract_index];
  }
  // Preconditions: date_index < dates().size(),
  // scenario_index < scenario_count(), and
  // contract_index < contract_count().
  [[nodiscard]] double scenario_pnl(std::size_t date_index, std::size_t scenario_index,
                                    std::size_t contract_index) const noexcept {
    return scenario_pnl_[(date_index * scenario_count() + scenario_index) * contract_count() +
                         contract_index];
  }
  [[nodiscard]] const OptionRiskPanelProvenance &provenance() const noexcept { return provenance_; }
  // Deterministic non-cryptographic regression fingerprint. The two supplied
  // 256-bit digests are caller-attested artifact-provenance fields; create()
  // rejects zero digests but does not recompute or cryptographically verify
  // them.
  [[nodiscard]] std::uint64_t definition_hash() const noexcept { return definition_hash_; }

private:
  OptionRiskPanel(std::vector<std::int64_t> dates, std::vector<std::uint64_t> contract_ids,
                  std::vector<atx::engine::InstrumentId> engine_ids,
                  std::vector<std::uint32_t> underlier_uids,
                  std::vector<std::uint64_t> scenario_ids,
                  std::vector<OptionRiskContractRow> contract_rows,
                  std::vector<double> scenario_pnl, OptionRiskPanelProvenance provenance,
                  std::uint64_t definition_hash) noexcept;

  std::vector<std::int64_t> dates_;
  std::vector<std::uint64_t> contract_ids_;
  std::vector<atx::engine::InstrumentId> engine_ids_;
  std::vector<std::uint32_t> underlier_uids_;
  std::vector<std::uint64_t> scenario_ids_;
  std::vector<OptionRiskContractRow> contract_rows_;
  std::vector<double> scenario_pnl_;
  OptionRiskPanelProvenance provenance_{};
  std::uint64_t definition_hash_{0};
};

struct OptionRiskLeaf {
  std::size_t contract_index{0};
  std::int64_t remaining_contracts{0};

  [[nodiscard]] bool operator==(const OptionRiskLeaf &) const noexcept = default;
};

struct OptionRiskEngineLimits {
  std::size_t max_contracts{100'000};
  std::size_t max_live_leaves{100'000};
  std::size_t max_candidate_leaves{100'000};
  std::size_t max_scenarios{1'024};
  std::size_t max_underliers{100'000};
  // Exact engine-owned vector payload bound. The immutable OptionRiskPanel is
  // externally owned and excluded.
  std::size_t max_workspace_bytes{1'073'741'824}; // 1 GiB
};

struct OptionRiskHardLimits {
  // Gross absolute remaining quantity across active order leaves only. Filled
  // positions are governed by the Greek, premium, and scenario limits.
  std::uint64_t max_open_order_contracts{(std::numeric_limits<std::uint64_t>::max)()};
  double max_abs_spot_delta_cash{(std::numeric_limits<double>::max)()};
  double max_abs_spot_gamma_cash{(std::numeric_limits<double>::max)()};
  double max_abs_vega_cash_per_vol_point{(std::numeric_limits<double>::max)()};
  double max_abs_theta_cash_per_day{(std::numeric_limits<double>::max)()};
  double max_abs_vanna_cash_per_return_vol_point{(std::numeric_limits<double>::max)()};
  double max_abs_volga_cash_per_vol_point_squared{(std::numeric_limits<double>::max)()};
  double max_gross_spot_gamma_cash{(std::numeric_limits<double>::max)()};
  double max_gross_vega_cash_per_vol_point{(std::numeric_limits<double>::max)()};
  double max_gross_vanna_cash_per_return_vol_point{(std::numeric_limits<double>::max)()};
  double max_gross_volga_cash_per_vol_point_squared{(std::numeric_limits<double>::max)()};
  double max_gross_premium_cash_notional{(std::numeric_limits<double>::max)()};
  double max_scenario_loss{(std::numeric_limits<double>::max)()};
  double max_single_underlier_scenario_loss{(std::numeric_limits<double>::max)()};
};

enum class OptionRiskBreach : std::uint32_t {
  None = 0U,
  OpenOrderContracts = 1U << 0U,
  SpotDelta = 1U << 1U,
  SpotGamma = 1U << 2U,
  Vega = 1U << 3U,
  Theta = 1U << 4U,
  Vanna = 1U << 5U,
  Volga = 1U << 6U,
  GrossGamma = 1U << 7U,
  GrossVega = 1U << 8U,
  GrossVanna = 1U << 9U,
  GrossVolga = 1U << 10U,
  GrossPremium = 1U << 11U,
  ScenarioLoss = 1U << 12U,
  UnderlierScenarioLoss = 1U << 13U,
};

enum class OptionRiskDisposition : std::uint8_t {
  Accept = 0,
  ReduceOnlyAccept = 1,
  CancelOnly = 2,
  RejectNewOrders = 3,
};

struct OptionRiskPointMetrics {
  double spot_delta_cash{0.0};
  double spot_gamma_cash{0.0};
  double vega_cash_per_vol_point{0.0};
  double theta_cash_per_day{0.0};
  double vanna_cash_per_return_vol_point{0.0};
  double volga_cash_per_vol_point_squared{0.0};
  double gross_spot_gamma_cash{0.0};
  double gross_vega_cash_per_vol_point{0.0};
  double gross_vanna_cash_per_return_vol_point{0.0};
  double gross_volga_cash_per_vol_point_squared{0.0};
  double gross_premium_cash_notional{0.0};
  double scenario_loss{0.0};
  double max_single_underlier_scenario_loss{0.0};
  std::uint64_t worst_scenario_id{0};
  std::uint64_t worst_underlier_scenario_id{0};
  std::uint32_t worst_underlier_uid{0};

  [[nodiscard]] bool operator==(const OptionRiskPointMetrics &) const noexcept = default;
};

struct OptionRiskWorstFillMetrics {
  // Gross absolute remaining quantity across active order leaves.
  std::uint64_t open_order_contracts{0};
  double max_abs_spot_delta_cash{0.0};
  double max_abs_spot_gamma_cash{0.0};
  double max_abs_vega_cash_per_vol_point{0.0};
  double max_abs_theta_cash_per_day{0.0};
  double max_abs_vanna_cash_per_return_vol_point{0.0};
  double max_abs_volga_cash_per_vol_point_squared{0.0};
  double max_gross_spot_gamma_cash{0.0};
  double max_gross_vega_cash_per_vol_point{0.0};
  double max_gross_vanna_cash_per_return_vol_point{0.0};
  double max_gross_volga_cash_per_vol_point_squared{0.0};
  double max_gross_premium_cash_notional{0.0};
  double scenario_loss{0.0};
  double max_single_underlier_scenario_loss{0.0};
  std::uint64_t worst_scenario_id{0};
  std::uint64_t worst_underlier_scenario_id{0};
  std::uint32_t worst_underlier_uid{0};

  [[nodiscard]] bool operator==(const OptionRiskWorstFillMetrics &) const noexcept = default;
};

struct OptionPreTradeRiskEvaluation {
  OptionRiskPointMetrics filled{};
  OptionRiskPointMetrics baseline_projected{};
  OptionRiskWorstFillMetrics baseline_worst_fill{};
  OptionRiskPointMetrics candidate_projected{};
  OptionRiskWorstFillMetrics candidate_worst_fill{};
  OptionRiskDisposition disposition{OptionRiskDisposition::RejectNewOrders};
  std::uint32_t baseline_breach_mask{0};
  std::uint32_t candidate_breach_mask{0};
  std::uint64_t input_hash{0};

  [[nodiscard]] bool operator==(const OptionPreTradeRiskEvaluation &) const noexcept = default;
};

[[nodiscard]] atx::core::Result<std::size_t>
option_pretrade_risk_required_workspace_bytes(const OptionRiskEngineLimits &limits);

// Reusable, bounded risk reducer. create() performs all allocation and returns
// a typed error on invalid capacities or allocation failure. A successful
// evaluate() performs no allocation. Existing and candidate leaves are
// individual signed remaining quantities; callers must include scheduled,
// working, partially filled, and pending-cancel leaves.
//
// The contract catalog must have been provenance-validated against each risk
// row's definition_source_identity before direct use; OptionAdaptiveCoordinator
// performs this check. OptionRiskRowStatus::Ok is the upstream freshness
// attestation. This reducer validates point-in-time ordering but intentionally
// has no independent maximum-age policy.
//
// Finite binary64 aggregates are authoritative: a limit is inclusive, direct
// comparisons use the rounded aggregate, and no tolerance or implicit haircut
// is applied. A caller requiring conservative boundary treatment must supply
// appropriately haircutted limits.
//
// evaluate() returns a typed error for catalog/date misalignment, bad status,
// nonfinite or overflowing arithmetic, inexact integer-to-binary64 quantities,
// or capacity violations. Returned evaluations own their values and borrow
// nothing from input spans. One instance is not safe for concurrent evaluate()
// calls; use an engine per execution lane.
class OptionPreTradeRiskEngine {
public:
  [[nodiscard]] static atx::core::Result<OptionPreTradeRiskEngine>
  create(OptionRiskEngineLimits limits = {});

  ~OptionPreTradeRiskEngine();
  OptionPreTradeRiskEngine(OptionPreTradeRiskEngine &&) noexcept;
  OptionPreTradeRiskEngine &operator=(OptionPreTradeRiskEngine &&) noexcept;
  OptionPreTradeRiskEngine(const OptionPreTradeRiskEngine &) = delete;
  OptionPreTradeRiskEngine &operator=(const OptionPreTradeRiskEngine &) = delete;

  [[nodiscard]] atx::core::Result<OptionPreTradeRiskEvaluation> evaluate(
      const OptionRiskPanel &risk_panel, std::size_t date_index,
      std::span<const research::OptionInstrument> contract_catalog,
      std::span<const std::int64_t> filled_contracts, std::span<const OptionRiskLeaf> live_leaves,
      std::span<const OptionRiskLeaf> candidate_leaves, const OptionRiskHardLimits &hard_limits);

private:
  struct Impl;
  explicit OptionPreTradeRiskEngine(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::options::risk
