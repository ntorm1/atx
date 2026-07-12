#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/curve_selector.hpp"
#include "atx/vol/fit_policy.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/spy_fixture.hpp"

namespace {

using atx::vol::CandidateScore;
using atx::vol::FitAdmissionPolicy;
using atx::vol::SurfaceAdmissionEvidence;
using atx::vol::SurfaceAdmissionReason;

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
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;

  const auto decision = atx::vol::evaluate_surface_admission(evidence, FitAdmissionPolicy{});
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
  evidence.european_price_bounds = true;
  FitAdmissionPolicy policy;
  policy.consumer = atx::vol::SurfaceConsumer::Mark;
  policy.require_calendar_arb_free = false;

  const auto decision = atx::vol::evaluate_surface_admission(evidence, policy);
  EXPECT_FALSE(decision.admitted);
  EXPECT_EQ(decision.primary_reason, SurfaceAdmissionReason::FiniteIvDomain);
}

} // namespace
