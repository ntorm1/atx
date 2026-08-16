#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "atx/vol/api/fitting/calib.hpp"         // FitObs
#include "atx/vol/api/fitting/vol_surface.hpp"   // essvi_backbone_w, essvi_phi_max
#include "fitting/essvi_anchored.hpp"            // the unit under test

// Anchored eSSVI calibrator coverage (Corbetta et al. 2019, arXiv:1804.04924).
//
// The fixtures are generated from KNOWN (theta, psi, rho) slices: observations
// carry the exact model total variance, so a faithful calibration must recover
// the generating parameters. The arbitrage tests do NOT trust the analytic
// interval algebra on its own — they re-derive both butterfly conditions and the
// three calendar conditions numerically on dense grids, which is what makes
// "arbitrage-free by construction" a checked claim rather than a comment.

namespace {

using atx::vol::AnchoredDiag;
using atx::vol::anchored_butterfly_ok;
using atx::vol::anchored_calendar_ok;
using atx::vol::anchored_fit_sequence;
using atx::vol::anchored_fit_slice;
using atx::vol::anchored_interpolate;
using atx::vol::AnchoredOpts;
using atx::vol::anchored_psi_bounds;
using atx::vol::AnchoredSlice;
using atx::vol::AnchoredSliceOrigin;
using atx::vol::AnchoredSliceRequest;
using atx::vol::anchored_to_essvi;
using atx::vol::anchored_w;
using atx::vol::ErrorCode;
using atx::vol::essvi_backbone_w;
using atx::vol::EssviParams;
using atx::vol::FitObs;

constexpr double kT = 0.25;
constexpr double kF = 100.0;

[[nodiscard]] AnchoredSlice make_slice(double theta, double psi, double rho,
                                       double T = kT) {
  AnchoredSlice s{};
  s.theta = theta;
  s.psi = psi;
  s.rho = rho;
  s.k_star = 0.0;
  s.theta_star = theta;
  s.T = T;
  s.F = kF;
  return s;
}

// Reference evaluator, written independently of the implementation: the eSSVI
// backbone in (theta, phi, rho) with phi = psi/theta.
[[nodiscard]] double ref_w(double theta, double psi, double rho, double k) {
  const double phi = psi / theta;
  const double pk = phi * k;
  const double inner = (pk + rho) * (pk + rho) + (1.0 - rho * rho);
  return 0.5 * theta * (1.0 + rho * pk + std::sqrt(inner));
}

// Observations on a symmetric log-moneyness grid carrying the exact model w.
[[nodiscard]] std::vector<FitObs> make_obs(const AnchoredSlice& s, std::size_t n,
                                           double k_half_width = 0.30) {
  std::vector<FitObs> obs;
  obs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double u = (n == 1u) ? 0.0
                               : (2.0 * static_cast<double>(i) /
                                      static_cast<double>(n - 1u) -
                                  1.0);
    FitObs o{};
    o.k = u * k_half_width;
    o.w_mkt = ref_w(s.theta, s.psi, s.rho, o.k);
    o.sigma_mkt = std::sqrt(o.w_mkt / s.T);
    o.weight_w = 1.0;
    o.active_weight_w = 1.0;
    o.F = kF;
    o.K = kF * std::exp(o.k);
    obs.push_back(o);
  }
  return obs;
}

// Butterfly, checked from the definition rather than from the psi algebra: the
// Gatheral (2004) g-function must stay non-negative, which is equivalent to a
// non-negative risk-neutral density.
[[nodiscard]] double g_function(const AnchoredSlice& s, double k) {
  const double h = 1.0e-4;
  const double w = anchored_w(s, k);
  const double wp = (anchored_w(s, k + h) - anchored_w(s, k - h)) / (2.0 * h);
  const double wpp =
      (anchored_w(s, k + h) - 2.0 * w + anchored_w(s, k - h)) / (h * h);
  const double t1 = 1.0 - k * wp / (2.0 * w);
  return t1 * t1 - 0.25 * wp * wp * (1.0 / w + 0.25) + 0.5 * wpp;
}

[[nodiscard]] double min_g(const AnchoredSlice& s, double k_max = 2.0,
                           int n = 401) {
  double lo = std::numeric_limits<double>::infinity();
  for (int i = 0; i < n; ++i) {
    const double k =
        -k_max + 2.0 * k_max * static_cast<double>(i) / static_cast<double>(n - 1);
    lo = std::min(lo, g_function(s, k));
  }
  return lo;
}

// ── Anchoring ────────────────────────────────────────────────────────────

TEST(AnchoredEssviParameterization, WAtZeroIsThetaAndSlopeAtZeroIsChi) {
  const AnchoredSlice s = make_slice(0.04, 0.30, -0.35);
  EXPECT_NEAR(anchored_w(s, 0.0), s.theta, 1.0e-14);
  const double h = 1.0e-6;
  const double slope = (anchored_w(s, h) - anchored_w(s, -h)) / (2.0 * h);
  EXPECT_NEAR(slope, s.chi(), 1.0e-6);
}

TEST(AnchoredEssviParameterization, MatchesTheProductionBackboneEvaluator) {
  const AnchoredSlice s = make_slice(0.0625, 0.42, 0.18);
  const EssviParams p = anchored_to_essvi(s);
  for (double k = -1.0; k <= 1.0; k += 0.05) {
    EXPECT_NEAR(anchored_w(s, k), essvi_backbone_w(p, k), 1.0e-12) << "k=" << k;
  }
}

TEST(AnchoredEssviParameterization, AnchorReproducesTheAtmObservationExactlyAtKStarZero) {
  // With the anchor AT the money the relation theta = theta* - rho*psi*k* is
  // exact, so the calibrated slice must pass through the anchor observation.
  const AnchoredSlice truth = make_slice(0.04, 0.30, -0.40);
  const std::vector<FitObs> obs = make_obs(truth, 11);
  AnchoredDiag diag{};
  const auto fit = anchored_fit_slice(obs, kT, kF, AnchoredOpts{}, nullptr, &diag);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_NEAR(diag.k_star, 0.0, 1.0e-15);
  EXPECT_NEAR(anchored_w(*fit, 0.0), diag.theta_star, 1.0e-12);
}

// ── The feasible interval ────────────────────────────────────────────────

TEST(AnchoredEssviInterval, UpperEndpointSaturatesTheButterflyBound) {
  const double k_star = 0.0;
  const double theta_star = 0.04;
  for (const double rho : {-0.9, -0.5, 0.0, 0.5, 0.9}) {
    const auto iv = anchored_psi_bounds(rho, k_star, theta_star, nullptr);
    ASSERT_FALSE(iv.empty()) << "rho=" << rho;
    // At k* = 0 the anchor is inert, so theta == theta* and the binding bound
    // is exactly essvi_phi_max scaled by theta.
    const double psi_max =
        theta_star * atx::vol::essvi_phi_max(theta_star, rho);
    EXPECT_NEAR(iv.hi, psi_max, 1.0e-12) << "rho=" << rho;
  }
}

TEST(AnchoredEssviInterval, EveryInteriorPointIsButterflyFree) {
  for (const double k_star : {-0.05, 0.0, 0.03}) {
    for (const double rho : {-0.95, -0.4, 0.0, 0.4, 0.95}) {
      const auto iv = anchored_psi_bounds(rho, k_star, 0.04, nullptr);
      ASSERT_FALSE(iv.empty()) << "rho=" << rho;
      for (int i = 1; i <= 20; ++i) {
        const double psi =
            iv.lo + (iv.hi - iv.lo) * static_cast<double>(i) / 20.0;
        AnchoredSlice s = make_slice(0.04 - rho * psi * k_star, psi, rho);
        s.k_star = k_star;
        s.theta_star = 0.04;
        ASSERT_TRUE(anchored_butterfly_ok(s))
            << "rho=" << rho << " psi=" << psi << " k*=" << k_star;
        EXPECT_GE(min_g(s), -1.0e-6) << "rho=" << rho << " psi=" << psi;
      }
    }
  }
}

TEST(AnchoredEssviInterval, PreviousSliceNarrowsTheIntervalAndKeepsItCalendarFeasible) {
  const AnchoredSlice prev = make_slice(0.020, 0.25, -0.30, 0.08);
  for (const double rho : {-0.9, -0.3, 0.0, 0.3, 0.9}) {
    const auto free_iv = anchored_psi_bounds(rho, 0.0, 0.04, nullptr);
    const auto cal_iv = anchored_psi_bounds(rho, 0.0, 0.04, &prev);
    ASSERT_FALSE(free_iv.empty());
    EXPECT_GE(cal_iv.lo, free_iv.lo - 1.0e-15) << "rho=" << rho;
    EXPECT_LE(cal_iv.hi, free_iv.hi + 1.0e-15) << "rho=" << rho;
    if (cal_iv.empty()) {
      continue;
    }
    for (int i = 0; i <= 10; ++i) {
      const double psi =
          cal_iv.lo + (cal_iv.hi - cal_iv.lo) * static_cast<double>(i) / 10.0;
      const AnchoredSlice next = make_slice(0.04, psi, rho, 0.25);
      EXPECT_TRUE(anchored_calendar_ok(prev, next))
          << "rho=" << rho << " psi=" << psi;
      EXPECT_TRUE(anchored_butterfly_ok(next)) << "rho=" << rho << " psi=" << psi;
    }
  }
}

// ── Calibration ──────────────────────────────────────────────────────────

TEST(AnchoredEssviCalibration, RecoversAKnownSliceWithoutAnyStartingPoint) {
  const AnchoredSlice truth = make_slice(0.04, 0.28, -0.45);
  const std::vector<FitObs> obs = make_obs(truth, 21);
  const auto fit = anchored_fit_slice(obs, kT, kF, AnchoredOpts{});
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  for (double k = -0.30; k <= 0.30; k += 0.01) {
    EXPECT_NEAR(anchored_w(*fit, k), ref_w(truth.theta, truth.psi, truth.rho, k),
                5.0e-6)
        << "k=" << k;
  }
}

TEST(AnchoredEssviCalibration, IsBitwiseDeterministic) {
  const AnchoredSlice truth = make_slice(0.0225, 0.25, 0.22);
  const std::vector<FitObs> obs = make_obs(truth, 13);
  const auto a = anchored_fit_slice(obs, kT, kF, AnchoredOpts{});
  const auto b = anchored_fit_slice(obs, kT, kF, AnchoredOpts{});
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(a->theta, b->theta);
  EXPECT_EQ(a->psi, b->psi);
  EXPECT_EQ(a->rho, b->rho);
}

TEST(AnchoredEssviCalibration, FitsAThreeQuoteSliceTheLmPathCannotSupport) {
  // Two free parameters; three observations leave one degree of freedom. The
  // LM path's `min_obs_per_slice` is 4 and prepared fitting refuses below 5.
  const AnchoredSlice truth = make_slice(0.04, 0.30, -0.30);
  const std::vector<FitObs> obs = make_obs(truth, 3, 0.20);
  AnchoredDiag diag{};
  const auto fit = anchored_fit_slice(obs, kT, kF, AnchoredOpts{}, nullptr, &diag);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_EQ(diag.n_obs, 3u);
  EXPECT_TRUE(anchored_butterfly_ok(*fit));
  for (const FitObs& o : obs) {
    EXPECT_NEAR(anchored_w(*fit, o.k), o.w_mkt, 5.0e-5) << "k=" << o.k;
  }
}

TEST(AnchoredEssviCalibration, FitsASingleQuoteSlice) {
  const AnchoredSlice truth = make_slice(0.04, 0.30, -0.30);
  const std::vector<FitObs> obs = make_obs(truth, 1);
  const auto fit = anchored_fit_slice(obs, kT, kF, AnchoredOpts{});
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_TRUE(anchored_butterfly_ok(*fit));
}

TEST(AnchoredEssviCalibration, EveryCalibratedSliceIsButterflyFreeByConstruction) {
  // psi = 0.60 at theta = 0.04 is DELIBERATELY past the butterfly bound for
  // every rho below: the generating "market" is itself arbitrageable. A fit that
  // merely reproduced its input would fail this test; the constrained sweep
  // cannot, because no inadmissible psi is reachable.
  for (const double rho : {-0.85, -0.35, 0.0, 0.35, 0.85}) {
    for (const double psi : {0.10, 0.30, 0.60}) {
      const AnchoredSlice truth = make_slice(0.04, psi, rho);
      const std::vector<FitObs> obs = make_obs(truth, 15);
      const auto fit = anchored_fit_slice(obs, kT, kF, AnchoredOpts{});
      ASSERT_TRUE(fit.has_value()) << "rho=" << rho << " psi=" << psi;
      EXPECT_TRUE(anchored_butterfly_ok(*fit)) << "rho=" << rho << " psi=" << psi;
      EXPECT_GE(min_g(*fit), -1.0e-6) << "rho=" << rho << " psi=" << psi;
    }
  }
}

TEST(AnchoredEssviCalibration, CalendarConstrainedFitCannotCrossThePreviousSlice) {
  // A previous slice deliberately RICHER than the data wants, so the calendar
  // interval genuinely binds.
  const AnchoredSlice prev = make_slice(0.030, 0.297, -0.10, 0.10);
  const AnchoredSlice truth = make_slice(0.032, 0.15, -0.30, 0.25);
  const std::vector<FitObs> obs = make_obs(truth, 15);
  const auto fit = anchored_fit_slice(obs, 0.25, kF, AnchoredOpts{}, &prev);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_TRUE(anchored_calendar_ok(prev, *fit));
  EXPECT_TRUE(anchored_butterfly_ok(*fit));
}

TEST(AnchoredEssviCalibration, RejectsEmptyObservationsAndNonPositiveT) {
  const std::vector<FitObs> empty;
  EXPECT_EQ(anchored_fit_slice(empty, kT, kF, AnchoredOpts{}).error().code(),
            ErrorCode::InvalidArgument);
  const AnchoredSlice truth = make_slice(0.04, 0.30, -0.30);
  const std::vector<FitObs> obs = make_obs(truth, 5);
  EXPECT_EQ(anchored_fit_slice(obs, 0.0, kF, AnchoredOpts{}).error().code(),
            ErrorCode::InvalidArgument);
}

TEST(AnchoredEssviCalibration, ObjectiveEvaluationCountIsBoundedAndDataIndependent) {
  const AnchoredOpts opts{};
  const std::uint32_t cap =
      static_cast<std::uint32_t>(opts.n_rho) *
      static_cast<std::uint32_t>(1u + opts.n_refine_passes) *
      static_cast<std::uint32_t>(opts.brent_max_iter + 3u);
  for (const std::size_t n : {3u, 9u, 40u}) {
    const AnchoredSlice truth = make_slice(0.04, 0.30, -0.30);
    const std::vector<FitObs> obs = make_obs(truth, n);
    AnchoredDiag diag{};
    const auto fit = anchored_fit_slice(obs, kT, kF, opts, nullptr, &diag);
    ASSERT_TRUE(fit.has_value());
    EXPECT_LE(diag.n_objective_evals, cap) << "n=" << n;
  }
}

// ── Interpolation ────────────────────────────────────────────────────────

TEST(AnchoredEssviInterpolation, EndpointsReproduceTheInputSlices) {
  const AnchoredSlice lo = make_slice(0.010, 0.15, -0.30, 0.05);
  const AnchoredSlice hi = make_slice(0.040, 0.30, -0.10, 0.50);
  const auto at_lo = anchored_interpolate(lo, hi, lo.T);
  const auto at_hi = anchored_interpolate(lo, hi, hi.T);
  ASSERT_TRUE(at_lo.has_value());
  ASSERT_TRUE(at_hi.has_value());
  EXPECT_NEAR(at_lo->theta, lo.theta, 1.0e-15);
  EXPECT_NEAR(at_lo->psi, lo.psi, 1.0e-15);
  EXPECT_NEAR(at_lo->rho, lo.rho, 1.0e-15);
  EXPECT_NEAR(at_hi->theta, hi.theta, 1.0e-15);
  EXPECT_NEAR(at_hi->psi, hi.psi, 1.0e-15);
  EXPECT_NEAR(at_hi->rho, hi.rho, 1.0e-15);
}

TEST(AnchoredEssviInterpolation, IsArbitrageFreeOnADenseGridOfIntermediateMaturities) {
  const AnchoredSlice lo = make_slice(0.010, 0.14, -0.60, 0.05);
  const AnchoredSlice hi = make_slice(0.060, 0.40, 0.30, 0.75);
  ASSERT_TRUE(anchored_butterfly_ok(lo));
  ASSERT_TRUE(anchored_butterfly_ok(hi));
  ASSERT_TRUE(anchored_calendar_ok(lo, hi));

  std::vector<AnchoredSlice> chain;
  chain.push_back(lo);
  for (int i = 1; i < 64; ++i) {
    const double T = lo.T + (hi.T - lo.T) * static_cast<double>(i) / 64.0;
    const auto mid = anchored_interpolate(lo, hi, T);
    ASSERT_TRUE(mid.has_value()) << "T=" << T;
    EXPECT_TRUE(anchored_butterfly_ok(*mid)) << "T=" << T;
    EXPECT_GE(min_g(*mid), -1.0e-6) << "T=" << T;
    chain.push_back(*mid);
  }
  chain.push_back(hi);
  // Calendar between EVERY ordered pair, not just adjacent ones.
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    for (std::size_t j = i + 1; j < chain.size(); ++j) {
      EXPECT_TRUE(anchored_calendar_ok(chain[i], chain[j]))
          << "i=" << i << " j=" << j;
    }
  }
}

TEST(AnchoredEssviInterpolation, RefusesEveryMaturityOutsideTheBracket) {
  const AnchoredSlice lo = make_slice(0.010, 0.15, -0.30, 0.10);
  const AnchoredSlice hi = make_slice(0.040, 0.30, -0.10, 0.50);
  EXPECT_EQ(anchored_interpolate(lo, hi, 0.05).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(anchored_interpolate(lo, hi, 0.75).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(anchored_interpolate(lo, hi, -1.0).error().code(),
            ErrorCode::InvalidArgument);
}

TEST(AnchoredEssviInterpolation, RefusesEndpointsThatAreNotThemselvesArbitrageFree) {
  const AnchoredSlice lo = make_slice(0.040, 0.30, -0.10, 0.50);
  const AnchoredSlice hi = make_slice(0.010, 0.15, -0.30, 0.90);  // theta inverts
  EXPECT_EQ(anchored_interpolate(lo, hi, 0.70).error().code(),
            ErrorCode::InvalidArgument);
}

// ── Sequential driver ────────────────────────────────────────────────────

TEST(AnchoredEssviSequence, InterpolatesAThinExpiryBracketedByTwoCalibratedOnes) {
  const AnchoredSlice s0 = make_slice(0.010, 0.14, -0.40, 0.08);
  const AnchoredSlice s1 = make_slice(0.025, 0.23, -0.30, 0.25);
  const AnchoredSlice s2 = make_slice(0.050, 0.36, -0.20, 0.60);
  const std::vector<FitObs> o0 = make_obs(s0, 15);
  const std::vector<FitObs> o1 = make_obs(s1, 2);  // starved
  const std::vector<FitObs> o2 = make_obs(s2, 15);

  const std::vector<AnchoredSliceRequest> reqs{
      AnchoredSliceRequest{o0, s0.T, kF, true},
      AnchoredSliceRequest{o1, s1.T, kF, false},
      AnchoredSliceRequest{o2, s2.T, kF, true},
  };
  const auto res = anchored_fit_sequence(reqs, AnchoredOpts{});
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), 3u);
  EXPECT_EQ((*res)[0].origin, AnchoredSliceOrigin::Calibrated);
  EXPECT_EQ((*res)[1].origin, AnchoredSliceOrigin::Interpolated);
  EXPECT_EQ((*res)[2].origin, AnchoredSliceOrigin::Calibrated);
  EXPECT_NEAR((*res)[1].slice.T, s1.T, 1.0e-15);
  EXPECT_TRUE(anchored_butterfly_ok((*res)[1].slice));
  EXPECT_TRUE(anchored_calendar_ok((*res)[0].slice, (*res)[1].slice));
  EXPECT_TRUE(anchored_calendar_ok((*res)[1].slice, (*res)[2].slice));
}

TEST(AnchoredEssviSequence, NeverExtrapolatesAThinExpiryOutsideTheCalibratedDomain) {
  const AnchoredSlice s0 = make_slice(0.010, 0.20, -0.40, 0.08);
  const AnchoredSlice s1 = make_slice(0.025, 0.30, -0.30, 0.25);
  const AnchoredSlice s2 = make_slice(0.050, 0.40, -0.20, 0.60);
  const std::vector<FitObs> o0 = make_obs(s0, 2);  // starved, BEFORE the domain
  const std::vector<FitObs> o1 = make_obs(s1, 15);
  const std::vector<FitObs> o2 = make_obs(s2, 15);
  const AnchoredSlice s3 = make_slice(0.090, 0.48, -0.10, 1.20);
  const std::vector<FitObs> o3 = make_obs(s3, 2);  // starved, AFTER the domain

  const std::vector<AnchoredSliceRequest> reqs{
      AnchoredSliceRequest{o0, s0.T, kF, false},
      AnchoredSliceRequest{o1, s1.T, kF, true},
      AnchoredSliceRequest{o2, s2.T, kF, true},
      AnchoredSliceRequest{o3, s3.T, kF, false},
  };
  const auto res = anchored_fit_sequence(reqs, AnchoredOpts{});
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), 4u);
  EXPECT_EQ((*res)[0].origin, AnchoredSliceOrigin::Dropped);
  EXPECT_EQ((*res)[1].origin, AnchoredSliceOrigin::Calibrated);
  EXPECT_EQ((*res)[2].origin, AnchoredSliceOrigin::Calibrated);
  EXPECT_EQ((*res)[3].origin, AnchoredSliceOrigin::Dropped);
}

TEST(AnchoredEssviSequence, WholeBoardIsCalendarAndButterflyFree) {
  std::vector<AnchoredSlice> truth;
  truth.push_back(make_slice(0.006, 0.10, -0.55, 0.04));
  truth.push_back(make_slice(0.014, 0.16, -0.45, 0.10));
  truth.push_back(make_slice(0.028, 0.24, -0.35, 0.25));
  truth.push_back(make_slice(0.050, 0.32, -0.25, 0.50));
  truth.push_back(make_slice(0.085, 0.42, -0.15, 1.00));

  std::vector<std::vector<FitObs>> store;
  store.reserve(truth.size());
  for (std::size_t i = 0; i < truth.size(); ++i) {
    store.push_back(make_obs(truth[i], (i == 2u) ? 2u : 17u));
  }
  std::vector<AnchoredSliceRequest> reqs;
  for (std::size_t i = 0; i < truth.size(); ++i) {
    reqs.push_back(
        AnchoredSliceRequest{store[i], truth[i].T, kF, i != 2u});
  }

  const auto res = anchored_fit_sequence(reqs, AnchoredOpts{});
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  std::vector<AnchoredSlice> served;
  for (const auto& r : *res) {
    ASSERT_NE(r.origin, AnchoredSliceOrigin::Dropped);
    EXPECT_TRUE(anchored_butterfly_ok(r.slice));
    EXPECT_GE(min_g(r.slice), -1.0e-6);
    served.push_back(r.slice);
  }
  for (std::size_t i = 0; i + 1 < served.size(); ++i) {
    for (std::size_t j = i + 1; j < served.size(); ++j) {
      EXPECT_TRUE(anchored_calendar_ok(served[i], served[j]))
          << "i=" << i << " j=" << j;
    }
  }
}

TEST(AnchoredEssviSequence, RejectsOutOfOrderMaturities) {
  const AnchoredSlice s0 = make_slice(0.010, 0.14, -0.40, 0.30);
  const AnchoredSlice s1 = make_slice(0.025, 0.23, -0.30, 0.10);
  const std::vector<FitObs> o0 = make_obs(s0, 9);
  const std::vector<FitObs> o1 = make_obs(s1, 9);
  const std::vector<AnchoredSliceRequest> reqs{
      AnchoredSliceRequest{o0, s0.T, kF, true},
      AnchoredSliceRequest{o1, s1.T, kF, true},
  };
  EXPECT_EQ(anchored_fit_sequence(reqs, AnchoredOpts{}).error().code(),
            ErrorCode::InvalidArgument);
}

}  // namespace
