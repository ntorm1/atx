#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "atx/vol/rates_curve.hpp"
#include "atx/vol/derivatives.hpp"
#include "atx/vol/detail/legacy_surface.hpp"  // EssviSurface (demoted, S4-T21)
#include "atx/vol/priced_surface.hpp"     // PricedSurface-native greeks overload
#include "atx/vol/surface.hpp"
#include "support/analytics_fixture.hpp"  // testkit::make_flat_surface (PricedSurface)
#include "support/deriv_test_fixture.hpp" // testsupport::make_flat_surface / make_flat_curves

// Task 7: finite-difference greeks for every vol-derivative kind. Every bump
// reprices through `deriv_price`, so a product/age/cap regime gets its greeks
// from exactly the path that produced its mark.

namespace atx::vol::detail {
// Task P-3 test seam: forces `eval_bump_table`'s `CachedBumpView` to bypass
// its shared read-vector cache and always read the surface live -- exactly
// reproducing the pre-P-3 RespotView+VolShiftView composition numerically
// (same `base->iv(k_log + k_shift, T) + vol_shift` expression). Forward-
// declared here (external linkage, no header touched) rather than exposed
// publicly, mirroring `derivatives_test.cpp`'s own `risk_validation_config`/
// `set_strip_batch_disabled_for_test` precedent. `DerivGreeks.ReadCache*`
// below uses it to run the SAME contract through both the cached and
// uncached tables and assert exact bit equality on every output greek.
void set_bump_read_cache_disabled_for_test(bool disabled) noexcept;
}  // namespace atx::vol::detail

namespace {

using atx::vol::CurveSet;
using atx::vol::deriv_default_config;
using atx::vol::deriv_greeks;
using atx::vol::deriv_price;
using atx::vol::DerivConfig;
using atx::vol::DerivContract;
using atx::vol::DerivDiscreteCorrection;
using atx::vol::DerivEngine;
using atx::vol::DerivGreekBumps;
using atx::vol::DerivGreekMethod;
using atx::vol::DerivKind;
using atx::vol::ErrorCode;
using atx::vol::EssviSlice;
using atx::vol::EssviSurface;
using atx::vol::testsupport::make_flat_curves;
using atx::vol::testsupport::make_flat_surface;

// Skewed eSSVI: rho < 0 (downside skew), phi > 0 (curvature). theta is
// proportional to T, so the ATM vol is a flat 20 across the term structure and
// only the SMILE shape drives the spot greeks.
EssviSurface make_skewed_surface() {
  EssviSurface surf(2);
  const EssviSlice s0{0.04 * 0.01, 1.5, -0.6, 0.01};
  const EssviSlice s1{0.04 * 1.00, 1.5, -0.6, 1.00};
  EXPECT_TRUE(surf.set_slice(0, s0).has_value());
  EXPECT_TRUE(surf.set_slice(1, s1).has_value());
  return surf;
}

// An unaged 1e6-notional var swap maturing at `T`.
DerivContract var_swap_at(double T) {
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  return c;
}

// Var swap on a FLAT surface: analytic truths.
//   vega  = dK_var/dsigma * w_future * df * N = 2*sigma * 1 * df * N
//   delta = 0 (no skew, sticky-strike)   gamma ~ 0
//   rho: PV(K != fair) discounts, d(df)/dr = -T*df
TEST(DerivGreeks, VarSwapFlatSurfaceAnalyticTruths) {
  const double sigma = 0.20, T = 0.25, N = 1e6;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = N;
  c.strike_dec = 0.02;  // off-fair so rho has something to discount
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  const double df = cs.yield.disc(T);
  EXPECT_NEAR(g->vega, 2.0 * sigma * df * N, 2e-2 * 2.0 * sigma * df * N);
  EXPECT_NEAR(g->delta * 100.0 / (N), 0.0, 1e-3);  // per-spot units, flat => ~0
  EXPECT_NEAR(g->rho, -T * g->pv, 5e-3 * std::fabs(-T * g->pv) + 1e-6);
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->theta));
  // theta of an off-fair var swap on a flat surface: future K_var is
  // T-independent, so d/dt only hits the discount: theta ~ r*pv = 0 here (r=0).
  EXPECT_NEAR(g->theta, 0.0, 1e-2 * std::fabs(g->pv) + 1.0);
}

// Skewed surface: delta must be nonzero and negative for a long var swap under
// sticky-strike with a negative skew (down-moves ride up the smile).
TEST(DerivGreeks, VarSwapSkewGivesNonzeroDelta) {
  EssviSurface surf(2);
  // rho < 0 skew, phi > 0 curvature
  const EssviSlice s0{0.04 * 0.01, 1.5, -0.6, 0.01};
  const EssviSlice s1{0.04 * 1.00, 1.5, -0.6, 1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.25;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value());
  EXPECT_LT(g->delta, 0.0);
  EXPECT_GT(std::fabs(g->delta) * 100.0, 1.0);  // economically visible
}

// FD self-consistency: greeks must reproduce a direct large-bump repricing.
//
// The reference is a CENTRAL large-bump difference, not a one-sided one. On
// this surface PV(sigma) = N*sigma^2 exactly, so a one-sided +1-vol reference
// evaluates to N*(2*sigma + 0.01) -- it carries its OWN O(h) bias of N*h =
// 10,000, i.e. 2.5% of the 400,000 truth (measured, not assumed). That would
// make the test assert the reference's discretization error rather than the
// greek's. Centering the reference cancels the O(h) term and leaves O(h^2),
// which is what makes "the greek reproduces a direct repricing" a real claim.
TEST(DerivGreeks, VegaMatchesDirectReprice) {
  const double sigma = 0.20, T = 0.25;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const EssviSurface surf_up = make_flat_surface(sigma + 0.01, 0.01, 1.00);
  const EssviSurface surf_dn = make_flat_surface(sigma - 0.01, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  const auto p1 = deriv_price(surf_up, cs, c, deriv_default_config());
  const auto pm1 = deriv_price(surf_dn, cs, c, deriv_default_config());
  ASSERT_TRUE(g.has_value());
  ASSERT_TRUE(p1.has_value());
  ASSERT_TRUE(pm1.has_value());
  const double fd = (p1->pv - pm1->pv) / 0.02;
  EXPECT_NEAR(g->vega, fd, 2e-2 * std::fabs(fd));
}

// Task P-2 / GK-P: `price_vol_swap`'s unaged-branch best-effort diagnostic
// strip (populates `uncapped_var_dec` / `convexity_adjustment_dec` / the grid
// fields, never read by any stencil) is skipped on every bumped/rolled
// evaluation `deriv_greeks` issues through `bumped_pv` -- see
// `is_bumped_greek_view`. Two claims to pin:
//   1. The CENTER's own diagnostic is untouched: `deriv_greeks` prices it via
//      the raw, un-wrapped surface, which never goes through `VolShiftView`.
//   2. Skipping the diagnostic on bumps changes NOTHING about the reported
//      greeks -- `vega` must still reproduce an INDEPENDENT direct central-
//      difference reprice through the ordinary `deriv_price` entry point
//      (also un-skipped, since a hand-built `EssviSurface` is never wrapped
//      either), mirroring `VegaMatchesDirectReprice` above but for an unaged
//      VolSwap, the kind/age combination the diagnostic actually guards.
TEST(DerivGreeks, DiagnosticStripSkipLeavesUnagedVolSwapGreeksUnaffected) {
  const double sigma = 0.20, T = 0.25;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const EssviSurface surf_up = make_flat_surface(sigma + 0.01, 0.01, 1.00);
  const EssviSurface surf_dn = make_flat_surface(sigma - 0.01, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;  // unaged: n_obs_done left at 0

  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  // Claim 1.
  EXPECT_GT(g->quote.uncapped_var_dec, 0.0);
  EXPECT_TRUE(std::isfinite(g->quote.convexity_adjustment_dec));
  EXPECT_TRUE(std::isfinite(g->quote.integration_error_est));
  EXPECT_GT(g->quote.strip_nodes_used, 0u);

  // Claim 2.
  const auto p1 = deriv_price(surf_up, cs, c, deriv_default_config());
  const auto pm1 = deriv_price(surf_dn, cs, c, deriv_default_config());
  ASSERT_TRUE(p1.has_value()) << p1.error().to_string();
  ASSERT_TRUE(pm1.has_value()) << pm1.error().to_string();
  const double fd = (p1->pv - pm1->pv) / 0.02;
  EXPECT_NEAR(g->vega, fd, 2e-2 * std::fabs(fd));

  // Every other greek stayed finite -- the skip did not silently drop
  // anything else in the block.
  for (const double v : {g->pv, g->delta, g->gamma, g->vega, g->volga, g->vanna,
                         g->theta, g->rho, g->charm}) {
    EXPECT_TRUE(std::isfinite(v));
  }
}

// Fully aged: pure discounting, all market greeks exactly zero.
TEST(DerivGreeks, FullyAgedHasOnlyRho) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = 0.0;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(g->delta, 0.0);
  EXPECT_EQ(g->gamma, 0.0);
  EXPECT_EQ(g->vega, 0.0);
  EXPECT_EQ(g->volga, 0.0);
  EXPECT_EQ(g->vanna, 0.0);
  EXPECT_EQ(g->theta, 0.0);
}

// Every product kind produces finite greeks mid-life (the full matrix).
//
// Task F-5: this test claimed "AllKinds" while listing FOUR of the six kinds
// that existed -- GammaSwap and CorridorVarSwap were never in it, and the name
// said otherwise. The list is now genuinely exhaustive over `DerivKind`,
// including F-5's two option kinds, and the per-kind setup below carries the
// two extra fields the omitted kinds always needed (a gamma anchor, and the
// corridor bounds left unbounded). If a kind is ever added without a row here
// the name lies again, so keep the list and the enum in step.
TEST(DerivGreeks, AllKindsMidLifeFinite) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  for (const DerivKind kind :
       {DerivKind::VarSwap, DerivKind::VolSwap, DerivKind::CappedVarSwap,
        DerivKind::CappedVolSwap, DerivKind::GammaSwap, DerivKind::CorridorVarSwap,
        DerivKind::VarianceCall, DerivKind::VariancePut}) {
    DerivContract c{};
    c.kind = kind;
    c.maturity_t = 0.10;
    c.notional = 1e5;
    c.strike_dec = 0.03;
    c.cap_dec = (kind == DerivKind::CappedVarSwap)   ? 0.25
                : (kind == DerivKind::CappedVolSwap) ? 0.50
                                                     : 0.0;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;
    c.rv_spec.n_obs_done = 21u;
    c.rv_spec.rv_done_dec = 0.05;
    // A mid-life GammaSwap needs its seed anchor to blend the accrued leg onto
    // the future one; every other kind ignores this field.
    c.rv_spec.gamma_seed_spot = 100.0;
    c.rv_spec.rv_gamma_done_dec = 0.05;
    const auto g = deriv_greeks(surf, cs, c);
    ASSERT_TRUE(g.has_value()) << static_cast<int>(kind) << " " << g.error().to_string();
    for (const double v : {g->pv, g->delta, g->gamma, g->vega, g->volga,
                           g->vanna, g->theta, g->rho, g->charm}) {
      EXPECT_TRUE(std::isfinite(v)) << static_cast<int>(kind);
    }
  }
}

// Task F-5, census site S4 (`analytic_in_scope`, derivatives.cpp): P-4's
// analytic-strip closed form is scoped to `DerivKind::VarSwap` by a `kind ==`
// WHITELIST, which -Wswitch cannot police -- a new kind falls to finite
// differences silently, and silently-correct is still unverified. Verified BY
// TEST rather than by reading the predicate, exactly as
// `Corridor.AnalyticStripMethodFallsBackToFiniteDifference` does for F-3's
// kind: requesting AnalyticStrip on a variance option must return BIT-IDENTICAL
// greeks to the default FD method, because the same FD code produced both.
TEST(DerivGreeks, VarianceOptionsFallBackToFiniteDifference) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  for (const DerivKind kind : {DerivKind::VarianceCall, DerivKind::VariancePut}) {
    DerivContract c{};
    c.kind = kind;
    c.maturity_t = 0.25;
    c.notional = 1e6;
    c.strike_dec = 0.045;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;
    c.rv_spec.n_obs_done = 21u;
    c.rv_spec.rv_done_dec = 0.0324;

    DerivGreekBumps fd{};
    fd.method = DerivGreekMethod::FiniteDifference;
    DerivGreekBumps an{};
    an.method = DerivGreekMethod::AnalyticStrip;

    const auto g_fd = deriv_greeks(surf, cs, c, deriv_default_config(), fd);
    const auto g_an = deriv_greeks(surf, cs, c, deriv_default_config(), an);
    ASSERT_TRUE(g_fd.has_value()) << g_fd.error().to_string();
    ASSERT_TRUE(g_an.has_value()) << g_an.error().to_string();
    EXPECT_EQ(g_an->pv, g_fd->pv) << static_cast<int>(kind);
    EXPECT_EQ(g_an->delta, g_fd->delta) << static_cast<int>(kind);
    EXPECT_EQ(g_an->gamma, g_fd->gamma) << static_cast<int>(kind);
    EXPECT_EQ(g_an->vega, g_fd->vega) << static_cast<int>(kind);
    EXPECT_EQ(g_an->volga, g_fd->volga) << static_cast<int>(kind);
    EXPECT_EQ(g_an->vanna, g_fd->vanna) << static_cast<int>(kind);
    EXPECT_EQ(g_an->theta, g_fd->theta) << static_cast<int>(kind);
    EXPECT_EQ(g_an->rho, g_fd->rho) << static_cast<int>(kind);
    EXPECT_EQ(g_an->charm, g_fd->charm) << static_cast<int>(kind);
    // Not vacuous: the greeks are real numbers, and a variance call has
    // strictly positive vega (more implied variance is worth more to it).
    EXPECT_TRUE(std::isfinite(g_fd->vega));
    EXPECT_NE(g_fd->vega, 0.0);
  }
}

// PricedSurface-native overload works end to end.
TEST(DerivGreeks, PricedSurfaceOverload) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(9, 100.0, 100.0, 0.30);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.35;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 88u;
  const auto g = atx::vol::deriv_greeks(ps, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  // The fixture surface is flat at 30 vol and carries kFixtureRate, so the
  // analytic var-swap vega is 2*sigma*df*N with df read off that same rate.
  const double df = std::exp(-atx::vol::testkit::kFixtureRate * 0.35);
  const double vega_expected = 2.0 * 0.30 * 1e6 * df;
  EXPECT_NEAR(g->vega, vega_expected, 0.05 * vega_expected);
}

// ── Task P-3 / GK-P2: greek bump table read-vector cache ──────────────────
//
// `eval_bump_table`'s six (k_shift, T) buckets memoize the surface read
// every bumped evaluation shares with its siblings (see `BumpReadCache` /
// `CachedBumpView`, derivatives.cpp). `set_bump_read_cache_disabled_for_test`
// forces every evaluation back onto a live read (the pre-P-3 RespotView+
// VolShiftView numerics), so these tests run the SAME contract/bumps through
// both and assert EXACT bit equality on every output greek -- proving the
// cache changes nothing but how many times the surface is actually read.
namespace {

// Always restores the cached (production default) path on scope exit, even
// across an ASSERT_* early return.
struct BumpCacheResetGuard {
  ~BumpCacheResetGuard() { atx::vol::detail::set_bump_read_cache_disabled_for_test(false); }
};

// EXPECT_EQ on NaN is always false (IEEE 754), so a field this task's stencils
// legitimately leave NaN -- vanna/charm when second_order is off, theta_carry/
// theta_zero_fixing when carry_theta is off or the roll cannot happen -- needs
// an isnan-first compare, not a raw equality.
void expect_eq_or_both_nan(double a, double b, const char *field) {
  if (std::isnan(a)) {
    EXPECT_TRUE(std::isnan(b)) << field << ": uncached=NaN cached=" << b;
  } else {
    EXPECT_EQ(a, b) << field;
  }
}

void expect_greeks_cache_matches_uncached(const atx::vol::PricedSurface &ps,
                                          const DerivContract &contract, const DerivConfig &cfg,
                                          const DerivGreekBumps &bumps) {
  atx::vol::detail::set_bump_read_cache_disabled_for_test(true);
  const auto g_uncached = atx::vol::deriv_greeks(ps, contract, cfg, bumps);
  atx::vol::detail::set_bump_read_cache_disabled_for_test(false);
  const auto g_cached = atx::vol::deriv_greeks(ps, contract, cfg, bumps);

  ASSERT_TRUE(g_uncached.has_value()) << g_uncached.error().to_string();
  ASSERT_TRUE(g_cached.has_value()) << g_cached.error().to_string();
  expect_eq_or_both_nan(g_uncached->pv, g_cached->pv, "pv");
  expect_eq_or_both_nan(g_uncached->delta, g_cached->delta, "delta");
  expect_eq_or_both_nan(g_uncached->gamma, g_cached->gamma, "gamma");
  expect_eq_or_both_nan(g_uncached->vega, g_cached->vega, "vega");
  expect_eq_or_both_nan(g_uncached->theta, g_cached->theta, "theta");
  expect_eq_or_both_nan(g_uncached->rho, g_cached->rho, "rho");
  expect_eq_or_both_nan(g_uncached->vanna, g_cached->vanna, "vanna");
  expect_eq_or_both_nan(g_uncached->volga, g_cached->volga, "volga");
  expect_eq_or_both_nan(g_uncached->charm, g_cached->charm, "charm");
  expect_eq_or_both_nan(g_uncached->theta_carry, g_cached->theta_carry, "theta_carry");
  expect_eq_or_both_nan(g_uncached->theta_zero_fixing, g_cached->theta_zero_fixing,
                        "theta_zero_fixing");
}

}  // namespace

// VarSwap: every one of the up to 14 bumped evaluations runs a FULL strip
// (price_var_swap always quadratures), so this exercises the cache's biggest
// dedup opportunity -- all six read-vector buckets, each shared by up to
// three evaluations. Covers both the flat and the genuinely skewed
// PricedSurface fixture, default bumps (second_order + carry_theta both on).
TEST(DerivGreeks, ReadCacheMatchesUncachedVarSwapFlatAndSkew) {
  BumpCacheResetGuard guard;
  const atx::vol::PricedSurface flat = atx::vol::testkit::make_flat_surface(40, 100.0, 100.0, 0.22);
  const atx::vol::PricedSurface skew = atx::vol::testkit::make_skewed_surface(41, 100.0, 100.0);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.35;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;

  const DerivConfig cfg = deriv_default_config();
  const DerivGreekBumps bumps{};  // second_order = true, carry_theta = true

  expect_greeks_cache_matches_uncached(flat, c, cfg, bumps);
  expect_greeks_cache_matches_uncached(skew, c, cfg, bumps);
}

// VolSwap, unaged: the CENTER's Carr-Lee branch reads only the ATM point
// (see `price_vol_swap`'s unaged dispatch, `is_bumped_greek_view` skips its
// diagnostic strip on every bumped view) -- a read pattern with far FEWER
// per-evaluation reads than VarSwap's full strip, and carry_theta's extra
// K_var_future strip (run on the UNWRAPPED surface, not cached) exercises a
// mixed cached/uncached call within one `eval_bump_table` invocation. Proves
// the cache is correct regardless of how many reads a given DerivKind's
// dispatch performs, not just in the "every call is a full strip" case above.
TEST(DerivGreeks, ReadCacheMatchesUncachedVolSwapUnagedCarryTheta) {
  BumpCacheResetGuard guard;
  const atx::vol::PricedSurface flat = atx::vol::testkit::make_flat_surface(42, 100.0, 100.0, 0.22);
  const atx::vol::PricedSurface skew = atx::vol::testkit::make_skewed_surface(43, 100.0, 100.0);

  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = 0.35;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;  // unaged, n_total > 1 -- carry_theta fully engages

  const DerivConfig cfg = deriv_default_config();
  const DerivGreekBumps bumps{};

  expect_greeks_cache_matches_uncached(flat, c, cfg, bumps);
  expect_greeks_cache_matches_uncached(skew, c, cfg, bumps);
}

// CappedVarSwap / CappedVolSwap: a third, independent dispatch family (the
// displaced-lognormal / split-domain-quadrature closed forms), plus
// second_order OFF and carry_theta OFF variants -- proving the cache is a
// correct no-op difference on the SMALLER bump tables those flags produce
// too, not just the maximal 14-evaluation table.
TEST(DerivGreeks, ReadCacheMatchesUncachedCappedKindsAndReducedBumpFlags) {
  BumpCacheResetGuard guard;
  const atx::vol::PricedSurface skew = atx::vol::testkit::make_skewed_surface(44, 100.0, 100.0);

  DerivContract capped_var{};
  capped_var.kind = DerivKind::CappedVarSwap;
  capped_var.maturity_t = 0.35;
  capped_var.notional = 1e6;
  capped_var.cap_dec = 0.10;  // variance cap, well above the fixture's K_var
  capped_var.rv_spec.annualization = 252.0;
  capped_var.rv_spec.n_obs_total = 63u;

  DerivContract capped_vol = capped_var;
  capped_vol.kind = DerivKind::CappedVolSwap;
  capped_vol.cap_dec = 0.40;  // vol cap

  const DerivConfig cfg = deriv_default_config();
  expect_greeks_cache_matches_uncached(skew, capped_var, cfg, DerivGreekBumps{});
  expect_greeks_cache_matches_uncached(skew, capped_vol, cfg, DerivGreekBumps{});

  DerivGreekBumps no_second_order{};
  no_second_order.second_order = false;
  expect_greeks_cache_matches_uncached(skew, capped_var, cfg, no_second_order);

  DerivGreekBumps no_carry_theta{};
  no_carry_theta.carry_theta = false;
  expect_greeks_cache_matches_uncached(skew, capped_vol, cfg, no_carry_theta);
}

// Fully aged with time still to run: PV(t) = e^{-r(T-t)}*X is a pure discount,
// so the two time greeks are analytic AND mutually consistent -- rho = -T*PV
// and theta = r*PV are one identity differentiated two ways. A fast path that
// returned theta = 0 alongside rho = -T*PV would be self-contradictory: a PV
// that discounts must also accrete.
TEST(DerivGreeks, FullyAgedWithRateAccretesAtTheCarry) {
  const double r = 0.043, T = 0.25;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;  // sqrt = 0.21, so the swap is 3 vols in the money
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  ASSERT_GT(g->pv, 0.0);  // an in-the-money leg, so the identities have signal
  EXPECT_NEAR(g->theta, r * g->pv, 1e-9 * std::fabs(r * g->pv));
  EXPECT_DOUBLE_EQ(g->rho, -T * g->pv);
  EXPECT_EQ(g->delta, 0.0);
  EXPECT_EQ(g->gamma, 0.0);
  EXPECT_EQ(g->vega, 0.0);
  EXPECT_EQ(g->volga, 0.0);
  EXPECT_EQ(g->vanna, 0.0);
  EXPECT_EQ(g->charm, 0.0);
}

// Fully aged AND past its own maturity marker (an expired lot not yet rolled
// off the book). rho = -T*PV must not sign-flip on negative T: clamp T to 0
// so an already-settled lot reports zero rate sensitivity instead of
// -(-0.01)*PV, which would fabricate a small positive rho out of a lot that
// has nothing left to discount. (PV-9)
TEST(DerivGreeks, FullyAgedNegativeMaturityClampsRhoToZero) {
  const double r = 0.043;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = -0.01;  // expired lot
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;  // sqrt = 0.21, so pv != 0 -- a real claim
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_NE(g->pv, 0.0);
  EXPECT_EQ(g->rho, 0.0);
}

// High-vol regime: sigma*sqrt(T) = 0.35 > 0.25, so the E2 adaptive-wing rescale
// is ACTIVE and its node count is a ceil() of a vol-dependent quantity. Without
// pinning the center's grid, a bumped evaluation can land on a different node
// count than the center and the second-order stencils then difference a step in
// the quadrature rather than a change in the price -- which shows up as a
// wildly inflated gamma on a surface whose PV does not depend on spot at all.
TEST(DerivGreeks, HighVolRegimeGridPinKeepsSecondOrderSane) {
  const double sigma = 0.35, T = 1.0, N = 1e5, S = 100.0;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(S, 0.01, 1.00);
  for (const DerivKind kind : {DerivKind::VarSwap, DerivKind::VolSwap,
                               DerivKind::CappedVarSwap, DerivKind::CappedVolSwap}) {
    DerivContract c{};
    c.kind = kind;
    c.maturity_t = T;
    c.notional = N;
    c.strike_dec = 0.03;
    c.cap_dec = (kind == DerivKind::CappedVarSwap)   ? 0.25
                : (kind == DerivKind::CappedVolSwap) ? 0.50
                                                     : 0.0;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;
    c.rv_spec.n_obs_done = 21u;
    c.rv_spec.rv_done_dec = 0.05;
    const auto g = deriv_greeks(surf, cs, c);
    ASSERT_TRUE(g.has_value()) << static_cast<int>(kind);
    for (const double v : {g->pv, g->delta, g->gamma, g->vega, g->volga,
                           g->vanna, g->theta, g->rho, g->charm}) {
      EXPECT_TRUE(std::isfinite(v)) << static_cast<int>(kind);
    }
    // The strip must report the grid it used, or there is nothing to pin. The
    // measured grid here is 361 nodes over [-2.1, 2.1] -- NOT the Standard tier
    // default of 257 over [-1.5, 1.5] -- which is the proof that this contract
    // really does exercise the adaptive rescale this test exists to guard.
    EXPECT_GT(g->quote.strip_nodes_used, 0u) << static_cast<int>(kind);
    EXPECT_TRUE(std::isfinite(g->quote.strip_k_lo_used)) << static_cast<int>(kind);
    EXPECT_TRUE(std::isfinite(g->quote.strip_k_hi_used)) << static_cast<int>(kind);
    EXPECT_GT(g->quote.strip_nodes_used, 257u) << static_cast<int>(kind);

    // CONTAMINATION TRIPWIRE. On a flat surface the strip is scale-invariant
    // and Carr-Lee's K_vol is forward-independent, so PV does not depend on
    // spot at all for ANY of the four kinds: the true gamma and vanna are
    // EXACTLY zero and every nonzero digit is numerical.
    //
    // Bound derivation: measured artifacts are |gamma| <= 2.2e-07 and |vanna|
    // <= 6.9e-07, which is the expected cancellation floor (three PVs of ~1.2e4
    // differenced, ~2.7e-12 of ULP noise, divided by ds^2 = 1e-4). A node-count
    // flip between bumped evaluations moves K_var by roughly the strip's own
    // Richardson error and lands gamma near 1e-1 -- five orders ABOVE this
    // bound. 1e-3 therefore sits ~4 orders above the noise (so FP evaluation
    // order, incl. the scalar-vs-AVX2 CI legs, cannot trip it) and ~2 orders
    // below the artifact it is built to catch.
    EXPECT_LT(std::fabs(g->gamma), 1.0e-3) << static_cast<int>(kind);
    EXPECT_LT(std::fabs(g->vanna), 1.0e-3) << static_cast<int>(kind);
  }
}

// second_order off: vanna and charm are the only greeks with evaluations of
// their own, so they go NaN; gamma and volga ride the first-order stencils and
// must survive.
TEST(DerivGreeks, SecondOrderOffLeavesOnlyVannaAndCharmNaN) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);
  DerivGreekBumps bumps{};
  bumps.second_order = false;
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isnan(g->vanna));
  EXPECT_TRUE(std::isnan(g->charm));
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->vega));
  EXPECT_TRUE(std::isfinite(g->volga));
  EXPECT_TRUE(std::isfinite(g->theta));
  EXPECT_TRUE(std::isfinite(g->rho));
}

// A contract too close to expiry to roll: the theta stencil would land at or
// past T = 0, where an unaged var swap has no future leg to price. Report those
// two as not-computed rather than failing the whole block.
TEST(DerivGreeks, RollPastExpiryLeavesThetaAndCharmNaN) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivGreekBumps bumps{};
  const DerivContract c = var_swap_at(0.5 * bumps.time_years);  // half a day out
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isnan(g->theta));
  EXPECT_TRUE(std::isnan(g->charm));
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->vega));
  EXPECT_TRUE(std::isfinite(g->volga));
  EXPECT_TRUE(std::isfinite(g->vanna));
  EXPECT_TRUE(std::isfinite(g->rho));
}

// Bump sizes and the spot divisor are caller inputs, validated at the boundary.
TEST(DerivGreeks, RejectsUnusableBumpsAndSpot) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);

  DerivGreekBumps zero_spot{};
  zero_spot.spot_rel = 0.0;
  const auto g0 = deriv_greeks(surf, cs, c, deriv_default_config(), zero_spot);
  ASSERT_FALSE(g0.has_value());
  EXPECT_EQ(g0.error().code(), ErrorCode::InvalidArgument);

  DerivGreekBumps whole_spot{};
  whole_spot.spot_rel = 1.0;  // would take the down-bumped spot to exactly 0
  EXPECT_FALSE(deriv_greeks(surf, cs, c, deriv_default_config(), whole_spot).has_value());

  DerivGreekBumps neg_time{};
  neg_time.time_years = -1.0;
  EXPECT_FALSE(deriv_greeks(surf, cs, c, deriv_default_config(), neg_time).has_value());

  CurveSet no_spot = make_flat_curves(100.0, 0.01, 1.00);
  no_spot.spot = 0.0;  // delta's divisor
  const auto gs = deriv_greeks(surf, no_spot, c);
  ASSERT_FALSE(gs.has_value());
  EXPECT_EQ(gs.error().code(), ErrorCode::InvalidArgument);
}

// A vol_abs bump >= the surface's own ATM vol pushes v_dn's down-shifted iv
// to <= 0 -- a silently-corrupted node the downstream strip resolves rather
// than errors on, hollowing out vega/volga/vanna with no visible signal.
// Reject up front off a cheap single sigma_atm read at k=0. (GK-C7)
TEST(DerivGreeks, RejectsVolAbsBumpAtOrAboveAtmVol) {
  const EssviSurface surf = make_flat_surface(0.10, 0.01, 1.00);  // 0.10-vol surface
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);

  DerivGreekBumps too_big{};
  too_big.vol_abs = 0.15;  // >= sigma_atm = 0.10
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), too_big);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error().code(), ErrorCode::InvalidArgument);
}

// Charm's sign on a negative-skew surface, cross-checked against an INDEPENDENT
// maturity difference.
//
// Why the sign is not readable off the skew alone: delta = N*df*(dK_var/dk)/S,
// and to leading order dK_var/dk = w'(0)/T = theta*rho*phi/T. eSSVI's theta is
// proportional to T on this surface, so that leading term is T-INDEPENDENT and
// contributes nothing to d(delta)/dT. Charm is therefore set entirely by the
// O(T) smile-curvature corrections (w'' and w''' averaged over an integration
// width that grows like sigma*sqrt(T)), whose net sign is not something to
// assert from a hand expansion. So: pin the sign the implementation produces,
// and independently corroborate it with a coarse d(delta)/dT taken across a
// 0.10-year maturity span -- a completely different difference from charm's own
// one-day roll, which a stencil sign error could not survive.
TEST(DerivGreeks, CharmSignOnSkewedSurfaceMatchesMaturitySlope) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto delta_at = [&surf, &cs](double T) -> double {
    const auto g = deriv_greeks(surf, cs, var_swap_at(T));
    if (!g.has_value()) {
      ADD_FAILURE() << "deriv_greeks failed at T=" << T << ": " << g.error().to_string();
      return 0.0;
    }
    return g->delta;
  };

  const auto g = deriv_greeks(surf, cs, var_swap_at(0.25));
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  // Measured: delta = -363.08, which confirms the leading-order account above
  // to ~1% (N*sigma^2*rho*phi/S = 1e6*0.04*(-0.6)*1.5/100 = -360).
  EXPECT_LT(g->delta, 0.0);

  // Measured: charm = +12.3766. POSITIVE -- the skew-driven short spot exposure
  // DECAYS toward zero as expiry approaches (delta rises from -363 toward 0),
  // so a hedger of a long negative-skew var swap buys back stock as the
  // contract runs off. Equivalently d(delta)/dT < 0: delta gets more negative
  // the longer the contract, the O(T) curvature correction adding to the
  // T-independent leading term.
  EXPECT_GT(g->charm, 0.0);

  // charm = d(delta)/dt and t runs opposite to T, so charm == -d(delta)/dT.
  // Measured agreement is 6.5e-5 relative between a one-DAY roll and this
  // 0.10-YEAR difference; 2e-2 leaves ~300x margin for the coarse stencil's own
  // O(h^2) error while still catching any sign or scale error.
  const double d_delta_dT = (delta_at(0.30) - delta_at(0.20)) / 0.10;
  EXPECT_LT(d_delta_dT, 0.0);
  EXPECT_NEAR(g->charm, -d_delta_dT, 2e-2 * std::fabs(d_delta_dT));
}

// Vega's bump size must not matter on a surface where PV is NOT quadratic in
// the vol shift.
//
// The flat-surface test above is exact but structurally lucky: there
// PV = N*sigma^2, so a central difference is exact at ANY bump size and the
// test cannot see a badly-sized stencil. On the skewed surface PV picks up
// genuine higher-order dependence on a parallel shift, so agreement between a
// 1e-4 and a 1e-2 bump is a real statement that the stencil is converged.
//
// (An independently-constructed shifted reference -- the flat test's two
// separate surfaces -- is not available here: a parallel ADDITIVE shift of
// iv(k,T) is not representable in the eSSVI parametrization, and deriv_price's
// template body lives in the .cpp, so a test-local shifted view cannot
// instantiate it.)
TEST(DerivGreeks, VegaIsBumpSizeIndependentOnSkewedSurface) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);

  const auto g_small = deriv_greeks(surf, cs, c);  // default vol_abs = 1e-4
  DerivGreekBumps big{};
  big.vol_abs = 1.0e-2;  // 100x
  const auto g_big = deriv_greeks(surf, cs, c, deriv_default_config(), big);
  ASSERT_TRUE(g_small.has_value());
  ASSERT_TRUE(g_big.has_value());
  EXPECT_GT(std::fabs(g_small->vega), 1.0);  // there is something to compare
  EXPECT_NEAR(g_small->vega, g_big->vega, 3e-2 * std::fabs(g_small->vega));
}

// Task P-2 / GK-C3: rho is EXACTLY -T*PV for every DerivKind and aging state,
// not just the fully-aged branch's own closed form (T clamped to >= 0 --
// fix round 1, C-1: a cap-pinned quote can succeed at T <= 0 without being
// FullyAged, where the true dPV/dr is 0, not a sign-flipped -T*PV). Every
// quote in this file is built as `pv = df(r) * X`, and X -- the
// fair-strike/expectation/cap-option blend -- never reads the rate curve:
// the strip's own OTM(K)/df integrand cancels its discount factor
// (Demeterfi-Derman-Kamal-Zou), the Carr-Lee ATMF straddle formula never
// touches `curves.yield`, and the Diffusion1OverN discrete-monitoring
// correction's carry differential (`resolve_carry_diff`) is read off the
// FORWARD and spot, never the yield curve either. So dPV/dr = X * d(df)/dr =
// -T*df*X = -T*PV identically (T >= 0), and the one-sided FD r+ bump this
// file used to pay for (a full extra repricing -- a second strip integration
// for VarSwap/CappedVarSwap/CappedVolSwap, a second Carr-Lee straddle plus
// its own diagnostic strip for VolSwap) only ever recomputed that identity
// the hard way, to FD precision, never deriving anything new.
//
// INDEPENDENT ORACLE (fix round 1, I-1): the FIRST version of this test
// compared `g->rho` against `-c.maturity_t * g->pv` computed IN THE TEST --
// the identical expression `deriv_greeks` itself now evaluates
// (`g.rho = -std::fmax(contract.maturity_t, 0.0) * center.pv`,
// `g.pv = center.pv`), making the comparison tautological and blind to any
// bug in that expression (it missed C-1's missing clamp entirely: the
// self-referential oracle clamps nowhere, so it "agreed" with the unclamped
// bug). The oracle here instead differences TWO direct `deriv_price` calls,
// through the ordinary PUBLIC entry point, at curves built independently at
// r -/+ dr/2 (`make_flat_curves`'s own `rate` parameter) -- sharing no
// expression, and no code path inside `deriv_greeks`/`eval_bump_table`, with
// production rho. `dr = 1e-6` keeps the central difference's own O(dr^2)
// truncation far below the 1e-6*|PV| bound (and is exact, not approximate,
// for every T <= 0 case below: `deriv_df_at_T` returns the same constant
// `df = 1.0` at both r -/+ dr/2, so the independent FD is EXACTLY 0 there,
// not merely converged).
//
// The matrix covers kind x age x T-sign: the ordinary unaged/mid-life case
// for all four kinds at T = 0.25, PLUS (C-1's own regression) a cap-pinned,
// PARTIALLY aged (not FullyAged), EXPIRED (T < 0) CappedVarSwap and
// CappedVolSwap -- exactly the configuration `price_capped_var_swap`'s /
// `price_capped_vol_swap`'s cap-pin exit reaches without ever checking
// T > 0 or setting `DerivFlags::FullyAged`.
TEST(Rho, AnalyticMatchesFD) {
  const EssviSurface surf = make_skewed_surface();
  constexpr double kR = 0.03;
  constexpr double kDr = 1.0e-6;  // independent-oracle central-difference step
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, kR);
  const CurveSet cs_lo = make_flat_curves(100.0, 0.01, 1.00, kR - 0.5 * kDr);
  const CurveSet cs_hi = make_flat_curves(100.0, 0.01, 1.00, kR + 0.5 * kDr);

  struct Case {
    DerivKind kind;
    double maturity_t;
    std::uint32_t n_obs_done;
    double cap_dec;
    double strike_dec;
    double rv_done_dec;
    const char *label;
  };
  const Case cases[] = {
      {DerivKind::VarSwap, 0.25, 0u, 0.0, 0.03, 0.05, "VarSwap unaged"},
      {DerivKind::VarSwap, 0.25, 21u, 0.0, 0.03, 0.05, "VarSwap mid-life"},
      {DerivKind::VolSwap, 0.25, 0u, 0.0, 0.03, 0.05, "VolSwap unaged"},
      {DerivKind::VolSwap, 0.25, 21u, 0.0, 0.03, 0.05, "VolSwap mid-life"},
      {DerivKind::CappedVarSwap, 0.25, 0u, 0.25, 0.03, 0.05, "CappedVarSwap unaged"},
      {DerivKind::CappedVarSwap, 0.25, 21u, 0.25, 0.03, 0.05, "CappedVarSwap mid-life"},
      {DerivKind::CappedVolSwap, 0.25, 0u, 0.50, 0.03, 0.05, "CappedVolSwap unaged"},
      {DerivKind::CappedVolSwap, 0.25, 21u, 0.50, 0.03, 0.05, "CappedVolSwap mid-life"},
      // C-1 regression: cap-pinned (accrued >= cap), PARTIALLY aged (21 of 63
      // -- not FullyAged), EXPIRED (T < 0, not yet rolled off the book).
      // w_done = 21/63 = 1/3, accrued = rv_done_dec/3.
      {DerivKind::CappedVarSwap, -0.01, 21u, 0.02, 0.01, 0.10,
       "CappedVarSwap pinned+partially-aged+expired"},
      {DerivKind::CappedVolSwap, -0.01, 21u, 0.10, 0.01, 0.10,
       "CappedVolSwap pinned+partially-aged+expired"},
  };

  for (const Case &tc : cases) {
    DerivContract c{};
    c.kind = tc.kind;
    c.maturity_t = tc.maturity_t;
    c.notional = 1e6;
    c.strike_dec = tc.strike_dec;
    c.cap_dec = tc.cap_dec;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;
    c.rv_spec.n_obs_done = tc.n_obs_done;
    c.rv_spec.rv_done_dec = tc.rv_done_dec;

    const auto g = deriv_greeks(surf, cs, c);
    ASSERT_TRUE(g.has_value()) << tc.label << ": " << g.error().to_string();
    // M-4: a tolerance scaled by |pv| is vacuous at pv == 0 -- assert there is
    // something real to compare against.
    ASSERT_GT(std::fabs(g->pv), 0.0) << tc.label;

    const auto p_lo = deriv_price(surf, cs_lo, c, deriv_default_config());
    const auto p_hi = deriv_price(surf, cs_hi, c, deriv_default_config());
    ASSERT_TRUE(p_lo.has_value()) << tc.label << ": " << p_lo.error().to_string();
    ASSERT_TRUE(p_hi.has_value()) << tc.label << ": " << p_hi.error().to_string();
    const double fd_rho = (p_hi->pv - p_lo->pv) / kDr;

    EXPECT_NEAR(g->rho, fd_rho, 1.0e-6 * std::fabs(g->pv)) << tc.label;
  }
}

// Task C-10 (GK-C2): `theta` rolls ONLY the calendar (T -> T - dt) with
// `rv_spec` held fixed, so it silently omits the implied->realized fixing
// rollover -- the largest deterministic daily P&L term on an unaged/mid-life
// swap (theta reports ~0 on a fair-struck swap even though the fixing roll
// itself is a real, large daily mark move). theta_carry / theta_zero_fixing
// price that roll too, by injecting one extra fixing into a COPY of rv_spec
// before the same T - dt roll `theta` already takes.
TEST(CarryTheta, FairSwapCarryIsDiscountingOnly) {
  const double sigma = 0.20, T = 0.25, N = 1e6, r = 0.03;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);

  // Resolve the strip's own fair strike first (a Simpson quadrature, not
  // exactly sigma^2), then re-price fair-struck so the center's PV is 0 to
  // floating precision and any residual theta_carry is real signal, not
  // off-fair drift.
  DerivContract probe{};
  probe.kind = DerivKind::VarSwap;
  probe.maturity_t = T;
  probe.notional = N;
  probe.rv_spec.annualization = 252.0;
  probe.rv_spec.n_obs_total = 63u;
  const auto probe_q = deriv_price(surf, cs, probe, deriv_default_config());
  ASSERT_TRUE(probe_q.has_value()) << probe_q.error().to_string();
  const double k_var = probe_q->fair_strike_dec;  // unaged: == raw K_var_future
  ASSERT_GT(k_var, 0.0);

  DerivContract c = probe;
  c.strike_dec = k_var;  // fair-struck: center PV == 0
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_NEAR(g->pv, 0.0, 1.0);  // sanity: really fair-struck

  const double df = cs.yield.disc(T);
  // Scale of ONE fixing's mark move: w_future ~= 1/n_total of the future leg
  // (valued at k_var) reweighted onto the accrued leg -- the natural,
  // non-annualized epsilon a "theta_carry is small" claim gets quantified
  // against (see theta_zero_fixing below, which IS this same scale,
  // annualized).
  const double one_fixing_pv = df * N * k_var / c.rv_spec.n_obs_total;

  // theta_carry: the injected fixing lands exactly at today's implied
  // variance rate, so the blend does not move (fair stays fair) and only the
  // discount roll is left -- r*PV, here 0 because a fair-struck swap has
  // nothing to discount. Brief's bound: < 5% of df*N*K_var/n_total.
  EXPECT_NEAR(g->theta_carry, r * g->pv, 0.05 * one_fixing_pv);

  // theta_zero_fixing: the deterministic "nothing happened overnight" mark --
  // one fixing's worth of future-leg weight moves from K_var to a realized
  // zero, annualized by the SAME dt the stencil rolls T by
  // (DerivGreekBumps::time_years -- NOT rv_spec.annualization; the two need
  // not agree, and here time_years = 1/365.25 while annualization = 252, see
  // task-C-10-report.md for the derivation of why the code (and this test)
  // divide by time_years, not a hardcoded 252).
  const DerivGreekBumps bumps{};
  EXPECT_NEAR(g->theta_zero_fixing, -one_fixing_pv / bumps.time_years,
              0.05 * (one_fixing_pv / bumps.time_years));
}

// Pins theta_carry against theta_zero_fixing (and the aged-blend arithmetic
// both ride) via a closed-form difference that is exact by construction,
// independent of n_done, strike_dec, and even the rolled future leg's own
// value -- see task-C-10-report.md for the full derivation. Deliberately
// AGED and OFF-FAIR (unlike the unaged/fair fixture above) so this exercises
// the general blend, not the n_done == 0 degenerate case.
TEST(CarryTheta, SumIdentity) {
  const double sigma = 0.20, T = 0.25, N = 1e6;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = N;
  c.strike_dec = 0.03;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.05;

  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_TRUE(std::isfinite(g->theta_carry));
  ASSERT_TRUE(std::isfinite(g->theta_zero_fixing));

  // theta_carry - theta_zero_fixing == df(T-dt)*N*K_var_future /
  //                                    (n_obs_total * bumps.time_years)
  // Both variants inject exactly one fixing (n_done -> n_done+1) into the
  // SAME blend at the SAME rolled T, differing only in the injected fixing's
  // own value (K_var_future vs 0); every other term -- the rolled future leg
  // K_var_future(T-dt), strike_dec, the n_done-weighted pre-fixing accrual --
  // is common to both reprices and cancels in the difference.
  const double k_var = g->quote.uncapped_var_dec;  // raw K_var_future at T
  ASSERT_GT(k_var, 0.0);
  const DerivGreekBumps bumps{};
  const double df_rolled = cs.yield.disc(T - bumps.time_years);
  const double expected =
      df_rolled * N * k_var / (c.rv_spec.n_obs_total * bumps.time_years);
  EXPECT_NEAR(g->theta_carry - g->theta_zero_fixing, expected, 0.05 * expected);
}

// bumps.carry_theta = false must leave both fields at their NaN default with
// no extra evaluation paid -- the opt-out this knob exists for.
TEST(CarryTheta, OptOutLeavesBothNaN) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);
  DerivGreekBumps bumps{};
  bumps.carry_theta = false;
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isnan(g->theta_carry));
  EXPECT_TRUE(std::isnan(g->theta_zero_fixing));
  EXPECT_TRUE(std::isfinite(g->theta));  // plain theta is unaffected
}

// Fully aged: both carry variants equal theta exactly -- nothing left to
// realize, so there is no fixing roll left to price either (mirrors
// FullyAgedWithRateAccretesAtTheCarry's theta/rho identity above).
TEST(CarryTheta, FullyAgedEqualsTheta) {
  const double r = 0.043, T = 0.25;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_DOUBLE_EQ(g->theta_carry, g->theta);
  EXPECT_DOUBLE_EQ(g->theta_zero_fixing, g->theta);
}

// --- Fix round 1 (review findings) -----------------------------------------

// CRITICAL-1: injecting the carry fixing turns an unaged VolSwap mid-life
// (n_obs_done 0 -> 1), and `price_vol_swap` rejects an EXPLICIT VolCarrLee
// engine mid-life (Carr-Lee cannot blend an accrued leg). Pre-fix, that
// InvalidArgument propagated out of `eval_bump_table` via ATX_TRY and dropped
// the ENTIRE greek block -- delta through charm, not just the two carry
// fields -- for a documented, previously-working configuration (this file's
// own vol-swap dispatch doc: unaged + explicit VolCarrLee is Marquee's own
// inception convention). No existing `deriv_greeks` test set that engine,
// which is why the suite stayed green pre-fix. Pins: the call still succeeds,
// the two carry fields report NaN (honest "not computed" for a diagnostic
// that cannot be priced under the caller's own explicit engine choice), and
// every other greek is unaffected.
TEST(CarryTheta, UnagedVolSwapExplicitCarrLeeKeepsBlockAliveWithCarryFieldsNaN) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = 0.25;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;  // unaged: n_obs_done left at 0

  DerivConfig cfg = deriv_default_config();
  cfg.engine = DerivEngine::VolCarrLee;
  const auto g = deriv_greeks(surf, cs, c, cfg);  // default bumps: carry_theta = true
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  EXPECT_TRUE(std::isnan(g->theta_carry));
  EXPECT_TRUE(std::isnan(g->theta_zero_fixing));
  EXPECT_TRUE(std::isfinite(g->pv));
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->vega));
  EXPECT_TRUE(std::isfinite(g->volga));
  EXPECT_TRUE(std::isfinite(g->vanna));
  EXPECT_TRUE(std::isfinite(g->theta));
  EXPECT_TRUE(std::isfinite(g->rho));
  EXPECT_TRUE(std::isfinite(g->charm));
}

// IMPORTANT-1: the fully-aged branch returns theta_carry == theta_zero_fixing
// == theta UNCONDITIONALLY -- nothing is realized there, so there is no
// fixing roll left for the knob to gate (the brief itself states "Fully-aged:
// both = theta" with no carve-out). Pins that `carry_theta = false` does NOT
// turn these NaN on that branch, matching the corrected doc on
// `DerivGreeks::theta_carry`. Otherwise identical to `FullyAgedEqualsTheta`.
TEST(CarryTheta, FullyAgedIgnoresCarryThetaOptOut) {
  const double r = 0.043, T = 0.25;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;
  DerivGreekBumps bumps{};
  bumps.carry_theta = false;
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isfinite(g->theta_carry));
  EXPECT_DOUBLE_EQ(g->theta_carry, g->theta);
  EXPECT_DOUBLE_EQ(g->theta_zero_fixing, g->theta);
}

// IMPORTANT-2: on an unaged VolSwap the center prices via Carr-Lee, but the
// injected +1-fixing copy is mid-life and therefore prices via the lognormal
// RV distribution model (`price_vol_swap_distribution`) -- the two carry
// variants momentarily difference PVs from two DIFFERENT pricers, unlike
// `theta` (which never touches `rv_spec` and so never leaves Carr-Lee).
//
// Reference derivation (also recorded on `DerivGreeks::theta_carry` and in
// task-C-10-report.md's "Fix round 1" section): both variants inject the SAME
// b = w_future and share the SAME lognormal W (same rolled T, same
// auto-calibrated xi -- `resolve_vol_of_vol` depends only on the surface/T,
// not on `a`), differing only in `a_carry = K_var_future/n_total` vs
// `a_zero = 0`. Since a_carry is a SMALL perturbation (1/n_total of the
// blend), a first-order Taylor expansion of sqrt(a+bW) around a = 0 gives
//   theta_carry - theta_zero_fixing
//       ~= df(T-dt)*N*K_var_future / (2*K_vol*n_total*bumps.time_years)
// (the extra 1/(2*K_vol) next to VarSwap's exact SumIdentity is d/da[sqrt] at
// a = 0). This reproduces the reviewer's own worked example almost exactly
// (their "~5.8e4 carry signal" on a 1e5-notional, sigma=0.20, T=0.25 fixture
// is precisely this formula), and the ~0.16% they separately quantify as the
// Jensen-gap residual (E[sqrt] vs sqrt-of-mean, from replacing 1/n_total of
// the lognormal leg with a deterministic one) is what the 1% tolerance below
// leaves room for -- so this is a real tripwire on the model-switch artifact
// staying second-order, not a restatement of the implementation.
TEST(CarryTheta, UnagedVolSwapCarryTracksLinearBlendWithinModelSwitchTolerance) {
  const double sigma = 0.20, T = 0.25, N = 1e6;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = N;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;  // unaged: n_obs_done left at 0

  const auto g = deriv_greeks(surf, cs, c);  // default config: Auto engine, auto-xi
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_TRUE(std::isfinite(g->theta_carry));
  ASSERT_TRUE(std::isfinite(g->theta_zero_fixing));

  // Best-effort strip diagnostic on the unaged Carr-Lee dispatch quote --
  // populated whenever the strip succeeds (it does, on this flat surface);
  // see price_vol_swap's own comment on `uncapped_var_dec`.
  const double k_var = g->quote.uncapped_var_dec;
  const double k_vol = g->quote.fair_strike_dec;  // Carr-Lee K_vol at the center
  ASSERT_GT(k_var, 0.0);
  ASSERT_GT(k_vol, 0.0);

  const DerivGreekBumps bumps{};
  const double df_rolled = cs.yield.disc(T - bumps.time_years);
  const double expected = df_rolled * N * k_var /
                          (2.0 * k_vol * c.rv_spec.n_obs_total * bumps.time_years);
  EXPECT_GT(expected, 0.0);  // there is something to compare
  EXPECT_NEAR(g->theta_carry - g->theta_zero_fixing, expected, 0.01 * expected);
}

// MUST-FIX 4 (aggregate review): `inject_carry_fixing`'s back-derivation of
// `sum_sq_log_returns_done` divides by `rv.annualization` with no guard --
// the first division on this path with none. `RealizedTracker::create`
// validates annualization > 0, but a hand-built `DerivContract` (every
// fixture in this file, and any caller who does not go through the tracker)
// bypasses that entirely, so annualization == 0 reaches this division
// unchecked and would write +-inf/NaN into the injected copy. No pricer in
// this file reads `sum_sq_log_returns_done` back (see the comment on
// `inject_carry_fixing`), so the failure mode was latent, not a wrong greek
// -- but this pins that the guard is in place and the block stays entirely
// finite regardless.
TEST(CarryTheta, ZeroAnnualizationDoesNotPoisonInjectedFixing) {
  const double sigma = 0.20, T = 0.25;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 0.0;  // hand-built: bypasses RealizedTracker::create
  c.rv_spec.n_obs_total = 63u;    // unaged: n_obs_done left at 0

  const auto g = deriv_greeks(surf, cs, c);  // default bumps: carry_theta == true
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isfinite(g->theta_carry));
  EXPECT_TRUE(std::isfinite(g->theta_zero_fixing));
  EXPECT_TRUE(std::isfinite(g->pv));
  EXPECT_TRUE(std::isfinite(g->theta));
}

// ── Task P-4 / GK-P: analytic strip greeks (opt-in, FD-parity-gated) ──────
//
// `DerivGreekMethod::AnalyticStrip` differentiates the model-free variance
// strip's own closed form (delta/gamma/vega/vanna/volga) instead of
// repricing it under a bump -- `DerivKind::VarSwap` only (uncapped, any
// age); every other kind falls back to `FiniteDifference` silently. Parity
// gate: |analytic - FD| <= max(1e-8*scale, 5*kFdNoiseFloor), kFdNoiseFloor
// the measured FD-cancellation bound `HighVolRegimeGridPinKeepsSecondOrder
// Sane` (above) pins (~2.2e-7) -- gamma/vanna/volga get a genuinely
// non-flat oracle here for the first time: the analytic form IS the oracle,
// and the FD path (already exercised by every other test in this file)
// cross-checks it, two independent constructions of the same number.
//
// `scale` is the CONTRACT's own economic scale (`notional`), the SAME gate
// applied to all five greeks in one case, rather than each greek's own raw
// FD value: vanna is a mixed second partial FD's own stencil differences
// four PVs and divides by `4*ds*dv` (ds, dv both O(1e-4) or smaller), so its
// OWN cancellation noise is larger than a plain central difference's and,
// like `HighVolRegimeGridPinKeepsSecondOrderSane`'s own measured bound,
// scales with the contract's notional (bigger PVs -> bigger absolute ULP
// noise from the same relative cancellation) -- `kFdNoiseFloor` was measured
// there at notional 1e5; this suite's fixtures run at 1e6, so the floor is
// scaled by that same 10x. Gating each greek on ITS OWN (possibly near-zero,
// noise-dominated) FD value instead would make the tolerance for a
// near-flat vanna arbitrarily tight for no principled reason -- the
// CONTRACT's scale, not any one greek's incidental size, is what "scale" in
// the brief's formula means here.
constexpr double kFdNoiseFloor = 2.2e-7;

double parity_gate(double scale) {
  return std::max(1.0e-8 * scale, 5.0 * kFdNoiseFloor * (scale / 1.0e5));
}

// One (fixture, age) case: analytic vs FD on the SAME contract. PV / rho /
// theta / theta_carry / theta_zero_fixing are asserted BIT-IDENTICAL --
// `method` only ever branches inside `deriv_greeks`'s delta/gamma/vega/
// vanna/volga assignment (see derivatives.cpp), so everything else runs the
// identical code either way. charm IS affected (Review fix round 1, I-2 --
// the CHANGELOG previously claimed otherwise): it differences an FD-rolled
// delta at T - dt (identical under either method) against `g.delta`, which
// IS the method-dependent value -- so |charm_an - charm_fd| =
// |delta_an - delta_fd| / bumps.time_years EXACTLY, a provable (not merely
// empirical) consequence of delta's own gate above, checked below rather
// than left as a liveness-only assertion.
void expect_analytic_matches_fd(const EssviSurface &surf, const CurveSet &cs,
                                 const DerivContract &c, const char *label) {
  DerivGreekBumps fd_bumps{};
  DerivGreekBumps an_bumps{};
  an_bumps.method = DerivGreekMethod::AnalyticStrip;

  const auto g_fd = deriv_greeks(surf, cs, c, deriv_default_config(), fd_bumps);
  const auto g_an = deriv_greeks(surf, cs, c, deriv_default_config(), an_bumps);
  ASSERT_TRUE(g_fd.has_value()) << label << ": " << g_fd.error().to_string();
  ASSERT_TRUE(g_an.has_value()) << label << ": " << g_an.error().to_string();

  EXPECT_EQ(g_an->pv, g_fd->pv) << label;
  EXPECT_EQ(g_an->rho, g_fd->rho) << label;
  EXPECT_EQ(g_an->theta, g_fd->theta) << label;
  if (std::isnan(g_fd->theta_carry)) {
    EXPECT_TRUE(std::isnan(g_an->theta_carry)) << label;
  } else {
    EXPECT_EQ(g_an->theta_carry, g_fd->theta_carry) << label;
  }
  if (std::isnan(g_fd->theta_zero_fixing)) {
    EXPECT_TRUE(std::isnan(g_an->theta_zero_fixing)) << label;
  } else {
    EXPECT_EQ(g_an->theta_zero_fixing, g_fd->theta_zero_fixing) << label;
  }

  const double gate = parity_gate(c.notional);
  EXPECT_NEAR(g_an->delta, g_fd->delta, gate) << label << " delta";
  EXPECT_NEAR(g_an->gamma, g_fd->gamma, gate) << label << " gamma";
  EXPECT_NEAR(g_an->vega, g_fd->vega, gate) << label << " vega";
  EXPECT_NEAR(g_an->vanna, g_fd->vanna, gate) << label << " vanna";
  EXPECT_NEAR(g_an->volga, g_fd->volga, gate) << label << " volga";

  // Review fix round 1, I-2: charm's own gate, derived (not guessed) from
  // delta's -- see this function's own doc above for the exact identity.
  EXPECT_TRUE(std::isfinite(g_an->charm)) << label;
  EXPECT_NEAR(g_an->charm, g_fd->charm, gate / fd_bumps.time_years) << label << " charm";
}

TEST(AnalyticGreeks, MatchesFDFlatUnaged) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  const DerivContract c = var_swap_at(0.25);
  expect_analytic_matches_fd(surf, cs, c, "flat unaged");
}

TEST(AnalyticGreeks, MatchesFDFlatMidLife) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c = var_swap_at(0.25);
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.05;
  expect_analytic_matches_fd(surf, cs, c, "flat mid-life");
}

TEST(AnalyticGreeks, MatchesFDSkewUnaged) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  const DerivContract c = var_swap_at(0.25);
  expect_analytic_matches_fd(surf, cs, c, "skew unaged");
}

TEST(AnalyticGreeks, MatchesFDSkewMidLife) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c = var_swap_at(0.25);
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.05;
  expect_analytic_matches_fd(surf, cs, c, "skew mid-life");
}

// High-vol / wing-clamped regime (same fixture as
// HighVolRegimeGridPinKeepsSecondOrderSane above): sigma*sqrt(T) = 0.35 puts
// the E2 adaptive-wing rescale AND the wing clamp both in play, so the strip
// splits into (up to) four panels -- [k_lo,-0.5], [-0.5,0], [0,0.5],
// [0.5,k_hi] -- exercising this task's multi-panel walk and the flat-tail
// sigma'=sigma''=0 convention inside the clamp, not just the plain k=0 split
// the flat/skew cases above take.
TEST(AnalyticGreeks, MatchesFDHighVolWingClamped) {
  const EssviSurface surf = make_flat_surface(0.35, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c = var_swap_at(1.0);
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.05;
  expect_analytic_matches_fd(surf, cs, c, "high-vol wing-clamped");
}

// PricedSurface-native path -- the production entry point, and the ONE
// fixture in this suite whose surface exposes a batched `iv_batch`
// (`PricedSurfaceStripView`), so it is the only case here exercising this
// task's batched read branch (`has_analytic_iv_batch`) rather than the
// scalar per-node fallback every EssviSurface case above takes.
TEST(AnalyticGreeks, MatchesFDPricedSurfaceSkewed) {
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_skewed_surface(21, 100.0, 100.0);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.35;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 88u;

  DerivGreekBumps fd_bumps{};
  DerivGreekBumps an_bumps{};
  an_bumps.method = DerivGreekMethod::AnalyticStrip;
  const auto g_fd = atx::vol::deriv_greeks(ps, c, DerivConfig{}, fd_bumps);
  const auto g_an = atx::vol::deriv_greeks(ps, c, DerivConfig{}, an_bumps);
  ASSERT_TRUE(g_fd.has_value()) << g_fd.error().to_string();
  ASSERT_TRUE(g_an.has_value()) << g_an.error().to_string();
  EXPECT_EQ(g_an->pv, g_fd->pv);
  const double gate = parity_gate(c.notional);
  EXPECT_NEAR(g_an->delta, g_fd->delta, gate);
  EXPECT_NEAR(g_an->gamma, g_fd->gamma, gate);
  EXPECT_NEAR(g_an->vega, g_fd->vega, gate);
  EXPECT_NEAR(g_an->vanna, g_fd->vanna, gate);
  EXPECT_NEAR(g_an->volga, g_fd->volga, gate);
  // Review fix round 1, I-2: see expect_analytic_matches_fd's own doc for
  // why this ratio is an exact, not merely empirical, consequence of delta's
  // gate above.
  EXPECT_NEAR(g_an->charm, g_fd->charm, gate / fd_bumps.time_years);
}

// Default stays FD -- no mark move for any existing caller (flip evaluated
// no sooner than 2.0, see DerivGreekBumps::method's own doc).
TEST(AnalyticGreeks, DefaultMethodIsFiniteDifference) {
  const DerivGreekBumps bumps{};
  EXPECT_EQ(bumps.method, DerivGreekMethod::FiniteDifference);
}

// Silent fallback: AnalyticStrip requested on an out-of-scope kind reprices
// EXACTLY like an explicit FiniteDifference request (bit-identical), never a
// half-implemented alternate path.
TEST(AnalyticGreeks, FallsBackToFDForOutOfScopeKinds) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  for (const DerivKind kind :
       {DerivKind::VolSwap, DerivKind::CappedVarSwap, DerivKind::CappedVolSwap}) {
    DerivContract c{};
    c.kind = kind;
    c.maturity_t = 0.25;
    c.notional = 1e6;
    c.strike_dec = 0.03;
    c.cap_dec = (kind == DerivKind::CappedVarSwap)   ? 0.25
                : (kind == DerivKind::CappedVolSwap) ? 0.50
                                                     : 0.0;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;

    DerivGreekBumps fd_bumps{};
    DerivGreekBumps an_bumps{};
    an_bumps.method = DerivGreekMethod::AnalyticStrip;
    const auto g_fd = deriv_greeks(surf, cs, c, deriv_default_config(), fd_bumps);
    const auto g_an = deriv_greeks(surf, cs, c, deriv_default_config(), an_bumps);
    ASSERT_TRUE(g_fd.has_value()) << static_cast<int>(kind);
    ASSERT_TRUE(g_an.has_value()) << static_cast<int>(kind);
    EXPECT_EQ(g_an->pv, g_fd->pv) << static_cast<int>(kind);
    EXPECT_EQ(g_an->delta, g_fd->delta) << static_cast<int>(kind);
    EXPECT_EQ(g_an->gamma, g_fd->gamma) << static_cast<int>(kind);
    EXPECT_EQ(g_an->vega, g_fd->vega) << static_cast<int>(kind);
    EXPECT_EQ(g_an->volga, g_fd->volga) << static_cast<int>(kind);
    if (std::isnan(g_fd->vanna)) {
      EXPECT_TRUE(std::isnan(g_an->vanna)) << static_cast<int>(kind);
    } else {
      EXPECT_EQ(g_an->vanna, g_fd->vanna) << static_cast<int>(kind);
    }
    EXPECT_EQ(g_an->charm, g_fd->charm) << static_cast<int>(kind);
  }
}

// Review fix round 1, C-1: `Diffusion1OverN` adds a term QUADRATIC in K_var
// to the future leg (`price_var_swap`'s discrete-monitoring branch) before
// it becomes the quantity PV is linear in -- the raw-strip closed form this
// task's analytic path differentiates knows nothing about that addend, so
// an in-scope-by-kind VarSwap priced with the correction ON must ALSO fall
// back to FD, bit-identically, exactly like an out-of-scope kind above.
// Covers both a flat and a skewed surface (the reviewer's measured C-1
// evidence showed the bug firing on both, not just skew).
TEST(AnalyticGreeks, FallsBackToFDForDiscreteCorrection) {
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivConfig cfg{};
  cfg.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;
  const DerivContract c = var_swap_at(0.25);  // n_obs_total = 63 -> correction engages

  for (const EssviSurface &surf : {make_flat_surface(0.20, 0.01, 1.00), make_skewed_surface()}) {
    DerivGreekBumps fd_bumps{};
    DerivGreekBumps an_bumps{};
    an_bumps.method = DerivGreekMethod::AnalyticStrip;
    const auto g_fd = deriv_greeks(surf, cs, c, cfg, fd_bumps);
    const auto g_an = deriv_greeks(surf, cs, c, cfg, an_bumps);
    ASSERT_TRUE(g_fd.has_value()) << g_fd.error().to_string();
    ASSERT_TRUE(g_an.has_value()) << g_an.error().to_string();
    EXPECT_EQ(g_an->pv, g_fd->pv);
    EXPECT_EQ(g_an->delta, g_fd->delta);
    EXPECT_EQ(g_an->gamma, g_fd->gamma);
    EXPECT_EQ(g_an->vega, g_fd->vega);
    EXPECT_EQ(g_an->volga, g_fd->volga);
    if (std::isnan(g_fd->vanna)) {
      EXPECT_TRUE(std::isnan(g_an->vanna));
    } else {
      EXPECT_EQ(g_an->vanna, g_fd->vanna);
    }
    EXPECT_EQ(g_an->charm, g_fd->charm);
  }
}

// Fallback selects a METHOD; it never changes what error the kind x engine
// dispatch matrix raises (PV-5). VarSwap x explicit VolCarrLee is
// InvalidArgument under EITHER method -- the same `deriv_price` dispatch
// runs first, unconditionally, regardless of `bumps.method`.
TEST(AnalyticGreeks, DoesNotSwallowDispatchMatrixErrors) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  const DerivContract c = var_swap_at(0.25);
  DerivConfig cfg{};
  cfg.engine = DerivEngine::VolCarrLee;  // invalid for VarSwap (PV-5)

  DerivGreekBumps an_bumps{};
  an_bumps.method = DerivGreekMethod::AnalyticStrip;
  const auto g_fd = deriv_greeks(surf, cs, c, cfg, DerivGreekBumps{});
  const auto g_an = deriv_greeks(surf, cs, c, cfg, an_bumps);
  ASSERT_FALSE(g_fd.has_value());
  ASSERT_FALSE(g_an.has_value());
  EXPECT_EQ(g_fd.error().code(), g_an.error().code());
  EXPECT_EQ(g_fd.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
