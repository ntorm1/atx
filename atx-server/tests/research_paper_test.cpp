#include "research_dataset_fixture.hpp"

#include <cmath>

namespace {
class PaperResearchTest : public DatasetResearchTest {
protected:
  atx::server::PaperReplayInput input(std::uint32_t through = 1) const {
    auto frozen = request();
    frozen["mode"] = "real_portfolio";
    frozen.erase("strategy_dsl");
    frozen["start_date"] = "2019-12-02";
    frozen["end_date"] = "2020-01-17";
    frozen["components"] = Json::array({
        {{"strategy_dsl", "returns"}, {"expression_sha256", digest("returns")}, {"weight", .5}},
        {{"strategy_dsl", "rank(returns)"},
         {"expression_sha256", digest("rank(returns)")}, {"weight", .5}}});
    frozen["cadence"] = 1;
    frozen["name_cap"] = .2;
    frozen["max_turnover"] = .3;
    frozen["trade_bps"] = 6;
    frozen["annual_borrow_bps"] = 300;
    atx::server::PaperReplayInput result{frozen.dump(), "2020-01-17", "2020-01-20",
                                       "2020-02-14", through, {}};
    if (through > 1) result.expected_cohort_security_ids = {1, 2, 3, 4};
    return result;
  }

  atx::server::PaperReplayResult replay(const atx::server::PaperReplayInput& request) const {
    return atx::server::ResearchBackend{options}.replay_paper(request, {}, {});
  }

  void reject(const atx::server::PaperReplayInput& request, std::string_view reason) const {
    try { (void)replay(request); FAIL() << "invalid paper input accepted"; }
    catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos);
    }
  }
};

TEST_F(PaperResearchTest, GenesisIsOneCompletedCashValuationWithStableCalendar) {
  const auto result = replay(input());
  EXPECT_EQ(result.window_calendar.size(), 20);
  EXPECT_EQ(result.window_calendar.front(), "2020-01-20");
  EXPECT_EQ(result.window_calendar.back(), "2020-02-14");
  EXPECT_EQ(result.cohort_security_ids, (std::vector<std::uint64_t>{1, 2, 3, 4}));
  ASSERT_EQ(result.valuations.size(), 1);
  EXPECT_DOUBLE_EQ(result.valuations.front().cash, 1e6);
  EXPECT_DOUBLE_EQ(result.valuations.front().nav, 1e6);
  EXPECT_TRUE(result.allocations.empty());
  EXPECT_TRUE(result.fills.empty());
  EXPECT_TRUE(result.financing.empty());
  for (const auto& position : result.final_positions) EXPECT_DOUBLE_EQ(position.tri_units, 0);
  const auto receipt = Json::parse(result.receipt_json);
  EXPECT_EQ(receipt.at("schema"), "atx.paper-replay/v1");
  EXPECT_FALSE(receipt.at("synthetic").get<bool>());
  EXPECT_EQ(receipt.at("evaluation"), "TRAIN");
}

TEST_F(PaperResearchTest, OneObservationPaperProfileDoesNotRequireFutureSessions) {
  auto request = input();
  request.paper_end_date = request.paper_start_date;
  const auto result = replay(request);
  ASSERT_EQ(result.window_calendar.size(), 1);
  ASSERT_EQ(result.valuations.size(), 1);
  EXPECT_DOUBLE_EQ(result.valuations.front().nav, 1e6);
}

TEST_F(PaperResearchTest, EveryBoundedPrefixMatchesTheSameBatchReplayExactly) {
  const auto batch = replay(input(20));
  ASSERT_EQ(batch.valuations.size(), 20);
  for (std::uint32_t count = 1; count <= 20; ++count) {
    const auto prefix = replay(input(count));
    EXPECT_EQ(prefix.window_calendar, batch.window_calendar);
    ASSERT_EQ(prefix.valuations.size(), count);
    for (std::size_t i = 0; i < count; ++i) EXPECT_EQ(prefix.valuations[i], batch.valuations[i]);
    for (std::size_t i = 0; i < prefix.allocations.size(); ++i)
      EXPECT_EQ(prefix.allocations[i], batch.allocations[i]);
    for (std::size_t i = 0; i < prefix.fills.size(); ++i) EXPECT_EQ(prefix.fills[i], batch.fills[i]);
    for (std::size_t i = 0; i < prefix.financing.size(); ++i)
      EXPECT_EQ(prefix.financing[i], batch.financing[i]);
    EXPECT_EQ(prefix.final_positions, prefix.valuations.back().positions);
  }
}

TEST_F(PaperResearchTest, DelayedFillRequiresAFollowingValuationAndPreservesNativeUnits) {
  EXPECT_TRUE(replay(input(2)).fills.empty());
  const auto result = replay(input(3));
  ASSERT_EQ(result.allocations.size(), 1);
  EXPECT_EQ(result.allocations.front().decision_session_index, 0);
  EXPECT_EQ(result.allocations.front().execution_session_index, 1);
  ASSERT_FALSE(result.fills.empty());
  for (const auto& fill : result.fills) {
    EXPECT_EQ(fill.session_index, 1);
    EXPECT_EQ(fill.decision_session_index, 0);
    EXPECT_DOUBLE_EQ(fill.old_tri_units, 0);
    EXPECT_DOUBLE_EQ(fill.signed_dollar_delta, fill.new_tri_units * fill.tri_mark);
    EXPECT_DOUBLE_EQ(fill.modeled_fee, std::abs(fill.signed_dollar_delta) * .0006);
  }
}

TEST_F(PaperResearchTest, CashPositionsFeesAndWeekendFinancingReconcileEveryInterval) {
  const auto result = replay(input(20));
  double cash = 1e6;
  bool weekend = false;
  for (std::size_t end = 1; end < result.valuations.size(); ++end) {
    const auto& valuation = result.valuations[end];
    double fees = 0;
    for (const auto& fill : result.fills) if (fill.session_index == end - 1) {
      cash -= fill.signed_dollar_delta;
      fees += fill.modeled_fee;
    }
    cash -= fees;
    const auto& financing = result.financing.at(end - 1);
    cash -= financing.borrow_charge;
    weekend = weekend || financing.calendar_days == 3;
    EXPECT_NEAR(cash, valuation.cash, 1e-8);
    EXPECT_NEAR(fees, valuation.trade_fees, 1e-9);
    EXPECT_DOUBLE_EQ(financing.borrow_charge, valuation.borrow_charge);
    EXPECT_EQ(financing.day_basis, 365);
    double assets = 0, gross = 0;
    for (const auto& position : valuation.positions) {
      assets += position.marked_dollars;
      gross += std::abs(position.marked_dollars);
    }
    EXPECT_DOUBLE_EQ(assets, valuation.assets);
    EXPECT_DOUBLE_EQ(gross, valuation.gross);
    EXPECT_DOUBLE_EQ(valuation.nav, valuation.cash + valuation.assets);
    EXPECT_NEAR(valuation.nav, result.valuations[end - 1].nav + valuation.gross_pnl -
                valuation.trade_fees - valuation.borrow_charge, 1e-8);
  }
  EXPECT_TRUE(weekend);
}

TEST_F(PaperResearchTest, EvidenceBoundaryTrainCohortAndClockGuardsRejectSpecifically) {
  auto request = input(); request.paper_start_date = "2020-01-17";
  reject(request, "strictly after");
  request = input(); request.paper_start_date = "2023-01-02";
  request.paper_end_date = "2023-01-31"; reject(request, "TRAIN");
  request = input(0); reject(request, "through_session_count");
  request = input(64); reject(request, "through_session_count");
  request = input(21); reject(request, "calendar");
  request = input(3); request.expected_cohort_security_ids.clear(); reject(request, "cohort");
  request = input(3); request.expected_cohort_security_ids = {4, 3, 2, 1};
  reject(request, "cohort");
  request = input(); auto frozen = Json::parse(request.approved_portfolio_request_json);
  frozen["instrument_limit"] = 129; request.approved_portfolio_request_json = frozen.dump();
  reject(request, "128");
}

TEST_F(PaperResearchTest, CancellationPublishesNoPartialOwnedResult) {
  std::stop_source stop; stop.request_stop();
  EXPECT_THROW((void)atx::server::ResearchBackend{options}.replay_paper(input(), stop.get_token(),
      {}), std::runtime_error);
  std::stop_source during;
  EXPECT_THROW((void)atx::server::ResearchBackend{options}.replay_paper(input(20),
      during.get_token(), [&](double fraction, std::string_view) {
        if (fraction >= .6) during.request_stop();
      }), std::runtime_error);
}

TEST_F(PaperResearchTest, FuturePayloadMutationCannotAlterEarlierOwnedLedgerRows) {
  const auto rolling = [&] {
    auto request = input(8);
    auto frozen = Json::parse(request.approved_portfolio_request_json);
    const std::array<std::string, 2> expressions{
        "ts_mean(returns,3)", "ts_mean(rank(returns),3)"};
    for (std::size_t i = 0; i < expressions.size(); ++i) {
      frozen["components"][i]["strategy_dsl"] = expressions[i];
      frozen["components"][i]["expression_sha256"] = digest(expressions[i]);
    }
    request.approved_portfolio_request_json = frozen.dump();
    return request;
  };
  const auto before = replay(rolling());
  rewrite_month("year=2020/panel-02.parquet",
      "UPDATE revision SET ret=0.15, raw_close=raw_close*3 WHERE session_date>='2020-02-12'");
  const auto after = replay(rolling());
  EXPECT_EQ(before.valuations, after.valuations);
  EXPECT_EQ(before.allocations, after.allocations);
  EXPECT_EQ(before.fills, after.fills);
  EXPECT_EQ(before.financing, after.financing);
  EXPECT_EQ(before.final_positions, after.final_positions);
}

TEST_F(PaperResearchTest, MissingHeldMarkOwnsExplicitAdverseLiquidationAndReconcilesCash) {
  rewrite_month("year=2020/panel-01.parquet",
      "UPDATE revision SET ret_guarded=true WHERE session_date='2020-01-24' AND security_id=4");
  const auto result = replay(input(5));
  ASSERT_EQ(result.liquidations.size(), 1);
  const auto& liquidation = result.liquidations.front();
  EXPECT_EQ(liquidation.session_index, 4);
  EXPECT_EQ(liquidation.security_id, 4);
  EXPECT_TRUE(liquidation.assumed);
  EXPECT_TRUE(liquidation.flagged);
  EXPECT_EQ(liquidation.return_source, "assumed_missing_price_adverse");
  EXPECT_LE(liquidation.signed_proceeds - liquidation.last_marked_dollars, 0);
  EXPECT_DOUBLE_EQ(liquidation.signed_proceeds,
                   liquidation.last_marked_dollars * (1 + liquidation.applied_return));
  const auto& previous = result.valuations.at(3);
  const auto& terminal = result.valuations.at(4);
  double cash = previous.cash + liquidation.signed_proceeds - terminal.borrow_charge;
  for (const auto& fill : result.fills) if (fill.session_index == 3)
    cash -= fill.signed_dollar_delta + fill.modeled_fee;
  EXPECT_NEAR(cash, terminal.cash, 1e-8);
  for (const auto& position : terminal.positions)
    if (position.security_id == 4) EXPECT_DOUBLE_EQ(position.tri_units, 0);
}

TEST_F(PaperResearchTest, MissingExecutionMarkDisclosesUnfilledTargetWithoutInventingFill) {
  rewrite_month("year=2020/panel-01.parquet",
      "UPDATE revision SET ret_guarded=true WHERE session_date='2020-01-21' AND security_id=4");
  const auto result = replay(input(3));
  ASSERT_EQ(result.unfilled_targets.size(), 1);
  EXPECT_EQ(result.unfilled_targets.front().security_id, 4);
  EXPECT_EQ(result.unfilled_targets.front().session_index, 1);
  for (const auto& fill : result.fills) EXPECT_NE(fill.security_id, 4);
  for (const auto& position : result.final_positions)
    if (position.security_id == 4) EXPECT_DOUBLE_EQ(position.tri_units, 0);
}

TEST_F(PaperResearchTest, NativePaperLedgerIsCompleteEvenWithTwoPointResearchSamplingLimit) {
  options.max_result_points = 2;
  const auto result = replay(input(20));
  EXPECT_EQ(result.valuations.size(), 20);
  EXPECT_EQ(result.financing.size(), 19);
  EXPECT_EQ(result.allocations.size(), 18);
  EXPECT_GT(result.fills.size(), 2);
  for (const auto& row : result.valuations) EXPECT_EQ(row.positions.size(), 4);
  const auto receipt = Json::parse(result.receipt_json);
  EXPECT_FALSE(receipt.at("portfolio_allocations_truncated").get<bool>());
  EXPECT_FALSE(receipt.at("trades_truncated").get<bool>());
}

TEST_F(PaperResearchTest, FullOwnedLedgerStorageIsAdmittedBeforeEvaluation) {
  options.max_working_bytes = 35ULL * 1024ULL * 1024ULL;
  reject(input(), "full owned ledger");
}

TEST_F(PaperResearchTest, WholeCalendarAbove63SessionsRejectsEvenAtCashOnlyGenesis) {
  rewrite_month("year=2020/panel-02.parquet",
      "INSERT INTO revision SELECT d::DATE, id::BIGINT, 100.0::DOUBLE, 999.0::DOUBLE, "
      "1000000.0::DOUBLE, ((id-2.5)*0.002)::DOUBLE, false, true, true, "
      "(50000000.0+id)::DOUBLE FROM generate_series(DATE '2020-02-17', DATE '2020-02-28', "
      "INTERVAL '1 DAY') q(d) CROSS JOIN range(1,5) n(id) WHERE isodow(d)<6");
  auto request = input();
  auto frozen = Json::parse(request.approved_portfolio_request_json);
  frozen["start_date"] = "2019-11-01"; frozen["end_date"] = "2019-11-29";
  request.approved_portfolio_request_json = frozen.dump();
  request.approved_evidence_end_date = "2019-11-29";
  request.paper_start_date = "2019-12-02"; request.paper_end_date = "2020-02-28";
  reject(request, "1..63 sessions");
}

TEST_F(PaperResearchTest, PaperFormationUsesNewStartCohortUnderTheUnchangedPriorAdvRule) {
  rewrite_month("year=2020/panel-01.parquet",
      "UPDATE revision SET adv63=100000000.0+security_id WHERE session_date='2020-01-20' "
      "AND security_id IN (1,2)");
  auto request = input();
  auto frozen = Json::parse(request.approved_portfolio_request_json);
  frozen["instrument_limit"] = 2;
  request.approved_portfolio_request_json = frozen.dump();
  const auto result = replay(request);
  EXPECT_EQ(result.cohort_security_ids, (std::vector<std::uint64_t>{1, 2}));
  const auto receipt = Json::parse(result.receipt_json);
  EXPECT_EQ(receipt.at("formation_date"), "2020-01-20");
  auto effective = receipt.at("effective_recipe");
  effective["start_date"] = frozen.at("start_date");
  effective["end_date"] = frozen.at("end_date");
  EXPECT_EQ(effective, frozen);
  EXPECT_EQ(receipt.at("approved_recipe_sha256"), digest(frozen.dump()));
}

TEST_F(PaperResearchTest, FrozenModeShapeExpressionAndDatasetPinsAreStrictlyAdmitted) {
  auto request = input(); auto frozen = Json::parse(request.approved_portfolio_request_json);
  frozen["mode"] = "synthetic_demo";
  request.approved_portfolio_request_json = frozen.dump(); reject(request, "real_portfolio");
  request = input(); frozen = Json::parse(request.approved_portfolio_request_json);
  frozen["unverified_revision_id"] = "alpha-42";
  request.approved_portfolio_request_json = frozen.dump(); reject(request, "unknown paper");
  request = input(); frozen = Json::parse(request.approved_portfolio_request_json);
  frozen["components"][0]["expression_sha256"] = std::string(64, '0');
  request.approved_portfolio_request_json = frozen.dump(); reject(request, "expression SHA256");
  request = input(); frozen = Json::parse(request.approved_portfolio_request_json);
  frozen["dataset_sha256"] = std::string(64, '0');
  request.approved_portfolio_request_json = frozen.dump(); reject(request, "registered pin");
  request = input(); request.approved_portfolio_request_json = "{\"mode\":false}";
  reject(request, "invalid paper frozen recipe");
  request = input(); request.paper_end_date = "2020-01-19"; reject(request, "reversed");
}

TEST_F(PaperResearchTest, ReceiptRetainsInjectedExecutingRuntimeContentIdentity) {
  options.runtime_build_sha256 = std::string(64, 'b');
  const auto result = replay(input());
  const auto receipt = Json::parse(result.receipt_json);
  EXPECT_EQ(receipt.at("runtime_build_sha256"), options.runtime_build_sha256);
  EXPECT_TRUE(receipt.at("runtime_build_identity_available").get<bool>());
}

} // namespace
