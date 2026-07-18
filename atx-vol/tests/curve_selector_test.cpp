#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp" // american_price, AmericanMethod
#include "atx/vol/chain.hpp"
#include "atx/vol/curve_selector.hpp" // CandidateScore, select_candidate_index, select_curve
#include "atx/vol/dividend.hpp"       // hybrid_forward, HybridDivParams, DividendEvent
#include "atx/vol/fit_policy.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/spy_fixture.hpp"
#include "atx/vol/vol_curve.hpp" // VolCurveKind, CurveConfig

namespace {

using atx::vol::CandidateScore;
using atx::vol::FitAdmissionPolicy;
using atx::vol::ParityDiagnosticState;
using atx::vol::select_best_candidate;
using atx::vol::SurfaceAdmissionEvidence;
using atx::vol::SurfaceAdmissionReason;
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

TEST(CurveSelector, ServedCoverageFloorRejectsNarrowFamilySpecificRebuild) {
  atx::vol::FitAdmissionPolicy base;
  base.min_quote_coverage = 0.25;
  atx::vol::SelectorConfig selector;
  selector.min_served_quote_coverage = 0.50;

  const atx::vol::FitAdmissionPolicy effective =
      atx::vol::detail::selector_served_admission_policy(base, selector);
  EXPECT_DOUBLE_EQ(effective.min_quote_coverage, 0.50);
  base.min_quote_coverage = 0.75;
  EXPECT_DOUBLE_EQ(
      atx::vol::detail::selector_served_admission_policy(base, selector).min_quote_coverage, 0.75);

  SurfaceAdmissionEvidence evidence;
  evidence.attempted_expiries = 1u;
  evidence.fitted_expiries = 1u;
  evidence.attempted_quotes = 100u;
  evidence.fitted_quotes = 47u;
  evidence.front_expiry_fitted = true;
  evidence.parity_state = ParityDiagnosticState::Disabled;
  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;
  const auto narrow = atx::vol::evaluate_surface_admission(evidence, effective);
  EXPECT_TRUE(
      atx::vol::has_admission_failure(narrow, SurfaceAdmissionReason::InsufficientQuoteCoverage));

  evidence.fitted_quotes = 89u;
  const auto broad = atx::vol::evaluate_surface_admission(evidence, effective);
  EXPECT_FALSE(
      atx::vol::has_admission_failure(broad, SurfaceAdmissionReason::InsufficientQuoteCoverage));
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

// Additional symbols used by the SpiderRock SplineVol candidacy tests (Task
// I5) and their bumpy-board fixture below. `CandidateScore`,
// `select_best_candidate`, and `VolCurveKind` are already imported at the top
// of this anonymous namespace.
using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::default_selector_candidates;
using atx::vol::DividendEvent;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::select_curve;
using atx::vol::SelectorConfig;
using atx::vol::Side;
using atx::vol::SplineVolCurve;
using atx::vol::SplineVolParams;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;
using atx::vol::detail::slice_butterfly_violations;

// A scorable candidate with the fields the policy reads. `dof_sum`/`n_slices`
// give avg DoF = dof_sum/n_slices.
[[nodiscard]] CandidateScore mk(VolCurveKind kind, double oos_vw, double chi2, bool metrics_valid,
                                std::size_t dof_sum, std::size_t n_slices,
                                bool disqualified = false) {
  CandidateScore s;
  s.kind = kind;
  s.oos_vw = oos_vw;
  s.chi2_reduced = chi2;
  s.metrics_valid = metrics_valid;
  s.dof_sum = dof_sum;
  s.n_slices = n_slices;
  s.n_holdout = 100; // scorable
  s.disqualified = disqualified;
  return s;
}

constexpr double kMargin = 0.004;

// ── Task I5.4 fixture: a synthetic board whose smile has a local ATM bump ──
//
// Essvi's functional form (`essvi_total_w`: a single smooth hyperbola-family
// curve) can represent a monotonic-ish skew but structurally CANNOT represent
// a genuine local curvature reversal (a bump); a natural cubic spline over
// enough active knots can. Bump amplitude/width are tuned to stay clean on
// the post-fit Lee/Roper butterfly-density scan (no local no-arb violation)
// while still being large enough that Essvi's held-out fit is materially
// worse than SplineVol's. Mirrors curve_fit_parallel_test.cpp's
// make_chain/make_synthetic_underlying (American-price-then-de-Americanize
// self-consistent fixture pattern), with an added bump term.
constexpr double kBumpSpot = 100.0;
constexpr double kBumpRate = 0.03;

[[nodiscard]] double bumpy_smile_sigma(double k) noexcept {
  const double base = 0.22 + 0.03 * k + 0.15 * k * k;
  const double bump = 0.05 * std::exp(-(k * k) / (2.0 * 0.09 * 0.09));
  return base + bump;
}

[[nodiscard]] Chain make_bumpy_chain(double T, int n_strikes) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * 3.1536e16); // ACT/365-ish, ns/yr

  const std::vector<DividendEvent> no_divs;
  const double F = hybrid_forward(kBumpSpot, kBumpRate, /*borrow=*/0.0, T, no_divs, c.expiry_ns,
                                  /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kBumpRate - std::log(F / kBumpSpot) / T;

  constexpr double kLo = -0.35;
  constexpr double kHi = 0.35;
  c.strikes.reserve(static_cast<std::size_t>(n_strikes));
  for (int i = 0; i < n_strikes; ++i) {
    const double frac = static_cast<double>(i) / static_cast<double>(n_strikes - 1);
    const double k = kLo + frac * (kHi - kLo);
    c.strikes.push_back(F * std::exp(k));
  }

  const std::size_t n = c.strikes.size();
  c.bids.assign(2 * n, 0.0);
  c.asks.assign(2 * n, 0.0);
  c.bid_sizes.assign(2 * n, 0);
  c.ask_sizes.assign(2 * n, 0);
  c.mids.assign(2 * n, 0.0);
  c.ivs.assign(2 * n, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(2 * n, 0);
  c.flags.assign(2 * n, 0);

  for (std::size_t i = 0; i < n; ++i) {
    const double K = c.strikes[i];
    const double k = std::log(K / F);
    const Side side = (k >= 0.0) ? Side::Call : Side::Put;
    const double sigma = bumpy_smile_sigma(k);
    const auto px_res = american_price(kBumpSpot, K, T, sigma, kBumpRate, q_eff, side,
                                       AmericanMethod::AndersenLake, std::nullopt);
    if (!px_res.has_value()) {
      ADD_FAILURE() << "american_price failed for K=" << K << " T=" << T;
      continue;
    }
    const double px = *px_res;
    const double half = std::max(0.0025 * px, 1.0e-4);
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
    c.mids[idx] = px;
    c.bids[idx] = px - half;
    c.asks[idx] = px + half;
    c.bid_sizes[idx] = 1;
    c.ask_sizes[idx] = 1;
  }
  return c;
}

[[nodiscard]] Underlying make_bumpy_underlying() {
  Underlying u;
  u.spot = kBumpSpot;
  u.chains.push_back(make_bumpy_chain(0.25, 25));
  return u;
}

[[nodiscard]] SurfaceParityInputs bumpy_inputs() {
  SurfaceParityInputs in{};
  in.S = kBumpSpot;
  in.r = kBumpRate;
  in.deam.imply_borrow = false; // borrow fixed at 0 -- see make_bumpy_chain
  in.deam.borrow_fixed = 0.0;
  return in;
}

TEST(CurveSelector, TieBreaksOnChiSquareClosestToOne) {
  // A leads on oos_vw but its reduced chi^2 is far from 1; B is within the
  // parsimony tie band and has chi^2 much closer to 1 (even at higher DoF).
  // The chi^2 tie-break runs BEFORE parsimony, so B wins.
  std::vector<CandidateScore> scores;
  scores.push_back(mk(VolCurveKind::Essvi, 0.900, 2.5, true, 20, 4)); // dof 5
  scores.push_back(mk(VolCurveKind::C8, 0.899, 1.1, true, 32, 4));    // dof 8
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
  scores.push_back(mk(VolCurveKind::C8, 0.900, 0.0, false, 32, 4));    // dof 8
  scores.push_back(mk(VolCurveKind::Essvi, 0.900, 0.0, false, 12, 4)); // dof 3
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

TEST(SelectorBudget, UnlimitedDefaultEvaluatesEveryCandidate) {
  const Underlying under = make_bumpy_underlying();
  const SurfaceParityInputs in = bumpy_inputs();

  // R-33: `select_curve` no longer defaults `sel`; pass the unbounded research
  // config explicitly (the behavior this test asserts).
  const auto selected = select_curve(under, in, atx::vol::SelectorConfig{});
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_FALSE(selected->budget_exhausted);
  EXPECT_EQ(selected->scores_evaluated, selected->scores.size());
  EXPECT_EQ(selected->scores.size(), default_selector_candidates().size());
  EXPECT_EQ(default_selector_candidates().front().kind, VolCurveKind::Essvi);
}

TEST(SelectorBudget, ExpiredBudgetStopsOnlyBetweenCompletedCandidates) {
  const Underlying under = make_bumpy_underlying();
  const SurfaceParityInputs in = bumpy_inputs();

  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  CurveConfig c8;
  c8.kind = VolCurveKind::C8;
  CurveConfig convex;
  convex.kind = VolCurveKind::ConvexDense;
  SelectorConfig config;
  config.candidates = {essvi, c8, convex};
  config.time_budget_ms = 0.001;

  const auto selected = select_curve(under, in, config);
  ASSERT_TRUE(selected.has_value()) << selected.error().to_string();
  EXPECT_TRUE(selected->budget_exhausted);
  EXPECT_EQ(selected->scores_evaluated, 1u);
  EXPECT_EQ(selected->scores.size(), 1u);
  EXPECT_EQ(selected->chosen.kind, VolCurveKind::Essvi);
}

TEST(SelectorBudget, ExpiredBudgetDoesNotRunPastAnUnselectableCandidate) {
  const Underlying under = make_bumpy_underlying();
  const SurfaceParityInputs in = bumpy_inputs();

  CurveConfig impossible;
  impossible.kind = VolCurveKind::SplineVol;
  impossible.spline.min_obs = 1000u;
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  CurveConfig c8;
  c8.kind = VolCurveKind::C8;
  SelectorConfig config;
  config.candidates = {impossible, essvi, c8};
  config.time_budget_ms = 0.001;

  const auto selected = select_curve(under, in, config);
  ASSERT_FALSE(selected.has_value());
  EXPECT_EQ(selected.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_NE(selected.error().message().find("time budget"), std::string::npos);
}

TEST(SelectorBudget, RejectsNegativeBudget) {
  SelectorConfig config;
  config.time_budget_ms = -1.0;
  EXPECT_FALSE(select_curve(make_bumpy_underlying(), bumpy_inputs(), config).has_value());
}

TEST(CurveSelector, RejectsInvalidServedCoverageFloor) {
  SelectorConfig config;
  config.min_served_quote_coverage = 1.01;
  EXPECT_FALSE(select_curve(make_bumpy_underlying(), bumpy_inputs(), config).has_value());
}

// Task I5.3: the per-kind butterfly gate (`detail::slice_butterfly_violations`,
// the mapping `select_curve` applies to every fitted candidate slice before
// scoring) must read SplineVol's OWN fitted diagnostic count
// (`SplineVolParams::n_butterfly_viol`, populated by the post-fit Lee/Roper
// scan in fit_spline_vol_slice -- see spline_curve.hpp step 6), not fall
// through to a hard-coded 0 the way an unlisted/placeholder kind would. Hand
// -build a SplineVolCurve directly (bypassing the fitter) so the count is
// injected exactly, independent of any real fit producing exactly 3
// violations.
TEST(CurveSelector, ButterflyGateReadsSplineViolations) {
  SplineVolParams p;
  p.atm_vol = 0.20;
  p.z = {-1.0, -0.5, 0.0, 0.5, 1.0};
  p.mult = {1.0, 1.0, 1.0, 1.0, 1.0};
  p.z_lo_valid = -1.0;
  p.z_hi_valid = 1.0;
  p.n_butterfly_viol = 3;
  const SplineVolCurve curve(p, /*T=*/0.25, /*F=*/100.0, /*df=*/0.98);

  EXPECT_EQ(slice_butterfly_violations(curve, 0.25, -0.5, 0.5), 3u);
}

// Task I5.4: with `spline_candidate` unset anywhere (the default -- every
// `default_selector_candidates()` entry defaults it `false`), a caller
// passing NO explicit candidates (the common/production path) is completely
// unaffected: appending SplineVol to a caller-supplied list -- even a
// spline-favorable board -- leaves every OTHER candidate's score
// byte-for-byte identical to the flag-unset run, because each candidate is
// fit/scored independently against the SAME shared per-expiry even/odd
// split (no shared mutable state between candidates in the scoring loop).
// This is the "flag=false selection is bit-identical to pre-task" guarantee,
// demonstrated directly rather than via a hardcoded golden value that could
// silently drift.
TEST(CurveSelector, SplineCandidateFlagOffIsBitIdenticalToBaseline) {
  const Underlying under = make_bumpy_underlying();
  const SurfaceParityInputs in = bumpy_inputs();

  SelectorConfig sel_off; // candidates empty -> default_selector_candidates()
  auto out_off = select_curve(under, in, sel_off);
  ASSERT_TRUE(out_off.has_value()) << out_off.error().to_string();
  ASSERT_EQ(out_off->scores.size(), 5u);

  std::vector<CurveConfig> six = default_selector_candidates();
  ASSERT_EQ(six.size(), 5u);
  six[0].spline_candidate = true; // opt-in lives on ANY candidate's config
  SelectorConfig sel_on;
  sel_on.candidates = six;
  auto out_on = select_curve(under, in, sel_on);
  ASSERT_TRUE(out_on.has_value()) << out_on.error().to_string();
  ASSERT_EQ(out_on->scores.size(), 6u); // the 5 + appended SplineVol

  for (std::size_t i = 0; i < 5; ++i) {
    const CandidateScore &a = out_off->scores[i];
    const CandidateScore &b = out_on->scores[i];
    EXPECT_EQ(a.kind, b.kind) << "candidate " << i;
    EXPECT_EQ(a.oos_in_band, b.oos_in_band) << "candidate " << i;
    EXPECT_EQ(a.oos_vw, b.oos_vw) << "candidate " << i;
    EXPECT_EQ(a.dof_sum, b.dof_sum) << "candidate " << i;
    EXPECT_EQ(a.n_holdout, b.n_holdout) << "candidate " << i;
    EXPECT_EQ(a.n_in_band, b.n_in_band) << "candidate " << i;
    EXPECT_EQ(a.n_slices, b.n_slices) << "candidate " << i;
    EXPECT_EQ(a.chi2_reduced, b.chi2_reduced) << "candidate " << i;
    EXPECT_EQ(a.metrics_valid, b.metrics_valid) << "candidate " << i;
    EXPECT_EQ(a.n_butterfly_viol, b.n_butterfly_viol) << "candidate " << i;
    EXPECT_EQ(a.disqualified, b.disqualified) << "candidate " << i;
  }
  EXPECT_EQ(out_on->scores[5].kind, VolCurveKind::SplineVol);
  // Selection with the flag unset never resolves to SplineVol (it is not in
  // the candidate list at all).
  EXPECT_NE(out_off->chosen.kind, VolCurveKind::SplineVol);
}

// Task I5.4: on a spline-favorable board (a local ATM smile bump Essvi's
// single-hyperbola functional form cannot represent but a natural cubic
// spline over active knots can), asking for SplineVol via the flag CAN
// select it as the winner -- proving the gate actually reaches a real
// candidacy, not just a scored-but-never-chosen entry.
TEST(CurveSelector, SplineCandidateFlagAddsCandidate) {
  const Underlying under = make_bumpy_underlying();
  const SurfaceParityInputs in = bumpy_inputs();

  // flag=false: selects from the current 5 families (never SplineVol, which
  // is not a candidate at all).
  SelectorConfig sel_off;
  auto out_off = select_curve(under, in, sel_off);
  ASSERT_TRUE(out_off.has_value()) << out_off.error().to_string();
  EXPECT_NE(out_off->chosen.kind, VolCurveKind::SplineVol);
  bool is_one_of_five = false;
  for (const VolCurveKind k : {VolCurveKind::ConvexDense, VolCurveKind::LinearVariance,
                               VolCurveKind::Essvi, VolCurveKind::Svi, VolCurveKind::C8}) {
    is_one_of_five = is_one_of_five || (out_off->chosen.kind == k);
  }
  EXPECT_TRUE(is_one_of_five);

  // flag=true, weak field (Essvi alone): SplineVol is appended and wins.
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  essvi.spline_candidate = true;
  SelectorConfig sel_on;
  sel_on.candidates = {essvi};
  auto out_on = select_curve(under, in, sel_on);
  ASSERT_TRUE(out_on.has_value()) << out_on.error().to_string();
  ASSERT_EQ(out_on->scores.size(), 2u);
  EXPECT_EQ(out_on->scores[0].kind, VolCurveKind::Essvi);
  EXPECT_EQ(out_on->scores[1].kind, VolCurveKind::SplineVol);
  EXPECT_FALSE(out_on->scores[1].disqualified);
  EXPECT_GT(out_on->scores[1].oos_vw, out_on->scores[0].oos_vw)
      << "SplineVol should out-fit Essvi's smooth functional form on the "
         "bumpy board's held-out sample";
  EXPECT_EQ(out_on->chosen.kind, VolCurveKind::SplineVol);
}

// Task I6 review fix: the appended SplineVol candidate must fit through the
// REQUESTER's own SplineFitOpts (grid/lambda/mult_floor/min_obs), not a
// silently-defaulted `SplineFitOpts{}` -- a caller who set
// `spline_candidate=true` alongside a tuned `spline` field would otherwise
// have those knobs dropped on the floor. Prove opts actually carry by
// setting `min_obs` on the requesting config to a count the bumpy fixture's
// even-strike fit split can never reach: with the opts wired through, the
// appended SplineVol candidate fails to produce ANY scorable slice (n_slices
// == 0, unscored); with the (pre-fix) defaulted opts, `min_obs` reverts to
// its default 6 and the SAME fixture fits and wins outright, as
// SplineCandidateFlagAddsCandidate demonstrates just above.
TEST(CurveSelector, SplineCandidateCarriesRequesterFitOpts) {
  const Underlying under = make_bumpy_underlying();
  const SurfaceParityInputs in = bumpy_inputs();

  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  essvi.spline_candidate = true;
  essvi.spline.min_obs = 1000; // unreachable by the fixture's held-in split
  SelectorConfig sel_on;
  sel_on.candidates = {essvi};
  auto out_on = select_curve(under, in, sel_on);
  ASSERT_TRUE(out_on.has_value()) << out_on.error().to_string();
  ASSERT_EQ(out_on->scores.size(), 2u);
  EXPECT_EQ(out_on->scores[1].kind, VolCurveKind::SplineVol);
  EXPECT_EQ(out_on->scores[1].n_slices, 0u)
      << "requester's min_obs=1000 must have reached the appended candidate's "
         "fit -- if it fell back to the default min_obs=6 the fixture fits "
         "cleanly (see SplineCandidateFlagAddsCandidate) and n_slices > 0";
  EXPECT_NE(out_on->chosen.kind, VolCurveKind::SplineVol);
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

  mark.min_worst_frac_within_bidask = 0.50;
  const auto quality_gated_mark = atx::vol::evaluate_surface_admission(evidence, mark);
  EXPECT_FALSE(quality_gated_mark.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(quality_gated_mark,
                                              SurfaceAdmissionReason::DiagnosticsUnavailable));

  FitAdmissionPolicy quote = mark;
  quote.min_worst_frac_within_bidask = 0.0;
  quote.consumer = atx::vol::SurfaceConsumer::Quote;
  const auto quote_decision = atx::vol::evaluate_surface_admission(evidence, quote);
  EXPECT_FALSE(quote_decision.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(quote_decision,
                                              SurfaceAdmissionReason::DiagnosticsUnavailable));

  FitAdmissionPolicy risk = mark;
  risk.consumer = atx::vol::SurfaceConsumer::Risk;
  const auto risk_decision = atx::vol::evaluate_surface_admission(evidence, risk);
  EXPECT_FALSE(risk_decision.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(risk_decision,
                                              SurfaceAdmissionReason::DiagnosticsUnavailable));
}

TEST(FitAdmission, ParityConsumptionCoversConsumerEnablementAndMarkQualityFloor) {
  FitAdmissionPolicy policy;
  policy.consumer = atx::vol::SurfaceConsumer::Mark;
  EXPECT_FALSE(atx::vol::fit_admission_consumes_parity(policy));

  policy.min_worst_frac_within_bidask = 0.50;
  EXPECT_TRUE(atx::vol::fit_admission_consumes_parity(policy));

  policy.min_worst_frac_within_bidask = 0.0;
  policy.consumer = atx::vol::SurfaceConsumer::Quote;
  EXPECT_TRUE(atx::vol::fit_admission_consumes_parity(policy));
  policy.consumer = atx::vol::SurfaceConsumer::Risk;
  EXPECT_TRUE(atx::vol::fit_admission_consumes_parity(policy));

  policy.enabled = false;
  EXPECT_FALSE(atx::vol::fit_admission_consumes_parity(policy));
}

} // namespace
