#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp"        // american_price, AmericanMethod
#include "atx/vol/curve_selector.hpp"  // CandidateScore, select_best_candidate, select_curve
#include "atx/vol/dividend.hpp"        // hybrid_forward, HybridDivParams, DividendEvent
#include "atx/vol/vol_curve.hpp"       // VolCurveKind, CurveConfig

// Task C2.5: unit coverage for the fit-metrics selection policy
// (select_best_candidate). The winner ordering is oos_vw (within
// parsimony_margin) -> reduced-chi-square closest to 1 -> parsimony DoF ->
// higher oos_vw, with butterfly-disqualified families excluded entirely.
// Scores are constructed directly so the policy is tested in isolation from the
// (expensive, fixture-bound) held-out fit machinery.

namespace {

using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::CandidateScore;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::default_selector_candidates;
using atx::vol::detail::slice_butterfly_violations;
using atx::vol::DividendEvent;
using atx::vol::HybridDivParams;
using atx::vol::hybrid_forward;
using atx::vol::select_best_candidate;
using atx::vol::select_curve;
using atx::vol::SelectorConfig;
using atx::vol::Side;
using atx::vol::SplineVolCurve;
using atx::vol::SplineVolParams;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;
using atx::vol::VolCurveKind;

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
  c.expiry_ns = static_cast<std::int64_t>(T * 3.1536e16);  // ACT/365-ish, ns/yr

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
  in.deam.imply_borrow = false;  // borrow fixed at 0 -- see make_bumpy_chain
  in.deam.borrow_fixed = 0.0;
  return in;
}

}  // namespace

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

  SelectorConfig sel_off;  // candidates empty -> default_selector_candidates()
  auto out_off = select_curve(under, in, sel_off);
  ASSERT_TRUE(out_off.has_value()) << out_off.error().to_string();
  ASSERT_EQ(out_off->scores.size(), 5u);

  std::vector<CurveConfig> six = default_selector_candidates();
  ASSERT_EQ(six.size(), 5u);
  six[0].spline_candidate = true;  // opt-in lives on ANY candidate's config
  SelectorConfig sel_on;
  sel_on.candidates = six;
  auto out_on = select_curve(under, in, sel_on);
  ASSERT_TRUE(out_on.has_value()) << out_on.error().to_string();
  ASSERT_EQ(out_on->scores.size(), 6u);  // the 5 + appended SplineVol

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
  essvi.spline.min_obs = 1000;  // unreachable by the fixture's held-in split
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
