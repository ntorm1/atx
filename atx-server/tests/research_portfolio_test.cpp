#include "research_dataset_fixture.hpp"

#include <cmath>

namespace {
class PortfolioResearchTest : public DatasetResearchTest {
protected:
  Json portfolio() const {
    auto input = request();
    input["mode"] = "real_portfolio";
    input.erase("strategy_dsl");
    input["components"] = Json::array({
        {{"strategy_dsl", "returns"}, {"expression_sha256", digest("returns")}, {"weight", 0.5}},
        {{"strategy_dsl", "rank(returns)"},
         {"expression_sha256", digest("rank(returns)")}, {"weight", 0.5}}});
    input["gross_limit"] = 1;
    input["name_cap"] = 0.2;
    input["max_turnover"] = 0.3;
    input["cadence"] = 1;
    return input;
  }

  void component(Json& input, std::size_t index, const std::string& expression, double weight) {
    input["components"][index] = {{"strategy_dsl", expression},
        {"expression_sha256", digest(expression)}, {"weight", weight}};
  }

  void reject(const Json& input, std::string_view reason) {
    try {
      (void)execute(input);
      FAIL() << "invalid portfolio unexpectedly admitted";
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos);
    }
  }
};

TEST_F(PortfolioResearchTest, NativePortfolioCombinesSignalsAndCertifiesActualDelayedAllocations) {
  const auto result = execute(portfolio());
  EXPECT_EQ(result.at("mode"), "real_portfolio");
  const auto& provenance = result.at("provenance");
  EXPECT_FALSE(provenance.at("synthetic").get<bool>());
  EXPECT_EQ(provenance.at("portfolio_geometry"), "identity_signal_projection_not_estimated_risk");
  EXPECT_EQ(provenance.at("blend_rule"), "StandardizedFixedGrossV2");
  EXPECT_EQ(provenance.at("components").size(), 2);
  EXPECT_EQ(provenance.at("execution_delay_sessions"), 1);
  EXPECT_LE(provenance.at("resource_admission").at("aggregate_estimated_work_units"),
            provenance.at("resource_admission").at("aggregate_work_budget"));
  const auto& allocations = result.at("portfolio_allocations");
  ASSERT_FALSE(allocations.empty());
  for (const auto& allocation : allocations) {
    EXPECT_NEAR(allocation.at("target_net").get<double>(), 0, 1e-6);
    EXPECT_LE(allocation.at("target_gross").get<double>(), 1 + 1e-6);
    EXPECT_LE(allocation.at("target_max_name").get<double>(), 0.2 + 1e-6);
    EXPECT_LE(allocation.at("target_turnover").get<double>(), 0.3 + 1e-6);
    EXPECT_LE(allocation.at("solver_primal_residual").get<double>(), 1e-5);
    EXPECT_FALSE(allocation.at("solver_primal_infeasible").get<bool>());
    EXPECT_LT(allocation.at("decision_date").get<std::string>(),
              allocation.at("execution_date").get<std::string>());
  }
  EXPECT_GT(result.at("metrics").at("final_nav").get<double>(), 1e6);
}

TEST_F(PortfolioResearchTest, OpposingIdenticalSignalsCancelToFlatCashBook) {
  auto input = portfolio();
  component(input, 0, "returns", 0.5);
  component(input, 1, "returns", -0.5);
  const auto result = execute(input);
  EXPECT_DOUBLE_EQ(result.at("metrics").at("total_return").get<double>(), 0);
  EXPECT_TRUE(result.at("trades").empty());
  EXPECT_TRUE(result.at("metrics").at("sharpe_ratio").is_null());
}

TEST_F(PortfolioResearchTest, UndefinedOrConstantOpinionsDoNotInventTradingEvidence) {
  for (const auto& expression : {"close/0", "0"}) {
    auto input = portfolio();
    component(input, 0, expression, 0.5);
    component(input, 1, expression, 0.5);
    const auto result = execute(input);
    EXPECT_DOUBLE_EQ(result.at("metrics").at("final_nav").get<double>(), 1e6);
    EXPECT_TRUE(result.at("trades").empty());
    EXPECT_TRUE(result.at("metrics").at("sharpe_ratio").is_null());
  }
}

TEST_F(PortfolioResearchTest, ComponentExpressionPinsAndStrictWeightsCannotBeForged) {
  auto input = portfolio();
  input["components"][0]["expression_sha256"] = std::string(64, '0');
  reject(input, "expression SHA256");
  for (const auto& weight : Json::array({nullptr, true, "0.5", 1e100})) {
    input = portfolio();
    input["components"][0]["weight"] = weight;
    reject(input, "component weight");
  }
}

TEST_F(PortfolioResearchTest, ComponentCountUnknownMetadataAndUnsupportedGroupsRejected) {
  auto input = portfolio();
  input["components"] = Json::array();
  reject(input, "2..12");
  input = portfolio();
  input["components"][0]["revision_id"] = "unverified-reference";
  reject(input, "unknown portfolio component");
  input = portfolio();
  input["neutralization"] = "sector";
  reject(input, "dollar");
}

TEST_F(PortfolioResearchTest, TurnoverUsesDriftedMarkedHoldingsRatherThanPriorTarget) {
  auto input = portfolio();
  input["max_turnover"] = 0.1;
  const auto result = execute(input);
  const auto& rows = result.at("portfolio_allocations");
  ASSERT_GE(rows.size(), 2);
  bool drifted = false;
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto previous = rows[i - 1].at("target_weights").get<std::vector<double>>();
    const auto held = rows[i].at("pretrade_held_weights").get<std::vector<double>>();
    ASSERT_EQ(previous.size(), held.size());
    for (std::size_t j = 0; j < held.size(); ++j)
      drifted = drifted || std::abs(previous[j] - held[j]) > 1e-8;
    EXPECT_LE(rows[i].at("target_turnover").get<double>(), 0.1 + 1e-6);
  }
  EXPECT_TRUE(drifted);
}

TEST_F(PortfolioResearchTest, FutureObservationsCannotChangeEarlierPortfolioAllocations) {
  const auto prior = execute(portfolio());
  rewrite_month("year=2020/panel-02.parquet",
      "UPDATE revision SET ret=-ret WHERE session_date>=DATE '2020-02-12'");
  const auto revised = execute(portfolio());
  const auto& before = prior.at("portfolio_allocations");
  const auto& after = revised.at("portfolio_allocations");
  ASSERT_EQ(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i)
    if (before[i].at("execution_date").get<std::string>() < "2020-02-12")
      EXPECT_EQ(before[i], after[i]);
}

TEST_F(PortfolioResearchTest, InfeasibleCompulsoryExitCannotSilentlyBreakTurnoverOrEligibility) {
  rewrite_month("year=2020/panel-02.parquet",
      "UPDATE revision SET member_equity=false WHERE security_id=4");
  auto input = portfolio();
  input["cadence"] = 5;
  input["max_turnover"] = 0.02;
  input["name_cap"] = 1;
  try {
    (void)execute(input);
    FAIL() << "compulsory exit unexpectedly admitted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("compulsory exit/dollar neutrality"), std::string::npos);
  }
}

TEST_F(PortfolioResearchTest, DelayedReplayCancellationPublishesNoPartialPortfolio) {
  std::stop_source cancellation;
  const auto progress = [&](double fraction, std::string_view) {
    if (fraction > 0.6) cancellation.request_stop();
  };
  EXPECT_THROW((void)atx::server::ResearchBackend{options}.execute(portfolio().dump(),
      cancellation.get_token(), progress), std::runtime_error);
  EXPECT_TRUE(cancellation.stop_requested());
}

TEST_F(PortfolioResearchTest, CausalDecayRaisesWarmupWithoutChangingFutureInvariance) {
  auto input = portfolio();
  input["signal_decay_sessions"] = 5;
  const auto result = execute(input);
  EXPECT_EQ(result.at("provenance").at("warmup_sessions"), 4);
  EXPECT_EQ(result.at("provenance").at("signal_decay_sessions"), 5);
  input["signal_decay_sessions"] = 1.5;
  EXPECT_THROW(execute(input), std::invalid_argument);
}

TEST_F(PortfolioResearchTest, AggregateReservationRejectsBeforeEvaluatingAnyConstituent) {
  options.max_working_bytes = 32ULL * 1024ULL * 1024ULL;
  auto input = portfolio();
  while (input["components"].size() < 12)
    input["components"].push_back(input["components"].front());
  bool evaluated = false;
  const auto progress = [&](double, std::string_view stage) {
    evaluated = evaluated || stage.find("constituent") != std::string_view::npos;
  };
  try {
    (void)atx::server::ResearchBackend{options}.execute(input.dump(), {}, progress);
    FAIL() << "aggregate portfolio memory reservation unexpectedly admitted";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("aggregate working memory"), std::string::npos);
  }
  EXPECT_FALSE(evaluated);
}

TEST_F(PortfolioResearchTest, CostAndBoundedDiagnosticsRetainCompleteBookEvidence) {
  options.max_result_points = 2;
  auto input = portfolio();
  input["trade_bps"] = 6;
  const auto result = execute(input);
  ASSERT_EQ(result.at("portfolio_allocations").size(), 2);
  const auto& provenance = result.at("provenance");
  EXPECT_TRUE(provenance.at("portfolio_allocations_truncated").get<bool>());
  EXPECT_TRUE(provenance.at("trades_truncated").get<bool>());
  EXPECT_TRUE(provenance.at("daily_returns_complete").get<bool>());
  EXPECT_EQ(result.at("daily_returns").size(), result.at("metrics").at("observations"));
  for (const auto& row : result.at("portfolio_allocations")) {
    EXPECT_GT(row.at("trade_cost").get<double>(), 0);
    EXPECT_LT(row.at("posttrade_nav").get<double>(), row.at("pretrade_nav").get<double>());
    EXPECT_GE(row.at("achieved_posttrade_gross").get<double>(), row.at("target_gross").get<double>());
  }
}

TEST_F(PortfolioResearchTest, AggregateSolverWorkIsRefusedBeforeConstituentEvaluation) {
  for (const auto& name : {"year=2019/panel-12.parquet", "year=2020/panel-01.parquet",
                           "year=2020/panel-02.parquet"})
    rewrite_month(name, "INSERT INTO revision SELECT * EXCLUDE(r) REPLACE "
        "((security_id+4*r)::BIGINT AS security_id) FROM revision CROSS JOIN range(1,64) q(r)");
  auto input = portfolio();
  input["instrument_limit"] = 256;
  bool evaluated = false;
  const auto progress = [&](double, std::string_view stage) {
    evaluated = evaluated || stage.find("constituent") != std::string_view::npos;
  };
  try {
    (void)atx::server::ResearchBackend{options}.execute(input.dump(), {}, progress);
    FAIL() << "unbounded aggregate solver work unexpectedly admitted";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("aggregate solver work"), std::string::npos);
  }
  EXPECT_FALSE(evaluated);
}

TEST_F(PortfolioResearchTest, BroadVolatileCrossSectionsCertifyDelayedCapsAndTurnover) {
  for (const auto& name : {"year=2019/panel-12.parquet", "year=2020/panel-01.parquet",
                           "year=2020/panel-02.parquet"})
    rewrite_month(name, "INSERT INTO revision SELECT * EXCLUDE(r) REPLACE "
        "((security_id+4*r)::BIGINT AS security_id) FROM revision CROSS JOIN range(1,32) q(r); "
        "UPDATE revision SET ret=0.008*sin(security_id*0.37+epoch(session_date)/86400*0.05)");
  auto input = portfolio();
  input["instrument_limit"] = 128;
  input["name_cap"] = 0.02;
  input["max_turnover"] = 1;
  input["cadence"] = 5;
  input["signal_decay_sessions"] = 3;
  component(input, 0, "-rank(delta(close,5)/delay(close,5))", .5);
  component(input, 1, "-rank(delta(close,10)/delay(close,10))", .5);
  const auto result = execute(input);
  ASSERT_EQ(result.at("provenance").at("instruments"), 128);
  ASSERT_GT(result.at("portfolio_allocations").size(), 2);
  for (const auto& row : result.at("portfolio_allocations")) {
    EXPECT_NEAR(row.at("target_net").get<double>(), 0, 1e-6);
    EXPECT_LE(row.at("target_gross").get<double>(), 1 + 1e-6);
    EXPECT_LE(row.at("target_max_name").get<double>(), .02 + 1e-6);
    EXPECT_LE(row.at("target_turnover").get<double>(), 1 + 1e-6);
  }
}
} // namespace
