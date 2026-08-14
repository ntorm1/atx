#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/surface_overlay.hpp"

// Task F-8 / FIT-F4: `SurfaceOverlay` is the one definition of the smile-shift
// algebra the greek stencil, the scenario engine and any future theta engine
// all read a shifted surface through.
//
// The oracles here are deliberately NOT the library. Every expectation is
// either a closed form written out by hand over a two-parameter analytic stub
// surface whose `iv` is `atm + slope*k + term*T` -- so the expected number can
// be computed on paper -- or a bitwise comparison against the expression the
// overlay is claimed to reduce to. Nothing in this file calls a pricer, a
// strip, or any other code path that could share a mistake with the thing
// under test.

namespace {

using atx::vol::floor_smile_iv;
using atx::vol::kMinSmileShiftedIv;
using atx::vol::StickyMode;
using atx::vol::sticky_k_shift;
using atx::vol::SurfaceOverlay;

// `iv(k,T) = atm + slope*k + term*T`. Exactly linear in both arguments, so
// every expectation below is a one-line hand computation rather than a
// tolerance around a fit.
struct LinearSurface {
  double atm{0.20};
  double slope{0.0};
  double term{0.0};

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    return atm + slope * k_log + term * T;
  }
};

// The same surface WITH a batched read, so the two `requires`-detected shapes
// can be distinguished. In-place safe (`x` and `out` may alias), matching what
// every batched surface in this library promises its callers.
struct BatchedLinearSurface : LinearSurface {
  void iv_batch(std::span<const double> x, double T, std::span<double> out) const noexcept {
    const std::size_t n = std::min(x.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = iv(x[i], T);
    }
  }
};

template <class SurfaceT>
[[nodiscard]] constexpr bool has_iv_batch() noexcept {
  return requires(const SurfaceT& s, std::span<const double> xs, double t, std::span<double> o) {
    s.iv_batch(xs, t, o);
  };
}

void expect_bit_eq(double a, double b, const char* what) {
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a), std::bit_cast<std::uint64_t>(b))
      << what << ": " << a << " vs " << b;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

}  // namespace

// ── The sign (§ the header's sticky-mode discussion) ────────────────────────

// k = ln(K/F) and a spot move scales F by (1+h), so a FIXED strike moves to
// k - ln(1+h); reading its unchanged vol means evaluating the base at
// k + ln(1+h). The PLUS is the whole content of sticky-strike, and it is the
// sign `deriv_greeks`' spot stencil has always used. This is an equality on
// the bits, not a tolerance: a `log(1+h)` written instead of `log1p(h)` is a
// different number at the 1e-4 bumps the stencil defaults to.
TEST(StickyStrike, SignIsPositiveLogOnePlusH) {
  for (const double h : {1.0e-6, 1.0e-4, 0.01, 0.25, -1.0e-4, -0.25}) {
    expect_bit_eq(sticky_k_shift(StickyMode::StickyStrike, h), std::log1p(h), "sticky-strike");
    expect_bit_eq(sticky_k_shift(StickyMode::StickyMoneyness, h), 0.0, "sticky-moneyness");
  }
}

// The definitional difference between the two modes, asserted as the brief
// frames it: a spot bump under StickyMoneyness leaves the ATM read untouched
// (that IS the definition), while under StickyStrike the same bump reads up the
// skew -- and the two disagree ONLY where the surface has slope.
//
// The two hand-computed expectations are what make this more than a
// consistency check: with slope s, StickyStrike's ATM read is
// `atm + s*ln(1+h)` exactly, and on a flat surface (s = 0) that collapses onto
// StickyMoneyness's `atm`. A sign error in `read_k` flips the first and leaves
// the second untouched -- which is exactly why a flat-surface fixture cannot
// see it.
TEST(StickyStrike, StickyModesDisagreeOnASkewedSurfaceOnly) {
  const double h = 0.01;
  const double ks = std::log1p(h);
  const LinearSurface skewed{0.20, -0.35, 0.0};  // equity skew: vol falls with k
  const LinearSurface flat{0.20, 0.0, 0.0};

  const SurfaceOverlay<LinearSurface> ss_skew{
      .base = &skewed, .k_shift = sticky_k_shift(StickyMode::StickyStrike, h)};
  const SurfaceOverlay<LinearSurface> sm_skew{
      .base = &skewed, .k_shift = sticky_k_shift(StickyMode::StickyMoneyness, h)};

  const double atm_base = skewed.iv(0.0, 1.0);
  const double atm_ss = ss_skew.iv(0.0, 1.0);
  const double atm_sm = sm_skew.iv(0.0, 1.0);

  // Sticky-moneyness: unchanged, by definition.
  expect_bit_eq(atm_sm, atm_base, "sticky-moneyness ATM");

  // Sticky-strike: moved by exactly slope * ln(1+h), hand-computed.
  EXPECT_DOUBLE_EQ(atm_ss, 0.20 + (-0.35) * ks);

  // The sign difference the brief asks for, stated on the DELTAS rather than
  // the levels: an up-spot move on a downward-sloping smile reads a LOWER vol
  // under sticky-strike, and no change at all under sticky-moneyness. Under
  // the opposite (wrong) k_shift sign this delta would be positive.
  EXPECT_LT(atm_ss - atm_base, 0.0);
  EXPECT_EQ(atm_sm - atm_base, 0.0);

  // And an upward-sloping smile flips the sticky-strike delta, confirming the
  // sign is carried by the surface's slope rather than baked into the shift.
  const LinearSurface up_sloped{0.20, +0.35, 0.0};
  const SurfaceOverlay<LinearSurface> ss_up{
      .base = &up_sloped, .k_shift = sticky_k_shift(StickyMode::StickyStrike, h)};
  EXPECT_GT(ss_up.iv(0.0, 1.0) - up_sloped.iv(0.0, 1.0), 0.0);

  // On a flat surface the two modes are indistinguishable -- the documented
  // blind spot, pinned so a future fixture author cannot mistake a flat-surface
  // pass for evidence about the sign.
  const SurfaceOverlay<LinearSurface> ss_flat{
      .base = &flat, .k_shift = sticky_k_shift(StickyMode::StickyStrike, h)};
  const SurfaceOverlay<LinearSurface> sm_flat{
      .base = &flat, .k_shift = sticky_k_shift(StickyMode::StickyMoneyness, h)};
  expect_bit_eq(ss_flat.iv(0.0, 1.0), sm_flat.iv(0.0, 1.0), "flat surface, both modes");
}

// ── The additive fields must be inert when they are zero ───────────────────

// The overlay carries three fields the views it replaced did not have. A
// zero-valued term is arithmetically inert but not always BITWISE inert --
// `0.0 * k` is `-0.0` for k < 0, and `0.0 * inf` is NaN -- so an
// unconditionally-evaluated smile term would let this type's mere existence
// move a mark on a parallel-only bump. Asserted over the awkward inputs a wing
// node can actually carry, on the bits.
TEST(SurfaceOverlay, ZeroSmileCoefficientsAreBitwiseTheBareParallelShift) {
  const double sigmas[] = {0.20, 0.0, -0.0, 1.0e-300, -1.0e-300, kInf, -kInf, kNaN};
  const double ks[] = {-2.0, -1.0, -0.0, 0.0, 1.0, 2.0, kInf, -kInf, kNaN};
  const double vol_shifts[] = {0.0, -0.0, 1.0e-4, -1.0e-4, kNaN};

  const LinearSurface surf{};
  for (const double vs : vol_shifts) {
    const SurfaceOverlay<LinearSurface> ov{.base = &surf, .vol_shift = vs};
    for (const double sigma : sigmas) {
      for (const double k : ks) {
        expect_bit_eq(ov.shift_iv(sigma, k), sigma + vs, "zero-coefficient shift_iv");
      }
    }
  }
}

// `term_scale` defaults to 1.0, and `T * 1.0` must be bitwise T -- the bump
// read cache keys on the bit pattern of the T it queries, so a term_scale that
// perturbed T would silently turn every cache hit into a miss.
TEST(SurfaceOverlay, DefaultTermScaleIsBitwiseIdentityOnT) {
  const LinearSurface surf{};
  const SurfaceOverlay<LinearSurface> ov{.base = &surf};
  for (const double T : {1.0e-8, 0.001, 0.35, 1.0, 30.0, 0.0, kInf}) {
    expect_bit_eq(ov.read_t(T), T, "default term_scale");
  }
  const SurfaceOverlay<LinearSurface> halved{.base = &surf, .term_scale = 0.5};
  EXPECT_DOUBLE_EQ(halved.read_t(0.35), 0.175);
  // A term-scaled read is the base evaluated at the scaled tenor: with the
  // stub's `term*T` leg, that is a hand-checkable number.
  const LinearSurface term_surf{0.20, 0.0, 0.10};
  const SurfaceOverlay<LinearSurface> ov_term{.base = &term_surf, .term_scale = 0.5};
  EXPECT_DOUBLE_EQ(ov_term.iv(0.0, 2.0), 0.20 + 0.10 * 1.0);
}

// ── The smile terms ────────────────────────────────────────────────────────

// Closed form, in the OVERLAY's own moneyness: composing a respot with a skew
// bump must give the skew the caller asked for at the k the caller asked for,
// not at the shifted k handed to the base.
TEST(SurfaceOverlay, SmileTermsUseTheOverlaysOwnMoneyness) {
  const LinearSurface surf{0.20, -0.30, 0.0};
  const double k_shift = 0.05;
  const double slope = 0.02;
  const double curv = 0.50;
  const SurfaceOverlay<LinearSurface> ov{.base = &surf,
                                         .vol_shift = 0.01,
                                         .skew_shift = slope,
                                         .convexity_shift = curv,
                                         .k_shift = k_shift};
  const double k = -0.20;
  // base read at k + k_shift, then the three shifts at k.
  const double expected =
      (0.20 - 0.30 * (k + k_shift)) + 0.01 + slope * k + curv * k * k;
  EXPECT_DOUBLE_EQ(ov.iv(k, 1.0), expected);
}

// The floor exists because the smile coefficients multiply k out to the strip's
// resolved wing band, where a large caller-set bump can drive a node's vol to
// zero or below. It must NOT apply to a parallel-only shift (which has always
// been allowed to report a small positive vol unmolested), and it must let NaN
// through -- a read with no opinion is not a read of 1e-4.
TEST(SurfaceOverlay, SmileFloorBindsOnlyOnSmileTermsAndPassesNaN) {
  const LinearSurface surf{0.20, 0.0, 0.0};

  // A skew bump big enough to take a deep wing below zero floors instead.
  const SurfaceOverlay<LinearSurface> steep{.base = &surf, .skew_shift = 1.0};
  EXPECT_DOUBLE_EQ(steep.iv(-0.40, 1.0), kMinSmileShiftedIv);

  // The same magnitude as a PARALLEL shift is reported as-is, floor untouched.
  const SurfaceOverlay<LinearSurface> parallel{.base = &surf, .vol_shift = -0.19999};
  EXPECT_LT(parallel.iv(0.0, 1.0), kMinSmileShiftedIv);
  EXPECT_GT(parallel.iv(0.0, 1.0), 0.0);

  // NaN in, NaN out, on both branches -- the comparison form of the floor is
  // false for a NaN left operand, unlike `std::fmax`, which would return the
  // floor and manufacture a usable vol out of nothing.
  EXPECT_TRUE(std::isnan(floor_smile_iv(kNaN)));
  const LinearSurface nan_surf{kNaN, 0.0, 0.0};
  const SurfaceOverlay<LinearSurface> ov_nan{.base = &nan_surf, .skew_shift = 1.0};
  EXPECT_TRUE(std::isnan(ov_nan.iv(0.1, 1.0)));
}

// ── The batched read, and the detection that gates it ──────────────────────

// R1, pinned. `iv_batch` must participate in overload resolution only when the
// WRAPPED surface has one. If the overlay declared it unconditionally, every
// scalar-only surface in this library would newly take a batched gather it has
// never taken -- a behaviour change that compiles clean and shows up only as
// moved marks. This static_assert is the cheapest possible guard on it.
TEST(SurfaceOverlay, BatchDetectionFollowsTheWrappedSurface) {
  static_assert(!has_iv_batch<LinearSurface>());
  static_assert(has_iv_batch<BatchedLinearSurface>());
  static_assert(!has_iv_batch<SurfaceOverlay<LinearSurface>>(),
                "overlaying a scalar-only surface must not manufacture a batched read");
  static_assert(has_iv_batch<SurfaceOverlay<BatchedLinearSurface>>(),
                "overlaying a batched surface must forward the batched read");
  SUCCEED();
}

// The two paths must be the same function. Bitwise, not near: the strip picks
// between them by detection alone, so any divergence is a mark that depends on
// which surface type it was priced through.
TEST(SurfaceOverlay, BatchedReadIsBitwiseTheScalarRead) {
  const BatchedLinearSurface surf{{0.20, -0.30, 0.10}};
  const double ks[] = {-1.2, -0.5, -0.0, 0.0, 0.25, 0.5, 1.2};

  const SurfaceOverlay<BatchedLinearSurface> variants[] = {
      {.base = &surf},
      {.base = &surf, .vol_shift = 0.01, .k_shift = std::log1p(0.01)},
      {.base = &surf, .skew_shift = 0.02},
      {.base = &surf, .convexity_shift = 0.5},
      {.base = &surf,
       .vol_shift = -0.01,
       .skew_shift = -0.02,
       .convexity_shift = 0.3,
       .k_shift = std::log1p(-0.01),
       .term_scale = 0.5},
  };

  for (const auto& ov : variants) {
    std::vector<double> out(std::size(ks), 0.0);
    ov.iv_batch(std::span<const double>{ks, std::size(ks)}, 0.35, std::span<double>{out});
    for (std::size_t i = 0; i < std::size(ks); ++i) {
      expect_bit_eq(out[i], ov.iv(ks[i], 0.35), "batched vs scalar");
    }
  }
}
