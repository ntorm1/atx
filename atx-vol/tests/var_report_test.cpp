#include "atx/vol/var_report.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "atx/vol/var.hpp"

namespace {

using namespace atx::vol;

// Synthetic 2-scenario x 2-leg HistoricalVarResult, entirely hand-filled (no
// replay is run). Scenario 0 is a normal Ok transition; scenario 1 is a
// VarScenarioStatus::ArchiveError transition (Task 2) whose legs are poisoned
// to VarLegStatus::SurfaceUnavailable, mirroring what
// var.cpp's fill_loaded_failure/poison_leg actually produce (every leg in a
// structurally-failed scenario reports non-Ok, even though the scenario
// itself failed for an infrastructure reason, not a market one).
//
// Every numeric field that write_var_scenario_tsv or attribute_by_underlier
// actually reads is chosen to be exactly representable in binary (an integer
// or a multiple of 0.5) so std::setprecision(17) default-float formatting
// prints it as a short, unambiguous literal -- the golden string below is
// hand-computed from these values, not captured from a run.
HistoricalVarResult make_synthetic_result() {
  HistoricalVarResult result;
  result.reference_date = "2026-01-06";
  result.n_legs = 2u;
  result.base_dates = {"2026-01-02", "2026-01-05"};
  result.shifted_dates = {"2026-01-05", "2026-01-06"};

  VarScenarioFrame scenario0;
  scenario0.base_ts_ns = 100;
  scenario0.shifted_ts_ns = 200;
  scenario0.status = VarScenarioStatus::Ok;
  scenario0.base_value = 18.0;     // leg00.base_value(50) + leg01.base_value(-32)
  scenario0.shifted_value = 19.0;  // leg00.shifted_value(55) + leg01.shifted_value(-36)
  scenario0.pnl = 1.0;             // leg00.pnl(5) + leg01.pnl(-4)
  scenario0.dollar_delta = 1350.0; // leg00.dollar_delta(750) + leg01.dollar_delta(600)
  scenario0.n_ok = 2u;
  scenario0.n_failed = 0u;

  VarScenarioFrame scenario1;
  scenario1.base_ts_ns = 300;
  scenario1.shifted_ts_ns = 400;
  scenario1.status = VarScenarioStatus::ArchiveError;
  scenario1.base_value = 0.0;
  scenario1.shifted_value = 0.0;
  scenario1.pnl = 0.0;
  scenario1.dollar_delta = 0.0;
  scenario1.n_ok = 0u;
  scenario1.n_failed = 2u;

  result.frames = {scenario0, scenario1};

  VarLegFrame leg00; // scenario 0, position 0 (AAPL)
  leg00.kind = VarLegKind::Option;
  leg00.status = VarLegStatus::Ok;
  leg00.uid = 111u;
  leg00.units = 10.0;
  leg00.base_delta = 0.5;
  leg00.dollar_delta = 750.0;
  leg00.base_mark = 5.0;
  leg00.shifted_mark = 5.5;
  leg00.base_value = 50.0;
  leg00.shifted_value = 55.0;
  leg00.pnl = 5.0;

  VarLegFrame leg01; // scenario 0, position 1 (MSFT)
  leg01.kind = VarLegKind::Option;
  leg01.status = VarLegStatus::Ok;
  leg01.uid = 222u;
  leg01.units = -4.0;
  leg01.base_delta = -0.5;
  leg01.dollar_delta = 600.0;
  leg01.base_mark = 8.0;
  leg01.shifted_mark = 9.0;
  leg01.base_value = -32.0;
  leg01.shifted_value = -36.0;
  leg01.pnl = -4.0;

  VarLegFrame leg10; // scenario 1 (ArchiveError), position 0 -- poisoned
  leg10.kind = VarLegKind::Option;
  leg10.status = VarLegStatus::SurfaceUnavailable;
  leg10.uid = 111u;

  VarLegFrame leg11; // scenario 1 (ArchiveError), position 1 -- poisoned
  leg11.kind = VarLegKind::Option;
  leg11.status = VarLegStatus::SurfaceUnavailable;
  leg11.uid = 222u;

  result.leg_frames = {leg00, leg01, leg10, leg11};
  return result;
}

std::vector<VarReferenceLeg> make_reference_legs() {
  VarReferenceLeg aapl;
  aapl.kind = VarLegKind::Option;
  aapl.uid = 111u;
  aapl.underlier = "AAPL";
  aapl.reference_units = 10.0;
  aapl.reference_spot = 150.0;
  aapl.reference_mark = 5.0;
  aapl.reference_delta = 0.5;
  aapl.target_dollar_delta = 75000.0;
  aapl.target_abs_delta = 0.5;
  aapl.log_moneyness = 0.25;

  VarReferenceLeg msft;
  msft.kind = VarLegKind::Option;
  msft.uid = 222u;
  msft.underlier = "MSFT";
  msft.reference_units = -4.0;
  msft.reference_spot = 300.0;
  msft.reference_mark = 8.0;
  msft.reference_delta = -0.5;
  msft.target_dollar_delta = -36000.0;
  msft.target_abs_delta = 0.5;
  msft.log_moneyness = -0.25;

  return {aapl, msft};
}

VarExclusionSummary make_exclusions() {
  VarExclusionSummary exclusions;
  exclusions.source_option_lots = 100u;
  exclusions.coverage_excluded_option_lots = 20u;
  exclusions.delta_boundary_excluded_option_lots = 5u;
  exclusions.replay_excluded_option_lots = 3u;
  exclusions.stock_hedges = 7u;
  return exclusions;
}

// Hand-computed from make_synthetic_result()/make_reference_legs()/
// make_exclusions(). Row 0 (scenario 0, Ok): the largest-|pnl| leg is
// position 0 (AAPL, |5| > |-4|). Row 1 (scenario 1, ArchiveError): both legs
// are poisoned to pnl=0, so std::max_element's strict '<' comparator never
// updates past the first element -- position 0 (AAPL) again, deterministically.
constexpr char kExpectedTsvWithLegs[] =
    "base_date\tshifted_date\tbase_value\tshifted_value\tpnl\tcumulative_pnl\t"
    "dollar_delta\tn_positions\tsource_option_lots\tcoverage_excluded_option_lots\t"
    "delta_boundary_excluded_option_lots\treplay_excluded_option_lots\tstock_hedges\t"
    "max_abs_leg_index\tmax_abs_leg_underlier\t"
    "max_abs_leg_reference_units\tmax_abs_leg_reference_delta\t"
    "max_abs_leg_target_dollar_delta\tmax_abs_leg_log_moneyness\t"
    "max_abs_leg_scenario_units\tmax_abs_leg_base_delta\tmax_abs_leg_base_mark\t"
    "max_abs_leg_shifted_mark\tmax_abs_leg_pnl\n"
    "2026-01-02\t2026-01-05\t18\t19\t1\t1\t1350\t2\t100\t20\t5\t3\t7\t"
    "0\tAAPL\t10\t0.5\t75000\t0.25\t10\t0.5\t5\t5.5\t5\n"
    "2026-01-05\t2026-01-06\t0\t0\t0\t1\t0\t0\t100\t20\t5\t3\t7\t"
    "0\tAAPL\t10\t0.5\t75000\t0.25\t0\t0\t0\t0\t0\n";

TEST(VarReport, ArchiveErrorStatusStringIsStable) {
  // Task 2 appended VarScenarioStatus::ArchiveError; pin its to_string here
  // since the golden fixture below exercises it as real data.
  EXPECT_STREQ(to_string(VarScenarioStatus::ArchiveError), "ArchiveError");
}

TEST(VarReport, ScenarioTsvMatchesGoldenStringWithRetainedLegs) {
  const HistoricalVarResult result = make_synthetic_result();
  const std::vector<VarReferenceLeg> reference_legs = make_reference_legs();
  const VarExclusionSummary exclusions = make_exclusions();

  std::ostringstream out;
  const Status status = write_var_scenario_tsv(out, result, exclusions, reference_legs);
  ASSERT_TRUE(status) << (status ? std::string{} : status.error().to_string());
  EXPECT_EQ(out.str(), kExpectedTsvWithLegs);
}

TEST(VarReport, ScenarioTsvEmitsEmptyLegColumnsWithoutRetainedLegs) {
  HistoricalVarResult result = make_synthetic_result();
  result.leg_frames.clear();
  const VarExclusionSummary exclusions = make_exclusions();

  std::ostringstream out;
  // reference_legs omitted -- defaults to empty and must be ignored, per
  // contract, whenever result.leg_frames is empty.
  const Status status = write_var_scenario_tsv(out, result, exclusions);
  ASSERT_TRUE(status) << (status ? std::string{} : status.error().to_string());

  // 11 empty max_abs_leg_* columns need 11 tabs after the "7" (stock_hedges)
  // value: the separator before the first empty column, plus 10 more between
  // the 11 empty columns themselves.
  const std::string empty_leg_columns(11u, '\t');
  const std::string expected =
      "base_date\tshifted_date\tbase_value\tshifted_value\tpnl\tcumulative_pnl\t"
      "dollar_delta\tn_positions\tsource_option_lots\tcoverage_excluded_option_lots\t"
      "delta_boundary_excluded_option_lots\treplay_excluded_option_lots\tstock_hedges\t"
      "max_abs_leg_index\tmax_abs_leg_underlier\t"
      "max_abs_leg_reference_units\tmax_abs_leg_reference_delta\t"
      "max_abs_leg_target_dollar_delta\tmax_abs_leg_log_moneyness\t"
      "max_abs_leg_scenario_units\tmax_abs_leg_base_delta\tmax_abs_leg_base_mark\t"
      "max_abs_leg_shifted_mark\tmax_abs_leg_pnl\n"
      "2026-01-02\t2026-01-05\t18\t19\t1\t1\t1350\t2\t100\t20\t5\t3\t7" +
      empty_leg_columns + "\n" + "2026-01-05\t2026-01-06\t0\t0\t0\t1\t0\t0\t100\t20\t5\t3\t7" +
      empty_leg_columns + "\n";
  EXPECT_EQ(out.str(), expected);
}

TEST(VarReport, ScenarioTsvRejectsMismatchedReferenceLegCount) {
  const HistoricalVarResult result = make_synthetic_result();
  const VarExclusionSummary exclusions = make_exclusions();
  const std::vector<VarReferenceLeg> one_leg = {make_reference_legs().front()};

  std::ostringstream out;
  const Status status = write_var_scenario_tsv(out, result, exclusions, one_leg);
  EXPECT_FALSE(status);
}

// A reference_legs span with the RIGHT COUNT but the WRONG identity (here,
// simply reordered) must be rejected, not silently trusted by position: a
// caller passing a reordered or unrelated-but-coincidentally-sized span
// would otherwise get every max_abs_leg_* name/reference-field silently
// misattributed to the wrong leg.
TEST(VarReport, ScenarioTsvRejectsReorderedReferenceLegs) {
  const HistoricalVarResult result = make_synthetic_result();
  const VarExclusionSummary exclusions = make_exclusions();
  const std::vector<VarReferenceLeg> ordered = make_reference_legs();
  const std::vector<VarReferenceLeg> reordered = {ordered[1],
                                                  ordered[0]}; // same count, wrong uid per position

  std::ostringstream out;
  const Status status = write_var_scenario_tsv(out, result, exclusions, reordered);
  EXPECT_FALSE(status);
  EXPECT_TRUE(out.str().empty()) << "a rejected call must not have written partial output";
}

// Schema-stability gate (Task 4 step 3): the header this engine-level writer
// produces must byte-match the header of the real SP100 fixture artifact
// var_bench used to hand-roll, proving the extraction did not silently
// change the column set/order/naming. Skips (not fails) when the artifact is
// absent -- it is a locally-generated bench fixture, not a repo file.
TEST(VarReport, ScenarioTsvHeaderMatchesArchivedSp100Artifact) {
  std::ifstream artifact{"C:/atx/artifacts/var/sp100_dispersion_ytd_pnl_cross.tsv"};
  if (!artifact.is_open()) {
    GTEST_SKIP() << "sp100_dispersion_ytd_pnl_cross.tsv fixture artifact not present on this host";
  }
  std::string artifact_header;
  ASSERT_TRUE(std::getline(artifact, artifact_header));

  const HistoricalVarResult empty_result;
  const VarExclusionSummary exclusions;
  std::ostringstream out;
  const Status status = write_var_scenario_tsv(out, empty_result, exclusions);
  ASSERT_TRUE(status) << (status ? std::string{} : status.error().to_string());
  const std::string produced = out.str();
  const std::size_t header_end = produced.find('\n');
  ASSERT_NE(header_end, std::string::npos);
  EXPECT_EQ(produced.substr(0, header_end), artifact_header);
}

TEST(VarReport, AttributeByUnderlierHandComputedTotalsWorstAndOrdering) {
  const HistoricalVarResult result = make_synthetic_result();
  const std::vector<VarReferenceLeg> reference_legs = make_reference_legs();

  const Result<std::vector<VarUnderlierAttribution>> attribution =
      attribute_by_underlier(result, reference_legs);
  ASSERT_TRUE(attribution) << (attribution ? std::string{} : attribution.error().to_string());

  // Only scenario 0's legs are Ok; scenario 1 (ArchiveError) contributes
  // nothing. AAPL: total_pnl = 5 (single Ok observation). MSFT: total_pnl =
  // -4 (single Ok observation). Ascending by total_pnl -> MSFT (the loss)
  // sorts first, AAPL (the gain) second.
  ASSERT_EQ(attribution->size(), 2u);
  const VarUnderlierAttribution &worst = (*attribution)[0];
  const VarUnderlierAttribution &best = (*attribution)[1];
  EXPECT_EQ(worst.underlier, "MSFT");
  EXPECT_DOUBLE_EQ(worst.total_pnl, -4.0);
  EXPECT_DOUBLE_EQ(worst.worst_scenario_pnl, -4.0);
  EXPECT_EQ(worst.worst_scenario_base_ts_ns, 100);

  EXPECT_EQ(best.underlier, "AAPL");
  EXPECT_DOUBLE_EQ(best.total_pnl, 5.0);
  EXPECT_DOUBLE_EQ(best.worst_scenario_pnl, 5.0);
  EXPECT_EQ(best.worst_scenario_base_ts_ns, 100);
}

TEST(VarReport, AttributeByUnderlierRequiresRetainedLegFrames) {
  HistoricalVarResult result = make_synthetic_result();
  result.leg_frames.clear();
  const std::vector<VarReferenceLeg> reference_legs = make_reference_legs();

  const Result<std::vector<VarUnderlierAttribution>> attribution =
      attribute_by_underlier(result, reference_legs);
  ASSERT_FALSE(attribution);
  EXPECT_NE(attribution.error().to_string().find("retain_leg_frames"), std::string::npos)
      << attribution.error().to_string();
}

TEST(VarReport, AttributeByUnderlierRejectsMismatchedReferenceLegCount) {
  const HistoricalVarResult result = make_synthetic_result();
  const std::vector<VarReferenceLeg> one_leg = {make_reference_legs().front()};

  const Result<std::vector<VarUnderlierAttribution>> attribution =
      attribute_by_underlier(result, one_leg);
  EXPECT_FALSE(attribution);
}

// Same identity concern as ScenarioTsvRejectsReorderedReferenceLegs: a
// same-sized but reordered reference_legs span must never be trusted by
// position alone, or P&L gets silently attributed to the wrong underlier.
TEST(VarReport, AttributeByUnderlierRejectsReorderedReferenceLegs) {
  const HistoricalVarResult result = make_synthetic_result();
  const std::vector<VarReferenceLeg> ordered = make_reference_legs();
  const std::vector<VarReferenceLeg> reordered = {ordered[1], ordered[0]};

  const Result<std::vector<VarUnderlierAttribution>> attribution =
      attribute_by_underlier(result, reordered);
  EXPECT_FALSE(attribution);
}

// Two underliers landing on the exact same total_pnl must not produce
// nondeterministic (hash-order-dependent) output ordering: stable_sort over
// discovery order (scenario-major, position-ascending) must keep AAPL
// (reference position 0) ahead of MSFT (position 1) on a tie.
TEST(VarReport, AttributeByUnderlierIsStableOnEqualTotalPnlTies) {
  HistoricalVarResult result;
  result.n_legs = 2u;
  result.base_dates = {"2026-01-02"};
  result.shifted_dates = {"2026-01-05"};

  VarScenarioFrame scenario;
  scenario.base_ts_ns = 100;
  scenario.shifted_ts_ns = 200;
  scenario.status = VarScenarioStatus::Ok;
  scenario.n_ok = 2u;
  result.frames = {scenario};

  VarLegFrame leg_aapl; // position 0
  leg_aapl.status = VarLegStatus::Ok;
  leg_aapl.uid = 111u;
  leg_aapl.pnl = 3.0;

  VarLegFrame leg_msft; // position 1
  leg_msft.status = VarLegStatus::Ok;
  leg_msft.uid = 222u;
  leg_msft.pnl = 3.0; // exact tie with leg_aapl

  result.leg_frames = {leg_aapl, leg_msft};

  const std::vector<VarReferenceLeg> reference_legs = make_reference_legs();
  const Result<std::vector<VarUnderlierAttribution>> attribution =
      attribute_by_underlier(result, reference_legs);
  ASSERT_TRUE(attribution) << (attribution ? std::string{} : attribution.error().to_string());
  ASSERT_EQ(attribution->size(), 2u);
  EXPECT_EQ((*attribution)[0].underlier, "AAPL");
  EXPECT_DOUBLE_EQ((*attribution)[0].total_pnl, 3.0);
  EXPECT_EQ((*attribution)[1].underlier, "MSFT");
  EXPECT_DOUBLE_EQ((*attribution)[1].total_pnl, 3.0);
}

// A stock hedge and an option leg on the same underlier must roll into one
// attribution row keyed on the underlier string, not split by VarLegKind.
// worst_scenario_pnl also demonstrates it is NOT just total_pnl restated: the
// stock hedge's -2 is worse than the option leg's +5 even though the total
// (+3) is positive.
TEST(VarReport, AttributeByUnderlierRollsUpStockAndOptionLegsForSameUnderlier) {
  HistoricalVarResult result;
  result.n_legs = 2u;
  result.base_dates = {"2026-01-02"};
  result.shifted_dates = {"2026-01-05"};

  VarScenarioFrame scenario;
  scenario.base_ts_ns = 100;
  scenario.shifted_ts_ns = 200;
  scenario.status = VarScenarioStatus::Ok;
  scenario.n_ok = 2u;
  result.frames = {scenario};

  VarLegFrame option_leg; // position 0: AAPL option
  option_leg.kind = VarLegKind::Option;
  option_leg.status = VarLegStatus::Ok;
  option_leg.uid = 111u;
  option_leg.pnl = 5.0;

  VarLegFrame stock_leg; // position 1: AAPL stock hedge, same underlier/uid
  stock_leg.kind = VarLegKind::Stock;
  stock_leg.status = VarLegStatus::Ok;
  stock_leg.uid = 111u;
  stock_leg.pnl = -2.0;

  result.leg_frames = {option_leg, stock_leg};

  VarReferenceLeg option_reference;
  option_reference.kind = VarLegKind::Option;
  option_reference.uid = 111u;
  option_reference.underlier = "AAPL";

  VarReferenceLeg stock_reference;
  stock_reference.kind = VarLegKind::Stock;
  stock_reference.uid = 111u;
  stock_reference.underlier = "AAPL";

  const std::vector<VarReferenceLeg> reference_legs = {option_reference, stock_reference};
  const Result<std::vector<VarUnderlierAttribution>> attribution =
      attribute_by_underlier(result, reference_legs);
  ASSERT_TRUE(attribution) << (attribution ? std::string{} : attribution.error().to_string());
  ASSERT_EQ(attribution->size(), 1u);
  EXPECT_EQ((*attribution)[0].underlier, "AAPL");
  EXPECT_DOUBLE_EQ((*attribution)[0].total_pnl, 3.0);
  EXPECT_DOUBLE_EQ((*attribution)[0].worst_scenario_pnl, -2.0);
  EXPECT_EQ((*attribution)[0].worst_scenario_base_ts_ns, 100);
}

TEST(VarReport, AttributeByUnderlierReturnsEmptyForNoScenarios) {
  const HistoricalVarResult empty_result;
  const Result<std::vector<VarUnderlierAttribution>> attribution =
      attribute_by_underlier(empty_result, {});
  ASSERT_TRUE(attribution) << (attribution ? std::string{} : attribution.error().to_string());
  EXPECT_TRUE(attribution->empty());
}

} // namespace
