#include "research_dataset_fixture.hpp"

namespace {
TEST_F(DatasetResearchTest, PublishedTrainParquetUsesNativeDelayedBookAccounting) {
  const auto result = execute(request());
  EXPECT_EQ(result.at("mode"), "real_dsl");
  const auto& provenance = result.at("provenance");
  EXPECT_FALSE(provenance.at("synthetic").get<bool>());
  EXPECT_EQ(provenance.at("dataset_manifest_sha256"), options.datasets.front().manifest_sha256);
  EXPECT_EQ(provenance.at("evaluation"), "TRAIN");
  EXPECT_FALSE(provenance.at("historical_vintage_verified").get<bool>());
  const auto& returns = result.at("daily_returns");
  ASSERT_GE(returns.size(), 3);
  EXPECT_DOUBLE_EQ(returns.at(0).at("value").get<double>(), 0.0);
  EXPECT_NEAR(returns.at(1).at("value").get<double>(), 0.0025, 1e-12);
  EXPECT_GT(result.at("metrics").at("total_return").get<double>(), 0);
  EXPECT_FALSE(result.at("trades").empty());
}

TEST_F(DatasetResearchTest, DatasetDiscoveryExposesPinnedFieldsAndSafeOperators) {
  const auto catalog = Json::parse(atx::server::ResearchBackend{options}.strategies_json());
  ASSERT_EQ(catalog.at("datasets").size(), 1);
  EXPECT_EQ(catalog.at("datasets").at(0).at("manifest_sha256"),
            options.datasets.front().manifest_sha256);
  EXPECT_FALSE(catalog.at("operators").empty());
  EXPECT_FALSE(catalog.at("datasets").at(0).at("fields").empty());
}

TEST_F(DatasetResearchTest, ChangedDatasetRevisionCannotExecuteQueuedRecipe) {
  auto input = request();
  input["dataset_sha256"] = std::string(64, '0');
  try { (void)execute(input); FAIL() << "changed revision admitted"; }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("revision SHA256"), std::string::npos);
  }
}

TEST_F(DatasetResearchTest, HoldoutDateRejectedBeforePayloadRead) {
  auto input = request();
  input["end_date"] = "2023-01-01";
  std::filesystem::remove(root / "year=2020/panel-01.parquet");
  try { (void)execute(input); FAIL() << "holdout admitted"; }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("TRAIN"), std::string::npos);
  }
}

TEST_F(DatasetResearchTest, ReversedDateWindowRejectedBeforePayloadRead) {
  auto input = request();
  input["start_date"] = "2020-02-14";
  input["end_date"] = "2020-01-20";
  try { (void)execute(input); FAIL() << "reversed window admitted"; }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("reversed"), std::string::npos);
  }
}

TEST_F(DatasetResearchTest, TamperedTrainPayloadFailsHashVerification) {
  std::ofstream output(root / "year=2020/panel-01.parquet", std::ios::binary | std::ios::app);
  output << 'x';
  output.close();
  try { (void)execute(request()); FAIL() << "tampered payload admitted"; }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("extent"), std::string::npos);
  }
}

TEST_F(DatasetResearchTest, ManifestTraversalFailsBeforeReadingAnOutsideFile) {
  manifest["files"]["../outside.parquet"] = {{"bytes", 1}, {"sha256", std::string(64, 'a')}};
  pin_manifest();
  try { (void)execute(request()); FAIL() << "traversal admitted"; }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("dataset file path"), std::string::npos);
  }
}

TEST_F(DatasetResearchTest, NonIntegralManifestReceiptRejectedWithoutConversion) {
  for (const auto& value : Json::array({1e100, 1.5, -1, true, "1", nullptr})) {
    manifest["files"]["year=2020/panel-01.parquet"]["bytes"] = value;
    pin_manifest();
    try { (void)execute(request()); FAIL() << "non-integral receipt admitted"; }
    catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("nonnegative integer"), std::string::npos);
    }
  }
}

TEST_F(DatasetResearchTest, DatasetCannotExposeUnprovenFieldsAsPointInTimeInputs) {
  auto input = request();
  input["strategy_dsl"] = "rank(gics)";
  EXPECT_THROW(execute(input), std::invalid_argument);
}

TEST_F(DatasetResearchTest, CancelledJobDoesNotLoadDatasetPayload) {
  std::stop_source cancellation;
  cancellation.request_stop();
  std::filesystem::remove(root / "year=2020/panel-01.parquet");
  EXPECT_THROW((void)atx::server::ResearchBackend{options}.execute(request().dump(),
      cancellation.get_token(), {}), std::runtime_error);
}

TEST_F(DatasetResearchTest, PublishedBackwardScaledCloseDoesNotAffectCausalSignal) {
  auto input = request();
  input["strategy_dsl"] = "rank(delta(close,5)/delay(close,5))";
  const auto prior = execute(input);
  rewrite_month("year=2020/panel-02.parquet", "UPDATE revision SET close=1e100");
  input["dataset_sha256"] = options.datasets.front().manifest_sha256;
  const auto revised = execute(input);
  EXPECT_EQ(prior.at("daily_returns"), revised.at("daily_returns"));
  EXPECT_EQ(prior.at("metrics"), revised.at("metrics"));
}

TEST_F(DatasetResearchTest, FutureObservedChangesCannotAlterEarlierResearchIntervals) {
  const auto prior = execute(request());
  rewrite_month("year=2020/panel-02.parquet",
      "UPDATE revision SET ret=-ret WHERE session_date>=DATE '2020-02-12'");
  const auto revised = execute(request());
  const auto& before = prior.at("daily_returns");
  const auto& after = revised.at("daily_returns");
  ASSERT_EQ(before.size(), after.size());
  bool changed = false;
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (before[i].at("date").get<std::string>() < "2020-02-12") EXPECT_EQ(before[i], after[i]);
    else changed = changed || before[i].at("value") != after[i].at("value");
  }
  EXPECT_TRUE(changed);
}

TEST_F(DatasetResearchTest, CostsAreChargedByNativeAccountingAndReturnsKeepExactIntervals) {
  const auto free = execute(request());
  auto input = request();
  input["trade_bps"] = 20;
  input["annual_borrow_bps"] = 1000;
  const auto charged = execute(input);
  EXPECT_LT(charged.at("metrics").at("final_nav").get<double>(),
            free.at("metrics").at("final_nav").get<double>());
  const auto& returns = charged.at("daily_returns");
  EXPECT_EQ(returns.at(0).at("period_start_date"), "2020-01-20");
  EXPECT_EQ(returns.at(0).at("date"), "2020-01-21");
  EXPECT_TRUE(charged.at("provenance").at("daily_returns_complete").get<bool>());
}

TEST_F(DatasetResearchTest, CancellationDuringPortfolioReplayPublishesNoPartialResult) {
  std::stop_source cancellation;
  const auto progress = [&](double fraction, std::string_view) {
    if (fraction > 0.6) cancellation.request_stop();
  };
  EXPECT_THROW((void)atx::server::ResearchBackend{options}.execute(request().dump(),
      cancellation.get_token(), progress), std::runtime_error);
  EXPECT_TRUE(cancellation.stop_requested());
}

TEST_F(DatasetResearchTest, GuardedHeldReturnTriggersDeclaredNativeAdverseLiquidation) {
  rewrite_month("year=2020/panel-02.parquet",
      "UPDATE revision SET ret_guarded=true WHERE security_id=4 "
      "AND session_date=DATE '2020-02-06'");
  const auto result = execute(request());
  const auto& provenance = result.at("provenance");
  EXPECT_EQ(provenance.at("guarded_return_rows"), 1);
  EXPECT_EQ(provenance.at("tri_segment_resets"), 1);
  EXPECT_GE(provenance.at("assumed_liquidations").get<std::size_t>(), 1);
  EXPECT_LT(provenance.at("assumed_liquidation_pnl").get<double>(), 0);
  EXPECT_EQ(provenance.at("nav_return_reconciliation").at("status"), "passed");
}

TEST_F(DatasetResearchTest, LaterMembershipRemovalKeepsInitialCohortAndHeldMarks) {
  rewrite_month("year=2020/panel-02.parquet",
      "UPDATE revision SET member_equity=false WHERE security_id=4");
  const auto result = execute(request());
  EXPECT_EQ(result.at("provenance").at("instruments"), 4);
  EXPECT_EQ(result.at("provenance").at("assumed_liquidations"), 0);
  const auto& positions = result.at("positions");
  ASSERT_EQ(positions.size(), 4);
  EXPECT_EQ(positions.at(3).at("instrument"), "SR:4");
  EXPECT_DOUBLE_EQ(positions.at(3).at("tri_units").get<double>(), 0.0);
}
} // namespace
