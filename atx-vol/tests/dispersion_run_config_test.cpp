// WS-X regression suite for the strict typed dispersion run config and the
// execution-realism knobs it turns on.
//
//   X1 — one typed DispersionRunConfig the run spec STRICTLY deserializes into:
//        unknown keys are rejected BY NAME instead of silently ignored, which is
//        the bug class that let ~14 of the ~20 spec keys do nothing.
//   X2 — frictions + financing actually reach the engine (they never did).
//   X6 — spread + square-root market-impact fill model.
//
// Everything here defaults to the pinned frictionless golden; each test that
// exercises realism opts in explicitly.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/dispersion_run.hpp"
#include "atx/vol/types.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path write_spec(const char *leaf, const std::string &body) {
  const fs::path dir = fs::temp_directory_path() / leaf;
  std::error_code error;
  fs::remove_all(dir, error);
  fs::create_directories(dir, error);
  const fs::path path = dir / "run_spec.tsv";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
  out.close();
  return path;
}

// VERBATIM copy of C:\atx-data\spy-dispersion\runs\bt-sota-baseline\run_spec.tsv,
// the spec behind the pinned 82-session golden (final_nav = 247.4065016443293).
// Every one of its keys is legitimate, so strict parsing must accept all of them.
constexpr const char *kBaselineSpec = "key\tvalue\n"
                                      "label\tSPY listed-options dispersion bt-sota baseline\n"
                                      "date_lo\t2026-01-02\n"
                                      "date_hi\t2026-04-30\n"
                                      "snapshot_suffix\tT19:55:00Z\n"
                                      "opra_root\tC:\\atx-data\\spy-dispersion\\opra\n"
                                      "path_template\t{symbol}/{date}.parquet\n"
                                      "universe_schedule\tuniverse_schedule.tsv\n"
                                      "definitions\tdefinitions.tsv\n"
                                      "flat_rate\t0.043\n"
                                      "min_names\t10\n"
                                      "min_weight_coverage\t0.8\n"
                                      "target_dte_days\t30\n"
                                      "min_dte_days\t21\n"
                                      "max_dte_days\t60\n"
                                      "roll_dte_days\t7\n"
                                      "gross_index_vega\t10000\n"
                                      "delta_band\t0\n"
                                      "fit_workers\t0\n"
                                      "core_mode\t0\n";

} // namespace

// ── X1: the compatibility test — the pinned spec parses clean ────────────────

TEST(DispersionRunConfigStrict, PinnedBaselineSpecParsesUnchanged) {
  const fs::path path = write_spec("atx-disp-cfg-baseline", kBaselineSpec);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  EXPECT_EQ(config->dates.lo, "2026-01-02");
  EXPECT_EQ(config->dates.hi, "2026-04-30");
  EXPECT_EQ(config->snapshot_suffix, "T19:55:00Z");
  EXPECT_EQ(config->rate.flat_rate, 0.043);
  EXPECT_EQ(config->universe.min_names, 10u);
  EXPECT_EQ(config->dte.target_days, 30.0);
  EXPECT_EQ(config->dte.min_days, 21.0);
  EXPECT_EQ(config->dte.max_days, 60.0);
  EXPECT_EQ(config->roll_dte_days, 7.0);
  EXPECT_EQ(config->gross_index_vega, 10000.0);
  EXPECT_EQ(config->hedge.band, 0.0);
  EXPECT_FALSE(config->fit.core_mode);
  // Relative paths resolve against the spec's own directory.
  EXPECT_EQ(config->universe.schedule_path.filename(), "universe_schedule.tsv");
  EXPECT_TRUE(config->universe.schedule_path.is_absolute());

  // ... and the defaults are exactly the frictionless golden, so a spec that
  // names none of the new knobs reproduces the pin.
  EXPECT_EQ(config->frictions.spread_kind, FrictionModel::SpreadKind::None);
  EXPECT_EQ(config->frictions.half_spread_bps, 0.0);
  EXPECT_EQ(config->frictions.per_contract_cost, 0.0);
  EXPECT_EQ(config->financing.borrow_rate, 0.0);
  EXPECT_FALSE(config->financing.finance_premium);
  EXPECT_FALSE(config->financing.shares_carry);
  EXPECT_EQ(config->costs.k, 0.0);
  EXPECT_EQ(config->costs.adv_fraction, 0.0);
  EXPECT_FALSE(config->limits.any());
  EXPECT_EQ(config->multiplier, 100.0);
  EXPECT_EQ(config->universe.index_symbol, "SPY");
  EXPECT_EQ(config->side, DispersionSide::ShortIndexLongNames);
  EXPECT_EQ(config->hedge.kind, HedgeSpec::Kind::DeltaToZero);

  // And the assembled backtest config carries the golden's engine settings.
  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*config);
  EXPECT_EQ(backtest.run.frictions.spread_kind, FrictionModel::SpreadKind::None);
  EXPECT_EQ(backtest.run.financing.borrow_rate, 0.0);
  EXPECT_FALSE(backtest.run.financing.finance_premium);
  EXPECT_EQ(backtest.multiplier, 100.0);
  EXPECT_EQ(backtest.entry_every_n, 21u);
}

// ── X1: the strictness that did not exist before ────────────────────────────

TEST(DispersionRunConfigStrict, UnknownKeyIsRejectedByName) {
  // A plausible typo for `gross_index_vega`. Pre-X1 this parsed "successfully"
  // and the run silently used the default size.
  const std::string body = std::string(kBaselineSpec) + "gross_vega\t25000\n";
  const fs::path path = write_spec("atx-disp-cfg-unknown", body);

  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_FALSE(config) << "an unknown key must not parse";
  const std::string message = config.error().to_string();
  EXPECT_NE(message.find("gross_vega"), std::string::npos)
      << "the error must NAME the offending key, got: " << message;
}

TEST(DispersionRunConfigStrict, SeveralUnknownKeysAreAllNamed) {
  const std::string body =
      std::string(kBaselineSpec) + "slippage_bps\t5\n" + "vega_target\t100\n";
  const fs::path path = write_spec("atx-disp-cfg-unknown-many", body);

  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_FALSE(config);
  const std::string message = config.error().to_string();
  EXPECT_NE(message.find("slippage_bps"), std::string::npos) << message;
  EXPECT_NE(message.find("vega_target"), std::string::npos) << message;
}

TEST(DispersionRunConfigStrict, UnsupportedEnumValueIsRejectedAndLostsTheLegalOnes) {
  // `equal_vega` was the unimplemented probe here; WS-X4 (bb5a144) shipped it as
  // a real weighting scheme, so it now parses. Probe with a value genuinely
  // outside the legal set {vega_neutral, equal_vega, gamma_neutral,
  // theta_neutral} so this still exercises the reject-and-list-the-legal-ones path.
  const std::string body = std::string(kBaselineSpec) + "weighting\tinverse_vega\n";
  const fs::path path = write_spec("atx-disp-cfg-enum", body);

  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_FALSE(config) << "an unimplemented weighting scheme must fail loudly";
  const std::string message = config.error().to_string();
  EXPECT_NE(message.find("weighting"), std::string::npos) << message;
  EXPECT_NE(message.find("vega_neutral"), std::string::npos)
      << "the error should list the supported values, got: " << message;
}

TEST(DispersionRunConfigStrict, DuplicateKeyIsRejected) {
  const std::string body = std::string(kBaselineSpec) + "roll_dte_days\t14\n";
  const fs::path path = write_spec("atx-disp-cfg-dup", body);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_FALSE(config);
  EXPECT_NE(config.error().to_string().find("roll_dte_days"), std::string::npos);
}

TEST(DispersionRunConfigStrict, MalformedNumberNamesTheKey) {
  const std::string body = std::string(kBaselineSpec) + "cost_impact_beta\tnot-a-number\n";
  const fs::path path = write_spec("atx-disp-cfg-nan", body);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_FALSE(config);
  EXPECT_NE(config.error().to_string().find("cost_impact_beta"), std::string::npos);
}

TEST(DispersionRunConfigStrict, OutOfContractRiskLimitIsRejected) {
  // drawdown_stop is a FRACTION; 25 is a units error, not a 25% stop.
  const std::string body = std::string(kBaselineSpec) + "limit_drawdown_stop\t25\n";
  const fs::path path = write_spec("atx-disp-cfg-dd", body);
  ASSERT_FALSE(read_dispersion_run_config(path));
}

TEST(DispersionRunConfigStrict, DrawdownStopRequiresACapitalBase) {
  // NAV is cumulative P&L from zero, so a drawdown stop measured against "peak
  // NAV" would be degenerate. The config refuses the ambiguous combination
  // instead of silently picking a meaningless base.
  const std::string alone = std::string(kBaselineSpec) + "limit_drawdown_stop\t0.2\n";
  const Result<DispersionRunConfig> without =
      read_dispersion_run_config(write_spec("atx-disp-cfg-dd-alone", alone));
  ASSERT_FALSE(without);
  EXPECT_NE(without.error().to_string().find("limit_capital"), std::string::npos)
      << without.error().to_string();

  const std::string paired = std::string(kBaselineSpec) + "limit_drawdown_stop\t0.2\n" +
                             "limit_capital\t1000000\n";
  const Result<DispersionRunConfig> with =
      read_dispersion_run_config(write_spec("atx-disp-cfg-dd-paired", paired));
  ASSERT_TRUE(with) << with.error().to_string();
  EXPECT_EQ(with->limits.drawdown_stop, 0.2);
  EXPECT_EQ(with->limits.capital, 1000000.0);
}

// ── X2: frictions + financing now reach the engine ──────────────────────────

TEST(DispersionRunConfigStrict, FrictionAndFinancingKeysReachTheEngineConfig) {
  const std::string body = std::string(kBaselineSpec) +
                           "friction_spread_kind\tprice_bps\n"
                           "friction_half_spread_bps\t30\n"
                           "friction_per_contract_cost\t0.65\n"
                           "friction_hedge_slippage_bps\t2\n"
                           "financing_borrow_rate\t0.005\n"
                           "financing_finance_premium\ttrue\n"
                           "financing_shares_carry\t1\n"
                           "financing_initial_cash\t1000000\n";
  const fs::path path = write_spec("atx-disp-cfg-frictions", body);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*config);
  EXPECT_EQ(backtest.run.frictions.spread_kind, FrictionModel::SpreadKind::PriceBps);
  EXPECT_EQ(backtest.run.frictions.half_spread_bps, 30.0);
  EXPECT_EQ(backtest.run.frictions.per_contract_cost, 0.65);
  EXPECT_EQ(backtest.run.frictions.hedge_slippage_bps, 2.0);
  EXPECT_EQ(backtest.run.financing.borrow_rate, 0.005);
  EXPECT_TRUE(backtest.run.financing.finance_premium);
  EXPECT_TRUE(backtest.run.financing.shares_carry);
  EXPECT_EQ(backtest.run.financing.initial_cash, 1000000.0);
}

TEST(DispersionRunConfigStrict, NamedRealisticPresetIsSelectableAndOverridable) {
  const std::string body =
      std::string(kBaselineSpec) + "friction_preset\tretail_listed_options\n";
  const fs::path path = write_spec("atx-disp-cfg-preset", body);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  const FrictionModel expected =
      dispersion_friction_preset(DispersionFrictionPreset::RetailListedOptions);
  EXPECT_EQ(config->frictions.spread_kind, expected.spread_kind);
  EXPECT_EQ(config->frictions.half_spread_bps, expected.half_spread_bps);
  EXPECT_GT(config->frictions.half_spread_bps, 0.0);
  EXPECT_GT(config->frictions.per_contract_cost, 0.0);

  // An explicit key refines the preset rather than being overwritten by it.
  const std::string tuned = std::string(kBaselineSpec) +
                            "friction_preset\tretail_listed_options\n"
                            "friction_half_spread_bps\t12.5\n";
  const fs::path tuned_path = write_spec("atx-disp-cfg-preset-tuned", tuned);
  const Result<DispersionRunConfig> tuned_config = read_dispersion_run_config(tuned_path);
  ASSERT_TRUE(tuned_config) << tuned_config.error().to_string();
  EXPECT_EQ(tuned_config->frictions.half_spread_bps, 12.5);
  EXPECT_EQ(tuned_config->frictions.per_contract_cost, expected.per_contract_cost);
}

TEST(DispersionRunConfigStrict, FlatRateReachesFinancingOnlyWhenOptedIn) {
  // The misrouting this flag exposes: `flat_rate` fed the fit batch only, so a
  // run could declare r = 4.3% and still accrue exactly zero carry.
  const fs::path off = write_spec("atx-disp-cfg-rate-off", kBaselineSpec);
  const Result<DispersionRunConfig> without = read_dispersion_run_config(off);
  ASSERT_TRUE(without) << without.error().to_string();
  EXPECT_EQ(dispersion_backtest_config_from(*without).run.financing.borrow_rate, 0.0);

  const std::string body = std::string(kBaselineSpec) + "rate_applies_to_financing\t1\n";
  const fs::path on = write_spec("atx-disp-cfg-rate-on", body);
  const Result<DispersionRunConfig> with = read_dispersion_run_config(on);
  ASSERT_TRUE(with) << with.error().to_string();
  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*with);
  EXPECT_EQ(backtest.run.financing.borrow_rate, 0.043);
  EXPECT_TRUE(backtest.run.financing.finance_premium);
}

// ── X6: spread + square-root impact ─────────────────────────────────────────

TEST(DispersionCostModelTest, ZeroCoefficientsCollapseToTheMidFill) {
  const DispersionCostModel none;
  EXPECT_FALSE(none.active());
  EXPECT_EQ(fill_price(+1.0, 2.50, 0.0, 0.0, none), 2.50);
  EXPECT_EQ(fill_price(-1.0, 2.50, 0.0, 0.0, none), 2.50);

  // ... and folding it into a friction model is the identity.
  FrictionModel base;
  base.spread_kind = FrictionModel::SpreadKind::PriceBps;
  base.half_spread_bps = 25.0;
  const FrictionModel folded = dispersion_effective_frictions(base, none);
  EXPECT_EQ(folded.spread_kind, base.spread_kind);
  EXPECT_EQ(folded.half_spread_bps, base.half_spread_bps);
}

TEST(DispersionCostModelTest, BuysPayAndSellsReceiveTheSpreadPlusImpact) {
  DispersionCostModel model;
  model.k = 0.10;  // 10% of price at 100% participation
  model.beta = 0.6;
  const double mid = 4.00;
  const double half_spread = 0.05;
  const double adv = 0.01; // 1% of ADV

  const double impact = mid * model.k * std::pow(adv, model.beta);
  EXPECT_NEAR(fill_price(+10.0, mid, half_spread, adv, model), mid + half_spread + impact, 1e-12);
  EXPECT_NEAR(fill_price(-10.0, mid, half_spread, adv, model), mid - half_spread - impact, 1e-12);

  // A buy always costs at least as much as the mid, a sell always at most.
  EXPECT_GT(fill_price(+1.0, mid, half_spread, adv, model), mid);
  EXPECT_LT(fill_price(-1.0, mid, half_spread, adv, model), mid);
}

TEST(DispersionCostModelTest, ImpactIsConcaveInParticipation) {
  // The square-root law's defining property: doubling size less than doubles the
  // impact (beta < 1). This is what distinguishes it from a linear cost.
  DispersionCostModel model;
  model.k = 0.05;
  model.beta = 0.6;
  const double mid = 10.0;
  const auto impact = [&](double adv) { return fill_price(+1.0, mid, 0.0, adv, model) - mid; };

  const double small = impact(0.01);
  const double large = impact(0.02);
  EXPECT_GT(large, small);
  EXPECT_LT(large, 2.0 * small) << "beta < 1 must make impact concave in size";
  EXPECT_NEAR(large / small, std::pow(2.0, 0.6), 1e-12);
}

TEST(DispersionCostModelTest, ImpactFoldsIntoThePriceBpsSpreadLane) {
  DispersionCostModel model;
  model.k = 0.02;
  model.beta = 0.6;
  model.adv_fraction = 0.04;
  ASSERT_TRUE(model.active());

  const double impact_fraction = model.k * std::pow(model.adv_fraction, model.beta);

  // With no configured spread, the impact alone selects the price-bps lane.
  const FrictionModel from_none = dispersion_effective_frictions(FrictionModel{}, model);
  EXPECT_EQ(from_none.spread_kind, FrictionModel::SpreadKind::PriceBps);
  EXPECT_NEAR(from_none.half_spread_bps, 1.0e4 * impact_fraction, 1e-9);

  // With a spread already configured, impact ADDS to it.
  FrictionModel base;
  base.spread_kind = FrictionModel::SpreadKind::PriceBps;
  base.half_spread_bps = 25.0;
  const FrictionModel combined = dispersion_effective_frictions(base, model);
  EXPECT_NEAR(combined.half_spread_bps, 25.0 + 1.0e4 * impact_fraction, 1e-9);
}

TEST(DispersionRunConfigStrict, CostKeysReachTheEngineAsAnAddedHalfSpread) {
  const std::string body = std::string(kBaselineSpec) +
                           "cost_impact_k\t0.02\n"
                           "cost_impact_beta\t0.6\n"
                           "cost_adv_fraction\t0.04\n";
  const fs::path path = write_spec("atx-disp-cfg-costs", body);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*config);
  EXPECT_EQ(backtest.run.frictions.spread_kind, FrictionModel::SpreadKind::PriceBps);
  EXPECT_NEAR(backtest.run.frictions.half_spread_bps,
              1.0e4 * 0.02 * std::pow(0.04, 0.6), 1e-9);
}


// ── Small item: the optional projected-VaR stage is now gated by verify ──────
//
// `run-projected-var` was half-wired: it wrote three artifacts and `verify`
// checked none of them, so a truncated or stale projected-VaR run passed
// silently. The gate is deliberately CONDITIONAL — the stage is optional — but
// once the summary exists the whole envelope is checked.

namespace {

[[nodiscard]] fs::path pv_dir(const char *leaf) {
  const fs::path dir = fs::temp_directory_path() / leaf;
  std::error_code error;
  fs::remove_all(dir, error);
  fs::create_directories(dir, error);
  return dir;
}

void write_file(const fs::path &path, const std::string &body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

constexpr const char *kPvHeader =
    "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
    "n_positions\tprojections_per_second\tprepared_fingerprint\n";

constexpr const char *kPvRow95 = "0.95\t1\t2\t3\t82\t8\t100\t7\n";
constexpr const char *kPvRow99 = "0.99\t1\t2\t3\t82\t8\t100\t7\n";

} // namespace

TEST(DispersionProjectedVarGate, AbsentStageIsNotAnError) {
  // The stage is optional: a run that never invoked it must still verify.
  EXPECT_TRUE(verify_projected_var_artifacts(pv_dir("atx-disp-pv-absent"), 82));
}

TEST(DispersionProjectedVarGate, SummaryWithoutCompanionsIsRejected) {
  const fs::path dir = pv_dir("atx-disp-pv-orphan");
  write_file(dir / "projected_var.tsv", std::string(kPvHeader) + kPvRow95);
  EXPECT_FALSE(verify_projected_var_artifacts(dir, 82)) << "an orphaned summary must not verify";
}

TEST(DispersionProjectedVarGate, TruncatedScenarioCoverageIsRejected) {
  const fs::path dir = pv_dir("atx-disp-pv-truncated");
  write_file(dir / "projected_risk_scenarios.tsv", "x\n");
  write_file(dir / "projected_risk_legs.tsv", "x\n");
  // 40 scenarios recorded for an 82-session run: exactly the stale/truncated
  // case that used to pass verify silently.
  write_file(dir / "projected_var.tsv",
             std::string(kPvHeader) + "0.95\t1\t2\t3\t40\t8\t100\t7\n");
  const Status status = verify_projected_var_artifacts(dir, 82);
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().to_string().find("82"), std::string::npos)
      << status.error().to_string();
}

TEST(DispersionProjectedVarGate, WellFormedCompleteRunPasses) {
  const fs::path dir = pv_dir("atx-disp-pv-ok");
  write_file(dir / "projected_risk_scenarios.tsv", "x\n");
  write_file(dir / "projected_risk_legs.tsv", "x\n");
  write_file(dir / "projected_var.tsv", std::string(kPvHeader) + kPvRow95 + kPvRow99);
  const Status status = verify_projected_var_artifacts(dir, 82);
  EXPECT_TRUE(status) << status.error().to_string();
}

TEST(DispersionProjectedVarGate, WrongHeaderIsRejected) {
  const fs::path dir = pv_dir("atx-disp-pv-header");
  write_file(dir / "projected_risk_scenarios.tsv", "x\n");
  write_file(dir / "projected_risk_legs.tsv", "x\n");
  write_file(dir / "projected_var.tsv", "confidence\tvar\n0.95\t1\n");
  EXPECT_FALSE(verify_projected_var_artifacts(dir, 82));
}

// ── Small items: knobs that existed in the code but not in the spec ──────────

// `entry_every_n` reached the lifecycle spec but was NOT readable from the run
// spec, so every run silently used the 21-step default no matter what an
// operator intended.
TEST(DispersionRunConfigStrict, EntryCadenceIsSettableFromTheSpec) {
  const std::string body = std::string(kBaselineSpec) + "entry_every_n\t5\n";
  const Result<DispersionRunConfig> config =
      read_dispersion_run_config(write_spec("atx-disp-cfg-entry", body));
  ASSERT_TRUE(config) << config.error().to_string();
  EXPECT_EQ(config->entry_every_n, 5u);
  EXPECT_EQ(dispersion_backtest_config_from(*config).entry_every_n, 5u);

  // Zero would silently disable entries; reject it rather than accept nonsense.
  const std::string zero = std::string(kBaselineSpec) + "entry_every_n\t0\n";
  EXPECT_FALSE(read_dispersion_run_config(write_spec("atx-disp-cfg-entry0", zero)));
}

// `record_diagnostics` gates the implied-correlation signal. The CLI never
// enabled it, so the diagnostic was dead code on the file-driven path.
TEST(DispersionRunConfigStrict, DiagnosticsAreEnableableFromTheSpec) {
  const Result<DispersionRunConfig> off =
      read_dispersion_run_config(write_spec("atx-disp-cfg-diag-off", kBaselineSpec));
  ASSERT_TRUE(off) << off.error().to_string();
  EXPECT_FALSE(off->record_diagnostics);
  EXPECT_FALSE(dispersion_backtest_config_from(*off).record_diagnostics);

  const std::string body = std::string(kBaselineSpec) + "record_diagnostics\t1\n";
  const Result<DispersionRunConfig> on =
      read_dispersion_run_config(write_spec("atx-disp-cfg-diag-on", body));
  ASSERT_TRUE(on) << on.error().to_string();
  EXPECT_TRUE(on->record_diagnostics);
  EXPECT_TRUE(dispersion_backtest_config_from(*on).record_diagnostics);
}

// The contract multiplier was a hardcoded 100.0 at every construction site.
TEST(DispersionRunConfigStrict, MultiplierIsSettableAndValidated) {
  const std::string body = std::string(kBaselineSpec) + "multiplier\t50\n";
  const Result<DispersionRunConfig> config =
      read_dispersion_run_config(write_spec("atx-disp-cfg-mult", body));
  ASSERT_TRUE(config) << config.error().to_string();
  EXPECT_EQ(config->multiplier, 50.0);
  EXPECT_EQ(dispersion_backtest_config_from(*config).multiplier, 50.0);

  const std::string bad = std::string(kBaselineSpec) + "multiplier\t0\n";
  EXPECT_FALSE(read_dispersion_run_config(write_spec("atx-disp-cfg-mult0", bad)));
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// WS-X-B â€” X4 policy knobs + X5 reporting, at the strict-config seam.
//
// The governing constraint for this workstream is that EVERY new knob defaults
// to today's behaviour, so a spec written before the change produces identical
// output after it. The first test below pins that at the config level; the
// end-to-end byte-identity is measured separately by replaying the reference run.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// â”€â”€ The pinned baseline spec still parses, and every X4/X5 field defaults â”€â”€â”€
TEST(DispersionRunConfigXB, BaselineSpec_LeavesEveryNewKnobAtTheShippedDefault) {
  const fs::path path = write_spec("atx_xb_defaults", kBaselineSpec);
  auto config = read_dispersion_run_config(path);
  ASSERT_TRUE(config.has_value()) << config.error().to_string();

  // X4: the shipped construction.
  EXPECT_EQ(config->weighting, WeightingScheme::VegaNeutral);
  EXPECT_EQ(config->strike.rule, StrikeRule::AtmForwardStraddle);
  EXPECT_EQ(config->strike.log_moneyness, 0.0);

  // X5: no benchmark claimed, 252-period annualization.
  EXPECT_TRUE(config->benchmark_series.empty());
  EXPECT_EQ(config->periods_per_year, 252.0);

  // And the regime the baseline describes is FRICTIONLESS â€” the pin's regime.
  EXPECT_EQ(dispersion_friction_regime(*config), DispersionFrictionRegime::Frictionless);

  // Those defaults must survive the trip into the backtest config, or the knob
  // would be typed at the seam and dropped on the way to the engine.
  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*config);
  EXPECT_EQ(backtest.weighting, WeightingScheme::VegaNeutral);
  EXPECT_EQ(backtest.strike.rule, StrikeRule::AtmForwardStraddle);
  EXPECT_EQ(backtest.strike.log_moneyness, 0.0);
}

// â”€â”€ X4: every nameable scheme parses AND reaches the engine config â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionRunConfigXB, WeightingSchemes_ParseAndReachTheEngine) {
  const struct {
    const char *spelling;
    WeightingScheme expected;
  } cases[] = {
      {"vega_neutral", WeightingScheme::VegaNeutral},
      {"equal_vega", WeightingScheme::EqualVega},
      {"gamma_neutral", WeightingScheme::GammaNeutral},
      {"theta_neutral", WeightingScheme::ThetaNeutral},
  };
  for (const auto &c : cases) {
    const std::string body = std::string(kBaselineSpec) + "weighting\t" + c.spelling + "\n";
    const fs::path path = write_spec("atx_xb_weighting", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << c.spelling << ": " << config.error().to_string();
    EXPECT_EQ(config->weighting, c.expected) << c.spelling;
    // The knob must survive into the engine config â€” a scheme parsed and then
    // dropped at the seam is exactly the silent-no-op this workstream forbids.
    EXPECT_EQ(dispersion_backtest_config_from(*config).weighting, c.expected) << c.spelling;
  }
}

TEST(DispersionRunConfigXB, StrikeRules_ParseAndReachTheEngine) {
  {
    const std::string body =
        std::string(kBaselineSpec) + "strike\tfixed_moneyness\nstrike_log_moneyness\t0.05\n";
    const fs::path path = write_spec("atx_xb_strike_fm", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(config->strike.rule, StrikeRule::FixedMoneyness);
    EXPECT_NEAR(config->strike.log_moneyness, 0.05, 1e-15);
    const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*config);
    EXPECT_EQ(backtest.strike.rule, StrikeRule::FixedMoneyness);
    EXPECT_NEAR(backtest.strike.log_moneyness, 0.05, 1e-15);
  }
  {
    const std::string body =
        std::string(kBaselineSpec) + "strike\tdelta_strangle\nstrike_abs_delta\t0.25\n";
    const fs::path path = write_spec("atx_xb_strike_ds", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(config->strike.rule, StrikeRule::DeltaStrangle);
    EXPECT_NEAR(config->strike.target_abs_delta, 0.25, 1e-15);
    EXPECT_EQ(dispersion_backtest_config_from(*config).strike.rule, StrikeRule::DeltaStrangle);
  }
}

// â”€â”€ X4: out-of-contract combinations are refused, naming the key â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionRunConfigXB, StrikeParameters_AreRefusedUnderARuleThatIgnoresThem) {
  // A moneyness offset under the default rule would silently do nothing â€” which
  // is the failure mode the strict seam exists to prevent.
  {
    const std::string body = std::string(kBaselineSpec) + "strike_log_moneyness\t0.05\n";
    const fs::path path = write_spec("atx_xb_moneyness_orphan", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_FALSE(config.has_value()) << "an inert strike_log_moneyness was accepted";
    EXPECT_NE(config.error().message().find("strike_log_moneyness"), std::string::npos)
        << "the error must name the offending key: " << config.error().message();
  }
  // Same for a delta under a rule that never reads one. The check keys off the
  // key being NAMED, not off its value, so setting it to its own default is
  // still refused -- otherwise `strike_abs_delta=0.25` under the default rule
  // would be silently inert, exactly the bug class this seam exists to prevent.
  for (const char *value : {"0.4", "0.25"}) {
    const std::string body = std::string(kBaselineSpec) + "strike_abs_delta\t" + value + "\n";
    const fs::path path = write_spec("atx_xb_delta_orphan", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_FALSE(config.has_value()) << "an inert strike_abs_delta=" << value << " was accepted";
    EXPECT_NE(config.error().message().find("strike_abs_delta"), std::string::npos)
        << "the error must name the offending key: " << config.error().message();
  }
  // Explicitly setting a moneyness of 0 under the default rule is ALSO refused:
  // it is inert regardless of being numerically harmless.
  {
    const std::string body = std::string(kBaselineSpec) + "strike_log_moneyness\t0\n";
    const fs::path path = write_spec("atx_xb_moneyness_zero", body);
    auto config = read_dispersion_run_config(path);
    EXPECT_FALSE(config.has_value()) << "an inert strike_log_moneyness=0 was accepted";
  }
  // An out-of-range delta is refused rather than clamped.
  for (const char *bad : {"0", "1", "1.5", "-0.25"}) {
    const std::string body =
        std::string(kBaselineSpec) + "strike\tdelta_strangle\nstrike_abs_delta\t" + bad + "\n";
    const fs::path path = write_spec("atx_xb_delta_bad", body);
    auto config = read_dispersion_run_config(path);
    EXPECT_FALSE(config.has_value()) << "strike_abs_delta=" << bad << " was accepted";
  }
  // An unimplemented scheme fails loudly AND the message lists the legal ones.
  {
    const std::string body = std::string(kBaselineSpec) + "weighting\tcorrelation_neutral\n";
    const fs::path path = write_spec("atx_xb_weighting_bad", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_FALSE(config.has_value());
    const std::string message = config.error().message();
    EXPECT_NE(message.find("weighting"), std::string::npos) << message;
    EXPECT_NE(message.find("gamma_neutral"), std::string::npos)
        << "the error must enumerate the supported values: " << message;
  }
}

// â”€â”€ X5: the friction/impact regime is classified from what reaches the engine â”€
TEST(DispersionRunConfigXB, FrictionRegime_ClassifiesTheThreeRegimes) {
  // Frictionless â€” the pin.
  {
    const fs::path path = write_spec("atx_xb_regime_none", kBaselineSpec);
    auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(dispersion_friction_regime(*config), DispersionFrictionRegime::Frictionless);
    EXPECT_EQ(to_string(dispersion_friction_regime(*config)), "frictionless");
    // The detail line must say so in words, not merely omit the parameters.
    EXPECT_NE(dispersion_regime_detail(config->frictions, config->costs).find("mid fills"),
              std::string::npos);
  }
  // Frictioned â€” a spread/commission, but no impact term.
  {
    const std::string body =
        std::string(kBaselineSpec) + "friction_preset\tretail_listed_options\n";
    const fs::path path = write_spec("atx_xb_regime_fric", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(dispersion_friction_regime(*config), DispersionFrictionRegime::Frictioned);
    EXPECT_EQ(to_string(dispersion_friction_regime(*config)), "frictioned");
    const std::string detail = dispersion_regime_detail(config->frictions, config->costs);
    EXPECT_NE(detail.find("half-spread"), std::string::npos) << detail;
    EXPECT_NE(detail.find("/contract"), std::string::npos) << detail;
  }
  // Frictioned + impact â€” the regime in which the pinned result flips sign.
  {
    const std::string body = std::string(kBaselineSpec) +
                             "friction_preset\tretail_listed_options\n"
                             "cost_impact_k\t0.4\ncost_adv_fraction\t0.02\n";
    const fs::path path = write_spec("atx_xb_regime_impact", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(dispersion_friction_regime(*config),
              DispersionFrictionRegime::FrictionedWithImpact);
    EXPECT_EQ(to_string(dispersion_friction_regime(*config)), "frictioned+impact");
    EXPECT_NE(dispersion_regime_detail(config->frictions, config->costs).find("sqrt-impact"),
              std::string::npos);
  }
  // An impact model with ZERO participation is inert, so it must NOT be reported
  // as an impact regime â€” overstating the realism of a run is the same class of
  // error as understating it.
  {
    const std::string body = std::string(kBaselineSpec) + "cost_impact_k\t0.4\n";
    const fs::path path = write_spec("atx_xb_regime_inert_impact", body);
    auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(dispersion_friction_regime(*config), DispersionFrictionRegime::Frictionless);
  }
}

// â”€â”€ X5: the report metadata leads with the regime and never fakes a benchmark â”€
TEST(DispersionRunConfigXB, ReportMetadata_LeadsWithTheRegime) {
  const std::string body = std::string(kBaselineSpec) +
                           "friction_preset\tretail_listed_options\n"
                           "cost_impact_k\t0.4\ncost_adv_fraction\t0.02\n";
  const fs::path path = write_spec("atx_xb_meta", body);
  auto config = read_dispersion_run_config(path);
  ASSERT_TRUE(config.has_value()) << config.error().to_string();

  TearSheet sheet;
  sheet.total_return = -64.60;
  sheet.total_cost = 312.01;

  const auto meta = dispersion_report_metadata(*config, sheet, 82u);
  ASSERT_FALSE(meta.empty());
  // REGIME FIRST: a truncated read must still carry which assumptions produced
  // the numbers below it.
  EXPECT_EQ(meta[0].first, "friction_regime");
  EXPECT_EQ(meta[0].second, "frictioned+impact");
  EXPECT_EQ(meta[1].first, "friction_detail");

  const auto find = [&](const char *key) -> const std::string * {
    for (const auto &kv : meta) {
      if (kv.first == key) {
        return &kv.second;
      }
    }
    return nullptr;
  };
  // The cost drag and the pre-cost figure are both present, so a reader can see
  // the cost share of the headline without a second artifact.
  ASSERT_NE(find("total_return"), nullptr);
  ASSERT_NE(find("total_cost"), nullptr);
  ASSERT_NE(find("gross_return"), nullptr);
  EXPECT_NEAR(std::stod(*find("gross_return")), -64.60 + 312.01, 1e-6);
  // The X4 construction is stated too.
  ASSERT_NE(find("weighting"), nullptr);
  EXPECT_EQ(*find("weighting"), "vega_neutral");
  ASSERT_NE(find("strike_rule"), nullptr);
  EXPECT_EQ(*find("strike_rule"), "atm_forward_straddle");

  // NO BENCHMARK SUPPLIED => no benchmark keys at all. An absent benchmark must
  // never be reported as a zero alpha or a zero beta.
  EXPECT_EQ(find("benchmark_beta"), nullptr);
  EXPECT_EQ(find("benchmark_alpha"), nullptr);
  EXPECT_EQ(find("benchmark_information_ratio"), nullptr);

  // With a benchmark, the block appears.
  sheet.benchmark.has_benchmark = true;
  sheet.benchmark.n_obs = 81;
  sheet.benchmark.beta = 1.8;
  sheet.benchmark.information_ratio = 0.53;
  const auto with_bench = dispersion_report_metadata(*config, sheet, 82u);
  bool saw_beta = false;
  for (const auto &kv : with_bench) {
    if (kv.first == "benchmark_beta") {
      saw_beta = true;
      EXPECT_EQ(kv.second, "1.8");
    }
  }
  EXPECT_TRUE(saw_beta);
}

// â”€â”€ X5: the benchmark series reader â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionRunConfigXB, BenchmarkSeriesReader_ParsesAndRefusesMalformedRows) {
  const fs::path dir = fs::temp_directory_path() / "atx_xb_bench";
  std::error_code error;
  fs::remove_all(dir, error);
  fs::create_directories(dir, error);

  // A header row is tolerated; rows are read in file order.
  {
    const fs::path path = dir / "good.tsv";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "date\tpnl\n2026-01-02\t4\n2026-01-05\t-2\n2026-01-06\t6\n";
    out.close();
    auto series = read_dispersion_benchmark_series(path);
    ASSERT_TRUE(series.has_value()) << series.error().to_string();
    ASSERT_EQ(series->size(), 3u);
    EXPECT_EQ((*series)[0], 4.0);
    EXPECT_EQ((*series)[1], -2.0);
    EXPECT_EQ((*series)[2], 6.0);
  }
  // A malformed row mid-file is an ERROR: a benchmark that silently half-loads
  // would corrupt every statistic derived from it.
  {
    const fs::path path = dir / "bad.tsv";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "date\tpnl\n2026-01-02\t4\n2026-01-05\tnot-a-number\n";
    out.close();
    auto series = read_dispersion_benchmark_series(path);
    EXPECT_FALSE(series.has_value()) << "a malformed benchmark row was silently skipped";
  }
  // An absent file is NotFound, not an empty series.
  {
    auto series = read_dispersion_benchmark_series(dir / "missing.tsv");
    ASSERT_FALSE(series.has_value());
    EXPECT_EQ(series.error().code(), ErrorCode::NotFound);
  }
}
