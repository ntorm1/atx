#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"       // american_price, AmericanMethod
#include "atx/vol/curve_fit.hpp"      // fit_curve_surface, CurveSurfaceReport
#include "atx/vol/deamer.hpp"         // CarrySource, DeAmOptions
#include "atx/vol/dividend.hpp"       // hybrid_forward, HybridDivParams
#include "atx/vol/pricer_fitter.hpp"  // merge_session_failure_context, ValidationDigest
#include "atx/vol/session.hpp"        // SessionDiagnostics
#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs, ExpiryFitOutcome
#include "atx/vol/surface_policy.hpp"  // decide_risk_surface_admission, SurfaceState
#include "atx/vol/types.hpp"          // Side
#include "atx/vol/universe.hpp"       // Underlying, Chain, chain_index
#include "atx/vol/vol_curve.hpp"      // CurveConfig, IVolCurve

// Decision B — term-structure carry fallback (bt-hotpath sprint).
//
// Under the RISK build (`require_carry_confidence = true`) a per-expiry carry
// solve that is NOT confident is, historically, HARD-DROPPED at
// resolve_chain_carry, so an XOM-class thin board loses most of its term
// structure. Decision B instead DERIVES a non-confident expiry's borrow by
// interpolating / flat-extending the borrow-vs-T term structure built from the
// SAME board's CONFIDENT expiries, admitting the slice with an explicit
// provenance flag (CarrySource::TermStructureInterp / TermStructureExtrap).
//
// These board-level cases exercise `fit_curve_surface` (the ConvexDense
// risk-fit driver): (a) an interior non-confident expiry is admitted via
// interpolation; (b) an edge non-confident expiry via flat extrapolation;
// (c) a board with ZERO confident expiries still drops everything; (d) a control
// board that is fully confident is bit-identical regardless of the confidence
// gate (the repair pass is a strict no-op on it).

namespace {

using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::CarrySource;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::CurveSurfaceReport;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::ExpiryFitOutcome;
using atx::vol::fit_curve_surface;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::Side;
using atx::vol::SliceContext;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;

// A gentle convex smile in log-moneyness (positive over the ±20% band used).
[[nodiscard]] double true_sigma(double k) noexcept { return 0.20 + 0.15 * k * k; }

// A smooth, monotone borrow term structure so an interior interpolation and an
// edge flat-extension both land near the true per-T borrow.
[[nodiscard]] double borrow_of_T(double T) noexcept { return 0.02 + 0.04 * T; }

// Build one expiry chain at (T, borrow). Every strike carries its OTM leg (so
// the de-Am strip has enough fit rows); the `n_pairs` strikes CLOSEST to ATM
// additionally carry the non-OTM leg, making them co-terminal carry pairs.
// n_pairs >= min_confident_borrow_pairs (3) => the carry solve is confident;
// n_pairs == 2 => not confident (n_retained < 3) but still fittable. Prices are
// cold accurate Andersen-Lake at the true smile on F = hybrid_forward(borrow),
// so the carry solve recovers `borrow` and every OTM inversion recovers the
// smile (deamer.hpp round-trip).
[[nodiscard]] Chain make_carry_chain(double T, double borrow, int n_pairs, int n_total) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * kYearNs);

  const std::vector<DividendEvent> no_divs;
  const double F =
      hybrid_forward(kSpot, kRate, borrow, T, no_divs, c.expiry_ns, /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;

  constexpr double kLo = -0.20;
  constexpr double kHi = 0.20;
  c.strikes.reserve(static_cast<std::size_t>(n_total));
  for (int i = 0; i < n_total; ++i) {
    const double frac = static_cast<double>(i) / static_cast<double>(n_total - 1);
    const double k = kLo + frac * (kHi - kLo);
    c.strikes.push_back(F * std::exp(k));
  }

  // The n_pairs strikes with smallest |ln(K/F)| become co-terminal pairs.
  std::vector<std::size_t> by_atm(c.strikes.size());
  std::iota(by_atm.begin(), by_atm.end(), std::size_t{0});
  std::stable_sort(by_atm.begin(), by_atm.end(), [&](std::size_t a, std::size_t b) {
    return std::fabs(std::log(c.strikes[a] / F)) < std::fabs(std::log(c.strikes[b] / F));
  });
  std::vector<bool> is_pair(c.strikes.size(), false);
  for (int j = 0; j < n_pairs && j < static_cast<int>(by_atm.size()); ++j) {
    is_pair[by_atm[static_cast<std::size_t>(j)]] = true;
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

  const auto put_leg = [&](std::size_t i, Side side) {
    const double K = c.strikes[i];
    const double sigma = true_sigma(std::log(K / F));
    const auto px = american_price(kSpot, K, T, sigma, kRate, q_eff, side, AmericanMethod::AndersenLake,
                                   std::nullopt);
    ASSERT_TRUE(px.has_value()) << "american_price failed K=" << K << " T=" << T;
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
    c.mids[idx] = *px;
    c.bids[idx] = *px * 0.99;
    c.asks[idx] = *px * 1.01;
    c.bid_sizes[idx] = 1;
    c.ask_sizes[idx] = 1;
  };

  for (std::size_t i = 0; i < n; ++i) {
    const double k = std::log(c.strikes[i] / F);
    const Side otm = (k >= 0.0) ? Side::Call : Side::Put;
    put_leg(i, otm);
    if (is_pair[i]) {
      put_leg(i, otm == Side::Call ? Side::Put : Side::Call);
    }
  }
  return c;
}

[[nodiscard]] SurfaceParityInputs carry_inputs(bool require_confidence) {
  SurfaceParityInputs in{};
  in.S = kSpot;
  in.r = kRate;
  in.deam.imply_borrow = true;
  in.deam.require_carry_confidence = require_confidence;
  in.deam.min_confident_borrow_pairs = 3;
  in.deam.n_atm = 3;
  in.deam.max_borrow_pairs = 6;
  in.deam.carry_atm_band = 0.12; // count the near-ATM co-terminal pairs
  in.fit_workers = 1;            // deterministic
  return in;
}

// Committed-slice borrow at maturity T (context is ascending-T, one per fitted
// slice). Returns NaN if no slice was committed at that T.
[[nodiscard]] double committed_borrow_at(const CurveSurfaceReport &rep, double T) {
  for (const SliceContext &c : rep.context) {
    if (std::fabs(c.T - T) < 1.0e-9) {
      return c.borrow;
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace

// (a) Interior non-confident expiry: bracketed by confident neighbours on both
// sides => admitted via LINEAR interpolation of the borrow term structure.
TEST(CurveFitCarryFallback, InteriorNonConfidentExpiryAdmittedViaInterpolation) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_carry_chain(0.10, borrow_of_T(0.10), /*n_pairs=*/15, /*n_total=*/15));
  under.chains.push_back(make_carry_chain(0.30, borrow_of_T(0.30), /*n_pairs=*/2, /*n_total=*/15));
  under.chains.push_back(make_carry_chain(0.60, borrow_of_T(0.60), /*n_pairs=*/15, /*n_total=*/15));
  under.chains.push_back(make_carry_chain(1.00, borrow_of_T(1.00), /*n_pairs=*/15, /*n_total=*/15));

  const auto rep = fit_curve_surface(under, carry_inputs(true), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_EQ(rep->expiry_reports.size(), 4u);

  EXPECT_EQ(rep->expiry_reports[1].outcome, ExpiryFitOutcome::Fitted);
  EXPECT_EQ(rep->expiry_reports[1].carry_source, CarrySource::TermStructureInterp);
  EXPECT_EQ(rep->expiry_reports[0].carry_source, CarrySource::Solved);
  EXPECT_EQ(rep->expiry_reports[2].carry_source, CarrySource::Solved);
  EXPECT_EQ(rep->expiry_reports[3].carry_source, CarrySource::Solved);
  EXPECT_EQ(rep->n_slices, 4u);
  EXPECT_EQ(rep->n_carry_skipped, 0u);

  // The repaired borrow is the linear interpolation of the two bracketing
  // confident solves (T=0.10, T=0.60) at T=0.30.
  const double b_lo = committed_borrow_at(*rep, 0.10);
  const double b_hi = committed_borrow_at(*rep, 0.60);
  const double b_mid = committed_borrow_at(*rep, 0.30);
  ASSERT_TRUE(std::isfinite(b_lo) && std::isfinite(b_hi) && std::isfinite(b_mid));
  const double alpha = (0.30 - 0.10) / (0.60 - 0.10);
  const double expected = b_lo + alpha * (b_hi - b_lo);
  EXPECT_NEAR(b_mid, expected, 1.0e-9);
}

// (b) Edge non-confident expiry: no confident neighbour on the short side =>
// admitted via FLAT extrapolation of the nearest confident borrow.
TEST(CurveFitCarryFallback, EdgeNonConfidentExpiryAdmittedViaExtrapolation) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_carry_chain(0.10, borrow_of_T(0.10), /*n_pairs=*/2, /*n_total=*/15));
  under.chains.push_back(make_carry_chain(0.30, borrow_of_T(0.30), /*n_pairs=*/15, /*n_total=*/15));
  under.chains.push_back(make_carry_chain(0.60, borrow_of_T(0.60), /*n_pairs=*/15, /*n_total=*/15));
  under.chains.push_back(make_carry_chain(1.00, borrow_of_T(1.00), /*n_pairs=*/15, /*n_total=*/15));

  const auto rep = fit_curve_surface(under, carry_inputs(true), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_EQ(rep->expiry_reports.size(), 4u);

  EXPECT_EQ(rep->expiry_reports[0].outcome, ExpiryFitOutcome::Fitted);
  EXPECT_EQ(rep->expiry_reports[0].carry_source, CarrySource::TermStructureExtrap);
  EXPECT_EQ(rep->n_slices, 4u);
  EXPECT_EQ(rep->n_carry_skipped, 0u);

  // Flat-extended from the nearest confident anchor (T=0.30).
  const double b_edge = committed_borrow_at(*rep, 0.10);
  const double b_anchor = committed_borrow_at(*rep, 0.30);
  ASSERT_TRUE(std::isfinite(b_edge) && std::isfinite(b_anchor));
  EXPECT_NEAR(b_edge, b_anchor, 1.0e-12);
}

// (c) Zero confident expiries on the whole board: no anchor to borrow from, so
// everything is STILL dropped (no fabrication from nothing) — behaviour
// unchanged from the historical hard-drop. The same board WITHOUT the gate is
// fittable (proving the drop is the gate, not a broken board).
TEST(CurveFitCarryFallback, ZeroConfidentBoardStillDropsEverything) {
  Underlying under;
  under.spot = kSpot;
  for (const double T : {0.10, 0.30, 0.60, 1.00}) {
    under.chains.push_back(make_carry_chain(T, borrow_of_T(T), /*n_pairs=*/2, /*n_total=*/15));
  }

  const auto rep = fit_curve_surface(under, carry_inputs(true), CurveConfig{});
  ASSERT_FALSE(rep.has_value());
  EXPECT_EQ(rep.error().code(), ErrorCode::NotFound);

  // Control: without the confidence gate the very same board admits every
  // expiry with a directly SOLVED carry (no fallback fabricated).
  const auto open = fit_curve_surface(under, carry_inputs(false), CurveConfig{});
  ASSERT_TRUE(open.has_value()) << open.error().to_string();
  EXPECT_EQ(open->n_slices, 4u);
  for (const auto &er : open->expiry_reports) {
    EXPECT_EQ(er.carry_source, CarrySource::Solved);
  }
}

// (d) Fully-confident control board: the repair pass is a strict no-op, so the
// fit is bit-identical whether or not the confidence gate is armed (same
// forward, same borrow, same surface) and every expiry carries Solved
// provenance.
TEST(CurveFitCarryFallback, ConfidentBoardBitIdenticalRegardlessOfGate) {
  Underlying under;
  under.spot = kSpot;
  for (const double T : {0.10, 0.30, 0.60, 1.00}) {
    under.chains.push_back(make_carry_chain(T, borrow_of_T(T), /*n_pairs=*/15, /*n_total=*/15));
  }

  const auto gated = fit_curve_surface(under, carry_inputs(true), CurveConfig{});
  const auto open = fit_curve_surface(under, carry_inputs(false), CurveConfig{});
  ASSERT_TRUE(gated.has_value()) << gated.error().to_string();
  ASSERT_TRUE(open.has_value()) << open.error().to_string();

  ASSERT_EQ(gated->n_slices, 4u);
  ASSERT_EQ(open->n_slices, 4u);
  EXPECT_EQ(gated->n_carry_skipped, 0u);
  for (const auto &er : gated->expiry_reports) {
    EXPECT_EQ(er.outcome, ExpiryFitOutcome::Fitted);
    EXPECT_EQ(er.carry_source, CarrySource::Solved);
  }

  // Byte-for-byte identical context + surface between the two gate settings.
  ASSERT_EQ(gated->context.size(), open->context.size());
  for (std::size_t i = 0; i < gated->context.size(); ++i) {
    EXPECT_EQ(gated->context[i].T, open->context[i].T) << "slice " << i;
    EXPECT_EQ(gated->context[i].forward, open->context[i].forward) << "slice " << i;
    EXPECT_EQ(gated->context[i].borrow, open->context[i].borrow) << "slice " << i;
    EXPECT_EQ(gated->context[i].q_eff, open->context[i].q_eff) << "slice " << i;
  }
  ASSERT_EQ(gated->surface.n_slices(), open->surface.n_slices());
  constexpr double kGrid[] = {-0.10, -0.05, 0.0, 0.05, 0.10};
  for (std::size_t si = 0; si < gated->surface.n_slices(); ++si) {
    const auto *ga = gated->surface.slices()[si].get();
    const auto *op = open->surface.slices()[si].get();
    for (const double k : kGrid) {
      EXPECT_EQ(ga->iv(k), op->iv(k)) << "slice " << si << " k=" << k;
    }
  }
}

// ── T5c (B3ii): the two carry-fallback extensions ────────────────────────

// (e) An expiry with NO co-terminal pair at all cannot solve its own carry, so
// `resolve_chain_forward` returns Unavailable. Historically that hard-dropped in
// the prepass, out of reach of the repair pass — even on a board whose other
// expiries pin the carry perfectly. It is a statement about ONE expiry's quote
// shape, not about the board's carry, so it now defers to the term-structure
// repair like any other non-confident expiry.
TEST(CurveFitCarryFallback, NoCoterminalPairExpiryRepairedFromTermStructure) {
  Underlying under;
  under.spot = kSpot;
  under.chains.push_back(make_carry_chain(0.10, borrow_of_T(0.10), /*n_pairs=*/15, /*n_total=*/15));
  // Zero co-terminal pairs: every strike carries only its OTM leg.
  under.chains.push_back(make_carry_chain(0.30, borrow_of_T(0.30), /*n_pairs=*/0, /*n_total=*/15));
  under.chains.push_back(make_carry_chain(0.60, borrow_of_T(0.60), /*n_pairs=*/15, /*n_total=*/15));

  const auto rep = fit_curve_surface(under, carry_inputs(true), CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  ASSERT_EQ(rep->expiry_reports.size(), 3u);
  EXPECT_EQ(rep->expiry_reports[1].outcome, ExpiryFitOutcome::Fitted);
  EXPECT_EQ(rep->expiry_reports[1].carry_source, CarrySource::TermStructureInterp);
  EXPECT_EQ(rep->n_slices, 3u);
  EXPECT_EQ(rep->n_carry_skipped, 0u);

  // Interpolated between the two solved neighbours, not invented.
  const double b_lo = committed_borrow_at(*rep, 0.10);
  const double b_hi = committed_borrow_at(*rep, 0.60);
  const double b_mid = committed_borrow_at(*rep, 0.30);
  ASSERT_TRUE(std::isfinite(b_lo) && std::isfinite(b_hi) && std::isfinite(b_mid));
  const double alpha = (0.30 - 0.10) / (0.60 - 0.10);
  EXPECT_NEAR(b_mid, b_lo + alpha * (b_hi - b_lo), 1.0e-9);

  // Without the repair pass armed (no confidence requirement) the board is
  // bit-identical to its historical shape: the pairless expiry is still dropped.
  const auto open = fit_curve_surface(under, carry_inputs(false), CurveConfig{});
  ASSERT_TRUE(open.has_value()) << open.error().to_string();
  EXPECT_EQ(open->n_slices, 2u);
  EXPECT_EQ(open->expiry_reports[1].outcome, ExpiryFitOutcome::CarryFailed);
}

// (f) A board with ZERO confident expiries recovers when at least one expiry's
// carry is MEASURED to inside the standard-deviation-moneyness budget: the
// second-tier anchor. Provenance stays honest — no expiry reports Solved, and the
// board is never confident, so admission publishes Degraded (CarryGap).
TEST(CurveFitCarryFallback, ZeroConfidentBoardRecoversViaMoneynessBoundedAnchor) {
  Underlying under;
  under.spot = kSpot;
  for (const double T : {0.10, 0.30, 0.60, 1.00}) {
    under.chains.push_back(make_carry_chain(T, borrow_of_T(T), /*n_pairs=*/15, /*n_total=*/15));
  }

  // A rate-unit leave-one-out gate no real solve can meet: every expiry is
  // NON-confident, so tier one is empty — the production shape of all 28 boards
  // that lost every expiry to carry on lqbench 2026-08-03.
  SurfaceParityInputs in = carry_inputs(true);
  in.deam.max_carry_leave_one_out = 1.0e-12;

  const auto rep = fit_curve_surface(under, in, CurveConfig{});
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string();
  EXPECT_EQ(rep->n_slices, 4u);
  EXPECT_EQ(rep->n_carry_skipped, 0u);
  for (const auto &er : rep->expiry_reports) {
    EXPECT_EQ(er.outcome, ExpiryFitOutcome::Fitted);
    EXPECT_NE(er.carry_source, CarrySource::Solved) << "a fallback carry must not read as solved";
  }
  // The anchors commit their OWN measured borrow, so the recovered carry tracks
  // the generating term structure rather than a fabricated constant.
  for (const double T : {0.10, 0.30, 0.60, 1.00}) {
    EXPECT_NEAR(committed_borrow_at(*rep, T), borrow_of_T(T), 5.0e-3) << "T=" << T;
  }

  // Turning the second tier OFF (non-positive budget) restores the historical
  // total refusal — the tier is the only reason this board serves.
  SurfaceParityInputs no_tier = in;
  no_tier.deam.max_carry_moneyness_shift = 0.0;
  const auto refused = fit_curve_surface(under, no_tier, CurveConfig{});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::NotFound);
}

// Stage B (session admission gate). A board whose non-confident expiries were
// ADMITTED via the term-structure fallback must publish DEGRADED (CarryGap), not
// be hard-rejected (InsufficientData) — otherwise Decision B would REGRESS thin
// risk boards from today's Degraded-publish-with-missing-expiries to a full
// reject. A fully-confident board is untouched, and a genuine carry hole (no
// fallback) still hard-rejects (fail-closed).
TEST(CarryFallbackAdmission, FallbackCarryPublishesDegradedNotInsufficientData) {
  using atx::vol::has_validation_failure;
  using atx::vol::merge_session_failure_context;
  using atx::vol::SessionDiagnostics;
  using atx::vol::ValidationDigest;
  using atx::vol::ValidationFailure;

  // Fallback-carry board: honest carry_confident=false, but every expiry is
  // admissible because one was borrowed from the board term structure.
  SessionDiagnostics fallback;
  fallback.n_slices = 4;
  fallback.carry_confident = false;
  fallback.n_carry_fallback_expiries = 1;
  fallback.inversion_certified = true; // isolate the carry path
  ValidationDigest d;
  merge_session_failure_context(fallback, d);
  EXPECT_FALSE(has_validation_failure(d.failures, ValidationFailure::InsufficientData));
  EXPECT_TRUE(has_validation_failure(d.failures, ValidationFailure::CarryGap));

  // Fully-confident board: no InsufficientData, no fallback CarryGap.
  SessionDiagnostics confident;
  confident.n_slices = 4;
  confident.carry_confident = true;
  confident.inversion_certified = true;
  ValidationDigest dc;
  merge_session_failure_context(confident, dc);
  EXPECT_TRUE(dc.admitted());

  // Genuine carry hole (not confident, NO fallback): still hard-rejects.
  SessionDiagnostics hole;
  hole.n_slices = 4;
  hole.carry_confident = false;
  hole.n_carry_fallback_expiries = 0;
  hole.inversion_certified = true;
  ValidationDigest dh;
  merge_session_failure_context(hole, dh);
  EXPECT_TRUE(has_validation_failure(dh.failures, ValidationFailure::InsufficientData));
}

// T6 (session admission gate). PREPARATION starvation is the largest class of
// lost expiry on this corpus and was the only one with no route into the
// published failure context: a board could lose a third of its expiries to it
// and still admit as Healthy, because the carry gate and the fit-inversion
// audit — the two gaps that DID reach the digest — had not fired.
TEST(CarryFallbackAdmission, PrepStarvedExpiriesPublishDegradedNotHealthy) {
  using atx::vol::decide_risk_surface_admission;
  using atx::vol::FitQualityMode;
  using atx::vol::has_validation_failure;
  using atx::vol::merge_session_failure_context;
  using atx::vol::SessionDiagnostics;
  using atx::vol::SurfaceFallback;
  using atx::vol::SurfaceState;
  using atx::vol::ValidationDigest;
  using atx::vol::ValidationFailure;

  // A board with a perfect carry and a certified inversion that nonetheless
  // lost expiries to a starved preparation.
  SessionDiagnostics starved;
  starved.n_slices = 4;
  starved.carry_confident = true;
  starved.inversion_certified = true;
  starved.n_prep_starved_expiries = 3;
  ValidationDigest ds;
  merge_session_failure_context(starved, ds);
  EXPECT_TRUE(has_validation_failure(ds.failures, ValidationFailure::CarryGap));
  EXPECT_FALSE(has_validation_failure(ds.failures, ValidationFailure::InsufficientData));

  // ... and it must still SERVE. Over-rejecting a thin board would destroy the
  // very coverage the rest of T6 recovers, so starvation reuses the one
  // publish-with-Degraded bit: Degraded alone, rejecting only in combination.
  const auto decision = decide_risk_surface_admission(
      ds, FitQualityMode::Balanced, /*candidate_generation=*/7u,
      /*last_admitted_generation=*/0u, SurfaceFallback::None);
  EXPECT_TRUE(decision.publish_candidate);
  EXPECT_EQ(decision.health.state, SurfaceState::Degraded);
  EXPECT_TRUE(has_validation_failure(decision.health.reasons, ValidationFailure::CarryGap));

  // A board that lost NOTHING keeps its clean Healthy admission.
  SessionDiagnostics whole;
  whole.n_slices = 4;
  whole.carry_confident = true;
  whole.inversion_certified = true;
  ValidationDigest dw;
  merge_session_failure_context(whole, dw);
  EXPECT_TRUE(dw.admitted());
  const auto healthy = decide_risk_surface_admission(
      dw, FitQualityMode::Balanced, /*candidate_generation=*/7u,
      /*last_admitted_generation=*/0u, SurfaceFallback::None);
  EXPECT_EQ(healthy.health.state, SurfaceState::Healthy);
}
