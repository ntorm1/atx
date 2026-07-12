#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/curve_selector.hpp"
#include "atx/vol/fit_policy.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/spy_fixture.hpp"
#include "atx/vol/vol_curve.hpp"

namespace {

using atx::vol::CandidateScore;
using atx::vol::FitAdmissionPolicy;
using atx::vol::SurfaceAdmissionEvidence;
using atx::vol::ParityDiagnosticState;
using atx::vol::SurfaceAdmissionReason;
using atx::vol::select_best_candidate;
using atx::vol::VolCurveKind;

TEST(CurveSelector, FullCommonKeyCoverageBeatsEasyPartialCandidate) {
  CandidateScore easy;
  easy.admitted = true;
  easy.oos_vw = 1.0;
  easy.n_required_slices = 3u;
  easy.n_slices = 1u;
  easy.n_required_holdout = 30u;
  easy.n_holdout = 10u;
  easy.expiry_coverage = 1.0 / 3.0;
  easy.holdout_coverage = 1.0 / 3.0;
  easy.dof_sum = 3u;

  CandidateScore complete;
  complete.admitted = true;
  complete.oos_vw = 0.80;
  complete.n_required_slices = 3u;
  complete.n_slices = 3u;
  complete.n_required_holdout = 30u;
  complete.n_holdout = 30u;
  complete.expiry_coverage = 1.0;
  complete.holdout_coverage = 1.0;
  complete.dof_sum = 18u;

  const std::vector<CandidateScore> scores{easy, complete};
  const auto selected = atx::vol::select_candidate_index(scores, 0.004);
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_EQ(*selected, 1u);
}

TEST(CurveSelector, RefusesToChooseWhenNoCandidateMeetsCommonKeyAdmission) {
  CandidateScore partial;
  partial.oos_vw = 1.0;
  partial.expiry_coverage = 0.5;
  partial.holdout_coverage = 0.5;
  partial.n_holdout = 10u;
  const std::vector<CandidateScore> scores{partial};

  const auto selected = atx::vol::select_candidate_index(scores, 0.004);
  ASSERT_FALSE(selected.has_value());
  EXPECT_EQ(selected.error().code(), atx::core::ErrorCode::NotFound);
}

TEST(CurveSelector, ParsimonyMarginIsAnchoredToGlobalQualityLeader) {
  CandidateScore leader;
  leader.admitted = true;
  leader.expiry_coverage = 1.0;
  leader.holdout_coverage = 1.0;
  leader.oos_vw = 1.0;
  leader.n_slices = 1u;
  leader.dof_sum = 10u;
  CandidateScore near = leader;
  near.oos_vw = 0.997;
  near.dof_sum = 5u;
  CandidateScore chained_but_too_far = leader;
  chained_but_too_far.oos_vw = 0.994;
  chained_but_too_far.dof_sum = 1u;

  const std::vector<CandidateScore> scores{leader, near, chained_but_too_far};
  const auto selected = atx::vol::select_candidate_index(scores, 0.004);
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_EQ(*selected, 1u);
}

TEST(CurveSelector, ButterflyDisqualifiedCandidateIsNotAdmissible) {
  // A butterfly-disqualified family must not survive select_candidate_index
  // even when its coverage and oos_vw dominate every rival.
  CandidateScore disq;
  disq.admitted = true;
  disq.disqualified = true;
  disq.oos_vw = 1.0;
  disq.expiry_coverage = 1.0;
  disq.holdout_coverage = 1.0;
  disq.n_holdout = 30u;
  disq.n_slices = 3u;
  disq.dof_sum = 15u;

  CandidateScore clean = disq;
  clean.disqualified = false;
  clean.oos_vw = 0.7;

  const std::vector<CandidateScore> scores{disq, clean};
  const auto selected = atx::vol::select_candidate_index(scores, 0.004);
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_EQ(*selected, 1u);
}

TEST(CurveSelector, ChiSquareBreaksTieInsideParsimonyBand) {
  // Within the parsimony band and at equal coverage, the candidate whose
  // reduced chi-square is closest to 1 wins even at higher DoF (Task C2.5
  // ordering: oos_vw band -> chi2 -> DoF -> oos_vw).
  CandidateScore far_chi2;
  far_chi2.admitted = true;
  far_chi2.oos_vw = 0.900;
  far_chi2.expiry_coverage = 1.0;
  far_chi2.holdout_coverage = 1.0;
  far_chi2.n_holdout = 100u;
  far_chi2.n_slices = 4u;
  far_chi2.dof_sum = 20u; // avg dof 5
  far_chi2.chi2_reduced = 2.5;
  far_chi2.metrics_valid = true;

  CandidateScore near_chi2 = far_chi2;
  near_chi2.oos_vw = 0.899;
  near_chi2.dof_sum = 32u; // avg dof 8
  near_chi2.chi2_reduced = 1.1;

  const std::vector<CandidateScore> scores{far_chi2, near_chi2};
  const auto selected = atx::vol::select_candidate_index(scores, 0.004);
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_EQ(*selected, 1u);
}

TEST(CurveSelector, SamplesLiquidityWithinDeterministicTenorStrataOnCommonKeys) {
  const atx::vol::SynthPanelSpec spec = atx::vol::make_spy_synthetic_spec();
  const auto panel = atx::vol::make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto chain = atx::vol::OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

  atx::vol::SurfaceParityInputs inputs;
  inputs.S = spec.spot;
  inputs.r = spec.r;
  inputs.cash_divs = spec.cash_divs;
  inputs.now_ts_ns = chain->now_ns();
  atx::vol::SelectorConfig selector;
  selector.oos_max_expiries = 3u;
  atx::vol::CurveConfig linear;
  linear.kind = atx::vol::VolCurveKind::LinearVariance;
  atx::vol::CurveConfig svi;
  svi.kind = atx::vol::VolCurveKind::Svi;
  selector.candidates = {linear, svi};

  const auto first = atx::vol::select_curve(chain->underlying(), inputs, selector);
  const auto second = atx::vol::select_curve(chain->underlying(), inputs, selector);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(second.has_value()) << second.error().to_string();
  EXPECT_EQ(first->sampled_expiry_indices, (std::vector<std::size_t>{1u, 3u, 5u}));
  EXPECT_EQ(first->sampled_expiry_indices, second->sampled_expiry_indices);
  ASSERT_EQ(first->scores.size(), 2u);
  EXPECT_EQ(first->scores[0].n_required_slices, first->scores[1].n_required_slices);
  EXPECT_EQ(first->scores[0].n_required_holdout, first->scores[1].n_required_holdout);
  for (const CandidateScore &score : first->scores) {
    EXPECT_EQ(score.n_holdout, score.n_required_holdout);
    EXPECT_LE(score.n_successful_holdout, score.n_holdout);
  }
  EXPECT_DOUBLE_EQ(first->scores[0].vega_weight_total, first->scores[1].vega_weight_total);
}

// ── Task C2.5: unit coverage for the fit-metrics selection policy ──
// (select_best_candidate). The winner ordering is oos_vw (within
// parsimony_margin) -> reduced-chi-square closest to 1 -> parsimony DoF ->
// higher oos_vw, with butterfly-disqualified families excluded entirely.
// Scores are constructed directly so the policy is tested in isolation from the
// (expensive, fixture-bound) held-out fit machinery.

// A scorable candidate with the fields the policy reads. `dof_sum`/`n_slices`
// give avg DoF = dof_sum/n_slices.
[[nodiscard]] CandidateScore mk(VolCurveKind kind, double oos_vw, double chi2,
                                bool metrics_valid, std::size_t dof_sum,
                                std::size_t n_slices, bool disqualified = false) {
  CandidateScore s;
  s.kind = kind;
  s.oos_vw = oos_vw;
  s.chi2_reduced = chi2;
  s.metrics_valid = metrics_valid;
  s.dof_sum = dof_sum;
  s.n_slices = n_slices;
  s.n_holdout = 100;  // scorable
  s.disqualified = disqualified;
  return s;
}

constexpr double kMargin = 0.004;

TEST(CurveSelector, TieBreaksOnChiSquareClosestToOne) {
  // A leads on oos_vw but its reduced chi^2 is far from 1; B is within the
  // parsimony tie band and has chi^2 much closer to 1 (even at higher DoF).
  // The chi^2 tie-break runs BEFORE parsimony, so B wins.
  std::vector<CandidateScore> scores;
  scores.push_back(mk(VolCurveKind::Essvi, 0.900, 2.5, true, 20, 4));  // dof 5
  scores.push_back(mk(VolCurveKind::C8, 0.899, 1.1, true, 32, 4));     // dof 8
  EXPECT_EQ(select_best_candidate(scores, kMargin), 1u);
}

TEST(CurveSelector, ButterflyDisqualifiedFamilyExcluded) {
  // A has the best oos_vw but is butterfly-disqualified; it must be dropped and
  // the next scorable family (B) chosen even at a lower oos_vw.
  std::vector<CandidateScore> scores;
  scores.push_back(mk(VolCurveKind::Svi, 0.950, 1.0, true, 20, 4,
                      /*disqualified=*/true));
  scores.push_back(mk(VolCurveKind::Essvi, 0.800, 3.0, true, 12, 4));
  EXPECT_EQ(select_best_candidate(scores, kMargin), 1u);
}

TEST(CurveSelector, ParsimonyBreaksTieWhenChiSquareUnavailable) {
  // Equal oos_vw, neither has valid metrics (chi^2 does not participate): the
  // tie falls through to fewer average DoF — the parsimonious family wins.
  std::vector<CandidateScore> scores;
  scores.push_back(mk(VolCurveKind::C8, 0.900, 0.0, false, 32, 4));      // dof 8
  scores.push_back(mk(VolCurveKind::Essvi, 0.900, 0.0, false, 12, 4));   // dof 3
  EXPECT_EQ(select_best_candidate(scores, kMargin), 1u);
}

TEST(CurveSelector, UniqueBestOosVwWinsOutright) {
  // When one family is strictly best on oos_vw beyond the tie band, it wins
  // regardless of chi^2 / DoF.
  std::vector<CandidateScore> scores;
  scores.push_back(mk(VolCurveKind::ConvexDense, 0.990, 5.0, true, 160, 4));
  scores.push_back(mk(VolCurveKind::Essvi, 0.700, 1.0, true, 12, 4));
  EXPECT_EQ(select_best_candidate(scores, kMargin), 0u);
}

TEST(CurveSelector, NoScorableCandidateReturnsZero) {
  // All families failed to fit (n_holdout == 0): the policy returns 0 and the
  // caller reports NotFound.
  std::vector<CandidateScore> scores(2);
  scores[0].n_holdout = 0;
  scores[1].n_holdout = 0;
  EXPECT_EQ(select_best_candidate(scores, kMargin), 0u);
}

TEST(FitAdmission, RejectsPartialAndUnhealthySurfaceWithStablePrimaryReason) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 4u;
  evidence.fitted_expiries = 1u;
  evidence.attempted_quotes = 100u;
  evidence.fitted_quotes = 20u;
  evidence.front_expiry_fitted = false;
  evidence.max_consecutive_expiry_gaps = 3u;
  evidence.calendar_arb_free = false;
  evidence.finite_diagnostics = true;
  evidence.parity_state = ParityDiagnosticState::Valid;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;

  // The strict risk contract is what rejects a partial/unhealthy surface on these
  // structural grounds; the default now serves marks and would admit this
  // evidence, so request the risk policy explicitly.
  const auto decision =
      atx::vol::evaluate_surface_admission(evidence, atx::vol::risk_admission_policy());
  EXPECT_FALSE(decision.admitted);
  EXPECT_EQ(decision.primary_reason, SurfaceAdmissionReason::InsufficientExpiryCoverage);
  EXPECT_TRUE(atx::vol::has_admission_failure(decision,
                                              SurfaceAdmissionReason::InsufficientExpiryCoverage));
  EXPECT_TRUE(
      atx::vol::has_admission_failure(decision, SurfaceAdmissionReason::FrontExpiryMissing));
  EXPECT_TRUE(atx::vol::has_admission_failure(decision, SurfaceAdmissionReason::CalendarArbitrage));
}

TEST(FitAdmission, ExplicitDegradedMarkPolicyCanAdmitPartialSurface) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 4u;
  evidence.fitted_expiries = 2u;
  evidence.attempted_quotes = 100u;
  evidence.fitted_quotes = 50u;
  evidence.front_expiry_fitted = false;
  evidence.max_consecutive_expiry_gaps = 1u;
  evidence.calendar_arb_free = false;
  evidence.finite_diagnostics = true;
  evidence.parity_state = ParityDiagnosticState::Valid;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;

  FitAdmissionPolicy policy;
  policy.consumer = atx::vol::SurfaceConsumer::Mark;
  policy.min_expiry_coverage = 0.5;
  policy.min_quote_coverage = 0.5;
  policy.require_front_expiry = false;
  policy.max_consecutive_expiry_gaps = 1u;
  policy.require_calendar_arb_free = false;

  const auto decision = atx::vol::evaluate_surface_admission(evidence, policy);
  EXPECT_TRUE(decision.admitted);
  EXPECT_EQ(decision.primary_reason, SurfaceAdmissionReason::None);
}

TEST(FitAdmission, ConsumerSelectsInvariantGuaranteesMaterially) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 1u;
  evidence.fitted_expiries = 1u;
  evidence.attempted_quotes = 10u;
  evidence.fitted_quotes = 10u;
  evidence.front_expiry_fitted = true;
  evidence.finite_diagnostics = true;
  evidence.parity_state = ParityDiagnosticState::Valid;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;
  evidence.strike_monotone = false;
  evidence.strike_convex = false;
  evidence.calendar_total_variance = false;
  evidence.forward_variance_nonnegative = false;

  FitAdmissionPolicy mark;
  mark.consumer = atx::vol::SurfaceConsumer::Mark;
  mark.require_calendar_arb_free = false;
  EXPECT_TRUE(atx::vol::evaluate_surface_admission(evidence, mark).admitted);

  FitAdmissionPolicy quote = mark;
  quote.consumer = atx::vol::SurfaceConsumer::Quote;
  EXPECT_EQ(atx::vol::evaluate_surface_admission(evidence, quote).primary_reason,
            SurfaceAdmissionReason::StrikeMonotonicity);

  FitAdmissionPolicy risk = mark;
  risk.consumer = atx::vol::SurfaceConsumer::Risk;
  EXPECT_TRUE(atx::vol::has_admission_failure(atx::vol::evaluate_surface_admission(evidence, risk),
                                              SurfaceAdmissionReason::CalendarTotalVariance));
}

TEST(FitAdmission, RejectsImpossibleCountsBeforeThresholdChecks) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 1u;
  evidence.fitted_expiries = 2u;
  evidence.attempted_quotes = 2u;
  evidence.fitted_quotes = 3u;
  evidence.front_expiry_fitted = true;
  evidence.finite_diagnostics = true;
  evidence.parity_state = ParityDiagnosticState::Valid;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;
  evidence.strike_monotone = true;
  evidence.strike_convex = true;
  evidence.calendar_total_variance = true;
  evidence.forward_variance_nonnegative = true;

  const auto decision = atx::vol::evaluate_surface_admission(evidence, FitAdmissionPolicy{});
  EXPECT_FALSE(decision.admitted);
  EXPECT_EQ(decision.primary_reason, SurfaceAdmissionReason::ImpossibleEvidence);
}

TEST(FitAdmission, EmptyOrNarrowCommonDomainCannotPassMarkAdmission) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 1u;
  evidence.fitted_expiries = 1u;
  evidence.attempted_quotes = 10u;
  evidence.fitted_quotes = 10u;
  evidence.front_expiry_fitted = true;
  evidence.finite_diagnostics = true;
  evidence.parity_state = ParityDiagnosticState::Valid;
  evidence.european_price_bounds = true;
  FitAdmissionPolicy policy;
  policy.consumer = atx::vol::SurfaceConsumer::Mark;
  policy.require_calendar_arb_free = false;

  const auto decision = atx::vol::evaluate_surface_admission(evidence, policy);
  EXPECT_FALSE(decision.admitted);
  EXPECT_EQ(decision.primary_reason, SurfaceAdmissionReason::FiniteIvDomain);
}

TEST(FitAdmission, DisabledParityIsAllowedOnlyForAnExplicitMarkConsumer) {
  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 1u;
  evidence.fitted_expiries = 1u;
  evidence.attempted_quotes = 10u;
  evidence.fitted_quotes = 10u;
  evidence.front_expiry_fitted = true;
  evidence.parity_state = ParityDiagnosticState::Disabled;
  evidence.calendar_arb_free = true;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;
  evidence.strike_monotone = true;
  evidence.strike_convex = true;
  evidence.calendar_total_variance = true;
  evidence.forward_variance_nonnegative = true;

  FitAdmissionPolicy mark;
  mark.consumer = atx::vol::SurfaceConsumer::Mark;
  EXPECT_TRUE(atx::vol::evaluate_surface_admission(evidence, mark).admitted);

  FitAdmissionPolicy quote = mark;
  quote.consumer = atx::vol::SurfaceConsumer::Quote;
  const auto quote_decision = atx::vol::evaluate_surface_admission(evidence, quote);
  EXPECT_FALSE(quote_decision.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(
      quote_decision, SurfaceAdmissionReason::DiagnosticsUnavailable));

  FitAdmissionPolicy risk = mark;
  risk.consumer = atx::vol::SurfaceConsumer::Risk;
  const auto risk_decision = atx::vol::evaluate_surface_admission(evidence, risk);
  EXPECT_FALSE(risk_decision.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(
      risk_decision, SurfaceAdmissionReason::DiagnosticsUnavailable));
}

} // namespace
