#include "atx/vol/american.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include "atx/core/math.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/counters.hpp"  // ATX_VOL_COUNT (opt-in P0.2; no-op when OFF)
#include "atx/vol/greeks.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// Early-exercise regime table — single source of truth in american.hpp detail.
// Brought into atx::vol scope so every entry point (anonymous-namespace helpers
// and the public API below) resolves the classifier unqualified.
using detail::classify_regime;
using detail::ExerciseRegime;

namespace {

using atx::core::norm_cdf;
using atx::core::norm_pdf;

// clang-cl / MSVC do not define M_PI; carry the extended literal explicitly.
inline constexpr double kPi = 3.14159265358979323846;

// Hard limits — every per-solve buffer is stack-bounded (matches C ATS_AL_*).
inline constexpr std::uint16_t kAlMaxNodes = 32;
inline constexpr unsigned kAlMaxQuad = detail::kMaxQuadNodes;  // 64

// ── European legs (Black-76 reuse) ──────────────────────────────────────
//
// With F = S·e^{(r-q)T} and df = e^{-rT}, black76 reproduces the (S,K,r,q)
// European put/call the C library computed inline. T,sigma are guaranteed > 0
// at every call site here, so the degenerate black76 branch never fires.
[[nodiscard]] double euro_put_sk(double S, double K, double T, double sigma,
                                 double r, double q) noexcept {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T),
                       Side::Put);
}
[[nodiscard]] double euro_call_sk(double S, double K, double T, double sigma,
                                  double r, double q) noexcept {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T),
                       Side::Call);
}

[[nodiscard]] double d1_of(double S, double K, double r, double q, double sigma,
                           double T) noexcept {
  const double v = sigma * std::sqrt(T);
  return (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / v;
}

// ── Barone-Adesi-Whaley smooth-pasting root find ────────────────────────

[[nodiscard]] double put_residual(double Sx, double K, double T, double sigma,
                                  double r, double q, double q1) noexcept {
  const double pE = euro_put_sk(Sx, K, T, sigma, r, q);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double bit = 1.0 - std::exp(-q * T) * norm_cdf(-d1);
  return K - Sx - pE + Sx * bit / q1;
}
[[nodiscard]] double put_residual_deriv(double Sx, double K, double T,
                                        double sigma, double r, double q,
                                        double q1) noexcept {
  const double v = sigma * std::sqrt(T);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double Nm = norm_cdf(-d1);
  const double phim = norm_pdf(-d1);
  const double dq = std::exp(-q * T);
  return -1.0 + dq * Nm + (1.0 - dq * Nm) / q1 - dq * phim / (q1 * v);
}
[[nodiscard]] double call_residual(double Sx, double K, double T, double sigma,
                                   double r, double q, double q2) noexcept {
  const double cE = euro_call_sk(Sx, K, T, sigma, r, q);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double bit = 1.0 - std::exp(-q * T) * norm_cdf(d1);
  return Sx - K - cE - Sx * bit / q2;
}
[[nodiscard]] double call_residual_deriv(double Sx, double K, double T,
                                         double sigma, double r, double q,
                                         double q2) noexcept {
  const double v = sigma * std::sqrt(T);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double Np = norm_cdf(d1);
  const double phip = norm_pdf(d1);
  const double dq = std::exp(-q * T);
  return 1.0 - dq * Np - (1.0 - dq * Np) / q2 - dq * phip / (q2 * v);
}

[[nodiscard]] double newton_critical_put(double K, double T, double sigma,
                                         double r, double q, double q1,
                                         std::uint16_t max_iter, double tol) noexcept {
  double lo = 1.0e-3 * K;
  double hi = K * (1.0 - 1.0e-6);
  double Sx = K * q1 / (q1 - 1.0);
  if (!(Sx > lo && Sx < hi)) {
    Sx = 0.5 * (lo + hi);
  }
  for (std::uint16_t it = 0; it < max_iter; ++it) {
    const double f = put_residual(Sx, K, T, sigma, r, q, q1);
    const double fp = put_residual_deriv(Sx, K, T, sigma, r, q, q1);
    if (f > 0.0) {
      lo = Sx;
    } else {
      hi = Sx;
    }
    if (std::fabs(f) < tol * K) {
      return Sx;
    }
    double Sx_new = (std::fabs(fp) > 1.0e-15) ? (Sx - f / fp) : 0.5 * (lo + hi);
    if (Sx_new <= lo || Sx_new >= hi) {
      Sx_new = 0.5 * (lo + hi);
    }
    const double dS = Sx_new - Sx;
    Sx = Sx_new;
    if (std::fabs(dS) < tol * K) {
      return Sx;
    }
  }
  return Sx;
}
[[nodiscard]] double newton_critical_call(double K, double T, double sigma,
                                          double r, double q, double q2,
                                          std::uint16_t max_iter, double tol) noexcept {
  double lo = K * (1.0 + 1.0e-6);
  double hi = K * 50.0;
  double Sx = K * q2 / (q2 - 1.0);
  if (!(Sx > lo && Sx < hi)) {
    Sx = 0.5 * (lo + hi);
  }
  for (std::uint16_t it = 0; it < max_iter; ++it) {
    const double f = call_residual(Sx, K, T, sigma, r, q, q2);
    const double fp = call_residual_deriv(Sx, K, T, sigma, r, q, q2);
    if (f < 0.0) {
      lo = Sx;
    } else {
      hi = Sx;
    }
    if (std::fabs(f) < tol * K) {
      return Sx;
    }
    double Sx_new = (std::fabs(fp) > 1.0e-15) ? (Sx - f / fp) : 0.5 * (lo + hi);
    if (Sx_new <= lo || Sx_new >= hi) {
      Sx_new = 0.5 * (lo + hi);
    }
    const double dS = Sx_new - Sx;
    Sx = Sx_new;
    if (std::fabs(dS) < tol * K) {
      return Sx;
    }
  }
  return Sx;
}

// Put critical price S* — the Jacobi-Newton seed for the AL boundary. Returns
// false on degenerate input (matches the C -1 status); the several
// "no early exercise" corners return true with S* = K.
[[nodiscard]] bool baw_critical_put(double K, double T, double sigma, double r,
                                    double q, std::uint16_t max_iter, double tol,
                                    double& Sx_out) noexcept {
  if (!(K > 0.0 && T > 0.0 && sigma > 0.0)) {
    return false;
  }
  if (!(r > 0.0)) {
    Sx_out = K;
    return true;
  }
  const double sigma2 = sigma * sigma;
  const double M = 2.0 * r / sigma2;
  const double N = 2.0 * (r - q) / sigma2;
  const double h = 1.0 - std::exp(-r * T);
  if (!(h > 0.0)) {
    Sx_out = K;
    return true;
  }
  const double disc = (N - 1.0) * (N - 1.0) + 4.0 * M / h;
  if (!(disc >= 0.0)) {
    Sx_out = K;
    return true;
  }
  const double sqrt_disc = std::sqrt(disc);
  const double q1 = 0.5 * (-(N - 1.0) - sqrt_disc);
  if (!(q1 < 0.0)) {
    Sx_out = K;
    return true;
  }
  const double Sx = newton_critical_put(K, T, sigma, r, q, q1,
                                        max_iter ? max_iter : std::uint16_t{16},
                                        tol > 0.0 ? tol : 1.0e-10);
  if (!(Sx > 0.0 && Sx <= K)) {
    return false;
  }
  Sx_out = Sx;
  return true;
}

// ── P2.2b SPIKE: QD+ critical-price seed (Li 2010) ────────────────────────
//
// The QD+ approximation refines the QD/Barone-Adesi-Whaley quadratic exponent with
// the leading Li (2010) "+" correction c = (1−h)·M / (h·√disc), which vanishes as
// τ→∞ (h→1, QD/BAW recovered) and grows near expiry (h→0) where the frozen-θ QD
// approximation is worst. The corrected exponent q1⁺ = q1 + c drives the SAME
// smooth-pasting root find (put_residual with q1⁺ in place of q1), so this reuses
// newton_critical_put unchanged. This is a MEASUREMENT SPIKE: it is compared with
// the BAW seed on median JN sweep-count-to-convergence to decide ship-or-kill; it is
// NOT wired into any production solve path.
[[nodiscard]] bool qdplus_critical_put(double K, double T, double sigma, double r,
                                       double q, std::uint16_t max_iter, double tol,
                                       double& Sx_out) noexcept {
  if (!(K > 0.0 && T > 0.0 && sigma > 0.0)) {
    return false;
  }
  if (!(r > 0.0)) {
    Sx_out = K;
    return true;
  }
  const double sigma2 = sigma * sigma;
  const double M = 2.0 * r / sigma2;
  const double N = 2.0 * (r - q) / sigma2;
  const double h = 1.0 - std::exp(-r * T);
  if (!(h > 0.0)) {
    Sx_out = K;
    return true;
  }
  const double disc = (N - 1.0) * (N - 1.0) + 4.0 * M / h;
  if (!(disc >= 0.0)) {
    Sx_out = K;
    return true;
  }
  const double sqrt_disc = std::sqrt(disc);
  const double q1 = 0.5 * (-(N - 1.0) - sqrt_disc);
  // Li (2010) QD+ exponent correction (2·q1 + N − 1 == −sqrt_disc). Steepens the
  // exponent near expiry (h→0), where the frozen-θ QD approximation lifts the
  // boundary toward K.
  const double c = (1.0 - h) * M / (h * sqrt_disc);
  const double q1_plus = q1 - c;
  if (!(q1_plus < 0.0)) {
    Sx_out = K;
    return true;
  }
  const double Sx = newton_critical_put(K, T, sigma, r, q, q1_plus,
                                        max_iter ? max_iter : std::uint16_t{16},
                                        tol > 0.0 ? tol : 1.0e-10);
  if (!(Sx > 0.0 && Sx <= K)) {
    return false;
  }
  Sx_out = Sx;
  return true;
}

// ── Gauss-Legendre tables on [-1, 1] via Golub-Welsch ───────────────────

// Implicit-QL (tqli) for a symmetric tridiagonal matrix, tracking only the
// first eigenvector row z[] (Golub-Welsch needs only that for the weights).
// Numerical Recipes 11.4.3, simplified. Returns true on convergence.
[[nodiscard]] bool tqli_first_row(double* d, double* e, double* z, int n) noexcept {
  for (int i = 1; i < n; ++i) {
    e[i - 1] = e[i];
  }
  e[n - 1] = 0.0;

  for (int l = 0; l < n; ++l) {
    int iter = 0;
    int m = 0;
    do {
      for (m = l; m < n - 1; ++m) {
        const double dd = std::fabs(d[m]) + std::fabs(d[m + 1]);
        if (std::fabs(e[m]) + dd == dd) {
          break;
        }
      }
      if (m != l) {
        if (iter++ == 30) {
          return false;
        }
        double g = (d[l + 1] - d[l]) / (2.0 * e[l]);
        double r = std::hypot(g, 1.0);
        g = d[m] - d[l] + e[l] / (g + std::copysign(r, g));
        double s = 1.0;
        double c = 1.0;
        double p = 0.0;
        bool recovered = false;
        for (int i = m - 1; i >= l; --i) {
          const double f = s * e[i];
          const double b = c * e[i];
          e[i + 1] = (r = std::hypot(f, g));
          if (r == 0.0) {
            d[i + 1] -= p;
            e[m] = 0.0;
            recovered = true;
            break;
          }
          s = f / r;
          c = g / r;
          g = d[i + 1] - p;
          r = (d[i] - g) * s + 2.0 * c * b;
          p = s * r;
          d[i + 1] = g + p;
          g = c * r - b;
          const double zi = z[i + 1];
          const double zj = z[i];
          z[i + 1] = s * zj + c * zi;
          z[i] = c * zj - s * zi;
        }
        if (recovered) {
          continue;
        }
        d[l] -= p;
        e[l] = g;
        e[m] = 0.0;
      }
    } while (m != l);
  }
  return true;
}

void sort_pairs(double* d, double* z, int n) noexcept {
  for (int i = 1; i < n; ++i) {
    const double dk = d[i];
    const double zk = z[i];
    int j = i - 1;
    while (j >= 0 && d[j] > dk) {
      d[j + 1] = d[j];
      z[j + 1] = z[j];
      --j;
    }
    d[j + 1] = dk;
    z[j + 1] = zk;
  }
}

// Build the n-point Gauss-Legendre table on [-1, 1]. Jacobi matrix for monic
// Legendre: diagonal 0, sub-diagonal b[i] = i / sqrt(4 i² - 1). Eigenvalues are
// the nodes; weights are 2·z[k]² (μ₀ = 2). Returns false on non-convergence.
[[nodiscard]] bool build_gl_table(unsigned n, double* xs, double* ws) noexcept {
  if (n < 2 || n > kAlMaxQuad) {
    return false;
  }
  std::array<double, kAlMaxQuad> d{};
  std::array<double, kAlMaxQuad> e{};
  std::array<double, kAlMaxQuad> z{};

  for (unsigned i = 0; i < n; ++i) {
    d[i] = 0.0;
    z[i] = (i == 0) ? 1.0 : 0.0;
  }
  e[0] = 0.0;
  for (unsigned i = 1; i < n; ++i) {
    const double k = static_cast<double>(i);
    e[i] = k / std::sqrt(4.0 * k * k - 1.0);
  }

  if (!tqli_first_row(d.data(), e.data(), z.data(), static_cast<int>(n))) {
    return false;
  }
  sort_pairs(d.data(), z.data(), static_cast<int>(n));

  for (unsigned i = 0; i < n; ++i) {
    xs[i] = d[i];
    ws[i] = 2.0 * z[i] * z[i];
  }
  return true;
}

// Lazily-built, thread-safe cache of the six supported orders. The magic-static
// initializer runs once; the tables are read-only thereafter.
[[nodiscard]] const std::array<detail::GaussLegendre, 6>& gl_tables() {
  static const std::array<detail::GaussLegendre, 6> tables = [] {
    constexpr std::array<unsigned, 6> sizes{8u, 16u, 24u, 32u, 48u, 64u};
    std::array<detail::GaussLegendre, 6> t{};
    for (std::size_t s = 0; s < sizes.size(); ++s) {
      t[s].n = sizes[s];
      t[s].ok = build_gl_table(sizes[s], t[s].nodes.data(), t[s].weights.data());
    }
    return t;
  }();
  return tables;
}
[[nodiscard]] const detail::GaussLegendre* gl_find(unsigned n) {
  // The six supported orders are static and scheme-fixed, so resolve directly to
  // the cached table by a constant-time switch (no per-solve linear scan) — a hot
  // loop of solves binds its Gauss-Legendre tables in O(1) each (P2.2 §2). Index
  // order matches gl_tables()'s {8,16,24,32,48,64}.
  const std::array<detail::GaussLegendre, 6>& all = gl_tables();
  switch (n) {
    case 8:
      return &all[0];
    case 16:
      return &all[1];
    case 24:
      return &all[2];
    case 32:
      return &all[3];
    case 48:
      return &all[4];
    case 64:
      return &all[5];
    default:
      break;
  }
  return nullptr;
}

// ── Second-kind (Chebyshev-Lobatto) boundary interpolation ──────────────

[[nodiscard]] double al_cheb_node(unsigned i, unsigned n) noexcept {
  if (n <= 1) {
    return 0.0;
  }
  if (i == 0) {
    return -1.0;
  }
  if (i == n - 1) {
    return 1.0;
  }
  return -std::cos(kPi * static_cast<double>(i) / static_cast<double>(n - 1));
}

// Second-kind barycentric Lagrange interpolation of y[] on the Lobatto grid,
// using PRECOMPUTED nodes `z[]` and barycentric weights `w[]` (built once per
// AlBoundary in al_init_nodes). Numerically identical to recomputing the nodes
// via cos() and the (-1)^i / ½-endpoint weights inline, but hoists ~10 cos()
// calls per evaluation out of the boundary hot loop — al_boundary_at is called
// O(n_quad * n_boundary * n_sweeps) times per Andersen-Lake solve, so this is
// the dominant cold-solve cost.
//
// Templated on NB (P2.2 §3): NB==0 uses the runtime node count `n_rt`; NB>0 (a
// fixed production scheme) bakes the trip count in so clang unrolls the barycentric
// sum. The body is a SINGLE source (same ops, same order) so the specialized and
// generic instantiations are bit-identical.
template <unsigned NB>
[[nodiscard]] double al_cheb_eval_t(const double* z, const double* w,
                                    const double* y, unsigned n_rt,
                                    double zq) noexcept {
  const unsigned n = (NB != 0) ? NB : n_rt;
  if (n == 0) {
    return 0.0;
  }
  if (n == 1) {
    return y[0];
  }
  double num = 0.0;
  double den = 0.0;
  for (unsigned i = 0; i < n; ++i) {
    const double dz = zq - z[i];
    if (dz == 0.0) {
      return y[i];
    }
    const double qq = w[i] / dz;
    num += qq * y[i];
    den += qq;
  }
  return num / den;
}
[[nodiscard]] double al_cheb_eval(const double* z, const double* w,
                                  const double* y, unsigned n, double zq) noexcept {
  return al_cheb_eval_t<0>(z, w, y, n, zq);
}

// ── AL boundary state + scheme ──────────────────────────────────────────

struct AlScheme {
  std::uint16_t n_boundary = 12;
  std::uint16_t n_quad_fp = 24;
  std::uint16_t n_quad_price = 48;
  std::uint16_t n_iter_jn = 2;
  std::uint16_t n_iter_fp = 4;
  double tol = 1.0e-10;
};

struct AlBoundary {
  std::array<double, kAlMaxNodes> z{};
  std::array<double, kAlMaxNodes> wbary{};  // 2nd-kind barycentric weights (fixed)
  std::array<double, kAlMaxNodes> x{};
  std::array<double, kAlMaxNodes> tau{};
  std::array<double, kAlMaxNodes> y{};  // H(τ) values — the live state
  std::uint16_t n = 0;
  double T = 0.0;
  double K = 0.0;
  double xmax = 0.0;  // asymptotic boundary B(∞)
};

// ── P2.2 sweep-invariant geometry precompute sizing ──────────────────────
//
// eqn_b_ND's inner fixed-point quadrature recomputes, every JN+FP sweep, a block
// of quantities that depend only on (tau_j, quad-node xs_i, T, sigma, r, q) — all
// fixed for a solve. Only the two PRODUCTION fixed schemes are specialized and use
// the precompute: fast {n_boundary=7, n_quad_fp=16} and accurate/explicit-default
// {12, 24}. The generic (arbitrary AlOpts) path stays the un-hoisted scalar
// reference. So the per-solve geometry table needs only cover those (nb, nq); the
// strides carry headroom above the largest specialized scheme.
inline constexpr unsigned kGeoNodeMax = 16;    // >= max specialized n_boundary (12)
inline constexpr unsigned kGeoQuadStride = 32;  // >= max specialized n_quad_fp (24)
inline constexpr unsigned kGeoSize = kGeoNodeMax * kGeoQuadStride;  // 512 doubles

// Is (nb, nq) a specialized fixed-point scheme (compile-time trip counts + geometry
// hoist)? Kept in ONE place so the sweep dispatch, premium dispatch, and geometry
// bind agree on exactly which schemes take the hoisted path.
[[nodiscard]] constexpr bool al_fp_specialized(unsigned nb, unsigned nq) noexcept {
  return (nb == 7 && nq == 16) || (nb == 12 && nq == 24);
}

struct AlWorkspace {
  const double* qx_fp = nullptr;
  const double* qw_fp = nullptr;
  unsigned n_quad_fp = 0;
  const double* qx_price = nullptr;
  const double* qw_price = nullptr;
  unsigned n_quad_price = 0;
  std::array<double, kAlMaxNodes> next_y{};  // iteration scratch
  // Force the generic (runtime trip-count) kernel even for a specialized scheme —
  // the test seam behind detail::andersen_lake_generic_kernel. Default true so
  // every production solve specializes.
  bool specialize = true;
  // Sweep-invariant geometry for eqn_b_ND, filled ONCE per solve by
  // al_bind_geometry when the scheme is specialized, indexed [j*kGeoQuadStride + i]
  // for collocation node j (1..n-1) and fixed-point quad node i. NOT zero-init'd
  // (filled before read; only the active (j,i) are read) so the greeks paths that
  // stack several workspaces do not pay a 512-double memset each.
  std::array<double, kGeoSize> geo_zc;    // clamped Chebyshev argument z_ji
  std::array<double, kGeoSize> geo_v;     // sigma * sqrt(t_u_ji)
  std::array<double, kGeoSize> geo_weru;  // qw_fp[i] * exp(r * u_ji)
  std::array<double, kGeoSize> geo_wequ;  // qw_fp[i] * exp(q * u_ji)
};

// ACCURATE preset when opts == nullopt; otherwise map the public knobs.
[[nodiscard]] AlScheme scheme_from_opts(const std::optional<AlOpts>& opts) noexcept {
  AlScheme s;  // {12, 24, 48, 2, 4, 1e-10}
  if (!opts) {
    return s;
  }
  const AlOpts& o = *opts;
  if (o.n_collocation >= 6 && o.n_collocation <= kAlMaxNodes) {
    s.n_boundary = o.n_collocation;
  }
  const unsigned n = o.n_quadrature;
  if (n >= 64) {
    s.n_quad_fp = 64;
  } else if (n >= 48) {
    s.n_quad_fp = 48;
  } else if (n >= 32) {
    s.n_quad_fp = 32;
  } else if (n >= 24) {
    s.n_quad_fp = 24;
  } else if (n >= 16) {
    s.n_quad_fp = 16;
  } else if (n >= 8) {
    s.n_quad_fp = 8;
  }
  // Premium integral uses the SAME Gauss-Legendre order as the fixed-point
  // integral. The nullopt (ACCURATE) path returned above keeps its 48-node
  // premium quad; an explicit AlOpts drives both integrals off n_quadrature, so
  // a fast preset can genuinely lower the dominant premium-quad cost.
  s.n_quad_price = s.n_quad_fp;
  if (o.max_newton_iter > 0) {
    const std::uint16_t total = o.max_newton_iter;
    s.n_iter_jn = (total >= 2) ? std::uint16_t{2} : std::uint16_t{1};
    s.n_iter_fp = (total > s.n_iter_jn) ? static_cast<std::uint16_t>(total - s.n_iter_jn)
                                        : std::uint16_t{0};
  }
  if (o.tol > 0.0) {
    s.tol = o.tol;
  }
  return s;
}

[[nodiscard]] double al_xmax_put(double K, double r, double q) noexcept {
  if (r > 0.0 && q > 0.0) {
    return K * std::fmin(1.0, r / q);
  }
  if (r > 0.0) {
    return K;
  }
  if (r == 0.0 && q < 0.0) {
    return K;
  }
  if (r == 0.0) {
    return 0.0;
  }
  if (r < 0.0 && q >= 0.0) {
    return 0.0;
  }
  if (r < 0.0 && q < r) {
    return K;
  }
  return 0.0;
}

[[nodiscard]] double y_from_b(double b_val, double xmax) noexcept {
  if (b_val <= 0.0 || xmax <= 0.0) {
    return 0.0;
  }
  const double lg = std::log(b_val / xmax);
  return lg * lg;
}
[[nodiscard]] double b_from_y(double y_val, double xmax) noexcept {
  const double yv = (y_val > 0.0) ? y_val : 0.0;
  return xmax * std::exp(-std::sqrt(yv));
}

[[nodiscard]] double d_plus(double t, double z, double sigma, double r, double q) noexcept {
  const double v = sigma * std::sqrt(t);
  return (std::log(z) + (r - q) * t) / v + 0.5 * v;
}
[[nodiscard]] double d_minus(double t, double z, double sigma, double r, double q) noexcept {
  const double v = sigma * std::sqrt(t);
  return (std::log(z) + (r - q) * t) / v - 0.5 * v;
}

void al_init_nodes(AlBoundary& b, std::uint16_t n, double T, double K, double r,
                   double q) noexcept {
  b.n = n;
  b.T = T;
  b.K = K;
  b.xmax = al_xmax_put(K, r, q);
  const double sqrt_T_half = 0.5 * std::sqrt(T);
  for (std::uint16_t i = 0; i < n; ++i) {
    b.z[i] = al_cheb_node(i, n);
    // 2nd-kind (Chebyshev-Lobatto) barycentric weight: (-1)^i, halved at the
    // two endpoints. Fixed by node index alone, so precomputed once here.
    double w = (i & 1u) ? -1.0 : 1.0;
    if (i == 0 || i + 1 == n) {
      w *= 0.5;
    }
    b.wbary[i] = w;
    b.x[i] = sqrt_T_half * (1.0 + b.z[i]);
    b.tau[i] = b.x[i] * b.x[i];
    b.y[i] = 0.0;
  }
}

[[nodiscard]] double al_boundary_at(const AlBoundary& b, double u) noexcept {
  if (b.T <= 0.0) {
    return b.xmax;
  }
  if (u <= 0.0) {
    return b.xmax;
  }
  const double u_eff = (u >= b.T) ? b.T : u;
  const double z = 2.0 * std::sqrt(u_eff / b.T) - 1.0;
  const double zc = atx::core::clamp(z, -1.0, 1.0);
  const double y_val =
      al_cheb_eval(b.z.data(), b.wbary.data(), b.y.data(), b.n, zc);
  return b_from_y(y_val, b.xmax);
}

void al_seed_boundary(AlBoundary& b, double sigma, double r, double q) noexcept {
  ATX_VOL_COUNT(BoundarySolves);  // one cold boundary seed (BAW re-seed per node)
  b.y[0] = 0.0;
  for (std::uint16_t i = 1; i < b.n; ++i) {
    const double tau_i = b.tau[i];
    if (tau_i <= 1.0e-14) {
      b.y[i] = 0.0;
      continue;
    }
    double Sx = 0.0;
    const bool ok = baw_critical_put(b.K, tau_i, sigma, r, q, 16, 1.0e-10, Sx);
    if (!ok || !(Sx > 0.0)) {
      const double frac = std::sqrt(tau_i / b.T);
      Sx = b.K * (1.0 - 0.3 * frac);
    }
    if (Sx > b.xmax) {
      Sx = b.xmax;
    }
    if (!(Sx > 0.0)) {
      Sx = 1.0e-6 * b.K;
    }
    b.y[i] = y_from_b(Sx, b.xmax);
  }
}

// P2.2b spike: identical to al_seed_boundary but seeds each node from the QD+
// critical price instead of BAW. Measurement-only (not on a production path).
void al_seed_boundary_qdplus(AlBoundary& b, double sigma, double r, double q) noexcept {
  b.y[0] = 0.0;
  for (std::uint16_t i = 1; i < b.n; ++i) {
    const double tau_i = b.tau[i];
    if (tau_i <= 1.0e-14) {
      b.y[i] = 0.0;
      continue;
    }
    double Sx = 0.0;
    const bool ok = qdplus_critical_put(b.K, tau_i, sigma, r, q, 16, 1.0e-10, Sx);
    if (!ok || !(Sx > 0.0)) {
      const double frac = std::sqrt(tau_i / b.T);
      Sx = b.K * (1.0 - 0.3 * frac);
    }
    if (Sx > b.xmax) {
      Sx = b.xmax;
    }
    if (!(Sx > 0.0)) {
      Sx = 1.0e-6 * b.K;
    }
    b.y[i] = y_from_b(Sx, b.xmax);
  }
}

// ── P2.2: per-solve precompute of the sweep-invariant quadrature geometry ──
//
// eqn_b_ND's inner loop, for each collocation node j and fixed-point quad node i,
// forms u_ji = half_tau_j·(1+xs_i), t_u_ji = tau_j − u_ji, the al_boundary_at
// argument transform z_ji = clamp(2·sqrt(u_ji/T) − 1, −1, 1), v_ji = sigma·sqrt(t_u),
// and exp(r·u_ji)/exp(q·u_ji). Every one of these depends only on quantities fixed
// for the whole solve (tau_j, xs_i, T, sigma, r, q) — but the loop recomputes them,
// including 2 sqrt + 2 exp, on EVERY Jacobi-Newton and fixed-point sweep. Only the
// Chebyshev value over the sweep-varying bnd.y[] and b_from_y actually change.
//
// This fills them ONCE (weights folded into geo_weru/geo_wequ so the sweep loop is a
// bare multiply-accumulate). It is a PURE HOIST: each stored quantity is computed
// with the identical expression the loop used, so the specialized sweep is
// bit-identical to the generic reference (which still recomputes inline). Only the
// specialized fixed schemes are hoisted; the generic path leaves geo untouched.
void al_bind_geometry(const AlBoundary& bnd, AlWorkspace& ws, double sigma,
                      double r, double q) noexcept {
  if (!ws.specialize || !al_fp_specialized(bnd.n, ws.n_quad_fp)) {
    return;  // generic path recomputes inline; no geometry needed
  }
  const double* xs = ws.qx_fp;
  const double* wv = ws.qw_fp;
  const unsigned nq = ws.n_quad_fp;
  const double T = bnd.T;
  for (std::uint16_t j = 1; j < bnd.n; ++j) {
    const double tau = bnd.tau[j];
    if (tau <= 1.0e-14) {
      continue;  // sweeps skip this node entirely
    }
    const double half_tau = 0.5 * tau;
    const unsigned gbase = static_cast<unsigned>(j) * kGeoQuadStride;
    for (unsigned i = 0; i < nq; ++i) {
      const double u = half_tau * (1.0 + xs[i]);
      const double t_u = tau - u;
      if (t_u <= 1.0e-14) {
        continue;  // inactive node; the loop re-checks t_u and skips it
      }
      // al_boundary_at's z transform (u < T here, so u_eff == u — replicated so the
      // stored zc is bit-identical to the inline computation).
      const double u_eff = (u >= T) ? T : u;
      const double zz = 2.0 * std::sqrt(u_eff / T) - 1.0;
      ws.geo_zc[gbase + i] = atx::core::clamp(zz, -1.0, 1.0);
      ws.geo_v[gbase + i] = sigma * std::sqrt(t_u);
      ws.geo_weru[gbase + i] = wv[i] * std::exp(r * u);
      ws.geo_wequ[gbase + i] = wv[i] * std::exp(q * u);
      ATX_VOL_COUNT_N(ExpCalls, 2);  // exp(r·u), exp(q·u) — now paid ONCE per solve
    }
  }
}

// Equation B kernel: N(τ,b), D(τ,b). Templated on the fixed-scheme trip counts
// <NB=n_boundary, NQ=n_quad_fp> (P2.2 §3); NB==0 && NQ==0 is the generic runtime
// path AND the un-hoisted scalar reference (recomputes the geometry inline exactly
// as before this task). NB>0 reads the al_bind_geometry precompute — a pure hoist,
// so the two paths are bit-identical.
template <unsigned NB, unsigned NQ>
void eqn_b_ND_impl(const AlBoundary& bnd, const AlWorkspace& ws,
                   unsigned node_idx, double tau, double b_val, double sigma,
                   double r, double q, double& N_out, double& D_out) noexcept {
  const double K = bnd.K;
  if (tau <= 1.0e-14) {
    if (b_val < K) {
      N_out = 0.0;
      D_out = 0.0;
    } else if (b_val > K) {
      N_out = 1.0;
      D_out = 1.0;
    } else {
      N_out = 0.5;
      D_out = 0.5;
    }
    return;
  }
  const double tip_p = norm_cdf(d_plus(tau, b_val / K, sigma, r, q));
  const double tip_m = norm_cdf(d_minus(tau, b_val / K, sigma, r, q));
  ATX_VOL_COUNT_N(NormCdfCalls, 2);  // tip_p, tip_m

  const double* xs = ws.qx_fp;
  const unsigned nq = (NQ != 0) ? NQ : ws.n_quad_fp;
  double n_int = 0.0;
  double d_int = 0.0;
  const double half_tau = 0.5 * tau;
  if constexpr (NB != 0) {
    // Specialized: read the sweep-invariant geometry; evaluate only the
    // sweep-VARYING Chebyshev value + b_from_y in the loop.
    const unsigned gbase = node_idx * kGeoQuadStride;
    const double rq = r - q;
    for (unsigned i = 0; i < nq; ++i) {
      const double u = half_tau * (1.0 + xs[i]);
      const double t_u = tau - u;
      if (t_u <= 1.0e-14) {
        continue;
      }
      const double y_val = al_cheb_eval_t<NB>(bnd.z.data(), bnd.wbary.data(),
                                              bnd.y.data(), bnd.n,
                                              ws.geo_zc[gbase + i]);
      const double bu = b_from_y(y_val, bnd.xmax);
      if (!(bu > 0.0)) {
        continue;
      }
      const double z = b_val / bu;
      const double v = ws.geo_v[gbase + i];
      const double base = (std::log(z) + rq * t_u) / v;
      const double dpv = base + 0.5 * v;
      const double dmv = base - 0.5 * v;
      n_int += ws.geo_weru[gbase + i] * norm_cdf(dmv);
      d_int += ws.geo_wequ[gbase + i] * norm_cdf(dpv);
      ATX_VOL_COUNT(LogCalls);
      ATX_VOL_COUNT_N(NormCdfCalls, 2);
    }
  } else {
    // Generic reference: recompute the geometry inline (the scalar cold path).
    const double* wv = ws.qw_fp;
    // Transcendental-bound inner loop (2×sqrt, log, 2×exp, 2×norm_cdf per point),
    // run n_quad·n_boundary·n_sweeps times per solve — the dominant cold cost.
    // Two AVX2 vectorizations were built and MEASURED here and both reverted:
    //   * xsimd (portable): ~6.6x SLOWER — its polynomial exp/log/erfc are far
    //     heavier than the SVML-backed scalar libm the loop already calls.
    //   * Intel SVML intrinsics (_mm256_exp/log/cdfnorm_pd): compile only under
    //     MSVC cl.exe, not the project's clang-cl toolchain, which exposes no SVML.
    // A profitable vectorization would need clang `-fveclib=SVML` (adds a fragile
    // Intel SVML runtime-DLL dependency to the whole library) or a batch-across-
    // options SoA solver. Neither is warranted; the scalar loop is the keeper.
    for (unsigned i = 0; i < nq; ++i) {
      const double u = half_tau * (1.0 + xs[i]);
      const double t_u = tau - u;
      if (t_u <= 1.0e-14) {
        continue;
      }
      const double bu = al_boundary_at(bnd, u);
      if (!(bu > 0.0)) {
        continue;
      }
      const double z = b_val / bu;
      // d_minus == d_plus - v, so share the log(z) and sqrt(t_u) between them.
      const double v = sigma * std::sqrt(t_u);
      const double base = (std::log(z) + (r - q) * t_u) / v;
      const double dpv = base + 0.5 * v;
      const double dmv = base - 0.5 * v;
      n_int += wv[i] * std::exp(r * u) * norm_cdf(dmv);
      d_int += wv[i] * std::exp(q * u) * norm_cdf(dpv);
      ATX_VOL_COUNT(LogCalls);
      ATX_VOL_COUNT_N(ExpCalls, 2);
      ATX_VOL_COUNT_N(NormCdfCalls, 2);
    }
  }
  n_int *= half_tau;
  d_int *= half_tau;

  N_out = tip_m + r * n_int;
  D_out = tip_p + q * d_int;
}

// ∂N/∂b, ∂D/∂b at fixed kernel.
void eqn_b_NDd(const AlBoundary& bnd, double tau, double b_val, double sigma,
               double r, double q, double& Nd_out, double& Dd_out) noexcept {
  if (tau <= 1.0e-14 || !(b_val > 0.0)) {
    Nd_out = 0.0;
    Dd_out = 0.0;
    return;
  }
  const double K = bnd.K;
  const double v = sigma * std::sqrt(tau);
  const double dpv = d_plus(tau, b_val / K, sigma, r, q);
  const double dmv = d_minus(tau, b_val / K, sigma, r, q);
  Nd_out = norm_pdf(dmv) / (b_val * v);
  Dd_out = norm_pdf(dpv) / (b_val * v);
}

template <unsigned NB, unsigned NQ>
[[nodiscard]] double al_jn_sweep_impl(AlBoundary& b, AlWorkspace& ws,
                                      double sigma, double r, double q) noexcept {
  const unsigned n = (NB != 0) ? NB : b.n;
  double max_dy = 0.0;
  ws.next_y[0] = 0.0;
  for (unsigned i = 1; i < n; ++i) {
    const double tau = b.tau[i];
    if (tau <= 1.0e-14) {
      ws.next_y[i] = 0.0;
      continue;
    }
    const double b_val = b_from_y(b.y[i], b.xmax);
    double Nv = 0.0;
    double Dv = 0.0;
    eqn_b_ND_impl<NB, NQ>(b, ws, i, tau, b_val, sigma, r, q, Nv, Dv);
    if (!(Dv > 1.0e-300)) {
      ws.next_y[i] = b.y[i];
      continue;
    }
    const double alpha = b.K * std::exp(-(r - q) * tau);
    const double f = alpha * Nv / Dv;

    double Nd = 0.0;
    double Dd = 0.0;
    eqn_b_NDd(b, tau, b_val, sigma, r, q, Nd, Dd);
    const double fprime = alpha * (Nd / Dv - Dd * Nv / (Dv * Dv));

    const double denom = fprime - 1.0;
    double b_new = (std::fabs(denom) > 1.0e-12) ? (b_val - (f - b_val) / denom) : f;
    if (b_new > b.xmax) {
      b_new = b.xmax;
    }
    if (!(b_new > 0.0)) {
      b_new = 1.0e-6 * b.K;
    }
    const double y_new = y_from_b(b_new, b.xmax);
    const double dy = std::fabs(y_new - b.y[i]);
    if (dy > max_dy) {
      max_dy = dy;
    }
    ws.next_y[i] = y_new;
  }
  for (unsigned i = 0; i < n; ++i) {
    b.y[i] = ws.next_y[i];
  }
  return max_dy;
}

// Dispatch to a compile-time-trip-count instantiation for the production fixed
// schemes; the generic <0,0> is both the arbitrary-AlOpts path and the reference.
[[nodiscard]] double al_jacobi_newton_sweep(AlBoundary& b, AlWorkspace& ws,
                                            double sigma, double r, double q) noexcept {
  ATX_VOL_COUNT(JacobiNewtonSweeps);
  if (ws.specialize) {
    if (b.n == 7 && ws.n_quad_fp == 16) {
      return al_jn_sweep_impl<7, 16>(b, ws, sigma, r, q);
    }
    if (b.n == 12 && ws.n_quad_fp == 24) {
      return al_jn_sweep_impl<12, 24>(b, ws, sigma, r, q);
    }
  }
  return al_jn_sweep_impl<0, 0>(b, ws, sigma, r, q);
}

template <unsigned NB, unsigned NQ>
[[nodiscard]] double al_fp_sweep_impl(AlBoundary& b, AlWorkspace& ws,
                                      double sigma, double r, double q) noexcept {
  const unsigned n = (NB != 0) ? NB : b.n;
  double max_dy = 0.0;
  ws.next_y[0] = 0.0;
  for (unsigned i = 1; i < n; ++i) {
    const double tau = b.tau[i];
    if (tau <= 1.0e-14) {
      ws.next_y[i] = 0.0;
      continue;
    }
    const double b_val = b_from_y(b.y[i], b.xmax);
    double Nv = 0.0;
    double Dv = 0.0;
    eqn_b_ND_impl<NB, NQ>(b, ws, i, tau, b_val, sigma, r, q, Nv, Dv);
    if (!(Dv > 1.0e-300)) {
      ws.next_y[i] = b.y[i];
      continue;
    }
    const double alpha = b.K * std::exp(-(r - q) * tau);
    double b_new = alpha * Nv / Dv;
    if (b_new > b.xmax) {
      b_new = b.xmax;
    }
    if (!(b_new > 0.0)) {
      b_new = 1.0e-6 * b.K;
    }
    const double y_new = y_from_b(b_new, b.xmax);
    const double dy = std::fabs(y_new - b.y[i]);
    if (dy > max_dy) {
      max_dy = dy;
    }
    ws.next_y[i] = y_new;
  }
  for (unsigned i = 0; i < n; ++i) {
    b.y[i] = ws.next_y[i];
  }
  return max_dy;
}

[[nodiscard]] double al_fixed_point_sweep(AlBoundary& b, AlWorkspace& ws,
                                          double sigma, double r, double q) noexcept {
  ATX_VOL_COUNT(FixedPointSweeps);
  if (ws.specialize) {
    if (b.n == 7 && ws.n_quad_fp == 16) {
      return al_fp_sweep_impl<7, 16>(b, ws, sigma, r, q);
    }
    if (b.n == 12 && ws.n_quad_fp == 24) {
      return al_fp_sweep_impl<12, 24>(b, ws, sigma, r, q);
    }
  }
  return al_fp_sweep_impl<0, 0>(b, ws, sigma, r, q);
}

[[nodiscard]] double premium_integrand_put(double z, const AlBoundary& b,
                                           double S, double sigma, double r,
                                           double q) noexcept {
  const double t = z * z;
  if (t <= 1.0e-14) {
    return 0.0;
  }
  const double rem = b.T - t;
  const double b_t = (rem > 0.0) ? al_boundary_at(b, rem) : b.K;
  if (!(b_t > 0.0)) {
    return 0.0;
  }
  const double v = sigma * std::sqrt(t);
  const double dq = std::exp(-q * t);
  const double dr = std::exp(-r * t);
  const double dp = std::log(S * dq / (b_t * dr)) / v + 0.5 * v;
  ATX_VOL_COUNT(PremiumQuadEvals);
  ATX_VOL_COUNT(LogCalls);
  ATX_VOL_COUNT_N(ExpCalls, 2);
  ATX_VOL_COUNT_N(NormCdfCalls, 2);
  return 2.0 * z *
         (r * b.K * dr * norm_cdf(-dp + v) - q * S * dq * norm_cdf(-dp));
}

// Premium quadrature, templated on the fixed premium trip count NP (P2.2 §3);
// NP==0 is the generic runtime path. Single body, so bit-identical across NP.
template <unsigned NP>
[[nodiscard]] double al_put_premium_impl(const AlBoundary& b, const AlWorkspace& ws,
                                         double S, double sigma, double r,
                                         double q) noexcept {
  const double sqrtT = std::sqrt(b.T);
  const double half_sqrtT = 0.5 * sqrtT;
  double total = 0.0;
  const double* xs = ws.qx_price;
  const double* wv = ws.qw_price;
  const unsigned nq = (NP != 0) ? NP : ws.n_quad_price;
  for (unsigned i = 0; i < nq; ++i) {
    const double zi = half_sqrtT * (1.0 + xs[i]);
    total += wv[i] * premium_integrand_put(zi, b, S, sigma, r, q);
  }
  total *= half_sqrtT;
  return (total > 0.0) ? total : 0.0;
}

[[nodiscard]] double al_put_premium(const AlBoundary& b, const AlWorkspace& ws,
                                    double S, double sigma, double r,
                                    double q) noexcept {
  if (ws.specialize) {
    switch (ws.n_quad_price) {
      case 16:
        return al_put_premium_impl<16>(b, ws, S, sigma, r, q);
      case 24:
        return al_put_premium_impl<24>(b, ws, S, sigma, r, q);
      case 48:
        return al_put_premium_impl<48>(b, ws, S, sigma, r, q);
      default:
        break;
    }
  }
  return al_put_premium_impl<0>(b, ws, S, sigma, r, q);
}

// ── Boundary solve / price split (S-independence seam) ───────────────────
//
// The Andersen-Lake exercise boundary depends on (K, T, sigma, r, q) but NOT on
// the spot S — al_init_nodes / al_seed_boundary / the sweeps all ignore S; only
// al_put_premium reads it. Splitting al_solve_put here lets a greeks bundle solve
// one boundary and re-price every SPOT stencil against it (american_greeks_fd),
// collapsing the 17 solves to the 7 unique (sigma,r,T) boundaries — bit-identical.
enum class AlSolveStatus { Ok, Collapsed, TableMissing };

// S-independent: init nodes, bind quadrature, seed + iterate the boundary. On Ok,
// `bnd`/`ws` hold a converged boundary ready for al_put_price_from_boundary.
[[nodiscard]] AlSolveStatus al_solve_put_boundary(double K, double T, double sigma,
                                                  double r, double q,
                                                  const AlScheme& sch,
                                                  AlBoundary& bnd,
                                                  AlWorkspace& ws,
                                                  bool specialize = true) noexcept {
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    // Negative-rate/carry corner: AL cannot run. Flagged unsupported.
    return AlSolveStatus::Collapsed;
  }
  const detail::GaussLegendre* fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre* pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return AlSolveStatus::TableMissing;
  }
  ws.specialize = specialize;
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;
  al_bind_geometry(bnd, ws, sigma, r, q);

  al_seed_boundary(bnd, sigma, r, q);

  double resid = 1.0;
  for (std::uint16_t k = 0; k < sch.n_iter_jn; ++k) {
    resid = al_jacobi_newton_sweep(bnd, ws, sigma, r, q);
    if (resid <= sch.tol) {
      ATX_VOL_COUNT(EarlyResidualExits);
      break;
    }
  }
  if (resid > sch.tol) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      resid = al_fixed_point_sweep(bnd, ws, sigma, r, q);
      if (resid <= sch.tol) {
        ATX_VOL_COUNT(EarlyResidualExits);
        break;
      }
    }
  }
  return AlSolveStatus::Ok;
}

// Warm variant of al_solve_put_boundary: seed from an already-converged boundary a
// tiny (sigma,r,T) bump away instead of the cold Barone-Adesi-Whaley re-seed — the
// dominant cold cost (12 nested Newton root-finds). The base boundary interpolated
// onto this grid (al_boundary_at, clamped to this grid's xmax) is a far better seed
// than BAW for a ~0.1% bump, so the SAME sweep budget reconverges to the same tol.
// This handles all three bump axes uniformly: sigma/r bumps keep the tau-grid (the
// interp is ~exact), a T bump shifts it (the interp re-maps the boundary). Used by
// american_greeks_fd's warm path to skip 6 of its 7 cold seeds.
[[nodiscard]] AlSolveStatus al_solve_put_boundary_warm(double K, double T, double sigma,
                                                       double r, double q,
                                                       const AlScheme& sch,
                                                       const AlBoundary& seed,
                                                       AlBoundary& bnd,
                                                       AlWorkspace& ws) noexcept {
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    return AlSolveStatus::Collapsed;
  }
  const detail::GaussLegendre* fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre* pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return AlSolveStatus::TableMissing;
  }
  ws.specialize = true;
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;
  al_bind_geometry(bnd, ws, sigma, r, q);

  // Warm seed: base boundary evaluated at this grid's tau, clamped to this xmax.
  bnd.y[0] = 0.0;
  for (std::uint16_t i = 1; i < bnd.n; ++i) {
    const double tau_i = bnd.tau[i];
    if (tau_i <= 1.0e-14) {
      bnd.y[i] = 0.0;
      continue;
    }
    double b_seed = al_boundary_at(seed, tau_i);
    if (b_seed > bnd.xmax) {
      b_seed = bnd.xmax;
    }
    if (!(b_seed > 0.0)) {
      b_seed = 1.0e-6 * K;
    }
    bnd.y[i] = y_from_b(b_seed, bnd.xmax);
  }

  double resid = 1.0;
  for (std::uint16_t k = 0; k < sch.n_iter_jn; ++k) {
    resid = al_jacobi_newton_sweep(bnd, ws, sigma, r, q);
    if (resid <= sch.tol) {
      break;
    }
  }
  if (resid > sch.tol) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      resid = al_fixed_point_sweep(bnd, ws, sigma, r, q);
      if (resid <= sch.tol) {
        break;
      }
    }
  }
  return AlSolveStatus::Ok;
}

// Put price at spot S from a solved boundary. Assumes the caller applied the
// andersen_lake degenerate (T~0/sigma~0) and no-early-exercise (r<=0) guards, so
// only the r>0 non-degenerate arm runs here. Clamp order matches al_solve_put's
// exactly, so euro + premium + clamps is bit-identical to a full cold solve.
[[nodiscard]] double al_put_price_from_boundary(const AlBoundary& bnd,
                                                const AlWorkspace& ws, double S,
                                                double K, double T, double sigma,
                                                double r, double q) noexcept {
  const double euro = euro_put_sk(S, K, T, sigma, r, q);
  const double prem = al_put_premium(bnd, ws, S, sigma, r, q);
  double price = euro + prem;
  const double intr = K - S;
  if (intr > price) {
    price = intr;
  }
  if (euro > price) {
    price = euro;
  }
  if (price < 0.0) {
    price = 0.0;
  }
  return price;
}

// ExerciseRegime / classify_regime are defined once in american.hpp detail (the
// single source of truth for the early-exercise regime table) and used here via
// the `using` declarations above. A call delegates to al_solve_put with
// (rate=q, yield=r), so passing (r,q) for a put and (q,r) for a call covers both
// sides through one classifier.

// Shared message for the double-continuation corner the ALO scheme cannot price.
constexpr const char* kDoubleContinuationMsg =
    "double-continuation regime (put q < r <= 0 / call r < q <= 0): the "
    "single-boundary Andersen-Lake scheme cannot represent two exercise "
    "boundaries; see Andersen-Lake 2021 (double-boundary case)";

// Put core — used directly for puts and via McDonald-Schroder for calls. The
// `specialize` flag (default true) forces the generic runtime-trip-count kernel when
// false — the seam behind detail::andersen_lake_generic_kernel that proves the
// specialized fixed-scheme kernel is bit-identical to the generic path.
[[nodiscard]] Result<double> al_solve_put(double S, double K, double T,
                                          double sigma, double r, double q,
                                          const AlScheme& sch,
                                          bool specialize = true) {
  switch (classify_regime(/*rate=*/r, /*yield=*/q)) {
    case ExerciseRegime::European: {
      const double euro = euro_put_sk(S, K, T, sigma, r, q);
      const double intr = K - S;
      const double price = (euro > intr) ? euro : (intr > 0.0 ? intr : 0.0);
      return Ok(price);
    }
    case ExerciseRegime::Unsupported:
      return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
    case ExerciseRegime::American:
      break;
  }

  AlBoundary bnd;
  AlWorkspace ws;
  switch (al_solve_put_boundary(K, T, sigma, r, q, sch, bnd, ws, specialize)) {
    case AlSolveStatus::Collapsed:
      return Err(ErrorCode::NotImplemented,
                 "andersen_lake: asymptotic boundary collapsed (xmax <= 0)");
    case AlSolveStatus::TableMissing:
      return Err(ErrorCode::Internal, "andersen_lake: Gauss-Legendre table unavailable");
    case AlSolveStatus::Ok:
      break;
  }
  return Ok(al_put_price_from_boundary(bnd, ws, S, K, T, sigma, r, q));
}

// Shared core of the public andersen_lake entry point, parameterized on `specialize`
// so detail::andersen_lake_generic_kernel can force the generic runtime-trip-count
// kernel for the SAME scheme and prove the specialized path is bit-identical.
[[nodiscard]] Result<double> andersen_lake_core(double S, double K, double T,
                                                double sigma, double r, double q,
                                                Side side,
                                                const std::optional<AlOpts>& opts,
                                                bool specialize) {
  if (!(K > 0.0 && S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake: S and K must be > 0");
  }
  if (!(T >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake: T must be >= 0");
  }
  if (!(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake: sigma must be >= 0");
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake: r and q must be finite");
  }

  // Degenerate: T ~ 0 or sigma ~ 0 collapses to intrinsic.
  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    const double intr = (side == Side::Call) ? (S - K) : (K - S);
    return Ok(intr > 0.0 ? intr : 0.0);
  }

  const double rate = (side == Side::Put) ? r : q;
  const double yield = (side == Side::Put) ? q : r;
  switch (classify_regime(rate, yield)) {
    case ExerciseRegime::European:
      return Ok(black76_price(S * std::exp((r - q) * T), K, T, sigma,
                              std::exp(-r * T), side));
    case ExerciseRegime::Unsupported:
      return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
    case ExerciseRegime::American:
      break;
  }

  const AlScheme sch = scheme_from_opts(opts);
  if (side == Side::Put) {
    return al_solve_put(S, K, T, sigma, r, q, sch, specialize);
  }
  // McDonald-Schroder symmetry: C(S,K,r,q) = P(K,S,q,r). Swap (S↔K), (r↔q).
  return al_solve_put(K, S, T, sigma, q, r, sch, specialize);
}

// ── American Greeks (chain rule + FD on the correction gradient) ─────────

void american_greeks_first_order(double S, double K, double T, double sigma,
                                 double r, double q, Side side,
                                 const CorrectionCache* correction,
                                 AmericanGreeks& out) {
  const double m = std::exp((r - q) * T);  // F/S
  const double F = S * m;
  const double df = std::exp(-r * T);
  const double k_log = std::log(K / F);

  const Black76Greeks gBpk = black76_greeks(F, K, T, sigma, r, df, side);
  const Greeks gB = gBpk.greeks;
  const double euro_price = gBpk.price;

  double dc_dk = 0.0;
  double dc_dT = 0.0;
  double dc_ds = 0.0;
  double c_val = 0.0;
  if (correction) {
    c_val = correction->eval_grad(k_log, T, sigma, &dc_dk, &dc_dT, &dc_ds);
  }

  out.price = euro_price + F * c_val;

  const double D = gB.delta + c_val - dc_dk;  // ∂A/∂F
  out.delta = m * D;                          // spot-delta convention
  out.vega = gB.vega + F * dc_ds;
  out.rho = gB.rho + T * F * D;
  out.theta = gB.theta - (r - q) * F * D - F * dc_dT;

  const double hF = 1.0e-4 * F;
  const double hS = 1.0e-4;
  const double hT = 1.0e-5;

  if (correction) {
    const double F_up = F + hF;
    const double F_dn = F - hF;
    const double k_up = std::log(K / F_up);
    const double k_dn = std::log(K / F_dn);

    double dc_dk_up = 0.0;
    double dc_dk_dn = 0.0;
    const double cu = correction->eval_grad(k_up, T, sigma, &dc_dk_up, nullptr, nullptr);
    const double cd = correction->eval_grad(k_dn, T, sigma, &dc_dk_dn, nullptr, nullptr);
    const Greeks gB_up = black76_greeks(F_up, K, T, sigma, r, df, side).greeks;
    const Greeks gB_dn = black76_greeks(F_dn, K, T, sigma, r, df, side).greeks;
    const double D_up = gB_up.delta + cu - dc_dk_up;
    const double D_dn = gB_dn.delta + cd - dc_dk_dn;
    const double dD_dF = (D_up - D_dn) / (2.0 * hF);
    out.gamma = m * m * dD_dF;

    double dc_ds_up = 0.0;
    double dc_ds_dn = 0.0;
    // Value discarded here -> eval_partials skips the (unused) value sweep.
    correction->eval_partials(k_up, T, sigma, nullptr, nullptr, &dc_ds_up);
    correction->eval_partials(k_dn, T, sigma, nullptr, nullptr, &dc_ds_dn);
    const double d2c_dF_ds = (dc_ds_up - dc_ds_dn) / (2.0 * hF);
    out.vanna = m * (gB.vanna + dc_ds + F * d2c_dF_ds);

    double dc_ds_p = 0.0;
    double dc_ds_m = 0.0;
    // Value discarded here -> eval_partials skips the (unused) value sweep.
    correction->eval_partials(k_log, T, sigma + hS, nullptr, nullptr, &dc_ds_p);
    correction->eval_partials(k_log, T, sigma - hS, nullptr, nullptr, &dc_ds_m);
    const double d2c_ds2 = (dc_ds_p - dc_ds_m) / (2.0 * hS);
    out.volga = gB.volga + F * d2c_ds2;

    double dc_dk_Tp = 0.0;
    double dc_dk_Tm = 0.0;
    const double c_Tp = correction->eval_grad(k_log, T + hT, sigma, &dc_dk_Tp, nullptr, nullptr);
    const double c_Tm = correction->eval_grad(k_log, T - hT, sigma, &dc_dk_Tm, nullptr, nullptr);
    const double dDcorr_dT = ((c_Tp - dc_dk_Tp) - (c_Tm - dc_dk_Tm)) / (2.0 * hT);
    out.charm = m * (gB.charm - dDcorr_dT);
  } else {
    out.gamma = gB.gamma * (m / S) * F;
    out.vanna = gB.vanna;
    out.volga = gB.volga;
    out.charm = gB.charm;
  }
}

}  // namespace

// ── Warm-started ALO pricer (fixed contract, sigma sweep) ────────────────
//
// State mirrors al_solve_put's internals but hoists the sigma-independent setup
// (node grid, Gauss-Legendre binding) into the constructor and keeps the
// early-exercise boundary `bnd.y[]` alive between price() calls. All calls solve
// an internal PUT; a Call is the McDonald-Schroder put P(K,S,q,r), so the boundary
// machinery is identical.
struct AloPricer::State {
  double Sp{};   // internal-put spot   (= K for a call)
  double Kp{};   // internal-put strike (= S for a call) — drives the boundary
  double T{};
  double rp{};   // internal-put rate   (= q for a call)
  double qp{};   // internal-put yield  (= r for a call)
  AlScheme sch{};
  AlBoundary bnd{};
  AlWorkspace ws{};
  bool prepared{false};       // node grid + quadrature bound, xmax > 0
  bool european_only{false};  // no early exercise -> American == European
  bool unsupported{false};    // double-continuation corner -> price() returns NaN
  bool seeded{false};         // bnd.y[] holds a usable warm boundary
  double last_sigma{-1.0};
};

AloPricer::AloPricer(double S, double K, double T, double r, double q, Side side,
                     const std::optional<AlOpts>& opts)
    : st_(std::make_unique<State>()) {
  State& s = *st_;
  s.T = T;
  s.sch = scheme_from_opts(opts);
  // Internal put contract. Put: as-is. Call: McDonald-Schroder swap (S<->K, r<->q).
  if (side == Side::Put) {
    s.Sp = S;
    s.Kp = K;
    s.rp = r;
    s.qp = q;
  } else {
    s.Sp = K;
    s.Kp = S;
    s.rp = q;
    s.qp = r;
  }
  // Classify the internal put's (rate=rp, yield=qp). European -> pure European
  // (American == European). Unsupported is the double-continuation corner the
  // single-boundary machinery cannot represent: match andersen_lake's
  // NotImplemented by routing price() to NaN (the pricer's existing failure
  // channel). Only the American regime (rp > 0) prepares the boundary state, so
  // this is a no-op for every r>0 put / q>0 call (the whole production corpus).
  switch (classify_regime(/*rate=*/s.rp, /*yield=*/s.qp)) {
    case ExerciseRegime::European:
      s.european_only = true;
      s.prepared = true;
      return;
    case ExerciseRegime::Unsupported:
      s.unsupported = true;  // prepared stays false -> price() returns NaN
      return;
    case ExerciseRegime::American:
      break;
  }
  al_init_nodes(s.bnd, s.sch.n_boundary, s.T, s.Kp, s.rp, s.qp);
  if (!(s.bnd.xmax > 0.0)) {
    return;  // asymptotic boundary collapsed (matches andersen_lake NotImplemented)
  }
  const detail::GaussLegendre* fp = gl_find(s.sch.n_quad_fp);
  const detail::GaussLegendre* pr = gl_find(s.sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return;  // quadrature table unavailable
  }
  s.ws.qx_fp = fp->nodes.data();
  s.ws.qw_fp = fp->weights.data();
  s.ws.n_quad_fp = s.sch.n_quad_fp;
  s.ws.qx_price = pr->nodes.data();
  s.ws.qw_price = pr->weights.data();
  s.ws.n_quad_price = s.sch.n_quad_price;
  s.prepared = true;
}

AloPricer::~AloPricer() = default;
AloPricer::AloPricer(AloPricer&&) noexcept = default;
AloPricer& AloPricer::operator=(AloPricer&&) noexcept = default;

double AloPricer::price(double sigma) noexcept {
  State& s = *st_;
  // Degenerate: sigma ~ 0 or T ~ 0 collapses to intrinsic (internal-put intrinsic
  // Kp - Sp equals the original option's intrinsic for both sides).
  if (!(sigma > 1.0e-8) || s.T <= 1.0e-12) {
    const double intr = s.Kp - s.Sp;
    return (intr > 0.0) ? intr : 0.0;
  }
  // Double-continuation corner: no single-boundary price exists (andersen_lake
  // returns NotImplemented here). Surface NaN, matching the boundary-collapse
  // failure convention below.
  if (s.unsupported) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double euro = euro_put_sk(s.Sp, s.Kp, s.T, sigma, s.rp, s.qp);
  if (s.european_only) {
    return euro;
  }
  if (!s.prepared) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // Warm-start unless this is the first solve or sigma moved more than ~12% (then
  // the previous boundary is a poor seed and we re-seed cold via Barone-Adesi-
  // Whaley). Inside an IV Newton loop the near-convergence sigma steps are tiny,
  // so almost every residual after the bracket is a cheap warm solve.
  const bool cold = !s.seeded || !(s.last_sigma > 0.0) ||
                    std::fabs(sigma - s.last_sigma) > 0.12 * s.last_sigma;
  if (cold) {
    al_seed_boundary(s.bnd, sigma, s.rp, s.qp);
  }
  // Rebind the sweep-invariant geometry for this sigma (v_ji = sigma·sqrt(t_u); the
  // zc/exp blocks are sigma-independent but r/q are fixed for the contract, so a
  // single per-price bind covers them). This replaces the transcendentals the sweep
  // would otherwise recompute, so warm solves stay at parity or better.
  al_bind_geometry(s.bnd, s.ws, sigma, s.rp, s.qp);
  // Warm start skips ONLY the ~12-node Barone-Adesi-Whaley re-seed (the dominant
  // cold cost — 12 nested Newton root-finds), then runs the SAME sweep budget as a
  // cold solve (andersen_lake's n_iter_jn JN + n_iter_fp FP, early break at tol).
  // The sweeps — not the seed — are what converges the boundary, so warm and cold
  // reach the same fixed point; a cheaper warm budget under-converges the curved
  // long-dated early-exercise boundary and breaks the price round-trip. The 12%
  // re-seed guard keeps a warm sigma step small enough that the full budget fully
  // reconverges.
  double resid = 1.0;
  for (std::uint16_t k = 0; k < s.sch.n_iter_jn; ++k) {
    resid = al_jacobi_newton_sweep(s.bnd, s.ws, sigma, s.rp, s.qp);
    if (resid <= s.sch.tol) {
      break;
    }
  }
  if (resid > s.sch.tol) {
    for (std::uint16_t k = 0; k < s.sch.n_iter_fp; ++k) {
      resid = al_fixed_point_sweep(s.bnd, s.ws, sigma, s.rp, s.qp);
      if (resid <= s.sch.tol) {
        break;
      }
    }
  }
  s.seeded = true;
  s.last_sigma = sigma;

  const double prem = al_put_premium(s.bnd, s.ws, s.Sp, sigma, s.rp, s.qp);
  double px = euro + prem;
  const double intr = s.Kp - s.Sp;
  if (intr > px) {
    px = intr;
  }
  if (euro > px) {
    px = euro;
  }
  return (px > 0.0) ? px : 0.0;
}

// ── Public API ──────────────────────────────────────────────────────────

AlOpts al_default_opts() noexcept { return AlOpts{12, 24, 8, 1.0e-10}; }

AlOpts al_fast_opts() noexcept { return AlOpts{7, 16, 4, 1.0e-8}; }

Result<double> andersen_lake(double S, double K, double T, double sigma,
                             double r, double q, Side side,
                             const std::optional<AlOpts>& opts) {
  return andersen_lake_core(S, K, T, sigma, r, q, side, opts, /*specialize=*/true);
}

Status andersen_lake_call_slice(double S, std::span<const double> strikes,
                                double T, double sigma, double r, double q,
                                std::span<double> price_out,
                                const std::optional<AlOpts>& opts) {
  if (!(S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_call_slice: S must be > 0");
  }
  if (!(T >= 0.0) || !(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_call_slice: T and sigma must be >= 0");
  }
  if (strikes.size() != price_out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_call_slice: strikes / price_out length mismatch");
  }
  for (const double K : strikes) {
    if (!(K > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "andersen_lake_call_slice: every strike must be > 0");
    }
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_call_slice: r and q must be finite");
  }

  const std::size_t n = strikes.size();

  // Degenerate: T ~ 0 or sigma ~ 0 collapses to intrinsic (mirrors andersen_lake).
  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    for (std::size_t i = 0; i < n; ++i) {
      const double intr = S - strikes[i];
      price_out[i] = (intr > 0.0) ? intr : 0.0;
    }
    return Ok();
  }

  // Regime classification (internal-put rate=q, yield=r, since a call delegates
  // via McDonald-Schroder). European writes the Black-76 European call per strike
  // (matches the andersen_lake short-circuit exactly); Unsupported is the
  // double-continuation corner the ALO scheme cannot price.
  switch (classify_regime(/*rate=*/q, /*yield=*/r)) {
    case ExerciseRegime::European: {
      const double F = S * std::exp((r - q) * T);
      const double df = std::exp(-r * T);
      for (std::size_t i = 0; i < n; ++i) {
        price_out[i] = black76_price(F, strikes[i], T, sigma, df, Side::Call);
      }
      return Ok();
    }
    case ExerciseRegime::Unsupported:
      return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
    case ExerciseRegime::American:
      break;
  }

  // Boundary case (q > 0). The internal put has strike Kp = S (fixed), spot
  // Sp = K_i, rate rp = q, yield qp = r — so the boundary is solved ONCE here and
  // reused across every strike's premium quadrature. The sequence mirrors
  // al_solve_put (with S=Sp) exactly so each price is bit-identical to
  // andersen_lake(S, K_i, T, sigma, r, q, Side::Call, opts).
  const AlScheme sch = scheme_from_opts(opts);
  const double rp = q;  // internal-put rate
  const double qp = r;  // internal-put yield

  AlBoundary bnd;
  AlWorkspace ws;
  al_init_nodes(bnd, sch.n_boundary, T, /*K=*/S, rp, qp);
  if (!(bnd.xmax > 0.0)) {
    return Err(ErrorCode::NotImplemented,
               "andersen_lake_call_slice: asymptotic boundary collapsed (xmax <= 0)");
  }
  const detail::GaussLegendre* fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre* pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return Err(ErrorCode::Internal,
               "andersen_lake_call_slice: Gauss-Legendre table unavailable");
  }
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;
  al_bind_geometry(bnd, ws, sigma, rp, qp);

  al_seed_boundary(bnd, sigma, rp, qp);
  double resid = 1.0;
  for (std::uint16_t k = 0; k < sch.n_iter_jn; ++k) {
    resid = al_jacobi_newton_sweep(bnd, ws, sigma, rp, qp);
    if (resid <= sch.tol) {
      break;
    }
  }
  if (resid > sch.tol) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      resid = al_fixed_point_sweep(bnd, ws, sigma, rp, qp);
      if (resid <= sch.tol) {
        break;
      }
    }
  }

  // Per-strike premium: only the internal-put spot Sp = K_i changes.
  for (std::size_t i = 0; i < n; ++i) {
    const double Ki = strikes[i];
    const double euro = euro_put_sk(/*S=*/Ki, /*K=*/S, T, sigma, rp, qp);
    const double prem = al_put_premium(bnd, ws, /*S=*/Ki, sigma, rp, qp);
    double px = euro + prem;
    const double intr = S - Ki;  // internal-put intrinsic Kp - Sp == call intrinsic
    if (intr > px) {
      px = intr;
    }
    if (euro > px) {
      px = euro;
    }
    price_out[i] = (px > 0.0) ? px : 0.0;
  }
  return Ok();
}

Status andersen_lake_put_slice(double S, std::span<const double> strikes,
                               double T, double sigma, double r, double q,
                               std::span<double> price_out,
                               const std::optional<AlOpts>& opts) {
  if (!(S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice: S must be > 0");
  }
  if (!(T >= 0.0) || !(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_put_slice: T and sigma must be >= 0");
  }
  if (strikes.size() != price_out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_put_slice: strikes / price_out length mismatch");
  }
  for (const double K : strikes) {
    if (!(K > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "andersen_lake_put_slice: every strike must be > 0");
    }
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_put_slice: r and q must be finite");
  }

  const std::size_t n = strikes.size();

  // Degenerate: T ~ 0 or sigma ~ 0 collapses to intrinsic (mirrors andersen_lake).
  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    for (std::size_t i = 0; i < n; ++i) {
      const double intr = strikes[i] - S;  // put intrinsic K_i - S
      price_out[i] = (intr > 0.0) ? intr : 0.0;
    }
    return Ok();
  }

  // Regime classification in the put's OWN (rate = r, yield = q) terms — the put
  // convention, NOT the call slice's (q, r). European writes the Black-76 European
  // put per strike (matches andersen_lake's short-circuit exactly); Unsupported is
  // the double-continuation corner the ALO scheme cannot price.
  switch (classify_regime(/*rate=*/r, /*yield=*/q)) {
    case ExerciseRegime::European: {
      const double F = S * std::exp((r - q) * T);
      const double df = std::exp(-r * T);
      for (std::size_t i = 0; i < n; ++i) {
        price_out[i] = black76_price(F, strikes[i], T, sigma, df, Side::Put);
      }
      return Ok();
    }
    case ExerciseRegime::Unsupported:
      return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
    case ExerciseRegime::American:
      break;
  }

  // American (r > 0). Solve ONE boundary at the reference strike strikes[0], then
  // reuse it for every K_i by strike HOMOGENEITY: the live state y = (log(b/xmax))²
  // is K-independent, so keep y[] and rescale only (K, xmax = al_xmax_put(K_i,r,q)),
  // which scales the boundary b = xmax·exp(-sqrt(y)) linearly in K. Each price runs
  // the SAME euro + premium + clamp path as al_solve_put (al_put_price_from_boundary),
  // so the reference strike strikes[0] is BIT-IDENTICAL to andersen_lake(S,strikes[0],
  // …,Put), and every other strike matches to a few ULP: the sweep kernels carry b.K
  // in absolute (non-ratio) terms — alpha = K·e^{-(r-q)τ}, y_from_b(b, K·min(1,r/q)) —
  // so the reused y[] equals a fresh per-strike y[] only in EXACT arithmetic. The
  // measured gap (see at-task-8-report.md) is why the correction cache's put row is
  // NOT rerouted here — that stays on the scalar path so its bit-identity guards hold.
  //
  // Formulation: SCALE-BOUNDARY (fix spot S, rescale the boundary to K_i) is chosen
  // over SCALE-SPOT (solve at K=1, price K_i·P(S/K_i,1)) because scale-boundary reuses
  // euro_put_sk(S,K_i) and al_put_premium unchanged, so the ONLY divergence from a
  // fresh solve is the reused y[]; scale-spot layers S/K_i-division and ×K_i rounding
  // on top, measurably widening the ULP gap (step-1 spike measured both).
  const AlScheme sch = scheme_from_opts(opts);
  AlBoundary bnd;
  AlWorkspace ws;
  switch (al_solve_put_boundary(/*K=*/strikes[0], T, sigma, r, q, sch, bnd, ws)) {
    case AlSolveStatus::Collapsed:
      return Err(ErrorCode::NotImplemented,
                 "andersen_lake_put_slice: asymptotic boundary collapsed (xmax <= 0)");
    case AlSolveStatus::TableMissing:
      return Err(ErrorCode::Internal,
                 "andersen_lake_put_slice: Gauss-Legendre table unavailable");
    case AlSolveStatus::Ok:
      break;
  }

  for (std::size_t i = 0; i < n; ++i) {
    const double Ki = strikes[i];
    bnd.K = Ki;                        // homogeneity rescale: strike …
    bnd.xmax = al_xmax_put(Ki, r, q);  // … and asymptotic level B(∞), same y[]
    price_out[i] = al_put_price_from_boundary(bnd, ws, S, Ki, T, sigma, r, q);
  }
  return Ok();
}

Result<double> baw_american(double S, double K, double T, double sigma,
                            double r, double q, Side side,
                            std::uint16_t max_iter, double tol) {
  if (!(K > 0.0 && S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "baw_american: S and K must be > 0");
  }
  if (!(T >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "baw_american: T must be >= 0");
  }
  if (!(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "baw_american: sigma must be >= 0");
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument, "baw_american: r and q must be finite");
  }

  const std::uint16_t mi = max_iter ? max_iter : std::uint16_t{16};
  const double tt = (tol > 0.0) ? tol : 1.0e-8;

  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    const double intr = (side == Side::Call) ? (S - K) : (K - S);
    return Ok(intr > 0.0 ? intr : 0.0);
  }

  const double euro =
      (side == Side::Call) ? euro_call_sk(S, K, T, sigma, r, q)
                           : euro_put_sk(S, K, T, sigma, r, q);

  // Same regime classification as andersen_lake (internal-put rate/yield). BAW is
  // a single-boundary approximation, so it cannot represent the double-continuation
  // corner either: return the SAME NotImplemented error there.
  switch (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                          /*yield=*/(side == Side::Put) ? q : r)) {
    case ExerciseRegime::European:
      return Ok(euro);
    case ExerciseRegime::Unsupported:
      return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
    case ExerciseRegime::American:
      break;
  }

  const double sigma2 = sigma * sigma;
  const double M = 2.0 * r / sigma2;
  const double N = 2.0 * (r - q) / sigma2;
  const double h = 1.0 - std::exp(-r * T);
  if (!(h > 0.0)) {
    return Ok(euro);
  }
  const double disc = (N - 1.0) * (N - 1.0) + 4.0 * M / h;
  if (!(disc >= 0.0)) {
    return Ok(euro);
  }
  const double sqrt_disc = std::sqrt(disc);

  if (side == Side::Put) {
    const double q1 = 0.5 * (-(N - 1.0) - sqrt_disc);
    if (!(q1 < 0.0)) {
      return Ok(euro);
    }
    const double Sx = newton_critical_put(K, T, sigma, r, q, q1, mi, tt);
    if (!(Sx > 0.0 && Sx < K)) {
      return Err(ErrorCode::Unavailable, "baw_american: put critical-price did not converge");
    }
    if (S <= Sx) {
      return Ok(K - S);
    }
    const double d1_Sx = d1_of(Sx, K, r, q, sigma, T);
    const double A1 = -(Sx / q1) * (1.0 - std::exp(-q * T) * norm_cdf(-d1_Sx));
    const double premium = A1 * std::pow(S / Sx, q1);
    const double price = euro + premium;
    const double intr = K - S;
    return Ok(price > intr ? price : intr);
  }

  const double q2 = 0.5 * (-(N - 1.0) + sqrt_disc);
  if (!(q2 > 1.0)) {
    return Ok(euro);
  }
  const double Sx = newton_critical_call(K, T, sigma, r, q, q2, mi, tt);
  if (!(Sx > K)) {
    return Err(ErrorCode::Unavailable, "baw_american: call critical-price did not converge");
  }
  if (S >= Sx) {
    return Ok(S - K);
  }
  const double d1_Sx = d1_of(Sx, K, r, q, sigma, T);
  const double A2 = (Sx / q2) * (1.0 - std::exp(-q * T) * norm_cdf(d1_Sx));
  const double premium = A2 * std::pow(S / Sx, q2);
  const double price = euro + premium;
  const double intr = S - K;
  return Ok(price > intr ? price : intr);
}

Result<double> american_price(double S, double K, double T, double sigma,
                              double r, double q, Side side,
                              AmericanMethod method,
                              const std::optional<AlOpts>& opts) {
  switch (method) {
    case AmericanMethod::AndersenLake:
      return andersen_lake(S, K, T, sigma, r, q, side, opts);
    case AmericanMethod::Baw:
      return baw_american(S, K, T, sigma, r, q, side);
  }
  return Err(ErrorCode::Internal, "american_price: unhandled method");  // unreachable
}

double american_price_cached(double S, double K, double T, double sigma,
                             double r, double q, Side side,
                             const CorrectionCache* correction) {
  if (!correction || !correction->populated()) {
    ATX_VOL_COUNT(CacheColdFallbacks);
    const Result<double> p = andersen_lake(S, K, T, sigma, r, q, side, std::nullopt);
    return p ? *p : std::numeric_limits<double>::quiet_NaN();
  }
  // Double-continuation corner: the cached Black-76 + correction is a silently
  // wrong European-shaped number here (the cache holds no valid early-exercise
  // premium in this regime). Surface NaN, matching the cold andersen_lake path,
  // which returns NotImplemented (-> NaN) above. No-op for r>0 puts / q>0 calls.
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) ==
      ExerciseRegime::Unsupported) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  const double euro = black76_price(F, K, T, sigma, df, side);
  const double k_log = std::log(K / F);
  const double corr = correction->eval(k_log, T, sigma);
  ATX_VOL_COUNT(CacheHits);
  return euro + F * corr;
}

Result<AmericanGreeks> american_greeks(double S, double K, double T,
                                       double sigma, double r, double q,
                                       Side side,
                                       const CorrectionCache* correction) {
  // Degenerate-input contract: SURFACE an error. This is deliberately asymmetric
  // with `american_vega`, which returns a 0.0 sentinel on the same input. The
  // difference is intentional: `american_greeks` has no sentinel consumer — a
  // caller must distinguish "greeks unavailable" from a legitimately zero
  // sensitivity — so it reports InvalidArgument. `american_vega`, by contrast,
  // feeds the IV inverter's Newton step, which reads a 0 vega as "force
  // bisection"; changing either behavior breaks a downstream contract.
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_greeks: S, K, T, sigma must be > 0");
  }
  // Double-continuation corner: the Black-76 + correction bundle would be built
  // on a silently-wrong European price. Surface the SAME NotImplemented error the
  // other entry points use rather than a wrong Greeks bundle. No-op for r>0 puts
  // / q>0 calls (the production corpus): those classify American.
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) ==
      ExerciseRegime::Unsupported) {
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  }
  AmericanGreeks out;
  american_greeks_first_order(S, K, T, sigma, r, q, side, correction, out);
  return Ok(out);
}

Result<AmericanGreeks> american_greeks_fd(double S, double K, double T,
                                          double sigma, double r, double q,
                                          Side side, AmericanMethod method,
                                          const std::optional<AlOpts>& opts,
                                          bool warm_start) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "american_greeks_fd: S, K, T, sigma must be > 0");
  }

  // Central-difference steps (match the P0-1 spec). Near expiry the T-derivatives
  // (theta, charm) fall back to a one-sided forward stencil so no bump reaches a
  // non-positive T.
  const double hS = 1.0e-3 * S;
  double hv = 1.0e-3;
  if (sigma - hv <= 0.0) {
    hv = 0.5 * sigma;
  }
  const double hr = 1.0e-4;
  const double hT = 1.0e-3;
  const bool near_expiry = (T - hT <= 1.0e-8);

  // Bumped cold price. Captures the FIRST american_price error and short-circuits;
  // the poisoned NaN it returns afterwards is never consumed once `failed` is set.
  bool failed = false;
  atx::core::Error first_err;
  auto P = [&](double dS, double dsig, double dr, double dT) -> double {
    if (failed) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    const Result<double> p =
        american_price(S + dS, K, T + dT, sigma + dsig, r + dr, q, side, method, opts);
    if (!p) {
      failed = true;
      first_err = p.error();
      return std::numeric_limits<double>::quiet_NaN();
    }
    return *p;
  };

  // Put fast path (AndersenLake only): the AL boundary is S-independent, so the 17
  // stencils share just 7 unique (sigma,r,T) boundaries. Memoize the boundary per
  // (dsig,dr,dT) and re-price each spot stencil against it. Bit-identical to P():
  // al_solve_put with r>0 non-degenerate IS solve-boundary + price-from-boundary,
  // and the degenerate / r<=0 guards below mirror andersen_lake exactly. The rare
  // boundary-collapse corner falls back to the scalar P() path (same error).
  const bool put_fast =
      (side == Side::Put) && (method == AmericanMethod::AndersenLake);
  // Call fast path (P2.1): McDonald-Schroder prices a call via an internal put, and
  // that internal-put boundary IS spot-dependent (its strike = the call spot the
  // delta/gamma stencils bump) — but homogeneous of degree one in that strike, so
  // one boundary per (sigma,r,T) state rescales across the spot stencils. Mirrors
  // put_fast structurally; see Pcall. NOT bit-identical to the cold call greeks —
  // the homogeneity rescale is exact in R, ~1e-13 in IEEE (far under the sprint's
  // ~1e-7 budget); accepted as default per the controller policy ruling.
  const bool call_fast =
      (side == Side::Call) && (method == AmericanMethod::AndersenLake);
  const AlScheme sch = scheme_from_opts(opts);
  struct BndCache {
    double dsig{0.0}, dr{0.0}, dT{0.0};
    AlBoundary bnd{};
    AlWorkspace ws{};
    bool ok{false};
    // Canonical base scaling (internal-strike = S, the unbumped call spot) captured
    // at solve time. Pcall rescales bnd.{K,xmax} per spot stencil when it prices, so
    // these let it restore the boundary to its canonical state afterwards — keeping
    // memo[0] an un-mutated warm seed. Unused by the put path (Pput never rescales).
    double base_K{0.0}, base_xmax{0.0};
  };
  std::array<BndCache, 7> memo{};
  std::size_t n_memo = 0;
  auto Pput = [&](double dS, double dsig, double dr, double dT) -> double {
    if (failed) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    const double S2 = S + dS;
    const double sig2 = sigma + dsig;
    const double r2 = r + dr;
    const double T2 = T + dT;
    // andersen_lake guards, replicated so each stencil matches a full cold call.
    if (T2 <= 1.0e-12 || sig2 <= 1.0e-8) {
      const double intr = K - S2;
      return (intr > 0.0) ? intr : 0.0;
    }
    // Put no-early-exercise regime is American == European (Black-76). The
    // double-continuation corner (yield q < rate r2 <= 0) is unpriceable — defer
    // to the scalar P() path so andersen_lake's NotImplemented error propagates
    // through the bundle instead of a silently-wrong European Greeks set.
    switch (classify_regime(/*rate=*/r2, /*yield=*/q)) {
      case ExerciseRegime::European:
        return black76_price(S2 * std::exp((r2 - q) * T2), K, T2, sig2,
                             std::exp(-r2 * T2), Side::Put);
      case ExerciseRegime::Unsupported:
        return P(dS, dsig, dr, dT);
      case ExerciseRegime::American:
        break;
    }
    BndCache* c = nullptr;
    for (std::size_t i = 0; i < n_memo; ++i) {
      if (memo[i].dsig == dsig && memo[i].dr == dr && memo[i].dT == dT) {
        c = &memo[i];
        break;
      }
    }
    if (c == nullptr) {
      c = &memo[n_memo++];
      c->dsig = dsig;
      c->dr = dr;
      c->dT = dT;
      // Warm-start a bumped boundary from the converged base (memo[0]); the base
      // itself, and every boundary when warm_start is off, is solved cold.
      const bool is_base = (dsig == 0.0 && dr == 0.0 && dT == 0.0);
      const bool can_warm = warm_start && !is_base && n_memo > 1 && memo[0].ok &&
                            memo[0].dsig == 0.0 && memo[0].dr == 0.0 &&
                            memo[0].dT == 0.0;
      const AlSolveStatus st =
          can_warm ? al_solve_put_boundary_warm(K, T2, sig2, r2, q, sch,
                                                 memo[0].bnd, c->bnd, c->ws)
                   : al_solve_put_boundary(K, T2, sig2, r2, q, sch, c->bnd, c->ws);
      c->ok = (st == AlSolveStatus::Ok);
    }
    if (!c->ok) {
      return P(dS, dsig, dr, dT);  // boundary collapsed: exact scalar fallback
    }
    return al_put_price_from_boundary(c->bnd, c->ws, S2, K, T2, sig2, r2, q);
  };
  // Call fast path evaluator. McDonald-Schroder: C(S,K,r,q) prices via an internal
  // put with spot = K (the call strike, FIXED across every stencil), strike = the
  // call SPOT S2 (bumped by the delta/gamma/vanna/charm stencils), internal rate =
  // q (the call yield, FIXED), internal yield = r2 (the call rate, bumped). Unlike
  // the put boundary, this boundary depends on its strike = the call spot, which the
  // spot stencils move — so per (dsig,dr,dT) state we solve the boundary ONCE at the
  // BASE internal-strike = S (the unbumped call spot) and rescale it to S2 per spot
  // stencil by strike homogeneity (T8's put-slice engine: keep y[], reset K + xmax).
  // The base spot stencil (S2 == S) rescales to itself, so p0 is bit-identical to
  // andersen_lake(...,Call), as are the no-spot-bump greeks (vega/volga/rho/theta);
  // only the spot-bumped greeks (delta/gamma/vanna/charm) carry the tiny shift.
  auto Pcall = [&](double dS, double dsig, double dr, double dT) -> double {
    if (failed) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    const double S2 = S + dS;
    const double sig2 = sigma + dsig;
    const double r2 = r + dr;
    const double T2 = T + dT;
    // andersen_lake guards, replicated so each stencil matches a full cold call.
    if (T2 <= 1.0e-12 || sig2 <= 1.0e-8) {
      const double intr = S2 - K;  // call intrinsic
      return (intr > 0.0) ? intr : 0.0;
    }
    // Regime in the call's internal-put terms (rate = q, yield = r2 — the same
    // order andersen_lake_call_slice uses). European short-circuits to the Black-76
    // European CALL (matches andersen_lake exactly); the double-continuation corner
    // (yield r2 < rate q <= 0) defers to the scalar P() path so andersen_lake's
    // NotImplemented propagates through the bundle, never a silently-wrong European.
    switch (classify_regime(/*rate=*/q, /*yield=*/r2)) {
      case ExerciseRegime::European:
        return black76_price(S2 * std::exp((r2 - q) * T2), K, T2, sig2,
                             std::exp(-r2 * T2), Side::Call);
      case ExerciseRegime::Unsupported:
        return P(dS, dsig, dr, dT);
      case ExerciseRegime::American:
        break;
    }
    BndCache* c = nullptr;
    for (std::size_t i = 0; i < n_memo; ++i) {
      if (memo[i].dsig == dsig && memo[i].dr == dr && memo[i].dT == dT) {
        c = &memo[i];
        break;
      }
    }
    if (c == nullptr) {
      c = &memo[n_memo++];
      c->dsig = dsig;
      c->dr = dr;
      c->dT = dT;
      // Solve the internal-put boundary ONCE at the base internal-strike = S (the
      // unbumped call spot), internal rate = q, internal yield = r2. Warm-start a
      // bumped state from the converged base memo[0] (also at internal-strike = S),
      // exactly as the put path; the base and all-cold path solve fresh.
      const bool is_base = (dsig == 0.0 && dr == 0.0 && dT == 0.0);
      const bool can_warm = warm_start && !is_base && n_memo > 1 && memo[0].ok &&
                            memo[0].dsig == 0.0 && memo[0].dr == 0.0 &&
                            memo[0].dT == 0.0;
      const AlSolveStatus st =
          can_warm ? al_solve_put_boundary_warm(/*K=*/S, T2, sig2, /*r=*/q, /*q=*/r2,
                                                 sch, memo[0].bnd, c->bnd, c->ws)
                   : al_solve_put_boundary(/*K=*/S, T2, sig2, /*r=*/q, /*q=*/r2, sch,
                                           c->bnd, c->ws);
      c->ok = (st == AlSolveStatus::Ok);
      // Capture the canonical base scaling (internal-strike = S) BEFORE any price
      // rescales it, so bumped states can warm-seed from an un-mutated memo[0].
      c->base_K = c->bnd.K;
      c->base_xmax = c->bnd.xmax;
    }
    if (!c->ok) {
      return P(dS, dsig, dr, dT);  // boundary collapsed: exact scalar fallback
    }
    // Homogeneity rescale of the base boundary to internal-strike = S2 (keep y[],
    // reset the strike and asymptotic level), then price the internal put at spot =
    // K (the fixed call strike), strike = S2 (the bumped call spot).
    c->bnd.K = S2;
    c->bnd.xmax = al_xmax_put(S2, /*r=*/q, /*q=*/r2);
    const double price = al_put_price_from_boundary(
        c->bnd, c->ws, /*spot=*/K, /*strike=*/S2, T2, sig2, /*r=*/q, /*q=*/r2);
    // T9a-M1: restore the canonical base scaling so the in-place rescale above never
    // leaves a ~0.1%-off xmax in the memoized boundary. Without this, a subsequent
    // warm_start state seeds al_solve_put_boundary_warm from memo[0].bnd whose xmax
    // was left at the last spot stencil's (S ± hS) scaling. Non-warm behavior is
    // unaffected (every price re-sets K/xmax before use).
    c->bnd.K = c->base_K;
    c->bnd.xmax = c->base_xmax;
    return price;
  };
  auto EV = [&](double dS, double dsig, double dr, double dT) -> double {
    return call_fast ? Pcall(dS, dsig, dr, dT)
                     : (put_fast ? Pput(dS, dsig, dr, dT) : P(dS, dsig, dr, dT));
  };

  // Base mark: EXACT unbumped args — bit-identical to fair_value()'s own call.
  const double p0 = EV(0.0, 0.0, 0.0, 0.0);

  // Spot stencils.
  const double p_Sp = EV(+hS, 0.0, 0.0, 0.0);
  const double p_Sm = EV(-hS, 0.0, 0.0, 0.0);
  // Vol stencils.
  const double p_vp = EV(0.0, +hv, 0.0, 0.0);
  const double p_vm = EV(0.0, -hv, 0.0, 0.0);
  // Rate stencils.
  const double p_rp = EV(0.0, 0.0, +hr, 0.0);
  const double p_rm = EV(0.0, 0.0, -hr, 0.0);
  // Time stencils (one-sided forward near expiry).
  const double p_Tp = EV(0.0, 0.0, 0.0, +hT);
  const double p_Tm = near_expiry ? p0 : EV(0.0, 0.0, 0.0, -hT);
  // Vanna cross (spot x vol), central.
  const double p_SpVp = EV(+hS, +hv, 0.0, 0.0);
  const double p_SpVm = EV(+hS, -hv, 0.0, 0.0);
  const double p_SmVp = EV(-hS, +hv, 0.0, 0.0);
  const double p_SmVm = EV(-hS, -hv, 0.0, 0.0);
  // Charm cross (spot x time); one-sided in T near expiry.
  const double p_SpTp = EV(+hS, 0.0, 0.0, +hT);
  const double p_SmTp = EV(-hS, 0.0, 0.0, +hT);
  const double p_SpTm = near_expiry ? p_Sp : EV(+hS, 0.0, 0.0, -hT);
  const double p_SmTm = near_expiry ? p_Sm : EV(-hS, 0.0, 0.0, -hT);

  if (failed) {
    return Err(std::move(first_err));
  }

  // Time denominator collapses to hT for the one-sided forward stencils.
  const double dT_den = near_expiry ? hT : (2.0 * hT);

  AmericanGreeks out;
  out.price = p0;
  out.delta = (p_Sp - p_Sm) / (2.0 * hS);
  out.gamma = (p_Sp - 2.0 * p0 + p_Sm) / (hS * hS);
  out.vega = (p_vp - p_vm) / (2.0 * hv);
  out.volga = (p_vp - 2.0 * p0 + p_vm) / (hv * hv);
  out.rho = (p_rp - p_rm) / (2.0 * hr);
  // theta = dP/dt = -dP/dT (calendar convention).
  out.theta = -(p_Tp - p_Tm) / dT_den;
  out.vanna = (p_SpVp - p_SpVm - p_SmVp + p_SmVm) / (4.0 * hS * hv);
  // charm = d2P/dS dt = -d2P/dS dT.
  out.charm = -(p_SpTp - p_SpTm - p_SmTp + p_SmTm) / (2.0 * hS * dT_den);
  return Ok(out);
}

double american_vega(double S, double K, double T, double sigma, double r,
                     double q, Side side, const CorrectionCache* correction) noexcept {
  // Degenerate-input contract: return the 0.0 SENTINEL, not an error. This is a
  // LOAD-BEARING difference from `american_greeks` (which returns InvalidArgument
  // on the same input): the IV inverter's Newton step reads a 0 vega as "vega
  // unavailable, force bisection". Do NOT change this to an error/NaN — see the
  // doc comment on american_vega() in american.hpp.
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return 0.0;
  }
  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  // Black-76 vega is closed-form (no early-exercise FD); the correction adds only
  // its first-order sigma partial — one cache eval_grad instead of the seven the
  // full american_greeks bundle runs for its second-order FD terms.
  const double euro_vega = black76_greeks(F, K, T, sigma, r, df, side).greeks.vega;
  double dc_ds = 0.0;
  if (correction) {
    correction->eval_grad(std::log(K / F), T, sigma, nullptr, nullptr, &dc_ds);
  }
  return euro_vega + F * dc_ds;
}

Result<AmericanGreeks> american_greeks_al(double S, double K, double T,
                                          double sigma, double r, double q,
                                          Side side,
                                          const std::optional<AlOpts>& opts) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "american_greeks_al: S, K, T, sigma must be > 0");
  }
  // Native analytic route for genuine early exercise on BOTH sides. Under the
  // McDonald-Schroder map C(S,K,r,q) = P(K,S,q,r) a call reduces to an internal put
  // with (rate=q, yield=r); a put is the internal put itself, (rate=r, yield=q). So
  // the early-exercise regime is governed by the internal-put SHORT RATE: r for a
  // put, q for a call (classify_regime(rate, yield) -> American iff rate > 0). The
  // degenerate corners (T~0 / sigma~0) and the no-early-exercise regime (rate <= 0,
  // American == European) fall back to the exact cold FD path on either side.
  const bool is_call = (side == Side::Call);
  const double al_rate = is_call ? q : r;  // internal-put short rate
  if (al_rate <= 0.0 || T <= 1.0e-12 || sigma <= 1.0e-8) {
    return american_greeks_fd(S, K, T, sigma, r, q, side,
                              AmericanMethod::AndersenLake, opts, /*warm_start=*/false);
  }

  // Boundaries: the base (spot-independent, so delta/gamma are EXACT finite
  // differences over it) plus sigma+/-, r+/- for the vega/rho/vanna/volga stencils
  // — the exercise boundary genuinely moves with sigma/r (the envelope/frozen-
  // boundary shortcut does NOT hold for the AL premium decomposition), so those are
  // re-solved. The T+/- solves are DROPPED: theta/charm come from the continuation-
  // region PDE. Five solves versus american_greeks_fd's seven, and theta/charm gain
  // accuracy (no time-bump truncation). Any bumped-boundary corner (collapse, or
  // r-hr crossing <=0) falls the whole bundle back to the exact FD path.
  const AlScheme sch = scheme_from_opts(opts);
  const double hS = 1.0e-3 * S;
  double hv = 1.0e-3;
  if (sigma - hv <= 0.0) hv = 0.5 * sigma;
  const double hr = 1.0e-4;
  // Rate-bump regime guard. For a PUT the r-stencil bumps the internal-put SHORT
  // RATE (= r); the down-bump r - hr <= 0 crosses out of the American regime, so the
  // bundle falls back to the exact FD path (byte-for-byte the pre-change put guard).
  // For a CALL the r-stencil bumps the internal YIELD (= r); the internal rate is
  // q > 0 (gated above) and is NEVER bumped, so classify_regime(rate=q, yield=r±hr)
  // stays American for every stencil — no regime crossing is possible and al_xmax_put
  // (rate q > 0) stays > 0. The only bumped-boundary failure a call can hit is a
  // numeric collapse, caught by the 5-solve `!= Ok` guard below.
  if (!is_call && r - hr <= 0.0) {
    return american_greeks_fd(S, K, T, sigma, r, q, side,
                              AmericanMethod::AndersenLake, opts, /*warm_start=*/false);
  }

  // Internal-put boundary solves. For a PUT the boundary is spot-independent (strike
  // = K fixed, rate = r±, yield = q). For a CALL it is solved at the BASE internal-
  // strike = S (the unbumped call spot) with rate = q fixed, yield = r± bumped, then
  // rescaled per spot stencil by strike homogeneity in `px`.
  const double Kb = is_call ? S : K;  // base internal-strike
  const auto solve = [&](AlBoundary& b, AlWorkspace& w, double sig_s,
                         double dr) -> AlSolveStatus {
    const double rate = is_call ? q : (r + dr);
    const double yield = is_call ? (r + dr) : q;
    return al_solve_put_boundary(Kb, T, sig_s, rate, yield, sch, b, w);
  };
  AlBoundary b0, bvp, bvm, brp, brm;
  AlWorkspace w0, wvp, wvm, wrp, wrm;
  if (solve(b0, w0, sigma, 0.0) != AlSolveStatus::Ok ||
      solve(bvp, wvp, sigma + hv, 0.0) != AlSolveStatus::Ok ||
      solve(bvm, wvm, sigma - hv, 0.0) != AlSolveStatus::Ok ||
      solve(brp, wrp, sigma, +hr) != AlSolveStatus::Ok ||
      solve(brm, wrm, sigma, -hr) != AlSolveStatus::Ok) {
    return american_greeks_fd(S, K, T, sigma, r, q, side,
                              AmericanMethod::AndersenLake, opts, /*warm_start=*/false);
  }

  // Price at spot stencil S2 on a given solved boundary (its own sigma/r). The PUT
  // boundary is spot-independent, so it prices directly (spot = S2, strike = K). The
  // CALL boundary depends on its strike = the call spot, so `px` rescales it to
  // internal-strike = S2 by strike homogeneity (keep y[], reset K + xmax) and prices
  // the internal put at spot = K (the fixed call strike), strike = S2. The in-place
  // rescale is safe: every px sets K + xmax before pricing, and no boundary is warm-
  // reused here (all five are solved cold above), so there is no stale-seed hazard.
  const auto px = [&](AlBoundary& b, const AlWorkspace& w, double S2, double sig2,
                      double r2) -> double {
    if (is_call) {
      b.K = S2;
      b.xmax = al_xmax_put(S2, /*r=*/q, /*q=*/r2);
      return al_put_price_from_boundary(b, w, /*spot=*/K, /*strike=*/S2, T, sig2,
                                        /*r=*/q, /*q=*/r2);
    }
    return al_put_price_from_boundary(b, w, S2, K, T, sig2, r2, q);
  };

  // Spot stencils on the base boundary (exact — boundary independent of S).
  const double v0 = px(b0, w0, S, sigma, r);
  const double vSp = px(b0, w0, S + hS, sigma, r), vSm = px(b0, w0, S - hS, sigma, r);
  const double vS2p = px(b0, w0, S + 2.0 * hS, sigma, r);
  const double vS2m = px(b0, w0, S - 2.0 * hS, sigma, r);
  // Vol stencils on the re-solved sigma+/- boundaries (incl. the vanna cross).
  const double vvp = px(bvp, wvp, S, sigma + hv, r), vvm = px(bvm, wvm, S, sigma - hv, r);
  const double vSpVp = px(bvp, wvp, S + hS, sigma + hv, r);
  const double vSmVp = px(bvp, wvp, S - hS, sigma + hv, r);
  const double vSpVm = px(bvm, wvm, S + hS, sigma - hv, r);
  const double vSmVm = px(bvm, wvm, S - hS, sigma - hv, r);
  // Rate stencils on the re-solved r+/- boundaries.
  const double vrp = px(brp, wrp, S, sigma, r + hr), vrm = px(brm, wrm, S, sigma, r - hr);

  AmericanGreeks out;
  out.price = v0;
  out.delta = (vSp - vSm) / (2.0 * hS);
  out.gamma = (vSp - 2.0 * v0 + vSm) / (hS * hS);
  out.vega = (vvp - vvm) / (2.0 * hv);
  out.volga = (vvp - 2.0 * v0 + vvm) / (hv * hv);
  out.rho = (vrp - vrm) / (2.0 * hr);
  out.vanna = (vSpVp - vSpVm - vSmVp + vSmVm) / (4.0 * hS * hv);

  // theta / charm from the continuation-region PDE. In the exercise region the
  // frozen price is at intrinsic (put delta -> -1 / call delta -> +1, gamma -> 0);
  // there the intrinsic (K - S for a put, S - K for a call) has no time value, so
  // theta = charm = 0. The PDE relations below are in the ORIGINAL option's (S,r,q)
  // and are side-agnostic given the correct V/delta/gamma/speed — no sign flip.
  const double intr0 = is_call ? (S - K) : (K - S);
  const bool exercised = (v0 <= intr0 + 1.0e-9 * K) && (intr0 > 0.0);
  if (exercised) {
    out.theta = 0.0;
    out.charm = 0.0;
  } else {
    // speed = d3V/dS3 (5-point), for charm = d(theta)/dS.
    const double speed = (vS2p - 2.0 * vSp + 2.0 * vSm - vS2m) / (2.0 * hS * hS * hS);
    out.theta = r * v0 - (r - q) * S * out.delta - 0.5 * sigma * sigma * S * S * out.gamma;
    out.charm = r * out.delta - (r - q) * (out.delta + S * out.gamma) -
                0.5 * sigma * sigma * (2.0 * S * out.gamma + S * S * speed);
  }
  return Ok(out);
}

// ─────────────────────────────────────────────────────────────────────────
// C1.7 (additive-only; see FILE-OWNERSHIP RULE in the 07-09 sprint doc) — a
// vega-ONLY mirror of american_greeks_al's vega branch just above, declared in
// american.hpp. Reuses the identical guard chain, step size, and sigma+/-
// boundary re-solve + centered difference, so the result is bit-identical to
// `american_greeks_al(...).vega` whenever both take the native analytic
// route: same `al_rate`/T/sigma degenerate guard, same put r-hr<=0 regime
// guard, same `hv`, same `solve`/`px` arithmetic (the rate-bump `dr` term the
// bundle's shared `solve` lambda carries is always 0.0 for the sigma+/-
// stencils, so dropping the parameter here is exact, not approximate: r+0.0
// is r bit-for-bit). Falls back to american_greeks_fd(...).vega — the SAME
// call greeks_analytic() itself forwards on these branches — whenever the
// bundle would.
//
// Cost: 0 boundary solves on the degenerate/European-regime fallback (routes
// straight to the FD bundle, same as the full bundle would), else 2 (sigma+,
// sigma-) instead of the full bundle's 5 (base + sigma+/- + r+/-). The base
// (b0) and rate (r+/-hr) boundary solves are NOT attempted here, so unlike
// the bundle this function's fallback trigger does not observe a b0- or
// r+/-hr-only collapse (bvp/bvm both convergent) — see the doc comment in
// american.hpp and task-c1.7-report.md for why this is the deliberate,
// disclosed scope of the additive kernel (not silently swept under a
// tolerance) and why it was not observed on the fitted-surface parity grid.
Result<double> american_vega_al(double S, double K, double T, double sigma, double r, double q,
                                Side side, const std::optional<AlOpts>& opts) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_vega_al: S, K, T, sigma must be > 0");
  }
  const bool is_call = (side == Side::Call);
  const double al_rate = is_call ? q : r; // internal-put short rate
  const auto fd_vega = [&]() -> Result<double> {
    const Result<AmericanGreeks> g = american_greeks_fd(
        S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts, /*warm_start=*/false);
    if (!g) {
      return Err(g.error());
    }
    return Ok(g->vega);
  };
  if (al_rate <= 0.0 || T <= 1.0e-12 || sigma <= 1.0e-8) {
    return fd_vega();
  }

  const AlScheme sch = scheme_from_opts(opts);
  double hv = 1.0e-3;
  if (sigma - hv <= 0.0) hv = 0.5 * sigma;
  const double hr = 1.0e-4;
  if (!is_call && r - hr <= 0.0) {
    return fd_vega();
  }

  const double Kb = is_call ? S : K; // base internal-strike
  const double rate = is_call ? q : r;
  const double yield = is_call ? r : q;
  AlBoundary bvp, bvm;
  AlWorkspace wvp, wvm;
  if (al_solve_put_boundary(Kb, T, sigma + hv, rate, yield, sch, bvp, wvp) != AlSolveStatus::Ok ||
      al_solve_put_boundary(Kb, T, sigma - hv, rate, yield, sch, bvm, wvm) != AlSolveStatus::Ok) {
    return fd_vega();
  }

  const auto px = [&](AlBoundary& b, const AlWorkspace& w, double sig2) -> double {
    if (is_call) {
      b.K = S;
      b.xmax = al_xmax_put(S, /*r=*/q, /*q=*/r);
      return al_put_price_from_boundary(b, w, /*spot=*/K, /*strike=*/S, T, sig2, /*r=*/q,
                                        /*q=*/r);
    }
    return al_put_price_from_boundary(b, w, S, K, T, sig2, r, q);
  };
  const double vvp = px(bvp, wvp, sigma + hv);
  const double vvm = px(bvm, wvm, sigma - hv);
  return Ok((vvp - vvm) / (2.0 * hv));
}
// ─────────────────────────────────────────────────────────────────────────

Result<double> american_delta(double S, double K, double T, double sigma, double r,
                              double q, Side side, AmericanMethod method,
                              const std::optional<AlOpts>& opts) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "american_delta: S, K, T, sigma must be > 0");
  }
  const double hS = 1.0e-3 * S;  // same spot step as american_greeks_fd

  // Put fast path (AndersenLake): the exercise boundary is spot-independent, so
  // BOTH spot stencils reprice against ONE base boundary — delta in a single
  // boundary solve + two price-from-boundary evals, BIT-IDENTICAL to
  // american_greeks_fd's put delta (identical stencil, step, and guard chain).
  if (side == Side::Put && method == AmericanMethod::AndersenLake) {
    const AlScheme sch = scheme_from_opts(opts);
    AlBoundary bnd{};
    AlWorkspace ws{};
    bool have_bnd = false;
    bool bnd_ok = false;
    bool failed = false;
    atx::core::Error first_err;
    // Price a put at spot S2, mirroring american_greeks_fd's Pput base stencil
    // exactly (degenerate -> intrinsic, r<=0 -> European Black-76, else the shared
    // AL boundary; a collapsed boundary falls back to the scalar cold price).
    const auto put_px = [&](double S2) -> double {
      if (failed) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      if (T <= 1.0e-12 || sigma <= 1.0e-8) {
        const double intr = K - S2;
        return (intr > 0.0) ? intr : 0.0;
      }
      // European put -> Black-76; the double-continuation corner (q < r <= 0) is
      // unpriceable -> surface andersen_lake's NotImplemented via american_price.
      switch (classify_regime(/*rate=*/r, /*yield=*/q)) {
        case ExerciseRegime::European:
          return black76_price(S2 * std::exp((r - q) * T), K, T, sigma,
                               std::exp(-r * T), Side::Put);
        case ExerciseRegime::Unsupported: {
          const Result<double> p =
              american_price(S2, K, T, sigma, r, q, Side::Put, method, opts);
          if (!p) {
            failed = true;
            first_err = p.error();
            return std::numeric_limits<double>::quiet_NaN();
          }
          return *p;
        }
        case ExerciseRegime::American:
          break;
      }
      if (!have_bnd) {
        bnd_ok = (al_solve_put_boundary(K, T, sigma, r, q, sch, bnd, ws) == AlSolveStatus::Ok);
        have_bnd = true;
      }
      if (!bnd_ok) {
        const Result<double> p = american_price(S2, K, T, sigma, r, q, Side::Put, method, opts);
        if (!p) {
          failed = true;
          first_err = p.error();
          return std::numeric_limits<double>::quiet_NaN();
        }
        return *p;
      }
      return al_put_price_from_boundary(bnd, ws, S2, K, T, sigma, r, q);
    };
    const double pSp = put_px(S + hS);
    const double pSm = put_px(S - hS);
    if (failed) {
      return Err(std::move(first_err));
    }
    return Ok((pSp - pSm) / (2.0 * hS));
  }

  // General path (calls, BAW, or anything not on the put-AL fast lane): the same
  // central difference on the cold `american_price` that american_greeks_fd runs —
  // two solves, and bit-identical to its delta.
  const Result<double> pSp = american_price(S + hS, K, T, sigma, r, q, side, method, opts);
  if (!pSp) {
    return Err(pSp.error());
  }
  const Result<double> pSm = american_price(S - hS, K, T, sigma, r, q, side, method, opts);
  if (!pSm) {
    return Err(pSm.error());
  }
  return Ok((*pSp - *pSm) / (2.0 * hS));
}

namespace detail {

GaussLegendre gauss_legendre(unsigned n) {
  const GaussLegendre* t = gl_find(n);
  return t ? *t : GaussLegendre{};
}

Result<double> andersen_lake_generic_kernel(double S, double K, double T,
                                            double sigma, double r, double q,
                                            Side side,
                                            const std::optional<AlOpts>& opts) {
  return andersen_lake_core(S, K, T, sigma, r, q, side, opts, /*specialize=*/false);
}

int al_boundary_jn_sweeps_to_converge(double K, double T, double sigma, double r,
                                      double q, const std::optional<AlOpts>& opts,
                                      AlSeedMode seed, double tol, int max_sweeps) {
  const AlScheme sch = scheme_from_opts(opts);
  AlBoundary bnd;
  AlWorkspace ws;
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    return -1;
  }
  const GaussLegendre* fp = gl_find(sch.n_quad_fp);
  const GaussLegendre* pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return -1;
  }
  ws.specialize = true;
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;
  al_bind_geometry(bnd, ws, sigma, r, q);

  if (seed == AlSeedMode::QdPlus) {
    al_seed_boundary_qdplus(bnd, sigma, r, q);
  } else if (seed == AlSeedMode::Oracle) {
    // Converge to the near-exact fixed point, then measure from THAT state — the
    // best any analytic seed could do (the lower bound on sweeps-to-converge).
    al_seed_boundary(bnd, sigma, r, q);
    for (int k = 0; k < 80; ++k) {
      if (al_jacobi_newton_sweep(bnd, ws, sigma, r, q) <= 1.0e-15) {
        break;
      }
    }
  } else {
    al_seed_boundary(bnd, sigma, r, q);
  }

  for (int k = 0; k < max_sweeps; ++k) {
    if (al_jacobi_newton_sweep(bnd, ws, sigma, r, q) <= tol) {
      return k + 1;
    }
  }
  return max_sweeps;
}

}  // namespace detail

}  // namespace atx::vol
