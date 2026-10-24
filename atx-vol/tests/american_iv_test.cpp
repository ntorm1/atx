#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"
#include "atx/vol/api/fitting/correction.hpp"
#include "fitting/counters.hpp"
#include "pricing/american_family_error.hpp"
#include "pricing/american_iv_batch.hpp" // K2: the native lane's typed outcome / flags
#include "pricing/american_iv_premium.hpp"
#include "pricing/american_iv_search.hpp"
#include "atx/vol/api/core/types.hpp"

// American implied-vol inverter coverage.
//   - price->sigma round-trip across a moneyness/maturity/vol grid for both
//     the Andersen-Lake and BAW forward pricers (self-consistent by design),
//   - monotonicity of the forward map the inverter relies on,
//   - deep ITM/OTM behaviour,
//   - no-arbitrage guards (sub-intrinsic, above upper bound, non-finite),
//   - the at-intrinsic clamp to the vol floor,
//   - the strike-axis batch helper and its parallel-status failure convention.
//
// Strict 1e-5 sigma recovery is asserted only where the Black-76 vega is large
// enough that a 1e-5 vol change is resolvable above the cold pricer's noise
// floor; at the deep-wing / short-maturity corners vega collapses and sigma is
// not identifiable, so only the price round-trip is checked there.

namespace atx::vol {
// Test seam defined in src/american_iv.cpp (not the public header). price_tol
// and the two bolt outputs expose the price-unit polish extension: `bolted` is
// set when the extension evaluated a beyond-drift-cap iterate, `bolt_reverted`
// when it rejected one that failed to reduce the cold-map residual.
Result<double> american_implied_vol_polish_traced(double price, double S, double K, double T,
                                                  double r, double q, Side side, double tol,
                                                  std::uint16_t max_iter,
                                                  const std::optional<AlOpts> &opts,
                                                  double warm_start, double &xl_out, double &xh_out,
                                                  bool &polish_ran_out, bool &polish_clamped_out,
                                                  double price_tol = 0.0,
                                                  bool *bolted_out = nullptr,
                                                  bool *bolt_reverted_out = nullptr);
} // namespace atx::vol

namespace {

using atx::vol::AlOpts;
using atx::vol::american_implied_vol;
using atx::vol::american_implied_vol_batch;
using atx::vol::american_price;
using atx::vol::american_price_cached;
using atx::vol::AmericanMethod;
using atx::vol::CorrectionBlend;
using atx::vol::CorrectionCache;
using atx::vol::Side;

// ── 2.10: the noexcept boundary is a promise, not a hint ─────────────────────
//
// american_iv.hpp declares both american_implied_vol overloads noexcept, but the
// inversion ALLOCATES: the cold Andersen-Lake route constructs an AloPricer,
// whose ~46 KB `State` is a PIMPL behind make_unique, and every error path
// composes a std::string message longer than any SSO buffer. Under memory
// pressure that was an escaping std::bad_alloc — i.e. std::terminate. The
// allocations now sit behind a bad_alloc catch INSIDE the noexcept boundary, so
// the declaration is true and OOM degrades to the declared Result error model.
//
// Allocation failure is not injectable from a unit test, so this pins the half
// that IS machine-checkable: the declaration. Silently "fixing" the mismatch by
// dropping noexcept — an ABI-visible change to a public entry point — breaks the
// build here rather than passing unnoticed.
static_assert(noexcept(american_implied_vol(1.0, 100.0, 100.0, 1.0, 0.01, 0.0, Side::Call)),
              "american_implied_vol's noexcept is a public contract: contain the throw, do not "
              "drop the specifier");
static_assert(noexcept(american_implied_vol(1.0, 100.0, 100.0, 1.0, 0.01, 0.0, Side::Call,
                                            std::declval<const CorrectionBlend &>())),
              "the blend overload carries the same public noexcept contract");

// Unwrap a Result<double>, flagging (non-fatally) an unexpected error.
double value_or_fail(const atx::core::Result<double> &r) {
  EXPECT_TRUE(r.has_value()) << (r ? std::string{} : r.error().to_string());
  return r ? *r : std::nan("");
}

// American finite-difference vega — the TRUE sensitivity of the American
// premium to sigma, and the correct conditioning proxy for strict recovery.
// A deep-ITM American option optimally exercised now sits at intrinsic and is
// insensitive to sigma (time value ~ 0), so sigma is unidentifiable there even
// though the European vega is large; only the FD American vega captures that.
double american_vega_fd(double S, double K, double T, double sigma, double r, double q, Side side,
                        AmericanMethod method) {
  const double h = 1.0e-4;
  const double pu = value_or_fail(american_price(S, K, T, sigma + h, r, q, side, method));
  const double pd = value_or_fail(american_price(S, K, T, sigma - h, r, q, side, method));
  return (pu - pd) / (2.0 * h);
}

const char *side_tag(Side side) { return side == Side::Call ? "C" : "P"; }

CorrectionCache make_iv_correction(double r, double q) {
  auto built = CorrectionCache::build(
      /*n_k=*/16, /*n_T=*/8, /*n_s=*/12, r, q,
      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
      /*T_min=*/0.05, /*T_max=*/2.0,
      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  EXPECT_TRUE(built.has_value());
  return built ? std::move(*built) : CorrectionCache{};
}

// Price a point, invert it, and assert the round-trip. Price recovery is always
// checked; strict sigma recovery only where vega is well above the noise floor.
void check_round_trip(double S, double K, double T, double sigma, double r, double q, Side side,
                      AmericanMethod method) {
  const double p = value_or_fail(american_price(S, K, T, sigma, r, q, side, method));
  if (!std::isfinite(p)) {
    return; // forward pricer already flagged the failure above
  }

  const atx::core::Result<double> iv = american_implied_vol(p, S, K, T, r, q, side, method);
  ASSERT_TRUE(iv.has_value()) << iv.error().to_string() << " [K=" << K << " T=" << T
                              << " sig=" << sigma << " q=" << q << " " << side_tag(side) << "]";

  const double reprice = value_or_fail(american_price(S, K, T, *iv, r, q, side, method));

  // Vega gates BOTH the strict price round-trip and the sigma recovery: a 1e-5 price
  // (or vol) resolution is only meaningful where a 1e-5 vol move clears the cold
  // pricer's noise floor.
  const double vega = american_vega_fd(S, K, T, sigma, r, q, side, method);
  const bool vega_resolvable = vega > 0.5;

  // A1/A3/A6 NOTE (core-review findings 1 + 3 + 6): at near-intrinsic / on-boundary
  // corners vega collapses (e.g. K=110,T=2,sig=0.1 put: vega~3e-3) so sigma is
  // genuinely UNidentifiable — a 1e-5 vol move there changes the price by ~3e-8, far
  // below the cold pricer's noise. A3 (polish bracket-clamp) + A6 (floor unification)
  // have landed; with both in, the price round-trip at every collapsed corner was
  // MEASURED and all but two put corners close to <=2.6e-6 relative or machine
  // precision. The exception is the A1-flagged K=110,T=2 (and K=120,T=2) sig=0.1 q=0
  // puts, which still close only to ~1.2e-4 absolute (~1.2e-5 relative): that residual
  // is the inverter's fundamental floor in the collapsed-vega regime, NOT a polish
  // artifact A3 could remove (A3 correctly bounds the iterate to [xl,xh] but cannot
  // manufacture identifiability where vega has vanished). So the vega gate stays:
  // strict 1e-5 where vega is resolvable, relaxed 1e-4 at the collapsed corners
  // (still trips any gross inversion break). Restoring a uniform strict 1e-5 was
  // checked and fails exactly at those two puts.
  const double px_tol = vega_resolvable ? 1.0e-5 : 1.0e-4;
  EXPECT_NEAR(reprice, p, px_tol * std::fmax(1.0, p))
      << "price round-trip [K=" << K << " T=" << T << " sig=" << sigma << " vega=" << vega << "]";

  if (vega_resolvable) {
    // BAW is a documented 3-4 significant-figure approximation, so its
    // self-consistent inversion inherits that coarseness at ITM corners;
    // Andersen-Lake is ~1e-7 accurate and holds the strict tolerance.
    const double sig_tol = (method == AmericanMethod::Baw) ? 1.0e-3 : 1.0e-5;
    EXPECT_NEAR(*iv, sigma, sig_tol)
        << "sigma round-trip [K=" << K << " T=" << T << " sig=" << sigma << " q=" << q << " "
        << side_tag(side) << "]";
  }
}

void grid_round_trip(AmericanMethod method) {
  const double S = 100.0, r = 0.05;
  for (double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
    for (double T : {0.1, 0.5, 1.0, 2.0}) {
      for (double sigma : {0.1, 0.2, 0.4}) {
        for (double q : {0.0, 0.03}) {
          for (Side side : {Side::Call, Side::Put}) {
            check_round_trip(S, K, T, sigma, r, q, side, method);
          }
        }
      }
    }
  }
}

} // namespace

// ── Round-trip over the full grid, both pricers ──────────────────────────

// A3 (core-review finding 3): the cold Andersen-Lake IV polish must never return
// an iterate outside the sign-change bracket [xl, xh]. Pre-fix the polish ran raw
// Newton steps on the cold reference map with only a `rts > 0` guard, so at hard
// corners (long-dated / low-vol / near-intrinsic, where the warm search map and
// the cold reference map disagree and vega collapses) the iterate could bolt out
// of the (tol-narrow) bracket — past kSigmaHiCap in the worst case — and be
// returned as a wild IV. The fix clamps each polish iterate into [xl, xh] and
// drops a step that bolts many× the final tol past the rtsafe root.
//
// This sweeps ITM corners on the cold-AL path (the exact path check_round_trip
// uses) and asserts the returned IV stays inside the bracket the inverter
// reported, and that the clamp actually engaged on at least one corner (so the
// invariant is not vacuously satisfied by a sweep that never leaves the bracket).
TEST(AmericanIv, ColdPolishStaysInBracket) {
  const double S = 100.0;
  int checked = 0;
  int clamped_count = 0;
  for (double K : {105.0, 110.0, 115.0, 120.0}) {
    for (double T : {1.0, 2.0}) {
      for (double sig : {0.05, 0.08, 0.10}) {
        for (double r : {0.05, 0.08}) {
          for (double q : {0.0, 0.02}) {
            for (Side side : {Side::Put, Side::Call}) {
              const double p = value_or_fail(
                  american_price(S, K, T, sig, r, q, side, AmericanMethod::AndersenLake));
              if (!std::isfinite(p)) {
                continue;
              }
              double xl = 0.0, xh = 0.0;
              bool ran = false, clamped = false;
              const auto iv = atx::vol::american_implied_vol_polish_traced(
                  p, S, K, T, r, q, side, 1.0e-7, 64, std::nullopt, /*warm_start=*/0.0, xl, xh, ran,
                  clamped);
              if (!iv.has_value() || !ran) {
                // Early-return quotes (at-intrinsic clamp, exact seed hit) never
                // reach the polish and have no bracket to check.
                continue;
              }
              ++checked;
              // The polished IV must lie inside the bracket the inverter converged.
              EXPECT_GE(*iv, xl) << "K=" << K << " T=" << T << " sig=" << sig << " r=" << r
                                 << " q=" << q << " " << side_tag(side);
              EXPECT_LE(*iv, xh) << "K=" << K << " T=" << T << " sig=" << sig << " r=" << r
                                 << " q=" << q << " " << side_tag(side);
              if (clamped) {
                ++clamped_count;
              }
            }
          }
        }
      }
    }
  }
  EXPECT_GT(checked, 0);
  EXPECT_GT(clamped_count, 0) << "sweep never exercised the polish bracket-clamp path";
}

// ── Price-unit cold polish (price_tol; Mode B round-trip contract) ─────────
//
// Diagnosis (2026-08-23, Mode B breadth): american_implied_vol root-finds on a
// WARM AloPricer forward map whose boundary is seed-dependent at the hard
// corners (long-dated deep-ITM puts) — the warm root sits 1e-5..6.2e-5 sigma
// (10-62x kModeBIvTol = 1e-6) from the COLD reference root, while the sigma-unit
// polish is capped at kPolishMaxDriftTols x tol = 4e-6 of authority. A caller
// re-pricing through the cold map (american_greeks_al) then misses by
// vega x gap, tripping the $0.005 round-trip screen wherever dollar vega is
// large. price_tol > 0 extends the polish: verify on the cold map and continue
// the cold Newton past the sigma cap, accepting ONLY residual-reducing
// iterates (the cap's collapsed-vega protection, restated in price units).
//
// The synthetic corners below reuse the diagnosis probe's NDX-like chain
// (S=23800, r=0.043, q=0.02, sigma ~0.22 smile): index-scale notional makes
// dollar vega 1,000-8,400 $/vol, so the measured warm/cold gap converts to a
// 0.006-0.018 cold re-price miss — over the screen on all four corners. Each
// case pins the exact warm-chain seed the sweep failed under, making the warm
// map (and so the failure) deterministic.

namespace {

struct HardCorner {
  double K, T, warm;
};

// Warm seeds captured from the 469d313c warm-chained sweep (pin_probe).
constexpr HardCorner kNdxHardCorners[] = {
    {30000.0, 0.494, 0.22757939019678425},
    {26600.0, 0.998, 0.22161176316871997},
    {27000.0, 0.998, 0.22211377754397438},
    {27400.0, 0.998, 0.22267430734825416},
};

constexpr double kNdxS = 23800.0;
constexpr double kNdxR = 0.043;
constexpr double kNdxQ = 0.02;

// The probe's smile and its half-cent mid rounding, reproduced exactly.
double ndx_true_sigma(double K) {
  const double lm = std::log(K / kNdxS);
  return std::fmin(2.5, 0.22 + 0.15 * lm * lm);
}

} // namespace

// CONTROL: the default (price_tol = 0) inversion is the pre-change binary,
// BIT FOR BIT, with this exact call shape (Mode B's leg: AndersenLake, tol 1e-6,
// max_iter 64, al_fast_opts). A change here means the control property broke —
// the default path must never move as a side effect of the price-unit extension.
//
// RE-PINNED at the T3 Newton-slope change (secant over the two most recent
// residual evaluations instead of the Black-76 proxy vega). That change moves the
// cold-route search PATH by construction, so the converged root moves inside the
// requested tol — 6.0e-8 and 4.0e-7 in sigma at tol = 1e-6. Re-pinning is only
// legitimate because the new roots are strictly MORE accurate on the cold
// reference map, measured at capture:
//   OTM put   |resid| 1.000e-5 -> 8.003e-6, |sigma - 0.22| 2.983e-7 -> 2.387e-7
//   NDX corner|resid| 1.815e-2 -> 1.502e-2
// The control property itself is unchanged: price_tol = 0 must still reproduce
// these bits exactly.
TEST(AmericanIv, PricePolishDefaultOffIsBitForBitControl) {
  const std::optional<AlOpts> opts = atx::vol::al_fast_opts();
  {
    // Well-conditioned OTM put: the polish stays inside the bracket.
    const double S = 100.0, K = 105.0, T = 0.75, r = 0.04, q = 0.01;
    const double p = value_or_fail(
        atx::vol::american_price(S, K, T, 0.22, r, q, Side::Put, AmericanMethod::AndersenLake, opts));
    const auto iv = american_implied_vol(p, S, K, T, r, q, Side::Put,
                                         AmericanMethod::AndersenLake, 1.0e-6, 64, opts,
                                         /*correction=*/nullptr, /*warm_start=*/0.0);
    ASSERT_TRUE(iv.has_value());
    EXPECT_EQ(std::bit_cast<std::uint64_t>(*iv), 0x3fcc28f3c1e76011ULL)
        << "default-path IV moved: " << std::setprecision(17) << *iv;
  }
  {
    // The diagnosed hard corner under the DEFAULT path: still the base bits
    // (the drift-cap drop, not the extension, decides here).
    const HardCorner c = kNdxHardCorners[3]; // K=27400, T=0.998
    const auto g_true = atx::vol::american_greeks_al(kNdxS, c.K, c.T, ndx_true_sigma(c.K), kNdxR,
                                                     kNdxQ, Side::Put, opts);
    ASSERT_TRUE(g_true.has_value());
    const double mid = std::nearbyint(g_true->price * 200.0) / 200.0;
    const auto iv = american_implied_vol(mid, kNdxS, c.K, c.T, kNdxR, kNdxQ, Side::Put,
                                         AmericanMethod::AndersenLake, 1.0e-6, 64, opts,
                                         /*correction=*/nullptr, c.warm);
    ASSERT_TRUE(iv.has_value());
    EXPECT_EQ(std::bit_cast<std::uint64_t>(*iv), 0x3fcc8a690ad53f53ULL)
        << "default-path IV moved at the hard corner: " << std::setprecision(17) << *iv;
  }
}

// CONTROL: where the sigma-unit polish already meets the price budget, the
// price-unit extension is verification only — the returned IV is bit-identical
// to the default path. This is the Mode B smoke/tune bit-identity property:
// rows whose cold residual is already <= price_tol do not move.
TEST(AmericanIv, PricePolishNoOpWhereSigmaPolishMeetsBudget) {
  const std::optional<AlOpts> opts = atx::vol::al_fast_opts();
  const double S = 100.0, K = 105.0, T = 0.75, r = 0.04, q = 0.01;
  const double p = value_or_fail(
      atx::vol::american_price(S, K, T, 0.22, r, q, Side::Put, AmericanMethod::AndersenLake, opts));
  const auto base = american_implied_vol(p, S, K, T, r, q, Side::Put,
                                         AmericanMethod::AndersenLake, 1.0e-6, 64, opts,
                                         /*correction=*/nullptr, /*warm_start=*/0.0);
  const auto polished = american_implied_vol(p, S, K, T, r, q, Side::Put,
                                             AmericanMethod::AndersenLake, 1.0e-6, 64, opts,
                                             /*correction=*/nullptr, /*warm_start=*/0.0,
                                             /*price_tol=*/2.5e-3);
  ASSERT_TRUE(base.has_value());
  ASSERT_TRUE(polished.has_value());
  EXPECT_EQ(std::bit_cast<std::uint64_t>(*base), std::bit_cast<std::uint64_t>(*polished));
}

// THE FIX: on every diagnosed hard corner the default inversion re-prices on
// the cold map outside the $0.005 round-trip screen (the failure this lane
// exists to close), and the SAME call with price_tol = half the screen
// ($0.0025, exactly what Mode B now passes) re-prices within price_tol — at a
// sigma drift bounded by the measured warm/cold gap scale.
TEST(AmericanIv, PricePolishClosesWarmColdGapOnHighVegaPuts) {
  const std::optional<AlOpts> opts = atx::vol::al_fast_opts();
  constexpr double kRoundTripTol = 0.005; // kModeBRoundTripTicks x tick, in $
  constexpr double kPriceTol = 0.5 * kRoundTripTol;
  for (const HardCorner &c : kNdxHardCorners) {
    const auto g_true = atx::vol::american_greeks_al(kNdxS, c.K, c.T, ndx_true_sigma(c.K), kNdxR,
                                                     kNdxQ, Side::Put, opts);
    ASSERT_TRUE(g_true.has_value()) << "K=" << c.K;
    const double mid = std::nearbyint(g_true->price * 200.0) / 200.0;

    const auto base = american_implied_vol(mid, kNdxS, c.K, c.T, kNdxR, kNdxQ, Side::Put,
                                           AmericanMethod::AndersenLake, 1.0e-6, 64, opts,
                                           /*correction=*/nullptr, c.warm);
    ASSERT_TRUE(base.has_value()) << "K=" << c.K;
    const auto g_base = atx::vol::american_greeks_al(kNdxS, c.K, c.T, *base, kNdxR, kNdxQ,
                                                     Side::Put, opts);
    ASSERT_TRUE(g_base.has_value()) << "K=" << c.K;
    EXPECT_GT(std::fabs(g_base->price - mid), kRoundTripTol)
        << "K=" << c.K << ": corner no longer trips the round-trip screen by default — "
        << "the control construction has drifted; re-derive the pinned warm seeds";

    const auto fixed = american_implied_vol(mid, kNdxS, c.K, c.T, kNdxR, kNdxQ, Side::Put,
                                            AmericanMethod::AndersenLake, 1.0e-6, 64, opts,
                                            /*correction=*/nullptr, c.warm, kPriceTol);
    ASSERT_TRUE(fixed.has_value()) << "K=" << c.K;
    const auto g_fixed = atx::vol::american_greeks_al(kNdxS, c.K, c.T, *fixed, kNdxR, kNdxQ,
                                                      Side::Put, opts);
    ASSERT_TRUE(g_fixed.has_value()) << "K=" << c.K;
    EXPECT_LE(std::fabs(g_fixed->price - mid), kPriceTol)
        << "K=" << c.K << ": polished sigma must re-price within price_tol on the cold map";
    // The correction is the warm/cold root gap: 1e-5..6.2e-5 sigma measured,
    // bounded well under 1e-4. A larger move would be a different defect.
    EXPECT_LT(std::fabs(*fixed - *base), 1.0e-4) << "K=" << c.K;
  }
}

// PROTECTION: the drift cap's collapsed-vega defence survives in price units.
// At the A3 corners (long-dated near-intrinsic puts, vega ~1e-3) the cold map
// is flat in sigma at its own noise floor, so an unreachably tight price_tol
// (far below anything a production caller passes — Mode B's is 2.5e-3, which
// these corners already meet without any extension step) forces the extension
// to attempt Newton bolts across the flat plateau. The contract's protection
// is the residual-reduction rule, and this pins its two observable halves:
// the returned IV never re-prices WORSE on the cold map than the sigma-polish
// result and never leaves the inverter's domain, and at least one corner must
// actually exercise the non-reducing-bolt revert path (so the protection is
// proven engaged, not vacuously passed). A reducing bolt across the plateau
// IS accepted — that is the documented trade, not a defect: sigma was never
// identifiable there, and the accepted iterate prices strictly closer.
TEST(AmericanIv, PricePolishKeepsCollapsedVegaProtection) {
  const double S = 100.0;
  int checked = 0;
  int bolted_n = 0;
  int reverted = 0;
  // Widened at the T3 Newton-slope change. The secant slope lands the rtsafe root
  // closer to the cold root, so on the ORIGINAL 3x2 grid every bolt now REDUCES
  // the cold residual and the revert half of the protection went unexercised —
  // a coverage loss, not a behaviour change. Reaching further onto the flat
  // plateau (longer T, nearer intrinsic, lower vol) engages it again.
  for (double K : {110.0, 115.0, 120.0, 125.0, 130.0, 135.0, 140.0}) {
    for (double sig : {0.04, 0.06, 0.08, 0.10, 0.14}) {
      for (double T : {1.0, 2.0, 3.0, 5.0}) {
        const double r = 0.05, q = 0.0;
        const double p = value_or_fail(atx::vol::american_price(S, K, T, sig, r, q, Side::Put,
                                                                AmericanMethod::AndersenLake));
        if (!std::isfinite(p)) {
          continue;
        }
        double xl = 0.0, xh = 0.0;
        bool ran = false, clamped = false;
        const auto base = atx::vol::american_implied_vol_polish_traced(
            p, S, K, T, r, q, Side::Put, 1.0e-7, 64, std::nullopt, /*warm_start=*/0.0, xl, xh, ran,
            clamped);
        bool bolted = false, bolt_reverted = false;
        const auto tight = atx::vol::american_implied_vol_polish_traced(
            p, S, K, T, r, q, Side::Put, 1.0e-7, 64, std::nullopt, /*warm_start=*/0.0, xl, xh, ran,
            clamped, /*price_tol=*/1.0e-12, &bolted, &bolt_reverted);
        if (!base.has_value() || !tight.has_value() || !ran) {
          continue;
        }
        ++checked;
        const double resid_base =
            std::fabs(value_or_fail(atx::vol::american_price(S, K, T, *base, r, q, Side::Put,
                                                             AmericanMethod::AndersenLake)) -
                      p);
        const double resid_tight =
            std::fabs(value_or_fail(atx::vol::american_price(S, K, T, *tight, r, q, Side::Put,
                                                             AmericanMethod::AndersenLake)) -
                      p);
        // Never worse on the cold map than the sigma-polish result, and never
        // outside the inverter's own domain — the observable halves of the
        // preserved protection (see the banner: reducing bolts are accepted).
        EXPECT_LE(resid_tight, resid_base + 1.0e-12) << "K=" << K << " sig=" << sig << " T=" << T;
        EXPECT_GE(*tight, atx::vol::kIvMin) << "K=" << K << " sig=" << sig << " T=" << T;
        EXPECT_LE(*tight, 40.0) << "K=" << K << " sig=" << sig << " T=" << T; // kSigmaHiCap
        if (bolted) {
          ++bolted_n;
        }
        if (bolt_reverted) {
          ++reverted;
        }
      }
    }
  }
  std::cout << "[A3 protection] corners=" << checked << " bolted=" << bolted_n
            << " reverted=" << reverted << "\n";
  EXPECT_GT(checked, 0);
  EXPECT_GT(bolted_n, 0) << "sweep never attempted a beyond-drift-cap bolt";
  EXPECT_GT(reverted, 0) << "sweep never exercised the residual-increasing-bolt revert path";
}

TEST(AmericanIv, RoundTrip_GridAndersenLake_RecoversSigma) {
  grid_round_trip(AmericanMethod::AndersenLake);
}

TEST(AmericanIv, RoundTrip_GridBaw_RecoversSigma) { grid_round_trip(AmericanMethod::Baw); }

// ── Monotonicity of the forward map the inverter assumes ─────────────────

TEST(AmericanIvBlend, EndpointMatchesSingleCacheExactly) {
  const CorrectionCache lower = make_iv_correction(0.04, 0.00);
  const CorrectionCache upper = make_iv_correction(0.06, 0.03);
  const CorrectionBlend endpoint = CorrectionBlend::single(&lower);
  constexpr double S = 100.0;
  constexpr double K = 102.0;
  constexpr double T = 0.5;
  constexpr double sigma = 0.27;
  constexpr double r = 0.04;
  constexpr double q = 0.00;
  const double price = american_price_cached(S, K, T, sigma, r, q, Side::Put, &lower);
  const auto single =
      american_implied_vol(price, S, K, T, r, q, Side::Put, AmericanMethod::AndersenLake, 1.0e-7,
                           64, std::nullopt, &lower);
  const auto blended = american_implied_vol(price, S, K, T, r, q, Side::Put, endpoint);
  ASSERT_TRUE(single.has_value());
  ASSERT_TRUE(blended.has_value());
  EXPECT_EQ(*blended, *single);

  const CorrectionBlend upper_endpoint{&lower, &upper, 1.0};
  const double upper_price = american_price_cached(S, K, T, sigma, 0.06, 0.03, Side::Put, &upper);
  const auto upper_single =
      american_implied_vol(upper_price, S, K, T, 0.06, 0.03, Side::Put,
                           AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt, &upper);
  const auto upper_blended =
      american_implied_vol(upper_price, S, K, T, 0.06, 0.03, Side::Put, upper_endpoint);
  ASSERT_TRUE(upper_single.has_value());
  ASSERT_TRUE(upper_blended.has_value());
  EXPECT_EQ(*upper_blended, *upper_single);
}

TEST(AmericanIvBlend, InteriorRoundTripRecoversSigma) {
  const CorrectionCache lower = make_iv_correction(0.04, 0.00);
  const CorrectionCache upper = make_iv_correction(0.06, 0.03);
  const CorrectionBlend blend{&lower, &upper, 0.4};
  constexpr double S = 100.0;
  constexpr double K = 102.0;
  constexpr double T = 0.5;
  constexpr double sigma = 0.27;
  constexpr double r = 0.048;
  constexpr double q = 0.012;
  const double price = american_price_cached(S, K, T, sigma, r, q, Side::Put, blend);
  const auto iv = american_implied_vol(price, S, K, T, r, q, Side::Put, blend);
  ASSERT_TRUE(iv.has_value()) << iv.error().to_string();
  EXPECT_NEAR(*iv, sigma, 1.0e-6);
  EXPECT_NEAR(american_price_cached(S, K, T, *iv, r, q, Side::Put, blend), price, 1.0e-7);
}

TEST(AmericanIv, ForwardPrice_IncreasingInSigma_Monotone) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  for (Side side : {Side::Call, Side::Put}) {
    double prev = -1.0;
    for (double sigma : {0.05, 0.1, 0.2, 0.3, 0.5, 0.8, 1.2}) {
      const double p =
          value_or_fail(american_price(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake));
      EXPECT_GT(p, prev) << "sigma=" << sigma << " " << side_tag(side);
      prev = p;
    }
  }
}

// ── Deep ITM / OTM at a well-conditioned vol ─────────────────────────────

TEST(AmericanIv, DeepItmOtm_ModerateVol_RecoversSigma) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.02, sigma = 0.3;
  for (double K : {60.0, 160.0}) {
    for (Side side : {Side::Call, Side::Put}) {
      const double p =
          value_or_fail(american_price(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake));
      const double iv = value_or_fail(american_implied_vol(p, S, K, T, r, q, side));
      // Deep-ITM American options optimally exercised early sit at intrinsic
      // and are sigma-insensitive (time value ~ 0), so sigma is only
      // identifiable on the well-conditioned (OTM) side.
      if (american_vega_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake) > 0.5) {
        EXPECT_NEAR(iv, sigma, 1.0e-5) << "K=" << K << " " << side_tag(side);
      }
    }
  }
}

// ── BAW method, single point ─────────────────────────────────────────────

TEST(AmericanIv, RoundTrip_BawPut_RecoversSigma) {
  const double S = 100.0, K = 105.0, T = 0.5, sigma = 0.25, r = 0.05, q = 0.0;
  const double p =
      value_or_fail(american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::Baw));
  const double iv =
      value_or_fail(american_implied_vol(p, S, K, T, r, q, Side::Put, AmericanMethod::Baw));
  EXPECT_NEAR(iv, sigma, 1.0e-5);
}

// ── No-arbitrage guards ──────────────────────────────────────────────────

TEST(AmericanIv, SubIntrinsicPrice_Call_ReturnsOutOfRange) {
  const double S = 100.0, K = 80.0, T = 1.0, r = 0.05, q = 0.0; // intrinsic = 20
  const auto iv = american_implied_vol(10.0, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, SubIntrinsicPrice_Put_ReturnsOutOfRange) {
  const double S = 100.0, K = 120.0, T = 1.0, r = 0.05, q = 0.0; // intrinsic = 20
  const auto iv = american_implied_vol(5.0, S, K, T, r, q, Side::Put);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, PriceAboveUpperBound_Call_ReturnsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0; // upper = S = 100
  const auto iv = american_implied_vol(101.0, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, NonFinitePrice_ReturnsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const auto iv = american_implied_vol(std::nan(""), S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, NonPositiveStrike_ReturnsInvalidArgument) {
  const auto iv = american_implied_vol(5.0, 100.0, 0.0, 1.0, 0.05, 0.0, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── At-intrinsic clamp ───────────────────────────────────────────────────

TEST(AmericanIv, PriceAtIntrinsic_ClampsToFloor) {
  const double S = 100.0, K = 140.0, T = 1.0, r = 0.05, q = 0.0; // put intrinsic = 40
  const auto iv = american_implied_vol(40.0, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_DOUBLE_EQ(*iv, atx::vol::kIvMin);
}

// ── T1: the zero-vol American bound, not the immediate intrinsic ─────────
//
// The screen above the search used to compare the quote against the IMMEDIATE
// intrinsic max(0, S-K) only. That is just the t = 0 corner of the sigma -> 0
// American value; the t = T corner is the discounted forward intrinsic
// df*(F-K)^+, and for a call with r > q it is the LARGER of the two. A quote
// between the two carries no volatility at all — no sigma > 0 reproduces it —
// yet it passed the screen, tripped residual(kSigmaLo) >= 0 inside the bracket
// and came back as Ok(kIvMin): a SUCCESS publishing 0.005 as a MEASURED vol
// (docs/LEDGER.md). The inverter must now refuse it and say why.

TEST(AmericanIv, PriceBelowZeroVolForwardBound_Call_ReturnsOutOfRange) {
  const double S = 100.0, K = 90.0, T = 1.0, r = 0.05, q = 0.0;
  // Immediate intrinsic = 10.0, so 10.05 clears the OLD screen. The zero-vol
  // value is S - K*e^{-rT} = 100 - 90*e^{-0.05} = 14.389..., so the quote is
  // unidentifiable. Proven with the library's OWN forward map rather than a
  // restated constant: the American price is increasing in sigma, so a price at
  // a sigma far BELOW the reported floor already exceeding the quote means no
  // positive sigma reproduces it.
  const double p_sub_floor = value_or_fail(american_price(S, K, T, 1.0e-6, r, q, Side::Call,
                                                          AmericanMethod::AndersenLake));
  ASSERT_GT(p_sub_floor, 10.05) << "fixture broken: the quote is identifiable after all";

  const auto iv = american_implied_vol(10.05, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value()) << "returned " << (iv ? *iv : 0.0) << " as a MEASURED vol";
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
  EXPECT_NE(iv.error().message().find("zero-vol"), std::string::npos) << iv.error().to_string();
}

// The clamp side of the same boundary: a quote AT the zero-vol value is the
// documented sigma -> 0 case and still reports the floor, so the fix moves the
// boundary, it does not delete the clamp.
TEST(AmericanIv, PriceAtZeroVolForwardBound_Call_ClampsToFloor) {
  const double S = 100.0, K = 90.0, T = 1.0, r = 0.05, q = 0.0;
  const double zero_vol = S - K * std::exp(-r * T); // = df*(F-K), the t = T corner
  const auto iv = american_implied_vol(zero_vol, S, K, T, r, q, Side::Call);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_DOUBLE_EQ(*iv, atx::vol::kIvMin);
}

// ... and a quote just above it still inverts to its true vol, so the widened
// screen costs no identifiable quote.
TEST(AmericanIv, PriceAboveZeroVolForwardBound_Call_StillInverts) {
  const double S = 100.0, K = 90.0, T = 1.0, r = 0.05, q = 0.0, sigma = 0.08;
  const double p = value_or_fail(
      american_price(S, K, T, sigma, r, q, Side::Call, AmericanMethod::AndersenLake));
  const auto iv = american_implied_vol(p, S, K, T, r, q, Side::Call);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_NEAR(*iv, sigma, 1.0e-5);
}

// Puts with r > q keep the immediate intrinsic as the binding corner (K*e^{-rT}
// - S*e^{-qT} < K - S there), so the put front door is untouched by the fix.
TEST(AmericanIv, PutZeroVolBoundIsImmediateIntrinsic_Unchanged) {
  const double S = 100.0, K = 140.0, T = 1.0, r = 0.05, q = 0.0; // intrinsic = 40
  const auto at = american_implied_vol(40.0, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(at.has_value()) << (at ? std::string{} : at.error().to_string());
  EXPECT_DOUBLE_EQ(*at, atx::vol::kIvMin);
  const auto below = american_implied_vol(39.5, S, K, T, r, q, Side::Put);
  ASSERT_FALSE(below.has_value());
  EXPECT_EQ(below.error().code(), atx::vol::ErrorCode::OutOfRange);
}

// The zero-vol value is a sup over exercise times, not a max over the two ends:
// for q > 0 and r > q the deterministic payoff S*e^{-qt} - K*e^{-rt} has an
// interior stationary MAXIMUM at t* = ln(qS/rK)/(q-r). Deep ITM and long dated
// that bump clears both endpoints by ~1.8 price points, so screening on the two
// corners alone still admits an unidentifiable quote.
TEST(AmericanIv, ZeroVolBoundHonoursInteriorExerciseOptimum_Call) {
  const double S = 409.4, K = 100.0, T = 5.0, r = 0.10, q = 0.02;
  const double end0 = S - K;                                    // 309.4
  const double endT = S * std::exp(-q * T) - K * std::exp(-r * T); // 309.79
  const double t_star = std::log((q * S) / (r * K)) / (q - r);
  ASSERT_GT(t_star, 0.0);
  ASSERT_LT(t_star, T);
  const double interior = S * std::exp(-q * t_star) - K * std::exp(-r * t_star);
  ASSERT_GT(interior, std::fmax(end0, endT) + 1.0) << "fixture broken: no interior bump";

  // Strictly between the best ENDPOINT and the interior optimum: no sigma > 0
  // reproduces it, but a two-corner screen admits it.
  const double quote = 0.5 * (std::fmax(end0, endT) + interior);
  const auto iv = american_implied_vol(quote, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value()) << "returned " << (iv ? *iv : 0.0) << " as a MEASURED vol";
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
  EXPECT_NE(iv.error().message().find("zero-vol"), std::string::npos) << iv.error().to_string();
}

// ── F2: the ceiling is a sup too, and under negative carry it is NOT S or K ──
//
// An American call under q < 0 is worth MORE than spot and a put under r < 0 is
// worth more than its strike. The old "S for a call, K for a put" ceiling then
// sits BELOW the zero-vol lower bound, and the two screens cross into an EMPTY
// admissible interval that rejects every price. Reachable in-house: q_eff is the
// borrow and `imply_borrow_european_pcp_from_base` brackets it from -0.5.

TEST(AmericanIv, NegativeCarryCall_InvertsInsteadOfCrossingTheScreens) {
  const double S = 100.0, K = 50.0, T = 1.0, r = 0.05, q = -0.5, sigma = 0.30;
  // The zero-vol bound is S*e^{-qT} - K*e^{-rT} = 117.31, ABOVE the old S=100
  // ceiling; the true price is above that again.
  const double zero_vol = S * std::exp(-q * T) - K * std::exp(-r * T);
  ASSERT_GT(zero_vol, S) << "fixture broken: this must exceed the naive ceiling";
  const double p = value_or_fail(
      american_price(S, K, T, sigma, r, q, Side::Call, AmericanMethod::AndersenLake));
  ASSERT_GT(p, zero_vol);
  const auto iv = american_implied_vol(p, S, K, T, r, q, Side::Call);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_NEAR(*iv, sigma, 1.0e-5);
}

TEST(AmericanIv, NegativeRatePut_InvertsInsteadOfCrossingTheScreens) {
  const double S = 100.0, K = 100.0, T = 20.0, r = -0.05, q = 0.0, sigma = 0.30;
  const double zero_vol = K * std::exp(-r * T) - S * std::exp(-q * T);
  ASSERT_GT(zero_vol, K) << "fixture broken: this must exceed the naive ceiling";
  const double p = value_or_fail(
      american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::AndersenLake));
  ASSERT_GT(p, zero_vol);
  const auto iv = american_implied_vol(p, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_NEAR(*iv, sigma, 1.0e-4);
}

// The other half of the same guard: a carry x maturity that OVERFLOWS the
// deterministic payoff must be refused by name, not silently turn every
// comparison against an infinity into a rejection.
TEST(AmericanIv, OverflowingCarryMaturity_RefusedByName) {
  const auto iv = american_implied_vol(50.0, 100.0, 100.0, 20000.0, 0.0, -0.05, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
  EXPECT_NE(iv.error().message().find("overflows"), std::string::npos) << iv.error().to_string();
}

// ── F4: the SELECTED MAP adjudicates the zero-vol screen ────────────────────
//
// The analytic sup is the sigma -> 0 limit of the EXACT American value (measured
// to 1.3e-13 against Andersen-Lake). BAW and the Chebyshev-corrected cached map
// are approximations, so the screen must never refuse a price the caller's own
// pricer produced. These pin the round trip on both routes at the very corner
// where the analytic bound is most aggressive — the interior-exercise optimum,
// where it exceeds both endpoints by 1.76 points. The only pre-existing
// BAW/cached inverter test is counters-gated and SKIPPED in this build, so
// without these that route has no coverage at all.

TEST(AmericanIv, ZeroVolScreenAdjudicatedByMap_BawRoundTripAtInteriorOptimum) {
  const double S = 409.4, K = 100.0, T = 5.0, r = 0.10, q = 0.02, sigma = 0.30;
  const double p = value_or_fail(american_price(S, K, T, sigma, r, q, Side::Call,
                                                AmericanMethod::Baw));
  const auto iv = american_implied_vol(p, S, K, T, r, q, Side::Call, AmericanMethod::Baw);
  ASSERT_TRUE(iv.has_value()) << "a pricer must never be refused a price it produced: "
                              << (iv ? std::string{} : iv.error().to_string());
  EXPECT_NEAR(*iv, sigma, 1.0e-5);
}

// The cached route's assertion is ADMISSION, not recovery. Measured at
// 654a9206 and byte-identical here: this grid's sigma = 0.12 rows recover
// 0.005 / 0.005 / 0.230 against a true 0.12, because the inversion searches
// down to kIvMin = 0.005 while the correction tensor is only FITTED over
// [0.10, 0.80] and extrapolates badly below it. That is a pre-existing cache
// defect of the same fabrication class as T1 and is recorded in the ledger; it
// is not what this test guards, and pinning a recovery bound here would pin the
// defect. What must hold is that the screen never REFUSES the cached map a
// price the cached map itself produced.
TEST(AmericanIv, ZeroVolScreenAdjudicatedByMap_CachedMapNeverRefusedItsOwnPrice) {
  const double r = 0.05, q = 0.0, S = 100.0, T = 1.0;
  const CorrectionCache cache = make_iv_correction(r, q); // Put cache
  int checked = 0;
  for (double K : {104.0, 112.0, 120.0, 128.0, 136.0}) {
    for (double sigma : {0.12, 0.25, 0.45}) {
      const double p = american_price_cached(S, K, T, sigma, r, q, Side::Put, &cache);
      ASSERT_TRUE(std::isfinite(p)) << "K=" << K;
      const auto iv = american_implied_vol(p, S, K, T, r, q, Side::Put,
                                           AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt,
                                           &cache);
      EXPECT_TRUE(iv.has_value())
          << "K=" << K << " sig=" << sigma << ": the cached map was refused its own price: "
          << (iv ? std::string{} : iv.error().to_string());
      ++checked;
    }
  }
  EXPECT_EQ(checked, 15);
}

// A6 (core-review finding 6): the IV bracket floor and the reported floor are the
// SAME constant (kIvMin). Pre-fix the search bracket floored at kSigmaLo=1e-4
// while the reported floor was kIvMin=0.005, so a quote decaying toward intrinsic
// produced IVs stepping DOWN through the (1e-4, 0.005) gap (0.004, 0.002, …,
// below the documented floor) and then a 50x SNAP up to 0.005 once the root fell
// below 1e-4 — a non-monotone cliff that poisons vega-weighted fitters. With the
// bracket floor unified to kIvMin the IV decreases monotonically to the floor and
// clamps there, with no sub-floor values and no cliff.
TEST(AmericanIv, DecayTowardIntrinsic_MonotoneNonIncreasingIvNoFloorCliff) {
  const double S = 100.0, K = 100.0, T = 0.25, r = 0.05, q = 0.0;
  const Side side = Side::Call; // ATM, q=0: time value stays resolvable at tiny vol
  // Decreasing sigma: some land in the old (1e-4, 0.005) gap, the last below 1e-4.
  const double sigmas[] = {0.05, 0.02, 0.01, 0.006, 0.004, 0.002, 0.001, 0.00005};
  double prev_iv = 1.0e9;
  for (double sig : sigmas) {
    const double p =
        value_or_fail(american_price(S, K, T, sig, r, q, side, AmericanMethod::AndersenLake));
    const auto iv = american_implied_vol(p, S, K, T, r, q, side);
    ASSERT_TRUE(iv.has_value()) << "sig=" << sig << ": "
                                << (iv ? std::string{} : iv.error().to_string());
    EXPECT_GE(*iv, atx::vol::kIvMin) << "sig=" << sig << ": IV below the unified floor";
    EXPECT_LE(*iv, prev_iv + 1.0e-12)
        << "sig=" << sig << ": IV rose as the quote decayed toward intrinsic (floor cliff)";
    prev_iv = *iv;
  }
}

// ── R-05: seeded fast path evaluates the floor/ceiling BEFORE clamping/rejecting
//
// The seeded bracket takes bounded (16-step) geometric steps from the warm/euro
// seed. A seed far from a genuine in-range root can exhaust those steps without
// reaching the true vol floor (step-down) or ceiling (step-up). Before this fix
// the fast path then clamped to kIvMin / returned OutOfRange spuriously — even
// though a real, identifiable IV sits inside the bracket — violating the
// documented warm_start contract ("the result is unchanged; only the iteration
// count differs"). The floor/ceiling is now priced first, matching the
// wide-bracket fallback, so the genuine root is solved.

TEST(AmericanIv, SeededStepDown_TinyInRangeIv_NotClampedToFloor) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const double true_sigma = 0.05; // small but far above kIvMin (1e-4)
  const double p = value_or_fail(
      american_price(S, K, T, true_sigma, r, q, Side::Call, AmericanMethod::AndersenLake));
  // warm_start = 2.0 >> true_sigma: the ~7%/step step-down halts near 0.62,
  // never reaching kSigmaLo, so the pre-fix fast path returned kIvMin.
  const auto iv =
      american_implied_vol(p, S, K, T, r, q, Side::Call, AmericanMethod::AndersenLake, 1.0e-7, 64,
                           std::nullopt, /*correction=*/nullptr, /*warm_start=*/2.0);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  // true_sigma (0.05) is a full 10x the vol floor (kIvMin = 0.005); the pre-fix
  // fast path returned exactly kIvMin here, so any value clearly above the floor
  // proves the spurious clamp is gone.
  EXPECT_GT(*iv, atx::vol::kIvMin * 2.0) << "genuine tiny IV must not be clamped to the floor";
  EXPECT_NEAR(*iv, true_sigma, 1.0e-5);
}

TEST(AmericanIv, SeededStepUp_HighInRangeIv_NotRejectedAsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const double true_sigma = 1.2; // high but well below kSigmaHiCap (40)
  const double p = value_or_fail(
      american_price(S, K, T, true_sigma, r, q, Side::Call, AmericanMethod::AndersenLake));
  // warm_start = 0.1 << true_sigma: the *1.15/step step-up halts near 0.81,
  // never reaching kSigmaHiCap, so the pre-fix fast path returned OutOfRange.
  const auto iv =
      american_implied_vol(p, S, K, T, r, q, Side::Call, AmericanMethod::AndersenLake, 1.0e-7, 64,
                           std::nullopt, /*correction=*/nullptr, /*warm_start=*/0.1);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  const double reprice =
      value_or_fail(american_price(S, K, T, *iv, r, q, Side::Call, AmericanMethod::AndersenLake));
  EXPECT_NEAR(reprice, p, 1.0e-5 * std::fmax(1.0, p));
  EXPECT_NEAR(*iv, true_sigma, 1.0e-3);
}

// The seeded fast path must agree with the no-warm-start (euro-seeded) path for
// the SAME quote: a warm_start only changes the search trajectory, never the
// returned IV. This pins the R-05 invariant across both branches at once.
TEST(AmericanIv, WarmStartResultInvariantToSeed) {
  const double S = 100.0, K = 105.0, T = 0.75, r = 0.04, q = 0.01;
  const double true_sigma = 0.22;
  const double p = value_or_fail(
      american_price(S, K, T, true_sigma, r, q, Side::Put, AmericanMethod::AndersenLake));
  const auto base = american_implied_vol(p, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(base.has_value()) << (base ? std::string{} : base.error().to_string());
  for (double ws : {0.02, 0.1, 0.5, 2.0, 4.5}) {
    const auto iv =
        american_implied_vol(p, S, K, T, r, q, Side::Put, AmericanMethod::AndersenLake, 1.0e-7, 64,
                             std::nullopt, /*correction=*/nullptr, /*warm_start=*/ws);
    ASSERT_TRUE(iv.has_value()) << "warm_start=" << ws << ": "
                                << (iv ? std::string{} : iv.error().to_string());
    EXPECT_NEAR(*iv, *base, 1.0e-6) << "warm_start=" << ws;
  }
}

// ── Batch helper over a strike axis ──────────────────────────────────────

TEST(AmericanIv, Batch_StrikeAxis_MatchesScalar) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.0, sigma = 0.25;
  const Side side = Side::Call;
  const std::array<double, 4> K{80.0, 95.0, 105.0, 120.0};
  std::array<double, 4> price{};
  for (std::size_t i = 0; i < K.size(); ++i) {
    price[i] =
        value_or_fail(american_price(S, K[i], T, sigma, r, q, side, AmericanMethod::AndersenLake));
  }

  std::array<double, 4> iv_out{};
  std::array<atx::vol::Status, 4> status{};
  const auto s = american_implied_vol_batch(price, S, K, T, r, q, side, iv_out, status);
  ASSERT_TRUE(s.has_value()) << (s ? std::string{} : s.error().to_string());
  for (std::size_t i = 0; i < K.size(); ++i) {
    EXPECT_TRUE(status[i].has_value()) << "K=" << K[i];
    EXPECT_NEAR(iv_out[i], sigma, 1.0e-5) << "K=" << K[i];
  }
}

TEST(AmericanIv, Batch_SubIntrinsicLane_NanValueAndErrorStatus) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const Side side = Side::Call;
  const std::array<double, 2> K{90.0, 90.0}; // intrinsic = 10
  const double good =
      value_or_fail(american_price(S, 90.0, T, 0.2, r, q, side, AmericanMethod::AndersenLake));
  const std::array<double, 2> price{5.0, good}; // lane 0 is sub-intrinsic

  std::array<double, 2> iv_out{};
  std::array<atx::vol::Status, 2> status{};
  const auto s = american_implied_vol_batch(price, S, K, T, r, q, side, iv_out, status);
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(status[0].has_value());
  EXPECT_TRUE(std::isnan(iv_out[0]));
  EXPECT_TRUE(status[1].has_value());
  EXPECT_NEAR(iv_out[1], 0.2, 1.0e-5);
}

TEST(AmericanIv, Batch_SpanLengthMismatch_ReturnsInvalidArgument) {
  const std::array<double, 3> price{1.0, 2.0, 3.0};
  const std::array<double, 2> K{90.0, 100.0}; // length mismatch vs price
  std::array<double, 3> iv_out{};
  std::array<atx::vol::Status, 3> status{};
  const auto s =
      american_implied_vol_batch(price, 100.0, K, 1.0, 0.05, 0.0, Side::Call, iv_out, status);
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── 4.3 error model: the batch reports through Result<std::size_t> ────────
//
// The entry used to return `Status` and leave the caller to recompute "how many
// lanes did that write" from an input span, while its only rejection was
// observable as nothing more specific than "not Ok" — so that boundary refusal
// could have been renamed, or folded into a neighbour's, without anything
// failing. These two tests pin BOTH halves of the converted shape: the exact
// Error the span-length refusal carries (reached from each of the three spans
// that can disagree with `price`), and the lane count a success carries.

TEST(AmericanIvErrorModel, BatchCarriesItsExactRejectionThroughResult) {
  const std::array<double, 3> price{1.0, 2.0, 3.0};
  const std::array<double, 3> K{90.0, 100.0, 110.0};
  std::array<double, 3> iv_out{};
  std::array<atx::vol::Status, 3> status{};

  const atx::vol::Result<std::size_t> short_k =
      american_implied_vol_batch(price, 100.0, std::span<const double>(K.data(), 2), 1.0, 0.05, 0.0,
                                 Side::Call, iv_out, status);
  ASSERT_FALSE(short_k.has_value());
  EXPECT_EQ(short_k.error().code(), atx::vol::ErrorCode::InvalidArgument);
  EXPECT_EQ(short_k.error().message(), "american_implied_vol_batch: span length mismatch");

  const atx::vol::Result<std::size_t> short_iv =
      american_implied_vol_batch(price, 100.0, K, 1.0, 0.05, 0.0, Side::Call,
                                 std::span<double>(iv_out.data(), 2), status);
  ASSERT_FALSE(short_iv.has_value());
  EXPECT_EQ(short_iv.error().code(), atx::vol::ErrorCode::InvalidArgument);
  EXPECT_EQ(short_iv.error().message(), "american_implied_vol_batch: span length mismatch");

  const atx::vol::Result<std::size_t> short_status =
      american_implied_vol_batch(price, 100.0, K, 1.0, 0.05, 0.0, Side::Call, iv_out,
                                 std::span<atx::vol::Status>(status.data(), 2));
  ASSERT_FALSE(short_status.has_value());
  EXPECT_EQ(short_status.error().code(), atx::vol::ErrorCode::InvalidArgument);
  EXPECT_EQ(short_status.error().message(), "american_implied_vol_batch: span length mismatch");
}

TEST(AmericanIvErrorModel, BatchCarriesTheLaneCountItWrote) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.0, sigma = 0.25;
  const Side side = Side::Call;
  const std::array<double, 3> K{80.0, 95.0, 105.0};
  std::array<double, 3> price{};
  for (std::size_t i = 0; i < K.size(); ++i) {
    price[i] =
        value_or_fail(american_price(S, K[i], T, sigma, r, q, side, AmericanMethod::AndersenLake));
  }
  std::array<double, 3> iv_out{};
  std::array<atx::vol::Status, 3> status{};
  const atx::vol::Result<std::size_t> wrote =
      american_implied_vol_batch(price, S, K, T, r, q, side, iv_out, status);
  ASSERT_TRUE(wrote.has_value()) << (wrote ? std::string{} : wrote.error().to_string());
  EXPECT_EQ(*wrote, K.size());

  // A lane that fails is still a lane written: the count is the length of the
  // parallel per-lane channel the caller must walk, and that channel — not the
  // return — owns the per-lane verdict.
  const std::array<double, 2> k_fail{90.0, 90.0}; // intrinsic = 10
  const double good =
      value_or_fail(american_price(S, 90.0, T, 0.2, r, q, side, AmericanMethod::AndersenLake));
  const std::array<double, 2> price_fail{5.0, good}; // lane 0 is sub-intrinsic
  std::array<double, 2> iv_fail{};
  std::array<atx::vol::Status, 2> status_fail{};
  const atx::vol::Result<std::size_t> partial =
      american_implied_vol_batch(price_fail, S, k_fail, T, r, q, side, iv_fail, status_fail);
  ASSERT_TRUE(partial.has_value());
  EXPECT_EQ(*partial, std::size_t{2});
  EXPECT_FALSE(status_fail[0].has_value());
  EXPECT_TRUE(status_fail[1].has_value());

  // An empty batch is a well-formed zero-lane call, not a rejection.
  const atx::vol::Result<std::size_t> empty = american_implied_vol_batch(
      std::span<const double>{}, S, std::span<const double>{}, T, r, q, side, std::span<double>{},
      std::span<atx::vol::Status>{});
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(*empty, std::size_t{0});
}

// ── P6 (perf F9): warm-start chaining on the public IV batch ──────────────
//
// ECONOMIC-PARITY gate (PM ruling, reclassified from bit-identity). Warm chaining
// changes only the Newton SEED, but the safeguarded (rtsafe) inversion converges
// on |dx| < tol (1e-7), NOT on the machine root, so two seeds land on roots that
// agree only to ~tol; the cold Andersen-Lake path's 2-step post-convergence polish
// (american_iv.cpp:512-553) is likewise seed-dependent and widens the gap on the
// short-dated put wing. Measured max |ΔIV| on this COLD-AL grid is ~1.2e-6 — the
// same order the pre-existing WarmStartResultInvariantToSeed test bounds the cold
// warm_start at (1e-6), and ~4 orders below calib.cpp's 1e-4 de-Am economic budget.
// This grid pins the cold-AL path < 1e-5; the CACHED production hot path (no polish)
// is far tighter and pinned < 1e-9 in Batch_WarmChain_EconomicParityCachedPath.
TEST(AmericanIv, Batch_WarmChain_EconomicParityToColdAcrossGrid) {
  // A gentle smile so every quote is genuinely invertible with a sigma root.
  const auto smile = [](double k_log) { return 0.20 + 0.15 * k_log * k_log; };
  const double S = 100.0;
  const std::array<double, 13> K{70.0, 75.0,  80.0,  85.0,  90.0,  95.0, 100.0,
                                 105.0, 110.0, 115.0, 120.0, 125.0, 130.0};

  struct Regime {
    double T, r, q;
  };
  int checked = 0;
  double max_drift = 0.0;
  for (const Regime g : {Regime{0.25, 0.05, 0.00}, Regime{0.75, 0.03, 0.015},
                         Regime{1.50, 0.02, 0.04}}) {
    for (const Side side : {Side::Call, Side::Put}) {
      std::array<double, 13> price{};
      for (std::size_t i = 0; i < K.size(); ++i) {
        price[i] = value_or_fail(american_price(S, K[i], g.T, smile(std::log(K[i] / S)), g.r, g.q,
                                                side, AmericanMethod::AndersenLake));
      }
      std::array<double, 13> iv_cold{}, iv_warm{};
      std::array<atx::vol::Status, 13> st_cold{}, st_warm{};
      const auto sc = american_implied_vol_batch(price, S, K, g.T, g.r, g.q, side, iv_cold, st_cold,
                                                 AmericanMethod::AndersenLake, 1.0e-7, 64,
                                                 std::nullopt, nullptr, /*warm_start_chain=*/false);
      const auto sw = american_implied_vol_batch(price, S, K, g.T, g.r, g.q, side, iv_warm, st_warm,
                                                 AmericanMethod::AndersenLake, 1.0e-7, 64,
                                                 std::nullopt, nullptr, /*warm_start_chain=*/true);
      ASSERT_TRUE(sc.has_value());
      ASSERT_TRUE(sw.has_value());
      for (std::size_t i = 0; i < K.size(); ++i) {
        ASSERT_EQ(st_cold[i].has_value(), st_warm[i].has_value()) << "K=" << K[i];
        if (st_cold[i].has_value()) {
          const double drift = std::fabs(iv_warm[i] - iv_cold[i]);
          max_drift = std::fmax(max_drift, drift);
          EXPECT_LT(drift, 1.0e-5)
              << "warm-chain moved a root beyond the cold-AL parity budget: side=" << side_tag(side)
              << " T=" << g.T << " K=" << K[i] << " cold=" << iv_cold[i] << " warm=" << iv_warm[i];
          ++checked;
        }
      }
    }
  }
  std::cout << "[P6 F9 parity] batch cold-AL cold-vs-warm max |dIV| = " << max_drift
            << " (budget 1e-5; cached path pinned < 1e-9 separately)\n";
  EXPECT_GT(checked, 0);
}

// Same economic-parity gate on the CACHED (correction-cache) forward map — the hot
// path the fitter actually uses. No cold polish here, so the drift is tighter (a
// few ULP), still bounded < 1e-9.
TEST(AmericanIv, Batch_WarmChain_EconomicParityCachedPath) {
  const double r = 0.05, q = 0.0, S = 100.0, T = 0.75;
  const CorrectionCache cache = make_iv_correction(r, q); // Put cache
  const std::array<double, 11> K{80.0, 84.0, 88.0, 92.0, 96.0,  100.0,
                                 104.0, 108.0, 112.0, 116.0, 120.0};
  std::array<double, 11> price{};
  for (std::size_t i = 0; i < K.size(); ++i) {
    price[i] = american_price_cached(S, K[i], T, 0.20 + 0.15 * std::log(K[i] / S) * std::log(K[i] / S),
                                     r, q, Side::Put, &cache);
  }
  std::array<double, 11> iv_cold{}, iv_warm{};
  std::array<atx::vol::Status, 11> st_cold{}, st_warm{};
  ASSERT_TRUE(american_implied_vol_batch(price, S, K, T, r, q, Side::Put, iv_cold, st_cold,
                                         AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt,
                                         &cache, /*warm_start_chain=*/false)
                  .has_value());
  ASSERT_TRUE(american_implied_vol_batch(price, S, K, T, r, q, Side::Put, iv_warm, st_warm,
                                         AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt,
                                         &cache, /*warm_start_chain=*/true)
                  .has_value());
  for (std::size_t i = 0; i < K.size(); ++i) {
    ASSERT_TRUE(st_cold[i].has_value() && st_warm[i].has_value()) << "K=" << K[i];
    EXPECT_LT(std::fabs(iv_warm[i] - iv_cold[i]), 1.0e-9)
        << "cached warm-chain moved a root beyond the parity budget at K=" << K[i];
  }
}

// Counter proof (perf F9). Warm chaining seeds each lane from the previous lane's
// root, so the batch spends FEWER residual/Newton evaluations than the cold
// per-quote-seed batch for the (economic-parity) roots. `IvNewtonIters` is the always-on
// solve-ledger plane, so the delta is proven on the shipping (counters-OFF)
// binary; the gated `ClenshawSweeps` block adds the exact traversal delta for the
// sprint-end counters-ON sweep. The exact cold vs warm counts are printed by the
// test (see the [P6 F9 counters] line); the gate only requires warm < cold.
TEST(AmericanIv, Batch_WarmChain_CutsResidualEvals) {
  namespace led = atx::vol::counters::ledger;
  using atx::vol::counters::Counter;
  const double r = 0.05, q = 0.0, S = 100.0, T = 0.75;
  const CorrectionCache cache = make_iv_correction(r, q); // Put cache
  const std::array<double, 11> K{80.0, 84.0, 88.0, 92.0, 96.0,  100.0,
                                 104.0, 108.0, 112.0, 116.0, 120.0};
  std::array<double, 11> price{};
  for (std::size_t i = 0; i < K.size(); ++i) {
    price[i] = american_price_cached(S, K[i], T, 0.20 + 0.15 * std::log(K[i] / S) * std::log(K[i] / S),
                                     r, q, Side::Put, &cache);
  }
  std::array<double, 11> iv{};
  std::array<atx::vol::Status, 11> st{};

  const auto run = [&](bool warm) -> std::pair<std::uint64_t, std::uint64_t> {
    led::reset();
    atx::vol::counters::reset();
    const led::Counts before_led = led::snapshot();
    const std::uint64_t before_sweeps = atx::vol::counters::snapshot().get(Counter::ClenshawSweeps);
    EXPECT_TRUE(american_implied_vol_batch(price, S, K, T, r, q, Side::Put, iv, st,
                                           AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt,
                                           &cache, warm)
                    .has_value());
    const std::uint64_t iters = (led::snapshot() - before_led).get(led::Solve::IvNewtonIters);
    const std::uint64_t sweeps =
        atx::vol::counters::snapshot().get(Counter::ClenshawSweeps) - before_sweeps;
    return std::pair<std::uint64_t, std::uint64_t>{iters, sweeps};
  };

  const auto [cold_iters, cold_sweeps] = run(false);
  const auto [warm_iters, warm_sweeps] = run(true);
  std::cout << "[P6 F9 counters] batch IvNewtonIters cold=" << cold_iters << " warm=" << warm_iters
            << " (always-on ledger)\n";
  ASSERT_GT(cold_iters, 0u);
  EXPECT_LT(warm_iters, cold_iters) << "warm chaining must reduce residual/Newton evaluations";
  if constexpr (atx::vol::counters::counters_enabled()) {
    std::cout << "[P6 F9 counters] batch ClenshawSweeps cold=" << cold_sweeps
              << " warm=" << warm_sweeps << " (gated cnt_)\n";
    EXPECT_LT(warm_sweeps, cold_sweeps);
  }
}

// ── T3: the Newton slope must differentiate the map being SOLVED ──────────
//
// The rtsafe refinement solves the AMERICAN residual but took its derivative
// from `american_vega`, which with a null correction is the pure Black-76
// EUROPEAN vega (american.cpp). docs/LEDGER.md measured the consequence: the
// proxy overstates the true Andersen-Lake slope 2-3x deep ITM, so every Newton
// step understeps, the rtsafe range test demotes it to bisection, and the
// inversion pays LINEAR convergence exactly where the proxy is worst. A secant
// slope over the two most recent residual evaluations differentiates the actual
// forward map, costs no extra pricer call, and stays inside the same sign
// bracket. `IvNewtonIters` is the always-on solve-ledger plane, so this counts
// residual evaluations on the shipping (counters-OFF) binary.
TEST(AmericanIv, DeepItmColdInversion_SecantSlopeCutsResidualEvals) {
  namespace led = atx::vol::counters::ledger;
  const double S = 100.0, r = 0.05, q = 0.0;
  const std::array<double, 4> K{104.0, 110.0, 116.0, 122.0};
  const std::array<double, 3> T{0.5, 1.0, 2.0};
  const std::array<double, 2> sig{0.25, 0.40};

  // Price the grid first so the ledger only sees the inversions.
  struct Quote { double K, T, sigma, price; };
  std::vector<Quote> quotes;
  for (double k : K) {
    for (double t : T) {
      for (double s : sig) {
        const auto p = american_price(S, k, t, s, r, q, Side::Put, AmericanMethod::AndersenLake);
        if (p.has_value() && std::isfinite(*p)) {
          quotes.push_back(Quote{k, t, s, *p});
        }
      }
    }
  }
  ASSERT_GE(quotes.size(), 24u);

  led::reset();
  const led::Counts before = led::snapshot();
  int recovered = 0;
  for (const Quote &qt : quotes) {
    const auto iv = american_implied_vol(qt.price, S, qt.K, qt.T, r, q, Side::Put,
                                         AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt,
                                         /*correction=*/nullptr, /*warm_start=*/0.0);
    ASSERT_TRUE(iv.has_value()) << "K=" << qt.K << " T=" << qt.T << " sig=" << qt.sigma << ": "
                                << (iv ? std::string{} : iv.error().to_string());
    // The slope change may not move a converged root beyond the cold-AL noise.
    EXPECT_NEAR(*iv, qt.sigma, 1.0e-5) << "K=" << qt.K << " T=" << qt.T;
    ++recovered;
  }
  const std::uint64_t iters = (led::snapshot() - before).get(led::Solve::IvNewtonIters);
  const double per_quote = static_cast<double>(iters) / static_cast<double>(recovered);
  std::cout << "[T3 slope] cold deep-ITM inversions=" << recovered
            << " IvNewtonIters=" << iters << " (" << per_quote << " residual evals/quote)\n";
  // MEASURED baseline with the Black-76-proxy vega: 222 residual evaluations over
  // 24 quotes = 9.25 per quote. The secant slope must beat it by a clear margin.
  EXPECT_LT(per_quote, 7.5) << "iters=" << iters << " over " << recovered << " quotes";
}

TEST(AmericanIv, LightweightTelemetryMeasuresCompleteInversionKernel) {
  namespace lw = atx::vol::counters::lightweight;
  constexpr double S = 100.0;
  constexpr double K = 105.0;
  constexpr double T = 0.75;
  constexpr double sigma = 0.24;
  constexpr double r = 0.04;
  constexpr double q = 0.01;
  const double price =
      value_or_fail(american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::AndersenLake));

  lw::reset();
  for (std::uint32_t i = 0; i < lw::kSamplePeriod; ++i) {
    const auto iv = american_implied_vol(price, S, K, T, r, q, Side::Put);
    ASSERT_TRUE(iv.has_value());
  }
  const lw::Snapshot measured = lw::snapshot();
  EXPECT_EQ(measured.american_iv_samples, 1u);
  EXPECT_GT(measured.residual_evaluations_in_sampled_iv, 0u);
  EXPECT_GT(measured.boundary_solves_in_sampled_iv, 0u);
  EXPECT_GT(measured.exp_calls_in_sampled_iv, 0u);
}

TEST(AmericanIv, RetainedThreadLocalPricerAllocatesOnlyOnFirstColdInversion) {
  using atx::vol::counters::Counter;
  if constexpr (!atx::vol::counters::counters_enabled()) {
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  }

  const AlOpts fast = atx::vol::al_fast_opts();
  constexpr double sigma_a = 0.24;
  constexpr double sigma_b = 0.34;
  const double price_a = value_or_fail(american_price(100.0, 105.0, 0.75, sigma_a, 0.04, 0.01,
                                                      Side::Put, AmericanMethod::AndersenLake));
  const double price_b = value_or_fail(american_price(180.0, 155.0, 0.35, sigma_b, 0.025, 0.065,
                                                      Side::Call, AmericanMethod::AndersenLake,
                                                      fast));

  bool inversion_ok = true;
  std::uint64_t first_allocations = 0u;
  std::uint64_t reuse_allocations = 0u;
  atx::vol::counters::reset();
  std::jthread worker([&] {
    const auto first = american_implied_vol(price_a, 100.0, 105.0, 0.75, 0.04, 0.01, Side::Put);
    inversion_ok = first.has_value() && std::fabs(*first - sigma_a) < 1.0e-5;
    first_allocations = atx::vol::counters::snapshot().get(Counter::AloStateAllocations);

    atx::vol::counters::reset();
    for (int i = 0; i < 8; ++i) {
      const bool use_fast = (i & 1) != 0;
      const auto iv = use_fast
                          ? american_implied_vol(price_b, 180.0, 155.0, 0.35, 0.025, 0.065,
                                                 Side::Call, AmericanMethod::AndersenLake, 1.0e-7,
                                                 64, fast)
                          : american_implied_vol(price_a, 100.0, 105.0, 0.75, 0.04, 0.01,
                                                 Side::Put);
      inversion_ok = inversion_ok && iv.has_value() &&
                     std::fabs(*iv - (use_fast ? sigma_b : sigma_a)) < 1.0e-5;
    }
    reuse_allocations = atx::vol::counters::snapshot().get(Counter::AloStateAllocations);
  });
  worker.join();

  EXPECT_TRUE(inversion_ok);
  EXPECT_EQ(first_allocations, 1u);
  EXPECT_EQ(reuse_allocations, 0u);
}

TEST(AmericanIv, BawAndCachedMapsBypassThreadLocalAloState) {
  using atx::vol::counters::Counter;
  if constexpr (!atx::vol::counters::counters_enabled()) {
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  }

  constexpr double S = 100.0;
  constexpr double K = 102.0;
  constexpr double T = 0.5;
  constexpr double sigma = 0.27;
  constexpr double r = 0.04;
  constexpr double q = 0.0;
  const double baw_price =
      value_or_fail(american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::Baw));
  const CorrectionCache cache = make_iv_correction(r, q);
  const double cache_price = american_price_cached(S, K, T, sigma, r, q, Side::Put, &cache);

  bool baw_ok = false;
  bool cache_ok = false;
  atx::vol::counters::reset();
  std::jthread worker([&] {
    const auto baw_iv =
        american_implied_vol(baw_price, S, K, T, r, q, Side::Put, AmericanMethod::Baw);
    const auto cache_iv = american_implied_vol(cache_price, S, K, T, r, q, Side::Put,
                                               AmericanMethod::AndersenLake, 1.0e-7, 64,
                                               std::nullopt, &cache);
    baw_ok = baw_iv.has_value() && std::fabs(*baw_iv - sigma) < 1.0e-3;
    cache_ok = cache_iv.has_value() && std::fabs(*cache_iv - sigma) < 1.0e-6;
  });
  worker.join();

  EXPECT_TRUE(baw_ok);
  EXPECT_TRUE(cache_ok);
  EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::AloStateAllocations), 0u);
}

// Perf review F1 (ATX_VOL_COUNTERS-only): on a fitter-style cache-backed inversion
// fixture (~200 quotes), the fused Newton step shrinks the correction-tensor
// traversal count. Each bracketing residual is one value sweep (price only); each
// Newton refinement step is the fused price+vega single pass — 1 sweep (was 3).
// Reports totals at the inversion granularity and bounds sweeps/residual-step at
// the fused-path ceiling.
TEST(AmericanIv, FusedCachedInversionTraversalCount) {
  using atx::vol::counters::Counter;
  if constexpr (!atx::vol::counters::counters_enabled()) {
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  }
  auto built = CorrectionCache::build(/*n_k=*/16, /*n_T=*/12, /*n_s=*/8, /*r=*/0.05, /*q=*/0.0,
                                      -0.4, 0.4, 0.05, 1.0, 0.10, 0.60, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);
  const double S = 100.0, r = 0.05, q = 0.0;

  // 25 strikes × 4 maturities × 2 vols = 200 quotes, priced by the cached map so
  // each inverts to a known root through the fused Newton loop.
  struct Quote {
    double K, T, sigma, price;
  };
  std::vector<Quote> quotes;
  for (double K = 80.0; K <= 128.5; K += 2.0) {
    for (double T : {0.15, 0.40, 0.75, 0.90}) {
      for (double sig : {0.18, 0.35}) {
        quotes.push_back({K, T, sig, american_price_cached(S, K, T, sig, r, q, Side::Put, &tbl)});
      }
    }
  }

  atx::vol::counters::reset();
  atx::vol::counters::ledger::reset();
  int ok = 0;
  for (const Quote &qt : quotes) {
    const auto iv = american_implied_vol(qt.price, S, qt.K, qt.T, r, q, Side::Put,
                                         AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt, &tbl);
    if (iv) {
      ++ok;
    }
  }
  const std::uint64_t sweeps = atx::vol::counters::snapshot().get(Counter::ClenshawSweeps);
  const std::uint64_t newton =
      atx::vol::counters::ledger::snapshot().get(atx::vol::counters::ledger::Solve::IvNewtonIters);
  ASSERT_GT(newton, 0u);
  std::cout << "[P1 F1 counters] " << ok << " cached inversions: " << sweeps
            << " ClenshawSweeps over " << newton << " residual/Newton steps ("
            << static_cast<double>(sweeps) / static_cast<double>(newton) << " sweeps/step)\n";
  EXPECT_EQ(ok, static_cast<int>(quotes.size()));
  // Fused-path ceiling: every residual step (bracketing OR fused refinement) is 1
  // sweep, plus one initial-df vega (2 sweeps) per inversion. So sweeps <= steps + 2·N.
  EXPECT_LE(sweeps, newton + 2u * static_cast<std::uint64_t>(ok));
}

// Perf review F1 stage (b) economic-parity fixture. The fused single-pass
// value+dsigma keeps the correction VALUE (hence the Newton residual) bit-identical
// to stage (a); only the sigma partial (vega) changes its floating-point
// accumulation, and where vega collapses the safeguarded Newton falls to bisection
// (vega-independent), so the recovered IV moves at most sub-ULP. Recover IV for a
// fixed ~200-quote grid (both sides); set ATX_P1B_DUMP=1 to print each IV to full
// precision so a diff of the stage-(a) and stage-(b) builds bounds |dIV| directly.
TEST(AmericanIv, FusedTraversalIvEconomicParityFixture) {
  auto put = CorrectionCache::build(16, 12, 8, 0.05, 0.00, -0.4, 0.4, 0.05, 1.0, 0.10, 0.60,
                                    Side::Put);
  auto call = CorrectionCache::build(16, 12, 8, 0.045, 0.02, -0.4, 0.4, 0.05, 1.0, 0.10, 0.60,
                                     Side::Call);
  ASSERT_TRUE(put.has_value());
  ASSERT_TRUE(call.has_value());
  const CorrectionCache put_c = std::move(*put);
  const CorrectionCache call_c = std::move(*call);
  // Opt-in per-IV dump (ATX_P1B_DUMP): _dupenv_s on Windows — plain std::getenv
  // trips /WX (-Wdeprecated-declarations) under clang-cl.
  const auto dump_enabled = []() noexcept -> bool {
#if defined(_WIN32)
    char *value = nullptr;
    std::size_t size = 0u;
    const bool present = ::_dupenv_s(&value, &size, "ATX_P1B_DUMP") == 0 && value != nullptr;
    const bool on = present && value[0] != '\0' && std::strcmp(value, "0") != 0;
    std::free(value);
    return on;
#else
    const char *value = std::getenv("ATX_P1B_DUMP");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
#endif
  };
  const bool dump = dump_enabled();
  const double S = 100.0;
  int n = 0;
  for (Side side : {Side::Put, Side::Call}) {
    const double r = (side == Side::Put) ? 0.05 : 0.045;
    const double q = (side == Side::Put) ? 0.00 : 0.02;
    const CorrectionCache &c = (side == Side::Put) ? put_c : call_c;
    for (double K = 85.0; K <= 115.5; K += 3.0) {
      for (double T : {0.15, 0.40, 0.75}) {
        for (double sig : {0.18, 0.30, 0.45}) {
          const double price = american_price_cached(S, K, T, sig, r, q, side, &c);
          const auto iv = american_implied_vol(price, S, K, T, r, q, side,
                                                AmericanMethod::AndersenLake, 1.0e-8, 64,
                                                std::nullopt, &c);
          ASSERT_TRUE(iv.has_value()) << "K=" << K << " T=" << T << " s=" << sig;
          // The forward map is bit-identical across stage a/b, so the recovered IV
          // is stable and always finite and inside the search bracket.
          EXPECT_TRUE(std::isfinite(*iv));
          EXPECT_GE(*iv, atx::vol::kIvMin);
          EXPECT_LE(*iv, 40.0);
          if (dump) {
            std::cout << "[P1b IV] " << static_cast<int>(side) << " K=" << K << " T=" << T
                      << " s=" << sig << " iv=" << std::setprecision(17) << *iv << "\n";
          }
          ++n;
        }
      }
    }
  }
  EXPECT_EQ(n, 198);
}

// ── L1c (2026-10-19): the opt-in non-monotone refusal ─────────────────────
//
// The cold Andersen-Lake map is not monotone in sigma inside the kernel's own
// exercise region close to the zero-vol value (american_iv.hpp, "Typed entry point
// with the opt-in non-monotone refusal"; region map and gate constants in
// sprints/2026-10-18-L1c-al-nonmonotone-sprint.md). Pinned here: the ledgered
// contract, the gate, the flag-off identity of the typed overload, and where the
// refusal fires and where it does not.

namespace {

using atx::vol::al_non_monotone_gate;
using atx::vol::al_non_monotone_region;
using atx::vol::andersen_lake;
using atx::vol::ErrorCode;
using atx::vol::exercise_boundary;
using atx::vol::IvOpts;
using atx::vol::IvResult;
using atx::vol::IvStatus;
using atx::vol::kIvMin;

// The ledgered trap contract (LEDGER 2026-10-17 | trap |): put S=100 K=128.989 T=2.
constexpr double kNmS = 100.0;
constexpr double kNmK = 128.989;
constexpr double kNmT = 2.0;
constexpr double kNmR = 0.05;
constexpr double kNmQ = 0.015;

[[nodiscard]] double cold_al(double S, double K, double T, double sig, double r, double q,
                             Side sd) {
  const atx::core::Result<double> p = andersen_lake(S, K, T, sig, r, q, sd);
  return p && std::isfinite(*p) ? *p : std::numeric_limits<double>::quiet_NaN();
}

// Spot on the exercise side of the kernel's own boundary at sigma (put S <= B).
[[nodiscard]] bool spot_exercises(double S, double K, double T, double sig, double r, double q,
                                  Side sd) {
  const atx::core::Result<double> b = exercise_boundary(K, T, sig, r, q, sd);
  return b && std::isfinite(*b) && (sd == Side::Put ? S <= *b : S >= *b);
}

// The sigma -> 0 American value, written out from its formula: the sup over exercise
// dates t in [0, T] of A e^{-a t} - B e^{-b t}, floored at 0.
[[nodiscard]] double zero_vol_value(double S, double K, double T, double r, double q, Side sd) {
  const bool call = sd == Side::Call;
  const double A = call ? S : K;
  const double a = call ? q : r;
  const double B = call ? K : S;
  const double b = call ? r : q;
  const auto pay = [&](double t) { return A * std::exp(-a * t) - B * std::exp(-b * t); };
  double best = std::fmax(pay(0.0), pay(T));
  const double ts = std::log((a * A) / (b * B)) / (a - b);
  if (std::isfinite(ts) && ts > 0.0 && ts < T) {
    best = std::fmax(best, pay(ts));
  }
  return std::fmax(best, 0.0);
}

// The typed status the positional overload's Error maps to (american_iv.hpp).
[[nodiscard]] IvStatus status_of(const atx::core::Result<double> &r) {
  if (r) {
    return IvStatus::Ok;
  }
  const ErrorCode c = r.error().code();
  return c == ErrorCode::InvalidArgument ? IvStatus::InvalidArgument
         : c == ErrorCode::OutOfRange    ? IvStatus::OutOfRange
         : c == ErrorCode::Unavailable   ? IvStatus::NoConvergence
                                         : IvStatus::Unpriceable;
}

[[nodiscard]] IvResult typed_iv(double price, double S, double K, double T, double r, double q,
                                Side sd, bool refuse,
                                AmericanMethod method = AmericanMethod::AndersenLake) {
  IvOpts o;
  o.method = method;
  o.refuse_non_monotone = refuse;
  return american_implied_vol(price, S, K, T, r, q, sd, o);
}

[[nodiscard]] bool same_double(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

} // namespace

// The ledgered numbers: +7.745e-6 above the zero-vol value at sigma 0.14, flat at
// it on [0.17, 0.18], +7.4e-4 at 0.185. Every decreasing step lies in the kernel's
// own exercise region (S <= B(T; sigma)); the map rises again once S leaves it.
TEST(AmericanIvNonMonotone, LedgerContractDipsOnlyInsideTheKernelExerciseRegion) {
  const auto P = [](double sig) { return cold_al(kNmS, kNmK, kNmT, sig, kNmR, kNmQ, Side::Put); };
  const auto ex = [](double sig) {
    return spot_exercises(kNmS, kNmK, kNmT, sig, kNmR, kNmQ, Side::Put);
  };
  const double zv = zero_vol_value(kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put);
  EXPECT_EQ(zv, kNmK - kNmS); // r > q: the immediate intrinsic binds
  EXPECT_NEAR(P(0.14) - zv, 7.745e-6, 1.0e-9);
  for (const double sig : {0.17, 0.175, 0.18}) {
    EXPECT_LE(std::abs(P(sig) - zv), 1.0e-12) << sig;
  }
  EXPECT_NEAR(P(0.185) - zv, 7.4027e-4, 1.0e-7);
  int n_dec = 0;
  double prev = P(0.14);
  for (int k = 1; k <= 6; ++k) {
    const double sig = 0.14 + 0.005 * k;
    const double p = P(sig);
    if (p < prev) {
      ++n_dec;
      EXPECT_TRUE(ex(sig)) << "decreasing step outside the exercise region at " << sig;
    }
    prev = p;
  }
  EXPECT_EQ(n_dec, 6); // 0.145 .. 0.17
  EXPECT_TRUE(ex(0.14));
  EXPECT_FALSE(ex(0.185));
  // The band and the region predicate classify the peak target.
  const double tgt = P(0.14);
  EXPECT_LE(tgt - zv, al_non_monotone_gate(kNmK, kNmT, std::nullopt));
  EXPECT_TRUE(al_non_monotone_region(tgt, kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put, 0.14,
                                     std::nullopt));
  EXPECT_FALSE(al_non_monotone_region(tgt, kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put, 0.185,
                                      std::nullopt));
  // Far above the band the predicate is false even with the spot in the region.
  EXPECT_FALSE(al_non_monotone_region(P(0.3), kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put, 0.14,
                                      std::nullopt));
}

// kappa * K * max(1, T)^2: kappa 5e-6 for the accurate scheme (nullopt or its explicit
// Fast-profile equivalent), 4e-4 for any other scheme; NaN for invalid K / T.
TEST(AmericanIvNonMonotone, GateScalesWithStrikeAndSquaredMaturity) {
  using atx::vol::al_fast_opts;
  using atx::vol::al_fast_profile;
  EXPECT_DOUBLE_EQ(al_non_monotone_gate(100.0, 0.5, std::nullopt), 5.0e-6 * 100.0);
  EXPECT_DOUBLE_EQ(al_non_monotone_gate(kNmK, kNmT, std::nullopt), 5.0e-6 * kNmK * 4.0);
  EXPECT_DOUBLE_EQ(al_non_monotone_gate(kNmK, kNmT, al_fast_profile(std::nullopt)),
                   5.0e-6 * kNmK * 4.0);
  EXPECT_DOUBLE_EQ(al_non_monotone_gate(kNmK, kNmT, al_fast_opts()), 4.0e-4 * kNmK * 4.0);
  EXPECT_TRUE(std::isnan(al_non_monotone_gate(0.0, 1.0, std::nullopt)));
  EXPECT_TRUE(std::isnan(al_non_monotone_gate(100.0, 0.0, std::nullopt)));
  const double nan_strike = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(std::isnan(al_non_monotone_gate(nan_strike, 1.0, std::nullopt)));
}

// With refuse_non_monotone false the typed overload IS the positional one: same bits,
// mapped status, over deep-ITM long-dated contracts whose targets sit on, near and far
// above the zero-vol value. With it true, the only change is Ok -> NonMonotoneRegion,
// and only for targets inside the band.
TEST(AmericanIvNonMonotone, FlagOffIsThePositionalOverloadAndFlagOnOnlyRefuses) {
  const double S = 100.0;
  std::size_t n = 0;
  std::size_t refused = 0;
  std::size_t kept_ok = 0;
  for (const Side sd : {Side::Put, Side::Call}) {
    for (const double m : {1.05, 1.2, 1.29, 1.4}) {
      const double K = sd == Side::Put ? S * m : S / m;
      for (const double T : {0.5, 2.0, 5.0}) {
        for (const std::array<double, 2> rq :
             {std::array<double, 2>{0.05, 0.0}, std::array<double, 2>{0.05, 0.015},
              std::array<double, 2>{0.05, 0.05}}) {
          // Calls mirror the put carries (q and r swap roles).
          const double r = sd == Side::Put ? rq[0] : rq[1];
          const double q = sd == Side::Put ? rq[1] : rq[0];
          const double zv = zero_vol_value(S, K, T, r, q, sd);
          const double gate = al_non_monotone_gate(K, T, std::nullopt);
          std::vector<double> targets;
          for (const double sig : {0.1, 0.2, 0.4}) {
            targets.push_back(cold_al(S, K, T, sig, r, q, sd));
          }
          for (const double x : {1.0e-6, 1.0e-5, 1.0e-4}) {
            targets.push_back(zv + x * K); // inside the band, or above it at short T
          }
          for (const double p : targets) {
            ++n;
            const atx::core::Result<double> pos = american_implied_vol(p, S, K, T, r, q, sd);
            const IvResult off = typed_iv(p, S, K, T, r, q, sd, false);
            const IvResult on = typed_iv(p, S, K, T, r, q, sd, true);
            ASSERT_EQ(off.status, status_of(pos)) << "K=" << K << " T=" << T << " p=" << p;
            if (pos) {
              EXPECT_TRUE(same_double(off.iv, *pos)) << "K=" << K << " T=" << T << " p=" << p;
            } else {
              EXPECT_TRUE(std::isnan(off.iv));
            }
            if (on.status == IvStatus::NonMonotoneRegion) {
              ++refused;
              EXPECT_EQ(off.status, IvStatus::Ok);
              EXPECT_TRUE(std::isnan(on.iv));
              EXPECT_LE(p - zv, gate) << "refused above the band: K=" << K << " T=" << T;
              continue;
            }
            EXPECT_EQ(on.status, off.status) << "K=" << K << " T=" << T << " p=" << p;
            EXPECT_TRUE(same_double(on.iv, off.iv) || (std::isnan(on.iv) && std::isnan(off.iv)));
            kept_ok += on.status == IvStatus::Ok ? 1u : 0u;
          }
        }
      }
    }
  }
  std::printf("[non-monotone scalar] %zu targets, %zu refused, %zu Ok kept\n", n, refused,
              kept_ok);
  EXPECT_EQ(n, 2u * 4u * 3u * 3u * 6u);
  EXPECT_GT(refused, 0u);
  EXPECT_GT(kept_ok, refused);
}

// Inside the band the flag refuses both kinds of wrong Ok: an answer inside the
// kernel's exercise region (one of several roots, or the kIvMin clamp at the
// intrinsic) and an answer outside it that the cold map does not certify (the warm
// search stopped 0.002-0.02 vol off; LEDGER 2026-10-17 trap).
TEST(AmericanIvNonMonotone, RefusesWrongOkAnswersInsideTheRegion) {
  struct Case {
    const char *name;
    double K, T, r, q;
    double sig_true; // target = cold AL at sig_true; <= 0: target = zero-vol value - sig_true
    double off_iv;   // today's (flag-off) answer
    bool off_in_region;
  };
  const Case cases[] = {
      {"ledger bump", kNmK, kNmT, kNmR, kNmQ, -5.0e-6, 0.0913378, true},
      {"ledger peak", kNmK, kNmT, kNmR, kNmQ, 0.14, 0.0913378, true},
      {"N2 K140 T5", 140.0, 5.0, 0.05, 0.0, 0.2, 0.2190581, false},
      {"K130 T5 r=q", 130.0, 5.0, 0.05, 0.05, 0.1, 0.1022202, false},
      {"intrinsic clamp", 140.0, 1.0, 0.05, 0.0, 0.0, kIvMin, true},
  };
  const double S = 100.0;
  for (const Case &c : cases) {
    const double zv = zero_vol_value(S, c.K, c.T, c.r, c.q, Side::Put);
    const double p =
        c.sig_true > 0.0 ? cold_al(S, c.K, c.T, c.sig_true, c.r, c.q, Side::Put) : zv - c.sig_true;
    ASSERT_LE(p - zv, al_non_monotone_gate(c.K, c.T, std::nullopt)) << c.name;
    const IvResult off = typed_iv(p, S, c.K, c.T, c.r, c.q, Side::Put, false);
    ASSERT_EQ(off.status, IvStatus::Ok) << c.name;
    EXPECT_NEAR(off.iv, c.off_iv, 1.0e-6) << c.name;
    EXPECT_EQ(spot_exercises(S, c.K, c.T, off.iv, c.r, c.q, Side::Put), c.off_in_region)
        << c.name;
    if (!c.off_in_region) {
      // Not a root of the cold map: the residual is far above any certified answer's.
      EXPECT_GT(std::abs(cold_al(S, c.K, c.T, off.iv, c.r, c.q, Side::Put) - p), 1.0e-5)
          << c.name;
    }
    const IvResult on = typed_iv(p, S, c.K, c.T, c.r, c.q, Side::Put, true);
    EXPECT_EQ(on.status, IvStatus::NonMonotoneRegion) << c.name;
    EXPECT_TRUE(std::isnan(on.iv)) << c.name;
    IvOpts opts;
    opts.refuse_non_monotone = true;
    const auto retained = atx::vol::american_implied_vol_search(
        p, S, c.K, c.T, c.r, c.q, Side::Put, opts);
    EXPECT_EQ(retained.answer.status, on.status) << c.name;
    EXPECT_TRUE(std::isnan(retained.answer.iv)) << c.name;
    ASSERT_TRUE(retained.rejected_candidate) << c.name;
    EXPECT_TRUE(same_double(*retained.rejected_candidate, off.iv)) << c.name;
  }
}

TEST(AmericanIvNonMonotone, SearchRetainsNoCandidateOnSuccessOrError) {
  IvOpts opts;
  opts.refuse_non_monotone = true;
  for (double price : {cold_al(100.0, 100.0, 1.0, 0.25, 0.05, 0.0, Side::Call),
                       -1.0, 200.0, std::numeric_limits<double>::quiet_NaN()}) {
    const auto reference = american_implied_vol(price, 100.0, 100.0, 1.0, 0.05, 0.0,
                                                Side::Call, opts);
    const auto retained = atx::vol::american_implied_vol_search(
        price, 100.0, 100.0, 1.0, 0.05, 0.0, Side::Call, opts);
    EXPECT_EQ(retained.answer.status, reference.status);
    EXPECT_TRUE(same_double(retained.answer.iv, reference.iv));
    EXPECT_FALSE(retained.rejected_candidate);
  }
  opts.refuse_non_monotone = false;
  const double price = zero_vol_value(kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put) + 5e-6;
  const auto reference = american_implied_vol(price, kNmS, kNmK, kNmT, kNmR, kNmQ,
                                              Side::Put, opts);
  ASSERT_EQ(reference.status, IvStatus::Ok);
  const auto retained = atx::vol::american_implied_vol_search(
      price, kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put, opts);
  EXPECT_EQ(retained.answer.status, reference.status);
  EXPECT_TRUE(same_double(retained.answer.iv, reference.iv));
  EXPECT_FALSE(retained.rejected_candidate);
}

TEST(AmericanIvNonMonotone, PriceBudgetCandidateUsesColdTargetAndRefusesExercisePlateaus) {
  IvOpts opts;
  opts.refuse_non_monotone = true;
  struct Case { double S, K, T, r, q, price; Side side; };
  const Case cases[] = {
      {773.0, 850.0, 0.425459623336792, 0.04721422493457794,
       0.010136770165678387, 77.00167497706315, Side::Put},
      {773.0, 925.0, 2.257885694503784, 0.0515890046954155,
       0.00582131263777355, 152.01331004092836, Side::Put},
      {773.0, 65.0, 0.9224853515625, 0.04965313524007797,
       0.008097793258072, 708.0004269712528, Side::Call},
      {100.0, 140.0, 5.0, 0.05, 0.0,
       cold_al(100.0, 140.0, 5.0, 0.2, 0.05, 0.0, Side::Put), Side::Put},
      {100.0, 105.0, 0.25, 0.04, -0.01,
       cold_al(100.0, 105.0, 0.25, 0.3, 0.04, -0.01, Side::Call), Side::Call},
  };
  for (const auto &c : cases) {
    const auto candidate = atx::vol::american_implied_vol_price_candidate(
        c.price, c.S, c.K, c.T, c.r, c.q, c.side, opts, 0.001);
    ASSERT_TRUE(candidate) << c.K;
    const double actual_error = std::abs(cold_al(c.S, c.K, c.T, *candidate, c.r, c.q,
                                                 c.side) - c.price);
    EXPECT_LE(actual_error, 0.001) << c.K;
    EXPECT_FALSE(spot_exercises(c.S, c.K, c.T, *candidate, c.r, c.q, c.side));
    if (actual_error > 0.0) {
      EXPECT_FALSE(atx::vol::american_implied_vol_price_candidate(
          c.price, c.S, c.K, c.T, c.r, c.q, c.side, opts, actual_error * 0.5));
    }
  }
  for (const double budget : {0.0, -0.001, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_FALSE(atx::vol::american_implied_vol_price_candidate(
        10.0, 100.0, 100.0, 1.0, 0.05, 0.0, Side::Call, opts, budget));
  }
  const double in_region = zero_vol_value(kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put) + 5e-6;
  EXPECT_FALSE(atx::vol::american_implied_vol_price_candidate(
      in_region, kNmS, kNmK, kNmT, kNmR, kNmQ, Side::Put, opts, 0.001));
  EXPECT_FALSE(atx::vol::american_implied_vol_price_candidate(
      40.0, 100.0, 140.0, 1.0, 0.05, 0.0, Side::Put, opts, 0.001));
  for (const auto mutate : {+[](IvOpts &o) { o.warm_start = 0.25; },
                            +[](IvOpts &o) { o.price_tol = 0.001; },
                            +[](IvOpts &o) { o.method = AmericanMethod::Baw; }}) {
    IvOpts unsupported = opts;
    mutate(unsupported);
    EXPECT_FALSE(atx::vol::american_implied_vol_price_candidate(
        10.0, 100.0, 100.0, 1.0, 0.05, 0.0, Side::Call, unsupported, 0.001));
  }
}

TEST(AmericanIvNonMonotone, ExternalPriceSeedUsesColdChecksWithoutAnInverse) {
  namespace led = atx::vol::counters::ledger;
  IvOpts opts;
  opts.refuse_non_monotone = true;
  for (const Side side : {Side::Call, Side::Put}) {
    const double target = cold_al(100, 105, .5, .3, .04, .01, side);
    const auto before = led::snapshot();
    const auto answer = atx::vol::american_implied_vol_price_seed(
        target, 100, 105, .5, .04, .01, side, opts, .001, .3);
    const auto work = led::snapshot() - before;
    ASSERT_TRUE(answer);
    EXPECT_DOUBLE_EQ(*answer, .3);
    EXPECT_EQ(work.get(led::Solve::IvNewtonIters), 0u);
    EXPECT_LE(work.get(led::Solve::AlBoundarySolves), 2u);
    EXPECT_FALSE(atx::vol::american_implied_vol_price_seed(
        target, 100, 105, .5, .04, .01, side, opts, .001, .5));
  }
  EXPECT_FALSE(atx::vol::american_implied_vol_price_seed(
      40, 100, 140, 1, .05, 0, Side::Put, opts, .001, .05));
  for (const double sigma : {0.0, atx::vol::kIvMin, -1.0,
                              std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity()}) {
    EXPECT_FALSE(atx::vol::american_implied_vol_price_seed(
        10, 100, 100, 1, .04, .01, Side::Put, opts, .001, sigma));
  }
  EXPECT_FALSE(atx::vol::american_implied_vol_price_seed(
      std::numeric_limits<double>::quiet_NaN(), 100, 100, 1, .04, .01,
      Side::Put, opts, .001, .3));
  EXPECT_FALSE(atx::vol::american_implied_vol_price_seed(
      10, 100, 100, 1, .04, .01, static_cast<Side>(255), opts, .001, .3));
  EXPECT_FALSE(atx::vol::american_implied_vol_price_seed(
      10, 100, 100, 1, .04, .01, Side::Put, opts, 0, .3));
}

// Outside the region the flag changes nothing: a target far above the band on the
// ledgered contract, an ATM contract, the kIvMin clamp of a European-regime call (no
// exercise region), and the BAW route (the refusal is for the cold Andersen-Lake map).
TEST(AmericanIvNonMonotone, KeepsAnswersOutsideTheRegionBitIdentical) {
  const double S = 100.0;
  struct Case {
    const char *name;
    double K, T, r, q;
    Side sd;
    double price;
    AmericanMethod method;
  };
  const double call_zv = S - 90.0 * std::exp(-0.05); // K=90 T=1 r=.05 q=0 call
  const Case cases[] = {
      {"ledger at 0.3", kNmK, kNmT, kNmR, kNmQ, Side::Put,
       cold_al(S, kNmK, kNmT, 0.3, kNmR, kNmQ, Side::Put), AmericanMethod::AndersenLake},
      {"ATM put", 100.0, 1.0, 0.05, 0.0, Side::Put,
       cold_al(S, 100.0, 1.0, 0.25, 0.05, 0.0, Side::Put), AmericanMethod::AndersenLake},
      {"European-regime clamp", 90.0, 1.0, 0.05, 0.0, Side::Call, call_zv,
       AmericanMethod::AndersenLake},
      {"BAW in the band", kNmK, kNmT, kNmR, kNmQ, Side::Put, kNmK - kNmS + 5.0e-6,
       AmericanMethod::Baw},
  };
  for (const Case &c : cases) {
    const IvResult off = typed_iv(c.price, S, c.K, c.T, c.r, c.q, c.sd, false, c.method);
    const IvResult on = typed_iv(c.price, S, c.K, c.T, c.r, c.q, c.sd, true, c.method);
    EXPECT_EQ(on.status, off.status) << c.name;
    EXPECT_TRUE(same_double(on.iv, off.iv) || (std::isnan(on.iv) && std::isnan(off.iv)))
        << c.name;
  }
  // Pin what the first and third return, so "unchanged" is not "both refused".
  const IvResult ledger = typed_iv(cases[0].price, S, kNmK, kNmT, kNmR, kNmQ, Side::Put, true);
  ASSERT_EQ(ledger.status, IvStatus::Ok);
  EXPECT_NEAR(ledger.iv, 0.3, 1.0e-5);
  const IvResult clamp = typed_iv(call_zv, S, 90.0, 1.0, 0.05, 0.0, Side::Call, true);
  ASSERT_EQ(clamp.status, IvStatus::Ok);
  EXPECT_EQ(clamp.iv, kIvMin);
}

// ── Opt-in Premium (de-Americanised) inversion strategy (L2e, 2026-10-19) ──
// Contract: american_iv.hpp IvStrategy; per-contract memory: src/pricing/
// american_iv_premium.hpp. Pinned here: an accepted Premium root is the cold map's
// root to the Search tolerance and agrees with Search; a fallback IS Search bit for
// bit; the default strategy is Search; the opt-in refusal still fires; the memory
// cuts the cold solves of a second price of the same contract.

namespace {

using atx::vol::american_implied_vol_premium;
using atx::vol::IvPremiumFallback;
using atx::vol::IvPremiumMemory;
using atx::vol::IvPremiumStats;
using atx::vol::IvStrategy;
using atx::vol::kIvPremiumExtraSolves;
using atx::vol::kIvPremiumMaxSolves;

[[nodiscard]] std::uint64_t al_solves_now() {
  return atx::vol::counters::ledger::this_thread(
      atx::vol::counters::ledger::Solve::AlBoundarySolves);
}

// One contract of the grid and its target (cold AL at sig_true).
struct PremiumCase {
  double K, T, r, q;
  Side sd;
  double sig_true;
};

[[nodiscard]] std::vector<PremiumCase> premium_grid() {
  std::vector<PremiumCase> out;
  const double S = 100.0;
  // r - q positive, zero and negative; q = 0 (no dividend) and q > 0.
  const std::array<std::array<double, 2>, 4> carries{
      {{0.05, 0.0}, {0.04, 0.04}, {0.01, 0.05}, {0.03, 0.015}}};
  for (const Side sd : {Side::Put, Side::Call}) {
    for (const double m : {0.6, 0.85, 0.97, 1.0, 1.03, 1.2, 1.5}) {
      const double K = sd == Side::Put ? S * m : S / m; // m > 1: in the money
      for (const double T : {0.03, 0.25, 1.0, 3.0}) {
        for (const auto &rq : carries) {
          for (const double sig : {0.12, 0.3, 0.8}) {
            out.push_back({K, T, rq[0], rq[1], sd, sig});
          }
        }
      }
    }
  }
  return out;
}

[[nodiscard]] IvResult premium_iv(double price, double S, const PremiumCase &c,
                                  IvPremiumMemory *memory, IvPremiumStats &stats,
                                  bool refuse = false) {
  IvOpts o;
  o.refuse_non_monotone = refuse;
  return american_implied_vol_premium(price, S, c.K, c.T, c.r, c.q, c.sd, o, memory, stats);
}

} // namespace

// Over puts/calls, deep ITM to deep OTM, 11 days to 3 years, carry positive / zero /
// negative, with and without a dividend yield: every accepted Premium root reprices
// the target on the cold map to within the Search tolerance (Newton distance with the
// cold map's own chord slope) and lies within 4 tol of the Search answer (the Search
// polish may stop up to kPolishMaxDriftTols = 4 tol from its rtsafe root); every
// fallback returns the Search answer bit for bit. Premium spends at most 4 cold
// solves before a fallback and, accepted, fewer AL solves than Search on average.
TEST(AmericanIvPremium, ConvergesToTheSearchRootOrFallsBackBitForBit) {
  const double S = 100.0;
  const double tol = IvOpts{}.tol;
  std::size_t n = 0, accepted = 0, max_solves = 0;
  std::size_t al_search = 0, al_premium = 0;
  std::array<std::size_t, atx::vol::kIvPremiumFallbackCount> why{};
  double max_dsig = 0.0;
  double max_newton = 0.0;
  double us_search = 0.0, us_premium = 0.0;
  std::size_t search_off = 0; // accepted cases more than 4 tol from Search
  for (const PremiumCase &c : premium_grid()) {
    const double p = cold_al(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
    if (!std::isfinite(p)) {
      continue;
    }
    ++n;
    const std::uint64_t a0 = al_solves_now();
    const auto t0 = std::chrono::steady_clock::now();
    const IvResult search = typed_iv(p, S, c.K, c.T, c.r, c.q, c.sd, false);
    const auto t1 = std::chrono::steady_clock::now();
    const std::uint64_t a1 = al_solves_now();
    IvPremiumStats stats;
    const IvResult prem = premium_iv(p, S, c, nullptr, stats);
    const auto t2 = std::chrono::steady_clock::now();
    const std::uint64_t a2 = al_solves_now();
    us_search += std::chrono::duration<double, std::micro>(t1 - t0).count();
    us_premium += std::chrono::duration<double, std::micro>(t2 - t1).count();
    al_search += static_cast<std::size_t>(a1 - a0);
    al_premium += static_cast<std::size_t>(a2 - a1);
    ++why[static_cast<std::size_t>(stats.fallback)];
    max_solves = std::max<std::size_t>(max_solves, stats.premium_solves);
    ASSERT_LE(stats.premium_solves, kIvPremiumMaxSolves + kIvPremiumExtraSolves);
    const std::string where = "K=" + std::to_string(c.K) + " T=" + std::to_string(c.T) +
                              " r=" + std::to_string(c.r) + " q=" + std::to_string(c.q) +
                              (c.sd == Side::Put ? " put" : " call") +
                              " sig=" + std::to_string(c.sig_true);
    if (stats.fallback != IvPremiumFallback::None) {
      ASSERT_EQ(prem.status, search.status) << where;
      EXPECT_TRUE(same_double(prem.iv, search.iv) ||
                  (std::isnan(prem.iv) && std::isnan(search.iv)))
          << where;
      continue;
    }
    ++accepted;
    ASSERT_EQ(prem.status, IvStatus::Ok) << where;
    ASSERT_EQ(search.status, IvStatus::Ok) << where;
    // Newton distance of an answer on the cold map (chord over 1e-6 vol at the answer).
    const auto newton_distance = [&](double iv) {
      const double h = 1.0e-6;
      const double slope = (cold_al(S, c.K, c.T, iv + h, c.r, c.q, c.sd) -
                            cold_al(S, c.K, c.T, iv - h, c.r, c.q, c.sd)) /
                           (2.0 * h);
      return std::abs(cold_al(S, c.K, c.T, iv, c.r, c.q, c.sd) - p) / slope;
    };
    const double newton = newton_distance(prem.iv);
    max_newton = std::max(max_newton, newton);
    EXPECT_LE(newton, tol) << where;
    // The same root: the two answers differ by at most Search's own distance from the
    // cold root (its warm-map search and drift-capped polish; LEDGER 2026-10-17 trap)
    // plus the shared tolerance band.
    const double dsig = std::abs(prem.iv - search.iv);
    max_dsig = std::max(max_dsig, dsig);
    if (dsig > 4.0 * tol) {
      ++search_off;
    }
    EXPECT_LE(dsig, newton_distance(search.iv) + 2.0 * tol)
        << where << " premium " << prem.iv << " search " << search.iv;
  }
  std::printf("[premium grid] %zu targets, %zu accepted, max |dsigma| vs Search %.3g, max "
              "Newton distance %.3g, AL solves Search %zu / Premium %zu (max per attempt %zu)\n",
              n, accepted, max_dsig, max_newton, al_search, al_premium, max_solves);
  std::printf("[premium grid] wall us Search %.0f / Premium %.0f (unqualified); accepted "
              "answers > 4 tol from Search %zu\n",
              us_search, us_premium, search_off);
  std::printf("[premium grid] fallbacks:");
  for (std::size_t i = 1; i < why.size(); ++i) {
    std::printf(" %zu=%zu", i, why[i]);
  }
  std::printf("\n");
  EXPECT_GT(accepted, n * 3 / 4);
  // The only fallbacks on this grid: targets within the non-monotone band (tiny deep
  // OTM prices and near-intrinsic deep ITM ones) and a handful of slow convergences.
  EXPECT_LE(why[static_cast<std::size_t>(IvPremiumFallback::NoConvergence)], n / 50);
}

// Opt-in only: IvOpts defaults to Search with no seed profile, and the typed overload
// under the default IS the positional inverter. seed_profile unset and seed_profile =
// al_opts' own (Reference) profile are the same kernel: same bits, same solves. A Fast
// seed profile keeps the accepted root on the accurate map (one confirming solve).
TEST(AmericanIvPremium, DefaultsAreSearchAndTheAccurateSeedKernel) {
  const IvOpts defaults;
  EXPECT_EQ(defaults.strategy, IvStrategy::Search);
  EXPECT_FALSE(defaults.seed_profile.has_value());
  const double S = 100.0;
  const double tol = defaults.tol;
  std::size_t fast_accepted = 0;
  for (const PremiumCase &c : premium_grid()) {
    if (c.T != 1.0 || c.sig_true != 0.3) {
      continue; // a slice of the grid is enough here
    }
    const double p = cold_al(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
    const atx::core::Result<double> pos = american_implied_vol(p, S, c.K, c.T, c.r, c.q, c.sd);
    const IvResult typed = american_implied_vol(p, S, c.K, c.T, c.r, c.q, c.sd, defaults);
    ASSERT_EQ(typed.status, status_of(pos));
    if (pos) {
      EXPECT_TRUE(same_double(typed.iv, *pos));
    }
    IvPremiumStats unset_stats;
    const IvResult unset = premium_iv(p, S, c, nullptr, unset_stats);
    IvOpts own;
    own.seed_profile = atx::vol::AlProfile::Reference;
    IvPremiumStats own_stats;
    const IvResult explicit_own =
        american_implied_vol_premium(p, S, c.K, c.T, c.r, c.q, c.sd, own, nullptr, own_stats);
    EXPECT_EQ(explicit_own.status, unset.status);
    EXPECT_TRUE(same_double(explicit_own.iv, unset.iv) ||
                (std::isnan(explicit_own.iv) && std::isnan(unset.iv)));
    EXPECT_EQ(own_stats.premium_solves, unset_stats.premium_solves);
    EXPECT_EQ(own_stats.fallback, unset_stats.fallback);
    IvOpts fast;
    fast.seed_profile = atx::vol::AlProfile::Fast;
    IvPremiumStats fast_stats;
    const IvResult fast_iv =
        american_implied_vol_premium(p, S, c.K, c.T, c.r, c.q, c.sd, fast, nullptr, fast_stats);
    if (fast_stats.fallback == IvPremiumFallback::None) {
      ++fast_accepted;
      ASSERT_EQ(fast_iv.status, IvStatus::Ok);
      EXPECT_LE(std::abs(fast_iv.iv - c.sig_true), tol);
    }
    // Through the public typed overload, Premium is the same computation.
    IvOpts pub;
    pub.strategy = IvStrategy::Premium;
    const IvResult via_public = american_implied_vol(p, S, c.K, c.T, c.r, c.q, c.sd, pub);
    EXPECT_TRUE(same_double(via_public.iv, unset.iv) ||
                (std::isnan(via_public.iv) && std::isnan(unset.iv)));
  }
  EXPECT_GT(fast_accepted, 0u);
}

// A constructed non-convergent case: a tolerance below the cold map's own noise floor
// can never be accepted, so the attempt spends its budget and falls back to Search,
// whose answer it then returns bit for bit.
TEST(AmericanIvPremium, ANonConvergentAttemptFallsBackToSearchBitForBit) {
  const double S = 100.0;
  const PremiumCase c{120.0, 1.0, 0.05, 0.0, Side::Put, 0.3};
  const double p = cold_al(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
  IvOpts o;
  o.tol = 1.0e-17;
  IvPremiumStats stats;
  const IvResult prem =
      american_implied_vol_premium(p, S, c.K, c.T, c.r, c.q, c.sd, o, nullptr, stats);
  // Never accepted: the budget runs out, or a noise-floor distance happens to shrink
  // enough to earn an extra solve whose chord to the previous iterate is then noise
  // (NotIncreasing). Either way the attempt spends at least the budget and at most the
  // budget plus the extra solves.
  EXPECT_NE(stats.fallback, IvPremiumFallback::None);
  EXPECT_TRUE(stats.fallback == IvPremiumFallback::NoConvergence ||
              stats.fallback == IvPremiumFallback::NotIncreasing)
      << static_cast<int>(stats.fallback);
  EXPECT_GE(stats.premium_solves, kIvPremiumMaxSolves);
  EXPECT_LE(stats.premium_solves, kIvPremiumMaxSolves + kIvPremiumExtraSolves);
  const IvResult search = american_implied_vol(p, S, c.K, c.T, c.r, c.q, c.sd, o);
  EXPECT_EQ(prem.status, search.status);
  EXPECT_TRUE(same_double(prem.iv, search.iv) || (std::isnan(prem.iv) && std::isnan(search.iv)));
}

// The budget extension: a cold solve beyond kIvPremiumMaxSolves is spent only while the
// Newton distance keeps shrinking by kIvPremiumShrink per solve. These SPY graded-v1
// deep-ITM puts (cohort5 slice, market prices, accurate kernel, no memory) converged
// geometrically but needed a 5th or 6th solve: without the extra solves they fall back
// after 4 (resume brief item 2: 392 of 395 budget fallbacks were still shrinking), with
// them they are accepted on the cold map within tol.
TEST(AmericanIvPremium, ExtraSolvesWhileShrinkingAcceptTheSlowDeepItmPuts) {
  const double S = 772.9949951171875;
  const double T = 1.6703925132751465;
  const double r = 0.051124785095453262;
  const double q = 0.0068890395460387381;
  struct Case {
    double K, price;
  };
  const std::array<Case, 3> cases{{{1300.0, 529.5}, {1310.0, 539.5}, {1320.0, 549.5}}};
  const double tol = IvOpts{}.tol;
  std::size_t accepted = 0;
  for (const Case &c : cases) {
    IvPremiumStats stats;
    const IvResult prem = american_implied_vol_premium(c.price, S, c.K, T, r, q, Side::Put,
                                                       IvOpts{}, nullptr, stats);
    if (stats.fallback != IvPremiumFallback::None) {
      continue;
    }
    ++accepted;
    ASSERT_EQ(prem.status, IvStatus::Ok) << c.K;
    EXPECT_GT(stats.premium_solves, kIvPremiumMaxSolves) << c.K;
    EXPECT_LE(stats.premium_solves, kIvPremiumMaxSolves + kIvPremiumExtraSolves) << c.K;
    const double h = 1.0e-6; // Newton distance on the cold map (chord over 1e-6 vol)
    const double slope = (cold_al(S, c.K, T, prem.iv + h, r, q, Side::Put) -
                          cold_al(S, c.K, T, prem.iv - h, r, q, Side::Put)) /
                         (2.0 * h);
    EXPECT_LE(std::abs(cold_al(S, c.K, T, prem.iv, r, q, Side::Put) - c.price) / slope, tol)
        << c.K;
  }
  EXPECT_EQ(accepted, cases.size());
}

// Inside the non-monotone band Premium never answers: it falls back (RangeEdge) and
// returns Search's answer bit for bit, so with refuse_non_monotone the L1c refusal
// still fires exactly where it fires on Search.
TEST(AmericanIvPremium, TheNonMonotoneBandFallsBackAndTheRefusalStillFires) {
  const double S = 100.0;
  struct Case {
    double K, T, r, q, sig_true;
  };
  // The L1c refusal cases (RefusesWrongOkAnswersInsideTheRegion), puts.
  const Case cases[] = {{kNmK, kNmT, kNmR, kNmQ, 0.14},
                        {140.0, 5.0, 0.05, 0.0, 0.2},
                        {130.0, 5.0, 0.05, 0.05, 0.1}};
  for (const Case &k : cases) {
    const PremiumCase c{k.K, k.T, k.r, k.q, Side::Put, k.sig_true};
    const double p = cold_al(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
    for (const bool refuse : {false, true}) {
      IvPremiumStats stats;
      const IvResult prem = premium_iv(p, S, c, nullptr, stats, refuse);
      const IvResult search = typed_iv(p, S, c.K, c.T, c.r, c.q, c.sd, refuse);
      EXPECT_EQ(stats.fallback, IvPremiumFallback::RangeEdge) << c.K << " " << refuse;
      EXPECT_EQ(prem.status, search.status) << c.K << " " << refuse;
      EXPECT_TRUE(same_double(prem.iv, search.iv) ||
                  (std::isnan(prem.iv) && std::isnan(search.iv)));
      if (refuse) {
        EXPECT_EQ(prem.status, IvStatus::NonMonotoneRegion) << c.K;
      }
    }
  }
}

// The cache's band edges: the mid inverted from scratch, then the bid and the ask each
// from a COPY of the mid's memory. Every edge is accepted on the cold map within tol,
// and the memory cuts the edges' cold solves (most need one).
TEST(AmericanIvPremium, TheMidMemoryCutsTheBandEdgeSolves) {
  const double S = 100.0;
  const double tol = IvOpts{}.tol;
  std::size_t edges = 0, with_memory = 0, without_memory = 0, one_solve = 0;
  for (const PremiumCase &c : premium_grid()) {
    const double mid = cold_al(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
    const double half = std::max(0.01, 0.02 * mid);
    if (!(mid - half > 0.05)) {
      continue; // two-sided quotes only
    }
    IvPremiumMemory memory;
    IvPremiumStats mid_stats;
    const IvResult m = premium_iv(mid, S, c, &memory, mid_stats);
    if (mid_stats.fallback != IvPremiumFallback::None) {
      continue;
    }
    ASSERT_EQ(m.status, IvStatus::Ok);
    ASSERT_GE(memory.points, 1u);
    for (const double edge : {mid - half, mid + half}) {
      IvPremiumMemory copy = memory;
      IvPremiumStats seeded;
      const IvResult a = premium_iv(edge, S, c, &copy, seeded);
      IvPremiumStats cold;
      const IvResult b = premium_iv(edge, S, c, nullptr, cold);
      if (seeded.fallback != IvPremiumFallback::None || cold.fallback != IvPremiumFallback::None) {
        continue;
      }
      ++edges;
      with_memory += seeded.premium_solves;
      without_memory += cold.premium_solves;
      one_solve += seeded.premium_solves == 1u ? 1u : 0u;
      EXPECT_LE(std::abs(a.iv - b.iv), 2.0 * tol) << c.K << " " << c.T;
    }
  }
  std::printf("[premium memory] %zu band edges: cold solves %zu with the mid's memory, %zu "
              "without; %zu edges in one solve\n",
              edges, with_memory, without_memory, one_solve);
  ASSERT_GT(edges, 100u);
  EXPECT_LT(with_memory * 10, without_memory * 7); // at least 30% fewer cold solves
  EXPECT_GT(one_solve * 2, edges);                  // most edges need one solve
}

// ── Seed-tier wiring (SPY1s Phase 1 / L2g, 2026-10-20) ──
// Pinned: IvOpts::seed_profile Screen runs the iterates on the MEASURED tier
// al_screen_opts() (7 nodes), confirms every accepted root on the accurate map (one
// Reference solve, Newton distance <= tol) and agrees with Search; a price inside Screen's
// wide non-monotone gate routes its iterates to the accurate kernel (the r = q dips of
// L2f section 4) exactly where a Screen-as-accurate inversion refuses; the band gate
// skips the confirm only where its statement holds and the skipped root's accurate-map
// Newton distance stays inside the bound the gate computed; Never skips every confirm;
// the Fast confirm kernel is honoured.

namespace {

using atx::vol::al_screen_opts;
using atx::vol::AlProfile;
using atx::vol::IvConfirmPolicy;
using atx::vol::IvPremiumGate;

// Accurate-map Newton distance of `sigma` for `price`: |P(sigma) - price| over the
// forward chord of the Reference map at kAlPackSigmaBump. NaN when the map is flat.
[[nodiscard]] double accurate_newton_distance(double price, double S, const PremiumCase &c,
                                              double sigma) {
  const auto p0 = andersen_lake(S, c.K, c.T, sigma, c.r, c.q, c.sd);
  const auto p1 = andersen_lake(S, c.K, c.T, sigma + atx::vol::kAlPackSigmaBump, c.r, c.q, c.sd);
  if (!p0 || !p1) return std::numeric_limits<double>::quiet_NaN();
  const double slope = (*p1 - *p0) / atx::vol::kAlPackSigmaBump;
  return slope > 0.0 ? std::abs(*p0 - price) / slope : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] IvResult screen_seeded_iv(double price, double S, const PremiumCase &c,
                                        IvPremiumStats &stats, const IvPremiumGate *gate) {
  IvOpts o;
  o.strategy = IvStrategy::Premium;
  o.seed_profile = AlProfile::Screen;
  return american_implied_vol_premium(price, S, c.K, c.T, c.r, c.q, c.sd, o, nullptr, stats, gate);
}

} // namespace

TEST(AmericanIvPremium, ScreenSeedsRunTheMeasuredTierAndAreConfirmedOnTheAccurateMap) {
  const double S = 100.0;
  const double tol = IvOpts{}.tol;
  std::size_t accepted = 0, confirmed = 0, routed = 0, fallbacks = 0, two_solves = 0;
  std::size_t edge_checked = 0, edge_over = 0, edge_over_twice = 0;
  std::array<std::size_t, atx::vol::kIvPremiumFallbackCount> why{};
  double max_newton = 0.0, max_dsig = 0.0, max_edge_rel = 0.0;
  for (const PremiumCase &c : premium_grid()) {
    const auto p = andersen_lake(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
    if (!p || !(*p > 0.0)) continue;
    // The band edges the cache prices on Screen (kIvMin / kIvMax) against the accurate
    // kernel: how far the tested bound is from what the edges see (printed; a count of
    // edges over the bound is reported, not asserted - the cache re-prices an ambiguous
    // edge accurately, and the measured ambiguous fraction is in RESULTS.md).
    for (const double edge_sigma : {atx::vol::kIvMin, atx::vol::kIvMax}) {
      const auto ea = andersen_lake(S, c.K, c.T, edge_sigma, c.r, c.q, c.sd);
      const auto es = andersen_lake(S, c.K, c.T, edge_sigma, c.r, c.q, c.sd, al_screen_opts());
      if (!ea || !es) continue;
      ++edge_checked;
      const double bound = atx::vol::al_profile_price_bound(AlProfile::Screen, *ea, c.T);
      const double err = std::abs(*es - *ea);
      max_edge_rel = std::max(max_edge_rel, err / std::max(*ea, 1.0));
      if (err > bound) ++edge_over;
      // The de-Am cache's edge margin: twice the WHOLE-GRID bound at every maturity
      // (quote_slice.cpp SliceDeAmModel::seed_price_bound).
      if (err > 2.0 * atx::vol::kAlScreenPriceBound * std::max(*ea, 1.0)) ++edge_over_twice;
    }
    IvPremiumStats st;
    const IvResult r = screen_seeded_iv(*p, S, c, st, nullptr);
    const IvResult s = american_implied_vol(*p, S, c.K, c.T, c.r, c.q, c.sd, IvOpts{});
    ASSERT_EQ(r.status == IvStatus::Ok, s.status == IvStatus::Ok) << c.K << " " << c.T;
    // The accurate-seeded Premium attempt of the same price: Screen seeds with the bounded
    // (two-solve) confirm lose none of its acceptances (a single-solve confirm turned 150
    // of them into Search fallbacks on this grid).
    IvPremiumStats accurate_seed;
    static_cast<void>(premium_iv(*p, S, c, nullptr, accurate_seed));
    if (st.fallback != IvPremiumFallback::None) {
      ++fallbacks; // a fallback IS Search, pinned by ConvergesToTheSearchRootOrFallsBackBitForBit
      ++why[static_cast<std::size_t>(st.fallback)];
      EXPECT_NE(accurate_seed.fallback, IvPremiumFallback::None) << c.K << " " << c.T << " " << c.sig_true;
      continue;
    }
    ++accepted;
    EXPECT_FALSE(st.confirm_skipped);
    if (st.seed_routed) {
      ++routed;
      EXPECT_EQ(st.seed_nodes, 12u); // the accurate preset's nodes
      EXPECT_EQ(st.confirm_solves, 0u);
    } else {
      ++confirmed;
      EXPECT_EQ(st.seed_nodes, al_screen_opts().n_collocation); // 7: the measured tier
      EXPECT_GE(st.confirm_solves, 1u);
      EXPECT_LE(st.confirm_solves, atx::vol::kIvPremiumConfirmSolves);
      two_solves += st.confirm_solves > 1u ? 1u : 0u;
      EXPECT_EQ(st.confirm_profile, AlProfile::Reference);
    }
    // Ruling (a): the criterion is the distance to the accurate root, not to Search's
    // answer (Search's polish may stop up to 4 tol from its root; L2e measured it up to
    // 1.75e-3 off on cohort5). |dsigma| vs Search is printed, not asserted.
    const double nd = accurate_newton_distance(*p, S, c, r.iv);
    if (std::isfinite(nd)) {
      max_newton = std::max(max_newton, nd);
      EXPECT_LE(nd, 4.0 * tol) << c.K << " " << c.T << " " << c.sig_true;
    }
    max_dsig = std::max(max_dsig, std::abs(r.iv - s.iv));
  }
  std::printf("[screen seeds] accepted %zu (confirmed %zu, of which %zu needed a second confirm "
              "solve; routed %zu), fallbacks %zu (1=%zu 2=%zu 3=%zu 4=%zu 5=%zu 6=%zu 7=%zu), max "
              "accurate-map Newton distance %.3g, max |dsigma| vs Search %.3g\n",
              accepted, confirmed, two_solves, routed, fallbacks, why[1], why[2], why[3], why[4],
              why[5], why[6], why[7], max_newton, max_dsig);
  std::printf("[screen edges] %zu kIvMin / kIvMax edge prices: Screen vs accurate max rel %.3g, "
              "%zu over the tested bound (short-dated bound included), %zu over twice the "
              "whole-grid bound\n",
              edge_checked, max_edge_rel, edge_over, edge_over_twice);
  EXPECT_GT(confirmed, 100u);
  EXPECT_LE(max_newton, 4.0 * tol);
  EXPECT_EQ(edge_over_twice, 0u); // the de-Am cache decides a Screen edge with that margin
}

// A price inside Screen's wide non-monotone gate (4e-4 K max(1, T)^2; the accurate gate is
// 5e-6) at r = q: an inversion on Screen AS the kernel refuses it (NonMonotoneRegion under
// refuse_non_monotone), while the Screen-seeded Premium inversion routes its iterates to
// the accurate kernel and serves Search's root. Screen is never classified accurate.
TEST(AmericanIvPremium, APriceInsideScreensNonMonotoneGateRunsAccurateIteratesNotARefusal) {
  const double S = 100.0;
  std::size_t routed = 0, screen_refusals = 0, candidates = 0;
  std::array<std::size_t, 6> screen_status{};
  double max_screen_newton = 0.0; // accurate-map Newton distance of a Screen-as-kernel root
  for (const double rq : {0.02, 0.03, 0.05}) {
    for (const double T : {0.5, 1.0, 2.0}) {
      for (const double m : {1.01, 1.02, 1.05, 1.1}) {
        for (const Side sd : {Side::Put, Side::Call}) {
          const double K = sd == Side::Put ? S * m : S / m; // in the money
          const PremiumCase c{K, T, rq, rq, sd, 0.0};
          const auto floor = andersen_lake(S, K, T, atx::vol::kIvMin, rq, rq, sd);
          if (!floor) continue;
          const double wide = al_non_monotone_gate(K, T, al_screen_opts());
          const double narrow = al_non_monotone_gate(K, T, std::nullopt);
          ASSERT_GT(wide, 10.0 * narrow); // Screen is NOT the accurate scheme
          for (const double frac : {0.5, 0.05}) {
            const double price = *floor + frac * wide;
            const IvResult s = american_implied_vol(price, S, K, T, rq, rq, sd, IvOpts{});
            if (s.status != IvStatus::Ok) continue;
            ++candidates;
            IvPremiumStats st;
            const IvResult r = screen_seeded_iv(price, S, c, st, nullptr);
            ASSERT_EQ(r.status, IvStatus::Ok) << K << " " << T;
            if (st.fallback == IvPremiumFallback::None) {
              EXPECT_TRUE(st.seed_routed) << K << " " << T;
              EXPECT_EQ(st.seed_nodes, 12u);
              EXPECT_EQ(st.confirm_solves, 0u);
              routed += st.seed_routed ? 1u : 0u;
            }
            // Near the intrinsic the map is flat and curved (L1c): the forward-chord
            // Newton distance is not a sharp yardstick there, so the routed root is
            // judged by its accurate-map price residual against Search's own.
            const auto pr = andersen_lake(S, K, T, r.iv, rq, rq, sd);
            const auto ps = andersen_lake(S, K, T, s.iv, rq, rq, sd);
            ASSERT_TRUE(pr && ps);
            EXPECT_LE(std::abs(*pr - price), 2.0 * std::abs(*ps - price) + 1.0e-9 * std::max(price, 1.0))
                << K << " " << T << " " << frac;
            // Screen as the (mis-)classified accurate kernel inside its own band: a
            // NonMonotoneRegion refusal, an out-of-range / unpriceable status, or the
            // kIvMin floor clamp (its kIvMin price above the target) would each be "no
            // root"; what it does return on this grid is counted and its accurate-map
            // distance printed (RESULTS.md / note section 2 read the outcome).
            IvOpts as_kernel;
            as_kernel.al_opts = al_screen_opts();
            as_kernel.refuse_non_monotone = true;
            const IvResult refused = american_implied_vol(price, S, K, T, rq, rq, sd, as_kernel);
            ++screen_status[static_cast<std::size_t>(refused.status)];
            const bool clamped = refused.status == IvStatus::Ok && !(refused.iv > atx::vol::kIvMin);
            screen_refusals += (refused.status != IvStatus::Ok || clamped) ? 1u : 0u;
            if (refused.status == IvStatus::Ok && !clamped) {
              const double nd_screen = accurate_newton_distance(price, S, c, refused.iv);
              if (std::isfinite(nd_screen)) max_screen_newton = std::max(max_screen_newton, nd_screen);
            }
          }
        }
      }
    }
  }
  std::printf("[screen routing] r = q candidates %zu, routed to accurate iterates %zu, not served "
              "by Screen as kernel %zu (status counts ok %zu invalid %zu out_of_range %zu "
              "no_convergence %zu unpriceable %zu non_monotone %zu; max accurate-map Newton "
              "distance of a Screen-as-kernel root %.3g)\n",
              candidates, routed, screen_refusals, screen_status[0], screen_status[1],
              screen_status[2], screen_status[3], screen_status[4], screen_status[5],
              max_screen_newton);
  EXPECT_GT(candidates, 0u);
  EXPECT_GT(routed, 0u);
  // (screen_refusals is reported, not asserted: on this r = q grid Screen as the kernel
  // returned Ok roots - the routing guard is in front regardless, pinned above.)
}

// Opt-in seed-tier routing gate (SPY1s Phase 2, IvPremiumGate::seed_gate_kappa): with kappa
// = 4.3e-5 (10x the measured Screen dip) a price whose excess over the zero-vol value lies
// between kappa K max(1, T^2) and Screen's own wide gate 4e-4 K max(1, T^2) iterates on
// Screen and is CONFIRMED on the accurate kernel instead of being routed to accurate
// iterates; inside the narrowed gate it is still routed; kappa 0 is the tier's own gate.
// Every accepted root stays at the accurate root (the r = q grid of the routing test, and
// the Premium grid under the key: Newton distance <= 4 tol, Search's acceptances kept).
TEST(AmericanIvPremium, SeedGateKappaNarrowsTheRoutingBandAndConfirmsScreenIteratedRoots) {
  using atx::vol::iv_premium_seed_gate;
  const double S = 100.0;
  const double tol = IvOpts{}.tol;
  constexpr double kKappa = 4.3e-5;
  // The gate helper: the tier's own gate without a kappa, the kappa band with one.
  IvPremiumGate off;
  IvPremiumGate keyed;
  keyed.seed_gate_kappa = kKappa;
  EXPECT_EQ(iv_premium_seed_gate(100.0, 2.0, al_screen_opts(), nullptr),
            al_non_monotone_gate(100.0, 2.0, al_screen_opts()));
  EXPECT_EQ(iv_premium_seed_gate(100.0, 2.0, al_screen_opts(), &off),
            al_non_monotone_gate(100.0, 2.0, al_screen_opts()));
  EXPECT_DOUBLE_EQ(iv_premium_seed_gate(100.0, 2.0, al_screen_opts(), &keyed), kKappa * 100.0 * 4.0);
  EXPECT_DOUBLE_EQ(iv_premium_seed_gate(100.0, 0.5, al_screen_opts(), &keyed), kKappa * 100.0);
  EXPECT_TRUE(std::isnan(iv_premium_seed_gate(0.0, 1.0, al_screen_opts(), &keyed)));
  EXPECT_TRUE(std::isnan(iv_premium_seed_gate(100.0, 0.0, al_screen_opts(), &keyed)));
  std::size_t candidates = 0, unrouted = 0, still_routed = 0, routed_off = 0;
  double max_unrouted_newton = 0.0;
  for (const double rq : {0.02, 0.03, 0.05}) {
    for (const double T : {0.5, 1.0, 2.0}) {
      for (const double m : {1.01, 1.02, 1.05, 1.1}) {
        for (const Side sd : {Side::Put, Side::Call}) {
          const double K = sd == Side::Put ? S * m : S / m; // in the money
          const PremiumCase c{K, T, rq, rq, sd, 0.0};
          const auto floor = andersen_lake(S, K, T, atx::vol::kIvMin, rq, rq, sd);
          if (!floor) continue;
          const double wide = al_non_monotone_gate(K, T, al_screen_opts());
          const double narrow = kKappa * K * std::fmax(1.0, T * T);
          for (const double frac : {0.5, 0.05}) {
            const double price = *floor + frac * wide;
            const IvResult s = american_implied_vol(price, S, K, T, rq, rq, sd, IvOpts{});
            if (s.status != IvStatus::Ok) continue;
            // Classified away from both gate edges (the library's zero-vol value is the
            // test's own replica to a few ULP).
            const double excess = price - zero_vol_value(S, K, T, rq, rq, sd);
            ++candidates;
            IvPremiumStats st_off, st_key;
            const IvResult r_off = screen_seeded_iv(price, S, c, st_off, &off);
            const IvResult r_key = screen_seeded_iv(price, S, c, st_key, &keyed);
            ASSERT_EQ(r_off.status, IvStatus::Ok) << K << " " << T;
            ASSERT_EQ(r_key.status, IvStatus::Ok) << K << " " << T;
            if (st_off.fallback == IvPremiumFallback::None && excess < 0.95 * wide) {
              EXPECT_TRUE(st_off.seed_routed) << K << " " << T << " " << frac;
              routed_off += st_off.seed_routed ? 1u : 0u;
            }
            if (st_key.fallback != IvPremiumFallback::None) continue; // = Search, bit for bit
            if (excess > 1.05 * narrow) {
              EXPECT_FALSE(st_key.seed_routed) << K << " " << T << " " << frac;
              EXPECT_EQ(st_key.seed_nodes, al_screen_opts().n_collocation);
              EXPECT_GE(st_key.confirm_solves, 1u);
              EXPECT_FALSE(st_key.confirm_skipped);
              unrouted += st_key.seed_routed ? 0u : 1u;
              // Near the intrinsic the forward-chord Newton distance is not a sharp yardstick
              // (L1c): the confirmed root's accurate-map price residual against Search's own.
              const auto pr = andersen_lake(S, K, T, r_key.iv, rq, rq, sd);
              const auto ps = andersen_lake(S, K, T, s.iv, rq, rq, sd);
              ASSERT_TRUE(pr && ps);
              EXPECT_LE(std::abs(*pr - price),
                        2.0 * std::abs(*ps - price) + 1.0e-9 * std::max(price, 1.0))
                  << K << " " << T << " " << frac;
              const double nd = accurate_newton_distance(price, S, c, r_key.iv);
              if (std::isfinite(nd)) max_unrouted_newton = std::max(max_unrouted_newton, nd);
            } else if (excess < 0.95 * narrow) {
              EXPECT_TRUE(st_key.seed_routed) << K << " " << T << " " << frac;
              still_routed += st_key.seed_routed ? 1u : 0u;
            }
          }
        }
      }
    }
  }
  // The general Premium grid under the key: the accurate root, Search's acceptances.
  std::size_t accepted = 0, grid_routed = 0;
  double max_newton = 0.0;
  for (const PremiumCase &c : premium_grid()) {
    const auto p = andersen_lake(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
    if (!p || !(*p > 0.0)) continue;
    IvPremiumStats st;
    const IvResult r = screen_seeded_iv(*p, S, c, st, &keyed);
    const IvResult s = american_implied_vol(*p, S, c.K, c.T, c.r, c.q, c.sd, IvOpts{});
    ASSERT_EQ(r.status == IvStatus::Ok, s.status == IvStatus::Ok) << c.K << " " << c.T;
    if (st.fallback != IvPremiumFallback::None) continue;
    ++accepted;
    grid_routed += st.seed_routed ? 1u : 0u;
    const double nd = accurate_newton_distance(*p, S, c, r.iv);
    if (std::isfinite(nd)) {
      max_newton = std::max(max_newton, nd);
      EXPECT_LE(nd, 4.0 * tol) << c.K << " " << c.T << " " << c.sig_true;
    }
  }
  std::printf("[seed gate kappa %.2g] r = q candidates %zu: routed without the key %zu; with it "
              "iterated on Screen and confirmed %zu (max accurate-map Newton %.3g), still routed "
              "%zu; Premium grid accepted %zu (routed %zu), max accurate-map Newton %.3g\n",
              kKappa, candidates, routed_off, unrouted, max_unrouted_newton, still_routed,
              accepted, grid_routed, max_newton);
  EXPECT_GT(candidates, 0u);
  EXPECT_GT(routed_off, 0u);
  EXPECT_GT(unrouted, 0u); // the key moves prices out of the routing band (fails on revert)
  EXPECT_GT(accepted, 100u);
  EXPECT_LE(max_newton, 4.0 * tol);
}

// Kernel-free family iterates (SPY1s Phase 2, IvPremiumGate::family): the Screen-seeded
// Premium iterates price the D-model A(sigma; own q) + P_family(sigma; q_family) -
// A(sigma; q_family) on an AlSigmaFamily built at a REPRESENTATIVE yield (the strike's own
// q differs by up to 5e-4), every accepted root is confirmed on the accurate map at the
// strike's own q (Newton distance <= 4 tol); a family that refuses (a box the iterates
// leave) sends the attempt to the Screen iterates with the same accuracy; under the band
// policy the family is not used (its bound is not the Screen tier's).
TEST(AmericanIvPremium, FamilyIteratesAreConfirmedAtTheStrikesOwnYield) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::AlSigmaFamily;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double q_family = 0.01;
  const double tol = IvOpts{}.tol;
  std::size_t accepted = 0, family_priced = 0, narrow_refused = 0, narrow_accepted = 0;
  std::size_t band_family = 0;
  double max_newton = 0.0, max_narrow_newton = 0.0;
  for (const double T : {0.25, 1.0}) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, q_family);
    for (const Side sd : {Side::Put, Side::Call}) {
      AlSigmaFamily wide(g, sd, S);
      ASSERT_TRUE(wide.prepare(0.05, 1.2, 12));
      AlSigmaFamily narrow(g, sd, S);
      ASSERT_TRUE(narrow.prepare(0.9, 1.0, 12)); // no root of the grid below lies inside
      for (const double dq : {0.0, 2.0e-4, 5.0e-4}) {
        const double q = q_family + dq;
        for (const double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
          for (const double sig : {0.15, 0.3, 0.6}) {
            const auto p = andersen_lake(S, K, T, sig, r, q, sd);
            if (!p || !(*p > 0.0)) continue;
            const PremiumCase c{K, T, r, q, sd, sig};
            IvPremiumGate gate;
            gate.family = &wide;
            gate.family_q = q_family;
            IvPremiumStats st;
            const IvResult res = screen_seeded_iv(*p, S, c, st, &gate);
            ASSERT_EQ(res.status, IvStatus::Ok) << K << " " << T << " " << sig;
            if (st.fallback == IvPremiumFallback::None && !st.seed_routed) {
              ++accepted;
              family_priced += st.family_prices > 0u ? 1u : 0u;
              EXPECT_GE(st.confirm_solves, 1u);
              const double nd = accurate_newton_distance(*p, S, c, res.iv);
              if (std::isfinite(nd)) {
                max_newton = std::max(max_newton, nd);
                EXPECT_LE(nd, 4.0 * tol) << K << " " << T << " " << sig << " " << dq;
              }
            }
            IvPremiumGate refusing = gate;
            refusing.family = &narrow;
            IvPremiumStats ns;
            const IvResult nr = screen_seeded_iv(*p, S, c, ns, &refusing);
            ASSERT_EQ(nr.status, IvStatus::Ok);
            narrow_refused += ns.family_refused ? 1u : 0u;
            if (ns.fallback == IvPremiumFallback::None && !ns.seed_routed) {
              ++narrow_accepted;
              const double nd = accurate_newton_distance(*p, S, c, nr.iv);
              if (std::isfinite(nd)) {
                max_narrow_newton = std::max(max_narrow_newton, nd);
                EXPECT_LE(nd, 4.0 * tol) << K << " " << T << " " << sig << " " << dq;
              }
            }
            IvPremiumGate band = gate;
            band.policy = IvConfirmPolicy::Band;
            band.half_spread_usd = 0.05;
            IvPremiumStats bs;
            static_cast<void>(screen_seeded_iv(*p, S, c, bs, &band));
            band_family += bs.family_prices;
          }
        }
      }
    }
  }
  std::printf("[family iterates] accepted %zu, of which family-priced %zu, max accurate-map "
              "Newton %.3g; refusing family: refused %zu, accepted %zu, max Newton %.3g; band "
              "policy family prices %zu\n",
              accepted, family_priced, max_newton, narrow_refused, narrow_accepted,
              max_narrow_newton, band_family);
  EXPECT_GT(accepted, 50u);
  EXPECT_GT(family_priced, accepted / 2); // the family carries the iterates (fails on revert)
  EXPECT_LE(max_newton, 4.0 * tol);
  EXPECT_GT(narrow_refused, 0u);
  EXPECT_GT(narrow_accepted, 0u);
  EXPECT_EQ(band_family, 0u);
}

// (sigma, q) family iterates (SPY1s Phase 2 step 5, IvPremiumGate::qfamily; L2h-b
// AlSigmaQFamily): the iterates price each strike at its OWN q on the family (strike yields
// spread over the q box), every accepted root under Always is confirmed at the accurate root
// (Newton distance <= 4 tol); under Band (the B-family arm) a confirm is skipped only on the
// lane-K3 enclosure - two family prices and their validated envelope bracketing the accurate
// root in at most 1e-7 vol, no Andersen-Lake gate solve - so with the family's envelope in
// scope it skips and every skipped root sits inside its bracket (gate_bound_vol, <= 1e-7) of
// the accurate root, and without one it never skips (the band criterion is not read on the
// family route); a yield outside the q box refuses and the attempt iterates on Screen; Never
// never uses the family.
TEST(AmericanIvPremium, SigmaQFamilyIteratesPriceTheStrikesOwnYield) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::AlSigmaQFamily;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double q_lo = 0.010;
  const double q_hi = 0.012;
  const double tol = IvOpts{}.tol;
  std::size_t accepted = 0, family_priced = 0, band_skipped = 0, band_gate_solves = 0;
  std::size_t outside_refused = 0, never_family = 0, unscoped_skipped = 0;
  double max_newton = 0.0, max_skipped_excess = 0.0, max_skipped_newton = 0.0;
  for (const double T : {0.25, 1.0}) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, 0.011);
    for (const Side sd : {Side::Put, Side::Call}) {
      AlSigmaQFamily fam(g, sd, S);
      ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, q_lo, q_hi, 4));
      const atx::vol::ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
      ASSERT_TRUE(env.covered());
      for (const double q : {0.0100, 0.0107, 0.0120, 0.0130}) {
        for (const double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
          for (const double sig : {0.15, 0.3, 0.6}) {
            const auto p = andersen_lake(S, K, T, sig, r, q, sd);
            if (!p || !(*p > 0.0)) continue;
            const PremiumCase c{K, T, r, q, sd, sig};
            IvPremiumGate gate;
            gate.qfamily = &fam;
            gate.qfamily_bound = 1.0e-9;
            IvPremiumStats st;
            const IvResult res = screen_seeded_iv(*p, S, c, st, &gate);
            ASSERT_EQ(res.status, IvStatus::Ok) << K << " " << T << " " << sig;
            const bool in_box = q <= q_hi;
            if (!in_box) outside_refused += st.family_refused ? 1u : 0u;
            if (st.fallback == IvPremiumFallback::None && !st.seed_routed) {
              ++accepted;
              family_priced += in_box && st.family_prices > 0u ? 1u : 0u;
              const double nd = accurate_newton_distance(*p, S, c, res.iv);
              if (std::isfinite(nd)) {
                max_newton = std::max(max_newton, nd);
                EXPECT_LE(nd, 4.0 * tol) << K << " " << T << " " << sig << " " << q;
              }
            }
            IvPremiumGate band = gate;
            band.policy = IvConfirmPolicy::Band;
            band.half_spread_usd = 0.05;
            IvPremiumStats us;
            ASSERT_EQ(screen_seeded_iv(*p, S, c, us, &band).status, IvStatus::Ok);
            unscoped_skipped += us.confirm_skipped ? 1u : 0u;
            IvPremiumStats bs;
            IvResult br{};
            {
              const atx::vol::FamilyEnvelopeScope scope(&env);
              br = screen_seeded_iv(*p, S, c, bs, &band);
            }
            ASSERT_EQ(br.status, IvStatus::Ok);
            if (in_box && bs.confirm_skipped) {
              ++band_skipped;
              band_gate_solves += bs.gate_solves;
              EXPECT_EQ(bs.acceptance.kind, atx::vol::RootAcceptanceKind::Skipped);
              EXPECT_LE(bs.gate_bound_vol, atx::vol::kIvRootRequirement);
              const double nd = accurate_newton_distance(*p, S, c, br.iv);
              if (std::isfinite(nd) && std::isfinite(bs.gate_bound_vol)) {
                // The bracket bounds the skipped root's distance to the accurate root (the
                // Newton distance only approximates that distance, to its second order).
                max_skipped_excess = std::max(max_skipped_excess, nd - bs.gate_bound_vol);
                max_skipped_newton = std::max(max_skipped_newton, nd);
              }
            }
            IvPremiumGate never = gate;
            never.policy = IvConfirmPolicy::Never;
            IvPremiumStats ns;
            static_cast<void>(screen_seeded_iv(*p, S, c, ns, &never));
            never_family += ns.family_prices;
          }
        }
      }
    }
  }
  std::printf("[sigma-q iterates] accepted %zu, of which family-priced %zu, max accurate-map "
              "Newton %.3g; band: skipped %zu with %zu AL gate solves (no envelope: %zu), max "
              "skipped Newton distance %.3g, over its bracket %.3g; outside the q box refused "
              "%zu; never-policy family prices %zu\n",
              accepted, family_priced, max_newton, band_skipped, band_gate_solves,
              unscoped_skipped, max_skipped_newton, max_skipped_excess, outside_refused,
              never_family);
  EXPECT_GT(accepted, 50u);
  EXPECT_GT(family_priced, accepted / 2); // fails with the family ignored by the iterates
  EXPECT_LE(max_newton, 4.0 * tol);
  EXPECT_GT(band_skipped, 20u);
  EXPECT_EQ(band_gate_solves, 0u); // the enclosure prices on the family only
  EXPECT_EQ(unscoped_skipped, 0u); // no envelope: no skip (no band criterion on the family)
  EXPECT_LE(max_skipped_excess, 1.0e-10);
  EXPECT_LE(max_skipped_newton, atx::vol::kIvRootRequirement);
  EXPECT_GT(outside_refused, 0u);
  EXPECT_EQ(never_family, 0u);
}

// Family-native iterates (SPY1s Phase 3 lever 1, IvPremiumGate::qfamily_native): the
// (sigma, q) family's own map is iterated without the seed leg. On the grid above every
// accepted root is confirmed at the accurate root (Newton distance <= 4 tol) with no
// Andersen-Lake iterate solve, a band's bid / ask inversions reuse the mid's family-native
// memory, and a deep in-the-money put whose seed leg refuses (SPY 0.21 y K 823, the
// NoEuropeanSeed fallback class of the Phase 2 profile) is accepted on the family instead of
// falling back to Search.
TEST(AmericanIvPremium, FamilyNativeIteratesNeedNoSeedLeg) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::AlSigmaQFamily;
  using atx::vol::IvPremiumMemory;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double tol = IvOpts{}.tol;
  std::size_t accepted = 0, without_al_iterates = 0, native_memory = 0;
  double max_newton = 0.0;
  for (const double T : {0.25, 1.0}) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, 0.011);
    for (const Side sd : {Side::Put, Side::Call}) {
      AlSigmaQFamily fam(g, sd, S);
      ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, 0.010, 0.012, 4));
      for (const double q : {0.0100, 0.0107, 0.0120}) {
        for (const double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
          for (const double sig : {0.15, 0.3, 0.6}) {
            const auto p = andersen_lake(S, K, T, sig, r, q, sd);
            if (!p || !(*p > 0.0)) continue;
            const PremiumCase c{K, T, r, q, sd, sig};
            IvPremiumGate gate;
            gate.qfamily = &fam;
            gate.qfamily_bound = 1.0e-9;
            gate.qfamily_native = true;
            IvOpts o;
            o.strategy = IvStrategy::Premium;
            o.seed_profile = AlProfile::Screen;
            // A band: the mid from scratch, then a bid below it from a copy of its memory.
            IvPremiumMemory mid_memory;
            IvPremiumStats st;
            const IvResult res =
                american_implied_vol_premium(*p, S, K, T, r, q, sd, o, &mid_memory, st, &gate);
            ASSERT_EQ(res.status, IvStatus::Ok) << K << " " << T << " " << sig;
            if (st.fallback != IvPremiumFallback::None || st.seed_routed) continue;
            ++accepted;
            without_al_iterates += st.premium_solves == st.confirm_solves ? 1u : 0u;
            const double nd = accurate_newton_distance(*p, S, c, res.iv);
            if (std::isfinite(nd)) max_newton = std::max(max_newton, nd);
            EXPECT_LE(nd, 4.0 * tol) << K << " " << T << " " << sig << " " << q;
            IvPremiumMemory bid_memory = mid_memory;
            native_memory += bid_memory.native ? 1u : 0u;
            const double bid = 0.97 * *p;
            IvPremiumStats bs;
            const IvResult br =
                american_implied_vol_premium(bid, S, K, T, r, q, sd, o, &bid_memory, bs, &gate);
            if (br.status != IvStatus::Ok || bs.fallback != IvPremiumFallback::None) continue;
            const double bnd = accurate_newton_distance(bid, S, c, br.iv);
            if (std::isfinite(bnd)) max_newton = std::max(max_newton, bnd);
            EXPECT_LE(bnd, 4.0 * tol) << "bid " << K << " " << T << " " << sig << " " << q;
          }
        }
      }
    }
  }
  // The deep in-the-money put of the Phase 2 fallback dump (FB reason=3).
  const double S2 = 772.99, K2 = 823.0, T2 = 0.213892, r2 = 0.045908, q2 = 0.013688;
  const AlExpiryGeometry g2 = prepare_al_geometry(T2, r2, q2);
  AlSigmaQFamily put(g2, Side::Put, S2);
  ASSERT_TRUE(put.prepare(0.05, 1.2, 16, 0.0136, 0.0138, 4));
  const auto p2 = andersen_lake(S2, K2, T2, 0.1104, r2, q2, Side::Put);
  ASSERT_TRUE(p2);
  IvOpts o2;
  o2.strategy = IvStrategy::Premium;
  o2.seed_profile = AlProfile::Screen;
  IvPremiumGate native_gate;
  native_gate.qfamily = &put;
  native_gate.qfamily_bound = 1.0e-9;
  native_gate.qfamily_native = true;
  native_gate.seed_gate_kappa = 4.3e-5; // the SPY arms' routing gate: not routed
  IvPremiumGate leg_gate = native_gate;
  leg_gate.qfamily_native = false;
  IvPremiumStats ns, ls;
  const IvResult nr =
      american_implied_vol_premium(*p2, S2, K2, T2, r2, q2, Side::Put, o2, nullptr, ns, &native_gate);
  const IvResult lr =
      american_implied_vol_premium(*p2, S2, K2, T2, r2, q2, Side::Put, o2, nullptr, ls, &leg_gate);
  std::printf("[family-native] accepted %zu (no AL iterate %zu), bid memories native %zu, max "
              "accurate-map Newton %.3g; deep ITM put: native fallback %u iv %.10g, leg fallback "
              "%u iv %.10g\n",
              accepted, without_al_iterates, native_memory, max_newton,
              static_cast<unsigned>(ns.fallback), nr.iv, static_cast<unsigned>(ls.fallback), lr.iv);
  EXPECT_GT(accepted, 50u);
  EXPECT_EQ(without_al_iterates, accepted); // no Andersen-Lake iterate on the family path
  EXPECT_EQ(native_memory, accepted);       // fails with the native path off (leg memory)
  EXPECT_LE(max_newton, 4.0 * tol);
  ASSERT_EQ(nr.status, IvStatus::Ok);
  EXPECT_EQ(ns.fallback, IvPremiumFallback::None); // fails with the native path off
  const PremiumCase deep{K2, T2, r2, q2, Side::Put, 0.1104};
  EXPECT_LE(accurate_newton_distance(*p2, S2, deep, nr.iv), 4.0 * tol);
}

// K2 (r5, 2026-10-22): the family-native attempt through the batch seam has a TYPED outcome
// (IvPremiumStats::native_status, review-R1 MJ2) and keeps what it learned: a price whose
// root lies above the family box is SigmaAboveBox with the box top's evaluated price kept in
// the memory's carry, so the quote's ask above it is settled with NO family evaluation; a
// deep in-the-money put whose Black-76 IV lies above the box (mn5: the old box-top seed could
// never form a chord) is priced AT the box top and converges natively; a bid behind its mid
// starts from the mid's carry (bracket side + Newton seed off the fused vega) in fewer
// evaluations. Every native root is confirmed at the accurate root within tol (not 4 tol:
// mn8), with the fused vega as the confirm's local slope (MJ1's required slope).
TEST(AmericanIvPremium, FamilyNativeOutcomesAreTypedAndTheCarryKeepsTheBracket) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::AlSigmaQFamily;
  using atx::vol::IvPremiumMemory;
  using atx::vol::prepare_al_geometry;
  namespace ib = atx::vol::iv_batch;
  const double S = 100.0;
  const double T = 0.5;
  const double r = 0.04;
  const double q = 0.011;
  const double tol = IvOpts{}.tol;
  const AlExpiryGeometry g = prepare_al_geometry(T, r, q);
  AlSigmaQFamily put(g, Side::Put, S);
  ASSERT_TRUE(put.prepare(0.1, 0.35, 16, 0.010, 0.012, 4));
  IvPremiumGate gate;
  gate.qfamily = &put;
  gate.qfamily_bound = 1.0e-9;
  gate.qfamily_native = true;
  gate.seed_gate_kappa = 4.3e-5; // the SPY arms' routing gate
  IvOpts o;
  o.strategy = IvStrategy::Premium;
  o.seed_profile = AlProfile::Screen;
  const auto invert = [&](double price, double K, IvPremiumMemory *m, IvPremiumStats &st) {
    return american_implied_vol_premium(price, S, K, T, r, q, Side::Put, o, m, st, &gate);
  };
  // (1) Above the box: typed, served by the seed-leg path, the box top kept in the carry.
  const auto above = andersen_lake(S, 100.0, T, 0.8, r, q, Side::Put);
  ASSERT_TRUE(above);
  IvPremiumMemory mid_memory;
  IvPremiumStats sa;
  const IvResult ra = invert(*above, 100.0, &mid_memory, sa);
  ASSERT_EQ(ra.status, IvStatus::Ok);
  EXPECT_EQ(sa.native_status, ib::InverseStatus::SigmaAboveBox);
  EXPECT_NEAR(ra.iv, 0.8, 1.0e-6);
  ASSERT_TRUE(mid_memory.carry.valid);
  EXPECT_EQ(mid_memory.carry.lo, put.sigma_hi());
  EXPECT_LT(mid_memory.carry.p_lo, *above);
  IvPremiumMemory ask_memory = mid_memory;
  IvPremiumStats sk;
  const IvResult rk = invert(1.02 * *above, 100.0, &ask_memory, sk);
  ASSERT_EQ(rk.status, IvStatus::Ok);
  EXPECT_TRUE(sk.native_carried);
  EXPECT_EQ(sk.native_status, ib::InverseStatus::SigmaAboveBox);
  EXPECT_EQ(sk.native_evaluations, 0u); // settled by the mid's evaluated box top
  // (2) mn5: the Black-76 IV above the box top, the American root inside it (the first
  // in-the-money put of a small grid whose seed lies above the box, off the plateau).
  double K2 = 0.0;
  double s2 = 0.0;
  double deep = 0.0;
  for (const double K : {105.0, 110.0, 115.0, 120.0, 125.0, 130.0}) {
    for (const double sig : {0.25, 0.3, 0.33}) {
      const auto p = andersen_lake(S, K, T, sig, r, q, Side::Put);
      if (!p || !(*p > (K - S) + 1.0e-6 * K) || K2 > 0.0) continue;
      const auto b76 = atx::vol::implied_vol(*p, S * std::exp((r - q) * T), K, T,
                                             std::exp(-r * T), Side::Put);
      if (!b76 || *b76 > put.sigma_hi()) {
        K2 = K;
        s2 = sig;
        deep = *p;
      }
    }
  }
  ASSERT_GT(K2, 0.0) << "fixture: no in-the-money put with its seed above the box";
  IvPremiumStats sd;
  const IvResult rd = invert(deep, K2, nullptr, sd);
  ASSERT_EQ(rd.status, IvStatus::Ok);
  EXPECT_EQ(sd.native_status, ib::InverseStatus::Converged);
  EXPECT_EQ(sd.fallback, IvPremiumFallback::None);
  EXPECT_NE(sd.native_flags & ib::lane_flag::kBoxEdgeEvaluated, 0u);
  const PremiumCase cd{K2, T, r, q, Side::Put, s2};
  EXPECT_LE(accurate_newton_distance(deep, S, cd, rd.iv), tol);
  // (3) A band: the mid from scratch, the bid behind it from the mid's carry.
  std::size_t bands = 0;
  std::uint32_t mid_evals = 0;
  std::uint32_t bid_evals = 0;
  double max_newton = 0.0;
  for (const double K : {85.0, 95.0, 100.0, 105.0, 115.0}) {
    for (const double sig : {0.18, 0.24, 0.3}) {
      const auto p = andersen_lake(S, K, T, sig, r, q, Side::Put);
      ASSERT_TRUE(p);
      IvPremiumMemory m;
      IvPremiumStats sm;
      const IvResult rm = invert(*p, K, &m, sm);
      if (rm.status != IvStatus::Ok || sm.native_status != ib::InverseStatus::Converged) continue;
      IvPremiumMemory bm = m;
      IvPremiumStats sb;
      const double bid = 0.99 * *p;
      const IvResult rb = invert(bid, K, &bm, sb);
      // A bid at the identifiable range's edge never reaches the native attempt.
      if (rb.status != IvStatus::Ok || sb.native_status == ib::InverseStatus::Inactive) continue;
      ASSERT_EQ(sb.native_status, ib::InverseStatus::Converged) << K << " " << sig;
      EXPECT_TRUE(sb.native_carried);
      ++bands;
      mid_evals += sm.native_evaluations;
      bid_evals += sb.native_evaluations;
      const PremiumCase c{K, T, r, q, Side::Put, sig};
      max_newton = std::max(max_newton, accurate_newton_distance(*p, S, c, rm.iv));
      max_newton = std::max(max_newton, accurate_newton_distance(bid, S, c, rb.iv));
    }
  }
  std::printf("[family-native K2] bands %zu, evaluations mid %u bid %u, max accurate-map Newton "
              "%.3g; above-box ask evaluations %u; deep ITM put K %g sigma %g flags 0x%02x\n",
              bands, mid_evals, bid_evals, max_newton,
              static_cast<unsigned>(sk.native_evaluations), K2, s2,
              static_cast<unsigned>(sd.native_flags));
  EXPECT_GE(bands, 12u);
  EXPECT_LT(bid_evals, mid_evals);
  EXPECT_LE(max_newton, tol);
}

// K2 fix round 1 (review-K2 m1): a native lane whose root is enclosed by the carried 2 tol
// bracket but has no usable slope (none evaluated: the carry settles it at the start) is
// typed ConvergedNoSlope — not Converged — and is not accepted: the seed-leg path serves
// the price within tol on the accurate map. The same price without the carry converges
// natively (Converged, with its fused vega).
TEST(AmericanIvPremium, ANativeRootWithoutASlopeIsTypedAndServedByTheSeedLeg) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::AlSigmaQFamily;
  using atx::vol::IvPremiumMemory;
  using atx::vol::prepare_al_geometry;
  namespace ib = atx::vol::iv_batch;
  namespace sq = atx::vol::sigma_q_family_detail;
  const double S = 100.0;
  const double T = 0.5;
  const double r = 0.04;
  const double q = 0.011;
  const double K = 100.0;
  const double sig = 0.24;
  const double tol = IvOpts{}.tol;
  const AlExpiryGeometry g = prepare_al_geometry(T, r, q);
  AlSigmaQFamily put(g, Side::Put, S);
  ASSERT_TRUE(put.prepare(0.1, 0.35, 16, 0.010, 0.012, 4));
  IvPremiumGate gate;
  gate.qfamily = &put;
  gate.qfamily_bound = 1.0e-9;
  gate.qfamily_native = true;
  gate.seed_gate_kappa = 4.3e-5;
  IvOpts o;
  o.strategy = IvStrategy::Premium;
  o.seed_profile = AlProfile::Screen;
  const auto p = andersen_lake(S, K, T, sig, r, q, Side::Put);
  ASSERT_TRUE(p);
  // The family map's own root of the price (bisection to adjacent doubles).
  double lo = put.sigma_lo();
  double hi = put.sigma_hi();
  for (;;) {
    const double m = 0.5 * (lo + hi);
    if (!(m > lo && m < hi)) break;
    const sq::FusedPriceVega pv = sq::price_and_vega(put, S, K, m, q);
    ASSERT_EQ(pv.status, sq::FusedStatus::Ok);
    (pv.price < *p ? lo : hi) = m;
  }
  IvPremiumMemory memory;
  memory.carry.valid = true;
  memory.carry.lo = lo - 0.4 * tol;
  memory.carry.hi = hi + 0.4 * tol;
  memory.carry.p_lo = sq::price_and_vega(put, S, K, memory.carry.lo, q).price;
  memory.carry.p_hi = sq::price_and_vega(put, S, K, memory.carry.hi, q).price;
  ASSERT_LT(memory.carry.p_lo, *p);
  ASSERT_GE(memory.carry.p_hi, *p);
  IvPremiumStats st;
  const IvResult rr =
      american_implied_vol_premium(*p, S, K, T, r, q, Side::Put, o, &memory, st, &gate);
  EXPECT_EQ(st.native_status, ib::InverseStatus::ConvergedNoSlope)
      << ib::inverse_status_name(st.native_status);
  EXPECT_EQ(st.native_evaluations, 0u);
  EXPECT_TRUE(st.native_carried);
  EXPECT_TRUE(st.family_refused); // not accepted: the seed-leg path ran
  ASSERT_EQ(rr.status, IvStatus::Ok);
  const PremiumCase c{K, T, r, q, Side::Put, sig};
  EXPECT_LE(accurate_newton_distance(*p, S, c, rr.iv), tol);
  IvPremiumStats s0;
  const IvResult r0 =
      american_implied_vol_premium(*p, S, K, T, r, q, Side::Put, o, nullptr, s0, &gate);
  ASSERT_EQ(r0.status, IvStatus::Ok);
  EXPECT_EQ(s0.native_status, ib::InverseStatus::Converged);
  EXPECT_FALSE(s0.family_refused);
}

TEST(AmericanIvPremium, BandGateSkipsTheConfirmOnlyInsideItsStatementAndNeverSkipsAll) {
  const double S = 100.0;
  const double tol = IvOpts{}.tol;
  std::size_t accepted = 0, skipped = 0, kept = 0, never_skipped = 0, never_accepted = 0;
  std::size_t fast_confirmed = 0;
  double max_skipped_newton = 0.0, max_bound = 0.0, max_fast_dsig = 0.0;
  for (const PremiumCase &c : premium_grid()) {
    const auto p = andersen_lake(S, c.K, c.T, c.sig_true, c.r, c.q, c.sd);
    if (!p || !(*p > 0.0)) continue;
    const IvResult s = american_implied_vol(*p, S, c.K, c.T, c.r, c.q, c.sd, IvOpts{});
    // (1) Band, k = 10, a band 10 % of the price wide: skipped only where the statement
    // holds, and every skipped root reprices inside the bound the gate computed.
    IvPremiumGate band;
    band.policy = IvConfirmPolicy::Band;
    band.k = 10.0;
    band.half_spread_usd = 0.05 * *p;
    band.min_relative_slope = 1.0e-5;
    IvPremiumStats st;
    const IvResult r = screen_seeded_iv(*p, S, c, st, &band);
    if (st.fallback == IvPremiumFallback::None) {
      ++accepted;
      if (st.confirm_skipped) {
        ++skipped;
        EXPECT_EQ(st.confirm_solves, 0u);
        EXPECT_EQ(st.gate_solves, 1u);
        ASSERT_TRUE(std::isfinite(st.gate_bound_vol));
        const double nd = accurate_newton_distance(*p, S, c, r.iv);
        if (std::isfinite(nd)) {
          max_skipped_newton = std::max(max_skipped_newton, nd);
          EXPECT_LE(nd, st.gate_bound_vol + 4.0 * tol) << c.K << " " << c.T << " " << c.sig_true;
        }
        max_bound = std::max(max_bound, st.gate_bound_vol);
      } else if (!st.seed_routed) {
        ++kept;
        EXPECT_GE(st.confirm_solves, 1u);
        const double nd = accurate_newton_distance(*p, S, c, r.iv);
        if (std::isfinite(nd)) EXPECT_LE(nd, 4.0 * tol) << c.K << " " << c.T;
      }
    }
    // (2) Band with no band to speak of: every accepted, unrouted root is confirmed.
    IvPremiumGate tight = band;
    tight.half_spread_usd = 1.0e-9;
    IvPremiumStats ts;
    static_cast<void>(screen_seeded_iv(*p, S, c, ts, &tight));
    if (ts.fallback == IvPremiumFallback::None && !ts.seed_routed) {
      EXPECT_FALSE(ts.confirm_skipped);
      EXPECT_GE(ts.confirm_solves, 1u);
    }
    // (3) Never: the seed-tier root as is (unless routed to accurate iterates).
    IvPremiumGate never;
    never.policy = IvConfirmPolicy::Never;
    IvPremiumStats ns;
    static_cast<void>(screen_seeded_iv(*p, S, c, ns, &never));
    if (ns.fallback == IvPremiumFallback::None) {
      ++never_accepted;
      EXPECT_EQ(ns.confirm_solves, 0u);
      if (!ns.seed_routed) {
        EXPECT_TRUE(ns.confirm_skipped);
        ++never_skipped;
      }
    }
    // (4) Always on the Fast confirm kernel (R3 arm): confirmed on Fast, Search's root.
    IvPremiumGate fast;
    fast.confirm_opts = atx::vol::al_fast_profile(std::nullopt);
    IvPremiumStats fs;
    const IvResult fr = screen_seeded_iv(*p, S, c, fs, &fast);
    if (fs.fallback == IvPremiumFallback::None && !fs.seed_routed) {
      ++fast_confirmed;
      EXPECT_GE(fs.confirm_solves, 1u);
      EXPECT_EQ(fs.confirm_profile, AlProfile::Fast);
      const double nd = accurate_newton_distance(*p, S, c, fr.iv);
      if (std::isfinite(nd)) EXPECT_LE(nd, 4.0 * tol) << c.K << " " << c.T;
      if (s.status == IvStatus::Ok) max_fast_dsig = std::max(max_fast_dsig, std::abs(fr.iv - s.iv));
    }
  }
  std::printf("[band gate] accepted %zu: skipped %zu / kept %zu at k = 10 (max skipped accurate-map "
              "Newton %.3g, max gate bound %.3g vol); never: %zu of %zu skipped; Fast confirm %zu "
              "(max |dsigma| vs Search %.3g)\n",
              accepted, skipped, kept, max_skipped_newton, max_bound, never_skipped, never_accepted,
              fast_confirmed, max_fast_dsig);
  EXPECT_GT(skipped, 50u);
  EXPECT_GT(never_skipped, 100u);
  EXPECT_GT(fast_confirmed, 100u);
}

// ── Lane K3 (r5): typed root acceptance of the (sigma, q) family route ─────────────────
namespace {

using atx::vol::AlSigmaQFamily;
using atx::vol::FamilyEnvelopeScope;
using atx::vol::kIvRootRequirement;
using atx::vol::RootAcceptanceKind;
using atx::vol::RootAcceptanceReason;
using atx::vol::ValidatedErrorEnvelope;

// The accurate (Reference) map's root of `price` bracketed by bisection to 1e-9 near
// `guess` (independent of every family and of the inversion under test); nullopt when no
// sign change is found within about +-0.05 vol.
[[nodiscard]] std::optional<std::pair<double, double>>
reference_bracket(double price, double S, const PremiumCase &c, double guess) {
  const auto at = [&](double s) { return andersen_lake(S, c.K, c.T, s, c.r, c.q, c.sd); };
  double lo = guess - 1.0e-7;
  double hi = guess + 1.0e-7;
  for (int i = 0;; ++i) {
    const auto pl = at(lo);
    const auto ph = at(hi);
    if (!pl || !ph || i >= 12 || hi - lo > 0.1) return std::nullopt;
    if (*pl <= price && *ph >= price) break;
    const double w = hi - lo;
    if (*pl > price) lo -= 4.0 * w;
    if (*ph < price) hi += 4.0 * w;
  }
  for (int i = 0; i < 64 && hi - lo > 1.0e-9; ++i) {
    const double mid = 0.5 * (lo + hi);
    const auto pm = at(mid);
    if (!pm) return std::nullopt;
    (*pm > price ? hi : lo) = mid;
  }
  return std::pair<double, double>{lo, hi};
}

// |served - sigma*| bounded by the reference bracket (its far end).
[[nodiscard]] double bracketed_error(const std::pair<double, double> &ref, double served) {
  return std::max(std::abs(served - ref.first), std::abs(served - ref.second));
}

struct NativeTally {
  std::size_t accepted = 0, skipped = 0, confirmed = 0, family_far = 0, both_ends = 0;
  double max_error = 0.0, max_width = 0.0;
};

// One native inversion under `gate`: on an accepted root the typed record, its bracket and
// the bracketed reference error are checked against the 1e-7 requirement.
void check_native_root(double price, double S, const PremiumCase &c, const IvPremiumGate &gate,
                       NativeTally &t) {
  IvOpts o;
  o.strategy = IvStrategy::Premium;
  o.seed_profile = AlProfile::Screen;
  IvPremiumStats st;
  const IvResult res = american_implied_vol_premium(price, S, c.K, c.T, c.r, c.q, c.sd, o,
                                                    nullptr, st, &gate);
  ASSERT_EQ(res.status, IvStatus::Ok);
  if (st.fallback != IvPremiumFallback::None || st.seed_routed) return;
  ++t.accepted;
  const atx::vol::RootAcceptance &a = st.acceptance;
  ASSERT_TRUE(a.kind == RootAcceptanceKind::Skipped || a.kind == RootAcceptanceKind::Confirmed)
      << static_cast<int>(a.kind);
  EXPECT_LE(a.lo, res.iv);
  EXPECT_GE(a.hi, res.iv);
  EXPECT_LE(a.hi - a.lo, kIvRootRequirement);
  t.max_width = std::max(t.max_width, a.hi - a.lo);
  if (a.kind == RootAcceptanceKind::Skipped) {
    ++t.skipped;
    EXPECT_EQ(a.reason, RootAcceptanceReason::FamilyEnclosure);
    EXPECT_EQ(st.confirm_solves, 0u);
    EXPECT_TRUE(st.confirm_skipped);
  } else {
    ++t.confirmed;
    EXPECT_GE(st.confirm_solves, 1u);
    t.family_far += a.reason == RootAcceptanceReason::ConfirmAndFamily ? 1u : 0u;
    t.both_ends += a.reason == RootAcceptanceReason::ConfirmBothEnds ? 1u : 0u;
  }
  const auto ref = reference_bracket(price, S, c, res.iv);
  ASSERT_TRUE(ref.has_value()) << c.K << " " << c.T;
  const double err = bracketed_error(*ref, res.iv);
  t.max_error = std::max(t.max_error, err);
  EXPECT_LE(err, kIvRootRequirement) << "K " << c.K << " T " << c.T << " sig " << c.sig_true
                                     << " kind " << static_cast<int>(a.kind);
}

} // namespace

// Lane K3 (replacing R1-A MJ1's secant-slope confirm): with the family's validated envelope
// in scope, a family-native root is Skipped only when two family prices and their bounds
// enclose the accurate map's root in a bracket no wider than 1e-7, and Confirmed otherwise by
// the confirm kernel closing such a bracket; every accepted root - skipped or confirmed, on
// the Reference and on the Fast confirm kernel - lies within 1e-7 of the root an
// independent bisection of the accurate map finds. Without an envelope (none in scope, or
// one built for another family) nothing is skipped.
TEST(AmericanIvPremium, FamilyNativeRootsSkipOnlyInsideTheEnvelopeAndAreBracketed) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  NativeTally with, fast, without, foreign;
  for (const double T : {0.25, 1.0}) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, 0.011);
    for (const Side sd : {Side::Put, Side::Call}) {
      AlSigmaQFamily fam(g, sd, S);
      ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, 0.010, 0.012, 4));
      AlSigmaQFamily other(g, sd, S);
      ASSERT_TRUE(other.prepare(0.05, 1.2, 16, 0.010, 0.012, 4));
      const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
      const ValidatedErrorEnvelope other_env = atx::vol::build_family_error_envelope(other);
      ASSERT_TRUE(env.covered());
      IvPremiumGate gate;
      gate.qfamily = &fam;
      gate.qfamily_native = true;
      IvPremiumGate fast_gate = gate;
      fast_gate.confirm_opts = atx::vol::al_fast_profile(std::nullopt);
      for (const double q : {0.0100, 0.0107, 0.0120}) {
        for (const double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
          for (const double sig : {0.15, 0.3, 0.6}) {
            const auto p = andersen_lake(S, K, T, sig, r, q, sd);
            if (!p || !(*p > 0.0)) continue;
            const PremiumCase c{K, T, r, q, sd, sig};
            {
              const FamilyEnvelopeScope scope(&env);
              check_native_root(*p, S, c, gate, with);
              check_native_root(*p, S, c, fast_gate, fast);
            }
            check_native_root(*p, S, c, gate, without);
            const FamilyEnvelopeScope scope(&other_env);
            check_native_root(*p, S, c, gate, foreign);
          }
        }
      }
    }
  }
  std::printf("[K3 native] envelope: %zu accepted, %zu skipped, %zu confirmed (family far end "
              "%zu); Fast confirm: %zu / %zu / %zu; no envelope: %zu / %zu / %zu (both ends %zu); "
              "foreign envelope skipped %zu; max bracketed error %.3g vol, max bracket %.3g\n",
              with.accepted, with.skipped, with.confirmed, with.family_far, fast.accepted,
              fast.skipped, fast.confirmed, without.accepted, without.skipped, without.confirmed,
              without.both_ends, foreign.skipped,
              std::max({with.max_error, fast.max_error, without.max_error}),
              std::max({with.max_width, fast.max_width, without.max_width}));
  EXPECT_GT(with.accepted, 50u);
  EXPECT_GT(with.skipped, with.accepted / 2); // the envelope admits most of this grid
  EXPECT_GT(fast.skipped, 0u);
  EXPECT_EQ(without.skipped, 0u);
  EXPECT_EQ(without.confirmed, without.accepted);
  EXPECT_EQ(foreign.skipped, 0u); // an envelope is never read for another family
}

// R1-A MJ1: the native iterate may accept with a secant to a FAR partner (the latest earlier
// point at any distance). Forced here through the iterate's memory: a remembered pair whose
// newer point sits 0.3 vol above the root and whose older one a hair above it puts the
// first iterate on the root, its partner 0.3 away, and |d| <= tol there - the acceptance
// R1-A flagged. Its secant no longer accepts anything: every such root is accepted only
// through a certified bracket of width <= 1e-7, and lies within 1e-7 of the independently
// bracketed accurate root (with and without the family's envelope in scope).
// W2c-g (K2 merged): the native attempt is now K2's batch-seam lane, a bracketed Newton on
// the fused vega that reads only the memory's carry, never its family points, and keeps no
// secant partner - the remembered pair is inert and every native root reports span 0. The
// acceptance assertions are unchanged; the far-partner count now pins that no secant exists.
TEST(AmericanIvPremium, FamilyNativeAcceptanceDoesNotRestOnANonLocalSecant) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::IvPremiumMemory;
  using atx::vol::kAlPackSigmaBump;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.05;
  const double q = 0.015;
  std::size_t far_partner = 0, checked = 0;
  double min_far_span = 1.0, max_error = 0.0;
  for (const double T : {0.25, 1.0}) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, q);
    for (const Side sd : {Side::Put, Side::Call}) {
      AlSigmaQFamily fam(g, sd, S);
      ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, 0.0149, 0.0151, 3));
      const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
      IvPremiumGate gate;
      gate.qfamily = &fam;
      gate.qfamily_native = true;
      for (const double K : {90.0, 100.0, 110.0, 125.0}) {
        for (const double sig : {0.15, 0.3}) {
          const auto p = andersen_lake(S, K, T, sig, r, q, sd);
          if (!p || !(*p > 1.0e-3)) continue;
          IvOpts o;
          o.strategy = IvStrategy::Premium;
          o.seed_profile = AlProfile::Screen;
          IvPremiumStats first;
          const IvResult r0 =
              american_implied_vol_premium(*p, S, K, T, r, q, sd, o, nullptr, first, &gate);
          if (r0.status != IvStatus::Ok || !std::isfinite(first.native_partner_span)) continue;
          const double far = std::min(r0.iv + 0.3, 1.19);
          const double near = r0.iv + 1.0e-9;
          IvPremiumMemory memory;
          memory.native = true;
          memory.points = 2;
          memory.sigma = {far, near};
          memory.rest = {fam.price(S, K, far, q).price, fam.price(S, K, near, q).price};
          for (const bool scoped : {false, true}) {
            IvPremiumMemory m = memory;
            IvPremiumStats st;
            IvResult res{};
            {
              const FamilyEnvelopeScope scope(scoped ? &env : nullptr);
              res = american_implied_vol_premium(*p, S, K, T, r, q, sd, o, &m, st, &gate);
            }
            ASSERT_EQ(res.status, IvStatus::Ok);
            if (st.fallback != IvPremiumFallback::None || st.seed_routed) continue;
            ASSERT_TRUE(std::isfinite(st.native_partner_span));
            ++checked;
            if (st.native_partner_span > 10.0 * kAlPackSigmaBump) {
              ++far_partner;
              min_far_span = std::min(min_far_span, st.native_partner_span);
            }
            EXPECT_TRUE(st.acceptance.kind == RootAcceptanceKind::Skipped ||
                        st.acceptance.kind == RootAcceptanceKind::Confirmed);
            EXPECT_LE(st.acceptance.hi - st.acceptance.lo, kIvRootRequirement);
            const PremiumCase pc{K, T, r, q, sd, sig};
            const auto ref = reference_bracket(*p, S, pc, res.iv);
            ASSERT_TRUE(ref.has_value());
            const double err = bracketed_error(*ref, res.iv);
            max_error = std::max(max_error, err);
            EXPECT_LE(err, kIvRootRequirement) << "K " << K << " T " << T << " sig " << sig;
          }
        }
      }
    }
  }
  std::printf("[K3 MJ1] %zu native roots accepted from a remembered far pair, %zu after a "
              "secant to a partner > 10 bumps away (min span %.3g), max bracketed error %.3g "
              "vol\n",
              checked, far_partner, min_far_span, max_error);
  EXPECT_GT(checked, 10u);
  EXPECT_EQ(far_partner, 0u); // K2's lane accepts on a tangent: no far secant partner
}

// A confirm kernel whose tested price bound cannot resolve the requirement at the root's
// local slope (slope x 1e-7 <= 2 eps_C) cannot certify any bracket end: the native root is
// typed PriceOnly (LowVega) and handed to Search - never accepted on a step length. Screen
// as the confirm kernel (eps_C = 7.8e-4 max(P, 1) for T > 0.1) puts every root there; the
// Fast kernel (1e-12) resolves the same roots.
TEST(AmericanIvPremium, FamilyNativeRootTheConfirmCannotResolveIsPriceOnly) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double T = 0.5;
  const AlExpiryGeometry g = prepare_al_geometry(T, r, 0.011);
  AlSigmaQFamily fam(g, Side::Put, S);
  ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, 0.010, 0.012, 4));
  IvPremiumGate screen;
  screen.qfamily = &fam;
  screen.qfamily_native = true;
  screen.confirm_opts = atx::vol::al_screen_opts();
  IvPremiumGate fast = screen;
  fast.confirm_opts = atx::vol::al_fast_profile(std::nullopt);
  std::size_t price_only = 0, typed = 0, fast_price_only = 0, fast_accepted = 0;
  for (const double K : {85.0, 95.0, 100.0, 105.0, 115.0}) {
    for (const double sig : {0.15, 0.3, 0.6}) {
      const auto p = andersen_lake(S, K, T, sig, r, 0.011, Side::Put);
      if (!p || !(*p > 1.0e-3)) continue;
      IvOpts o;
      o.strategy = IvStrategy::Premium;
      o.seed_profile = AlProfile::Screen;
      IvPremiumStats st;
      ASSERT_EQ(american_implied_vol_premium(*p, S, K, T, r, 0.011, Side::Put, o, nullptr, st,
                                             &screen)
                    .status,
                IvStatus::Ok);
      if (st.acceptance.kind != RootAcceptanceKind::None) {
        ++typed;
        if (st.acceptance.kind == RootAcceptanceKind::PriceOnly) {
          ++price_only;
          EXPECT_EQ(st.acceptance.reason, RootAcceptanceReason::LowVega);
          EXPECT_NE(st.fallback, IvPremiumFallback::None); // Search serves it
          EXPECT_EQ(st.confirm_solves, 0u);
        }
      }
      IvPremiumStats fs;
      ASSERT_EQ(american_implied_vol_premium(*p, S, K, T, r, 0.011, Side::Put, o, nullptr, fs,
                                             &fast)
                    .status,
                IvStatus::Ok);
      fast_price_only += fs.acceptance.kind == RootAcceptanceKind::PriceOnly ? 1u : 0u;
      fast_accepted += fs.acceptance.kind == RootAcceptanceKind::Confirmed ? 1u : 0u;
    }
  }
  std::printf("[K3 price-only] Screen confirm: %zu typed native roots, %zu price-only; Fast "
              "confirm: %zu confirmed, %zu price-only\n",
              typed, price_only, fast_accepted, fast_price_only);
  EXPECT_GT(typed, 5u);
  EXPECT_EQ(price_only, typed);
  EXPECT_GT(fast_accepted, 5u);
  EXPECT_EQ(fast_price_only, 0u);
}

// ── review-K3 mn-5 (fix round 1): the envelope bound decides the Skipped rule ───────────
namespace {

// A family whose early-exercise premium is material over its q box (a put at q < r, a call
// at q > r), so the trivial box bound g(xmax) alone never certifies a 1e-7 bracket at the
// test points and the envelope's node bounds decide.
struct EnclosureCase {
  Side side;
  double q_lo;
  double q_hi;
  double q; // the priced point's yield (inside the box)
};

constexpr std::array<EnclosureCase, 2> kEnclosureCases{{
    {Side::Put, 0.010, 0.012, 0.011},
    {Side::Call, 0.060, 0.062, 0.061},
}};

} // namespace

// The envelope BOUND at each end decides the Skipped rule (american_iv.cpp family_enclosure:
// P_f(a) + eps(a) <= price <= P_f(b) - eps(b)), not only the bracket width. x sits 0.3 W above
// the family's root of the price, so the price lies 0.2 W x slope above the lower end a and
// 0.8 W x slope below the upper end b (W <= 1e-7 the bracket): the envelope as built (eps below
// both gaps) encloses the root; the same envelope scaled until eps lies between the two gaps
// does not - the LOWER end's bound alone rejects, the upper end alone would accept. The mirror
// (x 0.3 W below the root) is rejected by the UPPER end's bound alone. Dropping either end's
// bound from the rule fails one of the two.
TEST(AmericanIvPremium, BatchedEnclosuresKeepScalarDecisionsAndCertifiedEnds) {
  using namespace atx::vol;
  std::size_t enclosed = 0, open = 0, recentered = 0;
  std::uint64_t batch_prices = 0, retry_prices = 0;
  for (const Side side : {Side::Put, Side::Call}) {
    const auto g = prepare_al_geometry(0.5, 0.04, 0.012);
    AlSigmaQFamily family(g, side, 100.0);
    ASSERT_TRUE(family.prepare(0.05, 1.2, 16, 0.011, 0.013, 4));
    const auto env = build_family_error_envelope(family);
    const double w = kIvRootRequirement * (1.0 - 0x1p-20);
    for (const double scale : {1.0, 1.0e7}) {
      auto envelope = env;
      for (auto &b : envelope.bound_y) if (std::isfinite(b)) b *= scale;
      std::vector<IvNativeBatchRequest> requests;
      std::vector<IvPremiumPrepared> scalar;
      for (const double K : {70.0, 90.0, 100.0, 110.0, 140.0}) {
        for (const double sigma : {0.08, 0.3, 1.1}) {
          for (const double offset : {0.0, -0.3, 0.3, 2.0}) {
            requests.push_back({family.price(100.0, K, sigma, 0.012).price,
                                100.0, K, 0.5, 0.04, 0.012, side, 1.0e-7});
            IvPremiumPrepared p;
            p.lane.valid = true;
            p.lane.sigma = sigma + offset * w;
            p.lane.status = iv_batch::InverseStatus::Converged;
            p.lane.vega = 10.0;
            scalar.push_back(p);
          }
        }
      }
      auto batch = scalar;
      // Fast confirm keeps this test entirely about the two family brackets.
      AlOpts opts;
      opts.profile = AlProfile::Fast;
      IvNativeBatchWork scalar_work, batch_work;
      iv_native_prepare_acceptance(family, &envelope, requests, scalar, opts,
                                   false, false, scalar_work);
      iv_native_prepare_acceptance(family, &envelope, requests, batch, opts,
                                   true, false, batch_work);
      EXPECT_EQ(batch_work.enclosures, scalar_work.enclosures);
      batch_prices += batch_work.family_batch_prices;
      retry_prices += batch_work.family_retry_prices;
      for (std::size_t i = 0; i < batch.size(); ++i) {
        ASSERT_EQ(batch[i].enclosure.valid, scalar[i].enclosure.valid);
        const auto &a = batch[i].enclosure.enclosure;
        const auto &b = scalar[i].enclosure.enclosure;
        ASSERT_EQ(a.enclosed, b.enclosed) << i;
        EXPECT_EQ(std::bit_cast<std::uint64_t>(a.slope),
                  std::bit_cast<std::uint64_t>(b.slope)) << i;
        if (!a.enclosed) { ++open; continue; }
        ++enclosed;
        recentered += b.lo != scalar[i].lane.sigma - 0.5 * w ? 1u : 0u;
        EXPECT_EQ(std::bit_cast<std::uint64_t>(a.lo), std::bit_cast<std::uint64_t>(b.lo));
        EXPECT_EQ(std::bit_cast<std::uint64_t>(a.hi), std::bit_cast<std::uint64_t>(b.hi));
        EXPECT_EQ(std::bit_cast<std::uint64_t>(a.root), std::bit_cast<std::uint64_t>(b.root));
        EXPECT_LE(a.hi - a.lo, kIvRootRequirement);
      }
    }
  }
  EXPECT_GT(enclosed, 0u);
  EXPECT_GT(open, 0u);
  EXPECT_GT(recentered, 0u);
  EXPECT_GT(batch_prices, 0u);
  EXPECT_GT(retry_prices, 0u);
}

TEST(AmericanIvPremium, FamilyEnclosureRejectsAnEndWhoseEnvelopeBoundLeavesItOpen) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::FamilyBoundedPrice;
  using atx::vol::FamilyEnclosure;
  using atx::vol::family_enclosure;
  using atx::vol::family_price_bounded;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double sig = 0.3;
  const double w = kIvRootRequirement * (1.0 - 0x1p-20); // acceptance_width(tol >= 1e-7)
  std::size_t lower = 0, upper = 0;
  double min_margin = std::numeric_limits<double>::infinity(); // eps / gap, or gap / eps
  for (const double T : {0.5, 1.0}) {
    for (const EnclosureCase &ec : kEnclosureCases) {
      const AlExpiryGeometry g = prepare_al_geometry(T, r, ec.q);
      AlSigmaQFamily fam(g, ec.side, S);
      ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, ec.q_lo, ec.q_hi, 4));
      const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
      ASSERT_TRUE(env.covered());
      const auto scaled = [&env](double s) {
        ValidatedErrorEnvelope e = env;
        for (double &b : e.bound_y) {
          if (std::isfinite(b)) b *= s;
        }
        return e;
      };
      for (const double K : {90.0, 100.0, 110.0}) {
        const double price = fam.price(S, K, sig, ec.q).price;
        for (const double off : {0.3, -0.3}) {
          SCOPED_TRACE("T " + std::to_string(T) + " side " +
                       std::to_string(static_cast<int>(ec.side)) + " K " + std::to_string(K) +
                       " off " + std::to_string(off));
          const double x = sig + off * w;
          const double a = x - 0.5 * w;
          const double b = x + 0.5 * w;
          const bool near_lower = off > 0.0; // the price sits nearer this end
          const auto bounds = [&](const ValidatedErrorEnvelope &e) {
            return std::pair<FamilyBoundedPrice, FamilyBoundedPrice>{
                family_price_bounded(fam, e, S, K, a, ec.q),
                family_price_bounded(fam, e, S, K, b, ec.q)};
          };
          const auto [pa, pb] = bounds(env);
          const double gap_lo = price - pa.price;
          const double gap_hi = pb.price - price;
          ASSERT_GT(gap_lo, 0.0);
          ASSERT_GT(gap_hi, 0.0);
          const double near_gap = near_lower ? gap_lo : gap_hi;
          const double far_gap = near_lower ? gap_hi : gap_lo;
          IvPremiumStats st;
          // The envelope as built: both bounds clear their gaps; the root is enclosed.
          ASSERT_LT(std::max(pa.bound_usd, pb.bound_usd), near_gap);
          const FamilyEnclosure built = family_enclosure(price, S, K, ec.q, x, w, fam, &env, st);
          ASSERT_TRUE(built.enclosed);
          EXPECT_EQ(built.lo, a);
          EXPECT_EQ(built.hi, b);
          EXPECT_LE(built.hi - built.lo, kIvRootRequirement);
          // Scale the envelope until the near end's bound sits at sqrt(near_gap far_gap):
          // above the near gap, below the far one (the bound grows with the scale).
          const double target = std::sqrt(near_gap * far_gap);
          double lo_s = 1.0;
          double hi_s = 1.0e20;
          for (int i = 0; i < 200; ++i) {
            const double mid = std::sqrt(lo_s * hi_s);
            const auto [ma, mb] = bounds(scaled(mid));
            ((near_lower ? ma : mb).bound_usd < target ? lo_s : hi_s) = mid;
          }
          const ValidatedErrorEnvelope e = scaled(hi_s);
          const auto [qa, qb] = bounds(e);
          const double eps_near = near_lower ? qa.bound_usd : qb.bound_usd;
          const double eps_far = near_lower ? qb.bound_usd : qa.bound_usd;
          ASSERT_GT(eps_near, near_gap); // this end alone leaves the root's side open ...
          ASSERT_LT(eps_far, far_gap);   // ... the other end alone would certify its side
          min_margin = std::min(min_margin, std::min(eps_near / near_gap, far_gap / eps_far));
          const FamilyEnclosure open = family_enclosure(price, S, K, ec.q, x, w, fam, &e, st);
          EXPECT_FALSE(open.enclosed);
          (near_lower ? lower : upper) += open.enclosed ? 0u : 1u;
        }
      }
    }
  }
  std::printf("[K3 enclosure bound] %zu brackets rejected by the lower end's bound alone, %zu by "
              "the upper end's alone (each enclosed with the envelope as built); min margin %.3g\n",
              lower, upper, min_margin);
  EXPECT_EQ(lower, 12u);
  EXPECT_EQ(upper, 12u);
}

// The same through both acceptance routes: with a copy of the family's envelope whose node
// bounds are inflated 1e6-fold in scope (same family, box and node counts: the identity check
// passes), neither the family-native acceptance nor the B-family band route skips a confirm -
// every accepted root is Confirmed and lies within 1e-7 of the bisected accurate root - while
// the envelope as built skips on the same grid. A rule that read no bound would skip here.
TEST(AmericanIvPremium, AnInflatedEnvelopeBoundSkipsNoConfirmOnEitherRoute) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double T = 0.5;
  NativeTally native, native_inflated;
  struct BandTally {
    std::size_t accepted = 0, skipped = 0, confirmed = 0;
    double max_error = 0.0;
  } band, band_inflated;
  // One B-family (policy Band) inversion: a skip is an enclosure of width <= 1e-7, a confirm
  // the established Newton contract; either way within 1e-7 of the accurate map's root.
  const auto check_band_root = [&](double price, const PremiumCase &c, const IvPremiumGate &gate,
                                   BandTally &t) {
    IvOpts o;
    o.strategy = IvStrategy::Premium;
    o.seed_profile = AlProfile::Screen;
    IvPremiumStats st;
    const IvResult res =
        american_implied_vol_premium(price, S, c.K, c.T, c.r, c.q, c.sd, o, nullptr, st, &gate);
    ASSERT_EQ(res.status, IvStatus::Ok);
    if (st.fallback != IvPremiumFallback::None || st.seed_routed) return;
    ++t.accepted;
    if (st.acceptance.kind == RootAcceptanceKind::Skipped) {
      ++t.skipped;
      EXPECT_EQ(st.acceptance.reason, RootAcceptanceReason::FamilyEnclosure);
      EXPECT_LE(st.acceptance.hi - st.acceptance.lo, kIvRootRequirement);
    } else {
      ASSERT_EQ(st.acceptance.kind, RootAcceptanceKind::Confirmed);
      EXPECT_EQ(st.acceptance.reason, RootAcceptanceReason::NewtonContract);
      ++t.confirmed;
    }
    const auto ref = reference_bracket(price, S, c, res.iv);
    ASSERT_TRUE(ref.has_value());
    t.max_error = std::max(t.max_error, bracketed_error(*ref, res.iv));
    EXPECT_LE(bracketed_error(*ref, res.iv), kIvRootRequirement);
  };
  for (const EnclosureCase &ec : kEnclosureCases) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, ec.q);
    AlSigmaQFamily fam(g, ec.side, S);
    ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, ec.q_lo, ec.q_hi, 4));
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
    ASSERT_TRUE(env.covered());
    ValidatedErrorEnvelope inflated = env;
    for (double &b : inflated.bound_y) {
      if (std::isfinite(b)) b *= 1.0e6;
    }
    IvPremiumGate native_gate;
    native_gate.qfamily = &fam;
    native_gate.qfamily_native = true;
    IvPremiumGate band_gate;
    band_gate.qfamily = &fam;
    band_gate.policy = IvConfirmPolicy::Band;
    for (const double K : {85.0, 95.0, 100.0, 105.0, 115.0}) {
      for (const double sig : {0.15, 0.3, 0.6}) {
        const auto p = andersen_lake(S, K, T, sig, r, ec.q, ec.side);
        if (!p || !(*p > 1.0e-3)) continue;
        const PremiumCase c{K, T, r, ec.q, ec.side, sig};
        {
          const FamilyEnvelopeScope scope(&env);
          check_native_root(*p, S, c, native_gate, native);
          check_band_root(*p, c, band_gate, band);
        }
        const FamilyEnvelopeScope scope(&inflated);
        check_native_root(*p, S, c, native_gate, native_inflated);
        check_band_root(*p, c, band_gate, band_inflated);
      }
    }
  }
  std::printf("[K3 inflated bound] native: %zu accepted / %zu skipped, inflated %zu / %zu; band "
              "route: %zu / %zu, inflated %zu / %zu; max bracketed error %.3g vol\n",
              native.accepted, native.skipped, native_inflated.accepted, native_inflated.skipped,
              band.accepted, band.skipped, band_inflated.accepted, band_inflated.skipped,
              std::max({native.max_error, native_inflated.max_error, band.max_error,
                        band_inflated.max_error}));
  EXPECT_GT(native.skipped, 0u);
  EXPECT_GT(band.skipped, 0u);
  EXPECT_GT(native_inflated.accepted, 5u);
  EXPECT_GT(band_inflated.accepted, 5u);
  EXPECT_EQ(native_inflated.skipped, 0u);
  EXPECT_EQ(band_inflated.skipped, 0u);
  EXPECT_EQ(native_inflated.confirmed, native_inflated.accepted);
  EXPECT_EQ(band_inflated.confirmed, band_inflated.accepted);
}

// ── Lane K5 (r6): the enclosure's bracket meets the width rule as computed ──────────────
// family_enclosure places its ends around x and skips the confirm only when the COMPUTED
// bracket satisfies b - a <= w (w = acceptance_width(tol) < 1e-7). With the ends placed at
// x -/+ w/2 in floating point, rounding widened b - a one ulp of x past w on about two thirds
// of SPY's native roots (17,186 of 28,117 at 746154d6): roots whose two ends the validated
// envelope certified were refused and paid a confirm solve. At points x where x -/+ w/2 round
// to a bracket wider than w, with the price the family's own price at x (its root centred) and
// the envelope bound clearing both end gaps, the enclosure must succeed, its bracket no wider
// than w and holding x. The width rule itself is unchanged: no bracket wider than w is accepted.
TEST(AmericanIvPremium, FamilyEnclosureMeetsTheWidthWhereRoundingWidensTheEnds) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::FamilyBoundedPrice;
  using atx::vol::FamilyEnclosure;
  using atx::vol::family_enclosure;
  using atx::vol::family_price_bounded;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double T = 0.5;
  const double w = kIvRootRequirement * (1.0 - 0x1p-20); // acceptance_width(tol >= 1e-7)
  std::size_t widened = 0;
  std::size_t enclosed = 0;
  std::size_t open = 0; // an end the envelope bound leaves open
  double max_width_excess = -std::numeric_limits<double>::infinity(); // (hi - lo) - w
  for (const EnclosureCase &ec : kEnclosureCases) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, ec.q);
    AlSigmaQFamily fam(g, ec.side, S);
    ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, ec.q_lo, ec.q_hi, 4));
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
    ASSERT_TRUE(env.covered());
    for (const double K : {90.0, 100.0, 110.0}) {
      // The binade fixes how the half-width rounds: the ends x -/+ w/2 round to a bracket
      // wider than w for every x where frac(w / (2 ulp(x))) > 1/2 - at this w, x in
      // [2^-6, 2^-2) and [1, 2) (also [2^-8, 2^-7) and [4, 8)); not [2^-7, 2^-6), [0.25, 1)
      // or [2, 4). 0.12 and 0.2 lie in [2^-4, 2^-3) and [2^-3, 2^-2), 1.1 in [1, 2).
      for (const double sig : {0.12, 0.2, 1.1}) {
        // The first points at or above sig where the two ends round to a wider bracket.
        double x = sig;
        std::size_t found = 0;
        for (int step = 0; step < 256 && found < 4u; ++step, x = std::nextafter(x, 1.0)) {
          const double a = x - 0.5 * w;
          const double b = x + 0.5 * w;
          if (b - a <= w) continue;
          ++found;
          SCOPED_TRACE("side " + std::to_string(static_cast<int>(ec.side)) + " K " +
                       std::to_string(K) + " x " + std::to_string(x));
          const double price = fam.price(S, K, x, ec.q).price;
          const FamilyBoundedPrice pa = family_price_bounded(fam, env, S, K, a, ec.q);
          const FamilyBoundedPrice pb = family_price_bounded(fam, env, S, K, b, ec.q);
          // Only points whose two ends the envelope certifies: there the width alone decides
          // (near the exercise boundary the bound can leave an end open - not this case).
          if (!(pa.price + pa.bound_usd <= price && price <= pb.price - pb.bound_usd)) {
            ++open;
            continue;
          }
          ++widened;
          IvPremiumStats st;
          const FamilyEnclosure enc = family_enclosure(price, S, K, ec.q, x, w, fam, &env, st);
          EXPECT_TRUE(enc.enclosed);
          if (!enc.enclosed) continue;
          ++enclosed;
          max_width_excess = std::max(max_width_excess, (enc.hi - enc.lo) - w);
          EXPECT_LE(enc.hi - enc.lo, w);
          EXPECT_EQ(enc.lo, a);
          EXPECT_LE(enc.lo, x);
          EXPECT_GE(enc.hi, x);
          EXPECT_GE(enc.root, enc.lo);
          EXPECT_LE(enc.root, enc.hi);
        }
        EXPECT_EQ(found, 4u) << "no rounding-widened bracket near sigma " << sig;
      }
    }
  }
  std::printf("[K5 enclosure width] %zu rounding-widened brackets with both ends certified, %zu "
              "enclosed (%zu points with an end the bound leaves open); max (hi - lo) - w %.3g\n",
              widened, enclosed, open, max_width_excess);
  EXPECT_GE(widened, 60u);
  EXPECT_EQ(enclosed, widened);
  EXPECT_LE(max_width_excess, 0.0);
}

// Fix round 1 (review-K5 m2): the ends the enclosure SERVES are the ends it certified. Where
// rounding widens x -/+ w/2, the upper end moves an ulp toward the lower one, so its certificate
// must be the family price and envelope bound AT the moved end, never the one at x + w/2. At each
// such x the price is set to the edge of what the unmoved end certifies, P_f(x + w/2) - eps
// there; at many of these points the moved end's P_f - eps is lower (the one-ulp move), so it
// does not certify that price and no enclosure may be served with it. Every served bracket's two
// ends are re-priced here and must certify the price; the discriminating points are counted.
TEST(AmericanIvPremium, FamilyEnclosureCertifiesTheEndsItServes) {
  using atx::vol::AlExpiryGeometry;
  using atx::vol::FamilyBoundedPrice;
  using atx::vol::FamilyEnclosure;
  using atx::vol::family_enclosure;
  using atx::vol::family_price_bounded;
  using atx::vol::prepare_al_geometry;
  const double S = 100.0;
  const double r = 0.04;
  const double T = 0.5;
  const double w = kIvRootRequirement * (1.0 - 0x1p-20); // acceptance_width(tol >= 1e-7)
  std::size_t checked = 0;
  std::size_t moved_open = 0; // the moved end does not certify the unmoved end's edge price
  std::size_t served = 0;
  for (const EnclosureCase &ec : kEnclosureCases) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, ec.q);
    AlSigmaQFamily fam(g, ec.side, S);
    ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, ec.q_lo, ec.q_hi, 4));
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
    ASSERT_TRUE(env.covered());
    const auto bounded = [&](double K, double sigma) {
      return family_price_bounded(fam, env, S, K, sigma, ec.q);
    };
    for (const double K : {90.0, 100.0, 110.0}) {
      for (const double sig : {0.12, 0.2, 1.1}) { // binades where x -/+ w/2 round wide
        double x = sig;
        std::size_t found = 0;
        for (int step = 0; step < 256 && found < 4u; ++step, x = std::nextafter(x, 1.0)) {
          const double a = x - 0.5 * w;
          const double b0 = x + 0.5 * w;
          if (b0 - a <= w) continue;
          ++found;
          double b = b0; // the moved end, as family_enclosure places it
          for (int k = 0; k < 4 && !(b - a <= w); ++k) {
            b = std::nextafter(b, a);
          }
          SCOPED_TRACE("side " + std::to_string(static_cast<int>(ec.side)) + " K " +
                       std::to_string(K) + " x " + std::to_string(x));
          const FamilyBoundedPrice pa = bounded(K, a);
          const FamilyBoundedPrice pb0 = bounded(K, b0);
          const FamilyBoundedPrice pb = bounded(K, b);
          const double edge = pb0.price - pb0.bound_usd; // the largest price b0 certifies
          if (!(pa.price + pa.bound_usd <= edge)) continue; // the lower end must certify it
          ++checked;
          const bool open_at_b = !(edge <= pb.price - pb.bound_usd);
          moved_open += open_at_b ? 1u : 0u;
          IvPremiumStats st;
          const FamilyEnclosure enc = family_enclosure(edge, S, K, ec.q, x, w, fam, &env, st);
          if (open_at_b) {
            EXPECT_FALSE(enc.enclosed && enc.hi == b) << "served the moved end on b0's certificate";
          }
          if (!enc.enclosed) continue;
          ++served;
          const FamilyBoundedPrice lo = bounded(K, enc.lo);
          const FamilyBoundedPrice hi = bounded(K, enc.hi);
          EXPECT_LE(lo.price + lo.bound_usd, edge) << "lower end served without its certificate";
          EXPECT_LE(edge, hi.price - hi.bound_usd) << "upper end served without its certificate";
          EXPECT_LE(enc.hi - enc.lo, w);
        }
        EXPECT_EQ(found, 4u) << "no rounding-widened bracket near sigma " << sig;
      }
    }
  }
  std::printf("[K5 served ends] %zu edge prices checked, %zu where the moved end does not certify "
              "the unmoved end's edge, %zu enclosures served (each end re-certified)\n",
              checked, moved_open, served);
  EXPECT_GE(checked, 60u);
  EXPECT_GE(moved_open, 10u); // the test discriminates: these points fail a stale certificate
}
