#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/engine/loop/weight_policy.hpp"
#include "atx/options/option_research_panel.hpp"

namespace { // focused bridge contract tests

using atx::core::ErrorCode;
using atx::options::research::make_option_target_book;
using atx::options::research::MarginLimitPolicy;
using atx::options::research::OptionPanelField;
using atx::options::research::OptionPanelLimits;
using atx::options::research::OptionPanelRow;
using atx::options::research::OptionPanelStatus;
using atx::options::research::OptionResearchPanel;
using atx::options::research::OptionSizingBasis;
using atx::options::research::OptionTargetSpec;
using atx::vol::ArchiveContentIdentity;

constexpr std::array<OptionPanelField, 17> kPanelFields{
    OptionPanelField::Signal,
    OptionPanelField::LaggedCapital,
    OptionPanelField::Mark,
    OptionPanelField::Bid,
    OptionPanelField::Ask,
    OptionPanelField::BidSizeContracts,
    OptionPanelField::AskSizeContracts,
    OptionPanelField::IntervalVolumeContracts,
    OptionPanelField::LaggedOpenInterestContracts,
    OptionPanelField::AdvContracts,
    OptionPanelField::ReturnSigma,
    OptionPanelField::VegaPerContract,
    OptionPanelField::InitialMarginPerContract,
    OptionPanelField::MaintenanceMarginPerContract,
    OptionPanelField::Multiplier,
    OptionPanelField::UnderlierUid,
    OptionPanelField::Status,
};

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) {
  return ArchiveContentIdentity{1'000U + seed, 2'000U + seed,
                                static_cast<std::uint32_t>(3'000U + seed),
                                static_cast<std::uint32_t>(4'000U + seed)};
}

[[nodiscard]] OptionPanelRow row(std::uint64_t contract_id, std::uint32_t underlier_uid,
                                 std::int64_t decision_ts_ns, double signal = 1.0) {
  OptionPanelRow out;
  out.observation.uid = underlier_uid;
  out.observation.observed_ts_ns = decision_ts_ns - 20;
  out.observation.available_ts_ns = decision_ts_ns - 10;
  out.observation.decision_ts_ns = decision_ts_ns;
  out.observation.execution_ts_ns = decision_ts_ns + 1;
  out.observation.label_end_ts_ns = decision_ts_ns + 100;
  out.observation.signal = signal;
  out.observation.forward_pnl = 2.0;
  out.observation.lagged_capital = 1'000.0;
  out.observation.source_identity = identity(contract_id + 1U);
  out.contract_id = contract_id;
  out.engine_id.id = static_cast<std::uint32_t>(contract_id);
  out.definition_available_ts_ns = decision_ts_ns - 30;
  out.quote_event_ts_ns = decision_ts_ns - 5;
  out.quote_available_ts_ns = decision_ts_ns - 2;
  out.expiry_ts_ns = 10'000;
  out.strike = 100.0 + static_cast<double>(contract_id);
  out.multiplier = 100.0;
  out.mark = 10.0;
  out.bid = 9.0;
  out.ask = 11.0;
  out.bid_size_contracts = 50.0;
  out.ask_size_contracts = 60.0;
  out.interval_volume_contracts = 100.0;
  out.lagged_open_interest_contracts = 1'000.0;
  out.adv_contracts = 10'000.0;
  out.return_sigma = 0.2;
  out.vega_per_contract = 10.0;
  out.initial_margin_per_contract = 100.0;
  out.maintenance_margin_per_contract = 80.0;
  out.definition_source_identity = identity(contract_id + 2U);
  out.feature_source_identity = identity(contract_id + 3U);
  out.execution_source_identity = identity(contract_id + 4U);
  return out;
}

[[nodiscard]] std::uint64_t bits(double value) noexcept {
  return std::bit_cast<std::uint64_t>(value);
}

void expect_identical(const OptionResearchPanel &left, const OptionResearchPanel &right) {
  ASSERT_EQ(left.decision_audit().size(), right.decision_audit().size());
  for (std::size_t i = 0; i < left.decision_audit().size(); ++i) {
    EXPECT_TRUE(left.decision_audit()[i] == right.decision_audit()[i]);
  }
  ASSERT_EQ(left.outcomes().size(), right.outcomes().size());
  for (std::size_t i = 0; i < left.outcomes().size(); ++i) {
    EXPECT_TRUE(left.outcomes()[i] == right.outcomes()[i]);
  }

  ASSERT_EQ(left.instruments().size(), right.instruments().size());
  for (std::size_t i = 0; i < left.instruments().size(); ++i) {
    EXPECT_TRUE(left.instruments()[i] == right.instruments()[i]);
    EXPECT_EQ(left.universe()[i], right.universe()[i]);
  }

  ASSERT_EQ(left.dataset().dates().size(), right.dataset().dates().size());
  for (std::size_t i = 0; i < left.dataset().dates().size(); ++i) {
    EXPECT_EQ(left.dataset().dates()[i], right.dataset().dates()[i]);
  }
  for (const OptionPanelField field : kPanelFields) {
    const auto left_column = left.column(field);
    const auto right_column = right.column(field);
    ASSERT_EQ(left_column.size(), right_column.size());
    for (std::size_t i = 0; i < left_column.size(); ++i) {
      EXPECT_EQ(bits(left_column[i]), bits(right_column[i]));
    }
  }
  for (std::size_t date = 0; date < left.dataset().num_dates(); ++date) {
    for (std::size_t instrument = 0; instrument < left.instruments().size(); ++instrument) {
      EXPECT_EQ(left.tradable(date, instrument), right.tradable(date, instrument));
    }
  }
}

[[nodiscard]] std::size_t instrument_index(const OptionResearchPanel &panel,
                                           std::uint64_t contract_id) {
  for (std::size_t i = 0; i < panel.instruments().size(); ++i) {
    if (panel.instruments()[i].contract_id == contract_id) {
      return i;
    }
  }
  return panel.instruments().size();
}

[[nodiscard]] OptionTargetSpec target_spec(OptionSizingBasis basis, double gross_budget) {
  OptionTargetSpec spec;
  spec.basis = basis;
  spec.gross_budget = gross_budget;
  spec.max_position_adv_fraction = 1.0;
  spec.available_initial_margin = 1.0e12;
  return spec;
}

TEST(OptionResearchPanelCreate, InputPermutationHasCanonicalIdentity) {
  std::vector<OptionPanelRow> input{
      row(30U, 3U, 200, 4.0),
      row(10U, 1U, 100, 1.0),
      row(30U, 3U, 100, 2.0),
      row(10U, 1U, 200, 3.0),
  };
  std::vector<OptionPanelRow> permuted{input.rbegin(), input.rend()};

  auto first = OptionResearchPanel::create(input);
  auto second = OptionResearchPanel::create(permuted);

  ASSERT_TRUE(first) << first.error().to_string();
  ASSERT_TRUE(second) << second.error().to_string();
  expect_identical(*first, *second);
}

TEST(OptionResearchPanelCreate, DuplicateDecisionContractKeyIsRejected) {
  OptionPanelRow duplicate = row(10U, 1U, 100, 2.0);
  const std::vector<OptionPanelRow> input{row(10U, 1U, 100, 1.0), duplicate};

  const auto result = OptionResearchPanel::create(input);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
}

TEST(OptionResearchPanelCreate, NonPointInTimeClocksAreRejected) {
  const OptionPanelRow valid = row(10U, 1U, 100);
  std::array<OptionPanelRow, 3> invalid{valid, valid, valid};
  invalid[0].quote_available_ts_ns = invalid[0].observation.decision_ts_ns + 1;
  invalid[1].definition_available_ts_ns = invalid[1].observation.decision_ts_ns + 1;
  invalid[2].observation.available_ts_ns = invalid[2].observation.decision_ts_ns + 1;

  for (const OptionPanelRow &candidate : invalid) {
    const std::array<OptionPanelRow, 1> input{candidate};
    const auto result = OptionResearchPanel::create(input);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(OptionResearchPanelCreate, MissingAnyRequiredLineageIsRejected) {
  const OptionPanelRow valid = row(10U, 1U, 100);
  std::array<OptionPanelRow, 4> invalid{valid, valid, valid, valid};
  invalid[0].observation.source_identity = {};
  invalid[1].definition_source_identity = {};
  invalid[2].feature_source_identity = {};
  invalid[3].execution_source_identity = {};

  for (const OptionPanelRow &candidate : invalid) {
    const std::array<OptionPanelRow, 1> input{candidate};
    const auto result = OptionResearchPanel::create(input);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(OptionResearchPanelCreate, NonTradableCellMasksItsSignal) {
  OptionPanelRow unavailable = row(10U, 1U, 100, 123.0);
  unavailable.status = OptionPanelStatus::MissingQuote;
  const std::array<OptionPanelRow, 1> input{unavailable};

  const auto result = OptionResearchPanel::create(input);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->column(OptionPanelField::Signal).size(), 1U);
  EXPECT_TRUE(std::isnan(result->column(OptionPanelField::Signal).front()));
  EXPECT_FALSE(result->tradable(0U, 0U));
  ASSERT_EQ(result->outcomes().size(), 1U);
  EXPECT_DOUBLE_EQ(result->outcomes().front().forward_pnl, unavailable.observation.forward_pnl);
  EXPECT_FALSE(result->dataset().column_by_name("forward_pnl"));
}

TEST(OptionResearchPanelCreate, FutureRowsCannotChangeEarlierSignals) {
  const std::vector<OptionPanelRow> early{
      row(10U, 1U, 100, -2.0),
      row(30U, 3U, 100, 4.0),
  };
  std::vector<OptionPanelRow> with_future = early;
  with_future.push_back(row(20U, 2U, 200, 1.0e100));
  with_future.push_back(row(10U, 1U, 200, -1.0e100));

  const auto baseline = OptionResearchPanel::create(early);
  const auto extended = OptionResearchPanel::create(with_future);

  ASSERT_TRUE(baseline) << baseline.error().to_string();
  ASSERT_TRUE(extended) << extended.error().to_string();
  const std::size_t baseline_10 = instrument_index(*baseline, 10U);
  const std::size_t baseline_30 = instrument_index(*baseline, 30U);
  const std::size_t extended_10 = instrument_index(*extended, 10U);
  const std::size_t extended_20 = instrument_index(*extended, 20U);
  const std::size_t extended_30 = instrument_index(*extended, 30U);
  ASSERT_LT(baseline_10, baseline->instruments().size());
  ASSERT_LT(baseline_30, baseline->instruments().size());
  ASSERT_LT(extended_10, extended->instruments().size());
  ASSERT_LT(extended_20, extended->instruments().size());
  ASSERT_LT(extended_30, extended->instruments().size());
  const auto baseline_signal = baseline->column(OptionPanelField::Signal);
  const auto extended_signal = extended->column(OptionPanelField::Signal);

  EXPECT_DOUBLE_EQ(extended_signal[extended_10], baseline_signal[baseline_10]);
  EXPECT_DOUBLE_EQ(extended_signal[extended_30], baseline_signal[baseline_30]);
  EXPECT_TRUE(std::isnan(extended_signal[extended_20]));
  EXPECT_FALSE(extended->tradable(0U, extended_20));
}

TEST(OptionResearchPanelCreate, DenseByteLimitRejectsBeforeMaterialization) {
  const std::array<OptionPanelRow, 1> input{row(10U, 1U, 100)};
  OptionPanelLimits limits;
  limits.max_dense_bytes = 17U * sizeof(double);

  const auto result = OptionResearchPanel::create(input, limits);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

TEST(OptionResearchPanelCreate, DistinctContractsCannotAliasCallerEngineId) {
  std::array<OptionPanelRow, 2> input{row(10U, 1U, 100), row(20U, 2U, 100)};
  input[1].engine_id = input[0].engine_id;

  const auto result = OptionResearchPanel::create(input);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
}

TEST(OptionResearchPanelCreate, NonstandardDeliverableIsExplicitlyUnsupported) {
  OptionPanelRow adjusted = row(10U, 1U, 100);
  adjusted.standard_deliverable = false;
  const std::array<OptionPanelRow, 1> invalid{adjusted};
  EXPECT_FALSE(OptionResearchPanel::create(invalid));

  adjusted.status = OptionPanelStatus::UnsupportedContract;
  const std::array<OptionPanelRow, 1> explicit_drop{adjusted};
  const auto panel = OptionResearchPanel::create(explicit_drop);

  ASSERT_TRUE(panel) << panel.error().to_string();
  EXPECT_FALSE(panel->tradable(0U, 0U));
}

TEST(OptionResearchPanelCreate, CheckedAccessorsRejectInvalidPublicIndices) {
  const std::array<OptionPanelRow, 1> input{row(10U, 1U, 100)};
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();

  const auto bad_field = panel->checked_column(
      static_cast<OptionPanelField>(std::numeric_limits<std::uint8_t>::max()));
  const auto bad_cell = panel->checked_tradable(1U, 0U);

  ASSERT_FALSE(bad_field);
  EXPECT_EQ(bad_field.error().code(), ErrorCode::OutOfRange);
  ASSERT_FALSE(bad_cell);
  EXPECT_EQ(bad_cell.error().code(), ErrorCode::OutOfRange);
}

TEST(OptionTargetBook, PremiumSizingUsesContractMultiplierAndWholeContracts) {
  std::vector<OptionPanelRow> input{
      row(10U, 1U, 100),
      row(20U, 2U, 100),
      row(30U, 3U, 100),
  };
  input[0].multiplier = 10.0;
  input[1].multiplier = 100.0;
  input[2].multiplier = 1'000.0;
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 3> weights{0.25, -0.25, 0.5};
  const std::array<std::int64_t, 3> current{};
  const OptionTargetSpec spec = target_spec(OptionSizingBasis::PremiumNotional, 40'000.0);

  const auto book = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_TRUE(book) << book.error().to_string();
  ASSERT_EQ(book->targets.size(), 3U);
  EXPECT_EQ(book->targets[0].target_contracts, 100);
  EXPECT_EQ(book->targets[1].target_contracts, -10);
  EXPECT_EQ(book->targets[2].target_contracts, 2);
  EXPECT_DOUBLE_EQ(book->requested_gross_exposure, 40'000.0);
  EXPECT_DOUBLE_EQ(book->realized_gross_exposure, 40'000.0);
}

TEST(OptionTargetBook, VegaSizingIgnoresMultiplierAndUsesWholeContracts) {
  std::vector<OptionPanelRow> input{
      row(10U, 1U, 100),
      row(20U, 2U, 100),
      row(30U, 3U, 100),
  };
  input[0].multiplier = 10.0;
  input[1].multiplier = 100.0;
  input[2].multiplier = 1'000.0;
  input[0].vega_per_contract = 10.0;
  input[1].vega_per_contract = 20.0;
  input[2].vega_per_contract = 40.0;
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 3> weights{0.25, -0.25, 0.5};
  const std::array<std::int64_t, 3> current{};
  const OptionTargetSpec spec = target_spec(OptionSizingBasis::Vega, 800.0);

  const auto book = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_TRUE(book) << book.error().to_string();
  ASSERT_EQ(book->targets.size(), 3U);
  EXPECT_EQ(book->targets[0].target_contracts, 20);
  EXPECT_EQ(book->targets[1].target_contracts, -10);
  EXPECT_EQ(book->targets[2].target_contracts, 10);
  EXPECT_DOUBLE_EQ(book->requested_gross_exposure, 800.0);
  EXPECT_DOUBLE_EQ(book->realized_gross_exposure, 800.0);
}

TEST(OptionTargetBook, AdvParticipationCapsRequestedContracts) {
  std::vector<OptionPanelRow> input{
      row(10U, 1U, 100),
      row(20U, 2U, 100),
      row(30U, 3U, 100),
  };
  input[0].adv_contracts = 99.0;
  input[1].adv_contracts = 100.0;
  input[2].adv_contracts = 101.0;
  input[0].vega_per_contract = 1.0;
  input[1].vega_per_contract = 1.0;
  input[2].vega_per_contract = 1.0;
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 3> weights{1.0 / 3.0, -1.0 / 3.0, 1.0 / 3.0};
  const std::array<std::int64_t, 3> current{};
  OptionTargetSpec spec = target_spec(OptionSizingBasis::Vega, 3'000.0);
  spec.max_position_adv_fraction = 0.1;

  const auto book = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_TRUE(book) << book.error().to_string();
  ASSERT_EQ(book->targets.size(), 3U);
  EXPECT_EQ(book->targets[0].target_contracts, 9);
  EXPECT_EQ(book->targets[1].target_contracts, -10);
  EXPECT_EQ(book->targets[2].target_contracts, 10);
  EXPECT_TRUE(book->targets[0].capacity_clamped);
  EXPECT_TRUE(book->targets[1].capacity_clamped);
  EXPECT_TRUE(book->targets[2].capacity_clamped);
  EXPECT_DOUBLE_EQ(book->realized_gross_exposure, 29.0);
}

TEST(OptionTargetBook, GrossBudgetRejectsWeightsAboveUnitL1Norm) {
  const std::array<OptionPanelRow, 2> input{row(10U, 1U, 100), row(20U, 2U, 100)};
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 2> weights{1.0, -1.0};
  const std::array<std::int64_t, 2> current{};
  const OptionTargetSpec spec = target_spec(OptionSizingBasis::Vega, 100.0);

  const auto result = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionTargetBook, NonFiniteMarginAggregateFailsClosed) {
  OptionPanelRow expensive = row(10U, 1U, 100);
  expensive.vega_per_contract = 1.0;
  expensive.initial_margin_per_contract = (std::numeric_limits<double>::max)();
  expensive.maintenance_margin_per_contract = (std::numeric_limits<double>::max)();
  const std::array<OptionPanelRow, 1> input{expensive};
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 1> weights{1.0};
  const std::array<std::int64_t, 1> current{};
  OptionTargetSpec spec = target_spec(OptionSizingBasis::Vega, 2.0);
  spec.available_initial_margin = (std::numeric_limits<double>::max)();

  const auto result = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

TEST(OptionTargetBook, MarginRejectLeavesCallerStateAndPanelReusable) {
  const std::array<OptionPanelRow, 1> input{row(10U, 1U, 100)};
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 1> weights{1.0};
  const std::array<std::int64_t, 1> current{3};
  const auto current_before = current;
  const std::vector<atx::options::research::OptionOutcomeLabel> outcomes_before{
      panel->outcomes().begin(), panel->outcomes().end()};
  OptionTargetSpec rejected_spec = target_spec(OptionSizingBasis::Vega, 100.0);
  rejected_spec.available_initial_margin = 999.0;
  rejected_spec.margin_policy = MarginLimitPolicy::RejectBatch;

  const auto rejected = make_option_target_book(*panel, 0U, weights, current, rejected_spec);

  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(current, current_before);
  ASSERT_EQ(panel->outcomes().size(), outcomes_before.size());
  for (std::size_t i = 0; i < outcomes_before.size(); ++i) {
    EXPECT_TRUE(panel->outcomes()[i] == outcomes_before[i]);
  }

  rejected_spec.available_initial_margin = 1'000.0;
  const auto accepted = make_option_target_book(*panel, 0U, weights, current, rejected_spec);
  ASSERT_TRUE(accepted) << accepted.error().to_string();
  ASSERT_EQ(accepted->targets.size(), 1U);
  EXPECT_EQ(accepted->targets.front().target_contracts, 10);
  EXPECT_EQ(accepted->targets.front().order_contracts, 7);
}

TEST(OptionTargetBook, MarginClampScalesTargetsProportionally) {
  std::vector<OptionPanelRow> input{
      row(10U, 1U, 100),
      row(20U, 2U, 100),
  };
  input[0].initial_margin_per_contract = 100.0;
  input[0].maintenance_margin_per_contract = 80.0;
  input[1].initial_margin_per_contract = 300.0;
  input[1].maintenance_margin_per_contract = 240.0;
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 2> weights{0.5, -0.5};
  const std::array<std::int64_t, 2> current{};
  OptionTargetSpec spec = target_spec(OptionSizingBasis::Vega, 200.0);
  spec.available_initial_margin = 2'000.0;
  spec.margin_policy = MarginLimitPolicy::ProportionalClamp;

  const auto book = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_TRUE(book) << book.error().to_string();
  ASSERT_EQ(book->targets.size(), 2U);
  EXPECT_TRUE(book->margin_clamped);
  EXPECT_EQ(book->targets[0].target_contracts, 5);
  EXPECT_EQ(book->targets[1].target_contracts, -5);
  EXPECT_DOUBLE_EQ(book->initial_margin, 2'000.0);
  EXPECT_DOUBLE_EQ(book->maintenance_margin, 1'600.0);
  EXPECT_DOUBLE_EQ(book->realized_gross_exposure, 100.0);
}

TEST(OptionTargetBook, NonTradableHeldContractProducesCloseIntent) {
  OptionPanelRow unavailable = row(10U, 1U, 100);
  unavailable.status = OptionPanelStatus::StaleQuote;
  const std::array<OptionPanelRow, 1> input{unavailable};
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array<double, 1> weights{0.0};
  const std::array<std::int64_t, 1> current{7};
  const OptionTargetSpec spec = target_spec(OptionSizingBasis::Vega, 1'000.0);

  const auto book = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_TRUE(book) << book.error().to_string();
  ASSERT_EQ(book->targets.size(), 1U);
  EXPECT_EQ(book->targets.front().current_contracts, 7);
  EXPECT_EQ(book->targets.front().target_contracts, 0);
  EXPECT_EQ(book->targets.front().order_contracts, -7);
}

TEST(OptionResearchBridge, RankWeightsProduceSymmetricVegaContractBook) {
  const std::vector<OptionPanelRow> input{
      row(40U, 4U, 100, 3.0),
      row(10U, 1U, 100, -3.0),
      row(30U, 3U, 100, 1.0),
      row(20U, 2U, 100, -1.0),
  };
  const auto panel = OptionResearchPanel::create(input);
  ASSERT_TRUE(panel) << panel.error().to_string();
  ASSERT_EQ(panel->instruments().size(), 4U);
  EXPECT_EQ(panel->instruments()[0].contract_id, 10U);
  EXPECT_EQ(panel->instruments()[1].contract_id, 20U);
  EXPECT_EQ(panel->instruments()[2].contract_id, 30U);
  EXPECT_EQ(panel->instruments()[3].contract_id, 40U);

  atx::engine::WeightPolicy policy;
  policy.transform = atx::engine::Transform::Rank;
  policy.winsorize_limit = 0.0;
  policy.truncation = 0.0;
  policy.dollar_neutral = true;
  policy.gross_leverage = 1.0;
  const auto weights = policy.to_target_weights(
      atx::engine::SignalView{panel->column(OptionPanelField::Signal)}, panel->universe());

  ASSERT_EQ(weights.size(), 4U);
  EXPECT_NEAR(weights[0], -3.0 / 8.0, 1.0e-12);
  EXPECT_NEAR(weights[1], -1.0 / 8.0, 1.0e-12);
  EXPECT_NEAR(weights[2], 1.0 / 8.0, 1.0e-12);
  EXPECT_NEAR(weights[3], 3.0 / 8.0, 1.0e-12);

  const std::array<std::int64_t, 4> current{};
  const OptionTargetSpec spec = target_spec(OptionSizingBasis::Vega, 800.0);
  const auto book = make_option_target_book(*panel, 0U, weights, current, spec);

  ASSERT_TRUE(book) << book.error().to_string();
  ASSERT_EQ(book->targets.size(), 4U);
  EXPECT_EQ(book->targets[0].contract_id, 10U);
  EXPECT_EQ(book->targets[0].target_contracts, -30);
  EXPECT_EQ(book->targets[1].contract_id, 20U);
  EXPECT_EQ(book->targets[1].target_contracts, -10);
  EXPECT_EQ(book->targets[2].contract_id, 30U);
  EXPECT_EQ(book->targets[2].target_contracts, 10);
  EXPECT_EQ(book->targets[3].contract_id, 40U);
  EXPECT_EQ(book->targets[3].target_contracts, 30);
  EXPECT_NEAR(book->requested_gross_exposure, 800.0, 1.0e-12);
  EXPECT_DOUBLE_EQ(book->realized_gross_exposure, 800.0);
}

} // namespace
