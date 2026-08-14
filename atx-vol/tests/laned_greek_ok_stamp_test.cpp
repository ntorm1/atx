// FIX-2/F2-B (rev-ws-g M1-5, carried from the WS-G review and left open by FIX-1):
// the laned analytic-Greek Ok-stamp must guard the REQUESTED greek set, not just the
// price.
//
// THE DEFECT. detail::laned_greek_run's scatter stamps atx::core::Ok() on
// `std::isfinite(g.price)` alone (laned_greek_run.hpp:83). The scalar production
// bundles can return SUCCESS with a finite price and a NON-finite differenced greek,
// so a lane could come back Ok while carrying a NaN greek. FIX-1 closed exactly this
// class at the three portfolio-level Ok-stamps (740b040 F2, 9c3e1d0 F3), but this
// driver is upstream of them: PricedSurface::evaluate_batch and
// PricedSurfaceView::evaluate_batch both route through it, so every DIRECT
// evaluate_batch consumer -- one that does not go through PortfolioPricer -- was
// still exposed. The scalar fallback does not close it either: evaluate_resolved
// returns a default (Ok) status whenever greeks_resolved yields a value, non-finite
// columns included (priced_surface.cpp:865-875).
//
// THE TRIGGER. A deep-ITM long-dated put: S=1e-8, K=100, T=1e7, sigma=0.2, r=q=0.
// The r+/r- differenced rho is non-finite while price/delta/gamma/theta/vega/volga/
// vanna/charm are all finite -- the same shape FIX-1/F3 found on the FD route,
// confirmed here to survive the laned Andersen-Lake batch kernel as well. Both halves
// of the guard's semantics are pinned below: a non-finite REQUESTED column must not be
// stamped Ok, and narrowing GreekNeeds must not veto a lane on a column the caller
// never asked for (FIX-1/F3 showed over-guarding is its own defect).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp"

#include "backtest/laned_greek_run.hpp"

namespace atx::vol {
namespace {

constexpr double kS = 1.0e-8;
constexpr double kK = 100.0;
constexpr double kT = 1.0e7;
constexpr double kSigma = 0.2;

// A sentinel the scalar fallback writes, so a test can tell "stamped Ok" from
// "routed to the caller's scalar path" from "demoted in place".
constexpr double kFallbackPrice = -98765.0;

[[nodiscard]] PricedSurface::ResolvedSurfacePoint trigger_point() {
  PricedSurface::ResolvedSurfacePoint p;
  p.K = kK;
  p.T = kT;
  p.forward = kS; // r == q_eff == 0 keeps F == S and the discount at 1.0
  p.q_eff = 0.0;
  p.k_log = 0.0;
  p.sigma = kSigma;
  p.rate = 0.0;
  p.valid = true;
  return p;
}

// The bundle the laned kernel produces for the trigger under `needs` -- the reference
// every "bit-identical" assertion below compares against.
[[nodiscard]] AmericanGreeks kernel_bundle(GreekNeeds needs) {
  AmericanGreeks g{};
  const double S = kS;
  const double K = kK;
  const double T = kT;
  const double sig = kSigma;
  const double r = 0.0;
  const double q = 0.0;
  (void)simd::american_put_greeks_batch(&S, &K, &T, &sig, &r, &q, 1,
                                        std::optional<AlOpts>{al_fast_opts()}, &g,
                                        simd::SimdIsa::Auto, needs.vega, needs.rho, needs.charm);
  return g;
}

struct RunOut {
  double iv{};
  double price{};
  AmericanGreeks greeks{};
  Status status{};
  bool fallback_taken{false};
};

// Drive ONE lane through the shared driver with full control of the resolved point,
// so the Ok-stamp is exercised directly rather than through a fitted surface.
[[nodiscard]] RunOut run_one(GreekNeeds needs, const PricedSurface::ResolvedSurfacePoint &p) {
  std::array<Side, 1> side{Side::Put};
  std::array<double, 1> iv{};
  std::array<double, 1> price{};
  std::array<AmericanGreeks, 1> greeks{};
  std::array<Status, 1> status{};
  PricedSurface::EvaluationSoA soa{iv, price, greeks, status, {}, {}};

  RunOut o;
  detail::laned_greek_run(
      kS, side, std::optional<AlOpts>{al_fast_opts()}, simd::SimdIsa::Auto, needs, soa,
      [&](const auto &append) { append(0, p); },
      [&](const PricedSurface::ResolvedSurfacePoint &, Side) {
        o.fallback_taken = true;
        PricedSurface::FusedResult fr;
        fr.price = kFallbackPrice;
        return fr;
      });
  o.iv = iv[0];
  o.price = price[0];
  o.greeks = greeks[0];
  o.status = status[0];
  return o;
}

// ---------------------------------------------------------------------------------
// Precondition: the trigger really is finite-everything-but-rho on the laned route.
// ---------------------------------------------------------------------------------

TEST(LanedGreekOkStamp, TriggerIsFiniteEverywhereExceptRho) {
  const AmericanGreeks g = kernel_bundle(GreekNeeds{});
  EXPECT_TRUE(std::isfinite(g.price));
  EXPECT_TRUE(std::isfinite(g.delta));
  EXPECT_TRUE(std::isfinite(g.gamma));
  EXPECT_TRUE(std::isfinite(g.theta));
  EXPECT_TRUE(std::isfinite(g.vega));
  EXPECT_TRUE(std::isfinite(g.volga));
  EXPECT_TRUE(std::isfinite(g.vanna));
  EXPECT_TRUE(std::isfinite(g.charm));
  EXPECT_FALSE(std::isfinite(g.rho)) << "trigger no longer produces a non-finite rho";

  // Informational: what the kernel leaves in the slot when rho is NOT requested.
  GreekNeeds no_rho{};
  no_rho.rho = false;
  const AmericanGreeks h = kernel_bundle(no_rho);
  std::printf("[precondition] needs.rho=false -> rho=%g (finite=%d)\n", h.rho,
              static_cast<int>(std::isfinite(h.rho)));
}

// ---------------------------------------------------------------------------------
// The guard: a non-finite REQUESTED greek must not be stamped Ok.
// ---------------------------------------------------------------------------------

TEST(LanedGreekOkStamp, F2B_NonFiniteRequestedGreekIsNotStampedOk) {
  const RunOut o = run_one(GreekNeeds{}, trigger_point()); // default = rho REQUESTED

  ASSERT_FALSE(std::isfinite(o.greeks.rho))
      << "precondition: this lane must carry a non-finite rho";
  EXPECT_TRUE(std::isfinite(o.price)) << "precondition: the mark itself is finite";
  EXPECT_FALSE(o.status.has_value())
      << "a lane with a finite price but a non-finite REQUESTED greek was stamped Ok, so a "
         "direct evaluate_batch consumer sees a NaN greek on an Ok lane";
}

// ---------------------------------------------------------------------------------
// The mirror image (FIX-1/F3): narrowing GreekNeeds must not veto a good lane, and
// every column the caller DID ask for must be bit-identical to today.
// ---------------------------------------------------------------------------------

TEST(LanedGreekOkStamp, F2B_UnrequestedRhoDoesNotVetoTheLaneAndRequestedColumnsAreUnchanged) {
  GreekNeeds needs{};
  needs.rho = false; // the K4 first-order tier: rho is NOT requested
  const RunOut o = run_one(needs, trigger_point());
  const AmericanGreeks ref = kernel_bundle(needs);

  EXPECT_TRUE(o.status.has_value())
      << "the lane was vetoed on rho -- a column the caller never requested";
  EXPECT_FALSE(o.fallback_taken) << "an admissible lane must not be re-routed to the scalar path";

  // Bit-identical on every REQUESTED column.
  EXPECT_EQ(o.price, ref.price);
  EXPECT_EQ(o.greeks.price, ref.price);
  EXPECT_EQ(o.greeks.delta, ref.delta);
  EXPECT_EQ(o.greeks.gamma, ref.gamma);
  EXPECT_EQ(o.greeks.theta, ref.theta);
  EXPECT_EQ(o.greeks.vega, ref.vega);
  EXPECT_EQ(o.greeks.volga, ref.volga);
  EXPECT_EQ(o.greeks.vanna, ref.vanna);
  EXPECT_EQ(o.greeks.charm, ref.charm);

  // The unrequested slot must never hand a NaN to a downstream product (NaN * 0.0 is
  // NaN, not 0.0 -- the FIX-1/F3 lesson).
  EXPECT_TRUE(std::isfinite(o.greeks.rho))
      << "an UNREQUESTED non-finite rho was passed through to the consumer";
}

// ---------------------------------------------------------------------------------
// The requested-set semantics, exercised directly.
//
// The laned kernel honors its "unrequested greeks are left 0" contract, so it will not
// hand this driver a non-finite UNREQUESTED column (the precondition test above prints
// rho=0 under needs.rho=false). The over-guard half of the semantics therefore cannot
// be reached through the kernel, and the guard predicate is pinned directly instead --
// otherwise a full-bundle guard would look correct on every reachable input and the
// FIX-1/F3 lesson would be silently unlearned here.
// ---------------------------------------------------------------------------------

TEST(LanedGreekOkStamp, F2B_GuardCoversTheRequestedSetOnly) {
  AmericanGreeks g{};
  g.price = 1.0;
  g.delta = -0.5;
  g.gamma = 0.1;
  g.theta = -0.01;
  g.vega = 0.2;
  g.volga = 0.3;
  g.vanna = 0.4;
  g.charm = 0.5;
  g.rho = std::numeric_limits<double>::quiet_NaN();

  GreekNeeds all{};
  EXPECT_FALSE(detail::requested_greeks_finite(g, all)) << "a REQUESTED non-finite rho must veto";

  GreekNeeds no_rho{};
  no_rho.rho = false;
  EXPECT_TRUE(detail::requested_greeks_finite(g, no_rho))
      << "an UNREQUESTED non-finite rho must NOT veto (FIX-1/F3: over-guarding is its own "
         "defect)";

  // The always-guarded columns hold regardless of GreekNeeds.
  AmericanGreeks bad_gamma = g;
  bad_gamma.rho = 0.0;
  bad_gamma.gamma = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(detail::requested_greeks_finite(bad_gamma, no_rho));

  // vega gates its whole group.
  AmericanGreeks bad_volga = g;
  bad_volga.rho = 0.0;
  bad_volga.volga = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(detail::requested_greeks_finite(bad_volga, no_rho));
  GreekNeeds no_vega{};
  no_vega.vega = false;
  no_vega.rho = false;
  EXPECT_TRUE(detail::requested_greeks_finite(bad_volga, no_vega));
}

TEST(LanedGreekOkStamp, F2B_UnrequestedNonFiniteSlotIsNormalizedNotPassedThrough) {
  AmericanGreeks g{};
  g.price = 1.0;
  g.rho = std::numeric_limits<double>::quiet_NaN();
  g.charm = std::numeric_limits<double>::quiet_NaN();
  g.vega = 7.0; // requested and finite: must be left exactly alone

  GreekNeeds needs{};
  needs.rho = false;
  needs.charm = false;

  AmericanGreeks n = g;
  detail::normalize_unrequested_greeks(n, needs);
  EXPECT_EQ(n.rho, 0.0) << "NaN * 0.0 is NaN, so an unrequested NaN must be zeroed, not kept";
  EXPECT_EQ(n.charm, 0.0);
  EXPECT_EQ(n.vega, 7.0) << "a requested, finite column must be untouched";

  // Restricted to the non-finite case: a finite unrequested value is preserved bitwise.
  AmericanGreeks finite_rho = g;
  finite_rho.rho = 1.25;
  finite_rho.charm = -3.5;
  AmericanGreeks m = finite_rho;
  detail::normalize_unrequested_greeks(m, needs);
  EXPECT_EQ(m.rho, 1.25);
  EXPECT_EQ(m.charm, -3.5);
}

// ---------------------------------------------------------------------------------
// Regression pin: an ordinary lane is untouched by the guard.
// ---------------------------------------------------------------------------------

TEST(LanedGreekOkStamp, F2B_OrdinaryLaneStillStampsOkBitIdentically) {
  PricedSurface::ResolvedSurfacePoint p;
  p.K = 100.0;
  p.T = 1.0;
  p.forward = 100.0;
  p.q_eff = 0.0;
  p.k_log = 0.0;
  p.sigma = 0.2;
  p.rate = 0.0;
  p.valid = true;

  std::array<Side, 1> side{Side::Put};
  std::array<double, 1> iv{};
  std::array<double, 1> price{};
  std::array<AmericanGreeks, 1> greeks{};
  std::array<Status, 1> status{};
  PricedSurface::EvaluationSoA soa{iv, price, greeks, status, {}, {}};
  bool fallback = false;
  detail::laned_greek_run(
      100.0, side, std::optional<AlOpts>{al_fast_opts()}, simd::SimdIsa::Auto, GreekNeeds{}, soa,
      [&](const auto &append) { append(0, p); },
      [&](const PricedSurface::ResolvedSurfacePoint &, Side) {
        fallback = true;
        PricedSurface::FusedResult fr;
        fr.price = kFallbackPrice;
        return fr;
      });

  AmericanGreeks ref{};
  const double S = 100.0;
  const double K = 100.0;
  const double T = 1.0;
  const double sig = 0.2;
  const double r = 0.0;
  const double q = 0.0;
  (void)simd::american_put_greeks_batch(&S, &K, &T, &sig, &r, &q, 1,
                                        std::optional<AlOpts>{al_fast_opts()}, &ref,
                                        simd::SimdIsa::Auto);

  EXPECT_FALSE(fallback);
  EXPECT_TRUE(status[0].has_value());
  EXPECT_EQ(price[0], ref.price);
  EXPECT_EQ(greeks[0].delta, ref.delta);
  EXPECT_EQ(greeks[0].gamma, ref.gamma);
  EXPECT_EQ(greeks[0].theta, ref.theta);
  EXPECT_EQ(greeks[0].vega, ref.vega);
  EXPECT_EQ(greeks[0].rho, ref.rho);
  EXPECT_EQ(greeks[0].volga, ref.volga);
  EXPECT_EQ(greeks[0].vanna, ref.vanna);
  EXPECT_EQ(greeks[0].charm, ref.charm);
  EXPECT_EQ(iv[0], p.sigma);
}

} // namespace
} // namespace atx::vol
