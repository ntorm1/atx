#include "fitting/cstar.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/error.hpp"
#include "fitting/legacy_cstar_surface.hpp" // CStarSurface (demoted, S4-T21)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// ── No-arb sweep grid (ats_vol_cstar_arb.c) ────────────────────────────────
constexpr int kArbGridN = 240;
constexpr double kArbZLo = -5.5;
constexpr double kArbZHi = 5.5;

// ── Calendar projection knobs (ats_vol_cstar_arb.c) ────────────────────────
constexpr int kCalGridN = 25;
constexpr double kCalZLo = -3.0;
constexpr double kCalZHi = 3.0;
constexpr int kCalMaxPasses = 6;
constexpr double kCalDefaultThetaBump = 1.5;

// Quintic Hermite smooth-step H(t) = 6t⁵ − 15t⁴ + 10t³ on [0, 1].
[[nodiscard]] double smooth_step(double t) noexcept {
  if (t <= 0.0) {
    return 0.0;
  }
  if (t >= 1.0) {
    return 1.0;
  }
  return t * t * t * (10.0 + t * (-15.0 + 6.0 * t));
}

// H'(t) = 30t⁴ − 60t³ + 30t² = 30·t²·(1−t)²; H''(t) = 60·t·(1−t)·(1−2t). Both
// vanish at t=0 and t=1 (H is C2 there), so the windows below are C2 across the
// whole z-line and the closed-form second derivative is continuous.
[[nodiscard]] double smooth_step_d1(double t) noexcept {
  if (t <= 0.0 || t >= 1.0) {
    return 0.0;
  }
  const double omt = 1.0 - t;
  return 30.0 * t * t * omt * omt;
}

[[nodiscard]] double smooth_step_d2(double t) noexcept {
  if (t <= 0.0 || t >= 1.0) {
    return 0.0;
  }
  return 60.0 * t * (1.0 - t) * (1.0 - 2.0 * t);
}

// Value + first two z-derivatives of a window (or any C2 scalar of z).
struct WinD2 {
  double v{};
  double d1{};
  double d2{};
};

// Window: 1 on [-1, +1], 0 outside [-3, +3], smooth C2 blend between.
[[nodiscard]] double atm_window(double z) noexcept {
  const double a = std::fabs(z);
  if (a <= 1.0) {
    return 1.0;
  }
  if (a >= 3.0) {
    return 0.0;
  }
  return smooth_step((3.0 - a) * 0.5);
}

// Right wing: 0 for z ≤ 1, 1 for z ≥ 3, smooth between.
[[nodiscard]] double right_wing_window(double z) noexcept {
  if (z <= 1.0) {
    return 0.0;
  }
  if (z >= 3.0) {
    return 1.0;
  }
  return smooth_step((z - 1.0) * 0.5);
}

// Left wing: 0 for z ≥ -1, 1 for z ≤ -3, smooth between.
[[nodiscard]] double left_wing_window(double z) noexcept {
  if (z >= -1.0) {
    return 0.0;
  }
  if (z <= -3.0) {
    return 1.0;
  }
  return smooth_step((-z - 1.0) * 0.5);
}

// Closed-form value + z-derivatives of the three windows. In the blend region
// each window is H(arg(z)) with arg affine in z: arg' is a constant c, arg''=0,
// so value' = c·H'(arg) and value'' = c²·H''(arg). In the flat regions the
// window is constant, hence both derivatives are zero.
[[nodiscard]] WinD2 atm_window_d2(double z) noexcept {
  const double a = std::fabs(z);
  if (a <= 1.0 || a >= 3.0) {
    return {atm_window(z), 0.0, 0.0};
  }
  const double arg = (3.0 - a) * 0.5;           // d(arg)/dz = -0.5·sign(z)
  const double c = (z >= 0.0) ? -0.5 : 0.5;     // = -0.5·sign(z)
  return {smooth_step(arg), c * smooth_step_d1(arg),
          c * c * smooth_step_d2(arg)};
}

[[nodiscard]] WinD2 right_wing_window_d2(double z) noexcept {
  if (z <= 1.0 || z >= 3.0) {
    return {right_wing_window(z), 0.0, 0.0};
  }
  const double arg = (z - 1.0) * 0.5;           // d(arg)/dz = +0.5
  return {smooth_step(arg), 0.5 * smooth_step_d1(arg),
          0.25 * smooth_step_d2(arg)};
}

[[nodiscard]] WinD2 left_wing_window_d2(double z) noexcept {
  if (z >= -1.0 || z <= -3.0) {
    return {left_wing_window(z), 0.0, 0.0};
  }
  const double arg = (-z - 1.0) * 0.5;          // d(arg)/dz = -0.5
  return {smooth_step(arg), -0.5 * smooth_step_d1(arg),
          0.25 * smooth_step_d2(arg)};
}

// True iff mode j is active in the bitmask. Shift on `unsigned` so the
// signedness-changing implicit conversions of `int >> size_t` are avoided
// (/Wsign-conversion clean).
[[nodiscard]] bool mode_active(std::uint16_t active_modes,
                               std::size_t j) noexcept {
  return ((static_cast<unsigned>(active_modes) >> j) & 1u) != 0u;
}

// Value + first two z-derivatives of the j'th compact-support mode
// B_j(z) = (1 − u²)³, u = (z − c_j)/h. With du/dz = 1/h:
//   B'(z)  = −6·u·(1 − u²)² · (1/h)
//   B''(z) = −6·(1 − u²)·(1 − 5u²) · (1/h²)
// (dB/du = −6u(1−u²)², d²B/du² = −6(1−u²)(1−5u²); verified by expanding
// (1−u²)³ and differentiating.) Zero outside |u| ≤ 1 or for out-of-range j.
[[nodiscard]] WinD2 cstar_basis_d2(int j, double z) noexcept {
  if (j < 0 || j >= static_cast<int>(kCStarNModes)) {
    return {};
  }
  const double inv_h = 1.0 / kCStarBasisH;
  const double u = (z - kCStarCenters[static_cast<std::size_t>(j)]) * inv_h;
  if (u < -1.0 || u > 1.0) {
    return {};
  }
  const double omu2 = 1.0 - u * u;
  return {omu2 * omu2 * omu2, -6.0 * u * omu2 * omu2 * inv_h,
          -6.0 * omu2 * (1.0 - 5.0 * u * u) * inv_h * inv_h};
}

// f(z), f'(z), f''(z) of the full modal shape (base + active modes), in closed
// form. f value component is structurally identical to cstar_base() so the two
// cannot drift. This replaces the prior central-FD `w_and_derivs`, whose w''
// via (w₊−2w₀+w₋)/h² (h=1e-4 ⇒ h²=1e-8) lost ~8 digits to cancellation and
// could false-flag/false-clear the butterfly gate near the boundary.
struct FDerivs {
  double f{};
  double fp{};
  double fpp{};
};

[[nodiscard]] FDerivs f_and_derivs_z(const CStarParams& s, double z) noexcept {
  const double atm = 1.0 + 2.0 * s.s2 * z + s.c2 * z * z;
  const double atmp = 2.0 * s.s2 + 2.0 * s.c2 * z;
  const double atmpp = 2.0 * s.c2;

  const WinD2 wa = atm_window_d2(z);
  const WinD2 wr = right_wing_window_d2(z);
  const WinD2 wl = left_wing_window_d2(z);

  const double rl = z * s.C_right + 1.0;  // right line; rl' = C_right, rl'' = 0
  const double rlp = s.C_right;
  const double ll = -z * s.C_left + 1.0;  // left line;  ll' = -C_left, ll'' = 0
  const double llp = -s.C_left;

  double f = wa.v * atm + wr.v * rl + wl.v * ll;
  double fp = wa.d1 * atm + wa.v * atmp + wr.d1 * rl + wr.v * rlp +
              wl.d1 * ll + wl.v * llp;
  double fpp = wa.d2 * atm + 2.0 * wa.d1 * atmp + wa.v * atmpp + wr.d2 * rl +
               2.0 * wr.d1 * rlp + wl.d2 * ll + 2.0 * wl.d1 * llp;

  if (s.active_modes != 0u) {
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      if (mode_active(s.active_modes, j)) {
        const WinD2 b = cstar_basis_d2(static_cast<int>(j), z);
        f += s.beta[j] * b.v;
        fp += s.beta[j] * b.d1;
        fpp += s.beta[j] * b.d2;
      }
    }
  }
  return {f, fp, fpp};
}

// RAW (un-floored) total variance w = θ·f(z) and its k-derivatives, in closed
// form. z = k/√θ ⇒ w' = √θ·f'(z), w'' = f''(z). Distinct from the public
// cstar_slice_w 1e-12 floor: w here may be ≤ 0 where the shape is degenerate,
// which the Roper functional treats as an invalid (−∞) point. θ > 0 is a caller
// precondition (cstar_min_roper_g / cstar_shape_valid guard it).
struct WDerivs {
  double w{};
  double wp{};
  double wpp{};
};

[[nodiscard]] WDerivs w_and_derivs(const CStarParams& s, double k) noexcept {
  const double sqrt_theta = std::sqrt(s.theta);
  const double z = k / sqrt_theta;
  const FDerivs d = f_and_derivs_z(s, z);
  return {s.theta * d.f, sqrt_theta * d.fp, d.fpp};
}

// Fused single-pass evaluation of the floored total variance w AND the full
// 16-partial gradient ∂w/∂(theta, s2, c2, C_left, C_right, beta[0..10]). One
// shape traversal replaces the prior cstar_slice_w + cstar_slice_grad_w pair
// (two independent f-evaluations) inside the LM normal-equations build, and
// the theta partial uses the CLOSED-FORM f'(z) instead of a central FD (which
// lost ~half the significant digits). `ok` is false when theta ≤ 0.
struct WGradFull {
  double w{};
  std::array<double, kCStarNParams> grad{};
  bool ok{false};
};

[[nodiscard]] WGradFull eval_w_and_grad(const CStarParams& s,
                                        double k_log) noexcept {
  WGradFull out;
  if (!(s.theta > 0.0)) {
    return out;  // ok == false
  }
  const double sqrt_theta = std::sqrt(s.theta);
  const double z = k_log / sqrt_theta;

  const double atm = 1.0 + 2.0 * s.s2 * z + s.c2 * z * z;
  const double atmp = 2.0 * s.s2 + 2.0 * s.c2 * z;
  const WinD2 wa = atm_window_d2(z);
  const WinD2 wr = right_wing_window_d2(z);
  const WinD2 wl = left_wing_window_d2(z);
  const double rl = z * s.C_right + 1.0;
  const double ll = -z * s.C_left + 1.0;

  double f = wa.v * atm + wr.v * rl + wl.v * ll;
  double fp = wa.d1 * atm + wa.v * atmp + wr.d1 * rl + wr.v * s.C_right +
              wl.d1 * ll + wl.v * (-s.C_left);

  // One mode traversal: accumulate f/f' from active modes AND fill every
  // ∂w/∂beta_j = θ·B_j(z) (defined for all modes; extraction picks the active).
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    const WinD2 b = cstar_basis_d2(static_cast<int>(j), z);
    out.grad[kCStarNBase + j] = s.theta * b.v;
    if (mode_active(s.active_modes, j)) {
      f += s.beta[j] * b.v;
      fp += s.beta[j] * b.d1;
    }
  }

  out.grad[0] = f - 0.5 * z * fp;          // ∂w/∂theta (analytic f')
  out.grad[1] = s.theta * 2.0 * z * wa.v;  // ∂w/∂s2
  out.grad[2] = s.theta * z * z * wa.v;    // ∂w/∂c2
  out.grad[3] = s.theta * wl.v * (-z);     // ∂w/∂C_left
  out.grad[4] = s.theta * wr.v * z;        // ∂w/∂C_right

  double w = s.theta * f;
  if (!std::isfinite(w) || w <= 1.0e-12) {
    w = 1.0e-12;  // same public variance floor as cstar_slice_w
  }
  out.w = w;
  out.ok = true;
  return out;
}

// ── Table-driven no-arb sweep (SPRINT W5.1) ────────────────────────────────
//
// The Roper min-g sweep over the fixed 240-point z-grid runs ~100–160×/project
// (30-iteration bisection × up to 4 mode groups + the 40-iteration c2 fallback).
// The window values (atm / right-wing / left-wing) and the modal basis B_j with
// its first two derivatives are functions of z ALONE — the grid and the mode
// centers are fixed — so they are constant across every sweep and every slice.
// Precompute them once into a static table; each sweep is then a
// transcendental-free BLAS-1 pass whose f/f'/f'' are affine combinations of the
// tabulated columns with the slice's current (θ, s2, c2, wings, β). The
// arithmetic is identical to f_and_derivs_z (same terms, same order), so the
// result matches the per-point analytic path to the ULP.
struct RoperGridTable {
  std::array<double, kArbGridN> z{};
  std::array<double, kArbGridN> wa{};    // atm window value/d1/d2
  std::array<double, kArbGridN> wap{};
  std::array<double, kArbGridN> wapp{};
  std::array<double, kArbGridN> wr{};    // right-wing window value/d1/d2
  std::array<double, kArbGridN> wrp{};
  std::array<double, kArbGridN> wrpp{};
  std::array<double, kArbGridN> wl{};    // left-wing window value/d1/d2
  std::array<double, kArbGridN> wlp{};
  std::array<double, kArbGridN> wlpp{};
  std::array<std::array<double, kCStarNModes>, kArbGridN> b{};    // basis value
  std::array<std::array<double, kCStarNModes>, kArbGridN> bp{};   // basis d1
  std::array<std::array<double, kCStarNModes>, kArbGridN> bpp{};  // basis d2
};

[[nodiscard]] const RoperGridTable& roper_grid_table() noexcept {
  static const RoperGridTable table = [] {
    RoperGridTable t;
    for (int i = 0; i < kArbGridN; ++i) {
      const auto ui = static_cast<std::size_t>(i);
      const double z = kArbZLo + (kArbZHi - kArbZLo) * static_cast<double>(i) /
                                     static_cast<double>(kArbGridN - 1);
      t.z[ui] = z;
      const WinD2 a = atm_window_d2(z);
      const WinD2 r = right_wing_window_d2(z);
      const WinD2 l = left_wing_window_d2(z);
      t.wa[ui] = a.v;
      t.wap[ui] = a.d1;
      t.wapp[ui] = a.d2;
      t.wr[ui] = r.v;
      t.wrp[ui] = r.d1;
      t.wrpp[ui] = r.d2;
      t.wl[ui] = l.v;
      t.wlp[ui] = l.d1;
      t.wlpp[ui] = l.d2;
      for (std::size_t j = 0; j < kCStarNModes; ++j) {
        const WinD2 bd = cstar_basis_d2(static_cast<int>(j), z);
        t.b[ui][j] = bd.v;
        t.bp[ui][j] = bd.d1;
        t.bpp[ui][j] = bd.d2;
      }
    }
    return t;
  }();
  return table;
}

// Compact the active mode indices into `out` (length ≥ kCStarNModes); returns
// the count. Hoisted out of the sweep so the BLAS-1 pass touches only live
// columns.
[[nodiscard]] std::size_t active_mode_list(
    std::uint16_t active_modes,
    std::array<std::size_t, kCStarNModes>& out) noexcept {
  std::size_t n = 0;
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    if (mode_active(active_modes, j)) {
      out[n++] = j;
    }
  }
  return n;
}

// One grid point's raw variance + k-derivatives from the tabulated columns.
struct GridPoint {
  double w{};
  double wp{};
  double wpp{};
  double k{};
};

[[nodiscard]] GridPoint sweep_point(
    const RoperGridTable& t, const CStarParams& s, double sqrt_theta,
    const std::array<std::size_t, kCStarNModes>& active, std::size_t n_active,
    std::size_t ui) noexcept {
  const double z = t.z[ui];
  const double atm = 1.0 + 2.0 * s.s2 * z + s.c2 * z * z;
  const double atmp = 2.0 * s.s2 + 2.0 * s.c2 * z;
  const double atmpp = 2.0 * s.c2;
  const double rl = z * s.C_right + 1.0;
  const double ll = -z * s.C_left + 1.0;

  double f = t.wa[ui] * atm + t.wr[ui] * rl + t.wl[ui] * ll;
  double fp = t.wap[ui] * atm + t.wa[ui] * atmp + t.wrp[ui] * rl +
              t.wr[ui] * s.C_right + t.wlp[ui] * ll + t.wl[ui] * (-s.C_left);
  double fpp = t.wapp[ui] * atm + 2.0 * t.wap[ui] * atmp + t.wa[ui] * atmpp +
               t.wrpp[ui] * rl + 2.0 * t.wrp[ui] * s.C_right + t.wlpp[ui] * ll +
               2.0 * t.wlp[ui] * (-s.C_left);
  for (std::size_t a = 0; a < n_active; ++a) {
    const std::size_t j = active[a];
    const double bj = s.beta[j];
    f += bj * t.b[ui][j];
    fp += bj * t.bp[ui][j];
    fpp += bj * t.bpp[ui][j];
  }
  return {s.theta * f, sqrt_theta * fp, fpp, z * sqrt_theta};
}

// Minimum Roper g over the tabulated grid (with the 1/w division — the exact
// value the public API returns). theta > 0 is a caller precondition.
[[nodiscard]] double roper_min_g_tab(const CStarParams& s) noexcept {
  const RoperGridTable& t = roper_grid_table();
  const double sqrt_theta = std::sqrt(s.theta);
  std::array<std::size_t, kCStarNModes> active{};
  const std::size_t n_active = active_mode_list(s.active_modes, active);

  double g_min = kInf;
  for (int i = 0; i < kArbGridN; ++i) {
    const GridPoint p =
        sweep_point(t, s, sqrt_theta, active, n_active, static_cast<std::size_t>(i));
    if (!(p.w > 0.0)) {
      return -kInf;  // degenerate raw variance (matches the per-point −∞ signal)
    }
    const double tt = 1.0 - p.k * p.wp / (2.0 * p.w);
    const double g =
        tt * tt - 0.25 * p.wp * p.wp * (1.0 / p.w + 0.25) + 0.5 * p.wpp;
    if (g < g_min) {
      g_min = g;
    }
  }
  return g_min;
}

// Division-free butterfly-arb-free predicate: true iff min Roper g ≥ 0.
//
// Since w > 0 at every grid point of an arb-free slice, sign(g) = sign(w²·g),
// and w²·g = (w − ½·k·w')² − ¼·w'²·(w + ¼·w²) + ½·w''·w² has NO 1/w division.
// The projection's inner bisections only ever sign-test min g ≥ 0, so routing
// them through this predicate removes the per-point division (the dominant cost)
// from the ~100–160 sweeps a projection performs — the real S3 win on top of
// the table. The public cstar_min_roper_g keeps the exact-value form above.
[[nodiscard]] bool roper_arb_free(const CStarParams& s) noexcept {
  if (!(s.theta > 0.0)) {
    return false;
  }
  const RoperGridTable& t = roper_grid_table();
  const double sqrt_theta = std::sqrt(s.theta);
  std::array<std::size_t, kCStarNModes> active{};
  const std::size_t n_active = active_mode_list(s.active_modes, active);

  for (int i = 0; i < kArbGridN; ++i) {
    const GridPoint p =
        sweep_point(t, s, sqrt_theta, active, n_active, static_cast<std::size_t>(i));
    if (!(p.w > 0.0)) {
      return false;
    }
    const double whalf = p.w - 0.5 * p.k * p.wp;
    const double w2g = whalf * whalf -
                       0.25 * p.wp * p.wp * (p.w + 0.25 * p.w * p.w) +
                       0.5 * p.wpp * p.w * p.w;
    if (w2g < 0.0) {
      return false;
    }
  }
  return true;
}

// Largest uniform damping factor in [0, 1] on the group's modal coefficients
// keeping min Roper g ≥ 0 (bisection). Non-group betas are held at `saved`;
// group betas are set to original[j] · factor. (ats_vol_cstar_arb.c)
//
// SPRINT W5.1 — incremental BLAS-1 sweep over the damping factor. The base
// shape and the NON-group modes are fixed across the 30 bisection iterations;
// only the group's contribution scales linearly with `factor`. So the fixed
// part f0/f0'/f0'' and the scalable part g/g'/g'' (group modes at `original`
// amplitude) are precomputed ONCE per bisection into six L1-resident grid
// vectors — one pass over the 63 KB static table — and each iteration is then a
// division-free f = f0 + factor·g sweep over cache-resident data. This replaces
// the prior 30 full re-sweeps of the table (the projection was memory-bound
// streaming the mode columns on every damping evaluation). Same betas at every
// factor ⇒ the same converged damping to within FP reassociation.
[[nodiscard]] double bisect_group_damping(
    CStarParams& s, std::uint16_t group_mask,
    const std::array<double, kCStarNModes>& original) noexcept {
  const std::array<double, kCStarNModes> saved = s.beta;

  if (roper_arb_free(s)) {
    return 1.0;
  }

  const RoperGridTable& t = roper_grid_table();
  const double sqrt_theta = std::sqrt(s.theta);

  // Partition active modes into fixed (non-group, at `saved`) and scalable
  // (group, at `original`).
  std::array<std::size_t, kCStarNModes> fixed{};
  std::array<std::size_t, kCStarNModes> grp{};
  std::size_t n_fixed = 0;
  std::size_t n_grp = 0;
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    if (!mode_active(s.active_modes, j)) {
      continue;
    }
    if (mode_active(group_mask, j)) {
      grp[n_grp++] = j;
    } else {
      fixed[n_fixed++] = j;
    }
  }

  // Precompute f0/f0'/f0'' (base + fixed modes) and g/g'/g'' (group at original).
  std::array<double, kArbGridN> f0{};
  std::array<double, kArbGridN> f0p{};
  std::array<double, kArbGridN> f0pp{};
  std::array<double, kArbGridN> gc{};
  std::array<double, kArbGridN> gcp{};
  std::array<double, kArbGridN> gcpp{};
  for (int i = 0; i < kArbGridN; ++i) {
    const auto ui = static_cast<std::size_t>(i);
    const double z = t.z[ui];
    const double atm = 1.0 + 2.0 * s.s2 * z + s.c2 * z * z;
    const double atmp = 2.0 * s.s2 + 2.0 * s.c2 * z;
    const double atmpp = 2.0 * s.c2;
    const double rl = z * s.C_right + 1.0;
    const double ll = -z * s.C_left + 1.0;
    double f = t.wa[ui] * atm + t.wr[ui] * rl + t.wl[ui] * ll;
    double fp = t.wap[ui] * atm + t.wa[ui] * atmp + t.wrp[ui] * rl +
                t.wr[ui] * s.C_right + t.wlp[ui] * ll + t.wl[ui] * (-s.C_left);
    double fpp = t.wapp[ui] * atm + 2.0 * t.wap[ui] * atmp + t.wa[ui] * atmpp +
                 t.wrpp[ui] * rl + 2.0 * t.wrp[ui] * s.C_right +
                 t.wlpp[ui] * ll + 2.0 * t.wlp[ui] * (-s.C_left);
    for (std::size_t a = 0; a < n_fixed; ++a) {
      const std::size_t j = fixed[a];
      f += saved[j] * t.b[ui][j];
      fp += saved[j] * t.bp[ui][j];
      fpp += saved[j] * t.bpp[ui][j];
    }
    f0[ui] = f;
    f0p[ui] = fp;
    f0pp[ui] = fpp;
    double g = 0.0;
    double gp = 0.0;
    double gpp = 0.0;
    for (std::size_t a = 0; a < n_grp; ++a) {
      const std::size_t j = grp[a];
      g += original[j] * t.b[ui][j];
      gp += original[j] * t.bp[ui][j];
      gpp += original[j] * t.bpp[ui][j];
    }
    gc[ui] = g;
    gcp[ui] = gp;
    gcpp[ui] = gpp;
  }

  const auto arb_free_at = [&](double factor) noexcept -> bool {
    for (int i = 0; i < kArbGridN; ++i) {
      const auto ui = static_cast<std::size_t>(i);
      const double f = f0[ui] + factor * gc[ui];
      const double w = s.theta * f;
      if (!(w > 0.0)) {
        return false;
      }
      const double fp = f0p[ui] + factor * gcp[ui];
      const double fpp = f0pp[ui] + factor * gcpp[ui];
      const double wp = sqrt_theta * fp;
      const double k = t.z[ui] * sqrt_theta;
      const double whalf = w - 0.5 * k * wp;
      const double w2g =
          whalf * whalf - 0.25 * wp * wp * (w + 0.25 * w * w) + 0.5 * fpp * w * w;
      if (w2g < 0.0) {
        return false;
      }
    }
    return true;
  };

  double lo = 0.0;
  double hi = 1.0;
  for (int it = 0; it < 30; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (arb_free_at(mid)) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  for (std::size_t a = 0; a < n_grp; ++a) {
    s.beta[grp[a]] = original[grp[a]] * lo;  // non-group betas already == saved
  }
  return lo;
}

// Split the active modes of `slice` into per-group bitmasks.
struct GroupMasks {
  std::uint16_t far{};
  std::uint16_t shoulder{};
  std::uint16_t near_wing{};
  std::uint16_t atm{};
};

[[nodiscard]] GroupMasks group_masks(std::uint16_t active_modes) noexcept {
  GroupMasks m;
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    if (!mode_active(active_modes, j)) {
      continue;
    }
    const auto bit = static_cast<std::uint16_t>(1u << j);
    switch (kCStarModeGroup[j]) {
    case CStarModeGroup::FarWing:
      m.far = static_cast<std::uint16_t>(m.far | bit);
      break;
    case CStarModeGroup::Shoulder:
      m.shoulder = static_cast<std::uint16_t>(m.shoulder | bit);
      break;
    case CStarModeGroup::NearWing:
      m.near_wing = static_cast<std::uint16_t>(m.near_wing | bit);
      break;
    case CStarModeGroup::Atm:
      m.atm = static_cast<std::uint16_t>(m.atm | bit);
      break;
    }
  }
  return m;
}

// ── Calendar helpers (ats_vol_cstar_arb.c) ─────────────────────────────────

struct PairViolation {
  double dw_min{kInf};
  double k_at_min{};
};

[[nodiscard]] PairViolation pair_min_dw(const CStarParams& prev,
                                        const CStarParams& curr,
                                        std::span<const double> z_grid) noexcept {
  PairViolation out;
  if (!(prev.theta > 0.0) || !(curr.theta > 0.0)) {
    return out;
  }
  const double sqt_prev = std::sqrt(prev.theta);
  for (const double z : z_grid) {
    const double k = z * sqt_prev;
    const double w_prev = cstar_slice_w(prev, k);
    const double w_curr = cstar_slice_w(curr, k);
    if (!std::isfinite(w_prev) || !std::isfinite(w_curr)) {
      continue;
    }
    const double dw = w_curr - w_prev;
    if (dw < out.dw_min) {
      out.dw_min = dw;
      out.k_at_min = k;
    }
  }
  return out;
}

// Largest damping in [0, 1] on curr's group modes keeping w_curr ≥ w_prev at
// the violation point k_viol (bisection).
void bisect_modal_damping_for_calendar(CStarParams& curr,
                                       const CStarParams& prev,
                                       std::uint16_t group_mask,
                                       double k_viol) noexcept {
  std::array<double, kCStarNModes> saved = curr.beta;
  double lo = 0.0;
  double hi = 1.0;
  for (int it = 0; it < 30; ++it) {
    const double mid = 0.5 * (lo + hi);
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      if (mode_active(group_mask, j)) {
        curr.beta[j] = saved[j] * mid;
      }
    }
    const double w_prev = cstar_slice_w(prev, k_viol);
    const double w_curr = cstar_slice_w(curr, k_viol);
    if (w_curr >= w_prev) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    if (mode_active(group_mask, j)) {
      curr.beta[j] = saved[j] * lo;
    }
  }
}

// Smallest theta multiplier r ∈ [1, max_bump] with r-scaled curr satisfying
// w_curr(k_viol) ≥ w_prev(k_viol) (bumping theta also moves the z-coordinate).
void bisect_theta_bump_for_calendar(CStarParams& curr, const CStarParams& prev,
                                    double k_viol, double max_bump) noexcept {
  const double theta_orig = curr.theta;
  if (max_bump <= 1.0) {
    max_bump = kCalDefaultThetaBump;
  }
  curr.theta = theta_orig * max_bump;
  const double w_prev = cstar_slice_w(prev, k_viol);
  const double w_at_max = cstar_slice_w(curr, k_viol);
  if (!(w_at_max >= w_prev)) {
    return;  // cap binds; leave at max_bump
  }
  double lo = 1.0;
  double hi = max_bump;
  for (int it = 0; it < 30; ++it) {
    const double mid = 0.5 * (lo + hi);
    curr.theta = theta_orig * mid;
    if (cstar_slice_w(curr, k_viol) >= w_prev) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  curr.theta = theta_orig * hi;
}

// Repair one (prev, curr) calendar violation: modal damping in priority order,
// then a theta bump, then a final no-arb projection.
void repair_calendar_violation(CStarParams& curr, const CStarParams& prev,
                               double k_viol, double max_theta_bump) {
  const double w_prev = cstar_slice_w(prev, k_viol);
  double w_curr = cstar_slice_w(curr, k_viol);
  if (!std::isfinite(w_prev) || !std::isfinite(w_curr)) {
    return;
  }
  if (w_curr >= w_prev) {
    return;
  }

  const GroupMasks m = group_masks(curr.active_modes);
  const std::array<std::uint16_t, 4> order = {m.far, m.shoulder, m.near_wing,
                                              m.atm};
  for (const std::uint16_t mask : order) {
    if (mask == 0) {
      continue;
    }
    bisect_modal_damping_for_calendar(curr, prev, mask, k_viol);
    w_curr = cstar_slice_w(curr, k_viol);
    if (w_curr >= w_prev) {
      return;
    }
  }

  bisect_theta_bump_for_calendar(curr, prev, k_viol, max_theta_bump);
  (void)cstar_arb_project(curr);  // theta change can re-introduce butterfly arb
}

// Build the shared z-grid used by the calendar sweep. Returns the number of
// grid points written (clamped to [1, kCalGridN]).
[[nodiscard]] std::size_t build_calendar_grid(
    std::uint32_t n_grid, std::array<double, kCalGridN>& z_grid) noexcept {
  if (n_grid == 0u) {
    n_grid = static_cast<std::uint32_t>(kCalGridN);
  }
  if (n_grid > static_cast<std::uint32_t>(kCalGridN)) {
    n_grid = static_cast<std::uint32_t>(kCalGridN);
  }
  const auto ng = static_cast<std::size_t>(n_grid);
  if (ng == 1u) {
    z_grid[0] = kCalZLo;
    return 1u;
  }
  for (std::size_t g = 0; g < ng; ++g) {
    z_grid[g] = kCalZLo + (kCalZHi - kCalZLo) * static_cast<double>(g) /
                              static_cast<double>(ng - 1u);
  }
  return ng;
}

}  // namespace

// ── Modal basis ────────────────────────────────────────────────────────────

double cstar_basis_center(int j) noexcept {
  if (j < 0 || j >= static_cast<int>(kCStarNModes)) {
    return 0.0;
  }
  return kCStarCenters[static_cast<std::size_t>(j)];
}

double cstar_basis(int j, double z) noexcept {
  if (j < 0 || j >= static_cast<int>(kCStarNModes)) {
    return 0.0;
  }
  const double u = (z - kCStarCenters[static_cast<std::size_t>(j)]) / kCStarBasisH;
  if (u < -1.0 || u > 1.0) {
    return 0.0;
  }
  const double v = 1.0 - u * u;
  return v * v * v;  // (1 - u²)³ — C2 at the boundary
}

// ── Base shape ─────────────────────────────────────────────────────────────

double cstar_base(double z, double s2, double c2, double C_left,
                  double C_right) noexcept {
  // ATM polynomial: 1 + 2·s2·z + c2·z² (skew = ½·f', literature convention).
  const double atm = 1.0 + 2.0 * s2 * z + c2 * z * z;

  const double w_atm = atm_window(z);
  const double w_R = right_wing_window(z);
  const double w_L = left_wing_window(z);

  const double right_line = z * C_right + 1.0;  // anchors at z=1 to ~atm(1)
  const double left_line = -z * C_left + 1.0;   // anchors at z=-1

  return w_atm * atm + w_R * right_line + w_L * left_line;
}

// ── Slice evaluators ───────────────────────────────────────────────────────

double cstar_slice_w(const CStarParams& s, double k_log) noexcept {
  if (!(s.theta > 0.0)) {
    return 1.0e-12;
  }
  const double sqrt_theta = std::sqrt(s.theta);
  const double z = k_log / sqrt_theta;

  double f = cstar_base(z, s.s2, s.c2, s.C_left, s.C_right);
  if (s.active_modes != 0u) {
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      if (mode_active(s.active_modes, j)) {
        f += s.beta[j] * cstar_basis(static_cast<int>(j), z);
      }
    }
  }

  double w = s.theta * f;
  if (!std::isfinite(w) || w <= 1.0e-12) {
    w = 1.0e-12;
  }
  return w;
}

double cstar_slice_iv(const CStarParams& s, double k_log) noexcept {
  if (!(s.T > 0.0)) {
    return kNaN;
  }
  const double w = cstar_slice_w(s, k_log);
  if (!(w > 0.0)) {
    return kNaN;
  }
  return std::sqrt(w / s.T);
}

CStarWDerivs cstar_slice_w_derivs(const CStarParams& s, double k_log) noexcept {
  if (!(s.theta > 0.0)) {
    return {kNaN, kNaN, kNaN};
  }
  const WDerivs d = w_and_derivs(s, k_log);
  return {d.w, d.wp, d.wpp};
}

std::optional<std::array<double, kCStarNParams>> cstar_slice_grad_w(
    const CStarParams& s, double k_log) noexcept {
  if (!(s.theta > 0.0)) {
    return std::nullopt;
  }
  const WGradFull wg = eval_w_and_grad(s, k_log);
  if (!wg.ok) {
    return std::nullopt;
  }
  return wg.grad;
}

std::optional<CStarWGrad> cstar_slice_w_and_grad(const CStarParams& s,
                                                 double k_log) noexcept {
  const WGradFull wg = eval_w_and_grad(s, k_log);
  if (!wg.ok) {
    return std::nullopt;
  }
  return CStarWGrad{wg.w, wg.grad};
}

// ── Block accessors ────────────────────────────────────────────────────────

int cstar_modal_indices(std::uint16_t active_modes,
                        std::span<int> out) noexcept {
  if (out.size() < kCStarNModes) {
    return 0;
  }
  int n = 0;
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    if (mode_active(active_modes, j)) {
      out[static_cast<std::size_t>(n)] = static_cast<int>(j);
      ++n;
    }
  }
  return n;
}

int cstar_extract_block_grad(std::span<const double> grad_full, CStarBlock block,
                             std::uint16_t active_modes,
                             std::span<double> out) noexcept {
  if (grad_full.size() < kCStarNParams) {
    return 0;
  }
  std::size_t n_out = 0;
  if (block == CStarBlock::Base || block == CStarBlock::Full) {
    for (std::size_t j = 0; j < kCStarNBase; ++j) {
      if (n_out >= out.size()) {
        return static_cast<int>(n_out);
      }
      out[n_out++] = grad_full[j];
    }
  }
  if (block == CStarBlock::Modal || block == CStarBlock::Full) {
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      if (mode_active(active_modes, j)) {
        if (n_out >= out.size()) {
          return static_cast<int>(n_out);
        }
        out[n_out++] = grad_full[kCStarNBase + j];
      }
    }
  }
  return static_cast<int>(n_out);
}

void cstar_apply_block_step(CStarParams& s, CStarBlock block,
                            std::span<const double> dx) noexcept {
  std::size_t p = 0;
  if (block == CStarBlock::Base || block == CStarBlock::Full) {
    if (dx.size() < kCStarNBase) {
      return;
    }
    s.theta += dx[p++];
    s.s2 += dx[p++];
    s.c2 += dx[p++];
    s.C_left += dx[p++];
    s.C_right += dx[p++];
    if (!(s.theta > 1.0e-6)) {
      s.theta = 1.0e-6;
    }
    if (!(s.C_left > 1.0e-6)) {
      s.C_left = 1.0e-6;
    }
    if (!(s.C_right > 1.0e-6)) {
      s.C_right = 1.0e-6;
    }
  }
  if (block == CStarBlock::Modal || block == CStarBlock::Full) {
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      if (mode_active(s.active_modes, j)) {
        if (p >= dx.size()) {
          return;
        }
        s.beta[j] += dx[p++];
      }
    }
  }
}

int cstar_block_dim(CStarBlock block, std::uint16_t active_modes) noexcept {
  int n_modes = 0;
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    if (mode_active(active_modes, j)) {
      ++n_modes;
    }
  }
  if (block == CStarBlock::Base) {
    return static_cast<int>(kCStarNBase);
  }
  if (block == CStarBlock::Modal) {
    return n_modes;
  }
  return static_cast<int>(kCStarNBase) + n_modes;  // Full
}

// ── No-arb projection ──────────────────────────────────────────────────────

bool cstar_shape_valid(const CStarParams& s) noexcept {
  if (!(s.theta > 0.0)) {
    return false;
  }
  // Raw-shape validity is a separate predicate from the public variance floor:
  // cstar_slice_w() clamps to 1e-12 for safe evaluation, which would mask a
  // model that produces a genuinely non-positive raw variance θ·f(z). Scan the
  // un-floored raw variance across the no-arb grid.
  for (int i = 0; i < kArbGridN; ++i) {
    const double z = kArbZLo + (kArbZHi - kArbZLo) * static_cast<double>(i) /
                                   static_cast<double>(kArbGridN - 1);
    const double w_raw = s.theta * f_and_derivs_z(s, z).f;
    if (!std::isfinite(w_raw) || !(w_raw > 0.0)) {
      return false;
    }
  }
  return true;
}

double cstar_min_roper_g(const CStarParams& s) noexcept {
  if (!(s.theta > 0.0)) {
    return -kInf;
  }
  // Table-driven BLAS-1 sweep (SPRINT W5.1): the constant window/basis columns
  // are precomputed once, so the ~100–160 sweeps a projection performs no longer
  // recompute the modal basis and smooth-step windows at every grid point.
  return roper_min_g_tab(s);
}

Status cstar_arb_project(CStarParams& s) {
  if (!(s.theta > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "cstar_arb_project: theta must be > 0");
  }

  if (roper_arb_free(s)) {
    s.arb_damping = 1.0;
    return Ok();
  }

  std::array<double, kCStarNModes> original = s.beta;
  const GroupMasks m = group_masks(s.active_modes);
  double damping_min = 1.0;

  // Damp far-wing → shoulder → near-wing → ATM, in that order.
  const std::array<std::uint16_t, 4> order = {m.far, m.shoulder, m.near_wing,
                                              m.atm};
  for (const std::uint16_t mask : order) {
    if (mask == 0) {
      continue;
    }
    const double d = bisect_group_damping(s, mask, original);
    if (d < damping_min) {
      damping_min = d;
    }
    // Fold this group's post-damping betas into the snapshot for the next
    // group (mirrors the C's progressive `original[j] = s->beta[j]`).
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      if (mode_active(mask, j)) {
        original[j] = s.beta[j];
      }
    }
    if (roper_arb_free(s)) {
      s.arb_damping = damping_min;
      return Ok();
    }
  }

  // All modes damped and still violating: reduce base curvature toward 0.
  //
  // BUG FIX (REVIEW §6.1 #11): the prior bisection labelled `c2_hi` (= original
  // c2) as the search's upper bound and updated the WRONG bracket end — an
  // infeasible midpoint advanced the feasible-labelled bound, so when the first
  // midpoint was infeasible BOTH bounds stayed infeasible and it returned the
  // arb-violating original c2 with Ok(). Fix: track the FEASIBLE endpoint
  // explicitly (c2 = 0 is the low-curvature end, most likely arb-free; the
  // original c2 is known infeasible here) and return it, so a feasible curvature
  // is never discarded. Correctness, not a numeric refactor.
  const double c2_orig = s.c2;
  double c2_feasible = 0.0;
  double c2_infeasible = c2_orig;
  s.c2 = c2_feasible;
  if (roper_arb_free(s)) {
    for (int it = 0; it < 40; ++it) {
      const double mid = 0.5 * (c2_feasible + c2_infeasible);
      s.c2 = mid;
      if (roper_arb_free(s)) {
        c2_feasible = mid;  // push the feasible endpoint toward the original
      } else {
        c2_infeasible = mid;
      }
    }
    s.c2 = c2_feasible;
  } else {
    s.c2 = 0.0;  // even zero curvature is infeasible; keep the best candidate
  }
  s.arb_damping = 0.0;

  // Post-projection no-arbitrage validation — two predicates, two error codes.
  // Projection is NOT guaranteed to reach feasibility (skew / wings can violate
  // independently of the modal/curvature levers we control), so propagate the
  // failure instead of the prior unconditional Ok(). Raw-shape invalidity (the
  // model produces a non-positive raw variance) is distinct from a residual
  // butterfly violation with positive variance.
  if (!cstar_shape_valid(s)) {
    return Err(ErrorCode::OutOfRange,
               "cstar_arb_project: raw shape variance non-positive after "
               "projection");
  }
  if (!roper_arb_free(s)) {
    return Err(ErrorCode::Unavailable,
               "cstar_arb_project: butterfly arbitrage persists after "
               "projection");
  }
  return Ok();
}

// ── Calendar projection ────────────────────────────────────────────────────

double cstar_calendar_min_dw(std::span<const CStarParams> slices,
                             std::uint32_t n_grid) noexcept {
  if (slices.size() < 2) {
    return kInf;
  }
  std::array<double, kCalGridN> z_grid{};
  const std::size_t ng = build_calendar_grid(n_grid, z_grid);
  const std::span<const double> grid{z_grid.data(), ng};

  double dw_min = kInf;
  for (std::size_t i = 1; i < slices.size(); ++i) {
    const PairViolation pv = pair_min_dw(slices[i - 1], slices[i], grid);
    if (pv.dw_min < dw_min) {
      dw_min = pv.dw_min;
    }
  }
  return dw_min;
}

Status cstar_project_calendar(std::span<CStarParams> slices,
                              std::uint32_t n_grid, double max_theta_bump) {
  if (slices.size() < 2) {
    return Ok();
  }
  if (max_theta_bump <= 1.0) {
    max_theta_bump = kCalDefaultThetaBump;
  }
  std::array<double, kCalGridN> z_grid{};
  const std::size_t ng = build_calendar_grid(n_grid, z_grid);
  const std::span<const double> grid{z_grid.data(), ng};

  for (int pass = 0; pass < kCalMaxPasses; ++pass) {
    bool any_violation = false;
    for (std::size_t i = 1; i < slices.size(); ++i) {
      const PairViolation pv = pair_min_dw(slices[i - 1], slices[i], grid);
      if (std::isfinite(pv.dw_min) && pv.dw_min < -1.0e-12) {
        any_violation = true;
        repair_calendar_violation(slices[i], slices[i - 1], pv.k_at_min,
                                  max_theta_bump);
      }
    }
    if (!any_violation) {
      return Ok();
    }
  }

  const double dw_final = cstar_calendar_min_dw(
      std::span<const CStarParams>{slices.data(), slices.size()}, n_grid);
  if (std::isfinite(dw_final) && dw_final < -1.0e-9) {
    return Err(ErrorCode::Unavailable,
               "cstar_project_calendar: calendar arbitrage persists after cap");
  }
  return Ok();
}

// ── CStarSurface ───────────────────────────────────────────────────────────

Result<CStarSurface> CStarSurface::create(std::uint32_t uid,
                                          std::size_t cap_slices) {
  if (cap_slices == 0) {
    return Err(ErrorCode::InvalidArgument,
               "CStarSurface::create: cap_slices must be > 0");
  }
  CStarSurface surf;
  surf.uid_ = uid;
  surf.cap_slices_ = cap_slices;
  surf.slices_.reserve(cap_slices);
  return Ok(std::move(surf));
}

Status CStarSurface::set_slice(std::size_t idx, const CStarParams& slice) {
  if (idx >= cap_slices_) {
    return Err(ErrorCode::OutOfRange,
               "CStarSurface::set_slice: idx exceeds capacity");
  }
  if (idx >= slices_.size()) {
    slices_.resize(idx + 1);
  }
  slices_[idx] = slice;
  return Ok();
}

double CStarSurface::w(double k_log, double T) const noexcept {
  const std::size_t n = slices_.size();
  if (n == 0) {
    return kNaN;
  }
  if (T < kTMinEval) {
    T = kTMinEval;
  }

  const double T0 = slices_[0].T;
  if (T <= T0) {
    if (T < 0.5 * T0) {
      return kNaN;  // Sprint-26 short-T extrapolation guard
    }
    return cstar_slice_w(slices_[0], k_log);
  }
  const double T_last = slices_[n - 1].T;
  if (T == T_last) {
    return cstar_slice_w(slices_[n - 1], k_log);
  }
  if (T > T_last) {
    return kNaN;  // extrapolation past the longest slice is never allowed
  }

  std::size_t lo = 0;
  std::size_t hi = n - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (slices_[mid].T <= T) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double T_lo = slices_[lo].T;
  const double T_hi = slices_[hi].T;
  const double w_lo = cstar_slice_w(slices_[lo], k_log);
  const double w_hi = cstar_slice_w(slices_[hi], k_log);
  const double alpha = (T - T_lo) / (T_hi - T_lo);
  return w_lo + alpha * (w_hi - w_lo);
}

double CStarSurface::iv(double k_log, double T) const noexcept {
  const double wv = w(k_log, T);
  if (!std::isfinite(wv) || wv <= 0.0) {
    return kNaN;
  }
  return std::sqrt(wv / T);
}

}  // namespace atx::vol
