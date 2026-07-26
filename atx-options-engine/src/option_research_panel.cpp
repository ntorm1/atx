#include "atx/options/option_research_panel.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>

namespace atx::options::research {
// All allocation and validation in this translation unit is cold-path setup.
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::Role;

constexpr std::size_t kFieldCount = static_cast<std::size_t>(OptionPanelField::Status) + 1U;

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool populated(const atx::vol::ArchiveContentIdentity &identity) noexcept {
  return identity.file_size != 0U;
}

[[nodiscard]] bool valid_side(atx::vol::Side side) noexcept {
  return side == atx::vol::Side::Call || side == atx::vol::Side::Put;
}

[[nodiscard]] bool valid_status(OptionPanelStatus status) noexcept {
  switch (status) {
  case OptionPanelStatus::Tradable:
  case OptionPanelStatus::MissingQuote:
  case OptionPanelStatus::StaleQuote:
  case OptionPanelStatus::CrossedMarket:
  case OptionPanelStatus::MissingLiquidity:
  case OptionPanelStatus::MissingRisk:
  case OptionPanelStatus::MissingMargin:
  case OptionPanelStatus::UnsupportedContract:
    return true;
  }
  return false;
}

[[nodiscard]] Result<void> validate_observation(const OptionPanelRow &row) {
  const atx::vol::ResearchObservation &observation = row.observation;
  if (observation.uid == 0U) {
    return Err(ErrorCode::InvalidArgument, "option panel underlier uid must be nonzero");
  }
  if (observation.observed_ts_ns > observation.available_ts_ns ||
      observation.available_ts_ns > observation.decision_ts_ns ||
      observation.decision_ts_ns >= observation.execution_ts_ns ||
      observation.execution_ts_ns >= observation.label_end_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "option panel row violates research point-in-time clock ordering");
  }
  if (!finite(observation.signal) || !finite(observation.forward_pnl) ||
      !finite(observation.lagged_capital) || observation.lagged_capital <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "option panel signal, PnL, and positive lagged capital must be finite");
  }
  if (!populated(observation.source_identity) || !populated(row.definition_source_identity) ||
      !populated(row.feature_source_identity) || !populated(row.execution_source_identity)) {
    return Err(ErrorCode::InvalidArgument,
               "option panel definition, feature, execution, and label lineage must be populated");
  }
  return Ok();
}

[[nodiscard]] Result<void> validate_definition(const OptionPanelRow &row) {
  if (row.contract_id == 0U) {
    return Err(ErrorCode::InvalidArgument, "option panel contract id must be nonzero");
  }
  if (row.definition_available_ts_ns > row.observation.decision_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "option definition was not available by the decision timestamp");
  }
  if (row.expiry_ts_ns <= row.observation.decision_ts_ns || !finite(row.strike) ||
      row.strike <= 0.0 || !finite(row.multiplier) || row.multiplier <= 0.0 ||
      !valid_side(row.side)) {
    return Err(ErrorCode::InvalidArgument, "option panel contract definition is invalid");
  }
  if (!row.standard_deliverable && row.status != OptionPanelStatus::UnsupportedContract) {
    return Err(ErrorCode::InvalidArgument,
               "nonstandard deliverable must be marked UnsupportedContract");
  }
  return Ok();
}

[[nodiscard]] Result<void> validate_tradable(const OptionPanelRow &row,
                                             const OptionPanelLimits &limits) {
  if (!row.standard_deliverable) {
    return Err(ErrorCode::InvalidArgument, "tradable option must have a standard deliverable");
  }
  if (row.quote_event_ts_ns > row.quote_available_ts_ns ||
      row.quote_available_ts_ns > row.observation.decision_ts_ns ||
      row.definition_available_ts_ns > row.quote_event_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "tradable option row violates definition/quote/decision clock ordering");
  }
  if (limits.max_quote_age_ns > 0) {
    const std::uint64_t age = static_cast<std::uint64_t>(row.observation.decision_ts_ns) -
                              static_cast<std::uint64_t>(row.quote_available_ts_ns);
    if (age > static_cast<std::uint64_t>(limits.max_quote_age_ns)) {
      return Err(ErrorCode::InvalidArgument,
                 "tradable option quote exceeds the configured maximum age");
    }
  }
  if (!finite(row.mark) || !finite(row.bid) || !finite(row.ask) || row.bid <= 0.0 ||
      row.ask <= 0.0 || row.bid > row.ask || row.mark < row.bid || row.mark > row.ask) {
    return Err(ErrorCode::InvalidArgument,
               "tradable option mark and uncrossed positive quote are required");
  }
  if (!finite(row.bid_size_contracts) || !finite(row.ask_size_contracts) ||
      row.bid_size_contracts <= 0.0 || row.ask_size_contracts <= 0.0 ||
      !finite(row.interval_volume_contracts) || row.interval_volume_contracts < 0.0 ||
      !finite(row.lagged_open_interest_contracts) || row.lagged_open_interest_contracts < 0.0 ||
      !finite(row.adv_contracts) || row.adv_contracts <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "tradable option liquidity must use finite nonnegative contract units");
  }
  if (!finite(row.return_sigma) || row.return_sigma < 0.0 || !finite(row.vega_per_contract) ||
      row.vega_per_contract == 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "tradable option return sigma and nonzero per-contract vega are required");
  }
  if (!finite(row.initial_margin_per_contract) || row.initial_margin_per_contract <= 0.0 ||
      !finite(row.maintenance_margin_per_contract) || row.maintenance_margin_per_contract <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "tradable option positive per-contract margins are required");
  }
  return Ok();
}

[[nodiscard]] Result<void> validate_row(const OptionPanelRow &row,
                                        const OptionPanelLimits &limits) {
  if (!valid_status(row.status)) {
    return Err(ErrorCode::InvalidArgument, "option panel status is invalid");
  }
  ATX_TRY_VOID(validate_observation(row));
  ATX_TRY_VOID(validate_definition(row));
  if (row.status == OptionPanelStatus::Tradable) {
    ATX_TRY_VOID(validate_tradable(row, limits));
  }
  return Ok();
}

[[nodiscard]] bool same_definition(const OptionPanelRow &left,
                                   const OptionPanelRow &right) noexcept {
  return left.observation.uid == right.observation.uid && left.expiry_ts_ns == right.expiry_ts_ns &&
         left.strike == right.strike && left.side == right.side &&
         left.multiplier == right.multiplier &&
         left.standard_deliverable == right.standard_deliverable &&
         left.engine_id == right.engine_id;
}

[[nodiscard]] DatasetSchema schema() {
  return DatasetSchema{{"signal", "lagged_capital", "mark", "bid", "ask", "bid_size_contracts",
                        "ask_size_contracts", "interval_volume_contracts",
                        "lagged_open_interest_contracts", "adv_contracts", "return_sigma",
                        "vega_per_contract", "initial_margin_per_contract",
                        "maintenance_margin_per_contract", "multiplier", "underlier_uid", "status"},
                       std::vector<ColumnDType>(kFieldCount, ColumnDType::F64),
                       Role::Reference,
                       0,
                       "US",
                       "listed-options-pit",
                       {true}};
}

[[nodiscard]] std::size_t field_index(OptionPanelField field) noexcept {
  return static_cast<std::size_t>(field);
}

void set_cell(std::vector<std::vector<double>> &columns, std::size_t cell,
              const OptionPanelRow &row) {
  const bool tradable = row.status == OptionPanelStatus::Tradable;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  columns[field_index(OptionPanelField::Signal)][cell] = tradable ? row.observation.signal : nan;
  columns[field_index(OptionPanelField::LaggedCapital)][cell] = row.observation.lagged_capital;
  columns[field_index(OptionPanelField::Mark)][cell] = row.mark;
  columns[field_index(OptionPanelField::Bid)][cell] = row.bid;
  columns[field_index(OptionPanelField::Ask)][cell] = row.ask;
  columns[field_index(OptionPanelField::BidSizeContracts)][cell] = row.bid_size_contracts;
  columns[field_index(OptionPanelField::AskSizeContracts)][cell] = row.ask_size_contracts;
  columns[field_index(OptionPanelField::IntervalVolumeContracts)][cell] =
      row.interval_volume_contracts;
  columns[field_index(OptionPanelField::LaggedOpenInterestContracts)][cell] =
      row.lagged_open_interest_contracts;
  columns[field_index(OptionPanelField::AdvContracts)][cell] = row.adv_contracts;
  columns[field_index(OptionPanelField::ReturnSigma)][cell] = row.return_sigma;
  columns[field_index(OptionPanelField::VegaPerContract)][cell] = row.vega_per_contract;
  columns[field_index(OptionPanelField::InitialMarginPerContract)][cell] =
      row.initial_margin_per_contract;
  columns[field_index(OptionPanelField::MaintenanceMarginPerContract)][cell] =
      row.maintenance_margin_per_contract;
  columns[field_index(OptionPanelField::Multiplier)][cell] = row.multiplier;
  columns[field_index(OptionPanelField::UnderlierUid)][cell] =
      static_cast<double>(row.observation.uid);
  columns[field_index(OptionPanelField::Status)][cell] =
      static_cast<double>(static_cast<std::uint8_t>(row.status));
}

[[nodiscard]] Result<std::int64_t> whole_contracts(double value) {
  constexpr long double kI64Max =
      static_cast<long double>((std::numeric_limits<std::int64_t>::max)());
  constexpr long double kI64Min =
      static_cast<long double>((std::numeric_limits<std::int64_t>::min)());
  const long double widened = static_cast<long double>(value);
  // Exclude INT64_MIN because later magnitude arithmetic must remain defined.
  if (!finite(value) || widened > kI64Max || widened <= kI64Min) {
    return Err(ErrorCode::OutOfRange, "whole-contract target exceeds int64 range");
  }
  return Ok(static_cast<std::int64_t>(value));
}

// Cross-sectional normalization can represent a mathematically integral target
// a few ulps below that integer (for example, (3/8)*800/10). Snap only values
// inside an eight-ulp envelope before the conservative toward-zero conversion.
// This removes representation noise without rounding a meaningful fraction up.
[[nodiscard]] double snap_near_integer(double value) noexcept {
  if (!finite(value)) {
    return value;
  }
  const double nearest = std::round(value);
  const double scale = (std::max)(1.0, std::abs(value));
  const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() * scale;
  return std::abs(value - nearest) <= tolerance ? nearest : value;
}

[[nodiscard]] Result<double> checked_nonnegative_product(double left, double right,
                                                         std::string message) {
  const double product = left * right;
  if (!finite(product) || product < 0.0) {
    return Err(ErrorCode::OutOfRange, std::move(message));
  }
  return Ok(product);
}

[[nodiscard]] Result<void> checked_accumulate(double &total, double value, std::string message) {
  const double next = total + value;
  if (!finite(next) || next < 0.0) {
    return Err(ErrorCode::OutOfRange, std::move(message));
  }
  total = next;
  return Ok();
}

[[nodiscard]] std::int64_t with_sign(std::int64_t magnitude, std::int64_t signed_value) noexcept {
  return signed_value < 0 ? -magnitude : magnitude;
}

[[nodiscard]] Result<std::int64_t> checked_order_quantity(std::int64_t target,
                                                          std::int64_t current) {
  constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
  constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
  if ((current > 0 && target < kMin + current) || (current < 0 && target > kMax + current)) {
    return Err(ErrorCode::OutOfRange, "option order quantity exceeds int64 range");
  }
  return Ok(target - current);
}

[[nodiscard]] Result<void> recompute_book_totals(OptionTargetBook &book,
                                                 const OptionResearchPanel &panel,
                                                 std::size_t row_offset, OptionSizingBasis basis) {
  book.realized_gross_exposure = 0.0;
  book.initial_margin = 0.0;
  book.maintenance_margin = 0.0;

  const std::span<const double> mark = panel.column(OptionPanelField::Mark);
  const std::span<const double> multiplier = panel.column(OptionPanelField::Multiplier);
  const std::span<const double> vega = panel.column(OptionPanelField::VegaPerContract);
  const std::span<const double> initial_margin =
      panel.column(OptionPanelField::InitialMarginPerContract);
  const std::span<const double> maintenance_margin =
      panel.column(OptionPanelField::MaintenanceMarginPerContract);

  for (std::size_t i = 0; i < book.targets.size(); ++i) {
    OptionContractTarget &target = book.targets[i];
    const std::size_t cell = row_offset + i;
    if (target.target_contracts == 0) {
      target.gross_exposure = 0.0;
      target.initial_margin = 0.0;
      target.maintenance_margin = 0.0;
      ATX_TRY(std::int64_t order,
              checked_order_quantity(target.target_contracts, target.current_contracts));
      target.order_contracts = order;
      continue;
    }
    const double quantity = static_cast<double>(
        target.target_contracts < 0 ? -target.target_contracts : target.target_contracts);
    double unit_exposure = std::abs(vega[cell]);
    if (basis == OptionSizingBasis::PremiumNotional) {
      ATX_TRY(unit_exposure,
              checked_nonnegative_product(mark[cell], multiplier[cell],
                                          "option premium unit exposure exceeds finite range"));
    }
    ATX_TRY(target.gross_exposure,
            checked_nonnegative_product(quantity, unit_exposure,
                                        "option gross exposure exceeds finite range"));
    ATX_TRY(target.initial_margin,
            checked_nonnegative_product(quantity, initial_margin[cell],
                                        "option initial margin exceeds finite range"));
    ATX_TRY(target.maintenance_margin,
            checked_nonnegative_product(quantity, maintenance_margin[cell],
                                        "option maintenance margin exceeds finite range"));
    ATX_TRY(std::int64_t order,
            checked_order_quantity(target.target_contracts, target.current_contracts));
    target.order_contracts = order;
    ATX_TRY_VOID(checked_accumulate(book.realized_gross_exposure, target.gross_exposure,
                                    "option gross exposure aggregate exceeds finite range"));
    ATX_TRY_VOID(checked_accumulate(book.initial_margin, target.initial_margin,
                                    "option initial margin aggregate exceeds finite range"));
    ATX_TRY_VOID(checked_accumulate(book.maintenance_margin, target.maintenance_margin,
                                    "option maintenance margin aggregate exceeds finite range"));
  }
  return Ok();
}

} // namespace

Result<OptionResearchPanel> OptionResearchPanel::create(std::span<const OptionPanelRow> rows,
                                                        OptionPanelLimits limits) {
  if (rows.empty()) {
    return Err(ErrorCode::InvalidArgument, "option research panel must not be empty");
  }
  if (limits.max_rows == 0U || limits.max_dense_cells == 0U || limits.max_dense_bytes == 0U ||
      limits.max_quote_age_ns < 0) {
    return Err(ErrorCode::InvalidArgument, "option panel limits must be positive");
  }
  if (rows.size() > limits.max_rows ||
      rows.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
    return Err(ErrorCode::OutOfRange, "option panel row limit exceeded");
  }

  std::vector<OptionPanelRow> canonical_rows(rows.begin(), rows.end());
  for (const OptionPanelRow &row : canonical_rows) {
    ATX_TRY_VOID(validate_row(row, limits));
  }
  std::sort(canonical_rows.begin(), canonical_rows.end(),
            [](const OptionPanelRow &left, const OptionPanelRow &right) noexcept {
              return std::tie(left.observation.decision_ts_ns, left.contract_id) <
                     std::tie(right.observation.decision_ts_ns, right.contract_id);
            });
  for (std::size_t i = 1U; i < canonical_rows.size(); ++i) {
    if (canonical_rows[i - 1U].observation.decision_ts_ns ==
            canonical_rows[i].observation.decision_ts_ns &&
        canonical_rows[i - 1U].contract_id == canonical_rows[i].contract_id) {
      return Err(ErrorCode::AlreadyExists,
                 "duplicate option panel key (decision_ts_ns, contract_id)");
    }
  }

  std::vector<DateKey> dates;
  std::vector<std::uint64_t> contract_ids;
  dates.reserve(canonical_rows.size());
  contract_ids.reserve(canonical_rows.size());
  for (const OptionPanelRow &row : canonical_rows) {
    dates.push_back(row.observation.decision_ts_ns);
    contract_ids.push_back(row.contract_id);
  }
  std::sort(dates.begin(), dates.end());
  dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
  std::sort(contract_ids.begin(), contract_ids.end());
  contract_ids.erase(std::unique(contract_ids.begin(), contract_ids.end()), contract_ids.end());

  if (dates.size() > limits.max_dense_cells / contract_ids.size()) {
    return Err(ErrorCode::OutOfRange, "option panel dense-cell limit exceeded");
  }
  const std::size_t cells = dates.size() * contract_ids.size();
  constexpr std::size_t kBytesPerDenseCell = kFieldCount * sizeof(double) + sizeof(std::uint8_t);
  if (cells > limits.max_dense_bytes / kBytesPerDenseCell) {
    return Err(ErrorCode::OutOfRange, "option panel dense-byte limit exceeded");
  }

  std::vector<std::size_t> contract_order(canonical_rows.size());
  std::iota(contract_order.begin(), contract_order.end(), std::size_t{0});
  std::sort(contract_order.begin(), contract_order.end(),
            [&canonical_rows](std::size_t left, std::size_t right) noexcept {
              const OptionPanelRow &left_row = canonical_rows[left];
              const OptionPanelRow &right_row = canonical_rows[right];
              if (left_row.contract_id != right_row.contract_id) {
                return left_row.contract_id < right_row.contract_id;
              }
              return left_row.observation.decision_ts_ns < right_row.observation.decision_ts_ns;
            });

  std::vector<OptionInstrument> instruments;
  std::vector<atx::engine::InstrumentId> universe;
  instruments.reserve(contract_ids.size());
  universe.reserve(contract_ids.size());
  std::vector<InstKey> dataset_instruments;
  dataset_instruments.reserve(contract_ids.size());
  std::size_t contract_begin = 0U;
  for (std::size_t i = 0; i < contract_ids.size(); ++i) {
    const std::uint64_t contract_id = contract_ids[i];
    if (contract_begin >= contract_order.size() ||
        canonical_rows[contract_order[contract_begin]].contract_id != contract_id) {
      return Err(ErrorCode::Internal, "canonical option contract discovery failed");
    }
    const OptionPanelRow &definition = canonical_rows[contract_order[contract_begin]];
    std::size_t contract_end = contract_begin + 1U;
    while (contract_end < contract_order.size() &&
           canonical_rows[contract_order[contract_end]].contract_id == contract_id) {
      if (!same_definition(definition, canonical_rows[contract_order[contract_end]])) {
        return Err(ErrorCode::InvalidArgument,
                   "stable option contract id maps to inconsistent definitions");
      }
      ++contract_end;
    }

    const atx::engine::InstrumentId engine_id = definition.engine_id;
    instruments.push_back(OptionInstrument{
        contract_id, definition.observation.uid, definition.expiry_ts_ns, definition.strike,
        definition.side, definition.multiplier, definition.standard_deliverable, engine_id});
    universe.push_back(engine_id);
    dataset_instruments.push_back(engine_id.id);
    contract_begin = contract_end;
  }
  std::vector<InstKey> unique_engine_ids = dataset_instruments;
  std::sort(unique_engine_ids.begin(), unique_engine_ids.end());
  if (std::adjacent_find(unique_engine_ids.begin(), unique_engine_ids.end()) !=
      unique_engine_ids.end()) {
    return Err(ErrorCode::AlreadyExists,
               "distinct option contracts must have unique caller-owned engine ids");
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::vector<double>> columns(kFieldCount, std::vector<double>(cells, nan));
  std::vector<std::uint8_t> mask(cells, 0U);
  for (const OptionPanelRow &row : canonical_rows) {
    const auto date_it =
        std::lower_bound(dates.begin(), dates.end(), row.observation.decision_ts_ns);
    const auto contract_it =
        std::lower_bound(contract_ids.begin(), contract_ids.end(), row.contract_id);
    const std::size_t date_index = static_cast<std::size_t>(date_it - dates.begin());
    const std::size_t instrument_index =
        static_cast<std::size_t>(contract_it - contract_ids.begin());
    const std::size_t cell = date_index * contract_ids.size() + instrument_index;
    set_cell(columns, cell, row);
    mask[cell] = row.status == OptionPanelStatus::Tradable ? std::uint8_t{1} : std::uint8_t{0};
  }

  ATX_TRY(Dataset dataset,
          Dataset::create(schema(), std::move(dates), std::move(dataset_instruments),
                          std::move(columns), std::move(mask),
                          DatasetProvenance{"atx-options-engine:option-research-panel-v1",
                                            "canonical PIT listed-option research panel"}));
  std::vector<OptionDecisionAudit> decision_audit;
  decision_audit.reserve(canonical_rows.size());
  std::vector<OptionOutcomeLabel> outcomes;
  outcomes.reserve(canonical_rows.size());
  for (const OptionPanelRow &row : canonical_rows) {
    decision_audit.push_back(OptionDecisionAudit{
        row.observation.decision_ts_ns, row.contract_id, row.observation.observed_ts_ns,
        row.observation.available_ts_ns, row.definition_available_ts_ns, row.quote_event_ts_ns,
        row.quote_available_ts_ns, row.status, row.definition_source_identity,
        row.feature_source_identity, row.execution_source_identity});
    outcomes.push_back(
        OptionOutcomeLabel{row.observation.decision_ts_ns, row.observation.execution_ts_ns,
                           row.observation.label_end_ts_ns, row.contract_id,
                           row.observation.forward_pnl, row.observation.source_identity});
  }
  return Ok(OptionResearchPanel{std::move(dataset), std::move(decision_audit), std::move(outcomes),
                                std::move(instruments), std::move(universe)});
}

Result<OptionTargetBook> make_option_target_book(const OptionResearchPanel &panel,
                                                 std::size_t date_index,
                                                 std::span<const double> weights,
                                                 std::span<const std::int64_t> current_contracts,
                                                 const OptionTargetSpec &spec) {
  const std::size_t instrument_count = panel.instruments().size();
  if (date_index >= panel.dataset().num_dates()) {
    return Err(ErrorCode::OutOfRange, "option target date index is out of range");
  }
  if (weights.size() != instrument_count || current_contracts.size() != instrument_count) {
    return Err(ErrorCode::InvalidArgument,
               "option target inputs must align with the contract catalog");
  }
  constexpr std::int64_t kMaxExactlyRepresentableContracts = 9'007'199'254'740'991LL; // 2^53 - 1
  if (!finite(spec.gross_budget) || spec.gross_budget < 0.0 ||
      !finite(spec.max_position_adv_fraction) || spec.max_position_adv_fraction < 0.0 ||
      spec.max_position_adv_fraction > 1.0 || spec.max_abs_contracts_per_instrument <= 0 ||
      spec.max_abs_contracts_per_instrument > kMaxExactlyRepresentableContracts ||
      !finite(spec.available_initial_margin) || spec.available_initial_margin < 0.0) {
    return Err(ErrorCode::InvalidArgument, "option target sizing specification is invalid");
  }
  switch (spec.basis) {
  case OptionSizingBasis::PremiumNotional:
  case OptionSizingBasis::Vega:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "option target sizing basis is invalid");
  }
  switch (spec.margin_policy) {
  case MarginLimitPolicy::RejectBatch:
  case MarginLimitPolicy::ProportionalClamp:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "option target margin policy is invalid");
  }

  const std::size_t row_offset = date_index * instrument_count;
  const std::span<const double> mark = panel.column(OptionPanelField::Mark);
  const std::span<const double> multiplier = panel.column(OptionPanelField::Multiplier);
  const std::span<const double> vega = panel.column(OptionPanelField::VegaPerContract);
  const std::span<const double> adv = panel.column(OptionPanelField::AdvContracts);

  double weight_l1 = 0.0;
  for (std::size_t i = 0; i < instrument_count; ++i) {
    if (!finite(weights[i])) {
      return Err(ErrorCode::InvalidArgument, "option target weights must be finite");
    }
    if (!panel.tradable(date_index, i) && weights[i] != 0.0) {
      return Err(ErrorCode::InvalidArgument,
                 "non-tradable option cell must have zero target weight");
    }
    ATX_TRY_VOID(checked_accumulate(weight_l1, std::abs(weights[i]),
                                    "option target weight L1 norm exceeds finite range"));
  }
  constexpr double kGrossWeightTolerance = 64.0 * std::numeric_limits<double>::epsilon();
  if (weight_l1 > 1.0 + kGrossWeightTolerance) {
    return Err(ErrorCode::InvalidArgument, "option target weight L1 norm must not exceed one");
  }

  OptionTargetBook book;
  book.decision_ts_ns = panel.dataset().dates()[date_index];
  book.targets.reserve(instrument_count);

  for (std::size_t i = 0; i < instrument_count; ++i) {
    const bool tradable = panel.tradable(date_index, i);

    std::int64_t target_contracts = 0;
    bool capacity_clamped = false;
    const std::size_t cell = row_offset + i;
    if (tradable && weights[i] != 0.0) {
      const double unit_exposure = spec.basis == OptionSizingBasis::PremiumNotional
                                       ? mark[cell] * multiplier[cell]
                                       : std::abs(vega[cell]);
      if (!finite(unit_exposure) || unit_exposure <= 0.0) {
        return Err(ErrorCode::InvalidArgument, "tradable option target has invalid unit exposure");
      }
      ATX_TRY(double requested_exposure,
              checked_nonnegative_product(std::abs(weights[i]), spec.gross_budget,
                                          "option requested exposure exceeds finite range"));
      ATX_TRY_VOID(checked_accumulate(book.requested_gross_exposure, requested_exposure,
                                      "option requested gross exposure exceeds finite range"));
      const double signed_requested_exposure = std::copysign(requested_exposure, weights[i]);
      ATX_TRY(std::int64_t requested_contracts,
              whole_contracts(snap_near_integer(signed_requested_exposure / unit_exposure)));

      ATX_TRY(double raw_capacity,
              checked_nonnegative_product(adv[cell], spec.max_position_adv_fraction,
                                          "option ADV position capacity exceeds finite range"));
      const double capacity_value = std::floor(snap_near_integer(raw_capacity));
      ATX_TRY(std::int64_t adv_capacity, whole_contracts(capacity_value));
      const std::int64_t capacity = (std::min)(adv_capacity, spec.max_abs_contracts_per_instrument);
      const std::int64_t requested_abs =
          requested_contracts < 0 ? -requested_contracts : requested_contracts;
      if (requested_abs > capacity) {
        target_contracts = with_sign(capacity, requested_contracts);
        capacity_clamped = true;
      } else {
        target_contracts = requested_contracts;
      }
    }

    const OptionInstrument &instrument = panel.instruments()[i];
    book.targets.push_back(OptionContractTarget{instrument.contract_id, instrument.engine_id,
                                                current_contracts[i], target_contracts, 0, 0.0, 0.0,
                                                0.0, capacity_clamped});
  }

  ATX_TRY_VOID(recompute_book_totals(book, panel, row_offset, spec.basis));
  if (book.initial_margin > spec.available_initial_margin) {
    if (spec.margin_policy == MarginLimitPolicy::RejectBatch) {
      return Err(ErrorCode::OutOfRange, "option target book exceeds available initial margin");
    }
    const double scale = spec.available_initial_margin / book.initial_margin;
    for (OptionContractTarget &target : book.targets) {
      const double scaled = snap_near_integer(static_cast<double>(target.target_contracts) * scale);
      ATX_TRY(std::int64_t clamped, whole_contracts(scaled));
      target.target_contracts = clamped;
    }
    book.margin_clamped = true;
    ATX_TRY_VOID(recompute_book_totals(book, panel, row_offset, spec.basis));
  }
  return Ok(std::move(book));
}

} // namespace atx::options::research
