#include "atx/vol/american.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "american_boundary.hpp" // amer:: seam (structs + boundary-solve decls)
#include "atx/core/math.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/counters.hpp" // ATX_VOL_COUNT (opt-in P0.2; no-op when OFF)
#include "atx/vol/greeks.hpp"

namespace atx::vol {

// The Andersen-Lake boundary primitives (types, constants, solve/price helpers)
// live in `namespace amer` (american_boundary.hpp) so boundary_interp.cpp can
// reuse them. Bring them into scope unqualified — every existing call site below
// (and the anonymous-namespace helpers) resolves them exactly as before.
using namespace amer;

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

// R-30: Release-mode observability for the al_bind_geometry_sigma safety fallback.
// When a specialized scheme reaches sigma-bind with the sweep-invariant static
// geometry unexpectedly unbound, the kernel drops to the generic runtime-trip-count
// path (ws.specialize = false) rather than consume indeterminate geometry. That path
// must never fire on a production flow — reset() always binds before price() reuses —
// so a non-zero tally flags a retained-workspace lifecycle bug (the obs-23864 shape).
// Relaxed ordering: a monotonic diagnostic counter with no cross-thread invariant.
// Not part of the ATX_VOL_COUNTERS enum (counters.hpp is owned by another sprint);
// exposed via al_geometry_specialize_off_fallback_count() for tests/observability.
std::atomic<std::uint64_t> g_specialize_off_fallbacks{0};

// clang-cl / MSVC do not define M_PI; carry the extended literal explicitly.
inline constexpr double kPi = 3.14159265358979323846;

// Hard limits — every per-solve buffer is stack-bounded (matches C ATS_AL_*).
// kAlMaxNodes now lives in namespace amer (american_boundary.hpp); in scope here
// via the `using namespace amer;` above.
inline constexpr unsigned kAlMaxQuad = detail::kMaxQuadNodes; // 64

// ── European legs (Black-76 reuse) ──────────────────────────────────────
//
// With F = S·e^{(r-q)T} and df = e^{-rT}, black76 reproduces the (S,K,r,q)
// European put/call the C library computed inline. T,sigma are guaranteed > 0
// at every call site here, so the degenerate black76 branch never fires.
[[nodiscard]] double euro_put_sk(double S, double K, double T, double sigma, double r,
                                 double q) noexcept {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T), Side::Put);
}
[[nodiscard]] double euro_call_sk(double S, double K, double T, double sigma, double r,
                                  double q) noexcept {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T), Side::Call);
}

[[nodiscard]] double d1_of(double S, double K, double r, double q, double sigma,
                           double T) noexcept {
  const double v = sigma * std::sqrt(T);
  return (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / v;
}

// ── Barone-Adesi-Whaley smooth-pasting root find ────────────────────────

[[nodiscard]] double put_residual(double Sx, double K, double T, double sigma, double r, double q,
                                  double q1) noexcept {
  const double pE = euro_put_sk(Sx, K, T, sigma, r, q);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double bit = 1.0 - std::exp(-q * T) * norm_cdf(-d1);
  return K - Sx - pE + Sx * bit / q1;
}
[[nodiscard]] double put_residual_deriv(double Sx, double K, double T, double sigma, double r,
                                        double q, double q1) noexcept {
  const double v = sigma * std::sqrt(T);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double Nm = norm_cdf(-d1);
  const double phim = norm_pdf(-d1);
  const double dq = std::exp(-q * T);
  // A1 (finding 1): the phi-term is +, not -. put_residual = K - Sx - pE + Sx*bit/q1
  // with bit = 1 - e^{-qT}N(-d1); d(bit)/dSx = +e^{-qT}phi(d1)/(Sx*v) (N(-d1) falls
  // as Sx rises), so d/dSx[Sx*bit/q1] contributes +dq*phim/(q1*v). FD-verified.
  return -1.0 + dq * Nm + (1.0 - dq * Nm) / q1 + dq * phim / (q1 * v);
}
[[nodiscard]] double call_residual(double Sx, double K, double T, double sigma, double r, double q,
                                   double q2) noexcept {
  const double cE = euro_call_sk(Sx, K, T, sigma, r, q);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double bit = 1.0 - std::exp(-q * T) * norm_cdf(d1);
  return Sx - K - cE - Sx * bit / q2;
}
[[nodiscard]] double call_residual_deriv(double Sx, double K, double T, double sigma, double r,
                                         double q, double q2) noexcept {
  const double v = sigma * std::sqrt(T);
  const double d1 = d1_of(Sx, K, r, q, sigma, T);
  const double Np = norm_cdf(d1);
  const double phip = norm_pdf(d1);
  const double dq = std::exp(-q * T);
  // A1 (finding 1): the phi-term is +, not - (McDonald-Schroder symmetric to the
  // put). call_residual = Sx - K - cE - Sx*bit/q2 with bit = 1 - e^{-qT}N(d1);
  // d(bit)/dSx = -e^{-qT}phi(d1)/(Sx*v), so -d/dSx[Sx*bit/q2] gives +dq*phip/(q2*v).
  return 1.0 - dq * Np - (1.0 - dq * Np) / q2 + dq * phip / (q2 * v);
}

// A1 convergence contract (finding 8). Reports how the safeguarded critical-price
// Newton terminated so callers can reject a silently non-converged root and tests
// can pin the iteration count. `converged` is true iff a Newton/step tolerance
// test fired INSIDE the loop (NOT max_iter bisection exhaustion); `iters` counts
// executed iterations; `residual` is the signed residual f at the returned Sx.
// The fields are populated only when a non-null `stats` is passed, so the
// production seed/pricer paths (nullptr) are numerically and performance-identical
// to before — the only behavior change on those paths is the finding-1 sign fix.
struct NewtonCriticalStats {
  std::uint16_t iters = 0;
  double residual = 0.0;
  bool converged = false;
};

// A1 (finding 8): baw_american accepts a critical price only if its residual is
// within this multiple of the solve tolerance scale (tol*K). Generous margin
// (Newton converges to << tol*K post-fix) that still rejects the off-root
// bisection midpoint the pre-fix loop returned on max_iter exhaustion.
inline constexpr double kBawCriticalResidualGate = 1.0e2;

[[nodiscard]] double newton_critical_put(double K, double T, double sigma, double r, double q,
                                         double q1, std::uint16_t max_iter, double tol,
                                         NewtonCriticalStats *stats = nullptr) noexcept {
  double lo = 1.0e-3 * K;
  double hi = K * (1.0 - 1.0e-6);
  double Sx = K * q1 / (q1 - 1.0);
  if (!(Sx > lo && Sx < hi)) {
    Sx = 0.5 * (lo + hi);
  }
  std::uint16_t it = 0;
  for (; it < max_iter; ++it) {
    const double f = put_residual(Sx, K, T, sigma, r, q, q1);
    const double fp = put_residual_deriv(Sx, K, T, sigma, r, q, q1);
    if (f > 0.0) {
      lo = Sx;
    } else {
      hi = Sx;
    }
    if (std::fabs(f) < tol * K) {
      if (stats) {
        *stats = {static_cast<std::uint16_t>(it + 1), f, true};
      }
      return Sx;
    }
    double Sx_new = (std::fabs(fp) > 1.0e-15) ? (Sx - f / fp) : 0.5 * (lo + hi);
    if (Sx_new <= lo || Sx_new >= hi) {
      Sx_new = 0.5 * (lo + hi);
    }
    const double dS = Sx_new - Sx;
    Sx = Sx_new;
    if (std::fabs(dS) < tol * K) {
      if (stats) {
        *stats = {static_cast<std::uint16_t>(it + 1),
                  put_residual(Sx, K, T, sigma, r, q, q1), true};
      }
      return Sx;
    }
  }
  if (stats) {
    *stats = {it, put_residual(Sx, K, T, sigma, r, q, q1), false};
  }
  return Sx;
}
[[nodiscard]] double newton_critical_call(double K, double T, double sigma, double r, double q,
                                          double q2, std::uint16_t max_iter, double tol,
                                          NewtonCriticalStats *stats = nullptr) noexcept {
  double lo = K * (1.0 + 1.0e-6);
  double hi = K * 50.0;
  double Sx = K * q2 / (q2 - 1.0);
  if (!(Sx > lo && Sx < hi)) {
    Sx = 0.5 * (lo + hi);
  }
  std::uint16_t it = 0;
  for (; it < max_iter; ++it) {
    const double f = call_residual(Sx, K, T, sigma, r, q, q2);
    const double fp = call_residual_deriv(Sx, K, T, sigma, r, q, q2);
    if (f < 0.0) {
      lo = Sx;
    } else {
      hi = Sx;
    }
    if (std::fabs(f) < tol * K) {
      if (stats) {
        *stats = {static_cast<std::uint16_t>(it + 1), f, true};
      }
      return Sx;
    }
    double Sx_new = (std::fabs(fp) > 1.0e-15) ? (Sx - f / fp) : 0.5 * (lo + hi);
    if (Sx_new <= lo || Sx_new >= hi) {
      Sx_new = 0.5 * (lo + hi);
    }
    const double dS = Sx_new - Sx;
    Sx = Sx_new;
    if (std::fabs(dS) < tol * K) {
      if (stats) {
        *stats = {static_cast<std::uint16_t>(it + 1),
                  call_residual(Sx, K, T, sigma, r, q, q2), true};
      }
      return Sx;
    }
  }
  if (stats) {
    *stats = {it, call_residual(Sx, K, T, sigma, r, q, q2), false};
  }
  return Sx;
}

// Put critical price S* — the Jacobi-Newton seed for the AL boundary. Returns
// false on degenerate input (matches the C -1 status); the several
// "no early exercise" corners return true with S* = K.
[[nodiscard]] bool baw_critical_put(double K, double T, double sigma, double r, double q,
                                    std::uint16_t max_iter, double tol, double &Sx_out) noexcept {
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
  const double Sx = newton_critical_put(
      K, T, sigma, r, q, q1, max_iter ? max_iter : std::uint16_t{16}, tol > 0.0 ? tol : 1.0e-10);
  if (!(Sx > 0.0 && Sx <= K)) {
    return false;
  }
  Sx_out = Sx;
  return true;
}

// ── QD+ critical-price seed (Li 2010) ─────────────────────────────────────
//
// The QD+ approximation refines the QD/Barone-Adesi-Whaley quadratic exponent with
// the leading Li (2010) "+" correction. In Li's derivation the SIGNED correction is
// (1−h)·M / (h·(2·q1 + N − 1)); since 2·q1 + N − 1 == −√disc (the residual-derivative
// denominator, see the code below), it equals −(1−h)·M / (h·√disc). Writing the
// positive MAGNITUDE c = (1−h)·M / (h·√disc), the corrected exponent is q1⁺ = q1 − c
// (A9 core-review finding 7: the code's q1 − c is correct; an earlier version of THIS
// comment said "q1 + c", dropping the sign of the 2·q1+N−1 = −√disc denominator).
// It STEEPENS the exponent (drives q1 more negative) near expiry (h→0), where the
// frozen-θ QD approximation is worst and the true boundary S* → K, and vanishes as
// τ→∞ (h→1, QD/BAW recovered). q1⁺ drives the SAME smooth-pasting root find
// (put_residual with q1⁺ in place of q1), so this reuses newton_critical_put
// unchanged. Measurement-only path (QdPlus is not on any production solve), so this
// is a pure doc reconciliation — no behavior change, no A6 shootout re-run implied.
//
// Reference: M. Li, "Analytical Approximations for the Critical Stock Price of
// American Options: A Performance Comparison" (2010), Review of Derivatives
// Research 13(1): 75-99 — the QD+ (quadratic + O(h) drift) seed. The fast-tier
// American price envelope this seed was hypothesized to help close is the
// Andersen-Lake-Offengenden QdFp benchmark, L. Andersen, M. Lake, D. Offengenden,
// "High-Performance American Option Pricing", SSRN 2547027 (2015) (~10–22 µs/op).
//
// Task A6 A/B'd this seed against BAW on the american-shootout price grid (see
// american_shootout_bench.cpp rows american/price/fast_{baw16,qdplus16,qdplus8,
// baw8}). RESULT — document-defer: under the truncated fast-tier sweep budget
// (2 JN + 2 FP) the QD+ seed REGRESSED max abs price error vs the reference from
// 1.44e-3 (BAW, the current fast-tier bound) to 4.62e-3, and trimming the premium
// quadrature 16→8 pushed it further (BAW/8 = 4.90e-3), so there is no accuracy
// headroom to trade for throughput and every production scheme keeps BAW. QdPlus
// stays SELECTABLE per-scheme (AlScheme::seed) so the shootout A/B and the
// al_boundary_jn_sweeps_to_converge seed-count spike keep measuring it; it is NOT
// on any production solve path.
[[nodiscard]] bool qdplus_critical_put(double K, double T, double sigma, double r, double q,
                                       std::uint16_t max_iter, double tol,
                                       double &Sx_out) noexcept {
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
  const double Sx =
      newton_critical_put(K, T, sigma, r, q, q1_plus, max_iter ? max_iter : std::uint16_t{16},
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
[[nodiscard]] bool tqli_first_row(double *d, double *e, double *z, int n) noexcept {
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

void sort_pairs(double *d, double *z, int n) noexcept {
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
[[nodiscard]] bool build_gl_table(unsigned n, double *xs, double *ws) noexcept {
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
[[nodiscard]] const std::array<detail::GaussLegendre, 6> &gl_tables() {
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
[[nodiscard]] const detail::GaussLegendre *gl_find(unsigned n) {
  // The six supported orders are static and scheme-fixed, so resolve directly to
  // the cached table by a constant-time switch (no per-solve linear scan) — a hot
  // loop of solves binds its Gauss-Legendre tables in O(1) each (P2.2 §2). Index
  // order matches gl_tables()'s {8,16,24,32,48,64}.
  const std::array<detail::GaussLegendre, 6> &all = gl_tables();
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
[[nodiscard]] double al_cheb_eval_t(const double *z, const double *w, const double *y,
                                    unsigned n_rt, double zq) noexcept {
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
[[nodiscard]] double al_cheb_eval(const double *z, const double *w, const double *y, unsigned n,
                                  double zq) noexcept {
  return al_cheb_eval_t<0>(z, w, y, n, zq);
}

// A6 (PR-P2): the HOISTED counterpart of al_cheb_eval_t for the specialized kernel.
//
// `qq[k]` holds wbary[k] / (zq - z[k]) and `den` their running sum — both laid down
// once per solve by al_bind_geometry_static from the SAME two doubles, with the same
// operator, accumulated in the same left-to-right order. `num` here accumulates the
// same products in the same order over the sweep-varying y[]. So
//
//     al_cheb_eval_hoisted<NB>(qq, den, hit, y) == al_cheb_eval_t<NB>(z, w, y, n, zq)
//
// BIT-for-bit, not merely to a tolerance — which is the whole justification for the
// hoist and is gated by BoundaryHoist.SpecializedMatchesGeneric (the generic kernel
// is untouched and still evaluates the inline form) plus
// BoundaryHoist.HoistedBaryTableMatchesInlineFormula (the table itself, entry by
// entry). `hit >= 0` reproduces the inline `dz == 0.0` early return.
template <unsigned NB>
[[nodiscard]] double al_cheb_eval_hoisted(const double *qq, double den, int hit,
                                          const double *y) noexcept {
  if (hit >= 0) {
    return y[static_cast<unsigned>(hit)];
  }
  double num = 0.0;
  for (unsigned k = 0; k < NB; ++k) {
    num += qq[k] * y[k];
  }
  return num / den;
}

// ── AL boundary state + scheme ──────────────────────────────────────────
//
// AlScheme / AlBoundary / AlWorkspace and the kGeo* geometry-sizing constants
// now live in namespace amer (american_boundary.hpp); in scope here via the
// file-level `using namespace amer;`.

// ── P2.2 sweep-invariant geometry precompute sizing ──────────────────────
//
// eqn_b_ND's inner fixed-point quadrature recomputes, every JN+FP sweep, a block
// of quantities that depend only on (tau_j, quad-node xs_i, T, sigma, r, q) — all
// fixed for a solve. Only the two PRODUCTION fixed schemes are specialized and use
// the precompute: fast {n_boundary=7, n_quad_fp=16} and accurate/explicit-default
// {12, 24}. The generic (arbitrary AlOpts) path stays the un-hoisted scalar
// reference. So the per-solve geometry table needs only cover those (nb, nq); the
// strides carry headroom above the largest specialized scheme.
// kGeoNodeMax / kGeoQuadStride / kGeoSize now live in namespace amer
// (american_boundary.hpp); in scope here via the file-level `using namespace amer;`.

// Is (nb, nq) a specialized fixed-point scheme (compile-time trip counts + geometry
// hoist)? Kept in ONE place so the sweep dispatch, premium dispatch, and geometry
// bind agree on exactly which schemes take the hoisted path.
[[nodiscard]] constexpr bool al_fp_specialized(unsigned nb, unsigned nq) noexcept {
  // (7,8) is the K2 ql_fast marks rung (docs/al-preset-ladder.md §4): a cheap
  // fixed-point quadrature (l=8) with a decoupled rich premium (p=32). Hoisting it
  // gives the scalar marks path compile-time trip counts + the geometry precompute,
  // removing the generic-path tax the ladder note §4 flagged so the AVX2-batch ship
  // gate compares against an honest (specialized) scalar baseline at the tier that
  // actually ships. (7,8) fits kGeoNodeMax=16 / kGeoQuadStride=32 (american_boundary.hpp).
  return (nb == 7 && nq == 8) || (nb == 7 && nq == 16) || (nb == 12 && nq == 24);
}

// A6 / REVWSA finding 4: the per-solve geometry sizing guard, DERIVED from
// al_fp_specialized above instead of hand-copied from it. The original shipped as
// three `static_assert`s that restated (7,8) / (7,16) / (12,24) literally, so a
// FOURTH scheme admitted above without growing the tables would have compiled
// cleanly and then silently taken the runtime `specialize = false` fallback in
// al_bind_geometry_static — losing the hoist with no diagnostic anywhere. Letting
// the predicate itself decide membership makes that a BUILD failure.
//
// Does (nb, nq) fit EVERY table al_bind_geometry_static writes? There are THREE
// independent sizings and ALL THREE are required. Each was found the same way, by
// someone enumerating what the bind actually writes rather than trusting the prose
// beside it: REVA7FIX Minor 3 found the guard covered only the first, REVA7TIDY
// found the corrected guard still did not cover the second and that the comment
// here asserted otherwise. State the index and the cap for each, and do not claim
// one bound implies another without checking the CONSTANT, not just the exponent:
//
//   * geo_bary — PACKED at (bpair + i)*nb with k < nb, so its largest written index
//     is (nb-1)*nq*nb - 1 and it needs (nb-1)*nq*nb <= kGeoBarySize (3168).
//   * geo_bary_den / geo_bary_hit — packed at bpair + i, so their largest written
//     index is (nb-1)*nq - 1 and they need (nb-1)*nq <= kGeoBaryPairs (264). This is
//     a SEPARATE term. kGeoBaryPairs is kGeoBarySize / kGeoBaryNodeMax — a factor of
//     kGeoBaryNodeMax smaller — so the geo_bary bound above does NOT imply it, and
//     the smaller index is written into a proportionally smaller array. (A previous
//     revision of this comment claimed the geo_bary bound covered these two "since
//     nb >= 1". It does bound the index by 3168; these arrays hold 264. (10,32) is
//     the counterexample: it passes the geo_bary bound at 9*32*10 = 2880 <= 3168 and
//     then writes geo_bary_den[264..287] / geo_bary_hit[264..287], 24 elements past
//     the end — and in a Release build geo_bary_hit is the LAST AlWorkspace member,
//     so that write leaves the object. Pinned as a compiled counterexample below.)
//   * geo_zc / geo_weru / geo_wequ — ROW-ADDRESSED at gbase + i with
//     gbase = j*kGeoQuadStride, j < nb, i < nq, into kGeoSize = kGeoNodeMax *
//     kGeoQuadStride doubles. That needs BOTH bounds separately, and the packed bary
//     bound implies NEITHER:
//       - nq <= kGeoQuadStride, or row j spills past its own stride into row j+1's
//         slots. (7,40) passes the bary bound (6*40*7 = 1680 <= 3168) and silently
//         OVERLAPS adjacent rows — no overflow, just corruption.
//       - nb <= kGeoNodeMax, or the last row runs off the end of the array. (17,8)
//         passes the bary bound (16*8*17 = 2176 <= 3168) and writes geo_zc[512..519]
//         OUT OF BOUNDS on a 512-element array.
//     Both are pinned as compiled counterexamples below, not left as prose.
[[nodiscard]] constexpr bool al_scheme_fits_geometry_tables(unsigned nb,
                                                            unsigned nq) noexcept {
  return nb >= 1u && nb <= kGeoNodeMax && nq <= kGeoQuadStride &&
         (nb - 1u) * nq <= kGeoBaryPairs &&
         (nb - 1u) * nq * nb <= kGeoBarySize;
}

// The three live schemes fit; the four ways to break it do not. These pin the
// per-scheme predicate directly, in both directions: one negative per conjunct that
// can reject (nb >= 1u only guards the nb - 1u wrap and short-circuits ahead of it),
// so a predicate that degenerated to `true` or to `false` fails to compile.
//
// What they do NOT pin, so nobody reads more into them than is there (REVA7TIDY):
// they say nothing about the SWEEP below, which still returns true VACUOUSLY if
// al_fp_specialized is ever narrowed to admit nothing. That is harmless — a scheme
// set admitting nothing makes al_bind_geometry_static return before it writes — but
// it is not a property these asserts establish. Nor is anything here tied to
// al_fp_specialized still ADMITTING (7,8) / (7,16) / (12,24): all three positives are
// on this predicate alone, so narrowing the scheme list silently loses hoists without
// tripping any of them. `static_assert(al_fp_specialized(12, 24));` (x3) would close
// that; left for a separate pass rather than smuggled into a wording fix.
static_assert(al_scheme_fits_geometry_tables(7, 8));
static_assert(al_scheme_fits_geometry_tables(7, 16));
static_assert(al_scheme_fits_geometry_tables(12, 24));
static_assert(!al_scheme_fits_geometry_tables(13, 24));  // 12*24*13 = 3744 > geo_bary
static_assert(!al_scheme_fits_geometry_tables(17, 8));   // geo_zc[512..519], OOB
static_assert(!al_scheme_fits_geometry_tables(7, 40));   // nq > row stride, rows overlap
static_assert(!al_scheme_fits_geometry_tables(10, 32));  // geo_bary_den/hit[264..287], OOB

// The sweep bound is DERIVED, not chosen (REVA7FIX Minor 4). It is exactly the
// domain in which al_bind_geometry_static can ever be REACHED, so a scheme outside
// it cannot corrupt anything and needs no assert:
//   * nb <= kAlMaxNodes — AlBoundary's z/wbary/x/tau/y are std::array<double,
//     kAlMaxNodes> and al_init_nodes writes b.z[i] for i < n, so a larger
//     n_boundary is already unrepresentable; scheme_from_opts does not even let an
//     out-of-range n_collocation through — it IGNORES it and keeps the default 12
//     (it does NOT clamp; the safety property is the same, the verb is not).
//   * nq <= kAlMaxQuad — n_quad_fp is only ever one of the six Gauss-Legendre
//     orders {8,16,24,32,48,64} that gl_tables() builds and gl_find() resolves;
//     al_gauss_legendre rejects n > kAlMaxQuad outright, so there is no table to
//     bind above it.
[[nodiscard]] constexpr bool al_geometry_tables_fit_every_specialized_scheme() noexcept {
  for (unsigned nb = 1; nb <= unsigned{kAlMaxNodes}; ++nb) {
    for (unsigned nq = 1; nq <= kAlMaxQuad; ++nq) {
      if (al_fp_specialized(nb, nq) && !al_scheme_fits_geometry_tables(nb, nq)) {
        return false;
      }
    }
  }
  return true;
}
static_assert(al_geometry_tables_fit_every_specialized_scheme(),
              "a scheme al_fp_specialized() admits does not fit the per-solve geometry "
              "tables: grow kGeoBaryNodeMax / kGeoBaryQuadMax (which size BOTH "
              "kGeoBarySize for geo_bary and kGeoBaryPairs for geo_bary_den / "
              "geo_bary_hit) and/or kGeoNodeMax / kGeoQuadStride (geo_zc / geo_weru / "
              "geo_wequ) in american_boundary.hpp, or the new scheme silently loses the "
              "barycentric hoist at runtime — or, for the geo_zc triple and for "
              "geo_bary_den / geo_bary_hit, writes out of bounds");

// AlWorkspace now lives in namespace amer (american_boundary.hpp).

} // namespace
// linkage in atx::vol::amer (matching american_boundary.hpp). The file's
// anonymous namespace is reopened right after al_xmax_put.

// scheme_from_opts / al_xmax_put are part of the boundary seam (amer). Definitions
// stay here; declarations are in american_boundary.hpp.
namespace amer {

// ACCURATE preset when opts == nullopt; otherwise map the public knobs.
[[nodiscard]] AlScheme scheme_from_opts(const std::optional<AlOpts> &opts) noexcept {
  AlScheme s; // {12, 24, 48, 2, 4, 1e-10}
  if (!opts) {
    return s;
  }
  const AlOpts &o = *opts;
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
  } else {
    // A9 (core-review finding 9): a sub-minimum request (n_quadrature < 8, incl. 0)
    // FLOORS to the cheapest supported Gauss-Legendre order (8) instead of falling
    // through the ladder and silently keeping the ACCURATE default (24) — a caller
    // asking for cheaper must not get more expensive.
    s.n_quad_fp = 8;
  }
  // Premium (pricing) Gauss-Legendre order. K2 (class: pure-refactor + new
  // capability): n_quad_price DECOUPLES the pricing quadrature from the fixed-point
  // quadrature — QuantLib QdFpAmericanEngine's l != p axis (docs/al-preset-ladder.md;
  // ALO SSRN 2547027). o.n_quad_price == 0 (the default) ties price to fp — the
  // historical behavior, so every existing / serialized AlOpts resolves to the SAME
  // scheme; a non-zero value quantizes to an available GL order {8,16,24,32,48,64}.
  // The nullopt (ACCURATE) path returned above keeps its 48-node premium quad.
  if (o.n_quad_price >= 8) {
    const unsigned p = o.n_quad_price;
    s.n_quad_price = (p >= 64)   ? std::uint16_t{64}
                     : (p >= 48) ? std::uint16_t{48}
                     : (p >= 32) ? std::uint16_t{32}
                     : (p >= 24) ? std::uint16_t{24}
                     : (p >= 16) ? std::uint16_t{16}
                                 : std::uint16_t{8};
  } else {
    s.n_quad_price = s.n_quad_fp;
  }
  if (o.max_newton_iter > 0) {
    const std::uint16_t total = o.max_newton_iter;
    s.n_iter_jn = (total >= 2) ? std::uint16_t{2} : std::uint16_t{1};
    s.n_iter_fp =
        (total > s.n_iter_jn) ? static_cast<std::uint16_t>(total - s.n_iter_jn) : std::uint16_t{0};
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

} // namespace amer

namespace { // reopen the file's anonymous namespace

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

// F7: the former d_plus/d_minus helpers were only ever called in adjacent pairs
// that recomputed sigma*sqrt(tau) and log(z) twice; both call sites (the eqn_b tip
// and eqn_b_NDd) now compute the shared base once and take base +/- v/2 inline, so
// the standalone helpers are gone (no other consumer — they were file-local).

void al_init_nodes(AlBoundary &b, std::uint16_t n, double T, double K, double r,
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

[[nodiscard]] double al_boundary_at(const AlBoundary &b, double u) noexcept {
  if (b.T <= 0.0) {
    return b.xmax;
  }
  if (u <= 0.0) {
    return b.xmax;
  }
  const double u_eff = (u >= b.T) ? b.T : u;
  const double z = 2.0 * std::sqrt(u_eff / b.T) - 1.0;
  const double zc = atx::core::clamp(z, -1.0, 1.0);
  const double y_val = al_cheb_eval(b.z.data(), b.wbary.data(), b.y.data(), b.n, zc);
  return b_from_y(y_val, b.xmax);
}

// F5: the strike-INVARIANT part of al_boundary_at — everything except the final
// xmax multiply. al_boundary_at(b, u) == b.xmax * al_boundary_factor_at(b, u)
// bit-for-bit (b_from_y is xmax * exp(-sqrt(y)); the guard branches return xmax ==
// xmax * 1.0). The boundary's live state y[] is K-independent (homogeneity), so when
// one solved boundary reprices many strikes/spots — with xmax rescaling LINEARLY in
// K — the factor is shared and only the xmax multiply moves per strike.
[[nodiscard]] double al_boundary_factor_at(const AlBoundary &b, double u) noexcept {
  if (b.T <= 0.0 || u <= 0.0) {
    return 1.0;
  }
  const double u_eff = (u >= b.T) ? b.T : u;
  const double z = 2.0 * std::sqrt(u_eff / b.T) - 1.0;
  const double zc = atx::core::clamp(z, -1.0, 1.0);
  const double y_val = al_cheb_eval(b.z.data(), b.wbary.data(), b.y.data(), b.n, zc);
  const double yv = (y_val > 0.0) ? y_val : 0.0;
  return std::exp(-std::sqrt(yv));
}

// F5 (perf finding 5): per-boundary premium precompute. premium_integrand_put's
// per-node work — the barycentric boundary factor, sigma*sqrt(t), exp(-q*t),
// exp(-r*t) — depends on (boundary, sigma, r, q, t) but NOT on the spot/strike being
// priced. When ONE solved boundary prices MANY strikes/spots (the two slice engines
// and american_greeks_fd's spot stencils) these are bound once and reused, leaving
// just log + 2 norm_cdf per (node, strike). Stored as `bfac` (pre-xmax factor) so
// b_t = xmax * bfac reconstructs bit-identically under the linear-in-K rescale.
// euro_fwd / euro_df hoist the two European-leg (euro_put_sk) exps out of the loop.
//
// Deliberately a SEPARATE object, NOT a member of AlWorkspace: american_greeks_fd
// stack-bundles seven AlWorkspaces and this must not multiply into that budget.
// One AlPremiumCache is ~2.6 KB (4 * 64 doubles + scalars); the slice engines keep
// one on the stack, the greeks bundle keeps ONE shared instance keyed to whichever
// boundary is active. Arrays are left uninitialised (filled [0,nq) before any read),
// matching the AlWorkspace geo-array convention.
struct AlPremiumCache {
  const void *bnd_id = nullptr; // boundary this was bound from (validity key)
  bool valid = false;
  unsigned nq = 0;
  double sigma_bound = 0.0;
  double r_bound = 0.0;
  double q_bound = 0.0;
  double euro_fwd = 0.0; // exp((r-q)*T) — European-leg forward factor
  double euro_df = 0.0;  // exp(-r*T)    — European-leg discount factor
  double bfac[kAlMaxQuad]; // boundary factor: b_t = xmax * bfac[i]
  double vv[kAlMaxQuad];   // sigma * sqrt(t_i)
  double dq[kAlMaxQuad];   // exp(-q * t_i)
  double dr[kAlMaxQuad];   // exp(-r * t_i)
};

// Is pc bound for exactly this (boundary, quadrature, sigma, r, q)? A miss falls the
// consumer back to the bit-identical inline path, so this is the only reuse guard.
[[nodiscard]] bool al_premium_cache_matches(const AlPremiumCache &pc, const AlBoundary &b,
                                            const AlWorkspace &ws, double sigma, double r,
                                            double q) noexcept {
  return pc.valid && pc.bnd_id == &b && pc.nq == ws.n_quad_price && pc.sigma_bound == sigma &&
         pc.r_bound == r && pc.q_bound == q;
}

void al_seed_boundary(AlBoundary &b, double sigma, double r, double q) noexcept {
  ATX_VOL_COUNT(BoundarySolves); // one cold boundary seed (BAW re-seed per node)
  counters::ledger::bump(counters::ledger::Solve::AlBoundarySolves); // V1 always-on gate metric
  counters::lightweight::record_boundary_solves();
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

// Identical to al_seed_boundary but seeds each node from the QD+ critical price
// (Li 2010) instead of BAW. Selectable per-scheme via AlScheme::seed for the A6
// shootout A/B and al_boundary_jn_sweeps_to_converge's QdPlus mode; NOT the default
// for any production scheme (A6 measured it worse than BAW — see qdplus_critical_put).
void al_seed_boundary_qdplus(AlBoundary &b, double sigma, double r, double q) noexcept {
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
// Bind the (T,r,q)-dependent portion once for a retained fixed contract. The
// combined cold wrapper below explicitly invalidates before calling this seam;
// AloPricer::reset does the same and then retains it across every sigma residual.
void al_bind_geometry_static(const AlBoundary &bnd, AlWorkspace &ws, double r,
                             double q) noexcept {
  if (!ws.specialize || !al_fp_specialized(bnd.n, ws.n_quad_fp)) {
    ws.geo_static_bound = false;
    return; // generic path recomputes inline; no geometry needed
  }
  if (ws.geo_static_bound) {
    return;
  }
  const double *xs = ws.qx_fp;
  const double *wv = ws.qw_fp;
  const unsigned nq = ws.n_quad_fp;
  const unsigned nb = bnd.n;
  const double T = bnd.T;
  // A6 defensive bound. Every scheme al_fp_specialized admits fits every geometry
  // table by construction — enforced at compile time by
  // al_geometry_tables_fit_every_specialized_scheme(), which is DERIVED from the
  // predicate rather than restating its scheme list (REVWSA finding 4). This runtime
  // arm is therefore unreachable today; it stays as the same safety shape as the R-30
  // specialize-off fallback, so a bound that ever did slip through falls back to the
  // generic kernel instead of writing out of bounds. It calls the SAME predicate the
  // static_assert sweeps rather than re-stating its arithmetic, so the two cannot
  // drift — and so it now also covers the geo_zc / geo_weru / geo_wequ row addressing
  // below, which the hand-copied geo_bary-only expression here did not (REVA7FIX
  // Minor 3), and the geo_bary_den / geo_bary_hit writes below, which the
  // predicate itself did not until REVA7TIDY (see its third bullet above).
  if (!al_scheme_fits_geometry_tables(nb, nq)) {
    ws.specialize = false;
    ws.geo_static_bound = false;
    g_specialize_off_fallbacks.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (std::uint16_t j = 1; j < bnd.n; ++j) {
    const double tau = bnd.tau[j];
    if (tau <= 1.0e-14) {
      continue; // sweeps skip this node entirely
    }
    const double half_tau = 0.5 * tau;
    const unsigned gbase = static_cast<unsigned>(j) * kGeoQuadStride;
    const unsigned bpair = (static_cast<unsigned>(j) - 1u) * nq;
    for (unsigned i = 0; i < nq; ++i) {
      const double u = half_tau * (1.0 + xs[i]);
      const double t_u = tau - u;
      if (t_u <= 1.0e-14) {
        continue; // inactive node; the loop re-checks t_u and skips it
      }
      // al_boundary_at's z transform (u < T here, so u_eff == u — replicated so the
      // stored zc is bit-identical to the inline computation).
      const double u_eff = (u >= T) ? T : u;
      const double zz = 2.0 * std::sqrt(u_eff / T) - 1.0;
      const double zc = atx::core::clamp(zz, -1.0, 1.0);
      ws.geo_zc[gbase + i] = zc;
      ws.geo_weru[gbase + i] = wv[i] * std::exp(r * u);
      ws.geo_wequ[gbase + i] = wv[i] * std::exp(q * u);
      ATX_VOL_COUNT_N(ExpCalls, 2); // exp(r·u), exp(q·u) — now paid ONCE per solve
      counters::lightweight::record_exp_calls(2u);
      // A6: the barycentric denominator half at this zc — the SAME expression, the
      // SAME operands and the SAME accumulation order al_cheb_eval_t ran inline on
      // every sweep. That is what makes al_cheb_eval_hoisted bit-identical to it.
      const unsigned bbase = (bpair + i) * nb;
      double den = 0.0;
      int hit = -1;
      for (unsigned k = 0; k < nb; ++k) {
        const double dz = zc - bnd.z[k];
        if (dz == 0.0) {
          hit = static_cast<int>(k); // the inline path returns y[k] here
          break;
        }
        const double qq = bnd.wbary[k] / dz;
        ws.geo_bary[bbase + k] = qq;
        den += qq;
      }
      ws.geo_bary_den[bpair + i] = den;
      ws.geo_bary_hit[bpair + i] = static_cast<std::int8_t>(hit);
    }
  }
  ws.geo_static_bound = true;
#ifndef NDEBUG
  // R-30: record the contract this static geometry was bound for so any later reuse
  // through al_bind_geometry_sigma can prove it still matches (Debug-only guardrail).
  ws.geo_bind_key = AlWorkspace::GeoBindKey{bnd.T, r, q, bnd.n, ws.n_quad_fp, true};
#endif
}

// Bind only sigma*sqrt(t_u). A missing static bind is an internal invariant
// violation: assert in Debug and select the generic kernel in Release so no
// indeterminate geometry can ever be consumed. r/q are passed only so the Debug
// bind-key assert (R-30) can prove the retained static geometry still matches this
// contract; the sigma-bind math itself is r/q-independent.
void al_bind_geometry_sigma(const AlBoundary &bnd, AlWorkspace &ws, double sigma,
                            [[maybe_unused]] double r, [[maybe_unused]] double q) noexcept {
  if (!ws.specialize || !al_fp_specialized(bnd.n, ws.n_quad_fp)) {
    return;
  }
  assert(ws.geo_static_bound);
  if (!ws.geo_static_bound) {
    ws.specialize = false;
    // R-30: the safety fallback fired — a specialized reuse found no bound geometry.
    // Never expected on a production flow; tally it (Release observability).
    g_specialize_off_fallbacks.fetch_add(1, std::memory_order_relaxed);
    return;
  }
#ifndef NDEBUG
  // R-30: retained static geometry must belong to THIS contract. A mismatch means a
  // caller reused a workspace across a (T, r, q, node-grid) change without rebinding
  // (the obs-23864 revalidation-trust regression) — fail loud in Debug.
  assert(ws.geo_bind_key.set && "al_bind_geometry_sigma: static geometry reused but never bound");
  assert(ws.geo_bind_key.T == bnd.T && "al_bind_geometry_sigma: retained geometry T mismatch");
  assert(ws.geo_bind_key.r == r && "al_bind_geometry_sigma: retained geometry r mismatch");
  assert(ws.geo_bind_key.q == q && "al_bind_geometry_sigma: retained geometry q mismatch");
  assert(ws.geo_bind_key.n == bnd.n && "al_bind_geometry_sigma: retained geometry node-count mismatch");
  assert(ws.geo_bind_key.nq == ws.n_quad_fp &&
         "al_bind_geometry_sigma: retained geometry quad-order mismatch");
#endif
  const double *xs = ws.qx_fp;
  const unsigned nq = ws.n_quad_fp;
  for (std::uint16_t j = 1; j < bnd.n; ++j) {
    const double tau = bnd.tau[j];
    if (tau <= 1.0e-14) {
      continue;
    }
    const double half_tau = 0.5 * tau;
    const unsigned gbase = static_cast<unsigned>(j) * kGeoQuadStride;
    for (unsigned i = 0; i < nq; ++i) {
      const double u = half_tau * (1.0 + xs[i]);
      const double t_u = tau - u;
      if (t_u <= 1.0e-14) {
        continue;
      }
      ws.geo_v[gbase + i] = sigma * std::sqrt(t_u);
    }
  }
}

// Cold/Greeks/slice callers own one workspace per solve. Invalidate first so a
// caller that reuses its stack object across contracts cannot consume stale
// static geometry, then preserve the former all-at-once bind semantics.
void al_bind_geometry(const AlBoundary &bnd, AlWorkspace &ws, double sigma, double r,
                      double q) noexcept {
  ws.geo_static_bound = false;
  al_bind_geometry_static(bnd, ws, r, q);
  al_bind_geometry_sigma(bnd, ws, sigma, r, q);
}

// Equation B kernel: N(τ,b), D(τ,b). Templated on the fixed-scheme trip counts
// <NB=n_boundary, NQ=n_quad_fp> (P2.2 §3); NB==0 && NQ==0 is the generic runtime
// path AND the un-hoisted scalar reference (recomputes the geometry inline exactly
// as before this task). NB>0 reads the al_bind_geometry precompute — a pure hoist,
// so the two paths are bit-identical.
template <unsigned NB, unsigned NQ>
void eqn_b_ND_impl(const AlBoundary &bnd, const AlWorkspace &ws, unsigned node_idx, double tau,
                   double b_val, double sigma, double r, double q, double &N_out,
                   double &D_out) noexcept {
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
  // F7 (perf finding 7): d_plus and d_minus at the tip both recompute
  // sigma*sqrt(tau) and log(b_val/K). Share them once as base +/- v/2 — bit-identical
  // to the two d_plus/d_minus calls (same op order), saving one log + one sqrt/node.
  const double tip_v = sigma * std::sqrt(tau);
  const double tip_base = (std::log(b_val / K) + (r - q) * tau) / tip_v;
  const double tip_p = norm_cdf(tip_base + 0.5 * tip_v);
  const double tip_m = norm_cdf(tip_base - 0.5 * tip_v);
  ATX_VOL_COUNT_N(NormCdfCalls, 2); // tip_p, tip_m

  const double *xs = ws.qx_fp;
  const unsigned nq = (NQ != 0) ? NQ : ws.n_quad_fp;
  double n_int = 0.0;
  double d_int = 0.0;
  const double half_tau = 0.5 * tau;
  if constexpr (NB != 0) {
    // Specialized: read the sweep-invariant geometry; evaluate only the
    // sweep-VARYING Chebyshev value + b_from_y in the loop.
    const unsigned gbase = node_idx * kGeoQuadStride;
    // A6: node_idx >= 1 on every specialized entry (both sweeps loop from 1), which
    // is what makes the packed (j-1) barycentric row index well-formed.
    const unsigned bpair = (node_idx - 1u) * nq;
    const double rq = r - q;
    for (unsigned i = 0; i < nq; ++i) {
      const double u = half_tau * (1.0 + xs[i]);
      const double t_u = tau - u;
      if (t_u <= 1.0e-14) {
        continue;
      }
      // A6 (PR-P2): the barycentric denominator is sweep-invariant and now comes from
      // the per-solve table instead of nb divisions per (node, quad node) per sweep.
      const double y_val = al_cheb_eval_hoisted<NB>(&ws.geo_bary[(bpair + i) * NB],
                                                    ws.geo_bary_den[bpair + i],
                                                    ws.geo_bary_hit[bpair + i], bnd.y.data());
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
    const double *wv = ws.qw_fp;
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
      counters::lightweight::record_exp_calls(2u);
      ATX_VOL_COUNT_N(NormCdfCalls, 2);
    }
  }
  n_int *= half_tau;
  d_int *= half_tau;

  N_out = tip_m + r * n_int;
  D_out = tip_p + q * d_int;
}

// ∂N/∂b, ∂D/∂b at fixed kernel.
void eqn_b_NDd(const AlBoundary &bnd, double tau, double b_val, double sigma, double r, double q,
               double &Nd_out, double &Dd_out) noexcept {
  if (tau <= 1.0e-14 || !(b_val > 0.0)) {
    Nd_out = 0.0;
    Dd_out = 0.0;
    return;
  }
  const double K = bnd.K;
  const double v = sigma * std::sqrt(tau);
  // F7: reuse the local v and share log(b_val/K) between d_plus/d_minus as
  // base +/- v/2 — bit-identical to the two d_plus/d_minus calls (each of which
  // recomputed v and log), saving two sqrt + one log per node.
  const double base = (std::log(b_val / K) + (r - q) * tau) / v;
  const double dpv = base + 0.5 * v;
  const double dmv = base - 0.5 * v;
  Nd_out = norm_pdf(dmv) / (b_val * v);
  Dd_out = norm_pdf(dpv) / (b_val * v);
}

// One sweep's outcome. `max_dy` is the collocation residual max|Δy| — but ONLY over
// the nodes that actually moved: a node whose fixed-point denominator D has
// collapsed is frozen at its current value and skips the |Δy| update, so it cannot
// raise the residual. `all_frozen` reports the pathological case where EVERY
// movable node (tau > 0) froze: the sweep moved nothing, so max_dy == 0 means "no
// progress is possible", NOT "converged". Without this flag a wholly frozen sweep
// looks like an immediate tol hit and the solve returns its raw seed as a solved
// boundary. The two are genuinely different states and the caller must be able to
// tell them apart, which is why this is a flag and not a sentinel residual: a
// residual big enough to defeat the tol test would only spend the rest of the
// sweep budget re-deriving the same frozen state, and would still leave the solve
// returning Ok with an unsolved boundary.
struct AlSweepResult {
  double max_dy = 0.0;
  bool all_frozen = false;
};

template <unsigned NB, unsigned NQ>
[[nodiscard]] AlSweepResult al_jn_sweep_impl(AlBoundary &b, AlWorkspace &ws, double sigma, double r,
                                             double q) noexcept {
  const unsigned n = (NB != 0) ? NB : b.n;
  double max_dy = 0.0;
  unsigned n_movable = 0;
  unsigned n_frozen = 0;
  ws.next_y[0] = 0.0;
  for (unsigned i = 1; i < n; ++i) {
    const double tau = b.tau[i];
    if (tau <= 1.0e-14) {
      ws.next_y[i] = 0.0;
      continue;
    }
    ++n_movable;
    const double b_val = b_from_y(b.y[i], b.xmax);
    double Nv = 0.0;
    double Dv = 0.0;
    eqn_b_ND_impl<NB, NQ>(b, ws, i, tau, b_val, sigma, r, q, Nv, Dv);
    if (!(Dv > 1.0e-300)) {
      ws.next_y[i] = b.y[i];
      ++n_frozen;
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
  return AlSweepResult{max_dy, n_movable > 0 && n_frozen == n_movable};
}

// Dispatch to a compile-time-trip-count instantiation for the production fixed
// schemes; the generic <0,0> is both the arbitrary-AlOpts path and the reference.
[[nodiscard]] AlSweepResult al_jacobi_newton_sweep(AlBoundary &b, AlWorkspace &ws, double sigma,
                                                   double r, double q) noexcept {
  ATX_VOL_COUNT(JacobiNewtonSweeps);
  if (ws.specialize) {
    if (b.n == 7 && ws.n_quad_fp == 8) {
      return al_jn_sweep_impl<7, 8>(b, ws, sigma, r, q);
    }
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
[[nodiscard]] AlSweepResult al_fp_sweep_impl(AlBoundary &b, AlWorkspace &ws, double sigma, double r,
                                             double q) noexcept {
  const unsigned n = (NB != 0) ? NB : b.n;
  double max_dy = 0.0;
  unsigned n_movable = 0;
  unsigned n_frozen = 0;
  ws.next_y[0] = 0.0;
  for (unsigned i = 1; i < n; ++i) {
    const double tau = b.tau[i];
    if (tau <= 1.0e-14) {
      ws.next_y[i] = 0.0;
      continue;
    }
    ++n_movable;
    const double b_val = b_from_y(b.y[i], b.xmax);
    double Nv = 0.0;
    double Dv = 0.0;
    eqn_b_ND_impl<NB, NQ>(b, ws, i, tau, b_val, sigma, r, q, Nv, Dv);
    if (!(Dv > 1.0e-300)) {
      ws.next_y[i] = b.y[i];
      ++n_frozen;
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
  return AlSweepResult{max_dy, n_movable > 0 && n_frozen == n_movable};
}

[[nodiscard]] AlSweepResult al_fixed_point_sweep(AlBoundary &b, AlWorkspace &ws, double sigma,
                                                 double r, double q) noexcept {
  ATX_VOL_COUNT(FixedPointSweeps);
  if (ws.specialize) {
    if (b.n == 7 && ws.n_quad_fp == 8) {
      return al_fp_sweep_impl<7, 8>(b, ws, sigma, r, q);
    }
    if (b.n == 7 && ws.n_quad_fp == 16) {
      return al_fp_sweep_impl<7, 16>(b, ws, sigma, r, q);
    }
    if (b.n == 12 && ws.n_quad_fp == 24) {
      return al_fp_sweep_impl<12, 24>(b, ws, sigma, r, q);
    }
  }
  return al_fp_sweep_impl<0, 0>(b, ws, sigma, r, q);
}

// pc == nullptr: the original inline path (recompute the boundary factor + the two
// exps + v per node). pc != nullptr (bound for this boundary at (sigma,r,q)): read
// the strike-invariant terms from the precompute and rescale b_t by the LIVE xmax —
// b.xmax * pc->bfac[i] == al_boundary_at(b, rem) bit-for-bit — so the shared dp/return
// below produce identical bits. The exp counters live only on the inline branch; the
// cached exps are counted once at al_bind_premium.
[[nodiscard]] double premium_integrand_put(double z, const AlBoundary &b, double S, double sigma,
                                           double r, double q, const AlPremiumCache *pc,
                                           unsigned i) noexcept {
  const double t = z * z;
  if (t <= 1.0e-14) {
    return 0.0;
  }
  double b_t;
  double v;
  double dq;
  double dr;
  if (pc != nullptr) {
    b_t = b.xmax * pc->bfac[i];
    v = pc->vv[i];
    dq = pc->dq[i];
    dr = pc->dr[i];
  } else {
    const double rem = b.T - t;
    b_t = (rem > 0.0) ? al_boundary_at(b, rem) : b.K;
    if (!(b_t > 0.0)) {
      return 0.0;
    }
    v = sigma * std::sqrt(t);
    dq = std::exp(-q * t);
    dr = std::exp(-r * t);
    ATX_VOL_COUNT_N(ExpCalls, 2);
    counters::lightweight::record_exp_calls(2u);
  }
  const double dp = std::log(S * dq / (b_t * dr)) / v + 0.5 * v;
  ATX_VOL_COUNT(PremiumQuadEvals);
  counters::ledger::bump(counters::ledger::Solve::AlPremiumEvals); // V1 always-on
  ATX_VOL_COUNT(LogCalls);
  ATX_VOL_COUNT_N(NormCdfCalls, 2);
  return 2.0 * z * (r * b.K * dr * norm_cdf(-dp + v) - q * S * dq * norm_cdf(-dp));
}

// Premium quadrature, templated on the fixed premium trip count NP (P2.2 §3);
// NP==0 is the generic runtime path. Single body, so bit-identical across NP. A
// non-null pc supplies the F5 per-boundary precompute.
template <unsigned NP>
[[nodiscard]] double al_put_premium_impl(const AlBoundary &b, const AlWorkspace &ws, double S,
                                         double sigma, double r, double q,
                                         const AlPremiumCache *pc) noexcept {
  const double sqrtT = std::sqrt(b.T);
  const double half_sqrtT = 0.5 * sqrtT;
  double total = 0.0;
  const double *xs = ws.qx_price;
  const double *wv = ws.qw_price;
  const unsigned nq = (NP != 0) ? NP : ws.n_quad_price;
  for (unsigned i = 0; i < nq; ++i) {
    const double zi = half_sqrtT * (1.0 + xs[i]);
    total += wv[i] * premium_integrand_put(zi, b, S, sigma, r, q, pc, i);
  }
  total *= half_sqrtT;
  return (total > 0.0) ? total : 0.0;
}

[[nodiscard]] double al_put_premium(const AlBoundary &b, const AlWorkspace &ws, double S,
                                    double sigma, double r, double q,
                                    const AlPremiumCache *pc = nullptr) noexcept {
  // Reuse the precompute only when it was bound for exactly this (boundary, nq,
  // sigma, r, q); any miss uses the bit-identical inline path.
  const AlPremiumCache *use =
      (pc != nullptr && al_premium_cache_matches(*pc, b, ws, sigma, r, q)) ? pc : nullptr;
  if (ws.specialize) {
    switch (ws.n_quad_price) {
    case 8:
      return al_put_premium_impl<8>(b, ws, S, sigma, r, q, use);
    case 16:
      return al_put_premium_impl<16>(b, ws, S, sigma, r, q, use);
    case 24:
      return al_put_premium_impl<24>(b, ws, S, sigma, r, q, use);
    case 48:
      return al_put_premium_impl<48>(b, ws, S, sigma, r, q, use);
    default:
      break;
    }
  }
  return al_put_premium_impl<0>(b, ws, S, sigma, r, q, use);
}

// F5: bind the per-boundary premium precompute for a solved boundary at (sigma,r,q).
// The caller rebinds when any of (boundary, sigma, r, q) changes across a reuse loop.
// If a quadrature node hits premium_integrand_put's degenerate guards (t<=1e-14 or
// b_t<=0 — unreachable for interior Gauss-Legendre price nodes, but guarded) the
// cache is left invalid so consumers fall back to the inline path. Counts the 2
// exps/node here — the exact spot the per-strike loop no longer pays them.
void al_bind_premium(const AlBoundary &b, const AlWorkspace &ws, double sigma, double r, double q,
                     AlPremiumCache &pc) noexcept {
  pc.valid = false;
  pc.bnd_id = nullptr;
  const unsigned nq = ws.n_quad_price;
  if (nq == 0 || nq > kAlMaxQuad || !(b.xmax > 0.0) || ws.qx_price == nullptr) {
    return;
  }
  const double half_sqrtT = 0.5 * std::sqrt(b.T);
  const double *xs = ws.qx_price;
  for (unsigned i = 0; i < nq; ++i) {
    const double zi = half_sqrtT * (1.0 + xs[i]);
    const double t = zi * zi;
    const double rem = b.T - t;
    if (t <= 1.0e-14 || !(rem > 0.0)) {
      return; // degenerate guard: inline path handles it (never hit for GL nodes)
    }
    const double bfac = al_boundary_factor_at(b, rem);
    if (!(b.xmax * bfac > 0.0)) {
      return;
    }
    pc.bfac[i] = bfac;
    pc.vv[i] = sigma * std::sqrt(t);
    pc.dq[i] = std::exp(-q * t);
    pc.dr[i] = std::exp(-r * t);
    ATX_VOL_COUNT_N(ExpCalls, 2);
    counters::lightweight::record_exp_calls(2u);
  }
  // European-leg forward/discount exps, hoisted out of the per-strike price (F5
  // rider). euro_put_sk never counted these, so they stay off ExpCalls.
  pc.euro_fwd = std::exp((r - q) * b.T);
  pc.euro_df = std::exp(-r * b.T);
  pc.nq = nq;
  pc.sigma_bound = sigma;
  pc.r_bound = r;
  pc.q_bound = q;
  pc.bnd_id = &b;
  pc.valid = true;
}

// F5: al_put_price_from_boundary with the premium precompute + hoisted euro leg.
// ALWAYS correct: when pc is bound for this (boundary, sigma, r, q) it uses the
// precomputed euro exps + cached premium; otherwise it recomputes both inline. Both
// paths are bit-identical to al_put_price_from_boundary(bnd, ws, S, K, T, sigma, r,
// q) — the euro factors equal euro_put_sk's own exps and the premium reuse is proven
// bit-exact — so the reuse is a pure throughput win, never a correctness dependency.
[[nodiscard]] double al_put_price_from_boundary_cached(const AlBoundary &bnd, const AlWorkspace &ws,
                                                       const AlPremiumCache &pc, double S, double K,
                                                       double T, double sigma, double r,
                                                       double q) noexcept {
  const bool hit = al_premium_cache_matches(pc, bnd, ws, sigma, r, q);
  const double euro = hit ? black76_price(S * pc.euro_fwd, K, T, sigma, pc.euro_df, Side::Put)
                          : euro_put_sk(S, K, T, sigma, r, q);
  const double prem = al_put_premium(bnd, ws, S, sigma, r, q, hit ? &pc : nullptr);
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

// ── Boundary solve / price split (S-independence seam) ───────────────────
//
// The Andersen-Lake exercise boundary depends on (K, T, sigma, r, q) but NOT on
// the spot S — al_init_nodes / al_seed_boundary / the sweeps all ignore S; only
// al_put_premium reads it. Splitting al_solve_put here lets a greeks bundle solve
// one boundary and re-price every SPOT stencil against it (american_greeks_fd),
// collapsing the 17 solves to the 7 unique (sigma,r,T) boundaries — bit-identical.
//
// AlSolveStatus and the three entry points below (al_solve_put_boundary[_warm],
// al_put_price_from_boundary) are part of the boundary seam (amer) reused by
// boundary_interp.cpp. The enum's declaration is in american_boundary.hpp.
} // namespace
// linkage in atx::vol::amer; reopened right after al_put_price_from_boundary.
namespace amer {

// S-independent: init nodes, bind quadrature, seed + iterate the boundary. On Ok,
// `bnd`/`ws` hold a converged boundary ready for al_put_price_from_boundary.
[[nodiscard]] AlSolveStatus al_solve_put_boundary(double K, double T, double sigma, double r,
                                                  double q, const AlScheme &sch, AlBoundary &bnd,
                                                  AlWorkspace &ws,
                                                  bool specialize) noexcept { // default in header
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    // Negative-rate/carry corner: AL cannot run. Flagged unsupported.
    return AlSolveStatus::Collapsed;
  }
  const detail::GaussLegendre *fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre *pr = gl_find(sch.n_quad_price);
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

  // A6: the scheme selects the cold critical-boundary seed. QdPlus (Li 2010)
  // steepens the QD/BAW exponent near expiry where the frozen-θ approximation is
  // worst; it reaches the SAME converged fixed point as BAW but from a closer
  // start, so a fixed fast-tier sweep budget lands nearer the boundary (smaller
  // residual → smaller price error). Baw stays the default for every other scheme.
  if (sch.seed == detail::AlSeedMode::QdPlus) {
    al_seed_boundary_qdplus(bnd, sigma, r, q);
  } else {
    al_seed_boundary(bnd, sigma, r, q);
  }

  // A frozen sweep (below) is DETERMINISTIC: nothing moved, so the next sweep sees
  // the identical (bnd.y, geometry) and freezes identically. Bailing out on the
  // first one is therefore not an early heuristic — it is the whole remaining
  // budget's answer, reported as a status the callers already branch on rather
  // than as a seed-valued price nothing downstream can recognise as unsolved.
  double resid = 1.0;
  for (std::uint16_t k = 0; k < sch.n_iter_jn; ++k) {
    const AlSweepResult s = al_jacobi_newton_sweep(bnd, ws, sigma, r, q);
    if (s.all_frozen) {
      return AlSolveStatus::NotConverged;
    }
    resid = s.max_dy;
    if (resid <= sch.tol) {
      ATX_VOL_COUNT(EarlyResidualExits);
      break;
    }
  }
  if (resid > sch.tol) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      const AlSweepResult s = al_fixed_point_sweep(bnd, ws, sigma, r, q);
      if (s.all_frozen) {
        return AlSolveStatus::NotConverged;
      }
      resid = s.max_dy;
      if (resid <= sch.tol) {
        ATX_VOL_COUNT(EarlyResidualExits);
        break;
      }
    }
  }
  return AlSolveStatus::Ok;
}

// Init-ONLY path for the AVX2 boundary batch (Task A5; supersedes A1's
// al_seed_put_boundary). Runs the pre-sweep prefix of al_solve_put_boundary — node
// init (z/wbary/x/tau/xmax) + Gauss-Legendre binding — but does NEITHER the cold BAW
// seed NOR al_bind_geometry. The AVX2 kernel recomputes geometry inline per lane and
// now lays down the BAW seed 4-wide itself (american_boundary_avx2.cpp), so the
// scalar per-lane Barone-Adesi-Whaley Newton — the dominant serialization that capped
// the pack speedup at ~1.6× — is gone from the seed path. bnd.y[] is left at 0 (the
// caller's vector seed fills it); ws.specialize is false (the kernel owns geometry).
[[nodiscard]] AlSolveStatus al_init_put_boundary(double K, double T, double r, double q,
                                                 const AlScheme &sch, AlBoundary &bnd,
                                                 AlWorkspace &ws) noexcept {
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    return AlSolveStatus::Collapsed;
  }
  const detail::GaussLegendre *fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre *pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return AlSolveStatus::TableMissing;
  }
  ws.specialize = false; // AVX2 kernel owns geometry; no ws.geo_* is bound or read
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;
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
[[nodiscard]] AlSolveStatus al_solve_put_boundary_warm(double K, double T, double sigma, double r,
                                                       double q, const AlScheme &sch,
                                                       const AlBoundary &seed, AlBoundary &bnd,
                                                       AlWorkspace &ws) noexcept {
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    return AlSolveStatus::Collapsed;
  }
  const detail::GaussLegendre *fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre *pr = gl_find(sch.n_quad_price);
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
    const AlSweepResult s = al_jacobi_newton_sweep(bnd, ws, sigma, r, q);
    if (s.all_frozen) {
      return AlSolveStatus::NotConverged; // see al_solve_put_boundary
    }
    resid = s.max_dy;
    if (resid <= sch.tol) {
      break;
    }
  }
  if (resid > sch.tol) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      const AlSweepResult s = al_fixed_point_sweep(bnd, ws, sigma, r, q);
      if (s.all_frozen) {
        return AlSolveStatus::NotConverged;
      }
      resid = s.max_dy;
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
[[nodiscard]] double al_put_price_from_boundary(const AlBoundary &bnd, const AlWorkspace &ws,
                                                double S, double K, double T, double sigma,
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

// P2 (WS-P) seam — the PURE collocation residual R(y; sigma, r, q), exposed as a
// linkable symbol so the adjoint-greeks kernel (detail/adjoint_greeks.cpp) can
// call it to form J = dR/dy and R_sigma/R_r. Pure function of (y, sigma, r, q):
// copies y into a scratch boundary, runs the generic inline-geometry kernel
// eqn_b_ND_impl<0,0>, and returns the fixed-point residual per node. Does not
// mutate bnd/ws state that any other caller observes (scr is a local copy).
void al_put_boundary_residual(const AlBoundary &bnd, const AlWorkspace &ws, const double *y,
                              double sigma, double r, double q, double *R_out) noexcept {
  const std::uint16_t n = bnd.n;
  const double xmax = bnd.xmax;
  const double K = bnd.K;
  AlBoundary scr = bnd; // scratch: only .y varies; node grid / xmax / K fixed
  for (std::uint16_t i = 0; i < n; ++i) {
    scr.y[i] = y[i];
  }
  R_out[0] = 0.0;
  for (std::uint16_t i = 1; i < n; ++i) {
    const double tau = scr.tau[i];
    if (tau <= 1.0e-14) {
      R_out[i] = 0.0;
      continue;
    }
    const double b_val = b_from_y(y[i], xmax);
    double N = 0.0, D = 0.0;
    eqn_b_ND_impl<0, 0>(scr, ws, i, tau, b_val, sigma, r, q, N, D);
    double R = 0.0;
    if (D > 1.0e-300) {
      const double alpha = K * std::exp(-(r - q) * tau);
      double b_new = alpha * N / D;
      if (b_new > xmax) {
        b_new = xmax;
      }
      if (!(b_new > 0.0)) {
        b_new = 1.0e-6 * K;
      }
      R = y[i] - y_from_b(b_new, xmax);
    }
    R_out[i] = R;
  }
}

// P3-pre (WS-P) seam — Christianson reverse-accumulation-through-iterations. The
// three primitives the adjoint-greeks kernel taps to differentiate through the
// BUDGET-LIMITED solve so the AAD greek matches the actual mark derivative on the
// whole domain (not just the well-converged fixed-point subset). See the header for
// the Christianson (1994) citation and the mark-consistency rationale.

AlSolveStatus al_solve_put_boundary_tape(double K, double T, double sigma, double r, double q,
                                         const AlScheme &sch, AlBoundary &bnd, AlWorkspace &ws,
                                         AlSolveTape &tape) noexcept {
  tape.n_steps = 0;
  if (static_cast<int>(sch.n_iter_jn) + static_cast<int>(sch.n_iter_fp) > kAlMaxTapeSweeps) {
    return AlSolveStatus::TableMissing; // budget exceeds tape capacity — caller falls back to fd
  }
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    return AlSolveStatus::Collapsed;
  }
  const detail::GaussLegendre *fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre *pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return AlSolveStatus::TableMissing;
  }
  // GENERIC (inline-geometry) kernel — specialize=false — so every taped sweep uses
  // the SAME map al_apply_boundary_sweep replays for the Christianson tangent (the
  // tangent MUST differentiate exactly the map that produced each taped iterate). This
  // boundary agrees with production al_solve_put_boundary (specialized) only to the
  // pure-hoist tolerance (~1e-9 in the delta/price tests), NOT bit-identically, so it is
  // NOT reused as a served mark — the adjoint kernel serves its own AL price.
  ws.specialize = false;
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;
  if (sch.seed == detail::AlSeedMode::QdPlus) {
    al_seed_boundary_qdplus(bnd, sigma, r, q);
  } else {
    al_seed_boundary(bnd, sigma, r, q);
  }
  for (std::uint16_t i = 0; i < bnd.n; ++i) {
    tape.y_iter[0][i] = bnd.y[i]; // y^0 = seed
  }
  std::uint16_t step = 0;
  double resid = 1.0;
  for (std::uint16_t k = 0; k < sch.n_iter_jn; ++k) {
    const AlSweepResult s = al_jacobi_newton_sweep(bnd, ws, sigma, r, q);
    if (s.all_frozen) {
      // Unsolvable boundary (see al_solve_put_boundary). tape.n_steps stays 0, so
      // no caller can replay a tangent through a tape this returned non-Ok on.
      return AlSolveStatus::NotConverged;
    }
    resid = s.max_dy;
    tape.is_jn[step] = true;
    for (std::uint16_t i = 0; i < bnd.n; ++i) {
      tape.y_iter[step + 1][i] = bnd.y[i];
    }
    ++step;
    if (resid <= sch.tol) {
      ATX_VOL_COUNT(EarlyResidualExits);
      break;
    }
  }
  if (resid > sch.tol) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      const AlSweepResult s = al_fixed_point_sweep(bnd, ws, sigma, r, q);
      if (s.all_frozen) {
        return AlSolveStatus::NotConverged;
      }
      resid = s.max_dy;
      tape.is_jn[step] = false;
      for (std::uint16_t i = 0; i < bnd.n; ++i) {
        tape.y_iter[step + 1][i] = bnd.y[i];
      }
      ++step;
      if (resid <= sch.tol) {
        ATX_VOL_COUNT(EarlyResidualExits);
        break;
      }
    }
  }
  tape.n_steps = step;
  return AlSolveStatus::Ok;
}

void al_apply_boundary_sweep(const AlBoundary &bnd, const AlWorkspace &ws, const double *y_in,
                             double sigma, double r, double q, bool is_jn,
                             double *y_out) noexcept {
  AlBoundary scr = bnd; // node grid / xmax / K / T / z / wbary / tau fixed; only .y varies
  for (std::uint16_t i = 0; i < bnd.n; ++i) {
    scr.y[i] = y_in[i];
  }
  AlWorkspace scr_ws;          // fresh scratch — generic kernel reads only the quad pointers
  scr_ws.specialize = false;   // force eqn_b_ND_impl<0,0> (inline geometry; no geo_* rebind)
  scr_ws.qx_fp = ws.qx_fp;
  scr_ws.qw_fp = ws.qw_fp;
  scr_ws.n_quad_fp = ws.n_quad_fp;
  if (is_jn) {
    (void)al_jacobi_newton_sweep(scr, scr_ws, sigma, r, q);
  } else {
    (void)al_fixed_point_sweep(scr, scr_ws, sigma, r, q);
  }
  for (std::uint16_t i = 0; i < bnd.n; ++i) {
    y_out[i] = scr.y[i];
  }
}

void al_seed_boundary_into(const AlBoundary &bnd, double sigma, double r, double q,
                           double *y_out) noexcept {
  AlBoundary scr = bnd;
  al_seed_boundary(scr, sigma, r, q);
  for (std::uint16_t i = 0; i < bnd.n; ++i) {
    y_out[i] = scr.y[i];
  }
}

} // namespace amer

namespace { // reopen the file's anonymous namespace

// ExerciseRegime / classify_regime are defined once in american.hpp detail (the
// single source of truth for the early-exercise regime table) and used here via
// the `using` declarations above. A call delegates to al_solve_put with
// (rate=q, yield=r), so passing (r,q) for a put and (q,r) for a call covers both
// sides through one classifier.

// Shared message for the boundary the collocation sweep cannot move at all: every
// node's fixed-point denominator D underflows, so the scheme has no equation to
// solve and the boundary would otherwise be served at its analytic seed. Reachable
// in the heavy-carry, near-zero-sigma corner (xmax = K*min(1,r/q) far below K with
// sigma just above the degenerate floor), where every d_plus underflows norm_cdf.
constexpr const char *kUnsolvableBoundaryMsg =
    "andersen_lake: exercise-boundary sweep froze at every collocation node "
    "(collapsed fixed-point denominator) — the boundary is unsolvable at this "
    "(sigma, r, q, T); no price is served off the unconverged seed";

// Shared message for the double-continuation corner the ALO scheme cannot price.
constexpr const char *kDoubleContinuationMsg =
    "double-continuation regime (put q < r <= 0 / call r < q <= 0): the "
    "single-boundary Andersen-Lake scheme cannot represent two exercise "
    "boundaries; see Andersen-Lake 2021 (double-boundary case)";

// Put core — used directly for puts and via McDonald-Schroder for calls. The
// `specialize` flag (default true) forces the generic runtime-trip-count kernel when
// false — the seam behind detail::andersen_lake_generic_kernel that proves the
// specialized fixed-scheme kernel is bit-identical to the generic path.
[[nodiscard]] Result<double> al_solve_put(double S, double K, double T, double sigma, double r,
                                          double q, const AlScheme &sch, bool specialize = true) {
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
  case AlSolveStatus::NotConverged:
    return Err(ErrorCode::NotImplemented, kUnsolvableBoundaryMsg);
  case AlSolveStatus::Ok:
    break;
  }
  return Ok(al_put_price_from_boundary(bnd, ws, S, K, T, sigma, r, q));
}

// A4 (PR-C4): the sigma->0 limit of an American option is the European sigma->0
// limit df*(forward intrinsic) floored at the spot (immediate-exercise) intrinsic —
// at sigma=0 there is no time value, so the value is max(hold-to-expiry, exercise
// now). Correct in BOTH regimes: in the European regime early exercise is never
// optimal and df*(forward intrinsic) dominates; in the American regime the spot
// intrinsic wins where exercise is optimal. Callers gate the double-continuation
// Unsupported corner separately (it has no single-boundary price).
[[nodiscard]] inline double sigma_zero_american_limit(double S, double K, double T, double r,
                                                      double q, Side side) noexcept {
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  const double fwd_intr = (side == Side::Call) ? (F - K) : (K - F);
  const double euro_lim = df * (fwd_intr > 0.0 ? fwd_intr : 0.0);
  const double spot_intr = (side == Side::Call) ? (S - K) : (K - S);
  return std::max(euro_lim, spot_intr > 0.0 ? spot_intr : 0.0);
}

// Shared core of the public andersen_lake entry point, parameterized on `specialize`
// so detail::andersen_lake_generic_kernel can force the generic runtime-trip-count
// kernel for the SAME scheme and prove the specialized path is bit-identical.
[[nodiscard]] Result<double> andersen_lake_core(double S, double K, double T, double sigma,
                                                double r, double q, Side side,
                                                const std::optional<AlOpts> &opts, bool specialize,
                                                std::optional<detail::AlSeedMode> seed_override =
                                                    std::nullopt,
                                                std::uint16_t n_quad_price_override = 0) {
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

  // Degenerate T ~ 0: no time left, collapse to the spot intrinsic.
  if (T <= 1.0e-12) {
    const double intr = (side == Side::Call) ? (S - K) : (K - S);
    return Ok(intr > 0.0 ? intr : 0.0);
  }
  // Degenerate sigma ~ 0 (A4/PR-C4): the European sigma->0 limit df*(forward
  // intrinsic) floored at the spot intrinsic — NOT the spot intrinsic alone, which
  // was wrong (and discontinuous) for a carry-dominant European-regime option
  // (e.g. a put with r=0, q>0 -> df*(K-F)+ > 0). At sigma=0 there is no optionality,
  // so this deterministic max(hold, exercise) is valid in EVERY regime — the
  // double-continuation corner is a sigma>0 single-boundary limitation, not a
  // sigma=0 one, so this is priced (as the pre-A4 degenerate guard did), not errored.
  if (sigma <= 1.0e-8) {
    return Ok(sigma_zero_american_limit(S, K, T, r, q, side));
  }

  const double rate = (side == Side::Put) ? r : q;
  const double yield = (side == Side::Put) ? q : r;
  switch (classify_regime(rate, yield)) {
  case ExerciseRegime::European:
    return Ok(black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T), side));
  case ExerciseRegime::Unsupported:
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  case ExerciseRegime::American:
    break;
  }

  AlScheme sch = scheme_from_opts(opts);
  // A6 measurement seam: force a specific seed / premium-quad order on top of the
  // preset so the shootout can A/B (BAW vs QD+, 16 vs 8 premium nodes) in one
  // build. Both overrides are inert (nullopt / 0) on every production call.
  if (seed_override) {
    sch.seed = *seed_override;
  }
  if (n_quad_price_override != 0) {
    sch.n_quad_price = n_quad_price_override;
  }
  if (side == Side::Put) {
    return al_solve_put(S, K, T, sigma, r, q, sch, specialize);
  }
  // McDonald-Schroder symmetry: C(S,K,r,q) = P(K,S,q,r). Swap (S↔K), (r↔q).
  return al_solve_put(K, S, T, sigma, q, r, sch, specialize);
}

// ── American Greeks (chain rule + analytic correction derivatives) ───────

// A2 (core-review finding 2): floor a SERVED cached price at max(intrinsic, euro,
// 0), matching the cold clamp chain (al_put_price_from_boundary :1376-1393,
// AloPricer::price :1813-). The cached euro + F*corr carries no floor of its own,
// so it can dip below intrinsic — from Chebyshev interpolation error in-box, or
// (worse) from the correction clamping to its k_log-box EDGE value OUT-of-box,
// where the shortfall grows ~linearly with moneyness — producing arbitrageable
// sub-intrinsic marks. Applied to the served mark only; the analytic greek
// sensitivities are deliberately left untouched. The floor introduces a kink, so
// delta/gamma are technically discontinuous where it binds, but the cached greeks
// are already fixed-carry approximations and consumers read out.price for the
// mark, so keeping the smooth sensitivities is the intended contract.
[[nodiscard]] inline double floor_cached_price(double price, double euro,
                                               double intrinsic) noexcept {
  if (intrinsic > price) {
    price = intrinsic;
  }
  if (euro > price) {
    price = euro;
  }
  if (price < 0.0) {
    price = 0.0;
  }
  return price;
}

// GR-P2-3 baked-carry staleness tripwire. Counts a cached-jet serve whose query
// risk-free rate has drifted from the fixed-carry cache's baked rate by more than
// the C2 stale-gate (25 bps) into the always-on solve ledger. RATE-ONLY: the
// per-tenor q_eff drift from the mid-expiry representative carry is a legitimate
// in-fit artifact (see the american_price_cached A9 note below — an assert on
// baked_q at 25 bps aborted the suite), so it is deliberately not counted. In-fit
// de-Am and a flat-rate session serve query at the baked rate, so this stays 0
// through a normal fit/serve; it fires only on a genuine query-vs-baked rate move.
inline void count_cache_carry_drift(double baked_r, double query_r) noexcept {
  constexpr double kCacheCarryDriftTol = 0.0025; // 25 bps, matches session cache_side_covers
  if (std::isfinite(baked_r) && std::isfinite(query_r) &&
      std::fabs(query_r - baked_r) > kCacheCarryDriftTol) {
    counters::ledger::bump(counters::ledger::Solve::CacheCarryDrift);
  }
}

// Cached routes obtain the full correction gradient/Hessian from one
// differentiated Clenshaw traversal; no off-point finite differences remain.
template <typename Correction>
void american_greeks_first_order(double S, double K, double T, double sigma, double r, double q,
                                 Side side, const Correction *correction, AmericanGreeks &out,
                                 double *dP_dq_out = nullptr) {
  const double m = std::exp((r - q) * T); // F/S
  const double F = S * m;
  const double df = std::exp(-r * T);
  const double k_log = std::log(K / F);

  const Black76Greeks gBpk = black76_greeks(F, K, T, sigma, r, df, side);
  const Greeks gB = gBpk.greeks;
  const double euro_price = gBpk.price;

  double dc_dk = 0.0;
  double dc_dT = 0.0;
  double dc_ds = 0.0;
  double d2c_dk2 = 0.0;
  double d2c_dk_dT = 0.0;
  double d2c_dk_ds = 0.0;
  double d2c_ds2 = 0.0;
  double c_val = 0.0;
  if (correction) {
    count_cache_carry_drift(correction->baked_r(), r); // GR-P2-3
    const CorrSecondOrder corr = correction->eval_second_order(k_log, T, sigma);
    c_val = corr.value;
    dc_dk = corr.dk_log;
    dc_dT = corr.dT;
    dc_ds = corr.dsigma;
    d2c_dk2 = corr.dkk;
    d2c_dk_dT = corr.dk_dT;
    d2c_dk_ds = corr.dk_dsigma;
    d2c_ds2 = corr.dsigma2;
  }

  // A2: served mark is floored; the sensitivity fields below are NOT (see
  // floor_cached_price — the kink at the floor is intentional).
  const double intrinsic = (side == Side::Put) ? (K - S) : (S - K);
  out.price = floor_cached_price(euro_price + F * c_val, euro_price, intrinsic);

  const double D = gB.delta + c_val - dc_dk; // ∂A/∂F
  out.delta = m * D;                         // spot-delta convention
  // G2: fixed-carry ∂P/∂q. q enters ONLY through F (S·e^{(r-q)T}, ∂F/∂q = -T·F);
  // the fixed-baked correction is held constant across the carry bump (same
  // contract as rho above), so ∂P/∂q = ∂P/∂F · ∂F/∂q = D·(-T·F). This is rho's
  // through-forward leg (T·F·D) with q's opposite sign and WITHOUT rho's discount-
  // factor leg (q does not enter the discount df = e^{-rT}).
  if (dP_dq_out != nullptr) {
    *dP_dq_out = -T * F * D;
  }
  out.vega = gB.vega + F * dc_ds;
  out.rho = gB.rho + T * F * D;
  out.theta = gB.theta - (r - q) * F * D - F * dc_dT;
  const double D_F = gB.gamma + (d2c_dk2 - dc_dk) / F;
  out.gamma = m * m * D_F;
  out.vanna = m * (gB.vanna + dc_ds - d2c_dk_ds);
  out.volga = gB.volga + F * d2c_ds2;
  // Calendar charm is -d(spot delta)/dT at fixed S. Since spot delta=m*D,
  // both m(T) and F(T)=S*m(T) contribute carry terms beyond the correction's
  // explicit fixed-carry T partial.
  const double carry = r - q;
  out.charm = m * (gB.charm - dc_dT + d2c_dk_dT - carry * (D + F * D_F));
}

} // namespace

// ── Warm-started ALO pricer (fixed contract, sigma sweep) ────────────────
//
// State mirrors al_solve_put's internals but hoists the sigma-independent setup
// (node grid, Gauss-Legendre binding) into reset() and keeps the
// early-exercise boundary `bnd.y[]` alive between price() calls. All calls solve
// an internal PUT; a Call is the McDonald-Schroder put P(K,S,q,r), so the boundary
// machinery is identical.
struct AloPricer::State {
  // User-provided construction deliberately avoids value-initializing the four
  // 512-double geometry arrays. reset() binds every active element before a
  // specialized kernel can read it.
  State() noexcept {}

  double Sp{}; // internal-put spot   (= K for a call)
  double Kp{}; // internal-put strike (= S for a call) — drives the boundary
  double T{};
  double rp{}; // internal-put rate   (= q for a call)
  double qp{}; // internal-put yield  (= r for a call)
  AlScheme sch{};
  AlBoundary bnd;
  AlWorkspace ws;
  bool prepared{false};      // node grid + quadrature bound, xmax > 0
  bool european_only{false}; // no early exercise -> American == European
  bool unsupported{false};   // double-continuation corner -> price() returns NaN
  bool seeded{false};        // bnd.y[] holds a usable warm boundary
  double last_sigma{-1.0};
};

AloPricer::AloPricer(double S, double K, double T, double r, double q, Side side,
                     const std::optional<AlOpts> &opts)
    : st_(std::make_unique<State>()) {
  ATX_VOL_COUNT(AloStateAllocations);
  reset(S, K, T, r, q, side, opts);
}

void AloPricer::reset(double S, double K, double T, double r, double q, Side side,
                      const std::optional<AlOpts> &opts) noexcept {
  assert(st_ != nullptr);
  if (st_ == nullptr) {
    return; // moved-from pricer: fail safe without allocating in reset()
  }
  State &s = *st_;
  s.prepared = false;
  s.european_only = false;
  s.unsupported = false;
  s.seeded = false;
  s.last_sigma = -1.0;
  s.ws.specialize = true;
  s.ws.geo_static_bound = false;
  s.ws.qx_fp = nullptr;
  s.ws.qw_fp = nullptr;
  s.ws.n_quad_fp = 0;
  s.ws.qx_price = nullptr;
  s.ws.qw_price = nullptr;
  s.ws.n_quad_price = 0;
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
    s.unsupported = true; // prepared stays false -> price() returns NaN
    return;
  case ExerciseRegime::American:
    break;
  }
  al_init_nodes(s.bnd, s.sch.n_boundary, s.T, s.Kp, s.rp, s.qp);
  if (!(s.bnd.xmax > 0.0)) {
    return; // asymptotic boundary collapsed (matches andersen_lake NotImplemented)
  }
  const detail::GaussLegendre *fp = gl_find(s.sch.n_quad_fp);
  const detail::GaussLegendre *pr = gl_find(s.sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return; // quadrature table unavailable
  }
  s.ws.qx_fp = fp->nodes.data();
  s.ws.qw_fp = fp->weights.data();
  s.ws.n_quad_fp = s.sch.n_quad_fp;
  s.ws.qx_price = pr->nodes.data();
  s.ws.qw_price = pr->weights.data();
  s.ws.n_quad_price = s.sch.n_quad_price;
  al_bind_geometry_static(s.bnd, s.ws, s.rp, s.qp);
  s.prepared = true;
}

AloPricer::~AloPricer() = default;
AloPricer::AloPricer(AloPricer &&) noexcept = default;
AloPricer &AloPricer::operator=(AloPricer &&) noexcept = default;

double AloPricer::price(double sigma) noexcept {
  assert(st_ != nullptr);
  if (st_ == nullptr) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  State &s = *st_;
  // Degenerate T ~ 0: no time left, collapse to the spot intrinsic (internal-put
  // intrinsic Kp - Sp equals the original option's intrinsic for both sides).
  if (s.T <= 1.0e-12) {
    const double intr = s.Kp - s.Sp;
    return (intr > 0.0) ? intr : 0.0;
  }
  // Degenerate sigma ~ 0 (A4/PR-C4): the European sigma->0 limit df*(Kp-Fp)+ in the
  // transformed put space, floored at the spot intrinsic — not the spot intrinsic
  // alone, which was wrong (and discontinuous) for a carry-dominant option. Priced
  // in EVERY regime (no optionality at sigma=0), so this precedes the unsupported
  // check — matching the pre-A4 degenerate guard, which priced sigma->0 regardless
  // of regime (the double-continuation corner is a sigma>0 limitation only).
  if (!(sigma > 1.0e-8)) {
    return sigma_zero_american_limit(s.Sp, s.Kp, s.T, s.rp, s.qp, Side::Put);
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
  const bool cold =
      !s.seeded || !(s.last_sigma > 0.0) || std::fabs(sigma - s.last_sigma) > 0.12 * s.last_sigma;
  if (cold) {
    al_seed_boundary(s.bnd, sigma, s.rp, s.qp);
  }
  // reset() retained zc and the weighted rate/yield exponentials. Only the
  // sigma-dependent diffusion scale changes between residual evaluations.
  al_bind_geometry_sigma(s.bnd, s.ws, sigma, s.rp, s.qp);
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
    const AlSweepResult sw = al_jacobi_newton_sweep(s.bnd, s.ws, sigma, s.rp, s.qp);
    if (sw.all_frozen) {
      // Unsolvable boundary (al_solve_put_boundary's NotConverged corner) surfaced
      // through this pricer's NaN failure channel. s.seeded / s.last_sigma are left
      // untouched, so the next call's warm/cold decision still refers to the last
      // SOLVED sigma and never records this frozen state as a converged one.
      return std::numeric_limits<double>::quiet_NaN();
    }
    resid = sw.max_dy;
    if (resid <= s.sch.tol) {
      break;
    }
  }
  if (resid > s.sch.tol) {
    for (std::uint16_t k = 0; k < s.sch.n_iter_fp; ++k) {
      const AlSweepResult sw = al_fixed_point_sweep(s.bnd, s.ws, sigma, s.rp, s.qp);
      if (sw.all_frozen) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      resid = sw.max_dy;
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

// R-30: read the file-local specialize-off fallback tally (see g_specialize_off_fallbacks).
std::uint64_t al_geometry_specialize_off_fallback_count() noexcept {
  return g_specialize_off_fallbacks.load(std::memory_order_relaxed);
}

AlOpts al_default_opts() noexcept { return AlOpts{12, 24, 8, 1.0e-10}; }

AlOpts al_fast_opts() noexcept { return AlOpts{7, 16, 4, 1.0e-8}; }

Result<double> andersen_lake(double S, double K, double T, double sigma, double r, double q,
                             Side side, const std::optional<AlOpts> &opts) {
  return andersen_lake_core(S, K, T, sigma, r, q, side, opts, /*specialize=*/true);
}

Status andersen_lake_call_slice(double S, std::span<const double> strikes, double T, double sigma,
                                double r, double q, std::span<double> price_out,
                                const std::optional<AlOpts> &opts) {
  if (!(S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_call_slice: S must be > 0");
  }
  if (!(T >= 0.0) || !(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_call_slice: T and sigma must be >= 0");
  }
  if (strikes.size() != price_out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_call_slice: strikes / price_out length mismatch");
  }
  for (const double K : strikes) {
    if (!(K > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "andersen_lake_call_slice: every strike must be > 0");
    }
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_call_slice: r and q must be finite");
  }

  const std::size_t n = strikes.size();

  // Degenerate T ~ 0: spot intrinsic per strike (mirrors andersen_lake).
  if (T <= 1.0e-12) {
    for (std::size_t i = 0; i < n; ++i) {
      const double intr = S - strikes[i];
      price_out[i] = (intr > 0.0) ? intr : 0.0;
    }
    return Ok();
  }
  // Degenerate sigma ~ 0 (A4/PR-C4): the European sigma->0 limit floored at the
  // spot intrinsic per strike — not the spot intrinsic alone. Priced in EVERY
  // regime (no optionality at sigma=0), as the pre-A4 degenerate guard did.
  if (sigma <= 1.0e-8) {
    for (std::size_t i = 0; i < n; ++i) {
      price_out[i] = sigma_zero_american_limit(S, strikes[i], T, r, q, Side::Call);
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
  const double rp = q; // internal-put rate
  const double qp = r; // internal-put yield

  AlBoundary bnd;
  AlWorkspace ws;
  al_init_nodes(bnd, sch.n_boundary, T, /*K=*/S, rp, qp);
  if (!(bnd.xmax > 0.0)) {
    return Err(ErrorCode::NotImplemented,
               "andersen_lake_call_slice: asymptotic boundary collapsed (xmax <= 0)");
  }
  const detail::GaussLegendre *fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre *pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return Err(ErrorCode::Internal, "andersen_lake_call_slice: Gauss-Legendre table unavailable");
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
    const AlSweepResult s = al_jacobi_newton_sweep(bnd, ws, sigma, rp, qp);
    if (s.all_frozen) {
      // Same unsolvable-boundary corner al_solve_put_boundary reports as
      // NotConverged; this slice runs its own copy of that sweep schedule.
      return Err(ErrorCode::NotImplemented, kUnsolvableBoundaryMsg);
    }
    resid = s.max_dy;
    if (resid <= sch.tol) {
      break;
    }
  }
  if (resid > sch.tol) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      const AlSweepResult s = al_fixed_point_sweep(bnd, ws, sigma, rp, qp);
      if (s.all_frozen) {
        return Err(ErrorCode::NotImplemented, kUnsolvableBoundaryMsg);
      }
      resid = s.max_dy;
      if (resid <= sch.tol) {
        break;
      }
    }
  }

  // F5: the boundary is fixed (Kp = S) across every strike, so the premium's
  // strike-invariant node terms and the euro forward/discount are bound ONCE here
  // and reused — leaving log + 2 norm_cdf per (node, strike). Bit-identical: the
  // cached euro factor equals euro_put_sk's exp and b_t = xmax*bfac == al_boundary_at.
  AlPremiumCache pc;
  al_bind_premium(bnd, ws, sigma, rp, qp, pc);
  const bool pc_ok = al_premium_cache_matches(pc, bnd, ws, sigma, rp, qp);
  // Per-strike premium: only the internal-put spot Sp = K_i changes.
  for (std::size_t i = 0; i < n; ++i) {
    const double Ki = strikes[i];
    const double euro = pc_ok
                            ? black76_price(Ki * pc.euro_fwd, S, T, sigma, pc.euro_df, Side::Put)
                            : euro_put_sk(/*S=*/Ki, /*K=*/S, T, sigma, rp, qp);
    const double prem = al_put_premium(bnd, ws, /*S=*/Ki, sigma, rp, qp, pc_ok ? &pc : nullptr);
    double px = euro + prem;
    const double intr = S - Ki; // internal-put intrinsic Kp - Sp == call intrinsic
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

Status andersen_lake_put_slice(double S, std::span<const double> strikes, double T, double sigma,
                               double r, double q, std::span<double> price_out,
                               const std::optional<AlOpts> &opts) {
  if (!(S > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice: S must be > 0");
  }
  if (!(T >= 0.0) || !(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice: T and sigma must be >= 0");
  }
  if (strikes.size() != price_out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "andersen_lake_put_slice: strikes / price_out length mismatch");
  }
  for (const double K : strikes) {
    if (!(K > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice: every strike must be > 0");
    }
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument, "andersen_lake_put_slice: r and q must be finite");
  }

  const std::size_t n = strikes.size();
  // Empty slice: nothing to price, and (unlike the call slice, whose boundary is
  // solved at the FIXED internal strike S) the American arm below picks its
  // reference strike as strikes[0] — an out-of-bounds read on an empty span. The
  // call slice answers Ok() on n == 0 with no writes; match that no-op contract
  // here rather than inventing an error the call side does not report.
  if (n == 0) {
    return Ok();
  }

  // Degenerate T ~ 0: spot intrinsic per strike (mirrors andersen_lake).
  if (T <= 1.0e-12) {
    for (std::size_t i = 0; i < n; ++i) {
      const double intr = strikes[i] - S; // put intrinsic K_i - S
      price_out[i] = (intr > 0.0) ? intr : 0.0;
    }
    return Ok();
  }
  // Degenerate sigma ~ 0 (A4/PR-C4): the European sigma->0 limit floored at the
  // spot intrinsic per strike — not the spot intrinsic alone. Priced in EVERY
  // regime (no optionality at sigma=0), as the pre-A4 degenerate guard did.
  if (sigma <= 1.0e-8) {
    for (std::size_t i = 0; i < n; ++i) {
      price_out[i] = sigma_zero_american_limit(S, strikes[i], T, r, q, Side::Put);
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
  // measured gap (see at-task-8-report.md) is the ~1e-7 the correction cache's PUT
  // row now accepts by routing here (T16a) — the put cache pins were repinned and
  // revalidated to the §9 accuracy gates against cold andersen_lake for this shift.
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
    return Err(ErrorCode::Internal, "andersen_lake_put_slice: Gauss-Legendre table unavailable");
  case AlSolveStatus::NotConverged:
    return Err(ErrorCode::NotImplemented, kUnsolvableBoundaryMsg);
  case AlSolveStatus::Ok:
    break;
  }

  // F5: y[] is homogeneity-invariant, so only bnd.xmax rescales per strike — the
  // premium's pre-xmax boundary factor (and v/dq/dr, euro exps) are bound ONCE and
  // reused, with b_t = xmax_i * bfac reconstructed per strike. Bit-identical to the
  // per-strike al_put_price_from_boundary (same reused y[]; the T16a ~1e-7 boundary-
  // reuse gap is unchanged — the hoist re-derives the SAME b_t bit-for-bit).
  AlPremiumCache pc;
  al_bind_premium(bnd, ws, sigma, r, q, pc);
  for (std::size_t i = 0; i < n; ++i) {
    const double Ki = strikes[i];
    bnd.K = Ki;                       // homogeneity rescale: strike …
    bnd.xmax = al_xmax_put(Ki, r, q); // … and asymptotic level B(∞), same y[]
    price_out[i] = al_put_price_from_boundary_cached(bnd, ws, pc, S, Ki, T, sigma, r, q);
  }
  return Ok();
}

Result<double> baw_american(double S, double K, double T, double sigma, double r, double q,
                            Side side, std::uint16_t max_iter, double tol) {
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

  // Degenerate T ~ 0: spot intrinsic.
  if (T <= 1.0e-12) {
    const double intr = (side == Side::Call) ? (S - K) : (K - S);
    return Ok(intr > 0.0 ? intr : 0.0);
  }
  // Degenerate sigma ~ 0 (A4/PR-C4): the European sigma->0 limit floored at the
  // spot intrinsic (consistent with andersen_lake), NOT the spot intrinsic alone.
  // Priced in EVERY regime (no optionality at sigma=0), as the pre-A4 guard did.
  if (sigma <= 1.0e-8) {
    return Ok(sigma_zero_american_limit(S, K, T, r, q, side));
  }

  const double euro =
      (side == Side::Call) ? euro_call_sk(S, K, T, sigma, r, q) : euro_put_sk(S, K, T, sigma, r, q);

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
    NewtonCriticalStats st;
    const double Sx = newton_critical_put(K, T, sigma, r, q, q1, mi, tt, &st);
    // A1 (finding 8): reject a silently non-converged root — the range check alone
    // masked finding 1 (a bisection-exhausted midpoint stays in (0,K) but is not at
    // the smooth-pasting root).
    if (!(Sx > 0.0 && Sx < K) || !(std::fabs(st.residual) <= kBawCriticalResidualGate * tt * K)) {
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
  NewtonCriticalStats st;
  const double Sx = newton_critical_call(K, T, sigma, r, q, q2, mi, tt, &st);
  // A1 (finding 8): reject a silently non-converged root (see the put branch).
  if (!(Sx > K) || !(std::fabs(st.residual) <= kBawCriticalResidualGate * tt * K)) {
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

Result<double> american_price(double S, double K, double T, double sigma, double r, double q,
                              Side side, AmericanMethod method, const std::optional<AlOpts> &opts) {
  switch (method) {
  case AmericanMethod::AndersenLake:
    return andersen_lake(S, K, T, sigma, r, q, side, opts);
  case AmericanMethod::Baw:
    return baw_american(S, K, T, sigma, r, q, side);
  }
  return Err(ErrorCode::Internal, "american_price: unhandled method"); // unreachable
}

double american_price_cached(double S, double K, double T, double sigma, double r, double q,
                             Side side, const CorrectionCache *correction) {
  if (!correction || !correction->populated() || correction->side() != side) {
    ATX_VOL_COUNT(CacheColdFallbacks);
    const Result<double> p = andersen_lake(S, K, T, sigma, r, q, side, std::nullopt);
    return p ? *p : std::numeric_limits<double>::quiet_NaN();
  }
  // Double-continuation corner: the cached Black-76 + correction is a silently
  // wrong European-shaped number here (the cache holds no valid early-exercise
  // premium in this regime). Surface NaN, matching the cold andersen_lake path,
  // which returns NotImplemented (-> NaN) above. No-op for r>0 puts / q>0 calls.
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // A9 (core-review finding 11): NO carry-consistency assert here — INVESTIGATED
  // and deliberately omitted. The kernel is carry-agnostic BY CONTRACT: the query
  // (r, q) drives F and the Black-76 leg, while the correction is a fixed-baked-
  // carry Chebyshev interpolation. A single cache is baked at ONE representative
  // carry (session build_session_caches) yet legitimately serves the WHOLE surface,
  // so short tenors query at a q_eff that drifts well past 25 bps from baked_q()
  // (see essvi_deam_test EssviDeAm.CachedDeAmMatchesCold: the residual concentrates
  // exactly there and the served path accepts it). The 25-bps figure is C2's
  // cross-DATE cache-REUSE stale-gate (session.cpp cache_side_covers) — a SESSION-
  // level opt-in, NOT a kernel invariant. An assert on baked_r()/baked_q() at 25 bps
  // therefore fires on legitimate in-fit usage (it aborted the suite when first
  // tried), so the finding's proposed check does not hold at this layer.
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  // Share one log-moneyness evaluation between Black-76 and the correction
  // tensor. The previous path computed log(F/K) inside black76_price and then
  // log(K/F) again here for the same point.
  const double ln_fk = std::log(F / K);
  const double sqrt_t = (T > 0.0) ? std::sqrt(T) : 0.0;
  const double euro = black76_price_from_lnfk(F, K, T, sigma, df, ln_fk, sqrt_t, side);
  const double k_log = -ln_fk;
  const double corr = correction->eval(k_log, T, sigma);
  count_cache_carry_drift(correction->baked_r(), r); // GR-P2-3
  ATX_VOL_COUNT(CacheHits);
  // A2 (core-review finding 2): floor at max(intrinsic, euro, 0), matching the
  // cold clamp chain — the correction clamps to its box edge out-of-box, so the
  // raw euro + F*corr can print below intrinsic (arbitrageable) otherwise.
  const double intr = (side == Side::Put) ? (K - S) : (S - K);
  return floor_cached_price(euro + F * corr, euro, intr);
}

double american_price_cached(double S, double K, double T, double sigma, double r, double q,
                             Side side, const CorrectionBlend &correction) {
  if (!correction.usable(side)) {
    return american_price_cached(S, K, T, sigma, r, q, side,
                                 static_cast<const CorrectionCache *>(nullptr));
  }
  if (correction.upper_weight == 0.0 || correction.lower == correction.upper) {
    return american_price_cached(S, K, T, sigma, r, q, side, correction.lower);
  }
  if (correction.upper_weight == 1.0) {
    return american_price_cached(S, K, T, sigma, r, q, side, correction.upper);
  }
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  const double ln_fk = std::log(F / K);
  const double sqrt_t = (T > 0.0) ? std::sqrt(T) : 0.0;
  const double euro = black76_price_from_lnfk(F, K, T, sigma, df, ln_fk, sqrt_t, side);
  const double corr = correction.eval(-ln_fk, T, sigma);
  count_cache_carry_drift(correction.baked_r(), r); // GR-P2-3
  ATX_VOL_COUNT(CacheHits);
  // A2 (core-review finding 2): floor at max(intrinsic, euro, 0) — see the
  // single-cache overload above.
  const double intr = (side == Side::Put) ? (K - S) : (S - K);
  return floor_cached_price(euro + F * corr, euro, intr);
}

// ── Fused cached price + vega (perf review F1 + F8) ───────────────────────────
// The IV-inversion Newton step evaluates the correction tensor 3x at the same
// (k_log, T, sigma): once for the residual (american_price_cached's value) and
// twice for the vega (american_vega -> eval_grad = value + dsigma partial). These
// entries collapse that to ONE shared value traversal + one dsigma partial (stage
// a: 3 -> 2), and — once eval_value_and_dsigma fuses them — to ~1 traversal.
AmericanPriceVega american_price_and_vega_cached(double S, double K, double T, double sigma,
                                                 double r, double q, Side side,
                                                 const CorrectionCache *correction) {
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  const double ln_fk = std::log(F / K);
  const double sqrt_t = (T > 0.0) ? std::sqrt(T) : 0.0;
  // Black-76 vega leg (F8: two-output kernel, bit-identical to the greeks bundle's
  // vega; √T and ln(F/K) are shared with the price leg below).
  const double euro_vega = black76_value_and_vega(F, K, T, sigma, df, side, sqrt_t).vega;

  if (correction == nullptr || !correction->populated() || correction->side() != side) {
    // Split cold contract, mirroring the two separate entries: the price falls
    // back to the cold Andersen-Lake solve (american_price_cached), the vega is
    // the Black-76 European leg only (american_vega's null-cache path).
    ATX_VOL_COUNT(CacheColdFallbacks);
    const Result<double> p = andersen_lake(S, K, T, sigma, r, q, side, std::nullopt);
    return AmericanPriceVega{p ? *p : std::numeric_limits<double>::quiet_NaN(), euro_vega};
  }

  // ONE shared value traversal + the sigma partial, at the price path's
  // k_log = -ln(F/K) (so the American price and its vega are mutually consistent).
  double dc_ds = 0.0;
  const double corr = correction->eval_value_and_dsigma(-ln_fk, T, sigma, &dc_ds);
  // Served-correction max(0, .) gate on the derivative (see american_vega): the
  // clamped branch has zero derivative too, keeping Newton's vega consistent with
  // the served forward map.
  if (!(corr > 0.0)) {
    dc_ds = 0.0;
  }
  const double vega = euro_vega + F * dc_ds;

  // Price: NaN on the double-continuation corner (as american_price_cached). The
  // vega is still returned, but the inverter reads the NaN price and bails before
  // consuming it.
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return AmericanPriceVega{std::numeric_limits<double>::quiet_NaN(), vega};
  }
  ATX_VOL_COUNT(CacheHits);
  const double euro = black76_price_from_lnfk(F, K, T, sigma, df, ln_fk, sqrt_t, side);
  const double intr = (side == Side::Put) ? (K - S) : (S - K);
  const double price = floor_cached_price(euro + F * corr, euro, intr);
  return AmericanPriceVega{price, vega};
}

AmericanPriceVega american_price_and_vega_cached(double S, double K, double T, double sigma,
                                                 double r, double q, Side side,
                                                 const CorrectionBlend &correction) {
  // Delegate the degenerate / single-cache cases to the CorrectionCache overload,
  // byte-for-byte as american_price_cached / american_vega do — so a single-cache
  // blend reproduces the CorrectionCache path exactly (IV blend==single pin).
  if (!correction.usable(side)) {
    return american_price_and_vega_cached(S, K, T, sigma, r, q, side,
                                          static_cast<const CorrectionCache *>(nullptr));
  }
  if (correction.upper_weight == 0.0 || correction.lower == correction.upper) {
    return american_price_and_vega_cached(S, K, T, sigma, r, q, side, correction.lower);
  }
  if (correction.upper_weight == 1.0) {
    return american_price_and_vega_cached(S, K, T, sigma, r, q, side, correction.upper);
  }
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  const double ln_fk = std::log(F / K);
  const double sqrt_t = (T > 0.0) ? std::sqrt(T) : 0.0;
  const double euro_vega = black76_value_and_vega(F, K, T, sigma, df, side, sqrt_t).vega;
  // The blend's fused value+dsigma applies the SAME per-endpoint max(0, .) gate as
  // eval_dsigma, so (unlike the single-cache path) no outer gate is applied here —
  // this matches american_vega's blend overload.
  double dc_ds = 0.0;
  const double corr = correction.eval_value_and_dsigma(-ln_fk, T, sigma, &dc_ds);
  const double vega = euro_vega + F * dc_ds;
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return AmericanPriceVega{std::numeric_limits<double>::quiet_NaN(), vega};
  }
  ATX_VOL_COUNT(CacheHits);
  const double euro = black76_price_from_lnfk(F, K, T, sigma, df, ln_fk, sqrt_t, side);
  const double intr = (side == Side::Put) ? (K - S) : (S - K);
  const double price = floor_cached_price(euro + F * corr, euro, intr);
  return AmericanPriceVega{price, vega};
}

Result<AmericanGreeks> american_greeks(double S, double K, double T, double sigma, double r,
                                       double q, Side side, const CorrectionCache *correction) {
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
  // A missing, empty, or opposite-side cache has no applicable correction.
  // Match the documented null-cache contract and return the Black-76 leg.
  if (correction != nullptr && (!correction->populated() || correction->side() != side)) {
    correction = nullptr;
  }
  // Double-continuation corner: the Black-76 + correction bundle would be built
  // on a silently-wrong European price. Surface the SAME NotImplemented error the
  // other entry points use rather than a wrong Greeks bundle. No-op for r>0 puts
  // / q>0 calls (the production corpus): those classify American.
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  }
  AmericanGreeks out;
  american_greeks_first_order(S, K, T, sigma, r, q, side, correction, out);
  return Ok(out);
}

Result<AmericanGreeks> american_greeks(double S, double K, double T, double sigma, double r,
                                       double q, Side side, const CorrectionBlend &correction) {
  if (!correction.usable(side)) {
    return american_greeks(S, K, T, sigma, r, q, side,
                           static_cast<const CorrectionCache *>(nullptr));
  }
  if (correction.upper_weight == 0.0 || correction.lower == correction.upper) {
    return american_greeks(S, K, T, sigma, r, q, side, correction.lower);
  }
  if (correction.upper_weight == 1.0) {
    return american_greeks(S, K, T, sigma, r, q, side, correction.upper);
  }
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_greeks: S, K, T, sigma must be > 0");
  }
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  }
  AmericanGreeks out;
  american_greeks_first_order(S, K, T, sigma, r, q, side, &correction, out);
  return Ok(out);
}

Result<AmericanGreeks> american_greeks_fd(double S, double K, double T, double sigma, double r,
                                          double q, Side side, AmericanMethod method,
                                          const std::optional<AlOpts> &opts, bool warm_start) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_greeks_fd: S, K, T, sigma must be > 0");
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
  // A5 (core-review finding 5): the rho DOWN-bump r - hr can cross OUT of the
  // American regime into double-continuation for a PRICEABLE base contract (a put
  // with 0 < r <= hr and q < r - hr; a call bumps the internal-put YIELD, not its
  // rate). The regime is classified in the internal-put's (rate, yield) — the SAME
  // order Pput/Pcall use below (put: rate=r2, yield=q; call: rate=q, yield=r2). When
  // the down-bump is Unsupported the whole bundle used to fail with NotImplemented;
  // fall back to a one-sided FORWARD rho stencil (mirroring the near-expiry theta
  // treatment) so no bump reaches the unpriceable regime.
  const bool rho_is_call = (side == Side::Call);
  const double rho_dn_rate = rho_is_call ? q : (r - hr);
  const double rho_dn_yield = rho_is_call ? (r - hr) : q;
  const bool rho_forward =
      (classify_regime(rho_dn_rate, rho_dn_yield) == ExerciseRegime::Unsupported);

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
  const bool put_fast = (side == Side::Put) && (method == AmericanMethod::AndersenLake);
  // Call fast path (P2.1): McDonald-Schroder prices a call via an internal put, and
  // that internal-put boundary IS spot-dependent (its strike = the call spot the
  // delta/gamma stencils bump) — but homogeneous of degree one in that strike, so
  // one boundary per (sigma,r,T) state rescales across the spot stencils. Mirrors
  // put_fast structurally; see Pcall. NOT bit-identical to the cold call greeks —
  // the homogeneity rescale is exact in R, ~1e-13 in IEEE (far under the sprint's
  // ~1e-7 budget); accepted as default per the controller policy ruling.
  const bool call_fast = (side == Side::Call) && (method == AmericanMethod::AndersenLake);
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
  // One slot per DISTINCT (dsig, dr, dT) boundary state the 17 stencils below span:
  // the base, the two vol bumps (+/-hv), the two rate bumps (+/-hr) and the two time
  // bumps (+/-hT). SPOT bumps open no state — that is the whole point of the fast
  // paths (the put boundary is spot-independent; the call's internal-put boundary
  // rescales to the bumped spot by strike homogeneity) — which is why 17 stencils
  // need only these 7 boundary solves. Written as the sum so a future stencil family
  // has to restate the count HERE rather than silently outgrow a literal 7; the
  // insert guards in Pput/Pcall then bound the array access itself.
  constexpr std::size_t kFdBoundaryStates = 1u /*base*/ + 2u /*vol*/ + 2u /*rate*/ + 2u /*time*/;
  std::array<BndCache, kFdBoundaryStates> memo{};
  std::size_t n_memo = 0;
  // F5: ONE shared premium precompute for the whole bundle — rebound whenever the
  // active (dsig,dr,dT) boundary changes, reused across a state's spot stencils.
  // Held here (not in BndCache) so the seven-workspace memo bundle does not pay 7x
  // this ~2.6 KB (trap 7: american_greeks_fd's stack budget). Consecutive same-state
  // spot bumps (the p0/p_Sp/p_Sm run and each vega/time state's crosses) hit it;
  // interleaved states rebind, which merely re-pays the inline cost — never more.
  AlPremiumCache prem_cache;
  auto Pput = [&](double dS, double dsig, double dr, double dT) -> double {
    if (failed) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    const double S2 = S + dS;
    const double sig2 = sigma + dsig;
    const double r2 = r + dr;
    const double T2 = T + dT;
    // andersen_lake guards, replicated so each stencil matches a full cold call.
    // Both arms mirror andersen_lake_core in ITS order: T ~ 0 first (no time left
    // -> spot intrinsic), then sigma ~ 0 -> sigma_zero_american_limit. The sigma
    // arm used to return the bare spot intrinsic, which disagrees with the pricer
    // by the whole discounted-forward intrinsic on a carry-dominant contract
    // (r = 0, q > 0 put: df*(K - F) > 0 with the spot intrinsic at 0) — the FD
    // bundle then served a price american_price never quotes on the same inputs.
    if (T2 <= 1.0e-12) {
      const double intr = K - S2;
      return (intr > 0.0) ? intr : 0.0;
    }
    if (sig2 <= 1.0e-8) {
      return sigma_zero_american_limit(S2, K, T2, r2, q, Side::Put);
    }
    // Put no-early-exercise regime is American == European (Black-76). The
    // double-continuation corner (yield q < rate r2 <= 0) is unpriceable — defer
    // to the scalar P() path so andersen_lake's NotImplemented error propagates
    // through the bundle instead of a silently-wrong European Greeks set.
    switch (classify_regime(/*rate=*/r2, /*yield=*/q)) {
    case ExerciseRegime::European:
      return black76_price(S2 * std::exp((r2 - q) * T2), K, T2, sig2, std::exp(-r2 * T2),
                           Side::Put);
    case ExerciseRegime::Unsupported:
      return P(dS, dsig, dr, dT);
    case ExerciseRegime::American:
      break;
    }
    BndCache *c = nullptr;
    for (std::size_t i = 0; i < n_memo; ++i) {
      if (memo[i].dsig == dsig && memo[i].dr == dr && memo[i].dT == dT) {
        c = &memo[i];
        break;
      }
    }
    if (c == nullptr) {
      // Bound the insert instead of trusting the state count: a new stencil family
      // added above without growing kFdBoundaryStates would otherwise write past the
      // array. Fail LOUD in debug, and in release fall back to the exact scalar path
      // (the same escape the boundary-collapse case below takes) rather than
      // corrupting the stack. Unreachable while the states enumerated at the array's
      // declaration are the states priced here.
      assert(n_memo < memo.size());
      if (n_memo == memo.size()) {
        return P(dS, dsig, dr, dT);
      }
      c = &memo[n_memo++];
      c->dsig = dsig;
      c->dr = dr;
      c->dT = dT;
      // Warm-start a bumped boundary from the converged base (memo[0]); the base
      // itself, and every boundary when warm_start is off, is solved cold.
      const bool is_base = (dsig == 0.0 && dr == 0.0 && dT == 0.0);
      const bool can_warm = warm_start && !is_base && n_memo > 1 && memo[0].ok &&
                            memo[0].dsig == 0.0 && memo[0].dr == 0.0 && memo[0].dT == 0.0;
      const AlSolveStatus st =
          can_warm ? al_solve_put_boundary_warm(K, T2, sig2, r2, q, sch, memo[0].bnd, c->bnd, c->ws)
                   : al_solve_put_boundary(K, T2, sig2, r2, q, sch, c->bnd, c->ws);
      c->ok = (st == AlSolveStatus::Ok);
    }
    if (!c->ok) {
      return P(dS, dsig, dr, dT); // boundary collapsed: exact scalar fallback
    }
    // F5: bind the premium precompute for this boundary/state if the shared cache is
    // pointing elsewhere; the spot stencils of a state then reuse it. Put boundary is
    // FIXED across spot bumps (only S2 moves), so a bound cache stays valid.
    if (!al_premium_cache_matches(prem_cache, c->bnd, c->ws, sig2, r2, q)) {
      al_bind_premium(c->bnd, c->ws, sig2, r2, q, prem_cache);
    }
    return al_put_price_from_boundary_cached(c->bnd, c->ws, prem_cache, S2, K, T2, sig2, r2, q);
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
    // andersen_lake guards, replicated so each stencil matches a full cold call
    // (same split and order as Pput above; the sigma limit is taken in the CALL's
    // own (S,K,r,q) — sigma_zero_american_limit is side-parameterised, so no
    // McDonald-Schroder swap belongs here).
    if (T2 <= 1.0e-12) {
      const double intr = S2 - K; // call intrinsic
      return (intr > 0.0) ? intr : 0.0;
    }
    if (sig2 <= 1.0e-8) {
      return sigma_zero_american_limit(S2, K, T2, r2, q, Side::Call);
    }
    // Regime in the call's internal-put terms (rate = q, yield = r2 — the same
    // order andersen_lake_call_slice uses). European short-circuits to the Black-76
    // European CALL (matches andersen_lake exactly); the double-continuation corner
    // (yield r2 < rate q <= 0) defers to the scalar P() path so andersen_lake's
    // NotImplemented propagates through the bundle, never a silently-wrong European.
    switch (classify_regime(/*rate=*/q, /*yield=*/r2)) {
    case ExerciseRegime::European:
      return black76_price(S2 * std::exp((r2 - q) * T2), K, T2, sig2, std::exp(-r2 * T2),
                           Side::Call);
    case ExerciseRegime::Unsupported:
      return P(dS, dsig, dr, dT);
    case ExerciseRegime::American:
      break;
    }
    BndCache *c = nullptr;
    for (std::size_t i = 0; i < n_memo; ++i) {
      if (memo[i].dsig == dsig && memo[i].dr == dr && memo[i].dT == dT) {
        c = &memo[i];
        break;
      }
    }
    if (c == nullptr) {
      // Same bound as Pput's insert: see the comment there (and at the array's
      // declaration) for why the state count and the array size must not drift.
      assert(n_memo < memo.size());
      if (n_memo == memo.size()) {
        return P(dS, dsig, dr, dT);
      }
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
                            memo[0].dsig == 0.0 && memo[0].dr == 0.0 && memo[0].dT == 0.0;
      const AlSolveStatus st =
          can_warm
              ? al_solve_put_boundary_warm(/*K=*/S, T2, sig2, /*r=*/q, /*q=*/r2, sch, memo[0].bnd,
                                           c->bnd, c->ws)
              : al_solve_put_boundary(/*K=*/S, T2, sig2, /*r=*/q, /*q=*/r2, sch, c->bnd, c->ws);
      c->ok = (st == AlSolveStatus::Ok);
      // Capture the canonical base scaling (internal-strike = S) BEFORE any price
      // rescales it, so bumped states can warm-seed from an un-mutated memo[0].
      c->base_K = c->bnd.K;
      c->base_xmax = c->bnd.xmax;
    }
    if (!c->ok) {
      return P(dS, dsig, dr, dT); // boundary collapsed: exact scalar fallback
    }
    // Homogeneity rescale of the base boundary to internal-strike = S2 (keep y[],
    // reset the strike and asymptotic level), then price the internal put at spot =
    // K (the fixed call strike), strike = S2 (the bumped call spot).
    c->bnd.K = S2;
    c->bnd.xmax = al_xmax_put(S2, /*r=*/q, /*q=*/r2);
    // F5: the internal-put boundary's y[] is fixed across this state's spot stencils
    // (only xmax rescales with S2), so bind the premium precompute once per state and
    // let b_t = xmax*bfac follow the rescale — bit-identical to the per-stencil price.
    if (!al_premium_cache_matches(prem_cache, c->bnd, c->ws, sig2, /*r=*/q, /*q=*/r2)) {
      al_bind_premium(c->bnd, c->ws, sig2, /*r=*/q, /*q=*/r2, prem_cache);
    }
    const double price = al_put_price_from_boundary_cached(
        c->bnd, c->ws, prem_cache, /*spot=*/K, /*strike=*/S2, T2, sig2, /*r=*/q, /*q=*/r2);
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
  // Rate stencils (one-sided forward when the down-bump exits the American regime).
  const double p_rp = EV(0.0, 0.0, +hr, 0.0);
  const double p_rm = rho_forward ? p0 : EV(0.0, 0.0, -hr, 0.0);
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
  // Rate denominator collapses to hr for the one-sided forward rho stencil (A5).
  const double dr_den = rho_forward ? hr : (2.0 * hr);

  AmericanGreeks out;
  out.price = p0;
  out.delta = (p_Sp - p_Sm) / (2.0 * hS);
  out.gamma = (p_Sp - 2.0 * p0 + p_Sm) / (hS * hS);
  out.vega = (p_vp - p_vm) / (2.0 * hv);
  out.volga = (p_vp - 2.0 * p0 + p_vm) / (hv * hv);
  out.rho = (p_rp - p_rm) / dr_den;
  // theta = dP/dt = -dP/dT (calendar convention).
  out.theta = -(p_Tp - p_Tm) / dT_den;
  out.vanna = (p_SpVp - p_SpVm - p_SmVp + p_SmVm) / (4.0 * hS * hv);
  // charm = d2P/dS dt = -d2P/dS dT.
  out.charm = -(p_SpTp - p_SpTm - p_SmTp + p_SmTm) / (2.0 * hS * dT_den);
  return Ok(out);
}

double american_vega(double S, double K, double T, double sigma, double r, double q, Side side,
                     const CorrectionCache *correction) noexcept {
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
  // full american_greeks bundle runs for its second-order FD terms. F8: the vega
  // comes from the two-output black76_value_and_vega, not the 9-output
  // black76_greeks bundle (its vega is bit-identical — same d1, φ(d1)=norm_pdf(d1),
  // and F·df·φ(d1)·√T with commutative F·df).
  const double euro_vega = black76_value_and_vega(F, K, T, sigma, df, side).vega;
  double dc_ds = 0.0;
  if (correction != nullptr && correction->populated() && correction->side() == side) {
    const double correction_value =
        correction->eval_grad(std::log(K / F), T, sigma, nullptr, nullptr, &dc_ds);
    // The served correction is max(0, polynomial). On the clamped branch its
    // derivative is zero as well; retaining the raw polynomial partial here
    // would make Newton's vega inconsistent with the cached forward map.
    if (!(correction_value > 0.0)) {
      dc_ds = 0.0;
    }
  }
  return euro_vega + F * dc_ds;
}

double american_vega(double S, double K, double T, double sigma, double r, double q, Side side,
                     const CorrectionBlend &correction) noexcept {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return 0.0;
  }
  if (!correction.usable(side)) {
    return american_vega(S, K, T, sigma, r, q, side, static_cast<const CorrectionCache *>(nullptr));
  }
  if (correction.upper_weight == 0.0 || correction.lower == correction.upper) {
    return american_vega(S, K, T, sigma, r, q, side, correction.lower);
  }
  if (correction.upper_weight == 1.0) {
    return american_vega(S, K, T, sigma, r, q, side, correction.upper);
  }
  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  // F8: cheap two-output vega kernel (bit-identical to black76_greeks(...).vega).
  const double euro_vega = black76_value_and_vega(F, K, T, sigma, df, side).vega;
  const double dc_ds = correction.eval_dsigma(std::log(K / F), T, sigma);
  return euro_vega + F * dc_ds;
}

Result<AmericanGreeks> american_greeks_al(double S, double K, double T, double sigma, double r,
                                          double q, Side side, const std::optional<AlOpts> &opts,
                                          bool need_vega, bool need_rho, bool need_charm) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_greeks_al: S, K, T, sigma must be > 0");
  }
  // Native analytic route for genuine early exercise on BOTH sides. Under the
  // McDonald-Schroder map C(S,K,r,q) = P(K,S,q,r) a call reduces to an internal put
  // with (rate=q, yield=r); a put is the internal put itself, (rate=r, yield=q). So
  // the early-exercise regime is governed by the internal-put SHORT RATE: r for a
  // put, q for a call (classify_regime(rate, yield) -> American iff rate > 0). The
  // degenerate corners (T~0 / sigma~0) and the no-early-exercise regime (rate <= 0,
  // American == European) fall back to the exact cold FD path on either side.
  const bool is_call = (side == Side::Call);
  const double al_rate = is_call ? q : r; // internal-put short rate
  if (al_rate <= 0.0 || T <= 1.0e-12 || sigma <= 1.0e-8) {
    return american_greeks_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts,
                              /*warm_start=*/false);
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
  if (sigma - hv <= 0.0)
    hv = 0.5 * sigma;
  const double hr = 1.0e-4;
  // Rate-bump regime guard. For a PUT the r-stencil bumps the internal-put SHORT
  // RATE (= r); the down-bump r - hr <= 0 crosses out of the American regime, so the
  // bundle falls back to the exact FD path (byte-for-byte the pre-change put guard).
  // For a CALL the r-stencil bumps the internal YIELD (= r); the internal rate is
  // q > 0 (gated above) and is NEVER bumped, so classify_regime(rate=q, yield=r±hr)
  // stays American for every stencil — no regime crossing is possible and al_xmax_put
  // (rate q > 0) stays > 0. The only bumped-boundary failure a call can hit is a
  // numeric collapse, caught by the 5-solve `!= Ok` guard below.
  // Only the r± stencils bump the put short rate, so this regime guard only matters when
  // rho is requested (K4 first-order tier: a hedge {delta} bundle skips r± entirely).
  if (need_rho && !is_call && r - hr <= 0.0) {
    return american_greeks_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts,
                              /*warm_start=*/false);
  }

  // Internal-put boundary solves. For a PUT the boundary is spot-independent (strike
  // = K fixed, rate = r±, yield = q). For a CALL it is solved at the BASE internal-
  // strike = S (the unbumped call spot) with rate = q fixed, yield = r± bumped, then
  // rescaled per spot stencil by strike homogeneity in `px`.
  //
  // K4 first-order tier: skip the boundary solves the requested greeks don't need.
  // sigma+/- (2 solves) feed only vega/volga/vanna; r+/- (2 solves) only rho; the base
  // solve (delta/gamma/theta/price) always runs. A hedge {delta} bundle thus does ONE
  // boundary solve (1 BoundarySolves ledger count) instead of five — honored on the
  // SCALAR production route, not just the dark laned kernel. Any NEEDED bumped-boundary
  // corner (collapse, or r-hr crossing) still falls the whole bundle back to exact FD.
  const double Kb = is_call ? S : K; // base internal-strike
  const auto solve = [&](AlBoundary &b, AlWorkspace &w, double sig_s, double dr) -> AlSolveStatus {
    const double rate = is_call ? q : (r + dr);
    const double yield = is_call ? (r + dr) : q;
    return al_solve_put_boundary(Kb, T, sig_s, rate, yield, sch, b, w);
  };
  AlBoundary b0, bvp, bvm, brp, brm;
  AlWorkspace w0, wvp, wvm, wrp, wrm;
  bool bundle_ok = (solve(b0, w0, sigma, 0.0) == AlSolveStatus::Ok);
  if (bundle_ok && need_vega) {
    bundle_ok = (solve(bvp, wvp, sigma + hv, 0.0) == AlSolveStatus::Ok) &&
                (solve(bvm, wvm, sigma - hv, 0.0) == AlSolveStatus::Ok);
  }
  if (bundle_ok && need_rho) {
    bundle_ok = (solve(brp, wrp, sigma, +hr) == AlSolveStatus::Ok) &&
                (solve(brm, wrm, sigma, -hr) == AlSolveStatus::Ok);
  }
  if (!bundle_ok) {
    return american_greeks_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts,
                              /*warm_start=*/false);
  }

  // Price at spot stencil S2 on a given solved boundary (its own sigma/r). The PUT
  // boundary is spot-independent, so it prices directly (spot = S2, strike = K). The
  // CALL boundary depends on its strike = the call spot, so `px` rescales it to
  // internal-strike = S2 by strike homogeneity (keep y[], reset K + xmax) and prices
  // the internal put at spot = K (the fixed call strike), strike = S2. The in-place
  // rescale is safe: every px sets K + xmax before pricing, and no boundary is warm-
  // reused here (all five are solved cold above), so there is no stale-seed hazard.
  const auto px = [&](AlBoundary &b, const AlWorkspace &w, double S2, double sig2,
                      double r2) -> double {
    if (is_call) {
      b.K = S2;
      b.xmax = al_xmax_put(S2, /*r=*/q, /*q=*/r2);
      return al_put_price_from_boundary(b, w, /*spot=*/K, /*strike=*/S2, T, sig2,
                                        /*r=*/q, /*q=*/r2);
    }
    return al_put_price_from_boundary(b, w, S2, K, T, sig2, r2, q);
  };

  // Spot stencils on the base boundary (exact — boundary independent of S). v0/vSp/vSm
  // (price/delta/gamma) always; the wide S+/-2h speed stencils only for charm.
  const double v0 = px(b0, w0, S, sigma, r);
  const double vSp = px(b0, w0, S + hS, sigma, r), vSm = px(b0, w0, S - hS, sigma, r);

  AmericanGreeks out; // unrequested greeks stay 0 (default-initialised)
  out.price = v0;
  out.delta = (vSp - vSm) / (2.0 * hS);
  out.gamma = (vSp - 2.0 * v0 + vSm) / (hS * hS);
  if (need_vega) {
    // Vol stencils on the re-solved sigma+/- boundaries (incl. the vanna cross).
    const double vvp = px(bvp, wvp, S, sigma + hv, r), vvm = px(bvm, wvm, S, sigma - hv, r);
    const double vSpVp = px(bvp, wvp, S + hS, sigma + hv, r);
    const double vSmVp = px(bvp, wvp, S - hS, sigma + hv, r);
    const double vSpVm = px(bvm, wvm, S + hS, sigma - hv, r);
    const double vSmVm = px(bvm, wvm, S - hS, sigma - hv, r);
    out.vega = (vvp - vvm) / (2.0 * hv);
    out.volga = (vvp - 2.0 * v0 + vvm) / (hv * hv);
    out.vanna = (vSpVp - vSpVm - vSmVp + vSmVm) / (4.0 * hS * hv);
  }
  if (need_rho) {
    const double vrp = px(brp, wrp, S, sigma, r + hr), vrm = px(brm, wrm, S, sigma, r - hr);
    out.rho = (vrp - vrm) / (2.0 * hr);
  }

  // theta / charm from the continuation-region PDE. In the exercise region the
  // frozen price is at intrinsic (put delta -> -1 / call delta -> +1, gamma -> 0);
  // there the intrinsic (K - S for a put, S - K for a call) has no time value, so
  // theta = charm = 0. The PDE relations below are in the ORIGINAL option's (S,r,q)
  // and are side-agnostic given the correct V/delta/gamma/speed — no sign flip. theta
  // rides the base boundary (v0/delta/gamma), so it is always available; charm's speed
  // term needs the wide base spot stencils, gated on need_charm.
  const double intr0 = is_call ? (S - K) : (K - S);
  const bool exercised = (v0 <= intr0 + 1.0e-9 * K) && (intr0 > 0.0);
  out.theta = exercised
                  ? 0.0
                  : r * v0 - (r - q) * S * out.delta - 0.5 * sigma * sigma * S * S * out.gamma;
  if (need_charm) {
    if (exercised) {
      out.charm = 0.0;
    } else {
      const double vS2p = px(b0, w0, S + 2.0 * hS, sigma, r);
      const double vS2m = px(b0, w0, S - 2.0 * hS, sigma, r);
      // speed = d3V/dS3 (5-point), for charm = d(theta)/dS.
      const double speed = (vS2p - 2.0 * vSp + 2.0 * vSm - vS2m) / (2.0 * hS * hS * hS);
      out.charm = r * out.delta - (r - q) * (out.delta + S * out.gamma) -
                  0.5 * sigma * sigma * (2.0 * S * out.gamma + S * S * speed);
    }
  }
  return Ok(out);
}

// ── Carry sensitivities: ∂P/∂q and ∂P/∂Div (G2, gaps-review finding 2) ────

Result<CarryGreeks> american_carry_greeks_fd(double S, double K, double T, double sigma, double r,
                                             double q, Side side, AmericanMethod method,
                                             const std::optional<AlOpts> &opts) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_carry_greeks_fd: S, K, T, sigma must be > 0");
  }
  const double hq = 1.0e-4; // match american_greeks_fd's hr
  // A5 regime guard for the q down-bump. Under McDonald-Schroder q is the internal-
  // put YIELD for a put (internal rate = r, untouched by the q-bump) and the
  // internal-put RATE for a call. Only a CALL can leave the American regime: a
  // down-bump q - hq that drops the internal rate out of the American regime has no
  // early-exercise boundary, so use a one-sided FORWARD stencil (mirroring
  // american_greeks_fd's rho_forward / near-expiry theta). A put is always central.
  const bool is_call = (side == Side::Call);
  const bool q_forward =
      is_call && (classify_regime(/*rate=*/q - hq, /*yield=*/r) != ExerciseRegime::American);

  bool failed = false;
  atx::core::Error first_err;
  auto P = [&](double dq) -> double {
    if (failed) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    const Result<double> p = american_price(S, K, T, sigma, r, q + dq, side, method, opts);
    if (!p) {
      failed = true;
      first_err = p.error();
      return std::numeric_limits<double>::quiet_NaN();
    }
    return *p;
  };
  const double p0 = P(0.0);
  const double pqp = P(+hq);
  const double pqm = q_forward ? p0 : P(-hq);
  if (failed) {
    return Err(std::move(first_err));
  }
  CarryGreeks out;
  out.price = p0;
  out.dP_dq = q_forward ? (pqp - p0) / hq : (pqp - pqm) / (2.0 * hq);
  out.q_one_sided = q_forward;
  return Ok(out);
}

Result<CarryGreeks> american_carry_greeks_al(double S, double K, double T, double sigma, double r,
                                             double q, Side side,
                                             const std::optional<AlOpts> &opts) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_carry_greeks_al: S, K, T, sigma must be > 0");
  }
  const bool is_call = (side == Side::Call);
  const double al_rate = is_call ? q : r; // internal-put short rate
  // Degenerate / no-early-exercise corners take the exact FD reference (which itself
  // short-circuits to the European Black-76 leg where the regime demands it).
  if (al_rate <= 0.0 || T <= 1.0e-12 || sigma <= 1.0e-8) {
    return american_carry_greeks_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts);
  }
  const AlScheme sch = scheme_from_opts(opts);
  const double hq = 1.0e-4;
  // A5: for a CALL, q is the internal-put RATE; a down-bump q - hq that leaves the
  // American regime has no boundary to solve -> one-sided forward stencil. For a PUT,
  // q is the internal YIELD (internal rate = r > 0 fixed) -> always central.
  const bool q_forward =
      is_call && (classify_regime(/*rate=*/q - hq, /*yield=*/r) != ExerciseRegime::American);

  // Boundaries: base + q+ (+ q- unless one-sided). q bumps the internal RATE for a
  // call, the internal YIELD for a put; the internal strike is S (call) / K (put).
  const double Kb = is_call ? S : K;
  const auto solve = [&](AlBoundary &b, AlWorkspace &w, double dq) -> AlSolveStatus {
    const double rate = is_call ? (q + dq) : r;
    const double yield = is_call ? r : (q + dq);
    return al_solve_put_boundary(Kb, T, sigma, rate, yield, sch, b, w);
  };
  AlBoundary b0, bqp, bqm;
  AlWorkspace w0, wqp, wqm;
  bool ok =
      (solve(b0, w0, 0.0) == AlSolveStatus::Ok) && (solve(bqp, wqp, +hq) == AlSolveStatus::Ok);
  if (ok && !q_forward) {
    ok = (solve(bqm, wqm, -hq) == AlSolveStatus::Ok);
  }
  if (!ok) {
    return american_carry_greeks_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts);
  }
  // Price from a solved boundary at the option's fixed spot/strike. Carry greeks bump
  // ONLY q (no spot stencil), so no homogeneity rescale is needed on either side, and
  // al_solve_put(S,K,..) == al_solve_put_boundary + this price call makes v0/vq± bit-
  // identical to american_carry_greeks_fd for BOTH sides.
  const auto px = [&](const AlBoundary &b, const AlWorkspace &w, double q2) -> double {
    const double rate = is_call ? q2 : r;
    const double yield = is_call ? r : q2;
    const double spot = is_call ? K : S;
    const double strike = is_call ? S : K;
    return al_put_price_from_boundary(b, w, spot, strike, T, sigma, rate, yield);
  };
  const double v0 = px(b0, w0, q);
  const double vqp = px(bqp, wqp, q + hq);
  CarryGreeks out;
  out.price = v0;
  out.q_one_sided = q_forward;
  if (q_forward) {
    out.dP_dq = (vqp - v0) / hq;
  } else {
    const double vqm = px(bqm, wqm, q - hq);
    out.dP_dq = (vqp - vqm) / (2.0 * hq);
  }
  return Ok(out);
}

Result<CarryGreeks> american_carry_greeks(double S, double K, double T, double sigma, double r,
                                          double q, Side side, const CorrectionCache *correction) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_carry_greeks: S, K, T, sigma must be > 0");
  }
  if (correction != nullptr && (!correction->populated() || correction->side() != side)) {
    correction = nullptr;
  }
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  }
  AmericanGreeks g;
  double dq = 0.0;
  american_greeks_first_order(S, K, T, sigma, r, q, side, correction, g, &dq);
  CarryGreeks out;
  out.price = g.price;
  out.dP_dq = dq;
  return Ok(out);
}

Result<CarryGreeks> american_carry_greeks(double S, double K, double T, double sigma, double r,
                                          double q, Side side, const CorrectionBlend &correction) {
  if (!correction.usable(side)) {
    return american_carry_greeks(S, K, T, sigma, r, q, side,
                                 static_cast<const CorrectionCache *>(nullptr));
  }
  if (correction.upper_weight == 0.0 || correction.lower == correction.upper) {
    return american_carry_greeks(S, K, T, sigma, r, q, side, correction.lower);
  }
  if (correction.upper_weight == 1.0) {
    return american_carry_greeks(S, K, T, sigma, r, q, side, correction.upper);
  }
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_carry_greeks: S, K, T, sigma must be > 0");
  }
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  }
  AmericanGreeks g;
  double dq = 0.0;
  american_greeks_first_order(S, K, T, sigma, r, q, side, &correction, g, &dq);
  CarryGreeks out;
  out.price = g.price;
  out.dP_dq = dq;
  return Ok(out);
}

void american_dividend_sensitivities(double dP_dq, double F, double T,
                                     std::span<const double> dF_dDiv,
                                     std::span<double> dP_dDiv_out) noexcept {
  // dP/dD_i = (∂P/∂q)·(∂q_eff/∂D_i); q_eff = r - ln(F/S)/T so ∂q_eff/∂D_i =
  // (-1/(F·T))·∂F/∂D_i. A degenerate F·T (non-finite / zero) yields no sensitivity.
  const double ft = F * T;
  const bool ok = std::isfinite(dP_dq) && std::isfinite(ft) && ft != 0.0;
  const double scale = ok ? (-dP_dq / ft) : 0.0;
  const std::size_t n =
      (dF_dDiv.size() < dP_dDiv_out.size()) ? dF_dDiv.size() : dP_dDiv_out.size();
  for (std::size_t i = 0; i < n; ++i) {
    dP_dDiv_out[i] = scale * dF_dDiv[i];
  }
}

// ── Early-exercise boundary (G4, gaps finding 5) ─────────────────────────
//
// Exposes the retained Andersen-Lake boundary state: the critical price B(T) at
// time-to-expiry T. The put boundary is solved directly (internal rate=r, yield=q);
// the call boundary is the McDonald-Schroder reflection B_call = K^2/B_put(rate=q,
// yield=r) of the internal put solved with (rate,yield) swapped. The internal put's
// boundary is read at u = T (al_boundary_at's z=+1 node), i.e. the critical price
// for the FULL time-to-expiry, so it lands on the smooth-paste seam of andersen_lake.
Result<double> exercise_boundary(double K, double T, double sigma, double r, double q, Side side,
                                 const std::optional<AlOpts> &opts) {
  if (!(K > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "exercise_boundary: K must be > 0");
  }
  if (!(T >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "exercise_boundary: T must be >= 0");
  }
  if (!(sigma >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "exercise_boundary: sigma must be >= 0");
  }
  if (!(std::isfinite(r) && std::isfinite(q))) {
    return Err(ErrorCode::InvalidArgument, "exercise_boundary: r and q must be finite");
  }

  // Internal-put (rate, yield): a put solves directly; a call is the McDonald-
  // Schroder put P(K,S,q,r), so it classifies and solves with (rate=q, yield=r).
  const double rp = (side == Side::Put) ? r : q;
  const double qp = (side == Side::Put) ? q : r;

  switch (classify_regime(/*rate=*/rp, /*yield=*/qp)) {
  case ExerciseRegime::European:
    // No early exercise is ever optimal, so there is NO finite boundary (it sits
    // at S=0 for the put / S=+inf for the call). Documented sentinel — no
    // fabricated price. (put r<=0 && r<=q; call q<=0 && q<=r.)
    return Err(ErrorCode::OutOfRange,
               "exercise_boundary: European regime (early exercise never optimal) — no "
               "finite early-exercise boundary (S=0 for the put / S=+inf for the call)");
  case ExerciseRegime::Unsupported:
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  case ExerciseRegime::American:
    break;
  }

  // xmax = K*min(1, rp/qp): the homogeneity scale AND the near-expiry limit B(0+)
  // of the internal put. Guaranteed > 0 in the American regime classified above.
  const double xmax = al_xmax_put(K, rp, qp);

  // Degenerate T ~ 0 / sigma ~ 0 -> the analytic near-expiry limit. Put: B(0+) =
  // xmax = K*min(1,r/q). Call: reflect, B_call(0+) = K^2/xmax = K*max(1,r/q).
  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    const double b = (side == Side::Put) ? xmax : (K * K / xmax);
    return Ok(b);
  }

  const AlScheme sch = scheme_from_opts(opts);
  AlBoundary bnd;
  AlWorkspace ws;
  switch (al_solve_put_boundary(K, T, sigma, rp, qp, sch, bnd, ws, /*specialize=*/true)) {
  case AlSolveStatus::Collapsed:
    return Err(ErrorCode::NotImplemented,
               "exercise_boundary: asymptotic boundary collapsed (xmax <= 0)");
  case AlSolveStatus::TableMissing:
    return Err(ErrorCode::Internal, "exercise_boundary: Gauss-Legendre table unavailable");
  case AlSolveStatus::NotConverged:
    return Err(ErrorCode::NotImplemented, kUnsolvableBoundaryMsg);
  case AlSolveStatus::Ok:
    break;
  }
  const double bp = al_boundary_at(bnd, T); // internal-put critical price at u = T
  const double b = (side == Side::Put) ? bp : (K * K / bp);
  return Ok(b);
}

// ── Early-assignment risk screen (G4) ────────────────────────────────────
//
// HEURISTIC screen (not a pricing statement): the carry benefit of exercising a
// deep-ITM American option now vs. the remaining time (extrinsic) value forfeited.
// CALL benefit = pending dividend income q*S*T; PUT benefit = interest on the
// strike r*K*T (both linear/undiscounted over the remaining life T). Flagged when
// the benefit exceeds the time value AND the option is in the money.
Result<AssignmentRisk> assignment_risk(double S, double K, double T, double sigma, double r,
                                       double q, Side side, const std::optional<AlOpts> &opts) {
  const Result<double> mark =
      american_price(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts);
  if (!mark) {
    return Err(mark.error());
  }

  const bool is_call = (side == Side::Call);
  const double raw_intr = is_call ? (S - K) : (K - S);
  const double intrinsic = (raw_intr > 0.0) ? raw_intr : 0.0;
  double time_value = *mark - intrinsic;
  if (time_value < 0.0) {
    time_value = 0.0; // American mark is floored at intrinsic; guard FP noise
  }
  // Linear carry benefit over the remaining life: dividend income for the call
  // (holder forgoes divs), interest on the strike for the put.
  const double carry_benefit = is_call ? (q * S * T) : (r * K * T);

  AssignmentRisk out;
  out.carry_benefit = carry_benefit;
  out.time_value = time_value;
  out.margin = carry_benefit - time_value;
  // Only an in-the-money option can be assigned; the margin decides deep-ITM cases.
  out.at_risk = (intrinsic > 0.0) && (carry_benefit > time_value);
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
                                Side side, const std::optional<AlOpts> &opts) {
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
  if (sigma - hv <= 0.0)
    hv = 0.5 * sigma;
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

  const auto px = [&](AlBoundary &b, const AlWorkspace &w, double sig2) -> double {
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

Result<double> american_delta(double S, double K, double T, double sigma, double r, double q,
                              Side side, AmericanMethod method, const std::optional<AlOpts> &opts) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_delta: S, K, T, sigma must be > 0");
  }
  const double hS = 1.0e-3 * S; // same spot step as american_greeks_fd

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
      if (T <= 1.0e-12) {
        const double intr = K - S2;
        return (intr > 0.0) ? intr : 0.0;
      }
      if (sigma <= 1.0e-8) {
        // Same split as american_greeks_fd's Pput — this lane claims to be
        // bit-identical to that bundle's put delta, so its degenerate-sigma
        // stencil value must be the pricer's limit, not the spot intrinsic.
        return sigma_zero_american_limit(S2, K, T, r, q, Side::Put);
      }
      // European put -> Black-76; the double-continuation corner (q < r <= 0) is
      // unpriceable -> surface andersen_lake's NotImplemented via american_price.
      switch (classify_regime(/*rate=*/r, /*yield=*/q)) {
      case ExerciseRegime::European:
        return black76_price(S2 * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T), Side::Put);
      case ExerciseRegime::Unsupported: {
        const Result<double> p = american_price(S2, K, T, sigma, r, q, Side::Put, method, opts);
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

Result<double> american_delta(double S, double K, double T, double sigma, double r, double q,
                              Side side, const CorrectionBlend &correction) {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "american_delta: S, K, T, sigma must be > 0");
  }
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) == ExerciseRegime::Unsupported) {
    return Err(ErrorCode::NotImplemented, kDoubleContinuationMsg);
  }
  const double carry = std::exp((r - q) * T);
  const double F = S * carry;
  const double df = std::exp(-r * T);
  const double black_delta = black76_greeks(F, K, T, sigma, r, df, side).greeks.delta;
  double correction_value = 0.0;
  double correction_dk = 0.0;
  if (correction.usable(side)) {
    correction_value = correction.eval_value_dk(std::log(K / F), T, sigma, &correction_dk);
    ATX_VOL_COUNT(CacheHits);
  }
  return Ok(carry * (black_delta + correction_value - correction_dk));
}

namespace detail {

GaussLegendre gauss_legendre(unsigned n) {
  const GaussLegendre *t = gl_find(n);
  return t ? *t : GaussLegendre{};
}

BawResidualEval baw_residual_eval(double Sx, double K, double T, double sigma, double r, double q,
                                  Side side) noexcept {
  BawResidualEval out;
  if (!(K > 0.0 && T > 0.0 && sigma > 0.0 && Sx > 0.0)) {
    return out;
  }
  // Same regime gate as baw_american: only the standard single-boundary
  // American regime has an interior smooth-pasting critical price.
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) != ExerciseRegime::American) {
    return out;
  }
  const double sigma2 = sigma * sigma;
  const double M = 2.0 * r / sigma2;
  const double N = 2.0 * (r - q) / sigma2;
  const double h = 1.0 - std::exp(-r * T);
  if (!(h > 0.0)) {
    return out;
  }
  const double disc = (N - 1.0) * (N - 1.0) + 4.0 * M / h;
  if (!(disc >= 0.0)) {
    return out;
  }
  const double sqrt_disc = std::sqrt(disc);
  if (side == Side::Put) {
    const double q1 = 0.5 * (-(N - 1.0) - sqrt_disc);
    if (!(q1 < 0.0)) {
      return out;
    }
    out.q_exp = q1;
    out.f = put_residual(Sx, K, T, sigma, r, q, q1);
    out.fprime = put_residual_deriv(Sx, K, T, sigma, r, q, q1);
  } else {
    const double q2 = 0.5 * (-(N - 1.0) + sqrt_disc);
    if (!(q2 > 1.0)) {
      return out;
    }
    out.q_exp = q2;
    out.f = call_residual(Sx, K, T, sigma, r, q, q2);
    out.fprime = call_residual_deriv(Sx, K, T, sigma, r, q, q2);
  }
  out.ok = true;
  return out;
}

BawCriticalSolve baw_critical_solve(double K, double T, double sigma, double r, double q, Side side,
                                    std::uint16_t max_iter, double tol) noexcept {
  BawCriticalSolve out;
  if (!(K > 0.0 && T > 0.0 && sigma > 0.0)) {
    return out;
  }
  if (classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                      /*yield=*/(side == Side::Put) ? q : r) != ExerciseRegime::American) {
    return out;
  }
  const double sigma2 = sigma * sigma;
  const double M = 2.0 * r / sigma2;
  const double N = 2.0 * (r - q) / sigma2;
  const double h = 1.0 - std::exp(-r * T);
  if (!(h > 0.0)) {
    return out;
  }
  const double disc = (N - 1.0) * (N - 1.0) + 4.0 * M / h;
  if (!(disc >= 0.0)) {
    return out;
  }
  const double sqrt_disc = std::sqrt(disc);
  const std::uint16_t mi = max_iter ? max_iter : std::uint16_t{16};
  const double tt = (tol > 0.0) ? tol : 1.0e-10;
  NewtonCriticalStats st;
  if (side == Side::Put) {
    const double q1 = 0.5 * (-(N - 1.0) - sqrt_disc);
    if (!(q1 < 0.0)) {
      return out;
    }
    out.Sx = newton_critical_put(K, T, sigma, r, q, q1, mi, tt, &st);
  } else {
    const double q2 = 0.5 * (-(N - 1.0) + sqrt_disc);
    if (!(q2 > 1.0)) {
      return out;
    }
    out.Sx = newton_critical_call(K, T, sigma, r, q, q2, mi, tt, &st);
  }
  out.residual = st.residual;
  out.iters = st.iters;
  out.converged = st.converged;
  out.ok = true;
  return out;
}

Result<double> andersen_lake_generic_kernel(double S, double K, double T, double sigma, double r,
                                            double q, Side side,
                                            const std::optional<AlOpts> &opts) {
  return andersen_lake_core(S, K, T, sigma, r, q, side, opts, /*specialize=*/false);
}

namespace {
// Bitwise double compare for the A6 audit — `==` cannot certify bit-identity (it is
// true for +0/-0 and false for NaN/NaN, both of which the audit must call correctly).
[[nodiscard]] bool bits_identical(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}
} // namespace

AlBaryHoistAudit al_bary_hoist_audit(double K, double T, double sigma, double r, double q,
                                     const std::optional<AlOpts> &opts) noexcept {
  AlBaryHoistAudit out;
  const AlScheme sch = scheme_from_opts(opts);
  AlBoundary bnd;
  AlWorkspace ws;
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    return out;
  }
  const detail::GaussLegendre *fp = gl_find(sch.n_quad_fp);
  const detail::GaussLegendre *pr = gl_find(sch.n_quad_price);
  if (!fp || !fp->ok || !pr || !pr->ok) {
    return out;
  }
  ws.qx_fp = fp->nodes.data();
  ws.qw_fp = fp->weights.data();
  ws.n_quad_fp = sch.n_quad_fp;
  ws.qx_price = pr->nodes.data();
  ws.qw_price = pr->weights.data();
  ws.n_quad_price = sch.n_quad_price;
  al_bind_geometry(bnd, ws, sigma, r, q);
  out.specialized = ws.specialize && ws.geo_static_bound;
  if (!out.specialized) {
    return out; // generic scheme: the hoisted kernel is not taken, nothing to audit
  }
  const double *xs = ws.qx_fp;
  const unsigned nq = ws.n_quad_fp;
  const unsigned nb = bnd.n;
  for (std::uint16_t j = 1; j < bnd.n; ++j) {
    const double tau = bnd.tau[j];
    if (tau <= 1.0e-14) {
      continue;
    }
    const double half_tau = 0.5 * tau;
    const unsigned gbase = static_cast<unsigned>(j) * kGeoQuadStride;
    const unsigned bpair = (static_cast<unsigned>(j) - 1u) * nq;
    for (unsigned i = 0; i < nq; ++i) {
      const double u = half_tau * (1.0 + xs[i]);
      if (tau - u <= 1.0e-14) {
        continue;
      }
      ++out.entries;
      // The INLINE form al_cheb_eval_t evaluated on every sweep, recomputed here from
      // the stored zc — same operands, same order.
      const double zc = ws.geo_zc[gbase + i];
      const unsigned bbase = (bpair + i) * nb;
      double den = 0.0;
      int hit = -1;
      bool bad = false;
      for (unsigned k = 0; k < nb; ++k) {
        const double dz = zc - bnd.z[k];
        if (dz == 0.0) {
          hit = static_cast<int>(k);
          break;
        }
        const double qq = bnd.wbary[k] / dz;
        if (!bits_identical(qq, ws.geo_bary[bbase + k])) {
          bad = true;
        }
        den += qq;
      }
      if (!bits_identical(den, ws.geo_bary_den[bpair + i]) ||
          hit != static_cast<int>(ws.geo_bary_hit[bpair + i])) {
        bad = true;
      }
      if (bad) {
        ++out.mismatches;
      }
    }
  }
  return out;
}

Result<double> andersen_lake_seeded(double S, double K, double T, double sigma, double r, double q,
                                    Side side, const std::optional<AlOpts> &opts, AlSeedMode seed,
                                    std::uint16_t n_quad_price) {
  return andersen_lake_core(S, K, T, sigma, r, q, side, opts, /*specialize=*/true, seed,
                            n_quad_price);
}

int al_boundary_jn_sweeps_to_converge(double K, double T, double sigma, double r, double q,
                                      const std::optional<AlOpts> &opts, AlSeedMode seed,
                                      double tol, int max_sweeps) {
  const AlScheme sch = scheme_from_opts(opts);
  AlBoundary bnd;
  AlWorkspace ws;
  al_init_nodes(bnd, sch.n_boundary, T, K, r, q);
  if (!(bnd.xmax > 0.0)) {
    return -1;
  }
  const GaussLegendre *fp = gl_find(sch.n_quad_fp);
  const GaussLegendre *pr = gl_find(sch.n_quad_price);
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
      const AlSweepResult s = al_jacobi_newton_sweep(bnd, ws, sigma, r, q);
      // A frozen sweep reproduces itself exactly, so there is nothing left to
      // pre-converge; stop and let the counting loop below report non-convergence.
      if (s.all_frozen || s.max_dy <= 1.0e-15) {
        break;
      }
    }
  } else {
    al_seed_boundary(bnd, sigma, r, q);
  }

  for (int k = 0; k < max_sweeps; ++k) {
    const AlSweepResult s = al_jacobi_newton_sweep(bnd, ws, sigma, r, q);
    if (s.all_frozen) {
      // max_dy == 0 because NOTHING moved, not because the boundary converged —
      // this seam measures sweeps-to-converge, so that must not read as 1 sweep.
      break;
    }
    if (s.max_dy <= tol) {
      return k + 1;
    }
  }
  return max_sweeps;
}

} // namespace detail

} // namespace atx::vol
