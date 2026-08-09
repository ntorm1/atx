#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/fit_policy.hpp"
#include "atx/vol/universe.hpp"

namespace {

using namespace atx::vol;

Chain make_chain(double T, std::size_t n_strikes, double spot, double spread_fraction) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = T;
  c.strikes.resize(n_strikes);
  const std::size_t n2 = 2u * n_strikes;
  c.bids.resize(n2);
  c.asks.resize(n2);
  c.mids.resize(n2);
  c.bid_sizes.assign(n2, 10);
  c.ask_sizes.assign(n2, 10);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, 0u);
  for (std::size_t i = 0; i < n_strikes; ++i) {
    c.strikes[i] = spot * (0.5 + static_cast<double>(i + 1u) / static_cast<double>(n_strikes + 1u));
    for (Side side : {Side::Call, Side::Put}) {
      const std::size_t j = chain_index(static_cast<std::uint16_t>(i), side);
      const double mid = 1.0 + 0.001 * static_cast<double>(i);
      c.bids[j] = mid * (1.0 - 0.5 * spread_fraction);
      c.asks[j] = mid * (1.0 + 0.5 * spread_fraction);
      c.mids[j] = mid;
    }
  }
  return c;
}

Underlying make_underlier(std::string ticker, std::size_t n_strikes, double spread_fraction) {
  Underlying u;
  u.uid = 1u;
  u.ticker = std::move(ticker);
  u.spot = 100.0;
  u.chains.push_back(make_chain(0.05, n_strikes, u.spot, spread_fraction));
  return u;
}

// A chain over an explicit strike ladder, so a fixture can place quotes at a
// chosen log-moneyness instead of inheriting make_chain's 0.5*S..1.5*S sweep.
Chain make_chain_at(double T, const std::vector<double> &strikes, double spread_fraction) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = T;
  c.strikes = strikes;
  const std::size_t n2 = 2u * strikes.size();
  c.bids.resize(n2);
  c.asks.resize(n2);
  c.mids.resize(n2);
  c.bid_sizes.assign(n2, 10);
  c.ask_sizes.assign(n2, 10);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, 0u);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      const std::size_t j = chain_index(static_cast<std::uint16_t>(i), side);
      const double mid = 1.0 + 0.001 * static_cast<double>(i);
      c.bids[j] = mid * (1.0 - 0.5 * spread_fraction);
      c.asks[j] = mid * (1.0 + 0.5 * spread_fraction);
      c.mids[j] = mid;
    }
  }
  return c;
}

// `n_expiries` chains, each carrying `n_strikes` strikes spread evenly over
// [lo*spot, hi*spot].  Board shape is expressed the way the identifiability test
// reads it: how many strikes sit near the money on how many expiries.
Underlying make_board(std::string ticker, std::size_t n_expiries, std::size_t n_strikes, double lo,
                      double hi, double spread_fraction) {
  Underlying u;
  u.uid = 1u;
  u.ticker = std::move(ticker);
  u.spot = 100.0;
  std::vector<double> strikes(n_strikes);
  for (std::size_t i = 0; i < n_strikes; ++i) {
    const double t =
        (n_strikes == 1u) ? 0.5 : static_cast<double>(i) / static_cast<double>(n_strikes - 1u);
    strikes[i] = u.spot * (lo + (hi - lo) * t);
  }
  for (std::size_t e = 0; e < n_expiries; ++e) {
    Chain c = make_chain_at(0.05 + 0.25 * static_cast<double>(e), strikes, spread_fraction);
    c.expiry_id = static_cast<ExpiryId>(e);
    u.chains.push_back(std::move(c));
  }
  return u;
}

TEST(FitPolicy, SpyTickerRoutesDirectlyToHftLinearVariance) {
  const Underlying u = make_underlier("SPY", 2500, 0.01);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_EQ(d.profile.kind, ProfileKind::IndexEtfUltraLiquid);
  EXPECT_EQ(d.preset, FitPreset::Hft);
  EXPECT_EQ(d.curve.kind, VolCurveKind::LinearVariance);
  EXPECT_FALSE(d.needs_cross_validation);
  EXPECT_EQ(d.source, FitDecisionSource::TickerPrior);
}

// The demotion is a test of IDENTIFIABILITY, not of volume: no expiry on this
// board carries `kMinIdentifiableSliceStrikes` distinct near-money strikes, so
// there is nothing to fit a level, a skew and a curvature against.
TEST(FitPolicy, UnidentifiableBoardUsesParsimoniousGuard) {
  const Underlying u = make_board("ZZZZ", /*expiries*/ 5, /*strikes*/ 3, 0.90, 1.10, 0.60);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_EQ(d.features.n_identifiable_expiries, 0u);
  EXPECT_EQ(d.profile.kind, ProfileKind::IlliquidSmallCap);
  EXPECT_EQ(d.curve.kind, VolCurveKind::Svi);
  EXPECT_FALSE(d.needs_cross_validation);
  EXPECT_EQ(d.source, FitDecisionSource::SparseGuard);
}

// The defect this replaces: 250 two-sided legs spread over eight well-quoted
// expiries (DUK's real shape on 2026-07-20) was demoted to IlliquidSmallCap by
// an absolute 600-leg floor, even though every expiry identifies a smile.
TEST(FitPolicy, ThinButIdentifiableBoardKeepsItsBoardVote) {
  const Underlying u = make_board("ZZZZ", /*expiries*/ 8, /*strikes*/ 16, 0.75, 1.30, 0.16);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_LT(d.features.n_live_quotes, 600u) << "the board the old absolute floor demoted";
  EXPECT_GE(d.features.n_identifiable_expiries, 1u);
  EXPECT_NE(d.source, FitDecisionSource::SparseGuard);
  EXPECT_NE(d.profile.kind, ProfileKind::IlliquidSmallCap);
  EXPECT_EQ(d.curve.kind, VolCurveKind::Essvi);
}

// The converse, which no absolute leg count can ever catch: 600 two-sided legs,
// every one of them outside the near-money band.
TEST(FitPolicy, DeepWingOnlyBoardIsNotIdentifiable) {
  const Underlying u = make_board("ZZZZ", /*expiries*/ 10, /*strikes*/ 30, 0.20, 0.55, 0.16);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_GE(d.features.n_live_quotes, 600u);
  EXPECT_EQ(d.features.n_atm_quotes, 0u);
  EXPECT_EQ(d.features.n_identifiable_expiries, 0u);
  EXPECT_EQ(d.source, FitDecisionSource::SparseGuard);
  EXPECT_EQ(d.profile.kind, ProfileKind::IlliquidSmallCap);
}

// A board can be structurally fine per expiry and still be too thin overall;
// `sparse_validation_floor` is the dead-board backstop, counted near the money.
TEST(FitPolicy, NearMoneyLegFloorDemotesADeadBoard) {
  const Underlying u = make_board("ZZZZ", /*expiries*/ 2, /*strikes*/ 5, 0.90, 1.10, 0.60);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_EQ(d.features.n_identifiable_expiries, 2u) << "the per-expiry arm is satisfied";
  EXPECT_LT(d.features.n_atm_quotes, FitPolicyConfig{}.sparse_validation_floor);
  EXPECT_EQ(d.source, FitDecisionSource::SparseGuard);
}

// `sparse_validation_floor == 0` is the documented "route on the board's own
// vote whatever the board looks like" switch; several callers rely on it.
TEST(FitPolicy, ZeroSparseFloorDisablesTheDemotionEntirely) {
  const Underlying u = make_board("ZZZZ", /*expiries*/ 5, /*strikes*/ 3, 0.90, 1.10, 0.60);
  FitPolicyConfig config;
  config.sparse_validation_floor = 0;
  const FitDecision d = select_fit_policy(u, u.ticker, {}, config);
  EXPECT_EQ(d.source, FitDecisionSource::BoardFeatures);
}

TEST(FitPolicy, ExplicitEventWindowSelectsC8) {
  const Underlying u = make_underlier("AAPL", 200, 0.04);
  FitContext context;
  context.event_phase = EventPhase::PreAnnouncement;
  const FitDecision d = select_fit_policy(u, u.ticker, context);
  EXPECT_GE(d.features.max_near_money_strikes, kC8MinSliceStrikes);
  EXPECT_EQ(d.profile.kind, ProfileKind::MegaCapEvent);
  EXPECT_EQ(d.curve.kind, VolCurveKind::C8);
  EXPECT_FALSE(d.needs_cross_validation);
}

TEST(FitPolicy, OpeningSnapshotSuppressesHighDofEventFit) {
  const Underlying u = make_underlier("AAPL", 200, 0.04);
  FitContext context;
  context.event_phase = EventPhase::PostAnnouncement;
  context.session_phase = MarketSessionPhase::Opening;
  const FitDecision d = select_fit_policy(u, u.ticker, context);
  EXPECT_LT(d.features.n_live_quotes, 500u) << "the opening guard's leg-count arm";
  EXPECT_EQ(d.curve.kind, VolCurveKind::Essvi);
}

// The opening guard's second arm -- a wide book at the open -- had no coverage
// at all. Size the board past the 500-leg arm so only the spread can fire.
TEST(FitPolicy, OpeningWideBookSuppressesHighDofEventFit) {
  const Underlying tight = make_underlier("AAPL", 400, 0.04);
  const Underlying wide = make_underlier("AAPL", 400, 0.40);
  FitContext context;
  context.event_phase = EventPhase::PreAnnouncement;
  context.session_phase = MarketSessionPhase::Opening;

  const FitDecision control = select_fit_policy(tight, tight.ticker, context);
  ASSERT_GE(control.features.n_live_quotes, 500u);
  EXPECT_LT(control.features.median_spread_pct, 0.25);
  EXPECT_EQ(control.curve.kind, VolCurveKind::C8) << "only the spread arm may differ";

  const FitDecision d = select_fit_policy(wide, wide.ticker, context);
  EXPECT_GE(d.features.n_live_quotes, 500u);
  EXPECT_GT(d.features.median_spread_pct, 0.25);
  EXPECT_EQ(d.curve.kind, VolCurveKind::Essvi);
}

// C8 spends eight free parameters on one slice. A ticker prior says WHICH
// underlier this is, not that today's snapshot carries enough near-money strikes
// to identify them, so a shallow event board must still drop to the
// five-parameter backbone.
TEST(FitPolicy, ThinEventBoardWillNotSpendEightDegreesOfFreedom) {
  const Underlying u = make_board("AAPL", /*expiries*/ 4, /*strikes*/ 6, 0.85, 1.15, 0.04);
  FitContext context;
  context.event_phase = EventPhase::PreAnnouncement;
  const FitDecision d = select_fit_policy(u, u.ticker, context);
  EXPECT_LT(d.features.max_near_money_strikes, kC8MinSliceStrikes);
  EXPECT_EQ(d.profile.kind, ProfileKind::MegaCapEvent);
  EXPECT_EQ(d.source, FitDecisionSource::TickerPrior);
  EXPECT_EQ(d.curve.kind, VolCurveKind::Essvi);
}

// An unseeded ticker keeps board provenance. This is the seam that used to test
// `verdict.confidence == 0.95`; a board vote must never impersonate a seed.
TEST(FitPolicy, UnseededTickerRetainsBoardProvenance) {
  const Underlying u = make_underlier("NOTATICKER", 2500, 0.01);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_EQ(d.source, FitDecisionSource::BoardFeatures);
  EXPECT_NE(d.profile.confidence, kTickerSeedConfidence);
}

// A board with no two-sided quote has no spread to measure; a 0.0 median would be
// the TIGHTEST possible value and would vote the most liquid tier. Assert on the
// classifier itself -- select_fit_policy's sparse guard would mask the inversion.
TEST(FitPolicy, EmptyBoardDoesNotVoteUltraLiquidIndex) {
  Underlying u;
  u.uid = 1u;
  u.ticker = "NOTATICKER";
  u.spot = 100.0;

  const ClassifierInputs features = classifier_inputs_from_underlier(u);
  EXPECT_EQ(features.n_live_quotes, 0u);
  EXPECT_GT(features.median_spread_pct, 0.40);
  EXPECT_NE(classify_profile(features).kind, ProfileKind::IndexEtfUltraLiquid);
}

// W3-A (F11). A Mark policy that does NOT CONSUME parity evidence must not
// reject a surface because that evidence is absent or incomplete: the evidence
// is advisory for such a policy, so its absence cannot be disqualifying. Before
// W3-A only `Disabled` reached this arm, because a floor-free Mark request
// turned scoring OFF; now every auto-routed board is scored, so a board where
// one slice produced no scored row resolves `Failed` — and rejecting THAT would
// convert a measurement shortfall into a lost surface, strictly worse than the
// unmeasured publish it replaces. Quote/Risk and a floored Mark still fail
// closed: they read the evidence, so its absence is disqualifying.
TEST(FitAdmission, UnconsumedMarkDiagnosticsSurviveAScoringShortfall) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 4u;
  evidence.fitted_expiries = 4u;
  evidence.attempted_quotes = 40u;
  evidence.fitted_quotes = 38u;
  evidence.front_expiry_fitted = true;
  evidence.calendar_arb_free = true;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;
  evidence.strike_monotone = true;
  evidence.strike_convex = true;
  evidence.calendar_total_variance = true;
  evidence.forward_variance_nonnegative = true;

  for (const ParityDiagnosticState state :
       {ParityDiagnosticState::Disabled, ParityDiagnosticState::Failed,
        ParityDiagnosticState::NotScored}) {
    evidence.parity_state = state;
    SCOPED_TRACE(static_cast<int>(state));

    FitAdmissionPolicy mark;
    mark.consumer = SurfaceConsumer::Mark;
    ASSERT_FALSE(fit_admission_consumes_parity(mark));
    EXPECT_TRUE(evaluate_surface_admission(evidence, mark).admitted);

    FitAdmissionPolicy floored = mark;
    floored.min_worst_frac_within_bidask = 0.50;
    const SurfaceAdmissionDecision floored_decision = evaluate_surface_admission(evidence, floored);
    EXPECT_FALSE(floored_decision.admitted);
    EXPECT_TRUE(
        has_admission_failure(floored_decision, SurfaceAdmissionReason::DiagnosticsUnavailable));

    for (const SurfaceConsumer consumer : {SurfaceConsumer::Quote, SurfaceConsumer::Risk}) {
      FitAdmissionPolicy strict = mark;
      strict.consumer = consumer;
      const SurfaceAdmissionDecision strict_decision = evaluate_surface_admission(evidence, strict);
      EXPECT_FALSE(strict_decision.admitted);
      EXPECT_TRUE(
          has_admission_failure(strict_decision, SurfaceAdmissionReason::DiagnosticsUnavailable));
    }
  }
}

// The converse half of the same contract: VALID diagnostics are still read by a
// floor-free Mark policy for every gate that does not depend on the floor, so
// broadening the exemption above must not have made Mark blind.
TEST(FitAdmission, ValidDiagnosticsStillGateNonFiniteQualityOnAFloorFreeMark) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 1u;
  evidence.fitted_expiries = 1u;
  evidence.attempted_quotes = 10u;
  evidence.fitted_quotes = 10u;
  evidence.front_expiry_fitted = true;
  evidence.parity_state = ParityDiagnosticState::Valid;
  evidence.finite_diagnostics = false; // a scored but non-finite chi2/RMSE
  evidence.calendar_arb_free = true;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;

  FitAdmissionPolicy mark;
  mark.consumer = SurfaceConsumer::Mark;
  const SurfaceAdmissionDecision decision = evaluate_surface_admission(evidence, mark);
  EXPECT_FALSE(decision.admitted);
  EXPECT_TRUE(has_admission_failure(decision, SurfaceAdmissionReason::NonFiniteDiagnostics));
}

TEST(FitPolicy, ForcedCrossValidationOverridesDirectTickerPrior) {
  const Underlying u = make_underlier("SPY", 2500, 0.01);
  FitPolicyConfig config;
  config.mode = FitSelectionMode::CrossValidated;
  const FitDecision d = select_fit_policy(u, u.ticker, {}, config);
  EXPECT_TRUE(d.needs_cross_validation);
  EXPECT_EQ(d.source, FitDecisionSource::CrossValidation);
}

} // namespace
