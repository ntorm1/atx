#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "oracle_convention_sweep.hpp"
#include "oracle_conventions.hpp"

namespace {

using namespace atx::vol;
using namespace atx::vol::oracle;

// Synthesizes a row whose oracle columns were produced BY `map`, so a sweep
// over such rows must resolve back to `map`'s input model AND its exercise
// style. It authors through `mode_a_price_row` — the same entry point the sweep
// and production Mode A price with — so the synthetic oracle honours the map's
// exercise-style rule instead of being unconditionally American.
OracleRow make_row(double strike, Side side, const ConventionMap &map, double ddiv = 0.0,
                   double sdiv = 0.0, std::string_view underlier = "SYNTH") {
  OracleRow row;
  row.underlier = std::string(underlier);
  row.side = side;
  row.strike = strike;
  row.uprc = 100.0;
  row.rate = 0.04;
  row.sdiv = sdiv;
  row.ddiv = ddiv;
  row.years = 45.0 / 365.0;
  row.sr_vol = 0.25;
  row.bid_prc = 1.0;
  row.ask_prc = 1.1;
  // The single-expiry schedule the sweep's pre-pass reconstructs for this
  // row's snapshot: every make_row cohort shares one `years`, so its whole
  // ddiv is one dividend whose upper bracket is the expiry itself. Authoring
  // with the same schedule keeps the tree arm a FIXED POINT of the sweep the
  // way the escrow arms are. Non-tree maps ignore it.
  std::vector<CashDividend> schedule;
  if (row.ddiv > 0.0) {
    schedule.push_back(CashDividend{row.years, row.ddiv});
  }
  const Result<ModeAPricing> priced = mode_a_price_row(
      row, map, al_fast_opts(), RowDividends{.schedule = schedule, .refused = false});
  EXPECT_TRUE(priced.has_value());
  if (!priced.has_value()) {
    return row;
  }
  EXPECT_TRUE(std::isfinite(priced->dp_dq));
  row.sr_prc = price_to_oracle_units(priced->greeks.price, map);
  // The AXIS-AWARE conversion, not the jet-only overload: a synthetic oracle
  // authored under a `Secant252` map must carry that map's SECANTS in `th` and
  // `de_decay`, or a sweep over these rows could not resolve back to the map
  // that produced them — the property every fixed-point test here rests on.
  const OracleUnitGreeks units = to_oracle_units(*priced, map);
  row.de = units.de;
  row.ga = units.ga;
  row.th = units.th;
  row.ve = units.ve;
  row.rh = units.rh;
  row.ph = units.ph;
  row.vo = units.vo;
  row.va = units.va;
  row.de_decay = units.de_decay;
  return row;
}

OracleRow make_row(double strike, Side side) {
  return make_row(strike, side, baseline_convention());
}

// A cohort authored under a NON-baseline input model, so the sweep's winner arm
// and its baseline arm price different inputs and both must run per row.
ConventionMap distinct_arm_map() {
  ConventionMap map = baseline_convention();
  map.input_model = InputModel::DiscreteDividendPvSdivYield;
  return map;
}

std::vector<OracleRow> distinct_arm_rows(double first_strike, double second_strike) {
  const ConventionMap map = distinct_arm_map();
  return {make_row(first_strike, Side::Call, map, 2.5, 0.01),
          make_row(second_strike, Side::Put, map, 2.5, 0.01)};
}

// A cohort spanning routed and unrouted roots, authored under `map`. A sweep
// over it must resolve back to `map`'s exercise style.
//
// THREE roots: an unrouted equity (SYNTH) that keeps the American arm honest,
// and two contract-fact European roots (SPX, and MGTN since its Cboe spec was
// located). With the empirical table empty the two European rules route
// identical sets, so they tie bit-for-bit on every metric and the sweep's
// deterministic identity tie-break resolves the plain
// `european_cash_settled_index` id — which is exactly what the production map
// pins. The deep-in-the-money puts are what make the European and American
// legs separable at all — that is where the early-exercise premium is worth
// ~0.9 per share rather than ~0.
//
// Non-zero ddiv/sdiv on the SYNTH rows keep the production input model
// distinguishable from the other eight at the same time. The two EUROPEAN
// rows carry ddiv == 0, exactly as their real roots do (every European-routed
// root in the store is dividend-free) — and not only for realism: a cash
// dividend at expiry makes a deep-ITM put's early exercise worthless (holding
// captures the dividend-driven forward drop), so under the tree model a
// ddiv-bearing SPX row prices IDENTICALLY through both exercise legs and the
// style axis would resolve on the identity tie-break instead of on evidence.
// Dividend-free, the two legs separate exactly as designed: the American leg
// floors at intrinsic, the European premium sits below it.
std::vector<OracleRow> rows_under(const ConventionMap &map, double first_strike,
                                  double second_strike) {
  return {make_row(first_strike, Side::Call, map, 2.5, 0.01),
          make_row(second_strike, Side::Put, map, 2.5, 0.01),
          make_row(200.0, Side::Put, map, 0.0, 0.01, "SPX"),
          make_row(200.0, Side::Put, map, 0.0, 0.01, "MGTN")};
}

// A cohort authored under the PRODUCTION map, so a sweep over it must resolve
// back to winning_convention().
std::vector<OracleRow> production_rows(double first_strike, double second_strike) {
  return rows_under(winning_convention(), first_strike, second_strike);
}

// erf-based Black-Scholes, deliberately NOT the repo's own Black-76 kernel: it
// is the independent rung `european_greeks` is checked against, and a check
// that reused the kernel under test would only prove the kernel equals itself.
[[nodiscard]] double independent_ncdf(double x) {
  return 0.5 * std::erfc(-x * 0.70710678118654752440);
}

[[nodiscard]] double independent_euro(double S, double K, double T, double sigma, double r,
                                      double q, Side side) {
  const double v = sigma * std::sqrt(T);
  const double d1 = (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / v;
  const double d2 = d1 - v;
  const double df = std::exp(-r * T);
  const double dq = std::exp(-q * T);
  return side == Side::Call
             ? S * dq * independent_ncdf(d1) - K * df * independent_ncdf(d2)
             : K * df * independent_ncdf(-d2) - S * dq * independent_ncdf(-d1);
}

// The exact bytes `convention_sweep_json` publishes as `production_conventions`,
// pinned from the deterministic aggregate smoke+tune sweep. That gate compares
// this rendering against the `conventions` it resolves on the same run and fails
// closed on any difference, so pinning the RENDERING (not a second hand-written
// struct literal that could drift the same way the first one did) is what makes
// the production map checkable against the sweep.
constexpr std::string_view kResolvedWinnerJson =
    R"({"input_model":"discrete_dividend_tree__rate__sdiv_yield",)"
    R"("forward_formula":"none","rate_model":"continuous_row_rate",)"
    R"("carry_model":"sdiv_as_yield","dividend_model":"discrete_cash_schedule",)"
    R"("exercise_style":"european_cash_settled_index",)"
    R"("time_decay_method":"analytic_derivative",)"
    R"("day_count":"BUS_252","dte_banding_day_count":"ACT_365F",)"
    R"("price_scale":"per_share","price_sign":"positive",)"
    R"("vol_scale":"decimal_identity","delta_scale":"per_unit","delta_sign":"positive",)"
    R"("gamma_scale":"per_unit","gamma_sign":"positive","theta_basis":"per_day",)"
    R"("theta_sign":"positive","vega_scale":"per_point","vega_sign":"positive",)"
    R"("rho_scale":"per_point","rho_sign":"positive","phi_scale":"per_point",)"
    R"("phi_sign":"positive","volga_source":"volga","volga_scale":"per_point_squared",)"
    R"("volga_sign":"positive","vanna_source":"vanna","vanna_scale":"per_point",)"
    R"("vanna_sign":"positive","delta_decay_basis":"per_day","delta_decay_day_count":"BUS_252",)"
    R"("delta_decay_sign":"positive"})";

TEST(OracleConvention, DiscreteDividendForwardIsAppliedExactly) {
  OracleRow row;
  row.uprc = 123.0;
  row.rate = 0.037;
  row.sdiv = 0.012;
  row.ddiv = 4.25;
  row.years = 1.75;
  ConventionMap map = baseline_convention();
  map.input_model = InputModel::DiscreteDividendPvSdivYield;
  const EnginePricingInputs inputs = mode_a_inputs(row, map);
  const double documented_forward = row.uprc * std::exp(row.rate * row.years) - row.ddiv;
  EXPECT_DOUBLE_EQ(inputs.spot, documented_forward * std::exp(-row.rate * row.years));
  EXPECT_DOUBLE_EQ(inputs.rate, row.rate);
  EXPECT_DOUBLE_EQ(inputs.carry, row.sdiv);
}

// The tree arm prices EXACTLY the engine bundle it claims to — same schedule,
// same exercise routing, dp_dq is the bundle's own phi — pinned bit-for-bit so
// the convention layer can never wrap the lattice in a second model choice.
TEST(OracleConvention, DiscreteDividendTreePricesTheLatticeBundleOnTheSchedule) {
  ConventionMap map = baseline_convention();
  map.input_model = InputModel::DiscreteDividendTree;
  OracleRow row;
  row.underlier = "SYNTH";
  row.side = Side::Put;
  row.strike = 97.5;
  row.uprc = 100.0;
  row.rate = 0.04;
  row.sdiv = 0.003;
  row.ddiv = 1.25;
  row.years = 0.35;
  row.sr_vol = 0.27;
  const std::vector<CashDividend> schedule = {CashDividend{0.10, 0.65}, CashDividend{0.35, 0.60}};
  const RowDividends divs{.schedule = schedule, .refused = false};
  const Result<ModeAPricing> priced = mode_a_price_row(row, map, al_fast_opts(), divs);
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  EXPECT_EQ(priced->style, atx::vol::oracle::ExerciseStyle::American);
  const auto bundle = american_discrete_div_greek_bundle(
      row.uprc, row.strike, row.years, row.sr_vol, row.rate, row.sdiv, row.side, schedule);
  ASSERT_TRUE(bundle.has_value()) << bundle.error().to_string();
  EXPECT_EQ(priced->greeks.price, bundle->price);
  EXPECT_EQ(priced->greeks.delta, bundle->delta);
  EXPECT_EQ(priced->greeks.gamma, bundle->gamma);
  EXPECT_EQ(priced->greeks.vega, bundle->vega);
  EXPECT_EQ(priced->greeks.theta, bundle->theta);
  EXPECT_EQ(priced->greeks.rho, bundle->rho);
  EXPECT_EQ(priced->greeks.vanna, bundle->vanna);
  EXPECT_EQ(priced->greeks.volga, bundle->volga);
  EXPECT_EQ(priced->greeks.charm, bundle->charm);
  EXPECT_EQ(priced->dp_dq, bundle->phi);
  // The stage-1 price tier is the same kernel at one rollback.
  const Result<double> price_only = mode_a_price(row, map, al_fast_opts(), divs);
  ASSERT_TRUE(price_only.has_value());
  EXPECT_EQ(*price_only, bundle->price);
  // A European-routed row with a live schedule takes the lattice's OWN
  // European rollback — the closed form has no schedule slot, and silently
  // dropping the cash would be a different model, not a different leg.
  row.underlier = "SPX";
  ConventionMap routed = map;
  routed.exercise_style = ExerciseStyleRule::EuropeanCashSettledIndex;
  const Result<ModeAPricing> euro = mode_a_price_row(row, routed, al_fast_opts(), divs);
  ASSERT_TRUE(euro.has_value()) << euro.error().to_string();
  EXPECT_EQ(euro->style, atx::vol::oracle::ExerciseStyle::European);
  const auto euro_bundle = american_discrete_div_greek_bundle(
      row.uprc, row.strike, row.years, row.sr_vol, row.rate, row.sdiv, row.side, schedule,
      kDiscreteDivDefaultSteps, atx::vol::ExerciseStyle::European);
  ASSERT_TRUE(euro_bundle.has_value()) << euro_bundle.error().to_string();
  EXPECT_EQ(euro->greeks.price, euro_bundle->price);
  EXPECT_EQ(euro->dp_dq, euro_bundle->phi);
}

// THE ddiv == 0 CONTROL, pinned bit-for-bit: with no cash in (0, T] the tree
// and the continuous-carry model are the SAME model, so a dividend-free row
// must not move at all under the tree arm — movement there is a defect, not a
// result, and it is what licenses reading the dividend rows' price deltas as
// the dividend treatment's effect.
TEST(OracleConvention, DiscreteDividendTreeWithoutDividendsIsTheContinuousEngine) {
  ConventionMap tree = baseline_convention();
  tree.input_model = InputModel::DiscreteDividendTree;
  const ConventionMap &spot = baseline_convention(); // CurrentSpotSdivYield
  OracleRow row;
  row.underlier = "SYNTH";
  row.side = Side::Call;
  row.strike = 102.0;
  row.uprc = 100.0;
  row.rate = 0.04;
  row.sdiv = 0.011;
  row.ddiv = 0.0;
  row.years = 0.35;
  row.sr_vol = 0.27;
  const Result<ModeAPricing> tree_priced = mode_a_price_row(row, tree, al_fast_opts());
  const Result<ModeAPricing> spot_priced = mode_a_price_row(row, spot, al_fast_opts());
  ASSERT_TRUE(tree_priced.has_value()) << tree_priced.error().to_string();
  ASSERT_TRUE(spot_priced.has_value());
  EXPECT_EQ(tree_priced->greeks, spot_priced->greeks);
  EXPECT_EQ(tree_priced->dp_dq, spot_priced->dp_dq);
  const Result<double> tree_price = mode_a_price(row, tree, al_fast_opts());
  const Result<double> spot_price = mode_a_price(row, spot, al_fast_opts());
  ASSERT_TRUE(tree_price.has_value());
  ASSERT_TRUE(spot_price.has_value());
  EXPECT_EQ(*tree_price, *spot_price);
}

// FAIL CLOSED: a row the reconstruction refused, or a ddiv-bearing row with no
// covering schedule (including every call through the no-pre-pass overload),
// is an Err — never a silent fallback onto a model the receipt did not name.
TEST(OracleConvention, DiscreteDividendTreeFailsClosedOnRefusedOrMissingSchedules) {
  ConventionMap map = baseline_convention();
  map.input_model = InputModel::DiscreteDividendTree;
  OracleRow row;
  row.underlier = "SYNTH";
  row.side = Side::Call;
  row.strike = 100.0;
  row.uprc = 100.0;
  row.rate = 0.04;
  row.sdiv = 0.002;
  row.ddiv = 1.0;
  row.years = 0.35;
  row.sr_vol = 0.27;
  // A refused snapshot refuses the row outright — even one whose ddiv is 0.
  const RowDividends refused{.schedule = {}, .refused = true};
  EXPECT_FALSE(mode_a_price_row(row, map, al_fast_opts(), refused).has_value());
  EXPECT_FALSE(mode_a_price(row, map, al_fast_opts(), refused).has_value());
  // ddiv > 0 with no covering schedule: the no-pre-pass overload cannot invent
  // a chain schedule from one row, so it must refuse rather than guess.
  EXPECT_FALSE(mode_a_price_row(row, map, al_fast_opts()).has_value());
  EXPECT_FALSE(mode_a_price(row, map, al_fast_opts()).has_value());
  // A schedule whose only events sit past this row's expiry covers nothing the
  // row's own ddiv claims, and is refused for the same reason.
  const std::vector<CashDividend> beyond = {CashDividend{0.60, 1.0}};
  EXPECT_FALSE(
      mode_a_price_row(row, map, al_fast_opts(), RowDividends{.schedule = beyond, .refused = false})
          .has_value());
  // ddiv == 0 needs no schedule at all: the continuous legs are the tree's own
  // no-dividend limit and the row prices.
  row.ddiv = 0.0;
  EXPECT_TRUE(mode_a_price_row(row, map, al_fast_opts()).has_value());
  EXPECT_TRUE(mode_a_price(row, map, al_fast_opts()).has_value());
}

TEST(OracleConvention, EuropeanLegMatchesTheIndependentRungWithNoIntrinsicFloor) {
  constexpr double S = 100.0;
  constexpr double T = 0.75;
  constexpr double sigma = 0.24;
  constexpr double r = 0.043;
  constexpr double q = 0.012;
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double strike : {60.0, 100.0, 140.0}) {
      double dp_dq = std::numeric_limits<double>::quiet_NaN();
      const Result<AmericanGreeks> leg = european_greeks(S, strike, T, sigma, r, q, side, &dp_dq);
      ASSERT_TRUE(leg.has_value());
      EXPECT_NEAR(leg->price, independent_euro(S, strike, T, sigma, r, q, side), 1e-9);
      EXPECT_TRUE(std::isfinite(dp_dq));
    }
  }
  // The case the axis exists for: a deep-ITM European put is entitled to sit
  // BELOW intrinsic — exactly what the intrinsic-flooring null-cache path would
  // have destroyed — and the leg must reproduce it, not floor it.
  const Result<AmericanGreeks> deep = european_greeks(S, 200.0, T, sigma, r, q, Side::Put);
  ASSERT_TRUE(deep.has_value());
  EXPECT_LT(deep->price, 200.0 - S);
  EXPECT_NEAR(deep->price, independent_euro(S, 200.0, T, sigma, r, q, Side::Put), 1e-9);
  // american_greeks_al's admission contract, verbatim: which leg a row takes
  // must not change whether the row is admitted.
  EXPECT_FALSE(european_greeks(S, 200.0, 0.0, sigma, r, q, Side::Put).has_value());
}

TEST(OracleConvention, ExerciseStyleRulesRouteOnlyTheirNamedRoots) {
  constexpr auto kAmerican = ExerciseStyleRule::AmericanAll;
  constexpr auto kIndex = ExerciseStyleRule::EuropeanCashSettledIndex;
  constexpr auto kPlus = ExerciseStyleRule::EuropeanCashSettledIndexPlusEmpirical;
  // OEX is the standing counterexample to "index => European" — cash-settled
  // but AMERICAN by contract; SPY and SYNTH are the unrouted equity controls.
  for (const std::string_view root : {"SPY", "OEX", "SYNTH"}) {
    EXPECT_FALSE(routes_european(root, kAmerican)) << root;
    EXPECT_FALSE(routes_european(root, kIndex)) << root;
    EXPECT_FALSE(routes_european(root, kPlus)) << root;
  }
  // The contract-fact roots: routed by both European rules, never the baseline.
  // MGTN is one of them since the Cboe Magnificent 10 contract specification
  // was located (kEuropeanIndexRoots carries the citation); the empirical table
  // is empty, so `_plus_empirical` routes exactly the contract-fact set today.
  for (const std::string_view root : {"SPX", "XSP", "MGTN"}) {
    EXPECT_FALSE(routes_european(root, kAmerican)) << root;
    EXPECT_TRUE(routes_european(root, kIndex)) << root;
    EXPECT_TRUE(routes_european(root, kPlus)) << root;
  }
}

TEST(OracleConvention, IngestedExerciseStyleOutranksTheRootListRule) {
  ConventionMap map = baseline_convention();
  map.exercise_style = ExerciseStyleRule::EuropeanCashSettledIndexPlusEmpirical;
  OracleRow row;
  row.underlier = "SPX";
  // `oracle::` throughout: atx::vol already carries an unrelated ExerciseStyle
  // and this file imports both namespaces.
  // Unknown (today, every row): the map's rule answers.
  EXPECT_EQ(exercise_style_for(row, map), oracle::ExerciseStyle::European);
  // An ingested style is fact and outranks the rule — in both directions.
  row.ingested_exercise_style = oracle::ExerciseStyle::American;
  EXPECT_EQ(exercise_style_for(row, map), oracle::ExerciseStyle::American);
  row.underlier = "SPY";
  row.ingested_exercise_style = oracle::ExerciseStyle::European;
  EXPECT_EQ(exercise_style_for(row, map), oracle::ExerciseStyle::European);
}

// The time-decay axis, fact 1: what the secant IS. SpiderRock measures theta
// "by calculating the difference between the current option price and the option
// price calculated with volatility time decreased by 1/252 years", and deDecay
// is the same finite difference taken on delta. The independent rung here is a
// second full `mode_a_price_row` on a row whose `years` has been moved by hand,
// which reproduces the bumped leg without borrowing `bumped_leg`'s arithmetic.
// The baseline input model derives no engine input from `years`, so moving the
// row's maturity and moving the derived `EnginePricingInputs::years` are the
// same bump — which is exactly what makes this rung independent AND exact.
TEST(OracleConvention, SecantDecayIsTheOneDayDifferenceOfPriceAndDelta) {
  EXPECT_DOUBLE_EQ(kTimeDecayStepYears, 1.0 / 252.0);
  const ConventionMap analytic = baseline_convention();
  ConventionMap secant = analytic;
  secant.time_decay_method = TimeDecayMethod::Secant252;

  const OracleRow row = make_row(100.0, Side::Call, secant);
  const Result<ModeAPricing> priced = mode_a_price_row(row, secant, al_fast_opts());
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();

  OracleRow bumped_row = row;
  bumped_row.years -= kTimeDecayStepYears;
  ASSERT_GT(bumped_row.years, 0.0);
  const Result<ModeAPricing> bumped = mode_a_price_row(bumped_row, analytic, al_fast_opts());
  ASSERT_TRUE(bumped.has_value()) << bumped.error().to_string();

  EXPECT_DOUBLE_EQ(priced->theta_secant, priced->greeks.price - bumped->greeks.price);
  EXPECT_DOUBLE_EQ(priced->delta_decay_secant, priced->greeks.delta - bumped->greeks.delta);
  // POSITIVE = decay, SpiderRock's own sign: what a long at-the-money call loses
  // when one business day passes.
  EXPECT_GT(priced->theta_secant, 0.0);

  // The analytic arm asks for no bumped valuation, so it publishes no secant —
  // non-finite rather than a 0.0 that would read as a measured "no decay".
  const Result<ModeAPricing> unbumped = mode_a_price_row(row, analytic, al_fast_opts());
  ASSERT_TRUE(unbumped.has_value()) << unbumped.error().to_string();
  EXPECT_FALSE(std::isfinite(unbumped->theta_secant));
  EXPECT_FALSE(std::isfinite(unbumped->delta_decay_secant));
  // THE INVARIANT THE AXIS RESTS ON: the decay method changes how two Greeks are
  // reported and never how a price is formed. If this ever fails, a price move
  // attributed to the sweep is a defect, not an improvement.
  EXPECT_DOUBLE_EQ(priced->greeks.price, unbumped->greeks.price);
  EXPECT_DOUBLE_EQ(priced->greeks.delta, unbumped->greeks.delta);
}

// The time-decay axis, fact 2, and the pin `to_oracle_units` names by test id:
// `theta_scale` and `delta_decay_scale` are INERT under `Secant252`. The secant
// is already SpiderRock's one-day quantity, so applying a per-day scale on top
// of it divides by a day count a SECOND time — a silent order-of-magnitude bug
// that no metric would identify as a unit error. Reintroducing either
// multiplier fails here.
TEST(OracleConvention, SecantDecayIgnoresTheThetaAndDecayScales) {
  ConventionMap secant = baseline_convention();
  secant.time_decay_method = TimeDecayMethod::Secant252;
  const OracleRow row = make_row(100.0, Side::Call, secant);
  const Result<ModeAPricing> priced = mode_a_price_row(row, secant, al_fast_opts());
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_TRUE(std::isfinite(priced->theta_secant));
  ASSERT_TRUE(std::isfinite(priced->delta_decay_secant));

  // RAW: exactly the secant, with no multiplier of any kind applied.
  const OracleUnitGreeks units = to_oracle_units(*priced, secant);
  EXPECT_DOUBLE_EQ(units.th, priced->theta_secant);
  EXPECT_DOUBLE_EQ(units.de_decay, priced->delta_decay_secant);

  // Move BOTH per-day scales far off identity — including a sign flip — and the
  // two published decay Greeks must not budge by a single ulp.
  ConventionMap scaled = secant;
  scaled.theta_scale = 1.0 / 252.0;
  scaled.theta_days_per_year = 252.0;
  scaled.delta_decay_scale = -100.0;
  const OracleUnitGreeks scaled_units = to_oracle_units(*priced, scaled);
  EXPECT_DOUBLE_EQ(scaled_units.th, units.th);
  EXPECT_DOUBLE_EQ(scaled_units.de_decay, units.de_decay);

  // The seven axes the decay method does not touch DO still scale, so this
  // cannot pass by the conversion having gone inert altogether.
  ConventionMap other = secant;
  other.delta_scale = 100.0;
  EXPECT_DOUBLE_EQ(to_oracle_units(*priced, other).de, 100.0 * units.de);

  // And the analytic arm still scales its own theta and charm, so inertness is a
  // property of THIS method rather than of the two fields.
  ConventionMap analytic = baseline_convention();
  analytic.theta_scale = 1.0 / 252.0;
  analytic.theta_days_per_year = 252.0;
  analytic.delta_decay_scale = 1.0 / 252.0;
  const OracleUnitGreeks analytic_units = to_oracle_units(*priced, analytic);
  EXPECT_DOUBLE_EQ(analytic_units.th, priced->greeks.theta * (1.0 / 252.0));
  EXPECT_DOUBLE_EQ(analytic_units.de_decay, priced->greeks.charm * (1.0 / 252.0));
  // A tangent and a secant are different QUANTITIES, which is the whole reason
  // this is a convention axis and not another entry in the scale grid.
  EXPECT_NE(analytic_units.th, units.th);
  EXPECT_NE(analytic_units.de_decay, units.de_decay);
}

// The time-decay axis, fact 3: the EXPIRATION-DAY boundary, where the bumped
// maturity lands at or below zero and both pricing legs refuse. SpiderRock's own
// expiration-day convention (American -> European AND rate = sdiv = carry = 0)
// collapses at zero remaining time to the payoff itself, so the bumped leg is
// the intrinsic payoff and its slope — no epsilon knob, and the row keeps every
// other Greek instead of being dropped.
TEST(OracleConvention, ExpirationDayDecayLegIsTheIntrinsicPayoff) {
  ConventionMap secant = baseline_convention();
  secant.time_decay_method = TimeDecayMethod::Secant252;
  OracleRow row;
  row.underlier = "SYNTH";
  row.side = Side::Call;
  row.uprc = 100.0;
  row.rate = 0.04;
  row.sr_vol = 0.25;
  row.years = 1.0 / 365.0;
  ASSERT_LE(row.years, kTimeDecayStepYears);

  // In the money: the payoff is 5 and its slope is 1, so the secant is the whole
  // remaining premium over intrinsic.
  row.strike = 95.0;
  const Result<ModeAPricing> itm_call = mode_a_price_row(row, secant, al_fast_opts());
  ASSERT_TRUE(itm_call.has_value()) << itm_call.error().to_string();
  EXPECT_DOUBLE_EQ(itm_call->theta_secant, itm_call->greeks.price - 5.0);
  EXPECT_DOUBLE_EQ(itm_call->delta_decay_secant, itm_call->greeks.delta - 1.0);
  EXPECT_GT(itm_call->theta_secant, 0.0);
  // The row is kept whole: the expiration-day rule costs it nothing else.
  EXPECT_TRUE(std::isfinite(itm_call->greeks.vega));
  EXPECT_TRUE(std::isfinite(itm_call->dp_dq));

  // Out of the money: payoff and slope are both zero, so the secant is the whole
  // price and the delta decay is the whole delta.
  row.strike = 105.0;
  const Result<ModeAPricing> otm_call = mode_a_price_row(row, secant, al_fast_opts());
  ASSERT_TRUE(otm_call.has_value()) << otm_call.error().to_string();
  EXPECT_DOUBLE_EQ(otm_call->theta_secant, otm_call->greeks.price);
  EXPECT_DOUBLE_EQ(otm_call->delta_decay_secant, otm_call->greeks.delta);

  // Exactly at the money: the payoff's kink resolves to the OUT-of-the-money
  // side, so the bumped leg claims delta 0 and price 0 rather than a one-sided
  // derivative the payoff does not have.
  row.strike = 100.0;
  const Result<ModeAPricing> atm_call = mode_a_price_row(row, secant, al_fast_opts());
  ASSERT_TRUE(atm_call.has_value()) << atm_call.error().to_string();
  EXPECT_DOUBLE_EQ(atm_call->theta_secant, atm_call->greeks.price);
  EXPECT_DOUBLE_EQ(atm_call->delta_decay_secant, atm_call->greeks.delta);

  // The put side, where the payoff slope is -1 rather than +1.
  row.side = Side::Put;
  row.strike = 105.0;
  const Result<ModeAPricing> itm_put = mode_a_price_row(row, secant, al_fast_opts());
  ASSERT_TRUE(itm_put.has_value()) << itm_put.error().to_string();
  EXPECT_DOUBLE_EQ(itm_put->theta_secant, itm_put->greeks.price - 5.0);
  EXPECT_DOUBLE_EQ(itm_put->delta_decay_secant, itm_put->greeks.delta + 1.0);
  row.strike = 100.0;
  const Result<ModeAPricing> atm_put = mode_a_price_row(row, secant, al_fast_opts());
  ASSERT_TRUE(atm_put.has_value()) << atm_put.error().to_string();
  EXPECT_DOUBLE_EQ(atm_put->theta_secant, atm_put->greeks.price);
  EXPECT_DOUBLE_EQ(atm_put->delta_decay_secant, atm_put->greeks.delta);
}

TEST(OracleConvention, ProductionMapIsTheResolvedHardCut) {
  const ConventionMap &map = winning_convention();
  EXPECT_EQ(map.input_model, InputModel::DiscreteDividendTree);
  EXPECT_EQ(map.exercise_style, ExerciseStyleRule::EuropeanCashSettledIndex);
  EXPECT_DOUBLE_EQ(map.price_scale, 1.0);
  // Never searched: the DTE-banding day count, not a unit the sweep may pick.
  EXPECT_DOUBLE_EQ(map.days_per_year, 365.0);
  EXPECT_DOUBLE_EQ(map.theta_days_per_year, 252.0);
  EXPECT_DOUBLE_EQ(map.delta_scale, 1.0);
  EXPECT_DOUBLE_EQ(map.gamma_scale, 1.0);
  EXPECT_DOUBLE_EQ(map.theta_scale, 1.0 / 252.0);
  EXPECT_DOUBLE_EQ(map.vega_scale, 0.01);
  EXPECT_DOUBLE_EQ(map.rho_scale, 0.01);
  EXPECT_DOUBLE_EQ(map.phi_scale, 0.01);
  EXPECT_EQ(map.volga_source, GreekSource::Volga);
  EXPECT_DOUBLE_EQ(map.volga_scale, 0.0001);
  EXPECT_EQ(map.vanna_source, GreekSource::Vanna);
  EXPECT_DOUBLE_EQ(map.vanna_scale, 0.01);
  EXPECT_DOUBLE_EQ(map.delta_decay_scale, 1.0 / 252.0);

  // The rendering the aggregate gate diffs against its own resolved winner.
  EXPECT_EQ(convention_map_json(map), kResolvedWinnerJson);

  // Stronger than any literal: the production map must be a FIXED POINT of the
  // sweep. A cohort whose oracle columns were produced BY winning_convention()
  // has to resolve back to it, so a map carrying a unit outside the candidate
  // grid, an input model the search cannot reach, or a theta_days_per_year
  // inconsistent with theta_scale fails here, in seconds on synthetic rows,
  // instead of only at the aggregate gate.
  const std::vector<OracleRow> smoke = production_rows(90.0, 110.0);
  const std::vector<OracleRow> tune = production_rows(95.0, 105.0);
  const auto resolved = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(resolved.has_value()) << resolved.error().to_string();
  EXPECT_EQ(convention_map_json(resolved->winner), convention_map_json(map));
}

TEST(OracleConvention, BestScaleRanksOnTheSelectionObjective) {
  std::vector<ScaleCandidate> candidates = {
      ScaleCandidate{GreekSource::Delta, 1.0, {}},
      ScaleCandidate{GreekSource::Delta, 0.01, {}},
  };
  // Candidate 0 wins the REPORTED floor and loses the SELECTION loss. The two
  // are deliberately different functions over the same rows, and selection is
  // what picks the production scale.
  candidates[0].error.report.relative(1.0, 1.0);
  candidates[0].error.selection.symmetric_relative(2.0, 1.0);
  candidates[1].error.report.relative(5.0, 1.0);
  candidates[1].error.selection.symmetric_relative(1.05, 1.0);
  EXPECT_EQ(best_scale(candidates), 1u);
}

// The regression test for the whole Stage 3 finding. The asymmetric objective
// divides by max(|oracle|, floor), so on a row whose oracle sits far below the
// floor the denominator pins while the numerator still grows with
// |model * scale| — a systematic gradient toward the smallest candidate scale,
// no matter how wrong it is. The symmetric objective is bounded per row, so the
// row that carries the true unit decides instead.
TEST(OracleConvention, SymmetricObjectiveHasNoSmallestScaleGradient) {
  constexpr double kSubFloorOracle = kSelectionAbsFloor / 100.0;
  constexpr double kRawModel = 1.0;
  std::vector<ScaleCandidate> symmetric = {
      ScaleCandidate{GreekSource::Delta, 1.0, {}},
      ScaleCandidate{GreekSource::Delta, 0.01, {}},
  };
  std::vector<ScaleCandidate> asymmetric = symmetric;
  for (std::size_t index = 0; index < symmetric.size(); ++index) {
    const double model = kRawModel * symmetric[index].scale;
    // One near-zero oracle row, and one row whose oracle says the true scale
    // is 1.0.
    symmetric[index].error.selection.symmetric_relative(model, kSubFloorOracle);
    symmetric[index].error.selection.symmetric_relative(model, kRawModel);
    asymmetric[index].error.selection.relative(model, kSubFloorOracle);
    asymmetric[index].error.selection.relative(model, kRawModel);
  }
  EXPECT_EQ(best_scale(asymmetric), 1u);
  EXPECT_EQ(best_scale(symmetric), 0u);
}

TEST(OracleConvention, FinalistRankPrefersNoGreekRegressionOverLowerPriceMae) {
  // A finalist that regresses on a Greek loses to one that does not, even when
  // its price MAE is an order of magnitude better: the keys are lexicographic,
  // never weighted, because ticks and dimensionless ratios have no exchange
  // rate.
  const FinalistRank clean{.regresses_any_greek = false, .tune_price_mae = 10.0,
                           .candidate_id = "b"};
  const FinalistRank regressing{.regresses_any_greek = true, .tune_price_mae = 1.0,
                                .candidate_id = "a"};
  EXPECT_TRUE(less_finalist(clean, regressing));
  EXPECT_FALSE(less_finalist(regressing, clean));
  // Both regressing: the rank degenerates to the lower tune-sample price MAE.
  EXPECT_TRUE(less_finalist(
      FinalistRank{.regresses_any_greek = true, .tune_price_mae = 1.0, .candidate_id = "b"},
      FinalistRank{.regresses_any_greek = true, .tune_price_mae = 2.0, .candidate_id = "a"}));
  // Equal on both keys: the stable candidate identity, never source order.
  EXPECT_TRUE(less_finalist(
      FinalistRank{.regresses_any_greek = false, .tune_price_mae = 1.0, .candidate_id = "a"},
      FinalistRank{.regresses_any_greek = false, .tune_price_mae = 1.0, .candidate_id = "b"}));
  EXPECT_FALSE(less_finalist(
      FinalistRank{.regresses_any_greek = false, .tune_price_mae = 1.0, .candidate_id = "b"},
      FinalistRank{.regresses_any_greek = false, .tune_price_mae = 1.0, .candidate_id = "a"}));
}

// The trap the third axis sprang, pinned so it cannot spring again on a fourth.
// '|' is 0x7C and '_' is 0x5F, so the separator sorts ABOVE an id character, and
// one exercise-style id is a strict prefix of another. A FLAT string comparison
// therefore reverses that pair the moment a field is appended after the style —
// changing which arm the sweep resolves on a tie the evidence never decided.
TEST(OracleConvention, CandidateIdentityOrdersFieldByFieldNotAsAFlatString) {
  const std::string index = "m|european_cash_settled_index|analytic_derivative";
  const std::string plus = "m|european_cash_settled_index_plus_empirical|analytic_derivative";
  // The flat order, asserted so this test names the thing it forbids rather than
  // merely asserting the thing it wants.
  EXPECT_LT(plus, index);

  const auto rank = [](std::string_view id) {
    return FinalistRank{.regresses_any_greek = false, .tune_price_mae = 1.0, .candidate_id = id};
  };
  // Field order: the style field alone decides, and the prefix sorts first.
  EXPECT_TRUE(less_finalist(rank(index), rank(plus)));
  EXPECT_FALSE(less_finalist(rank(plus), rank(index)));
  // The FIRST differing field decides, never a later one.
  EXPECT_TRUE(less_finalist(rank("a|z|z"), rank("b|a|a")));
  EXPECT_FALSE(less_finalist(rank("b|a|a"), rank("a|z|z")));
  // Fewer fields sorts first, and equal ids are not less than one another — the
  // sort would not be a strict weak ordering otherwise.
  EXPECT_TRUE(less_finalist(rank("a|b"), rank("a|b|c")));
  EXPECT_FALSE(less_finalist(rank("a|b|c"), rank("a|b")));
  EXPECT_FALSE(less_finalist(rank(index), rank(index)));
}

TEST(OracleConvention, BestScaleTieBreaksOnSourceThenNumericScale) {
  std::vector<ScaleCandidate> candidates = {
      ScaleCandidate{GreekSource::Gamma, -1.0, {}},
      ScaleCandidate{GreekSource::Delta, -0.01, {}},
      ScaleCandidate{GreekSource::Delta, -1.0, {}},
  };
  for (ScaleCandidate &candidate : candidates) {
    candidate.error.selection.symmetric_relative(1.0, 1.0);
  }
  // "delta" < "gamma" on the source ID; within one source the SIGNED scale
  // orders numerically, so -1.0 beats -0.01. A formatted-string comparison
  // would order "-0.010000" first and pick the wrong candidate.
  EXPECT_EQ(best_scale(candidates), 2u);
}

TEST(OracleConvention, BestScaleWithoutSelectionEvidenceUsesCandidateIdentity) {
  const std::vector<ScaleCandidate> candidates = {
      ScaleCandidate{GreekSource::Vanna, 1.0, {}},
      ScaleCandidate{GreekSource::Volga, -1.0, {}},
      ScaleCandidate{GreekSource::Vanna, -1.0, {}},
  };
  EXPECT_EQ(best_scale(candidates), 2u);
}

TEST(OracleConvention, CompleteMapNamesEveryGreekSignAndScale) {
  const std::string json = convention_map_json(baseline_convention());
  const char *const tokens[] = {"input_model",
                                "forward_formula",
                                "rate_model",
                                "carry_model",
                                "dividend_model",
                                "exercise_style",
                                "time_decay_method",
                                "day_count",
                                "dte_banding_day_count",
                                "price_scale",
                                "price_sign",
                                "vol_scale",
                                "delta_scale",
                                "delta_sign",
                                "gamma_scale",
                                "gamma_sign",
                                "theta_basis",
                                "theta_sign",
                                "vega_scale",
                                "vega_sign",
                                "rho_scale",
                                "rho_sign",
                                "phi_scale",
                                "phi_sign",
                                "volga_source",
                                "volga_scale",
                                "volga_sign",
                                "vanna_source",
                                "vanna_scale",
                                "vanna_sign",
                                "delta_decay_basis",
                                "delta_decay_day_count",
                                "delta_decay_sign"};
  for (const char *token : tokens) {
    EXPECT_NE(json.find(std::string{"\""} + token + "\""), std::string::npos) << token;
  }
  // A substring sweep cannot see an EXTRA key, and five gate layers pin this map
  // at exactly one size: a key added to the C++ emission alone fails the gate
  // only after a 12-minute sweep. Every value is a closed enum token with no
  // ':' in it, so counting colons counts keys.
  EXPECT_EQ(static_cast<std::size_t>(std::count(json.begin(), json.end(), ':')),
            std::size(tokens));
  EXPECT_EQ(std::size(tokens), 33u);
}

TEST(OracleConvention, ThetaDayCountNeverRebucketsDteBands) {
  ConventionMap map = baseline_convention();
  map.theta_days_per_year = 360.0;
  map.theta_scale = 1.0 / 360.0;
  EXPECT_NE(convention_map_json(map).find("\"day_count\":\"ACT_360\""), std::string::npos);
  // The banding day count is a separate convention and stays pinned.
  EXPECT_DOUBLE_EQ(dte_days(1.0, map), 365.0);

  const std::vector<OracleRow> smoke = distinct_arm_rows(90.0, 110.0);
  const std::vector<OracleRow> tune = distinct_arm_rows(95.0, 105.0);
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_DOUBLE_EQ(result->winner.days_per_year, baseline_convention().days_per_year);
  EXPECT_DOUBLE_EQ(result->winner.days_per_year, 365.0);
}

TEST(OracleConvention, SweepIsClosedDeterministicAndCoversElevenMetrics) {
  const std::vector<OracleRow> smoke = {make_row(90.0, Side::Call), make_row(110.0, Side::Put)};
  const std::vector<OracleRow> tune = {make_row(95.0, Side::Call), make_row(105.0, Side::Put)};
  const auto first = run_convention_sweep(smoke, tune);
  const auto second = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(second.has_value()) << second.error().to_string();
  ASSERT_EQ(first->metrics.size(), 11u);
  ASSERT_EQ(first->baseline_metrics.size(), 11u);
  ASSERT_EQ(first->symmetric_metrics.size(), 11u);
  ASSERT_EQ(first->baseline_symmetric_metrics.size(), 11u);
  for (std::size_t index = 0; index < first->metrics.size(); ++index) {
    EXPECT_EQ(first->symmetric_metrics[index].metric_id, first->metrics[index].metric_id);
    EXPECT_EQ(first->baseline_symmetric_metrics[index].metric_id,
              first->baseline_metrics[index].metric_id);
  }
  // The CROSS PRODUCT of the three searched axes: nine input models x three
  // exercise-style rules x two time-decay methods. The tune sample is paid for
  // by the survivors of the smoke cut alone — two input models times the FULL
  // tied fan (3 x 2), because the stage-1 price ranking can separate neither the
  // exercise axis (single unrouted smoke underlier) nor the decay axis (it never
  // touches a price at all). This exact number is mirrored by the candidate-id
  // set $expectedCandidateIds pins in scripts/oracle-targeted-gate.ps1; the two
  // pins MUST move together or the smoke_tune gate fails on a registry mismatch
  // after the full sweep has already run.
  ASSERT_EQ(first->candidate_prices.size(), 54u);
  EXPECT_EQ(std::count_if(first->candidate_prices.begin(), first->candidate_prices.end(),
                          [](const CandidatePriceMetric &candidate) {
                            return candidate.tune_sample_count > 0;
                          }),
            12);
  // Every published id names all three axes, in the order the gate reproduces
  // them. A two-part id would still be unique and would still pass the count
  // above, and the gate's exact-set pin is what would then fail.
  for (const CandidatePriceMetric &candidate : first->candidate_prices) {
    EXPECT_EQ(std::count(candidate.candidate_id.begin(), candidate.candidate_id.end(), '|'), 2)
        << candidate.candidate_id;
  }
  EXPECT_NE(std::find_if(first->candidate_prices.begin(), first->candidate_prices.end(),
                         [](const CandidatePriceMetric &candidate) {
                           return candidate.candidate_id ==
                                  "uprc_spot__rate__sdiv_yield|american_all|secant_252";
                         }),
            first->candidate_prices.end());
  // The discrete-dividend ENGINE arm is on the grid: the 8 -> 9 input-model
  // widening this pin exists to catch would otherwise only fail at the gate's
  // id-set check, a full sweep later.
  EXPECT_NE(std::find_if(first->candidate_prices.begin(), first->candidate_prices.end(),
                         [](const CandidatePriceMetric &candidate) {
                           return candidate.candidate_id ==
                                  "discrete_dividend_tree__rate__sdiv_yield|american_all|"
                                  "analytic_derivative";
                         }),
            first->candidate_prices.end());
  EXPECT_EQ(convention_map_json(first->winner), convention_map_json(second->winner));
  const std::string json = convention_sweep_json(*first, "0123456789abcdef");
  EXPECT_NE(json.find("\"cohorts\":[\"smoke\",\"tune\"]"), std::string::npos);
  EXPECT_NE(json.find("\"oracle_suspect_candidates\":[]"), std::string::npos);
  EXPECT_NE(json.find("\"selection_count\":"), std::string::npos);
  // Both floor arrays are published: the standard-relative one stays comparable
  // to the charter target, the symmetric one is what the gate and the ratchet
  // baseline are stated against. A key present in the C++ emission alone fails
  // five gate layers closed after a 12-minute sweep, so pin all three here.
  EXPECT_NE(json.find("\"symmetric_metrics\":"), std::string::npos);
  EXPECT_NE(json.find("\"baseline_symmetric_metrics\":"), std::string::npos);
  EXPECT_NE(json.find("\"symmetric_metric_deltas\":"), std::string::npos);
  // Always present, empty when nothing regressed: five validator layers require
  // the key, and an absent one fails them closed after a 12-minute sweep.
  EXPECT_NE(json.find("\"accepted_regressions\":"), std::string::npos);
  // The schedule pre-pass publishes its refusal ledger as RUN-LEVEL AGGREGATES
  // — counts only, never a group key: the reconstruction groups are
  // (date, bucket_et, underlier) snapshots, which are cohort membership.
  EXPECT_NE(json.find("\"dividend_reconstruction\":{\"rows_seen\":"), std::string::npos);
  EXPECT_NE(json.find("\"groups_refused\":"), std::string::npos);
  EXPECT_NE(json.find("not_evaluated_no_nbbo_gate"), std::string::npos);
  EXPECT_EQ(json.find("holdout"), std::string::npos);
}

// The bounded no-regression rule, pinned on values small enough to read. A
// convention fit over eleven targets sharing one map has no strictly-dominating
// point in the candidate grid, so a metric is allowed to lose ground while it
// stays within kRegressionBoundMultiplier of its baseline — and every such loss
// is PUBLISHED. Beyond the bound nothing is emitted: the gate layers fail closed
// there, and an entry would read as an endorsement.
TEST(OracleConvention, AcceptedRegressionsPublishWithinBoundAndOmitBeyondIt) {
  const auto metric = [](std::string id, double value) {
    return FloorMetric{.metric_id = std::move(id), .value = value, .count = 4,
                       .selection_count = 4, .unit = "relative"};
  };
  const std::vector<FloorMetric> baseline = {
      metric("mode_a_delta_rel", 1.0), metric("mode_a_gamma_rel", 1.0),
      metric("mode_a_theta_rel", 1.0), metric("mode_a_vega_rel", 1.0),
      metric("mode_a_rho_rel", 0.0)};
  const std::vector<FloorMetric> candidate = {
      // Improved, equal, within bound, beyond bound, and off a zero baseline.
      metric("mode_a_delta_rel", 0.5),   metric("mode_a_gamma_rel", 1.0),
      metric("mode_a_theta_rel", 1.005), metric("mode_a_vega_rel", 1.02),
      metric("mode_a_rho_rel", 0.25)};
  const std::vector<AcceptedRegression> published = accepted_regressions(candidate, baseline);
  ASSERT_EQ(published.size(), 1u);
  EXPECT_EQ(published[0].metric_id, "mode_a_theta_rel");
  EXPECT_DOUBLE_EQ(published[0].candidate, 1.005);
  EXPECT_DOUBLE_EQ(published[0].baseline, 1.0);
  // A FRACTION of baseline, never a percentage: 0.5% of baseline is 0.005.
  EXPECT_NEAR(published[0].pct_of_baseline, 0.005, 1.0e-12);

  // Exactly ON the bound is inside it, so the rule has no unreachable sliver.
  const std::vector<FloorMetric> on_bound = {metric("mode_a_delta_rel", 1.0 * 1.01)};
  const std::vector<FloorMetric> one_baseline = {metric("mode_a_delta_rel", 1.0)};
  ASSERT_EQ(accepted_regressions(on_bound, one_baseline).size(), 1u);
  // A baseline of zero divides by nothing: moving off it is beyond the bound.
  EXPECT_TRUE(accepted_regressions(baseline, baseline).empty());
}

TEST(OracleConvention, CandidateAndBaselineFloorsShareOneRowPopulation) {
  const std::vector<OracleRow> smoke = distinct_arm_rows(90.0, 110.0);
  const std::vector<OracleRow> tune = distinct_arm_rows(95.0, 105.0);
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  // Both arms must actually run: a baseline arm that silently degenerates to
  // the winner arm would satisfy the parity check for free.
  ASSERT_NE(result->winner.input_model, baseline_convention().input_model);
  ASSERT_EQ(result->metrics.size(), result->baseline_metrics.size());
  ASSERT_EQ(result->symmetric_metrics.size(), result->metrics.size());
  ASSERT_EQ(result->baseline_symmetric_metrics.size(), result->metrics.size());
  for (std::size_t index = 0; index < result->metrics.size(); ++index) {
    const FloorMetric &candidate = result->metrics[index];
    const FloorMetric &baseline = result->baseline_metrics[index];
    EXPECT_EQ(candidate.metric_id, baseline.metric_id);
    EXPECT_EQ(candidate.count, baseline.count) << candidate.metric_id;
    EXPECT_EQ(candidate.selection_count, baseline.selection_count) << candidate.metric_id;
    // Selection now runs on the FULL reported population: the symmetric
    // objective is well-conditioned on every row, so nothing is excluded.
    EXPECT_EQ(candidate.selection_count, candidate.count) << candidate.metric_id;
    EXPECT_GT(candidate.count, 0) << candidate.metric_id;
    // The symmetric array is a second OBJECTIVE over the same rows, never a
    // second population: the gate compares it arm-to-arm and the deltas would
    // otherwise compare two different samples.
    const FloorMetric &symmetric = result->symmetric_metrics[index];
    const FloorMetric &baseline_symmetric = result->baseline_symmetric_metrics[index];
    EXPECT_EQ(symmetric.metric_id, candidate.metric_id);
    EXPECT_EQ(baseline_symmetric.metric_id, candidate.metric_id);
    EXPECT_EQ(symmetric.count, candidate.count) << candidate.metric_id;
    EXPECT_EQ(symmetric.selection_count, candidate.selection_count) << candidate.metric_id;
    EXPECT_EQ(baseline_symmetric.count, baseline.count) << candidate.metric_id;
    EXPECT_EQ(baseline_symmetric.selection_count, baseline.selection_count) << candidate.metric_id;
  }
}

// The ENTIRE reason two floor arrays are published, pinned on data small enough
// to reason about. On one row the oracle evidences a 100x delta unit; on another
// the oracle delta sits far below kSelectionAbsFloor. The standard-relative
// floor pins its denominator on that near-zero row while its numerator keeps
// growing with the multiplier, so it ranks the baseline scale better; the
// symmetric floor is bounded there, so it ranks the evidenced scale better. Same
// rows, opposite direction — which is why the no-regression gate must be stated
// against the same objective the selector minimises, and why a future reader
// must not unify the two arrays.
TEST(OracleConvention, StandardAndSymmetricFloorsDisagreeInDirection) {
  std::vector<OracleRow> smoke = {make_row(90.0, Side::Call)};
  std::vector<OracleRow> tune = {make_row(105.0, Side::Call)};
  smoke[0].de *= 100.0;
  tune[0].de = kSelectionAbsFloor / 1.0e8;
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  // Selection followed the evidenced unit, not the smallest multiplier.
  EXPECT_DOUBLE_EQ(result->winner.delta_scale, 100.0);

  const auto find = [](const std::vector<FloorMetric> &metrics) {
    return std::find_if(metrics.begin(), metrics.end(), [](const FloorMetric &metric) {
      return metric.metric_id == "mode_a_delta_rel";
    });
  };
  const auto standard = find(result->metrics);
  const auto standard_baseline = find(result->baseline_metrics);
  const auto symmetric = find(result->symmetric_metrics);
  const auto symmetric_baseline = find(result->baseline_symmetric_metrics);
  ASSERT_NE(standard, result->metrics.end());
  ASSERT_NE(standard_baseline, result->baseline_metrics.end());
  ASSERT_NE(symmetric, result->symmetric_metrics.end());
  ASSERT_NE(symmetric_baseline, result->baseline_symmetric_metrics.end());
  // Worse than baseline on the reported array, better on the symmetric one.
  EXPECT_GT(standard->value, standard_baseline->value);
  EXPECT_LT(symmetric->value, symmetric_baseline->value);
  // Both arrays are bounded to the same rows, so the disagreement is one of
  // objective and not of sample.
  EXPECT_EQ(symmetric->count, standard->count);
  EXPECT_EQ(symmetric_baseline->count, standard_baseline->count);
}

// A sub-floor oracle row used to be reported but excluded from selection, as a
// workaround for the asymmetric objective's denominator pinning. The symmetric
// objective needs no workaround, so such a row now BOTH reports and selects and
// selection_count stays equal to count.
TEST(OracleConvention, SubFloorOracleRowsBothReportAndSelect) {
  std::vector<OracleRow> smoke = {make_row(90.0, Side::Call), make_row(110.0, Side::Put)};
  const std::vector<OracleRow> tune = {make_row(95.0, Side::Call), make_row(105.0, Side::Put)};
  smoke[0].vo = kSelectionAbsFloor / 1000.0;
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const auto volga = std::find_if(result->metrics.begin(), result->metrics.end(),
                                  [](const FloorMetric &metric) {
                                    return metric.metric_id == "mode_a_volga_rel";
                                  });
  ASSERT_NE(volga, result->metrics.end());
  EXPECT_EQ(volga->count, 4);
  EXPECT_EQ(volga->selection_count, 4);
}

// The input model is now chosen on Greeks before price, and what that choice
// cost is published: a cohort authored BY the production map cannot regress
// against the baseline on any Greek, so the field must be present and empty.
TEST(OracleConvention, SweepPublishesTheSelectedInputModelGreekRegressions) {
  const std::vector<OracleRow> smoke = production_rows(90.0, 110.0);
  const std::vector<OracleRow> tune = production_rows(95.0, 105.0);
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_TRUE(result->input_model_regressed_greeks.empty());
  const std::string json = convention_sweep_json(*result, "0123456789abcdef");
  EXPECT_NE(json.find("\"input_model_regressed_greeks\":[]"), std::string::npos);
}

TEST(OracleConvention, SweepJsonPublishesTheProductionMapBesideTheWinner) {
  const std::vector<OracleRow> smoke = {make_row(90.0, Side::Call), make_row(110.0, Side::Put)};
  const std::vector<OracleRow> tune = {make_row(95.0, Side::Call), make_row(105.0, Side::Put)};
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const std::string json = convention_sweep_json(*result, "0123456789abcdef");
  EXPECT_NE(json.find("\"production_conventions\":"), std::string::npos);
  EXPECT_NE(json.find(convention_map_json(winning_convention())), std::string::npos);
}

// An accumulator that admitted nothing means infinity, `%.17g` renders that as
// a bare `inf`, and the receipt would be JSON that does not parse — diagnosed
// as "sweep is not JSON" after a 12-minute aggregate run. The sweep must name
// the empty metric instead.
TEST(OracleConvention, SweepRefusesAMetricNoRowObserved) {
  std::vector<OracleRow> smoke = {make_row(90.0, Side::Call), make_row(110.0, Side::Put)};
  std::vector<OracleRow> tune = {make_row(95.0, Side::Call), make_row(105.0, Side::Put)};
  const double missing = std::numeric_limits<double>::quiet_NaN();
  for (OracleRow &row : smoke) {
    row.ph = missing;
  }
  for (OracleRow &row : tune) {
    row.ph = missing;
  }
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().to_string().find("mode_a_phi_rel"), std::string::npos)
      << result.error().to_string();
}

TEST(OracleConvention, SweepRejectsEmptyCohort) {
  const std::vector<OracleRow> one = {make_row(100.0, Side::Call)};
  EXPECT_FALSE(run_convention_sweep({}, one).has_value());
  EXPECT_FALSE(run_convention_sweep(one, {}).has_value());
}

} // namespace
