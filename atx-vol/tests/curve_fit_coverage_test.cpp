// Task 1 — board-level k-coverage refusal (PrepUncovered).
//
// A ConvexDense slice whose admitted rows are entirely one-sided (the
// 2025-04-24 freshly-listed-daily shape) must be REFUSED into the existing
// slice-drop lane (ExpiryFitOutcome::PrepUncovered -> tenor truncation), not
// fitted and extrapolated. The healthy sibling expiry still fits.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp"       // american_price, AmericanMethod
#include "atx/vol/curve_fit.hpp"      // fit_curve_surface, CurveSurfaceReport
#include "atx/vol/dividend.hpp"       // hybrid_forward, HybridDivParams
#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs, ExpiryFitOutcome
#include "atx/vol/types.hpp"          // Side
#include "atx/vol/universe.hpp"       // Underlying, Chain, chain_index
#include "atx/vol/vol_curve.hpp"      // CurveConfig

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
