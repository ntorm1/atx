#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "atx/vol/american.hpp"

// R-30 (Sub-Sprint A, Task A4): retained-geometry bind-key guardrail.
//
// The Andersen-Lake specialized kernel retains sweep-invariant "static geometry"
// (geo_zc / geo_weru / geo_wequ) across a fixed contract's sigma sweep. If a
// retained workspace were ever reused for a DIFFERENT (T, r, q, node-grid, quad)
// contract without rebinding — the obs-23864 revalidation-trust regression shape —
// the sigma-bind would silently consume stale geometry and mis-price.
//
// The fix is two guardrails, both exercised here:
//   * Debug: al_bind_geometry_sigma asserts a stored {T,r,q,n,nq} bind key matches
//     the current contract on every specialized reuse. These asserts fire on EVERY
//     price() in the sweeps below, so if the happy-path key check false-fired the
//     process would abort — a passing run proves the assert is compiled in and
//     correct on the matching path.
//   * Release (all configs): al_geometry_specialize_off_fallback_count() tallies the
//     safety fallback (static geometry unexpectedly unbound -> generic kernel). It
//     must stay 0 on every production flow.
//
// There is deliberately NO public API that reuses a workspace across a contract
// change without rebinding (AloPricer::reset always rebinds), so the mismatch/
// fallback paths are unreachable from a test — which is exactly why they are
// guardrails against a FUTURE regression, not a currently-triggerable branch. The
// tests therefore prove the guardrails are wired and never false-fire on the real
// warm/cold/reset flows.

namespace {

using atx::vol::al_default_opts;
using atx::vol::al_fast_opts;
using atx::vol::al_geometry_specialize_off_fallback_count;
using atx::vol::AloPricer;
using atx::vol::AlOpts;
using atx::vol::Side;

// A warm sigma sweep on one fixed contract exercises the specialized reuse path
// (al_bind_geometry_sigma) many times; the bind-key assert runs on each and the
// specialize-off fallback must never fire.
TEST(AloBindKey, WarmSigmaSweepNeverHitsSpecializeOffFallback) {
  const std::uint64_t before = al_geometry_specialize_off_fallback_count();
  AloPricer pr(100.0, 105.0, 0.75, 0.04, 0.01, Side::Put, al_default_opts());
  for (int i = 0; i < 64; ++i) {
    const double sigma = 0.10 + 0.005 * static_cast<double>(i); // tiny warm steps
    const double px = pr.price(sigma);
    ASSERT_TRUE(std::isfinite(px)) << "sigma=" << sigma;
    EXPECT_GT(px, 0.0);
  }
  EXPECT_EQ(al_geometry_specialize_off_fallback_count(), before);
}

// Reset across contracts, sides, and accuracy schemes: every reset rebinds the
// static geometry to the new contract, then price() reuses it. The bind key must
// track the rebind exactly (Debug asserts pass) and the fallback must stay 0.
TEST(AloBindKey, ResetAcrossContractsRebindsKeyAndKeepsFallbackZero) {
  struct Case {
    double S, K, T, sigma, r, q;
    Side side;
    std::optional<AlOpts> opts;
  };
  const std::array<Case, 5> cases{{
      {100.0, 112.0, 1.75, 0.21, 0.045, 0.012, Side::Put, std::nullopt},
      {180.0, 155.0, 0.35, 0.34, 0.025, 0.065, Side::Call, al_fast_opts()},
      {72.0, 80.0, 0.08, 0.46, 0.052, 0.018, Side::Put, al_fast_opts()},
      {310.0, 335.0, 2.0, 0.17, 0.02, 0.055, Side::Call, al_default_opts()},
      {50.0, 48.0, 0.5, 0.28, 0.03, 0.0, Side::Put, std::nullopt},
  }};

  const std::uint64_t before = al_geometry_specialize_off_fallback_count();
  AloPricer pr(cases[0].S, cases[0].K, cases[0].T, cases[0].r, cases[0].q, cases[0].side,
               cases[0].opts);
  for (const Case &c : cases) {
    // reset to a DIFFERENT contract/side/scheme, then a small warm sweep on it.
    pr.reset(c.S, c.K, c.T, c.r, c.q, c.side, c.opts);
    for (int i = 0; i < 8; ++i) {
      const double sigma = c.sigma * (1.0 + 0.01 * static_cast<double>(i));
      ASSERT_TRUE(std::isfinite(pr.price(sigma)));
    }
  }
  EXPECT_EQ(al_geometry_specialize_off_fallback_count(), before);
}

// The accessor is monotonic and never decreases across independent pricer lifetimes.
TEST(AloBindKey, FallbackCounterIsMonotonic) {
  const std::uint64_t a = al_geometry_specialize_off_fallback_count();
  { AloPricer pr(90.0, 95.0, 1.0, 0.03, 0.005, Side::Put); (void)pr.price(0.2); }
  const std::uint64_t b = al_geometry_specialize_off_fallback_count();
  EXPECT_GE(b, a);
}

} // namespace
