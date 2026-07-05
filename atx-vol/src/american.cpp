#include "atx/vol/american.hpp"

#include <array>
#include <cmath>
#include <limits>

#include "atx/core/math.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/greeks.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

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
  const std::array<detail::GaussLegendre, 6>& all = gl_tables();
  for (const detail::GaussLegendre& t : all) {
    if (t.n == n) {
      return &t;
    }
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
[[nodiscard]] double al_cheb_eval(const double* z, const double* w,
                                  const double* y, unsigned n, double zq) noexcept {
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

struct AlWorkspace {
  const double* qx_fp = nullptr;
  const double* qw_fp = nullptr;
  unsigned n_quad_fp = 0;
  const double* qx_price = nullptr;
  const double* qw_price = nullptr;
  unsigned n_quad_price = 0;
  std::array<double, kAlMaxNodes> next_y{};  // iteration scratch
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

// Equation B kernel: N(τ,b), D(τ,b).
void eqn_b_ND(const AlBoundary& bnd, const AlWorkspace& ws, double tau,
              double b_val, double sigma, double r, double q, double& N_out,
              double& D_out) noexcept {
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

  const double* xs = ws.qx_fp;
  const double* wv = ws.qw_fp;
  const unsigned nq = ws.n_quad_fp;
  double n_int = 0.0;
  double d_int = 0.0;
  const double half_tau = 0.5 * tau;
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

[[nodiscard]] double al_jacobi_newton_sweep(AlBoundary& b, AlWorkspace& ws,
                                            double sigma, double r, double q) noexcept {
  double max_dy = 0.0;
  ws.next_y[0] = 0.0;
  for (std::uint16_t i = 1; i < b.n; ++i) {
    const double tau = b.tau[i];
    if (tau <= 1.0e-14) {
      ws.next_y[i] = 0.0;
      continue;
    }
    const double b_val = b_from_y(b.y[i], b.xmax);
    double Nv = 0.0;
    double Dv = 0.0;
    eqn_b_ND(b, ws, tau, b_val, sigma, r, q, Nv, Dv);
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
  for (std::uint16_t i = 0; i < b.n; ++i) {
    b.y[i] = ws.next_y[i];
  }
  return max_dy;
}

[[nodiscard]] double al_fixed_point_sweep(AlBoundary& b, AlWorkspace& ws,
                                          double sigma, double r, double q) noexcept {
  double max_dy = 0.0;
  ws.next_y[0] = 0.0;
  for (std::uint16_t i = 1; i < b.n; ++i) {
    const double tau = b.tau[i];
    if (tau <= 1.0e-14) {
      ws.next_y[i] = 0.0;
      continue;
    }
    const double b_val = b_from_y(b.y[i], b.xmax);
    double Nv = 0.0;
    double Dv = 0.0;
    eqn_b_ND(b, ws, tau, b_val, sigma, r, q, Nv, Dv);
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
  for (std::uint16_t i = 0; i < b.n; ++i) {
    b.y[i] = ws.next_y[i];
  }
  return max_dy;
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
  return 2.0 * z *
         (r * b.K * dr * norm_cdf(-dp + v) - q * S * dq * norm_cdf(-dp));
}

[[nodiscard]] double al_put_premium(const AlBoundary& b, const AlWorkspace& ws,
                                    double S, double sigma, double r,
                                    double q) noexcept {
  const double sqrtT = std::sqrt(b.T);
  const double half_sqrtT = 0.5 * sqrtT;
  double total = 0.0;
  const double* xs = ws.qx_price;
  const double* wv = ws.qw_price;
  const unsigned nq = ws.n_quad_price;
  for (unsigned i = 0; i < nq; ++i) {
    const double zi = half_sqrtT * (1.0 + xs[i]);
    total += wv[i] * premium_integrand_put(zi, b, S, sigma, r, q);
  }
  total *= half_sqrtT;
  return (total > 0.0) ? total : 0.0;
}

// Put core — used directly for puts and via McDonald-Schroder for calls.
[[nodiscard]] Result<double> al_solve_put(double S, double K, double T,
                                          double sigma, double r, double q,
                                          const AlScheme& sch) {
  const double euro = euro_put_sk(S, K, T, sigma, r, q);

  if (r <= 0.0) {
    const double intr = K - S;
    const double price = (euro > intr) ? euro : (intr > 0.0 ? intr : 0.0);
    return Ok(price);
  }

  AlBoundary bnd;
  AlWorkspace ws;
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);

  if (!(bnd.xmax > 0.0)) {
    // Negative-rate/carry corner: AL cannot run. Flagged unsupported.
    return Err(ErrorCode::NotImplemented,
               "andersen_lake: asymptotic boundary collapsed (xmax <= 0)");
  }

  const detail::GaussLegendre* fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre* pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return Err(ErrorCode::Internal, "andersen_lake: Gauss-Legendre table unavailable");
  }
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;

  al_seed_boundary(bnd, sigma, r, q);

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
  return Ok(price);
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
    correction->eval_grad(k_up, T, sigma, nullptr, nullptr, &dc_ds_up);
    correction->eval_grad(k_dn, T, sigma, nullptr, nullptr, &dc_ds_dn);
    const double d2c_dF_ds = (dc_ds_up - dc_ds_dn) / (2.0 * hF);
    out.vanna = m * (gB.vanna + dc_ds + F * d2c_dF_ds);

    double dc_ds_p = 0.0;
    double dc_ds_m = 0.0;
    correction->eval_grad(k_log, T, sigma + hS, nullptr, nullptr, &dc_ds_p);
    correction->eval_grad(k_log, T, sigma - hS, nullptr, nullptr, &dc_ds_m);
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

// ── Public API ──────────────────────────────────────────────────────────

AlOpts al_default_opts() noexcept { return AlOpts{12, 24, 8, 1.0e-10}; }

AlOpts al_fast_opts() noexcept { return AlOpts{7, 16, 4, 1.0e-8}; }

Result<double> andersen_lake(double S, double K, double T, double sigma,
                             double r, double q, Side side,
                             const std::optional<AlOpts>& opts) {
  if (!(K > 0.0 && S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake: S and K must be > 0");
  }
  if (!(T >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake: T must be >= 0");
  }
  if (!(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake: sigma must be >= 0");
  }

  // Degenerate: T ~ 0 or sigma ~ 0 collapses to intrinsic.
  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    const double intr = (side == Side::Call) ? (S - K) : (K - S);
    return Ok(intr > 0.0 ? intr : 0.0);
  }

  // No-early-exercise short-circuits: American == European.
  if (side == Side::Call && q <= 0.0) {
    return Ok(black76_price(S * std::exp((r - q) * T), K, T, sigma,
                            std::exp(-r * T), Side::Call));
  }
  if (side == Side::Put && r <= 0.0) {
    return Ok(black76_price(S * std::exp((r - q) * T), K, T, sigma,
                            std::exp(-r * T), Side::Put));
  }

  const AlScheme sch = scheme_from_opts(opts);
  if (side == Side::Put) {
    return al_solve_put(S, K, T, sigma, r, q, sch);
  }
  // McDonald-Schroder symmetry: C(S,K,r,q) = P(K,S,q,r). Swap (S↔K), (r↔q).
  return al_solve_put(K, S, T, sigma, q, r, sch);
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

  const std::uint16_t mi = max_iter ? max_iter : std::uint16_t{16};
  const double tt = (tol > 0.0) ? tol : 1.0e-8;

  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    const double intr = (side == Side::Call) ? (S - K) : (K - S);
    return Ok(intr > 0.0 ? intr : 0.0);
  }

  const double euro =
      (side == Side::Call) ? euro_call_sk(S, K, T, sigma, r, q)
                           : euro_put_sk(S, K, T, sigma, r, q);

  if (side == Side::Call && q <= 0.0) {
    return Ok(euro);
  }
  if (side == Side::Put && r <= 0.0) {
    return Ok(euro);
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
    const Result<double> p = andersen_lake(S, K, T, sigma, r, q, side, std::nullopt);
    return p ? *p : std::numeric_limits<double>::quiet_NaN();
  }
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  const double euro = black76_price(F, K, T, sigma, df, side);
  const double k_log = std::log(K / F);
  const double corr = correction->eval(k_log, T, sigma);
  return euro + F * corr;
}

Result<AmericanGreeks> american_greeks(double S, double K, double T,
                                       double sigma, double r, double q,
                                       Side side,
                                       const CorrectionCache* correction) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_greeks: S, K, T, sigma must be > 0");
  }
  AmericanGreeks out;
  american_greeks_first_order(S, K, T, sigma, r, q, side, correction, out);
  return Ok(out);
}

namespace detail {

GaussLegendre gauss_legendre(unsigned n) {
  const GaussLegendre* t = gl_find(n);
  return t ? *t : GaussLegendre{};
}

}  // namespace detail

}  // namespace atx::vol
