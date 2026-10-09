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
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

#include "atx/core/hash.hpp"
#include "atx/vol/research/dispersion_backtest.hpp"
#include "atx/vol/research/dispersion_run.hpp"
#include "atx/vol/research/dispersion_workflow.hpp" // read_run_spec / write_resolved_spec (F4)
#include "storage/track_key.hpp" // kBacktestEconomicsRev (E1 fix round)
#include "atx/vol/api/core/types.hpp"

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
// the spec behind the pinned 82-session golden (final_nav = 24740.624124981368).
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
  EXPECT_FALSE(dispersion_backtest_config_from(*without).run.financing.flat_r.has_value());

  const std::string body = std::string(kBaselineSpec) + "rate_applies_to_financing\t1\n";
  const fs::path on = write_spec("atx-disp-cfg-rate-on", body);
  const Result<DispersionRunConfig> with = read_dispersion_run_config(on);
  ASSERT_TRUE(with) << with.error().to_string();
  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*with);
  // Task E1: `flat_rate` must land in `FinancingConfig::flat_r` -- the field
  // `finance_premium`'s cash-carry accrual actually reads (backtest.cpp) --
  // not `borrow_rate` (a different, short-shares-borrow-proxy knob this used
  // to silently clobber; see dispersion_run.cpp's own comment on the fix).
  // `borrow_rate` stays at ITS OWN default (0.0, unset by this spec), proving
  // the two knobs no longer collide.
  EXPECT_DOUBLE_EQ(backtest.run.financing.borrow_rate, 0.0);
  ASSERT_TRUE(backtest.run.financing.flat_r.has_value());
  EXPECT_DOUBLE_EQ(*backtest.run.financing.flat_r, 0.043);
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

// REVIEW C-4. The impact occupies its OWN additive lane and never rewrites the
// configured spread kind. (This test previously asserted the opposite — that any
// active impact folded the model to `PriceBps` — which is exactly the behaviour
// that deleted a configured `vol_tick`.)
TEST(DispersionCostModelTest, ImpactRidesItsOwnAdditiveLaneAndKeepsTheSpreadKind) {
  DispersionCostModel model;
  model.k = 0.02;
  model.beta = 0.6;
  model.adv_fraction = 0.04;
  ASSERT_TRUE(model.active());

  const double impact_fraction = model.k * std::pow(model.adv_fraction, model.beta);

  // With no configured spread, the impact alone is the whole execution cost —
  // and the kind stays `None`, because the impact lane no longer needs a lane
  // to hide in.
  const FrictionModel from_none = dispersion_effective_frictions(FrictionModel{}, model);
  EXPECT_EQ(from_none.spread_kind, FrictionModel::SpreadKind::None);
  EXPECT_NEAR(from_none.impact_fraction, impact_fraction, 1e-15);
  EXPECT_DOUBLE_EQ(from_none.half_spread_bps, 0.0);

  // With a price-bps spread configured, the spread is preserved UNCHANGED and
  // the impact is carried alongside it.
  FrictionModel base;
  base.spread_kind = FrictionModel::SpreadKind::PriceBps;
  base.half_spread_bps = 25.0;
  const FrictionModel combined = dispersion_effective_frictions(base, model);
  EXPECT_EQ(combined.spread_kind, FrictionModel::SpreadKind::PriceBps);
  EXPECT_DOUBLE_EQ(combined.half_spread_bps, 25.0);
  EXPECT_NEAR(combined.impact_fraction, impact_fraction, 1e-15);
}

// REVIEW C-4 — the combination the pre-fix code silently discarded. The engine
// charges `vega * vol_tick + mark * impact_fraction`; the ENGINE-LEVEL charged
// cost is pinned by `Backtest.C4_ImpactIsChargedOnTopOfAVolTickSpreadNotInsteadOfIt`
// in backtest_test.cpp, which is the independent additive oracle. This one pins
// the config seam that used to drop `vol_tick` on the floor.
TEST(DispersionCostModelTest, C4_AVolTickSpreadSurvivesAnActiveImpactModel) {
  DispersionCostModel model;
  model.k = 0.02;
  model.beta = 0.6;
  model.adv_fraction = 0.04;
  ASSERT_TRUE(model.active());

  FrictionModel base;
  base.spread_kind = FrictionModel::SpreadKind::VolTicks;
  base.vol_tick = 0.15;

  const FrictionModel combined = dispersion_effective_frictions(base, model);
  EXPECT_EQ(combined.spread_kind, FrictionModel::SpreadKind::VolTicks)
      << "an active impact model must not rewrite the configured spread kind";
  EXPECT_DOUBLE_EQ(combined.vol_tick, 0.15) << "the configured vol-tick spread was discarded";
  EXPECT_NEAR(combined.impact_fraction, model.k * std::pow(model.adv_fraction, model.beta), 1e-15);
  // ... and the price-bps lane is untouched, so nothing is charged twice.
  EXPECT_DOUBLE_EQ(combined.half_spread_bps, 0.0);
}

TEST(DispersionRunConfigStrict, CostKeysReachTheEngineAsASeparateAdditiveImpactLane) {
  const std::string body = std::string(kBaselineSpec) +
                           "friction_spread_kind\tvol_ticks\n"
                           "friction_vol_tick\t0.15\n"
                           "cost_impact_k\t0.02\n"
                           "cost_impact_beta\t0.6\n"
                           "cost_adv_fraction\t0.04\n";
  const fs::path path = write_spec("atx-disp-cfg-costs", body);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  // REVIEW C-4: a spec naming BOTH a vol-tick spread and an impact model must
  // deliver both to the engine. Pre-fix this arrived as `PriceBps` with the
  // vol-tick silently gone.
  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(*config);
  EXPECT_EQ(backtest.run.frictions.spread_kind, FrictionModel::SpreadKind::VolTicks);
  EXPECT_DOUBLE_EQ(backtest.run.frictions.vol_tick, 0.15);
  EXPECT_NEAR(backtest.run.frictions.impact_fraction, 0.02 * std::pow(0.04, 0.6), 1e-15);
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

// REVIEW C-1: the last three columns are the as-of provenance the route now
// publishes (which session the immutable book was resolved and sized on, and
// that book's identity).
constexpr const char *kPvHeader =
    "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
    "n_positions\tprojections_per_second\tprepared_fingerprint\tas_of_date\tas_of_ts_ns\t"
    "book_fingerprint\n";

constexpr const char *kPvRow95 = "0.95\t1\t2\t3\t82\t8\t100\t7\t2026-07-11\t200\t9\n";

void write_valid_projected_var(const fs::path &dir) {
  constexpr std::int64_t ts0 = 1'790'884'800'000'000'000LL;
  constexpr std::int64_t day = 86'400'000'000'000LL;
  constexpr std::uint64_t prepared = 7u;
  constexpr std::uint64_t leg_fp0 = 11u;
  constexpr std::uint64_t leg_fp1 = 13u;
  const std::uint64_t frame_fp0 = atx::core::hash_combine(prepared, leg_fp0);
  const std::uint64_t frame_fp1 = atx::core::hash_combine(prepared, leg_fp1);

  std::ostringstream scenarios;
  scenarios << std::setprecision(17)
            << "date\tts_ns\tvalue\tdelta\tgamma\tvega\ttheta\tn_ok\tn_failed\t"
               "definition_fingerprint\n"
            << "2026-10-01\t" << ts0 << "\t90\t1\t2\t3\t4\t1\t0\t" << frame_fp0 << '\n'
            << "2026-10-02\t" << (ts0 + day) << "\t100\t1\t2\t3\t4\t1\t0\t" << frame_fp1
            << '\n';
  write_file(dir / "projected_risk_scenarios.tsv", scenarios.str());

  std::ostringstream legs;
  legs << std::setprecision(17)
       << "date\tleg\tuid\tside\texpiry_ts_ns\tstrike\tquantity\tmultiplier\tmark\t"
          "delta\tgamma\tvega\ttheta\tdefinition_fingerprint\tstatus\n"
       << "2026-10-01\t0\t1\tCall\t" << (ts0 + 30 * day)
       << "\t100\t2\t100\t0.45\t0.5\t0.01\t0.2\t-0.1\t" << leg_fp0 << "\tOk\n"
       << "2026-10-02\t0\t1\tCall\t" << (ts0 + 31 * day)
       << "\t101\t2\t100\t0.5\t0.5\t0.01\t0.2\t-0.1\t" << leg_fp1 << "\tOk\n";
  write_file(dir / "projected_risk_legs.tsv", legs.str());

  const std::string row =
      "\t100\t10\t10\t2\t1\t100\t7\t2026-10-02\t" + std::to_string(ts0 + day) +
      "\t9\n";
  write_file(dir / "projected_var.tsv",
             std::string(kPvHeader) + "0.95" + row + "0.99" + row);
}

// A summary in the pre-C-1 shape: economically well-formed, but silent about
// which session's book it describes. `verify` must reject it.
constexpr const char *kPvHeaderWithoutAsOf =
    "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
    "n_positions\tprojections_per_second\tprepared_fingerprint\n";
constexpr const char *kPvRow95WithoutAsOf = "0.95\t1\t2\t3\t82\t8\t100\t7\n";

} // namespace

TEST(DispersionProjectedVarGate, AbsentStageIsNotAnError) {
  // The stage is optional: a run that never invoked it must still verify.
  const Status status =
      verify_projected_var_artifacts(pv_dir("atx-disp-pv-absent"), 82);
  EXPECT_TRUE(status) << status.error().to_string();
}

TEST(DispersionProjectedVarGate, SummaryWithoutCompanionsIsRejected) {
  const fs::path dir = pv_dir("atx-disp-pv-orphan");
  write_file(dir / "projected_var.tsv", std::string(kPvHeader) + kPvRow95);
  EXPECT_FALSE(verify_projected_var_artifacts(dir, 82)) << "an orphaned summary must not verify";
}

TEST(DispersionProjectedVarGate, PendingGenerationIsRejectedRatherThanTreatedAsAbsent) {
  const fs::path dir = pv_dir("atx-disp-pv-pending");
  write_file(dir / "projected_var.tsv.pending", std::string(kPvHeader) + kPvRow95);
  EXPECT_FALSE(verify_projected_var_artifacts(dir, 82));
}

TEST(DispersionProjectedVarGate, TruncatedScenarioCoverageIsRejected) {
  const fs::path dir = pv_dir("atx-disp-pv-truncated");
  write_file(dir / "projected_risk_scenarios.tsv", "x\n");
  write_file(dir / "projected_risk_legs.tsv", "x\n");
  // 40 scenarios recorded for an 82-session run: exactly the stale/truncated
  // case that used to pass verify silently.
  write_file(dir / "projected_var.tsv",
             std::string(kPvHeader) +
                 "0.95\t1\t2\t3\t40\t8\t100\t7\t2026-07-11\t200\t9\n");
  const Status status = verify_projected_var_artifacts(dir, 82);
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().to_string().find("82"), std::string::npos)
      << status.error().to_string();
}

TEST(DispersionProjectedVarGate, WellFormedCompleteRunPasses) {
  const fs::path dir = pv_dir("atx-disp-pv-ok");
  write_valid_projected_var(dir);
  const Status status = verify_projected_var_artifacts(dir, 2);
  EXPECT_TRUE(status) << status.error().to_string();
}

TEST(DispersionProjectedVarGate, GarbageCompanionAndTrailingNumericInputAreRejected) {
  const fs::path garbage = pv_dir("atx-disp-pv-garbage");
  write_valid_projected_var(garbage);
  write_file(garbage / "projected_risk_legs.tsv", "x\n");
  EXPECT_FALSE(verify_projected_var_artifacts(garbage, 2));

  const fs::path trailing = pv_dir("atx-disp-pv-trailing");
  write_valid_projected_var(trailing);
  std::string summary = std::string(kPvHeader) +
                        "0.95\t100\t10junk\t10\t2\t1\t100\t7\t2026-10-02\t"
                        "1790971200000000000\t9\n";
  write_file(trailing / "projected_var.tsv", summary);
  EXPECT_FALSE(verify_projected_var_artifacts(trailing, 2));
}

TEST(DispersionProjectedVarGate, CountsAsOfBookAndFiniteRiskMustStayConsistent) {
  constexpr const char *row95 =
      "0.95\t100\t10\t10\t2\t1\t100\t7\t2026-10-02\t1790971200000000000\t9\n";
  constexpr const char *row99 =
      "0.99\t100\t10\t10\t2\t1\t100\t7\t2026-10-02\t1790971200000000000\t9\n";

  const fs::path count = pv_dir("atx-disp-pv-leg-count");
  write_valid_projected_var(count);
  write_file(count / "projected_risk_legs.tsv",
             "date\tleg\tuid\tside\texpiry_ts_ns\tstrike\tquantity\tmultiplier\tmark\t"
             "delta\tgamma\tvega\ttheta\tdefinition_fingerprint\tstatus\n");
  EXPECT_FALSE(verify_projected_var_artifacts(count, 2));

  const fs::path as_of = pv_dir("atx-disp-pv-asof");
  write_valid_projected_var(as_of);
  write_file(as_of / "projected_var.tsv",
             std::string(kPvHeader) +
                 "0.95\t100\t10\t10\t2\t1\t100\t7\t2026-10-01\t"
                 "1790884800000000000\t9\n"
                 "0.99\t100\t10\t10\t2\t1\t100\t7\t2026-10-01\t"
                 "1790884800000000000\t9\n");
  EXPECT_FALSE(verify_projected_var_artifacts(as_of, 2));

  const fs::path identity = pv_dir("atx-disp-pv-book-fp");
  write_valid_projected_var(identity);
  write_file(identity / "projected_var.tsv",
             std::string(kPvHeader) + row95 +
                 "0.99\t100\t10\t10\t2\t1\t100\t7\t2026-10-02\t"
                 "1790971200000000000\t10\n");
  EXPECT_FALSE(verify_projected_var_artifacts(identity, 2));

  const fs::path finite = pv_dir("atx-disp-pv-finite");
  write_valid_projected_var(finite);
  write_file(finite / "projected_var.tsv",
             std::string(kPvHeader) +
                 "0.95\t100\tnan\t10\t2\t1\t100\t7\t2026-10-02\t"
                 "1790971200000000000\t9\n" +
                 row99);
  EXPECT_FALSE(verify_projected_var_artifacts(finite, 2));
}

TEST(DispersionProjectedVarGate, WrongHeaderIsRejected) {
  const fs::path dir = pv_dir("atx-disp-pv-header");
  write_file(dir / "projected_risk_scenarios.tsv", "x\n");
  write_file(dir / "projected_risk_legs.tsv", "x\n");
  write_file(dir / "projected_var.tsv", "confidence\tvar\n0.95\t1\n");
  EXPECT_FALSE(verify_projected_var_artifacts(dir, 82));
}

// REVIEW C-1. The economics parse, the scenario count matches, and every field
// the pre-C-1 contract named is present and well formed — the ONLY thing wrong
// is that the summary does not say which session's book it measures. That has to
// be a verify failure, otherwise a stale artifact left by an earlier binary
// passes `verify` and gets read as current.
TEST(DispersionProjectedVarGate, C1_SummaryWithoutTheAsOfProvenanceIsRejected) {
  const fs::path dir = pv_dir("atx-disp-pv-no-asof");
  write_file(dir / "projected_risk_scenarios.tsv", "x\n");
  write_file(dir / "projected_risk_legs.tsv", "x\n");
  write_file(dir / "projected_var.tsv",
             std::string(kPvHeaderWithoutAsOf) + kPvRow95WithoutAsOf);
  EXPECT_FALSE(verify_projected_var_artifacts(dir, 82))
      << "a projected-VaR summary silent about its as-of session verified clean";
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
  // E1 fix round: economics_rev sits right beside the regime it completes.
  ASSERT_GT(meta.size(), 2u);
  EXPECT_EQ(meta[2].first, "economics_rev");
  EXPECT_EQ(meta[2].second, std::to_string(kBacktestEconomicsRev));

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

// -- M4 [WS-M]: the EMITTED artifact FILES lead with the regime -----------------
// `ReportMetadata_LeadsWithTheRegime` above pins the in-memory meta vector; this
// pins the ON-DISK contract that the Python renderer actually consumes. The
// renderer `tools/spy_dispersion_tearsheet_report.py` HARD-REFUSES a track with no
// `friction_regime` key (see that file, and the "REGIME IS NOT OPTIONAL METADATA"
// header contract in include/atx/vol/research/dispersion_run.hpp). So a writer that stopped
// serializing the key -- e.g. someone deleting the emplace_back in
// `dispersion_report_metadata`, or a new run path that never routed through
// `write_dispersion_tearsheet` -- would silently break every downstream tearsheet
// while the in-memory unit test above stayed green. This drives the real file
// writer end to end and pins both emitted artifacts. (Python-side enforcement of
// the same contract is task Y4, not this test.)
TEST(DispersionRunConfigXB, SurfaceArtifacts_EmitFrictionRegimeFirst) {
  // A frictioned+impact spec, so the emitted regime is a SPECIFIC non-default
  // string ("frictioned+impact") rather than the frictionless default -- a deleted
  // emission line therefore cannot be masked by a coincidental default/empty match.
  const std::string body = std::string(kBaselineSpec) +
                           "friction_preset\tretail_listed_options\n"
                           "cost_impact_k\t0.4\ncost_adv_fraction\t0.02\n";
  const fs::path spec = write_spec("atx_m4_regime_artifacts", body);
  const fs::path run_dir = spec.parent_path();
  auto config = read_dispersion_run_config(spec);
  ASSERT_TRUE(config.has_value()) << config.error().to_string();

  // A minimal outcome suffices: the regime is derived from the config, and the
  // series body is irrelevant to the metadata-header contract under test. A
  // default-constructed track writes a header-only series, which is fine.
  DispersionBacktestOutcome outcome;
  outcome.sheet.total_return = -64.60;
  outcome.sheet.total_cost = 312.01;

  ASSERT_TRUE(write_dispersion_tearsheet(run_dir, *config, outcome))
      << "write_dispersion_tearsheet failed";

  const auto slurp = [](const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::string content;
    std::string line;
    while (std::getline(in, line)) {
      content += line;
      content += '\n';
    }
    return content;
  };

  // surface_tearsheet.tsv: `metric<TAB>value` header, then meta rows REGIME FIRST.
  {
    const std::string tsv = slurp(run_dir / "surface_tearsheet.tsv");
    ASSERT_FALSE(tsv.empty()) << "surface_tearsheet.tsv was not written";
    // The FIRST metric row after the header must be the regime with a non-empty value.
    EXPECT_EQ(tsv.rfind("metric\tvalue\nfriction_regime\t", 0), 0u)
        << "surface_tearsheet.tsv does not lead with friction_regime:\n"
        << tsv.substr(0, 128);
    EXPECT_NE(tsv.find("friction_regime\tfrictioned+impact\n"), std::string::npos)
        << tsv.substr(0, 128);
    // E1 fix round: economics_rev must be in this artifact too.
    EXPECT_NE(tsv.find("economics_rev\t" + std::to_string(kBacktestEconomicsRev) + '\n'),
              std::string::npos)
        << tsv.substr(0, 200);
  }

  // surface_pnl_track.tsv: `# key=value` meta header, REGIME FIRST, non-empty value.
  {
    const std::string tsv = slurp(run_dir / "surface_pnl_track.tsv");
    ASSERT_FALSE(tsv.empty()) << "surface_pnl_track.tsv was not written";
    EXPECT_EQ(tsv.rfind("# friction_regime=", 0), 0u)
        << "surface_pnl_track.tsv does not lead with '# friction_regime=':\n"
        << tsv.substr(0, 128);
    EXPECT_NE(tsv.find("# friction_regime=frictioned+impact\n"), std::string::npos)
        << tsv.substr(0, 128);
    // E1 fix round: economics_rev must be in this artifact too.
    EXPECT_NE(tsv.find("# economics_rev=" + std::to_string(kBacktestEconomicsRev) + '\n'),
              std::string::npos)
        << tsv.substr(0, 200);
  }
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
    // REVIEW C-6: the DATE is retained. It used to be parsed and discarded, which
    // is what made every downstream alignment positional.
    EXPECT_EQ((*series)[0].date, "2026-01-02");
    EXPECT_EQ((*series)[0].pnl, 4.0);
    EXPECT_EQ((*series)[1].date, "2026-01-05");
    EXPECT_EQ((*series)[1].pnl, -2.0);
    EXPECT_EQ((*series)[2].date, "2026-01-06");
    EXPECT_EQ((*series)[2].pnl, 6.0);
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

// ── REVIEW C-6: the reader's own validation ─────────────────────────────────
//
// Every shape below used to load cleanly and then be aligned by POSITION, so it
// reached the published report as a confident number over the wrong observations.
TEST(DispersionRunConfigXB, C6_BenchmarkReaderRefusesUnusableDateColumns) {
  const fs::path dir = fs::temp_directory_path() / "atx_c6_bench_reader";
  std::error_code error;
  fs::remove_all(dir, error);
  fs::create_directories(dir, error);

  const auto read_body = [&](const char *leaf, const std::string &body) {
    const fs::path path = dir / leaf;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return read_dispersion_benchmark_series(path);
  };

  // Duplicated date.
  {
    const auto series =
        read_body("dup.tsv", "date\tpnl\n2026-01-02\t4\n2026-01-02\t-2\n2026-01-06\t6\n");
    ASSERT_FALSE(series.has_value()) << "a duplicated benchmark date was accepted";
    EXPECT_NE(series.error().to_string().find("2026-01-02"), std::string::npos)
        << series.error().to_string();
  }
  // Reverse order.
  {
    const auto series =
        read_body("rev.tsv", "date\tpnl\n2026-01-06\t6\n2026-01-05\t-2\n2026-01-02\t4\n");
    ASSERT_FALSE(series.has_value()) << "a reversed benchmark series was accepted";
    EXPECT_NE(series.error().to_string().find("ascending"), std::string::npos)
        << series.error().to_string();
  }
  // Non-finite values. `from_chars` parses "nan"/"inf", so these are real rows,
  // not parse failures — they used to flow straight into alpha/beta.
  for (const char *token : {"nan", "inf", "-inf"}) {
    const std::string body =
        "date\tpnl\n2026-01-02\t4\n2026-01-05\t" + std::string(token) + "\n2026-01-06\t6\n";
    const auto series = read_body("nonfinite.tsv", body);
    ASSERT_FALSE(series.has_value()) << "a non-finite benchmark value '" << token
                                     << "' reached the report";
    EXPECT_NE(series.error().to_string().find("non-finite"), std::string::npos)
        << series.error().to_string();
  }
  // Empty date field.
  {
    const auto series = read_body("nodate.tsv", "date\tpnl\n2026-01-02\t4\n\t-2\n");
    ASSERT_FALSE(series.has_value()) << "a benchmark row with no date was accepted";
    EXPECT_NE(series.error().to_string().find("empty date"), std::string::npos)
        << series.error().to_string();
  }

  fs::remove_all(dir, error);
}

// ── REVIEW C-6: the join itself ─────────────────────────────────────────────
namespace {

[[nodiscard]] std::vector<DispersionBenchmarkRow> bench_rows(
    std::initializer_list<std::pair<const char *, double>> rows) {
  std::vector<DispersionBenchmarkRow> out;
  for (const auto &[date, pnl] : rows) {
    out.push_back(DispersionBenchmarkRow{std::string(date), pnl});
  }
  return out;
}

const std::vector<std::string> kStrategyDates = {"2026-01-02", "2026-01-05", "2026-01-06",
                                                 "2026-01-07"};
const std::vector<double> kStrategyPnl = {1.0, -2.0, 3.0, -4.0};

} // namespace

TEST(DispersionBenchmarkJoinUnit, C6_ExactDatesPairsEveryObservationAndNothingElse) {
  const std::vector<DispersionBenchmarkRow> rows = bench_rows(
      {{"2026-01-02", 10.0}, {"2026-01-05", 20.0}, {"2026-01-06", 30.0}, {"2026-01-07", 40.0}});
  const auto paired = pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, rows,
                                                DispersionBenchmarkJoin::ExactDates);
  ASSERT_TRUE(paired.has_value()) << paired.error().to_string();
  EXPECT_EQ(paired->strategy, kStrategyPnl);
  EXPECT_EQ(paired->benchmark, (std::vector<double>{10.0, 20.0, 30.0, 40.0}));
  EXPECT_EQ(paired->n_unmatched, 0u);
}

// THE case. Equal length, ascending, well-formed — and off by one session. This
// is the only shape that produces a confidently wrong number rather than an
// obviously wrong one, so it must be an error and not a silent pairing.
TEST(DispersionBenchmarkJoinUnit, C6_AShiftedEqualLengthSeriesIsRefusedUnderExactDates) {
  const std::vector<DispersionBenchmarkRow> rows = bench_rows(
      {{"2026-01-05", 20.0}, {"2026-01-06", 30.0}, {"2026-01-07", 40.0}, {"2026-01-08", 50.0}});
  const auto paired = pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, rows,
                                                DispersionBenchmarkJoin::ExactDates);
  ASSERT_FALSE(paired.has_value()) << "a shifted equal-length benchmark was paired positionally";
  const std::string message = paired.error().to_string();
  // Both sides of the first disagreement, so an operator knows which file is wrong.
  EXPECT_NE(message.find("2026-01-02"), std::string::npos) << message;
  EXPECT_NE(message.find("2026-01-05"), std::string::npos) << message;
}

TEST(DispersionBenchmarkJoinUnit, C6_AMissingSessionIsRefusedUnderExactDatesAndDroppedUnderInner) {
  const std::vector<DispersionBenchmarkRow> rows =
      bench_rows({{"2026-01-02", 10.0}, {"2026-01-06", 30.0}, {"2026-01-07", 40.0}});
  const auto exact = pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, rows,
                                               DispersionBenchmarkJoin::ExactDates);
  ASSERT_FALSE(exact.has_value()) << "a benchmark missing a session was accepted as complete";

  // The named opt-in compares over the intersection and REPORTS what it dropped.
  const auto inner = pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, rows,
                                               DispersionBenchmarkJoin::InnerJoinOnDates);
  ASSERT_TRUE(inner.has_value()) << inner.error().to_string();
  EXPECT_EQ(inner->strategy, (std::vector<double>{1.0, 3.0, -4.0}));
  EXPECT_EQ(inner->benchmark, (std::vector<double>{10.0, 30.0, 40.0}));
  EXPECT_EQ(inner->n_unmatched, 1u) << "the dropped 2026-01-05 observation was not reported";
}

TEST(DispersionBenchmarkJoinUnit, C6_ALengthMismatchNeverTruncatesTheStrategyTail) {
  // Short benchmark: positional alignment used to silently drop the strategy's
  // last two observations and publish the ratios as if the sample were complete.
  const std::vector<DispersionBenchmarkRow> shorter =
      bench_rows({{"2026-01-02", 10.0}, {"2026-01-05", 20.0}});
  const auto exact = pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, shorter,
                                               DispersionBenchmarkJoin::ExactDates);
  ASSERT_FALSE(exact.has_value()) << "a short benchmark was silently truncated against";
  EXPECT_NE(exact.error().to_string().find("2 sessions"), std::string::npos)
      << exact.error().to_string();

  // Longer than the strategy is equally an error under exact dates.
  const std::vector<DispersionBenchmarkRow> longer =
      bench_rows({{"2026-01-02", 10.0},
                  {"2026-01-05", 20.0},
                  {"2026-01-06", 30.0},
                  {"2026-01-07", 40.0},
                  {"2026-01-08", 50.0}});
  EXPECT_FALSE(pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, longer,
                                         DispersionBenchmarkJoin::ExactDates)
                   .has_value());

  // Under inner join, a longer benchmark is fine — the extra session has no
  // strategy observation to pair with and simply does not appear.
  const auto inner = pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, longer,
                                               DispersionBenchmarkJoin::InnerJoinOnDates);
  ASSERT_TRUE(inner.has_value()) << inner.error().to_string();
  EXPECT_EQ(inner->strategy.size(), 4u);
  EXPECT_EQ(inner->n_unmatched, 0u);
}

TEST(DispersionBenchmarkJoinUnit, C6_FewerThanTwoPairedObservationsIsNotABenchmark) {
  const std::vector<DispersionBenchmarkRow> rows = bench_rows({{"2026-01-06", 30.0}});
  const auto inner = pair_dispersion_benchmark(kStrategyDates, kStrategyPnl, rows,
                                               DispersionBenchmarkJoin::InnerJoinOnDates);
  ASSERT_FALSE(inner.has_value())
      << "a single paired observation was reported as a benchmark comparison";
  EXPECT_NE(inner.error().to_string().find("sample variance"), std::string::npos)
      << inner.error().to_string();
}

// The join policy is a NAMED spec value, not a bool and not an implicit default.
TEST(DispersionRunConfigXB, C6_BenchmarkJoinPolicyIsANamedSpecKeyDefaultingToExact) {
  {
    const fs::path path = write_spec("atx_c6_join_default", kBaselineSpec);
    const auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(config->benchmark_join, DispersionBenchmarkJoin::ExactDates)
        << "the default must not silently restore a partial comparison";
    EXPECT_EQ(to_string(config->benchmark_join), "exact_dates");
  }
  {
    const fs::path path =
        write_spec("atx_c6_join_inner", std::string(kBaselineSpec) + "benchmark_join\tinner\n");
    const auto config = read_dispersion_run_config(path);
    ASSERT_TRUE(config.has_value()) << config.error().to_string();
    EXPECT_EQ(config->benchmark_join, DispersionBenchmarkJoin::InnerJoinOnDates);
    EXPECT_EQ(to_string(config->benchmark_join), "inner_join_on_dates");
  }
  {
    const fs::path path = write_spec("atx_c6_join_bad",
                                     std::string(kBaselineSpec) + "benchmark_join\tpositional\n");
    EXPECT_FALSE(read_dispersion_run_config(path).has_value())
        << "an unknown join policy must be refused by name, not defaulted";
  }
}

// â”€â”€ WS-F F4 (BT-W): the LISTED route's execution knobs â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// X2/X6 routed frictions, financing, limits and costs into the SURFACE backtest
// (`dispersion_backtest_config_from`). The LISTED `run-backtest` â€” the headline
// artifact â€” still built `RunConfig config; config.unpriced = Error;` and
// nothing else, so every published listed NAV was frictionless, carry-free and
// provenance-permissive REGARDLESS of what the spec declared. And even for the
// surface route the value could not reach the run directory, because
// `write_resolved_spec` re-emits only the RunSpec vocabulary at build-corpus
// time and dropped everything else on the floor.

namespace {

// The pinned baseline plus every execution knob turned on. Nothing here is a
// recommendation; the point is that each value is observable downstream.
constexpr const char *kFullyKnobbedSpec =
    "key\tvalue\n"
    "label\tF4 wiring\n"
    "date_lo\t2026-01-02\n"
    "date_hi\t2026-04-30\n"
    "snapshot_suffix\tT19:55:00Z\n"
    "opra_root\tC:\\atx-data\\spy-dispersion\\opra\n"
    "path_template\t{symbol}/{date}.parquet\n"
    "universe_schedule\tuniverse_schedule.tsv\n"
    "flat_rate\t0.043\n"
    "rate_applies_to_financing\t1\n"
    "min_names\t10\n"
    "min_weight_coverage\t0.8\n"
    "target_dte_days\t30\n"
    "min_dte_days\t21\n"
    "max_dte_days\t60\n"
    "roll_dte_days\t7\n"
    "gross_index_vega\t10000\n"
    "delta_band\t0\n"
    "fit_workers\t0\n"
    "core_mode\t0\n"
    "friction_spread_kind\tprice_bps\n"
    "friction_half_spread_bps\t25\n"
    "friction_per_contract_cost\t0.65\n"
    "friction_hedge_slippage_bps\t1.5\n"
    "cost_impact_k\t0.3\n"
    "cost_adv_fraction\t0.05\n"
    "financing_shares_carry\t1\n"
    "financing_initial_cash\t2500000\n"
    "provenance\trequire_admitted_risk\n"
    "unpriced\texclude\n"
    "fill_policy\tcross_spread\n"
    "book_entry_fill_slippage\t1\n"
    "reconcile_nav\t1\n"
    "quote_min_bid\t0.05\n"
    "quote_max_age_ns\t300000000000\n"
    "quote_reject_locked\t1\n";

[[nodiscard]] std::map<std::string, std::string> read_kv(const fs::path &path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos || line.substr(0, tab) == "key") {
      continue;
    }
    out.emplace(line.substr(0, tab), line.substr(tab + 1));
  }
  return out;
}

} // namespace

TEST(DispersionRunConfigStrict, ListedEngineConfigCarriesEveryDeclaredExecutionKnob) {
  const fs::path path = write_spec("atx-disp-cfg-f4-knobs", kFullyKnobbedSpec);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  // THE gate: the engine config the listed replay actually runs under.
  const RunConfig engine = dispersion_engine_run_config_from(*config);

  EXPECT_EQ(engine.frictions.spread_kind, FrictionModel::SpreadKind::PriceBps);
  // REVIEW C-4: the declared 25 bps reaches the engine UNCHANGED, and the
  // square-root impact term (X6) reaches it as its own additive lane. This used
  // to assert `half_spread_bps > 25` because impact was folded into the spread —
  // the fold that discarded a `VolTicks` base. Both halves are asserted, so
  // dropping either one is red.
  EXPECT_DOUBLE_EQ(engine.frictions.half_spread_bps, 25.0);
  EXPECT_NEAR(engine.frictions.impact_fraction, 0.3 * std::pow(0.05, 0.6), 1e-15);
  EXPECT_GT(engine.frictions.impact_fraction, 0.0);
  EXPECT_DOUBLE_EQ(engine.frictions.per_contract_cost, 0.65);
  EXPECT_DOUBLE_EQ(engine.frictions.hedge_slippage_bps, 1.5);
  EXPECT_TRUE(engine.financing.shares_carry);
  EXPECT_DOUBLE_EQ(engine.financing.initial_cash, 2'500'000.0);
  // Task E1: rate_applies_to_financing routes flat_rate into
  // `FinancingConfig::flat_r` -- the field `finance_premium`'s cash-carry
  // accrual actually reads -- not `borrow_rate` (this spec never sets
  // `financing_borrow_rate`, so it stays at its own 0.0 default).
  EXPECT_DOUBLE_EQ(engine.financing.borrow_rate, 0.0);
  ASSERT_TRUE(engine.financing.flat_r.has_value());
  EXPECT_DOUBLE_EQ(*engine.financing.flat_r, 0.043);
  EXPECT_TRUE(engine.financing.finance_premium);
  EXPECT_EQ(engine.surface_provenance_policy, SurfaceProvenancePolicy::RequireAdmittedRisk);
  EXPECT_EQ(engine.unpriced, UnpricedLotPolicy::ExcludeAndReport);
  EXPECT_TRUE(engine.book_entry_fill_slippage);
  EXPECT_TRUE(engine.reconcile_nav);
  EXPECT_EQ(config->fill_policy, ScheduleFillPolicy::CrossSpread);
  EXPECT_DOUBLE_EQ(config->quote_quality.min_bid, 0.05);
  EXPECT_EQ(config->quote_quality.max_quote_age_ns, 300'000'000'000LL);
  EXPECT_TRUE(config->quote_quality.reject_locked);

  EXPECT_EQ(dispersion_friction_regime(*config), DispersionFrictionRegime::FrictionedWithImpact);
}

TEST(DispersionRunConfigStrict, DefaultSpecStillYieldsThePinnedFrictionlessEngineConfig) {
  const fs::path path = write_spec("atx-disp-cfg-f4-default", kBaselineSpec);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  // The reproduction guarantee: a spec that names no execution knob must produce
  // exactly the engine config the pinned golden ran under.
  const RunConfig engine = dispersion_engine_run_config_from(*config);
  const RunConfig pinned{}; // the pre-F4 listed route was this, plus unpriced=Error
  EXPECT_EQ(engine.frictions.spread_kind, FrictionModel::SpreadKind::None);
  EXPECT_DOUBLE_EQ(engine.frictions.half_spread_bps, pinned.frictions.half_spread_bps);
  EXPECT_DOUBLE_EQ(engine.frictions.vol_tick, pinned.frictions.vol_tick);
  EXPECT_DOUBLE_EQ(engine.frictions.per_contract_cost, pinned.frictions.per_contract_cost);
  EXPECT_DOUBLE_EQ(engine.frictions.hedge_slippage_bps, pinned.frictions.hedge_slippage_bps);
  EXPECT_DOUBLE_EQ(engine.financing.borrow_rate, pinned.financing.borrow_rate);
  EXPECT_EQ(engine.financing.finance_premium, pinned.financing.finance_premium);
  EXPECT_EQ(engine.financing.shares_carry, pinned.financing.shares_carry);
  EXPECT_DOUBLE_EQ(engine.financing.initial_cash, pinned.financing.initial_cash);
  EXPECT_EQ(engine.surface_provenance_policy, SurfaceProvenancePolicy::Compatibility);
  EXPECT_EQ(engine.unpriced, UnpricedLotPolicy::Error); // the pre-F4 hardcode
  EXPECT_FALSE(engine.book_entry_fill_slippage);
  EXPECT_FALSE(engine.reconcile_nav);
  EXPECT_EQ(config->fill_policy, ScheduleFillPolicy::ModelMark);
}

TEST(DispersionRunConfigStrict, QuoteSideFillWithoutSlippageBookingIsRejected) {
  // A fill policy the engine does not CHARGE is invisible in NAV (F2), so the
  // combination is refused rather than shipped as a knob that does nothing.
  std::string body = kBaselineSpec;
  body += "fill_policy\tcross_spread\n";
  const fs::path path = write_spec("atx-disp-cfg-f4-inert-fill", body);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_FALSE(config);
  EXPECT_EQ(config.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(config.error().message().find("book_entry_fill_slippage"), std::string::npos)
      << config.error().message();
}

TEST(DispersionRunConfigStrict, EffectiveRunConfigArtifactRecordsRegimeFirstAndEveryValue) {
  const fs::path path = write_spec("atx-disp-cfg-f4-artifact", kFullyKnobbedSpec);
  const Result<DispersionRunConfig> config = read_dispersion_run_config(path);
  ASSERT_TRUE(config) << config.error().to_string();

  const fs::path artifact = path.parent_path() / "run_config.tsv";
  ASSERT_TRUE(write_dispersion_effective_config(artifact, *config).has_value());

  // REGIME FIRST (M4): the very first data row names the execution regime.
  std::ifstream in(artifact, std::ios::binary);
  std::string header;
  std::string first;
  ASSERT_TRUE(std::getline(in, header));
  ASSERT_TRUE(std::getline(in, first));
  EXPECT_EQ(header.substr(0, 9), "key\tvalue");
  EXPECT_EQ(first.substr(0, first.find('\t')), "friction_regime");

  const std::map<std::string, std::string> kv = read_kv(artifact);
  for (const char *key : {"friction_regime", "economics_rev", "friction_regime_detail",
                          "friction_spread_kind", "friction_half_spread_bps",
                          "friction_per_contract_cost", "friction_hedge_slippage_bps",
                          "cost_impact_k", "cost_adv_fraction", "financing_borrow_rate",
                          "financing_finance_premium", "financing_shares_carry",
                          "financing_initial_cash", "provenance", "unpriced", "fill_policy",
                          "book_entry_fill_slippage", "reconcile_nav", "quote_min_bid",
                          "quote_max_age_ns", "quote_reject_locked"}) {
    EXPECT_NE(kv.find(key), kv.end()) << "run_config.tsv is missing " << key;
  }
  EXPECT_EQ(kv.at("provenance"), "require_admitted_risk");
  EXPECT_EQ(kv.at("unpriced"), "exclude");
  EXPECT_EQ(kv.at("fill_policy"), "cross_spread");
  EXPECT_EQ(kv.at("book_entry_fill_slippage"), "1");
  EXPECT_EQ(kv.at("quote_max_age_ns"), "300000000000");
  EXPECT_NE(kv.at("friction_regime_detail").find("bps half-spread"), std::string::npos)
      << kv.at("friction_regime_detail");
  // E1 fix round: run_config.tsv is a per-run artifact -- it must name WHICH
  // revision of the engine's economics interpretation produced its numbers,
  // right beside the assumptions it already names.
  EXPECT_EQ(kv.at("economics_rev"), std::to_string(kBacktestEconomicsRev));
}

TEST(DispersionRunConfigStrict, BuildCorpusPreservesEveryTypedKeyIntoTheRunDirectory) {
  // `write_resolved_spec` re-emits ONLY the RunSpec vocabulary, so build-corpus
  // used to erase every typed knob when it rewrote the run dir's spec: the value
  // was declared, accepted, and then unreachable by every later stage.
  const fs::path source = write_spec("atx-disp-cfg-f4-source", kFullyKnobbedSpec);

  const fs::path run_dir = fs::temp_directory_path() / "atx-disp-cfg-f4-rundir";
  std::error_code error;
  fs::remove_all(run_dir, error);
  fs::create_directories(run_dir, error);
  const fs::path run_spec = run_dir / "run_spec.tsv";

  // What build-corpus writes: the RunSpec projection only.
  {
    const Result<RunSpec> spec = read_run_spec(source);
    ASSERT_TRUE(spec) << spec.error().to_string();
    ASSERT_TRUE(write_resolved_spec(run_spec, *spec).has_value());
  }
  const std::map<std::string, std::string> projected = read_kv(run_spec);
  ASSERT_EQ(projected.find("friction_half_spread_bps"), projected.end())
      << "fixture assumption broken: the RunSpec writer already carries typed keys";

  // ... and the F4 repair.
  ASSERT_TRUE(persist_typed_spec_keys(source, run_spec).has_value());
  const std::map<std::string, std::string> preserved = read_kv(run_spec);
  for (const char *key :
       {"friction_spread_kind", "friction_half_spread_bps", "friction_per_contract_cost",
        "friction_hedge_slippage_bps", "cost_impact_k", "cost_adv_fraction",
        "rate_applies_to_financing", "financing_shares_carry", "financing_initial_cash",
        "provenance", "unpriced", "fill_policy", "book_entry_fill_slippage", "reconcile_nav",
        "quote_min_bid", "quote_max_age_ns", "quote_reject_locked"}) {
    EXPECT_NE(preserved.find(key), preserved.end()) << "run dir spec lost " << key;
  }
  // The RunSpec keys are NOT duplicated (a duplicate key is a hard parse error).
  const Result<DispersionRunConfig> round_trip = read_dispersion_run_config(run_spec);
  ASSERT_TRUE(round_trip) << round_trip.error().to_string();
  EXPECT_DOUBLE_EQ(round_trip->frictions.half_spread_bps, 25.0);
  EXPECT_EQ(round_trip->fill_policy, ScheduleFillPolicy::CrossSpread);
  EXPECT_EQ(round_trip->provenance, SurfaceProvenancePolicy::RequireAdmittedRisk);
  EXPECT_EQ(round_trip->quote_quality.max_quote_age_ns, 300'000'000'000LL);

  fs::remove_all(run_dir, error);
}

TEST(DispersionRunConfigStrict, QuoteRejectReportIsAPerDateAuditTrailForTheAdmissionGates) {
  // F6's counters existed only in memory, so "check quote_rejects if a schedule
  // SHA moves" was not performable after the fact. `build-schedule` now persists
  // them; this pins the shape and the arithmetic of that artifact.
  const fs::path dir = fs::temp_directory_path() / "atx-disp-quote-rejects";
  std::error_code error;
  fs::remove_all(dir, error);
  fs::create_directories(dir, error);
  const fs::path path = dir / "quote_rejects.tsv";

  ListedQuoteRejectCounts clean{};
  ListedQuoteRejectCounts dirty{};
  dirty.zero_bid = 2u;
  dirty.stale = 3u;
  dirty.stale_unevaluable = 7u; // reported, NOT dropped
  dirty.locked = 5u;            // flagged, NOT dropped under the default policy
  dirty.non_standard = 1u;
  // FIX-F M2: the subset the policy REFUSED. Under the default no-drop policy
  // this is 0 and `total_dropped` is unchanged; when `reject_locked` is set it
  // must be counted, or a policy-dropped quote appears in no dropped total.
  ListedQuoteRejectCounts strict = dirty;
  strict.locked_dropped = 4u;
  // FIX-F m4: a date whose selection FAILED still gets a row, marked `no_basket`
  // and carrying the first candidate expiry's tally.
  const std::vector<QuoteRejectRow> rows = {{"2026-01-02", true, clean},
                                            {"2026-02-02", true, dirty},
                                            {"2026-03-02", false, strict}};
  ASSERT_TRUE(write_quote_reject_report(path, rows).has_value());

  std::ifstream in(path, std::ios::binary);
  std::string schema;
  std::string header;
  std::string row0;
  std::string row1;
  std::string row2;
  ASSERT_TRUE(std::getline(in, schema));
  ASSERT_TRUE(std::getline(in, header));
  ASSERT_TRUE(std::getline(in, row0));
  ASSERT_TRUE(std::getline(in, row1));
  ASSERT_TRUE(std::getline(in, row2));
  // FIX-F m5: a version line, so a positional reader written against an older
  // column order fails loudly instead of silently shifting one column left.
  EXPECT_EQ(schema, "# schema=quote_rejects/1");
  EXPECT_EQ(header, "date\tselection\tnot_two_sided\tzero_bid\tstale\tstale_unevaluable\tlocked\t"
                    "locked_dropped\tnon_standard\ttotal_dropped");
  EXPECT_EQ(row0, "2026-01-02\tok\t0\t0\t0\t0\t0\t0\t0\t0");
  // total_dropped = 2 + 3 + 1 = 6: `locked` is flagged but admitted under the
  // default policy, and `stale_unevaluable` is a measurability report rather
  // than a rejection, so neither may inflate the dropped count.
  EXPECT_EQ(row1, "2026-02-02\tok\t0\t2\t3\t7\t5\t0\t1\t6");
  EXPECT_EQ(dirty.total_dropped(), 6u);
  // ... and 6 + 4 = 10 once the policy actually refuses the locked markets.
  EXPECT_EQ(row2, "2026-03-02\tno_basket\t0\t2\t3\t7\t5\t4\t1\t10");
  EXPECT_EQ(strict.total_dropped(), 10u);

  fs::remove_all(dir, error);
}
