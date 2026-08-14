#pragma once

// Point-in-time listed-options bridge into the generic atx-engine.
//
// This layer owns no pricing, signal transformation, optimizer, execution
// simulation, or portfolio accounting. It makes the option-domain contracts
// required by those existing engine stages explicit:
//
//   immutable PIT rows -> canonical date x contract Dataset
//                      -> whole-contract target book
//
// Contract quantities are always listed contracts. Premium exposure is
// mark * multiplier; vega is supplied per listed contract; displayed size,
// interval volume, ADV, and margin are all in contracts. Mixing share and
// contract units at this boundary is rejected by construction.

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/vol/api/backtest/research_validation.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::options::research {

inline constexpr std::uint64_t kOptionResearchPanelSchemaSalt =
    0x4154584F50540002ULL; // "ATXOPT", revision 2

// Non-Tradable values are explicit drop reasons, never encoded by a zero price.
enum class OptionPanelStatus : std::uint8_t {
  Tradable = 0,
  MissingQuote = 1,
  StaleQuote = 2,
  CrossedMarket = 3,
  MissingLiquidity = 4,
  MissingRisk = 5,
  MissingMargin = 6,
  UnsupportedContract = 7,
};

// One sparse contract observation at one decision timestamp.
//
// The ResearchObservation supplies feature/outcome clocks and the realized
// +1-contract outcome. Its source_identity is the outcome-label lineage.
// Separate identities below attest definition, feature, and execution inputs.
//
// Clock contract for Tradable rows:
//   definition_available <= quote_event <= quote_available <= decision
//   observed <= available <= decision < execution < label_end
//
// A decision may observe a quote at decision time, but execution remains
// strictly later through ResearchObservation::execution_ts_ns and the generic
// ExecutionSimulator's default next-slice firewall.
struct OptionPanelRow {
  atx::vol::ResearchObservation observation{};

  // Permanent caller-supplied contract identity. It must not be a daily
  // publisher instrument id that can be remapped or reused.
  std::uint64_t contract_id{0};
  // Caller-owned SymbolTable identity for the same contract. The adapter never
  // fabricates a private Symbol namespace.
  atx::engine::InstrumentId engine_id{};
  std::int64_t definition_available_ts_ns{0};
  std::int64_t quote_event_ts_ns{0};
  std::int64_t quote_available_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  atx::vol::Side side{atx::vol::Side::Call};
  atx::vol::ExerciseStyle exercise_style{atx::vol::ExerciseStyle::American};
  double multiplier{100.0};
  // XS-1 trades only standard deliverables. A false value must carry
  // UnsupportedContract and is retained only as an explicit drop reason.
  bool standard_deliverable{true};

  // Price units are dollars per option unit. Quantities below are contracts.
  double mark{0.0};
  double bid{0.0};
  double ask{0.0};
  double bid_size_contracts{0.0};
  double ask_size_contracts{0.0};
  double interval_volume_contracts{0.0};
  double lagged_open_interest_contracts{0.0};
  double adv_contracts{0.0};
  double return_sigma{0.0};

  // Vega and margins are already per listed contract. The margin values are a
  // caller-supplied conservative research schedule, not OCC STANS or broker
  // portfolio margin.
  double vega_per_contract{0.0};
  double initial_margin_per_contract{0.0};
  double maintenance_margin_per_contract{0.0};

  OptionPanelStatus status{OptionPanelStatus::Tradable};
  atx::vol::ArchiveContentIdentity definition_source_identity{};
  atx::vol::ArchiveContentIdentity feature_source_identity{};
  atx::vol::ArchiveContentIdentity execution_source_identity{};

  [[nodiscard]] bool operator==(const OptionPanelRow &) const noexcept = default;
};

struct OptionPanelLimits {
  // Both bounds are checked before the corresponding allocation.
  std::size_t max_rows{5'000'000};
  std::size_t max_dense_cells{5'000'000};

  // Exact dense payload bound: 17 f64 columns plus one mask byte per cell.
  // Catalog, canonical sparse rows, and container metadata are additional and
  // remain bounded by max_rows/max_dense_cells.
  std::size_t max_dense_bytes{1'073'741'824}; // 1 GiB

  // Zero disables an additional age bound. PIT ordering is always enforced.
  std::int64_t max_quote_age_ns{0};
};

// Cold-path catalog row aligned one-to-one with Dataset::instruments().
struct OptionInstrument {
  std::uint64_t contract_id{0};
  std::uint32_t underlier_uid{0};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  atx::vol::Side side{atx::vol::Side::Call};
  atx::vol::ExerciseStyle exercise_style{atx::vol::ExerciseStyle::American};
  double multiplier{0.0};
  bool standard_deliverable{true};
  atx::engine::InstrumentId engine_id{};

  [[nodiscard]] bool operator==(const OptionInstrument &) const noexcept = default;
};

// Fixed dataset field ordinals. The signal field is NaN outside the tradable
// PIT mask, so atx-engine WeightPolicy excludes the cell from cross-sectional
// transforms without inventing an opinion.
enum class OptionPanelField : std::uint8_t {
  Signal = 0,
  LaggedCapital = 1,
  Mark = 2,
  Bid = 3,
  Ask = 4,
  BidSizeContracts = 5,
  AskSizeContracts = 6,
  IntervalVolumeContracts = 7,
  LaggedOpenInterestContracts = 8,
  AdvContracts = 9,
  ReturnSigma = 10,
  VegaPerContract = 11,
  InitialMarginPerContract = 12,
  MaintenanceMarginPerContract = 13,
  Multiplier = 14,
  UnderlierUid = 15,
  Status = 16,
};

// Ex-post labels are intentionally not columns in the decision Dataset. A
// consumer must opt into this separately typed evaluation view.
struct OptionOutcomeLabel {
  std::int64_t decision_ts_ns{0};
  std::int64_t execution_ts_ns{0};
  std::int64_t label_end_ts_ns{0};
  std::uint64_t contract_id{0};
  double forward_pnl{0.0};
  atx::vol::ArchiveContentIdentity source_identity{};

  [[nodiscard]] bool operator==(const OptionOutcomeLabel &) const noexcept = default;
};

// Decision-time audit metadata, kept separate from both f64 engine fields and
// ex-post labels so clocks and identities retain exact types.
struct OptionDecisionAudit {
  std::int64_t decision_ts_ns{0};
  std::uint64_t contract_id{0};
  std::int64_t observed_ts_ns{0};
  std::int64_t feature_available_ts_ns{0};
  std::int64_t definition_available_ts_ns{0};
  std::int64_t quote_event_ts_ns{0};
  std::int64_t quote_available_ts_ns{0};
  OptionPanelStatus status{OptionPanelStatus::Tradable};
  atx::vol::ArchiveContentIdentity definition_source_identity{};
  atx::vol::ArchiveContentIdentity feature_source_identity{};
  atx::vol::ArchiveContentIdentity execution_source_identity{};

  [[nodiscard]] bool operator==(const OptionDecisionAudit &) const noexcept = default;
};

class OptionResearchPanel {
public:
  // Validates all boundaries, canonicalizes by
  // (decision_ts_ns, contract_id), verifies contract-definition stability, and
  // materializes a bounded dense Dataset. Duplicate keys fail with
  // AlreadyExists. Input order cannot affect output bytes.
  [[nodiscard]] static atx::core::Result<OptionResearchPanel>
  create(std::span<const OptionPanelRow> rows, OptionPanelLimits limits = {});

  [[nodiscard]] const atx::engine::data::Dataset &dataset() const noexcept { return dataset_; }
  [[nodiscard]] std::span<const OptionDecisionAudit> decision_audit() const noexcept {
    return decision_audit_;
  }
  [[nodiscard]] std::span<const OptionOutcomeLabel> outcomes() const noexcept { return outcomes_; }
  [[nodiscard]] std::span<const OptionInstrument> instruments() const noexcept {
    return instruments_;
  }
  [[nodiscard]] std::span<const atx::engine::InstrumentId> universe() const noexcept {
    return universe_;
  }
  // Hot-path access. Preconditions: field <= Status and both indices supplied
  // to tradable are inside the dataset axes. Use the checked variants at an
  // external/configuration boundary.
  [[nodiscard]] std::span<const double> column(OptionPanelField field) const noexcept {
    return dataset_.column(static_cast<atx::usize>(field));
  }
  [[nodiscard]] std::span<const double> row(OptionPanelField field,
                                            std::size_t date_index) const noexcept {
    const std::size_t instrument_count = instruments_.size();
    return column(field).subspan(date_index * instrument_count, instrument_count);
  }
  [[nodiscard]] bool tradable(std::size_t date_index, std::size_t instrument_index) const noexcept {
    return dataset_.in_universe(date_index, instrument_index);
  }
  [[nodiscard]] atx::core::Result<std::span<const double>>
  checked_column(OptionPanelField field) const {
    const auto index = static_cast<atx::usize>(field);
    if (index > static_cast<atx::usize>(OptionPanelField::Status)) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "option panel field is out of range");
    }
    return atx::core::Ok(dataset_.column(index));
  }
  [[nodiscard]] atx::core::Result<bool> checked_tradable(std::size_t date_index,
                                                         std::size_t instrument_index) const {
    if (date_index >= dataset_.num_dates() || instrument_index >= dataset_.num_instruments()) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "option panel cell is out of range");
    }
    return atx::core::Ok(dataset_.in_universe(date_index, instrument_index));
  }
  [[nodiscard]] atx::core::Result<std::span<const double>>
  checked_row(OptionPanelField field, std::size_t date_index) const {
    const auto index = static_cast<atx::usize>(field);
    if (index > static_cast<atx::usize>(OptionPanelField::Status) ||
        date_index >= dataset_.num_dates()) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "option panel row is out of range");
    }
    return atx::core::Ok(row(field, date_index));
  }

private:
  OptionResearchPanel(atx::engine::data::Dataset dataset,
                      std::vector<OptionDecisionAudit> decision_audit,
                      std::vector<OptionOutcomeLabel> outcomes,
                      std::vector<OptionInstrument> instruments,
                      std::vector<atx::engine::InstrumentId> universe) noexcept
      : dataset_{std::move(dataset)}, decision_audit_{std::move(decision_audit)},
        outcomes_{std::move(outcomes)}, instruments_{std::move(instruments)},
        universe_{std::move(universe)} {}

  atx::engine::data::Dataset dataset_;
  std::vector<OptionDecisionAudit> decision_audit_;
  std::vector<OptionOutcomeLabel> outcomes_;
  std::vector<OptionInstrument> instruments_;
  std::vector<atx::engine::InstrumentId> universe_;
};

enum class OptionSizingBasis : std::uint8_t {
  PremiumNotional = 0,
  Vega = 1,
};

enum class MarginLimitPolicy : std::uint8_t {
  RejectBatch = 0,
  ProportionalClamp = 1,
};

struct OptionTargetSpec {
  OptionSizingBasis basis{OptionSizingBasis::Vega};

  // PremiumNotional: maximum dollars of gross premium exposure.
  // Vega: maximum absolute gross portfolio vega units.
  // Input weights must have finite L1 norm <= 1 (within numeric tolerance).
  double gross_budget{0.0};

  // Position ownership/capacity is capped at floor(ADV * fraction). This is not
  // order participation: displayed size and per-slice volume remain fill-model
  // constraints.
  double max_position_adv_fraction{0.025};
  std::int64_t max_abs_contracts_per_instrument{1'000'000};

  // Deterministic independent-contract research limit. No spread offsets,
  // portfolio netting, or SPAN/STANS claims are made.
  double available_initial_margin{0.0};
  MarginLimitPolicy margin_policy{MarginLimitPolicy::RejectBatch};
};

struct OptionContractTarget {
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t current_contracts{0};
  std::int64_t target_contracts{0};
  std::int64_t order_contracts{0};
  double gross_exposure{0.0};
  double initial_margin{0.0};
  double maintenance_margin{0.0};
  bool capacity_clamped{false};

  [[nodiscard]] bool operator==(const OptionContractTarget &) const noexcept = default;
};

struct OptionTargetBook {
  std::int64_t decision_ts_ns{0};
  std::vector<OptionContractTarget> targets;
  double requested_gross_exposure{0.0};
  double realized_gross_exposure{0.0};
  double initial_margin{0.0};
  double maintenance_margin{0.0};
  bool margin_clamped{false};
};

// Convert engine weights into deterministic whole-contract targets for one
// decision row. `weights` and `current_contracts` are catalog-aligned.
//
// Non-tradable cells must carry zero weight; existing positions receive a zero
// target (a closing intent) but execution remains the later replay stage's
// responsibility. All validation and margin checks happen before a result is
// returned, providing a strong failure guarantee to caller-owned state.
[[nodiscard]] atx::core::Result<OptionTargetBook> make_option_target_book(
    const OptionResearchPanel &panel, std::size_t date_index, std::span<const double> weights,
    std::span<const std::int64_t> current_contracts, const OptionTargetSpec &spec);

// Allocation-free compatibility seam for reusable date-major coordinators.
// `output.targets.capacity()` must cover the panel catalog. The output is
// invalidated at entry and remains reusable after an error; successful calls do
// not allocate. The owning overload above retains its strong failure guarantee.
[[nodiscard]] atx::core::Result<void>
make_option_target_book_into(const OptionResearchPanel &panel, std::size_t date_index,
                             std::span<const double> weights,
                             std::span<const std::int64_t> current_contracts,
                             const OptionTargetSpec &spec, OptionTargetBook &output);

} // namespace atx::options::research
