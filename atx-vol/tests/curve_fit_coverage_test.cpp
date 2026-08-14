// Task 1 — board-level k-coverage refusal (PrepUncovered).
//
// A ConvexDense slice whose admitted rows are entirely one-sided (the
// 2025-04-24 freshly-listed-daily shape) must be REFUSED into the existing
// slice-drop lane (ExpiryFitOutcome::PrepUncovered -> tenor truncation), not
// fitted and extrapolated. The healthy sibling expiry still fits.
//
// Task 6 (CurveFitCalendarAuthority) — the same predicate, re-used at the OTHER
// end of the driver: a COMMITTED slice that passes it earns full (pre-Task-3)
// calendar-floor authority over the next slice, so a healthy narrow front's
// out-of-support wing gets FLOORED rather than refusing the slice behind it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"       // american_price, AmericanMethod
#include "atx/vol/api/fitting/curve_fit.hpp"      // fit_curve_surface, CurveSurfaceReport
#include "atx/vol/api/pricing/dividend.hpp"       // hybrid_forward, HybridDivParams
#include "atx/vol/api/fitting/surface_parity.hpp" // SurfaceParityInputs, ExpiryFitOutcome
#include "atx/vol/api/core/types.hpp"          // Side
#include "atx/vol/api/marketdata/universe.hpp"       // Underlying, Chain, chain_index
#include "atx/vol/api/fitting/vol_curve.hpp"      // CurveConfig

namespace {

using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::DividendEvent;
using atx::vol::ExpiryFitOutcome;
using atx::vol::fit_curve_surface;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::Side;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;

// Task 6 fixture calibration (see CoveredPrevKeepsFullFloorAuthority).
constexpr double kT6FrontT = 0.05;
constexpr double kT6BackT = 0.25;
constexpr double kT6Sigma0 = 0.15;
constexpr double kT6Skew = 10.0;

[[nodiscard]] double true_sigma(double k) noexcept { return 0.20 + 0.15 * k * k; }
[[nodiscard]] double borrow_of_T(double T) noexcept { return 0.02 + 0.04 * T; }

void size_chain(Chain &c) {
  const std::size_t n = c.strikes.size();
  c.bids.assign(2 * n, 0.0);
  c.asks.assign(2 * n, 0.0);
  c.bid_sizes.assign(2 * n, 0);
  c.ask_sizes.assign(2 * n, 0);
  c.mids.assign(2 * n, 0.0);
  c.ivs.assign(2 * n, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(2 * n, 0);
  c.flags.assign(2 * n, 0);
}

// Accurate Andersen-Lake quote for one (strike, side) leg, 1% half-spreads
// (the curve_fit_carry_fallback_test recipe). void so ASSERT is legal.
void fill_leg(Chain &c, double F, double q_eff, double T, std::size_t i, Side side) {
  const double K = c.strikes[i];
  const double sigma = true_sigma(std::log(K / F));
  const auto px = american_price(kSpot, K, T, sigma, kRate, q_eff, side,
                                 AmericanMethod::AndersenLake, std::nullopt);
  ASSERT_TRUE(px.has_value()) << "american_price failed K=" << K << " T=" << T;
  const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
  c.mids[idx] = *px;
  c.bids[idx] = *px * 0.99;
  c.asks[idx] = *px * 1.01;
  c.bid_sizes[idx] = 1;
  c.ask_sizes[idx] = 1;
}

// Two-sided healthy expiry: every strike carries BOTH legs (co-terminal carry
// pairs everywhere, so the borrow solve is confident and the OTM strip is
// dense on both sides of F).
[[nodiscard]] Chain make_two_sided_chain(double T) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * kYearNs);
  const std::vector<DividendEvent> no_divs;
  const double borrow = borrow_of_T(T);
  const double F = hybrid_forward(kSpot, kRate, borrow, T, no_divs, c.expiry_ns,
                                  /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;
  for (int i = 0; i < 15; ++i) {
    const double k = -0.20 + 0.40 * static_cast<double>(i) / 14.0;
    c.strikes.push_back(F * std::exp(k));
  }
  size_chain(c);
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    fill_leg(c, F, q_eff, T, i, Side::Call);
    fill_leg(c, F, q_eff, T, i, Side::Put);
  }
  return c;
}

// Steep-narrow smile leg: sigma is PER-CHAIN (sigma0 + skew*k^2) rather than
// the board-wide true_sigma() above, because the calendar crossing must exist
// only OUT of the front slice's data-supported band. void so ASSERT is legal
// (same idiom as fill_leg).
void fill_steep_leg(Chain &c, double F, double q_eff, double T, std::size_t i, Side side,
                    double sigma0, double skew) {
  const double K = c.strikes[i];
  const double k = std::log(K / F);
  const double sigma = sigma0 + skew * k * k;
  const auto px = american_price(kSpot, K, T, sigma, kRate, q_eff, side,
                                 AmericanMethod::AndersenLake, std::nullopt);
  ASSERT_TRUE(px.has_value()) << "american_price failed K=" << K << " T=" << T;
  const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
  c.mids[idx] = *px;
  c.bids[idx] = *px * 0.99;
  c.asks[idx] = *px * 1.01;
  c.bid_sizes[idx] = 1;
  c.ask_sizes[idx] = 1;
}

// Narrow but coverage-ADMISSIBLE (straddles ATM with 0.015 spacing, so no
// central gap at all) carrying a steep convex smile: the ConvexDense
// extrapolation beyond |k| ~ 0.09 climbs well above the later slice's own
// wing. This is the calm-day shape the T3 redesign investigation measured on
// 22/35 healthy 2020-03-19 slices.
[[nodiscard]] Chain make_steep_narrow_chain(double T, double sigma0, double skew) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * kYearNs);
  const std::vector<DividendEvent> no_divs;
  const double borrow = borrow_of_T(T);
  const double F = hybrid_forward(kSpot, kRate, borrow, T, no_divs, c.expiry_ns,
                                  /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;
  for (int i = 0; i < 13; ++i) {
    const double k = -0.09 + 0.015 * static_cast<double>(i); // [-0.09, +0.09]
    c.strikes.push_back(F * std::exp(k));
  }
  size_chain(c);
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    fill_steep_leg(c, F, q_eff, T, i, Side::Call, sigma0, skew);
    fill_steep_leg(c, F, q_eff, T, i, Side::Put, sigma0, skew);
  }
  return c;
}

// Freshly-listed-daily shape (the 2025-04-24 seed): quotes exist ONLY below
// the forward, k in [-0.124, -0.069]. Both legs are quoted (so the borrow
// solve has co-terminal pairs); the prefer-OTM heuristic (calib.cpp:119-124)
// admits only the PUT legs to the fit strip, so every admitted row sits left
// of ATM.
[[nodiscard]] Chain make_one_sided_put_chain(double T) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * kYearNs);
  const std::vector<DividendEvent> no_divs;
  const double borrow = borrow_of_T(T);
  const double F = hybrid_forward(kSpot, kRate, borrow, T, no_divs, c.expiry_ns,
                                  /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;
  for (int i = 0; i < 11; ++i) {
    const double k = -0.124 + 0.0055 * static_cast<double>(i);
    c.strikes.push_back(F * std::exp(k));
  }
  size_chain(c);
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    fill_leg(c, F, q_eff, T, i, Side::Put);
    fill_leg(c, F, q_eff, T, i, Side::Call);
  }
  return c;
}

[[nodiscard]] SurfaceParityInputs coverage_inputs() {
  SurfaceParityInputs in{};
  in.S = kSpot;
  in.r = kRate;
  in.deam.imply_borrow = true;
  in.deam.require_carry_confidence = false;
  in.fit_workers = 1; // deterministic
  return in;
}

} // namespace

TEST(CurveFitCoverage, OneSidedThinExpiryIsRefusedAsPrepUncovered) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_one_sided_put_chain(0.04));
  under.chains.push_back(make_two_sided_chain(0.50));

  const auto rep = fit_curve_surface(under, coverage_inputs(), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_EQ(rep->expiry_reports.size(), 2u);
  EXPECT_EQ(rep->expiry_reports[0].outcome, ExpiryFitOutcome::PrepUncovered);
  EXPECT_EQ(rep->expiry_reports[1].outcome, ExpiryFitOutcome::Fitted);
  EXPECT_EQ(rep->n_slices, 1u);
  EXPECT_EQ(rep->n_slices_uncovered, 1u);
}

TEST(CurveFitCoverage, WellCoveredBoardIsUntouched) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_two_sided_chain(0.25));
  under.chains.push_back(make_two_sided_chain(0.50));

  const auto rep = fit_curve_surface(under, coverage_inputs(), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  EXPECT_EQ(rep->n_slices, 2u);
  EXPECT_EQ(rep->n_slices_uncovered, 0u);
  for (const auto &er : rep->expiry_reports) {
    EXPECT_EQ(er.outcome, ExpiryFitOutcome::Fitted);
  }
}

// ── T10b (plan D5) — the served path can now REPORT what its fits did ──────
//
// T10 built the `FitDiag` out-param and stated plainly that no production call
// site passed one, so D5 stayed unobservable. These pin the sink: the driver
// passes a real struct and parks the result on the public report.

TEST(CurveFitDiagnostics, EveryCommittedSliceCarriesAFitDiagnostic) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_two_sided_chain(0.25));
  under.chains.push_back(make_two_sided_chain(0.50));

  const auto rep = fit_curve_surface(under, coverage_inputs(), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_EQ(rep->n_slices, 2u);

  // Parallel to `context` / `per_expiry` is the whole contract — a consumer
  // indexes all three together, so a length mismatch is a silent misattribution
  // of one slice's fit to another slice's curve.
  ASSERT_EQ(rep->slice_diagnostics.size(), rep->context.size());
  ASSERT_EQ(rep->slice_diagnostics.size(), rep->per_expiry.size());

  for (std::size_t i = 0; i < rep->slice_diagnostics.size(); ++i) {
    const auto &sd = rep->slice_diagnostics[i];
    // The recorded family must be the SERVED curve's, not the configured one.
    EXPECT_EQ(sd.kind, rep->surface.slices()[i]->kind());
    EXPECT_DOUBLE_EQ(sd.maturity, rep->context[i].T);
    // Non-vacuous: the diagnostic must carry real fit content, not a
    // default-constructed struct that merely occupies the slot.
    EXPECT_GT(sd.diag.n_quotes_used, 0u);
  }
}

// The default ConvexDense board is the one family that fails closed, so a
// returned fit is certified — it must say `Converged` and carry an ENGAGED
// optimality residual. This is the assertion that would catch the sink being
// wired to a struct nobody populates.
TEST(CurveFitDiagnostics, ConvexDenseReportsACertifiedTerminationAndGradient) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_two_sided_chain(0.25));
  under.chains.push_back(make_two_sided_chain(0.50));

  CurveConfig cfg{};
  cfg.kind = atx::vol::VolCurveKind::ConvexDense;
  const auto rep = fit_curve_surface(under, coverage_inputs(), cfg);
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_FALSE(rep->slice_diagnostics.empty());

  for (const auto &sd : rep->slice_diagnostics) {
    EXPECT_EQ(sd.kind, atx::vol::VolCurveKind::ConvexDense);
    EXPECT_EQ(sd.diag.termination, atx::vol::FitTermination::Converged);
    ASSERT_TRUE(sd.diag.final_grad_norm.has_value())
        << "a disengaged gradient here means the sink is inert";
    EXPECT_GE(*sd.diag.final_grad_norm, 0.0);
    // COLD entry point: `fit_slice_curve` takes no prior curve, so a warm-start
    // claim here would be false.
    EXPECT_FALSE(sd.diag.warm_started);
  }
}

// ── T10b (plan D4) — the served reduced chi-square uses the fitted family's
// own dof, not a hardcoded 3 ────────────────────────────────────────────────

// chi2_reduced is chi2/(N - dof). Hardcoding dof = 3 was right only for eSSVI
// and inflated every other family's denominator, making the SERVED quality
// number systematically optimistic. Raw SVI carries 5.
TEST(CurveFitDiagnostics, ServedChiSquareIsScoredAgainstTheFittedFamilysOwnDof) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_two_sided_chain(0.25));
  under.chains.push_back(make_two_sided_chain(0.50));

  CurveConfig cfg{};
  cfg.kind = atx::vol::VolCurveKind::Svi;
  const auto rep = fit_curve_surface(under, coverage_inputs(), cfg);
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_FALSE(rep->slice_diagnostics.empty());

  for (std::size_t i = 0; i < rep->slice_diagnostics.size(); ++i) {
    const auto &sd = rep->slice_diagnostics[i];
    ASSERT_EQ(sd.kind, atx::vol::VolCurveKind::Svi);
    // The curve's own dof, and specifically NOT the retired constant 3.
    EXPECT_EQ(sd.chi2_dof, rep->surface.slices()[i]->dof());
    EXPECT_EQ(sd.chi2_dof, 5u);
    EXPECT_NE(sd.chi2_dof, 3u);
    // A 5-parameter fit on this board has ample rows, so the statistic is
    // well-posed and must NOT be blanked.
    EXPECT_FALSE(sd.chi2_dof_underdetermined);
    EXPECT_GT(rep->per_expiry[i].chi2_reduced, 0.0);
  }
}

// The other half, and the one with teeth. An INTERPOLATING family's dof is its
// node count, so N <= dof by construction and the reduced chi-square is
// genuinely undefined. What must NOT happen is losing the BAND evidence that
// shares the same ParityReport: it does not depend on dof, and losing it is not
// free —
// session.cpp averages over every per_expiry entry and mins into `worst`, so a
// dropped report publishes "reprices 0% in band" while parity_state stays Valid.
// Measured cost of getting this wrong on lqbench: 9 of 240 boards, corpus
// mean_in_band 0.9652 -> 0.9293.
TEST(CurveFitDiagnostics, UnderdeterminedChiSquareIsFlaggedAndBandEvidenceSurvives) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_two_sided_chain(0.25));
  under.chains.push_back(make_two_sided_chain(0.50));

  CurveConfig cfg{};
  cfg.kind = atx::vol::VolCurveKind::LinearVariance;
  const auto rep = fit_curve_surface(under, coverage_inputs(), cfg);
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_FALSE(rep->slice_diagnostics.empty());
  ASSERT_EQ(rep->slice_diagnostics.size(), rep->per_expiry.size());

  bool saw_underdetermined = false;
  for (std::size_t i = 0; i < rep->slice_diagnostics.size(); ++i) {
    const auto &sd = rep->slice_diagnostics[i];
    const auto &par = rep->per_expiry[i];
    EXPECT_EQ(sd.chi2_dof, rep->surface.slices()[i]->dof());
    if (!sd.chi2_dof_underdetermined) {
      continue;
    }
    saw_underdetermined = true;
    // chi2/(N - dof) has no positive denominator here, so what is published is
    // chi2/N and the FLAG is what says so. It must NOT be blanked to zero: an
    // exact zero chi-square reads as a PERFECT fit, and zeroing it re-creates the
    // W3-A all-zero diagnostics blackout on exactly this route
    // (pricer_fitter_test's AutoRoutedLinearVarianceMarkAlwaysScoresParity
    // asserts mean_chi2_reduced > 0 for an auto-routed LinearVariance Mark).
    EXPECT_TRUE(std::isfinite(par.chi2_reduced));
    EXPECT_GE(par.chi2_reduced, 0.0);
    // The band evidence is fully measured and MUST survive. These are
    // the assertions that fail if the dof-0 re-score is removed: without it the
    // whole ParityReport is dropped and n collapses to 0.
    //
    // NOT asserted here: rmse_mid_vol > 0. This fixture is noiseless, and a
    // LinearVariance curve INTERPOLATES its nodes, so it reproduces the market
    // vol exactly and a zero RMSE is the correct answer rather than a missing
    // measurement. `n` and the in-band fraction are what separate the two.
    EXPECT_GT(par.n, 0u);
    EXPECT_GT(par.frac_fv_within_bidask, 0.0);
    EXPECT_TRUE(std::isfinite(par.rmse_mid_vol));
  }
  // Fixture guard: an interpolating family on this board must actually REACH
  // the under-determined case, or the assertions above never ran and this test
  // silently proves nothing.
  ASSERT_TRUE(saw_underdetermined)
      << "fixture no longer exercises the N <= dof path; the test is vacuous";
}

// ── Task 6 — D1: a coverage-admissible committed prev keeps FULL floor
// authority. The calm-day shape (narrow COVERED front whose extrapolated wing
// overshoots the next slice's bare fit at out-of-support k) must be FLOORED
// (the exact pre-Task-3 QP: floor rows at every lattice node), not refused.
// Under the unconditional Task 3 gate this board truncates to one slice with
// n_slice_calendar_unsupported == 1.
//
// Fixture calibration contract (mirrors Task 3's fixture discipline): the
// PRECONDITION self-check below pins the geometry the red run depends on — the
// front's extrapolated wing must exceed the back's BARE (unfloored) wing at
// k=+0.60 and NOWHERE inside the front's floor-support band (its data range
// [-0.09,+0.09] widened by kCalendarFloorSupportMargin = 0.10). If the geometry
// stops cooperating, raise kT6Skew first (then widen the T ratio); never relax
// the assertions. Measured at these values (2026-08-07, clang-cl release):
// front w(+0.60) = 0.023632 vs back bare 0.021650 (dw = +1.98e-03, worst
// out-of-support dw = +4.83e-03 at k = +0.4313 — squarely in the calm-day
// 5.6e-03..2.3e-02 range the investigation measured on 2020-03-19); in-band
// headroom min(w_back - w_front) over |k| <= 0.19 = +4.31e-03.
TEST(CurveFitCalendarAuthority, CoveredPrevKeepsFullFloorAuthority) {
  Underlying under;
  under.spot = kSpot;
  // Front: covered, narrow, steep smile -> wing extrapolation overshoots.
  under.chains.push_back(make_steep_narrow_chain(kT6FrontT, kT6Sigma0, kT6Skew));
  // Back: ordinary smile over the full lattice width.
  under.chains.push_back(make_two_sided_chain(kT6BackT));

  // PRECONDITION: fit each chain ALONE (no calendar floor in play) and confirm
  // the front's extrapolated wing really does cross the back's bare fit, and
  // that it does so ONLY outside the front's floor-support band. Without this
  // the test cannot distinguish the Task 3 gate from the D1 gate.
  {
    Underlying front_only;
    front_only.spot = kSpot;
    front_only.chains.push_back(make_steep_narrow_chain(kT6FrontT, kT6Sigma0, kT6Skew));
    const auto front_rep = fit_curve_surface(front_only, coverage_inputs(), CurveConfig{});
    ASSERT_TRUE(front_rep.has_value()) << front_rep.error().to_string();
    ASSERT_EQ(front_rep->n_slices, 1u);
    Underlying back_only;
    back_only.spot = kSpot;
    back_only.chains.push_back(make_two_sided_chain(kT6BackT));
    const auto back_rep = fit_curve_surface(back_only, coverage_inputs(), CurveConfig{});
    ASSERT_TRUE(back_rep.has_value()) << back_rep.error().to_string();
    ASSERT_EQ(back_rep->n_slices, 1u);
    const auto &fb = *front_rep->surface.slices()[0];
    const auto &bb = *back_rep->surface.slices()[0];
    ASSERT_GT(fb.w(0.60), bb.w(0.60) + 1.0e-7)
        << "front wing no longer overshoots the back's bare wing at k=+0.60: "
           "nothing for the calendar floor to do out of support (raise kT6Skew). "
           "front="
        << fb.w(0.60) << " back=" << bb.w(0.60);
    // ... and NOT inside the floor-support band [-0.19, +0.19], so the only
    // thing the Task 3 gate can object to is the out-of-support wing.
    double in_band_headroom = std::numeric_limits<double>::infinity();
    double worst_in_band_k = 0.0;
    for (int gi = 0; gi <= 64; ++gi) {
      const double k = -0.60 + 0.01875 * static_cast<double>(gi);
      if (std::abs(k) > 0.19) {
        continue;
      }
      const double head = bb.w(k) - fb.w(k);
      if (head < in_band_headroom) {
        in_band_headroom = head;
        worst_in_band_k = k;
      }
    }
    ASSERT_GT(in_band_headroom, 0.0)
        << "front exceeds the back INSIDE the support band at k=" << worst_in_band_k
        << ": not an out-of-support-only fixture (lower kT6Sigma0)";
  }

  const auto rep = fit_curve_surface(under, coverage_inputs(), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  // D1: BOTH slices commit; nothing is refused.
  EXPECT_EQ(rep->n_slices, 2u);
  EXPECT_EQ(rep->n_slice_calendar_unsupported, 0u);
  ASSERT_EQ(rep->expiry_reports.size(), 2u);
  EXPECT_EQ(rep->expiry_reports[0].outcome, ExpiryFitOutcome::Fitted);
  EXPECT_EQ(rep->expiry_reports[1].outcome, ExpiryFitOutcome::Fitted);
  // And the served surface is calendar-clean INCLUDING the wings: the floor
  // rows exist everywhere (this is what a skip-not-refuse regression breaks).
  ASSERT_EQ(rep->surface.n_slices(), 2u);
  const auto &front = *rep->surface.slices()[0];
  const auto &back = *rep->surface.slices()[1];
  for (int gi = 0; gi <= 64; ++gi) {
    const double k = -0.60 + 0.01875 * static_cast<double>(gi);
    EXPECT_GE(back.w(k), front.w(k) - 1.0e-7) << "calendar crossing at k=" << k;
  }
}
