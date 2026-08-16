#include "backtest/quant_pipeline.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/api/fitting/surface_parity.hpp"  // SliceContext
#include "atx/vol/api/fitting/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"     // EssviParams
#include "atx/vol/api/pricing/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/api/storage/surface_db.hpp"      // SurfaceDb (vrp-backtest roots)

namespace atx::vol {
namespace {

[[nodiscard]] std::filesystem::path pipeline_test_root() {
  static std::atomic<std::uint64_t> sequence{0u};
  const auto root = std::filesystem::temp_directory_path() /
                    ("atx_quant_pipeline_" + std::to_string(sequence.fetch_add(1u)));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return root;
}

[[nodiscard]] BacktestSeriesInfo series_info(std::size_t rows) {
  BacktestSeriesInfo info;
  info.template_id = "three-month-strangle";
  info.template_fingerprint = 0x111u;
  info.symbol = "SPY";
  info.uid = 101u;
  info.row_count = rows;
  info.run_identity_hash = 0xabcdefu;
  info.partition_filename = "partition.atxrun";
  info.partition_identity = ArchiveContentIdentity{4096u, 123u, 456u, 789u};
  return info;
}

[[nodiscard]] BacktestSeriesData series_data(std::size_t rows) {
  BacktestSeriesData data;
  data.backtest.ts_ns.reserve(rows);
  data.backtest.date.reserve(rows);
  data.backtest.pnl_total.reserve(rows);
  std::vector<double> signal;
  signal.reserve(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    data.backtest.ts_ns.push_back(static_cast<std::int64_t>(i + 1u) * 100);
    data.backtest.date.push_back("2026-01-" + std::to_string(10u + i));
    data.backtest.pnl_total.push_back(i == 0u ? 0.0 : (i % 3u == 0u ? -2.0 : 3.0));
    signal.push_back(static_cast<double>(static_cast<int>(i % 5u) - 2));
  }
  data.backtest.signals.emplace_back("implied_correlation", std::move(signal));
  return data;
}

[[nodiscard]] BacktestSignalResearchSpec research_spec() {
  BacktestSignalResearchSpec spec;
  spec.signal_name = "implied_correlation";
  spec.lagged_capital = 1'000.0;
  spec.validation.min_train_groups = 8u;
  spec.validation.test_groups = 4u;
  spec.validation.step_groups = 4u;
  spec.newey_west_lag = 1u;
  spec.candidates = {
      ResearchSignalCandidate{"level-long", ResearchSignalTransform::Identity, 0u, 0u,
                              ResearchSignalDirection::LongHigh},
      ResearchSignalCandidate{"change-long", ResearchSignalTransform::Difference, 0u, 1u,
                              ResearchSignalDirection::LongHigh},
  };
  spec.family = ResearchTrialFamily{true, spec.candidates.size()};
  return spec;
}

TEST(QuantPipeline, MapsCloseSignalToNextObservationAndFollowingOutcome) {
  const BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  const auto result = mine_backtest_signal_series(info, data, research_spec());
  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->observations.size(), data.backtest.size() - 2u);
  const ResearchObservation &first = result->observations.front();
  EXPECT_EQ(first.decision_ts_ns, data.backtest.ts_ns[0]);
  EXPECT_EQ(first.execution_ts_ns, data.backtest.ts_ns[1]);
  EXPECT_EQ(first.label_end_ts_ns, data.backtest.ts_ns[2]);
  EXPECT_DOUBLE_EQ(first.forward_pnl, data.backtest.pnl_total[2]);
  EXPECT_EQ(first.source_identity, info.partition_identity);
  EXPECT_FALSE(result->validation.folds.empty());
  EXPECT_FALSE(result->mining.selected_candidate_id.empty());
}

TEST(QuantPipeline, RejectsUnknownOrNonParallelSignalAndUnsealedFamily) {
  BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  BacktestSignalResearchSpec spec = research_spec();
  spec.signal_name = "unknown";
  EXPECT_FALSE(mine_backtest_signal_series(info, data, spec));

  spec = research_spec();
  data.backtest.signals.front().second.pop_back();
  EXPECT_FALSE(mine_backtest_signal_series(info, data, spec));

  data = series_data(18u);
  spec.family.sealed = false;
  EXPECT_FALSE(mine_backtest_signal_series(info, data, spec));
}

TEST(QuantPipeline, PublishesImmutableOosTrialWithExactBacktestDependency) {
  const BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  const BacktestSignalResearchSpec spec = research_spec();
  auto result = mine_backtest_signal_series(info, data, spec);
  ASSERT_TRUE(result) << result.error().to_string();

  const std::filesystem::path root = pipeline_test_root();
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db) << db.error().to_string();
  auto published = publish_backtest_signal_trial(
      *db, info, spec, *result, ResearchTrialPublishSpec{"trials/spy/implied-correlation", {}});
  ASSERT_TRUE(published) << published.error().to_string();
  EXPECT_EQ(published->kind, ResearchArtifactKind::Trial);

  auto dependencies = db->load_dependencies(published->artifact_id);
  ASSERT_TRUE(dependencies) << dependencies.error().to_string();
  ASSERT_EQ(dependencies->size(), 1u);
  EXPECT_EQ(dependencies->front().archive_identity, info.partition_identity);

  auto returns = db->map_section(published->artifact_id, "oos_returns");
  ASSERT_TRUE(returns) << returns.error().to_string();
  EXPECT_GT(returns->view.n_rows(), 0u);
  EXPECT_EQ(returns->view.f64_col("value").size(), returns->view.n_rows());
  returns->view = {};
  returns->archive.reset();
  std::filesystem::remove_all(root);
}

TEST(QuantPipeline, PublicationRevalidatesLineageFoldsFamilyAndEvidence) {
  const BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  const BacktestSignalResearchSpec spec = research_spec();
  auto valid = mine_backtest_signal_series(info, data, spec);
  ASSERT_TRUE(valid) << valid.error().to_string();

  const std::filesystem::path root = pipeline_test_root();
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db) << db.error().to_string();
  const ResearchTrialPublishSpec publication{"trials/spy/audit", {}};

  BacktestSignalResearchResult mutated = *valid;
  mutated.observations.front().source_identity.header_crc32c += 1u;
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, spec, mutated, publication));

  mutated = *valid;
  mutated.validation.folds.front().train_indices.push_back(
      mutated.validation.folds.front().test_indices.front());
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, spec, mutated, publication));

  mutated = *valid;
  mutated.mining.evaluations.front().oos_returns.front().value += 1.0;
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, spec, mutated, publication));

  BacktestSignalResearchSpec unsealed = spec;
  unsealed.family.sealed = false;
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, unsealed, *valid, publication));
  EXPECT_TRUE(db->artifacts().empty());
  std::filesystem::remove_all(root);
}

TEST(QuantPipeline, MapsAuthoritativeDispersionBookPositionsToSymbols) {
  DispersionBook book;
  book.index_leg.symbol = "SPX";
  book.index_leg.uid = 1u;
  book.index_leg.straddle_qty = -2.0;
  DispersionLeg name;
  name.symbol = "AAPL";
  name.uid = 2u;
  name.straddle_qty = 3.0;
  book.name_legs.push_back(name);
  book.positions = {
      Position{1u, OptionContract{1u, 100.0, 0.25, Side::Call}, -2.0, 100.0},
      Position{2u, OptionContract{1u, 100.0, 0.25, Side::Put}, -2.0, 100.0},
      Position{3u, OptionContract{2u, 50.0, 0.25, Side::Call}, 3.0, 100.0},
      Position{4u, OptionContract{2u, 50.0, 0.25, Side::Put}, 3.0, 100.0},
  };
  auto mapped = dispersion_named_positions(book);
  ASSERT_TRUE(mapped) << mapped.error().to_string();
  ASSERT_EQ(mapped->size(), 4u);
  EXPECT_EQ((*mapped)[0].symbol, "SPX");
  EXPECT_EQ((*mapped)[1].symbol, "SPX");
  EXPECT_EQ((*mapped)[2].symbol, "AAPL");
  EXPECT_EQ((*mapped)[3].symbol, "AAPL");

  book.positions.back().qty = 4.0;
  EXPECT_FALSE(dispersion_named_positions(book));
}

TEST(QuantPipeline, ComposesTargetRiskScenarioClampAndResearchIntent) {
  const std::vector<NamedPosition> target = {
      NamedPosition{"SPX", Position{1u, OptionContract{1u, 100.0, 0.25, Side::Put}, 2.0, 100.0}},
      NamedPosition{"AAPL", Position{2u, OptionContract{2u, 50.0, 0.25, Side::Call}, -1.0, 100.0}},
  };
  AmericanGreeks index_greeks;
  index_greeks.price = 5.0;
  index_greeks.delta = 0.5;
  index_greeks.gamma = 0.01;
  index_greeks.vega = 2.0;
  index_greeks.theta = -0.1;
  AmericanGreeks name_greeks = index_greeks;
  name_greeks.price = 3.0;
  name_greeks.delta = -0.5;

  const std::vector<PositionRiskInput> risk = {
      PositionRiskInput{target[0].position, index_greeks},
      PositionRiskInput{target[1].position, name_greeks},
  };
  const std::vector<ScenarioRiskInput> scenarios = {
      ScenarioRiskInput{target[0].position, 100.0, index_greeks},
      ScenarioRiskInput{target[1].position, 50.0, name_greeks},
  };
  StrategyImplementationSpec spec;
  spec.strategy_fingerprint = 0x1234u;
  spec.decision_ts_ns = 1'000;
  spec.risk_limits.max_abs_delta = 75.0;
  ConditionalComponentScenario scenario;
  scenario.index_uid = 1u;
  scenario.index_spot_pct = -0.05;
  scenario.index_vol_bump = 0.02;
  scenario.components = {ComponentShockModel{2u, 1.2, 0.0, 1.0, 0.0}};
  spec.scenarios.push_back(scenario);
  spec.hedge_targets.push_back(HedgeTarget{1u, 0.0, 100.0});

  auto plan = build_strategy_implementation_plan(target, {}, risk, scenarios, spec);
  ASSERT_TRUE(plan) << plan.error().to_string();
  EXPECT_DOUBLE_EQ(plan->unconstrained_risk.delta, 150.0);
  EXPECT_DOUBLE_EQ(plan->risk_overlay.scale, 0.5);
  ASSERT_EQ(plan->risk_adjusted_target.size(), 2u);
  EXPECT_DOUBLE_EQ(plan->risk_adjusted_target[0].position.qty, 1.0);
  EXPECT_DOUBLE_EQ(plan->risk_adjusted_target[1].position.qty, -0.5);
  ASSERT_EQ(plan->scenario_results.size(), 1u);
  EXPECT_EQ(plan->intent.disposition, IntentDisposition::ResearchOnly);
  ASSERT_EQ(plan->intent.option_orders.size(), 2u);
  ASSERT_EQ(plan->intent.hedges.size(), 1u);
  EXPECT_DOUBLE_EQ(plan->intent.hedges.front().target_shares, 50.0);
}

TEST(QuantPipeline, ImplementationPlanRequiresScenarioEvidence) {
  const NamedPosition target{"SPX",
                             Position{1u, OptionContract{1u, 100.0, 0.25, Side::Put}, 2.0, 100.0}};
  AmericanGreeks greeks;
  greeks.price = 5.0;
  const std::vector<PositionRiskInput> risk = {
      PositionRiskInput{target.position, greeks},
  };

  StrategyImplementationSpec spec;
  spec.strategy_fingerprint = 0x1234u;
  spec.decision_ts_ns = 1'000;
  EXPECT_FALSE(build_strategy_implementation_plan(std::span{&target, 1u}, {}, risk, {}, spec));

  const std::vector<ScenarioRiskInput> orphaned_scenario_input = {
      ScenarioRiskInput{target.position, 100.0, greeks},
  };
  EXPECT_FALSE(build_strategy_implementation_plan(std::span{&target, 1u}, {}, risk,
                                                  orphaned_scenario_input, spec));
}

TEST(QuantPipeline, AlignsImplementationInputsByStableIdAndGreekSnapshot) {
  const std::vector<NamedPosition> target = {
      NamedPosition{"SPX", Position{1u, OptionContract{1u, 100.0, 0.25, Side::Put}, 2.0, 100.0}},
      NamedPosition{"AAPL", Position{2u, OptionContract{2u, 50.0, 0.25, Side::Call}, -1.0, 100.0}},
  };
  AmericanGreeks index_greeks;
  index_greeks.price = 5.0;
  index_greeks.delta = 0.5;
  AmericanGreeks name_greeks;
  name_greeks.price = 3.0;
  name_greeks.delta = -0.5;

  const std::vector<PositionRiskInput> reversed_risk = {
      PositionRiskInput{target[1].position, name_greeks},
      PositionRiskInput{target[0].position, index_greeks},
  };
  std::vector<ScenarioRiskInput> reversed_scenario_inputs = {
      ScenarioRiskInput{target[1].position, 50.0, name_greeks},
      ScenarioRiskInput{target[0].position, 100.0, index_greeks},
  };
  StrategyImplementationSpec spec;
  spec.strategy_fingerprint = 0x1234u;
  spec.decision_ts_ns = 1'000;
  ConditionalComponentScenario scenario;
  scenario.index_uid = 1u;
  scenario.index_spot_pct = -0.05;
  scenario.index_vol_bump = 0.02;
  scenario.components = {ComponentShockModel{2u, 1.2, 0.0, 1.0, 0.0}};
  spec.scenarios.push_back(scenario);

  EXPECT_TRUE(build_strategy_implementation_plan(target, {}, reversed_risk,
                                                 reversed_scenario_inputs, spec));

  reversed_scenario_inputs[0].greeks_per_share.delta = -0.4;
  EXPECT_FALSE(build_strategy_implementation_plan(target, {}, reversed_risk,
                                                  reversed_scenario_inputs, spec));

  reversed_scenario_inputs[0].greeks_per_share = name_greeks;
  reversed_scenario_inputs[0].position.qty = -2.0;
  EXPECT_FALSE(build_strategy_implementation_plan(target, {}, reversed_risk,
                                                  reversed_scenario_inputs, spec));
}

// ── vrp-backtest subcommand seam (lane vrp-book) ────────────────────────────

namespace {

// A real on-disk vrp_signal_v1 file so the parse stage's existence check and
// the loader's schema check are both exercised against genuine files.
[[nodiscard]] std::filesystem::path write_signal_fixture(const std::filesystem::path &root,
                                                         const std::string &body) {
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  const std::filesystem::path path = root / "signal.tsv";
  std::ofstream out(path, std::ios::binary);
  out << body;
  out.close();
  return path;
}

[[nodiscard]] std::string valid_signal_body() {
  std::string body{kVrpSignalSchemaLineV1};
  body += '\n';
  body += kVrpSignalHeaderV1;
  body += "\nAAA\t2026-01-05\t1\t0.5\t0.8\nBBB\t2026-01-05\t-1\t-0.5\t1.2\n";
  return body;
}

} // namespace

TEST(QuantPipeline, VrpBacktestArgsParseDefaultsAndOverrides) {
  const std::filesystem::path root = pipeline_test_root();
  const std::filesystem::path signal = write_signal_fixture(root, valid_signal_body());
  // Runtime-composed dummy roots (never opened by the parse stage): a source
  // literal would trip the shared-resolver path-literal guard, and rightly so.
  const std::string db_a = (root / "spy-2025").string();
  const std::string db_b = (root / "spy-2026").string();

  const std::vector<std::string> args = {
      "--signal",        signal.string(), "--surface-db",   db_a,
      "--surface-db",    db_b,            "--report",       (root / "report.tsv").string(),
      "--from",          "2026-01-02",    "--to",           "2026-06-30",
      "--risk-budget",   "2500",          "--vov-floor",    "0.1",
      "--vega-cap",      "10000",         "--net-short-tilt", "0.2",
      "--no-trade-band", "0.25",          "--cost-vol-pts", "1.5",
      "--stock-bps",     "3",             "--hedge-band",   "50",
      "--rebalance-steps", "5",           "--horizon-days", "10",
      "--long-frac",     "0.2",           "--short-frac",   "0.3",
      "--expiry-guard",  "4",
  };
  const auto spec = parse_vrp_backtest_args(args);
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  EXPECT_EQ(spec->signal_path, signal.string());
  ASSERT_EQ(spec->surface_db_roots.size(), 2u);
  EXPECT_EQ(spec->surface_db_roots[1], db_b);
  EXPECT_EQ(spec->date_lo, "2026-01-02");
  EXPECT_EQ(spec->date_hi, "2026-06-30");
  EXPECT_DOUBLE_EQ(spec->config.risk_budget_vega, 2500.0);
  EXPECT_DOUBLE_EQ(spec->config.vov_floor, 0.1);
  EXPECT_DOUBLE_EQ(spec->config.per_name_vega_cap, 10000.0);
  EXPECT_DOUBLE_EQ(spec->config.net_short_tilt, 0.2);
  EXPECT_DOUBLE_EQ(spec->config.no_trade_band, 0.25);
  EXPECT_DOUBLE_EQ(spec->config.cost_half_spread_vol_pts, 1.5);
  EXPECT_DOUBLE_EQ(spec->config.stock_half_spread_bps, 3.0);
  EXPECT_DOUBLE_EQ(spec->config.delta_hedge_band, 50.0);
  EXPECT_EQ(spec->config.rebalance_every_n_steps, 5u);
  EXPECT_DOUBLE_EQ(spec->config.horizon_days, 10.0);
  EXPECT_DOUBLE_EQ(spec->config.expiry_guard_days, 4.0);
  EXPECT_TRUE(spec->config.delta_hedge);

  // --no-hedge is the one value-free flag.
  const std::vector<std::string> hedgeless = {"--signal", signal.string(), "--surface-db",
                                              db_a, "--report",
                                              (root / "r.tsv").string(), "--no-hedge"};
  const auto hedgeless_spec = parse_vrp_backtest_args(hedgeless);
  ASSERT_TRUE(hedgeless_spec.has_value()) << hedgeless_spec.error().to_string();
  EXPECT_FALSE(hedgeless_spec->config.delta_hedge);
  std::filesystem::remove_all(root);
}

TEST(QuantPipeline, VrpBacktestArgsFailClosedOnMissingSignalFile) {
  const std::filesystem::path root = pipeline_test_root();
  const std::filesystem::path signal = write_signal_fixture(root, valid_signal_body());
  const std::string report = (root / "report.tsv").string();
  // Runtime-composed dummy root — see the path-literal note in the parse test.
  const std::string db = (root / "db").string();

  // No --signal flag at all.
  const auto no_flag = parse_vrp_backtest_args(
      std::vector<std::string>{"--surface-db", db, "--report", report});
  ASSERT_FALSE(no_flag.has_value());
  EXPECT_EQ(no_flag.error().code(), ErrorCode::InvalidArgument);

  // --signal names a file that does not exist: NotFound at PARSE time.
  const auto missing_file = parse_vrp_backtest_args(std::vector<std::string>{
      "--signal", (root / "nope.tsv").string(), "--surface-db", db, "--report", report});
  ASSERT_FALSE(missing_file.has_value());
  EXPECT_EQ(missing_file.error().code(), ErrorCode::NotFound);

  // A real signal file but no surface-db root / no report / an unknown flag /
  // a bad numeric — each fails closed with InvalidArgument.
  const auto no_root = parse_vrp_backtest_args(
      std::vector<std::string>{"--signal", signal.string(), "--report", report});
  ASSERT_FALSE(no_root.has_value());
  EXPECT_EQ(no_root.error().code(), ErrorCode::InvalidArgument);

  const auto no_report = parse_vrp_backtest_args(
      std::vector<std::string>{"--signal", signal.string(), "--surface-db", db});
  ASSERT_FALSE(no_report.has_value());
  EXPECT_EQ(no_report.error().code(), ErrorCode::InvalidArgument);

  const auto unknown = parse_vrp_backtest_args(
      std::vector<std::string>{"--signal", signal.string(), "--surface-db", db,
                               "--report", report, "--frobnicate", "1"});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code(), ErrorCode::InvalidArgument);

  const auto bad_numeric = parse_vrp_backtest_args(
      std::vector<std::string>{"--signal", signal.string(), "--surface-db", db,
                               "--report", report, "--risk-budget", "lots"});
  ASSERT_FALSE(bad_numeric.has_value());
  EXPECT_EQ(bad_numeric.error().code(), ErrorCode::InvalidArgument);

  // A dangling flag with no value fails closed too.
  const auto dangling = parse_vrp_backtest_args(
      std::vector<std::string>{"--signal", signal.string(), "--surface-db", db,
                               "--report", report, "--vov-floor"});
  ASSERT_FALSE(dangling.has_value());
  EXPECT_EQ(dangling.error().code(), ErrorCode::InvalidArgument);

  // An invalid config combination is refused at parse time as well.
  const auto bad_config = parse_vrp_backtest_args(
      std::vector<std::string>{"--signal", signal.string(), "--surface-db", db,
                               "--report", report, "--long-frac", "0.9"});
  ASSERT_FALSE(bad_config.has_value());
  EXPECT_EQ(bad_config.error().code(), ErrorCode::InvalidArgument);

  // A guard at or above the tenor's calendar days can never hold a book:
  // refused at parse time by the same config validation.
  const auto bad_guard = parse_vrp_backtest_args(
      std::vector<std::string>{"--signal", signal.string(), "--surface-db", db,
                               "--report", report, "--horizon-days", "5",
                               "--expiry-guard", "7.3"});
  ASSERT_FALSE(bad_guard.has_value());
  EXPECT_EQ(bad_guard.error().code(), ErrorCode::InvalidArgument);
  std::filesystem::remove_all(root);
}

namespace {

// The strategy_test synthetic-eSSVI fixture (flat forward, genuine American
// premium), reproduced here so the vrp-backtest seam can be driven over a
// REAL SurfaceDb root end to end.
[[nodiscard]] PricedSurface vrp_test_surface(std::uint32_t uid, std::int64_t now_ts,
                                             double vol_bump) {
  constexpr double kRate = 0.043;
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double tenors[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : tenors) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = 100.0;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kRate * T)));
    ctx.push_back(SliceContext{T, 100.0, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = 100.0;
  pc.r = kRate;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto surface = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(surface.has_value())
      << (surface.has_value() ? std::string{} : surface.error().to_string());
  return std::move(*surface);
}

} // namespace

TEST(QuantPipeline, RunVrpBacktestWritesTheReportOverARealSurfaceDbRoot) {
  const std::filesystem::path root = pipeline_test_root();
  const std::filesystem::path db_root = root / "surface-db";
  auto db = SurfaceDb::create(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();

  constexpr std::int64_t kNow0 = 1'700'000'000'000'000'000LL; // 2023-11-14 UTC
  constexpr std::int64_t kDay = 86'400'000'000'000LL;
  std::vector<std::string> dates;
  for (int d = 0; d < 2; ++d) {
    const std::int64_t now = kNow0 + static_cast<std::int64_t>(d) * kDay;
    const PricedSurface aaa = vrp_test_surface(11u, now, 0.0);
    const PricedSurface bbb = vrp_test_surface(12u, now, 0.01);
    const std::vector<SurfaceArchiveItem> items = {SurfaceArchiveItem{"AAA", &aaa},
                                                   SurfaceArchiveItem{"BBB", &bbb}};
    const std::string date = vol_edge_session_date(now);
    dates.push_back(date);
    const Status written = db->write_partition(date, items);
    ASSERT_TRUE(written.has_value()) << written.error().to_string();
  }

  std::string signal_body{kVrpSignalSchemaLineV1};
  signal_body += '\n';
  signal_body += kVrpSignalHeaderV1;
  signal_body += "\nAAA\t" + dates[0] + "\t1\t2\t0.8\nBBB\t" + dates[0] + "\t-1\t-2\t1.2\n";

  VrpBacktestSpec spec;
  spec.signal_path = write_signal_fixture(root, signal_body).string();
  spec.surface_db_roots = {db_root.string()};
  spec.report_path = (root / "report.tsv").string();
  spec.config.long_fraction = 0.5;
  spec.config.short_fraction = 0.5;
  spec.config.risk_budget_vega = 5'000.0;
  spec.config.vov_floor = 0.05;
  spec.config.cost_half_spread_vol_pts = 1.0;
  spec.config.delta_hedge = false;
  spec.config.rebalance_every_n_steps = 100; // one entry, hold to run end

  const auto summary = run_vrp_backtest(spec);
  ASSERT_TRUE(summary.has_value()) << summary.error().to_string();
  EXPECT_EQ(summary->n_rows, 2u);
  EXPECT_TRUE(std::isfinite(summary->final_nav));
  EXPECT_GT(summary->total_cost, 0.0) << "the configured half-spread must charge";
  // A clean two-session hold: no fail-soft or fail-safe path fired, and the
  // summary says so explicitly (the hardening counters are surfaced, not
  // inferred from silence).
  EXPECT_EQ(summary->n_held_steps, 0u);
  EXPECT_EQ(summary->n_skipped_names, 0u);
  EXPECT_EQ(summary->n_roll_closes, 0u);

  // The report exists, carries the promised columns, and is row-parallel.
  std::ifstream report(spec.report_path);
  ASSERT_TRUE(report.is_open());
  std::string header;
  ASSERT_TRUE(static_cast<bool>(std::getline(report, header)));
  EXPECT_NE(header.find("nav"), std::string::npos);
  EXPECT_NE(header.find("pnl_gamma"), std::string::npos);
  EXPECT_NE(header.find("ledger_collected"), std::string::npos);
  EXPECT_NE(header.find("ledger_repriced"), std::string::npos);
  EXPECT_NE(header.find("turnover_vega"), std::string::npos);
  EXPECT_NE(header.find("cost"), std::string::npos);
  EXPECT_NE(header.find("vol_edge_n_long"), std::string::npos);
  EXPECT_NE(header.find("vol_edge_held_steps"), std::string::npos);
  EXPECT_NE(header.find("vol_edge_roll_closed"), std::string::npos);
  std::size_t data_rows = 0;
  std::string line;
  while (std::getline(report, line)) {
    if (!line.empty()) {
      ++data_rows;
      EXPECT_EQ(line.find(dates[data_rows - 1u]), 0u) << "rows follow the clock order";
    }
  }
  EXPECT_EQ(data_rows, 2u);
  report.close();
  std::filesystem::remove_all(root);
}

TEST(QuantPipeline, RunVrpBacktestFailsClosedBeforeTouchingRoots) {
  const std::filesystem::path root = pipeline_test_root();

  // A signal file that exists but breaks the frozen schema: the run must die
  // in the loader, before any surface-db root is opened (the fake root would
  // fail with a DIFFERENT error if it were reached first).
  VrpBacktestSpec bad_schema;
  bad_schema.signal_path =
      write_signal_fixture(root, "symbol\tdate\tpred_label\tpred_edge_norm\tvov_63d\n").string();
  bad_schema.surface_db_roots = {(root / "no-such-db").string()};
  bad_schema.report_path = (root / "report.tsv").string();
  const auto schema_result = run_vrp_backtest(bad_schema);
  ASSERT_FALSE(schema_result.has_value());
  EXPECT_EQ(schema_result.error().code(), ErrorCode::InvalidArgument);

  // A valid signal against a nonexistent root: NotFound from SurfaceDb::open.
  VrpBacktestSpec bad_root;
  bad_root.signal_path = write_signal_fixture(root / "ok", valid_signal_body()).string();
  bad_root.surface_db_roots = {(root / "no-such-db").string()};
  bad_root.report_path = (root / "report.tsv").string();
  const auto root_result = run_vrp_backtest(bad_root);
  ASSERT_FALSE(root_result.has_value());
  EXPECT_EQ(root_result.error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

} // namespace
} // namespace atx::vol
