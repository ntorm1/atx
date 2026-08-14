#pragma once

// Smile-dynamics overlay over any log-moneyness vol source (Task F-8, FIT-F4 /
// LIT-8). Tier-B: additive, depends only on `<span>`/`<algorithm>`.
//
// WHY THIS EXISTS. Every surface in this library answers `iv(k, T)` and nothing
// else: there is no way to say "the same surface, seen from 1% higher spot" or
// "the same surface, one skew point steeper" without refitting it. Scenario,
// theta and greek engines all need exactly that, and before this header each
// grew its own private answer -- `derivatives.cpp` alone carried three views
// (parallel shift, sticky-strike respot, smile shape) that had to be kept
// numerically in step by hand. This is the one place that algebra lives:
//
//   overlay.iv(k, T) = base.iv(k + k_shift, T * term_scale)
//                      + vol_shift + skew_shift*k + convexity_shift*k*k
//
// with the smile terms floored (see `kMinSmileShiftedIv`). The shifts are in
// the OVERLAY's own coordinate: `k` is what the caller asked for, not the
// shifted `k + k_shift` handed to the base -- so a caller composing a respot
// with a skew bump gets the skew it asked for at the moneyness it asked for.
//
// ── STICKY MODE, AND THE SIGN THAT IS EASY TO GET BACKWARDS ─────────────────
//
// `k = ln(K/F)`. A relative spot move `h` scales the forward to `F*(1+h)`, so a
// FIXED absolute strike `K` sits at the new moneyness `k' = k - ln(1+h)`.
// Reading that strike's UNCHANGED vol off the base surface therefore means
// evaluating the base at `k' + ln(1+h)`, i.e.
//
//   StickyStrike     k_shift = +ln(1+h)   -- vol travels with the strike
//   StickyMoneyness  k_shift = 0          -- vol travels with the moneyness
//
// The `+` is load-bearing and is the sign `deriv_greeks`' spot stencil has
// always used (`eval_bump_table`, derivatives.cpp). Taking it negative inverts
// the respot: it stays silently correct on a FLAT surface, where the smile has
// no slope to read the wrong side of, and breaks delta, gamma and vanna on
// every skewed one. `StickyStrike.SignIsPositiveLogOnePlusH` and
// `StickyModesDisagreeOnASkewedSurfaceOnly` (surface_overlay_test.cpp) pin
// both halves of that statement.
//
// ── WHAT THIS DELIBERATELY DOES NOT DO ─────────────────────────────────────
//
// It does not move the CURVES. A spot scenario is a surface read AND a bumped
// forward/spot pair; this type is only the first half (`respot_curves` in
// derivatives.cpp is the second). A `k_shift` alone is not a spot bump.
//
// It exposes no `certified_wing_band` member, deliberately -- matching the
// bumped views it replaces. Wing trust belongs to the surface that was
// certified, not to a shifted read of it; the strip receives the centre's
// resolved band through `DerivConfig::wing_clamp_k` instead. Adding the member
// here would be picked up by `deriv_greeks`' structural detection and would
// silently re-certify a shifted surface.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace atx::vol {

// Floor for any smile-shifted vol. `skew_shift`/`convexity_shift` multiply k,
// which a strip evaluates out to its resolved wing band, so a large caller-set
// coefficient can drive a wing node's vol to zero or below. Small enough to sit
// far below any economically meaningful vol, so it never binds on a sanely
// sized bump and a central difference taken across it stays clean.
inline constexpr double kMinSmileShiftedIv = 1.0e-4;

// NOT `std::fmax`: `fmax(NaN, floor)` returns FLOOR, which would manufacture a
// usable vol out of a read that had no opinion at all and defeat the NaN
// propagation every layer above depends on ("NaN = not computed", plus the
// strip's own bad-node accounting). The comparison form is false for a NaN left
// operand, so NaN passes straight through.
[[nodiscard]] constexpr double floor_smile_iv(double v) noexcept {
  return v < kMinSmileShiftedIv ? kMinSmileShiftedIv : v;
}

// How a relative spot move maps onto the base surface's log-moneyness axis.
enum class StickyMode : std::uint8_t {
  StickyStrike = 0,    // a fixed strike keeps its vol
  StickyMoneyness = 1  // a fixed moneyness keeps its vol
};

// The `k_shift` a relative spot move `spot_rel` (h, so S -> S*(1+h)) implies
// under `mode`. See the sign discussion in this file's header comment; the
// whole point of routing the stencil's own `log1p` through here is that the
// convention has exactly one definition to disagree with.
[[nodiscard]] double sticky_k_shift(StickyMode mode, double spot_rel) noexcept;

// A non-owning view over `base`, which must outlive it and must expose
// `double iv(double k_log, double T) const noexcept`. `iv_batch` is forwarded
// only when `base` itself has one -- see the note on that member.
//
// Aggregate by design (`{&surface, .vol_shift = ...}`), and cheap enough to
// build per read: every member is a double or a pointer.
template <class SurfaceT>
struct SurfaceOverlay {
  const SurfaceT* base;  // non-owning, non-null
  double vol_shift{0.0};
  double skew_shift{0.0};        // vol per unit k; s < 0 steepens the equity skew
  double convexity_shift{0.0};   // vol per unit k^2; symmetric, leaves ATM alone
  double k_shift{0.0};           // see `sticky_k_shift`
  double term_scale{1.0};        // base is read at T * term_scale

  // The base-surface query this overlay's `(k_log, T)` resolves to. Split out
  // so a caller that memoizes base reads (derivatives.cpp's bump-read cache)
  // can key on exactly the query that will be issued.
  [[nodiscard]] constexpr double read_k(double k_log) const noexcept { return k_log + k_shift; }
  [[nodiscard]] constexpr double read_t(double T) const noexcept { return T * term_scale; }

  [[nodiscard]] constexpr bool has_smile_shift() const noexcept {
    return skew_shift != 0.0 || convexity_shift != 0.0;
  }

  // The post-read half of the algebra: what this overlay reports given the
  // base's answer `sigma` at `read_k(k_log)`.
  //
  // The zero-coefficient branches are not an optimization. A term that is
  // arithmetically zero is not always bitwise inert -- `0.0 * k` is `-0.0` for
  // k < 0, `0.0 * inf` is NaN -- so evaluating the smile terms unconditionally
  // would let this type's mere EXISTENCE move a mark that carries no smile
  // shift, which is precisely what the overlay must not do. Skipping a
  // coefficient that is exactly zero makes the parallel-only expression
  // `sigma + vol_shift` character for character, for every input including the
  // infinities and NaNs a wing node can carry.
  [[nodiscard]] constexpr double shift_iv(double sigma, double k_log) const noexcept {
    const double parallel = sigma + vol_shift;
    if (!has_smile_shift()) {
      return parallel;
    }
    double v = parallel;
    if (skew_shift != 0.0) {
      v += skew_shift * k_log;
    }
    if (convexity_shift != 0.0) {
      v += convexity_shift * k_log * k_log;
    }
    return floor_smile_iv(v);
  }

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    return shift_iv(base->iv(read_k(k_log), read_t(T)), k_log);
  }

  // Participates in overload resolution -- and so in the structural
  // `requires`-detection every batched consumer uses -- ONLY when the wrapped
  // `SurfaceT` has a batched read of its own. Declaring it unconditionally
  // would newly route every scalar-only surface through a batched gather it
  // has never taken, which is a behaviour change dressed as a refactor; the
  // mirror of that mistake cost Task P-3 a review round.
  //
  // Uses `out` as scratch for the shifted queries, so `x` and `out` must be
  // distinct buffers and `base->iv_batch` must tolerate in-place operation --
  // both true at every call site in this library.
  void iv_batch(std::span<const double> x, double T, std::span<double> out) const noexcept
      requires requires(const SurfaceT& s, std::span<const double> xs, double t,
                        std::span<double> o) { s.iv_batch(xs, t, o); }
  {
    const std::size_t n = std::min(x.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = read_k(x[i]);
    }
    base->iv_batch(out.first(n), read_t(T), out.first(n));
    if (!has_smile_shift()) {
      for (std::size_t i = 0; i < n; ++i) {
        out[i] += vol_shift;
      }
      return;
    }
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = shift_iv(out[i], x[i]);
    }
  }
};

}  // namespace atx::vol
