#pragma once

// Gauss-Hermite quadrature + lognormal random-variable helpers, consumed by
// the vol-derivatives kernels (variance/vol swaps and their capped/mid-life
// variants) to evaluate E[g(W)] for a lognormal W by numerical integration
// against the physicists' weight exp(-x^2), plus the closed-form lognormal
// identities used both as engine primitives and as accuracy oracles.
//
// ── Numerical basis (primary source: Abramowitz & Stegun 25.4.46) ─────────
// Fixed-order (21-point) Gauss-Hermite rule: nodes are the roots of the
// physicists' Hermite polynomial H_n (recurrence
// H_{j+1}(x) = 2x*H_j(x) - 2j*H_{j-1}(x), seeded H_0=1, H_1=2x), found by
// Newton's method; weights w_i = sqrt(pi)*2^(n-1)*n! / (n^2*H_{n-1}(x_i)^2).
// The rule is symmetric about 0: only the upper half is solved and the lower
// half is mirrored (the odd order used here also has an exact middle node at
// x=0, taken directly rather than iterated). The rule is computed once, at
// first use, and cached in a function-local static (a "magic static" -- the
// C++11 memory model guarantees the one-time init races resolve safely with
// no explicit lock), so no call after the first performs any work beyond a
// static-local access.
//
// A lognormal random variable W with E[W] = m and log-stdev s is represented
// in the standard forward-measure parametrization
//   W = m * exp(s*sqrt(2)*x - s^2/2),  x ~ Gauss-Hermite (weight exp(-x^2)),
// so lognormal_expect(m, s, g) approximates E[g(W)] as the corresponding
// weighted sum. lognormal_call and lognormal_sqrt_moment are the closed-form
// (Black-76-style) identities for E[(W-k)+] and E[sqrt(W)] respectively --
// used both as engine primitives (so the price of a vol/var derivative need
// not always pay for quadrature) and as independent accuracy oracles for
// lognormal_expect in tests.
//
// ── Kinked payoffs: use lognormal_truncated_expect, not lognormal_expect ──
// Gauss-type quadrature (Hermite here) is only spectrally accurate for
// smooth/entire integrands, so lognormal_expect loses its near-machine-
// precision convergence on a KINKED payoff (e.g. a call's max(W-k,0)) and can
// be off by percent-level error even at this order; split the domain at the
// kink (in standard-normal z units) and evaluate each smooth piece with
// lognormal_truncated_expect instead, which uses a separate cached
// Gauss-Legendre rule (kGlOrder-point, on [-1,1], same Newton-on-recurrence
// construction as gh_rule but for the Legendre recurrence
// P_{n+1} = ((2n+1)*x*P_n - n*P_{n-1})/(n+1), weight w = 2/((1-x^2)*P_n'(x)^2))
// mapped onto [z_lo, z_hi] with the explicit normal density as part of the
// integrand -- exact for polynomials up to degree 2*kGlOrder-1 and near-exact
// for the smooth lognormal/payoff product once the kink is excluded.

#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>

namespace atx::vol::detail {

// Fixed-order Gauss-Hermite rule (physicists' weight exp(-x^2)). Nodes are
// computed once at first use (Newton on the Hermite recurrence) and cached in
// a function-local static array -- no per-call allocation.
inline constexpr std::size_t kGhOrder = 21;

struct GhRule {
  std::array<double, kGhOrder> x;
  std::array<double, kGhOrder> w;
};

// The order-kGhOrder Gauss-Hermite rule. Thread-safe (magic static); every
// call after the first is a plain reference return, no allocation, no lock.
[[nodiscard]] const GhRule &gh_rule() noexcept;

// Fixed-order (64-point) Gauss-Legendre rule on [-1,1]. Cached the same way
// as gh_rule() (Newton on the Legendre recurrence, magic-static cache, no
// per-call allocation). Backs lognormal_truncated_expect below.
inline constexpr std::size_t kGlOrder = 64;

struct GlRule {
  std::array<double, kGlOrder> x;
  std::array<double, kGlOrder> w;
};

[[nodiscard]] const GlRule &gl_rule() noexcept;

// Standard-normal CDF, Phi(x) = 0.5*erfc(-x/sqrt(2)).
[[nodiscard]] double norm_cdf(double x) noexcept;

// E[sqrt(W)] for W lognormal with mean m and log-stdev s: sqrt(m)*exp(-s^2/8).
// Precondition: m >= 0 (a lognormal mean).
[[nodiscard]] double lognormal_sqrt_moment(double m, double s) noexcept;

// E[(W-k)+] for W lognormal with mean m and log-stdev s, closed form
// (Black-76-style call). Degenerate edges: k <= 0 -> m - k (exercise is
// certain, no optionality); s <= 0 -> max(m-k, 0) (W is deterministic == m).
[[nodiscard]] double lognormal_call(double m, double s, double k) noexcept;

// E[g(W)] for W lognormal with mean m and log-stdev s, by Gauss-Hermite
// quadrature: evaluates g(m*exp(s*sqrt(2)*x_i - s^2/2)) at each rule node,
// weighted by w_i/sqrt(pi). s <= 0 collapses to g(m) exactly (W is
// deterministic, so no quadrature is needed or well-defined).
//
// noexcept contract: this leaf is unconditionally noexcept, matching the
// rest of this header; g must not throw. If it does, std::terminate is
// invoked -- a documented, loud failure rather than a silently-corrupted
// partial sum. Callers pass arithmetic-only lambdas (the sole use case this
// header exists for).
template <class G>
  requires std::invocable<G, double>
[[nodiscard]] double lognormal_expect(double m, double s, G &&g) noexcept {
  if (s <= 0.0) {
    return static_cast<double>(g(m));
  }
  const GhRule &rule = gh_rule();
  constexpr double kSqrt2 = 1.4142135623730950488016887242097;
  constexpr double kSqrtPi = 1.7724538509055160272981674833411;
  const double half_s_sq = 0.5 * s * s;
  double sum = 0.0;
  // Bound is kGhOrder (21), a compile-time constant -- statically obvious.
  for (std::size_t i = 0; i < rule.x.size(); ++i) {
    const double w_i = m * std::exp(s * kSqrt2 * rule.x[i] - half_s_sq);
    sum += rule.w[i] * static_cast<double>(g(w_i));
  }
  return sum / kSqrtPi;
}

// E[g(W) * 1{z_lo < z <= z_hi}] where W = m*exp(s*z - s^2/2), z ~ N(0,1), by
// fixed-order Gauss-Legendre quadrature in z with the explicit normal density
// folded into the integrand. Bounds are in STANDARD-NORMAL z units and are
// clamped to [-8, 8] (tail mass beyond +-8 sigma is ~1e-15, below where it
// could move any of this header's tolerances). The caller is responsible for
// choosing [z_lo, z_hi] so g is smooth on the interval -- split at a kink's z
// location (e.g. z* solving W(z*) = k) rather than calling this across one.
//
// Precondition: s > 0 (W must be genuinely random for "smooth on an
// interval" to be meaningful; s <= 0 collapses W to a point mass, which
// lognormal_expect already handles). Debug-asserted, not runtime-checked --
// this is a leaf-math noexcept primitive, callers guard the precondition.
// z_hi <= z_lo (after clamping) is treated as an empty interval -> 0.0.
template <class G>
  requires std::invocable<G, double>
[[nodiscard]] double lognormal_truncated_expect(double m, double s, double z_lo, double z_hi,
                                                G &&g) noexcept {
  assert(s > 0.0 && "lognormal_truncated_expect requires s > 0 (W must be non-degenerate)");
  const double lo = z_lo < -8.0 ? -8.0 : z_lo;
  const double hi = z_hi > 8.0 ? 8.0 : z_hi;
  if (hi <= lo) {
    return 0.0;
  }
  const GlRule &rule = gl_rule();
  constexpr double kInvSqrt2Pi = 0.398942280401432677939946059934381868;
  const double half_width = 0.5 * (hi - lo);
  const double mid = 0.5 * (hi + lo);
  const double half_s_sq = 0.5 * s * s;
  double sum = 0.0;
  // Bound is kGlOrder (64), a compile-time constant -- statically obvious.
  for (std::size_t i = 0; i < rule.x.size(); ++i) {
    const double z = mid + half_width * rule.x[i];
    const double phi = kInvSqrt2Pi * std::exp(-0.5 * z * z);
    const double w_val = m * std::exp(s * z - half_s_sq);
    sum += rule.w[i] * phi * static_cast<double>(g(w_val));
  }
  return sum * half_width;
}

} // namespace atx::vol::detail
