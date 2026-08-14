// Gauss-Hermite quadrature + lognormal RV distribution kernel -- see
// atx/vol/detail/rv_lognormal.hpp for the numerical basis and contracts.

#include "atx/vol/detail/rv_lognormal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "atx/core/math.hpp" // atx::core::norm_cdf<double> -- the shared Phi primitive

namespace atx::vol::detail {

namespace {

// H_n(x) and H_{n-1}(x) from one pass of the recurrence
// H_{j+1} = 2x*H_j - 2j*H_{j-1}, seeded H_0=1, H_1=2x. Newton's step on H_n
// needs both the value and the derivative H_n' = 2n*H_{n-1}, so returning the
// pair avoids a second recurrence pass per iteration.
struct HermitePair {
  double hn;
  double hn_minus1;
};

// n is always kGhOrder or kGhOrder-1 here (a small, compile-time-fixed
// constant): the loop bound is statically obvious despite being a runtime
// parameter.
[[nodiscard]] HermitePair hermite_pair(std::size_t n, double x) noexcept {
  double h_prev = 1.0;    // H_0(x)
  double h_curr = 2.0 * x; // H_1(x)
  for (std::size_t j = 1; j < n; ++j) {
    const double h_next = 2.0 * x * h_curr - 2.0 * static_cast<double>(j) * h_prev;
    h_prev = h_curr;
    h_curr = h_next;
  }
  return {h_curr, h_prev};
}

// Newton root of H_n(x) starting from `guess`. Converges to machine
// precision in a handful of iterations for the order used here; kMaxIt
// bounds the loop against a pathological seed rather than expecting to spend
// it (every seed below is within Newton's basin of convergence for n=21).
[[nodiscard]] double newton_hermite_root(std::size_t n, double guess) noexcept {
  constexpr int kMaxIt = 100;
  constexpr double kTol = 1e-15;
  double z = guess;
  for (int it = 0; it < kMaxIt; ++it) {
    const HermitePair hp = hermite_pair(n, z);
    const double deriv = 2.0 * static_cast<double>(n) * hp.hn_minus1;
    const double dz = hp.hn / deriv;
    z -= dz;
    if (std::fabs(dz) <= kTol * (std::fabs(z) + kTol)) {
      break;
    }
  }
  return z;
}

[[nodiscard]] GhRule compute_gh_rule() noexcept {
  GhRule rule{};
  constexpr std::size_t n = kGhOrder;
  constexpr std::size_t m = (n + 1) / 2; // upper half incl. the odd-order middle node

  // n! and 2^(n-1): both comfortably double-representable for n=21 (20! ~=
  // 2.43e18, 2^20 ~= 1.05e6 -> product ~2.5e24, far under DBL_MAX ~1.8e308;
  // the weight denominator n^2*H_{n-1}(x_i)^2 stays finite too -- the extreme
  // node's H_20 is order 1e19-1e20, squared ~1e38-1e40, still nowhere near
  // overflow at this fixed order).
  double fact_n = 1.0;
  for (std::size_t j = 2; j <= n; ++j) {
    fact_n *= static_cast<double>(j);
  }
  double two_pow_nm1 = 1.0;
  for (std::size_t j = 1; j < n; ++j) {
    two_pow_nm1 *= 2.0;
  }
  const double sqrt_pi = std::sqrt(std::acos(-1.0));
  const double weight_num = sqrt_pi * two_pow_nm1 * fact_n;
  const double n_sq = static_cast<double>(n) * static_cast<double>(n);

  std::array<double, m> upper{}; // upper[i]: i-th positive/zero root, largest first

  for (std::size_t i = 0; i < m; ++i) {
    const bool is_middle = (n % 2 == 1) && (i == m - 1);
    double z;
    if (is_middle) {
      z = 0.0; // exact by symmetry for odd order -- Newton would only add noise
    } else {
      // Seed via the classic asymptotic extrapolation (Numerical Recipes'
      // gauher): the edge root from the Hermite-zero asymptotic, subsequent
      // roots extrapolated from the two previously converged roots. Only used
      // to seed Newton, so approximate accuracy is enough.
      double guess;
      if (i == 0) {
        const double two_n_plus_1 = 2.0 * static_cast<double>(n) + 1.0;
        guess = std::sqrt(two_n_plus_1) - 1.85575 * std::pow(two_n_plus_1, -1.0 / 6.0);
      } else if (i == 1) {
        guess = upper[0] - 1.14 * std::pow(static_cast<double>(n), 0.426) / upper[0];
      } else if (i == 2) {
        guess = 1.86 * upper[1] - 0.86 * upper[0];
      } else if (i == 3) {
        guess = 1.91 * upper[2] - 0.91 * upper[1];
      } else {
        guess = 2.0 * upper[i - 1] - upper[i - 2];
      }
      z = newton_hermite_root(n, guess);
    }
    upper[i] = z;

    const double hn_minus1 = hermite_pair(n, z).hn_minus1;
    const double w_i = weight_num / (n_sq * hn_minus1 * hn_minus1);

    rule.x[i] = z;
    rule.w[i] = w_i;
    rule.x[n - 1 - i] = -z;
    rule.w[n - 1 - i] = w_i;
  }
  return rule;
}

// Legendre P_n(x) and P_{n-1}(x) from one pass of the recurrence
// P_{j+1} = ((2j+1)*x*P_j - j*P_{j-1})/(j+1), seeded P_0=1, P_1=x. Mirrors
// hermite_pair's shape: Newton's step on P_n needs both the value and (via
// the standard identity below) the derivative.
struct LegendrePair {
  double pn;
  double pn_minus1;
};

[[nodiscard]] LegendrePair legendre_pair(std::size_t n, double x) noexcept {
  double p_prev = 1.0; // P_0(x)
  double p_curr = x;   // P_1(x)
  for (std::size_t j = 1; j < n; ++j) {
    const double jd = static_cast<double>(j);
    const double p_next = ((2.0 * jd + 1.0) * x * p_curr - jd * p_prev) / (jd + 1.0);
    p_prev = p_curr;
    p_curr = p_next;
  }
  return {p_curr, p_prev};
}

// P_n'(x) via the standard identity (1-x^2)*P_n'(x) = n*(P_{n-1}(x) - x*P_n(x)).
// Only ever evaluated at interior roots (all Legendre roots lie strictly
// inside (-1,1)), so the (x^2-1) denominator never sees x = +-1.
[[nodiscard]] double legendre_deriv(std::size_t n, double x, const LegendrePair &lp) noexcept {
  return static_cast<double>(n) * (x * lp.pn - lp.pn_minus1) / (x * x - 1.0);
}

// Newton root of P_n(x) starting from `guess`; same bounded-iteration shape
// as newton_hermite_root.
[[nodiscard]] double newton_legendre_root(std::size_t n, double guess) noexcept {
  constexpr int kMaxIt = 100;
  constexpr double kTol = 1e-15;
  double z = guess;
  for (int it = 0; it < kMaxIt; ++it) {
    const LegendrePair lp = legendre_pair(n, z);
    const double deriv = legendre_deriv(n, z, lp);
    const double dz = lp.pn / deriv;
    z -= dz;
    if (std::fabs(dz) <= kTol * (std::fabs(z) + kTol)) {
      break;
    }
  }
  return z;
}

[[nodiscard]] GlRule compute_gl_rule() noexcept {
  GlRule rule{};
  constexpr std::size_t n = kGlOrder;
  constexpr std::size_t m = (n + 1) / 2; // upper half; odd order would add an exact x=0 root
  const double pi = std::acos(-1.0);

  std::array<double, m> upper{}; // upper[i]: i-th positive/zero root, largest first

  for (std::size_t i = 0; i < m; ++i) {
    const bool is_middle = (n % 2 == 1) && (i == m - 1);
    double z;
    if (is_middle) {
      z = 0.0; // exact by symmetry for odd order (Legendre polys of odd degree are odd functions)
    } else {
      // Classic Chebyshev-node seed for the i-th Legendre root (Numerical
      // Recipes' gauleg); only used to seed Newton.
      const double id = static_cast<double>(i);
      const double nd = static_cast<double>(n);
      const double guess = std::cos(pi * (id + 0.75) / (nd + 0.5));
      z = newton_legendre_root(n, guess);
    }
    upper[i] = z;

    const LegendrePair lp = legendre_pair(n, z);
    const double deriv = legendre_deriv(n, z, lp);
    const double w_i = 2.0 / ((1.0 - z * z) * deriv * deriv);

    rule.x[i] = z;
    rule.w[i] = w_i;
    rule.x[n - 1 - i] = -z;
    rule.w[n - 1 - i] = w_i;
  }
  return rule;
}

} // namespace

const GhRule &gh_rule() noexcept {
  static const GhRule rule = compute_gh_rule(); // magic static: thread-safe one-time init
  return rule;
}

const GlRule &gl_rule() noexcept {
  static const GlRule rule = compute_gl_rule(); // magic static: thread-safe one-time init
  return rule;
}

double norm_cdf(double x) noexcept {
  // Delegates to the shared Phi primitive (0.5*erfc(-x/sqrt2)) rather than
  // duplicating the erfc-based formula -- same contract, one source of truth.
  return atx::core::norm_cdf<double>(x);
}

double lognormal_sqrt_moment(double m, double s) noexcept {
  return std::sqrt(m) * std::exp(-(s * s) / 8.0);
}

double lognormal_call(double m, double s, double k) noexcept {
  if (k <= 0.0) {
    return m - k; // exercise is certain: E[(W-k)+] = E[W]-k = m-k
  }
  if (s <= 0.0) {
    return std::max(m - k, 0.0); // W deterministic == m: intrinsic value only
  }
  // Black-76-style closed form for E[(W-k)+] with forward m and total
  // log-stdev s: d1 = (ln(m/k) + s^2/2)/s, d2 = d1 - s.
  const double d1 = (std::log(m / k) + 0.5 * s * s) / s;
  const double d2 = d1 - s;
  return m * norm_cdf(d1) - k * norm_cdf(d2);
}

double lognormal_put(double m, double s, double k) noexcept {
  if (k <= 0.0) {
    return 0.0; // W > 0 with probability one: (k-W)+ is identically 0
  }
  if (s <= 0.0) {
    return std::max(k - m, 0.0); // W deterministic == m: intrinsic value only
  }
  // The call's mirror on the SAME d1/d2, not a parity rearrangement of its
  // RESULT -- see the header for why. Parity then holds to the accuracy of
  // Phi(x) + Phi(-x) == 1 alone, which is the tightest it can be made.
  const double d1 = (std::log(m / k) + 0.5 * s * s) / s;
  const double d2 = d1 - s;
  return k * norm_cdf(-d2) - m * norm_cdf(-d1);
}

} // namespace atx::vol::detail
