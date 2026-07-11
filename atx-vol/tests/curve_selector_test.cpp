#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "atx/vol/curve_selector.hpp"  // CandidateScore, select_best_candidate
#include "atx/vol/vol_curve.hpp"       // VolCurveKind

// Task C2.5: unit coverage for the fit-metrics selection policy
// (select_best_candidate). The winner ordering is oos_vw (within
// parsimony_margin) -> reduced-chi-square closest to 1 -> parsimony DoF ->
// higher oos_vw, with butterfly-disqualified families excluded entirely.
// Scores are constructed directly so the policy is tested in isolation from the
// (expensive, fixture-bound) held-out fit machinery.

namespace {

using atx::vol::CandidateScore;
using atx::vol::select_best_candidate;
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
