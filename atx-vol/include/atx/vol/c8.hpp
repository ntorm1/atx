#pragma once

// C8 parametric volatility family — evaluator, compact-support bump basis, the
// SVI Jump-Wings <-> raw-SVI reparametrization, the eSSVI warm-start seed, and
// the Roper no-arbitrage projection. (The standalone per-family container that
// stacked C8 slices by ascending T was demoted to
// `detail/legacy_c8_surface.hpp` by S4-T21 / plan 4.4 — the canonical pipeline
// is CurveSurface -> PricedSurface / PricedSurfaceView -> SurfaceSet.)
//
// Ported from the C `ats-vol` library (ats_vol_c8.{h,c}, ats_vol_c8_basis.c,
// ats_vol_c8_jw.c, ats_vol_c8_arb.c, ats_vol_c8_internal.h). The C8 slice is an
// SVI-JW backbone (5 params: v, psi, p, c, v_min) plus three additive
// compact-support bumps (kappa: ATM curvature; q_L / q_R: asymmetric deep-wing
// curvature) — 8 free parameters per slice. Relative to SVI / eSSVI it closes
// two structural shape gaps:
//
//   - kappa < 0 admits NEGATIVE ATM curvature (FOMC / earnings / SPX-VIX),
//     which eSSVI's (theta, phi, rho) form cannot represent;
//   - (q_L, q_R) decouple per-side wing curvature (asymmetric tails).
//
//   w(k) = raw_SVI( JW->raw(v, psi, p, c, v_min) )(k)
//        + kappa * B_atm(k; h_atm)
//        + q_L   * B_left(k; k_L, h_L)
//        + q_R   * B_right(k; k_R, h_R)
//
// where k = log(K/F) is log-moneyness and w = sigma^2 * T is total variance.
//
// The refactor to the atx house style (.agents/cpp/agent.md) routes the C's
// negative-integer status returns through `std::optional` (domain-inadmissible
// inputs are an expected, allocation-free normal-flow failure — §4) and the
// container factory through `Result<T>`. Numeric constants (basis shapes, the
// 1/7 level-preserving knot, window placements, arb grid) are ported EXACTLY.
//
// Thread-safety: every entry here is a pure function of its arguments (no
// globals, no allocation) — safe to call concurrently. `c8_arb_project` is the
// one mutator, and it mutates only the caller's own slice.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/types.hpp"        // Result, Status, kTMinEval
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_backbone_w (seed input)

namespace atx::vol {

// ── Per-slice C8 parameters (ports the C `AtsVolSliceC8`) ────────────────
//
// Aggregate; trivially copyable; every member value-initialized to a BENIGN
// slice — the defaults form an admissible JW backbone (v > v_min > 0, symmetric
// unit wings) with the window scales consistent with sqrt(v) = 0.2 and the
// bumps zeroed, so a default-constructed slice evaluates to a finite positive
// total variance everywhere.
struct C8Params {
  // Slice context (placement on the surface time axis).
  double T{0.25};
  double F{100.0};
  std::int64_t expiry_ns{0};
  std::uint16_t expiry_id{0};

  // SVI-JW backbone (5).
  double v{0.04};       // ATM total variance,  > 0   (= sigma_atm^2 * T)
  double psi{-0.01};    // ATM skew (d w / d k at k = 0), free
  double p{0.4};        // left-wing asymptotic slope, >= 0
  double c{0.4};        // right-wing asymptotic slope, >= 0
  double v_min{0.038};  // minimum total variance,  0 <= v_min <= v

  // Bump coefficients (3).
  double kappa{0.0};  // ATM curvature DoF (sign-free)
  double q_L{0.0};    // deep-left wing curvature DoF
  double q_R{0.0};    // deep-right wing curvature DoF

  // Bump window scales (derived from sigma_atm*sqrt(T) at calibration time;
  // NOT free parameters, but stored per slice so eval needs no curveset).
  double h_atm{0.2};  // half-width of B_atm support   (~ 1.0*sigma_atm*sqrt(T))
  double k_L{-0.5};   // center of B_left bump         (~ -2.5*sigma_atm*sqrt(T))
  double h_L{0.2};    // half-width of B_left support  (~ 1.0*sigma_atm*sqrt(T))
  double k_R{0.5};    // center of B_right bump        (~ +2.5*sigma_atm*sqrt(T))
  double h_R{0.2};    // half-width of B_right support (~ 1.0*sigma_atm*sqrt(T))

  // Per-slice diagnostics (logged by the calibrator).
  double arb_damping_factor{1.0};  // 1.0 = no damping, 0 = bumps fully reverted
  double rmse_price{0.0};
  double rmse_vol{0.0};
  int n_lm_iters{0};
  int n_irls_iters{0};
  bool bumps_active{true};  // false => JW seed kept (C8 didn't improve)
};

// ── Reparam value types (raw SVI 5-tuple / JW backbone 5-tuple) ──────────

// Raw-SVI parameters: w(k) = a + b*(rho*(k-m) + sqrt((k-m)^2 + sigma^2)).
struct C8RawSvi {
  double a{0.0};
  double b{0.0};
  double rho{0.0};
  double m{0.0};
  double sigma{0.0};
};

// SVI Jump-Wings backbone parameters.
struct C8Jw {
  double v{0.0};
  double psi{0.0};
  double p{0.0};
  double c{0.0};
  double v_min{0.0};
};

// ── Compact-support bump basis ────────────────────────────────────────────
//
// All three return 0 outside their support. The atm bump is level-preserving:
// the constant 1/7 makes integral over [-h,h] of B_atm vanish.

// B_atm(k; h) = (1 - u^2)^2 * (u^2 - 1/7),  u = k/h, |u| <= 1; else 0. Even.
[[nodiscard]] double c8_basis_atm(double k, double h_atm) noexcept;

// B_left(k; k_L, h_L) = (1 - u^2)^2 * max(0, -u)^2, u = (k - k_L)/h_L. Zero at
// center; quadratic on the left half (k < k_L).
[[nodiscard]] double c8_basis_left(double k, double k_L, double h_L) noexcept;

// B_right(k; k_R, h_R) = (1 - u^2)^2 * max(0, +u)^2, u = (k - k_R)/h_R. Zero at
// center; quadratic on the right half (k > k_R).
[[nodiscard]] double c8_basis_right(double k, double k_R, double h_R) noexcept;

// ── Raw SVI + JW reparametrization ────────────────────────────────────────

// Raw-SVI total variance at log-moneyness k. Bare evaluator (no domain checks).
[[nodiscard]] double c8_raw_svi_w(double k, const C8RawSvi& raw) noexcept;

// Forward conversion raw SVI -> JW (Gatheral 2006 §3.4; the factor 1/2 on psi
// is the literature convention d w / d k at k = 0). Returns nullopt when the
// raw input is outside the admissible domain (b < 0, sigma <= 0, |rho| >= 1).
[[nodiscard]] std::optional<C8Jw> c8_raw_to_jw(const C8RawSvi& raw,
                                               double T) noexcept;

// JW -> raw (Gatheral & Jacquier 2014 eq. 3.5, with sign-of-alpha and the
// degenerate v == v_min / symmetric-smile handling). `sigma_floor` is the
// minimum sigma used on the degenerate branch. Returns nullopt when the JW
// input is inadmissible (v <= 0, p < 0, c < 0, v_min < 0, v_min > v, p+c <= 0).
[[nodiscard]] std::optional<C8RawSvi> c8_jw_to_raw(const C8Jw& jw, double T,
                                                   double sigma_floor) noexcept;

// Closed-form Jacobian of c8_jw_to_raw: jac[i][j] = d raw_i / d jw_j with raw
// order (a, b, rho, m, sigma) and jw order (v, psi, p, c, v_min). It is the
// EXACT derivative of the algebra c8_jw_to_raw evaluates, differentiated as
// written: an active rho / beta clamp (at +/-(1-1e-9)) or an active sigma floor
// contributes a ZERO partial for the clamped quantity, and the degenerate
// branch (|v-v_min| < 1e-12 or |beta| < 1e-3 or |denom| < 1e-12, where m := 0
// and sigma := sigma_floor) yields identically-zero m- and sigma-rows. The a
// row always carries the full a = v_min - b*sigma*sqrt(1-rho^2) chain (b, rho,
// sigma each functions of jw; the sigma term drops when sigma is floored/const).
// Returns nullopt on exactly the inadmissible JW inputs that make c8_jw_to_raw
// return nullopt (so a slice's gradient availability tracks its conversion).
[[nodiscard]] std::optional<std::array<std::array<double, 5>, 5>>
c8_jw_to_raw_jac(const C8Jw& jw, double T, double sigma_floor) noexcept;

// ── Slice evaluator + 8-parameter gradient ────────────────────────────────

// Total variance w(k) of the C8 slice at log-moneyness k. Falls back to the
// stored v (floored) when JW->raw rejects or produces a non-finite backbone,
// then floors the result at 1e-12 so downstream IV inversion stays finite.
[[nodiscard]] double c8_slice_w(const C8Params& s, double k_log) noexcept;

// Overload taking a PRECOMPUTED JW->raw conversion (nullopt = conversion
// failed). The JW->raw step is strike-invariant, so a caller sweeping many k at
// one parameter point converts once and reuses `raw_conv` here; identical ops to
// the single-argument form. `raw_conv` must be c8_jw_to_raw(jw(s), s.T, 1e-4).
[[nodiscard]] double c8_slice_w(
    const C8Params& s, double k_log,
    const std::optional<C8RawSvi>& raw_conv) noexcept;

// Partial derivatives of c8_slice_w w.r.t the 8 parameters in canonical order
// {v, psi, p, c, v_min, kappa, q_L, q_R}. The raw-SVI -> bumps chain and the
// JW -> raw step are both closed-form (see c8_jw_to_raw_jac). Returns nullopt on
// a JW-domain failure (matches the C's non-zero rc).
[[nodiscard]] std::optional<std::array<double, 8>> c8_slice_grad_w(
    const C8Params& s, double k_log) noexcept;

// Overload taking the PRECOMPUTED strike-invariant conversion + Jacobian
// (nullopt = conversion/Jacobian unavailable -> nullopt gradient). Lets the
// calibrator convert + build the 5x5 Jacobian ONCE per parameter point and reuse
// them across every observation. `raw_conv`/`jac_conv` must be
// c8_jw_to_raw / c8_jw_to_raw_jac of jw(s) at (s.T, 1e-4).
[[nodiscard]] std::optional<std::array<double, 8>> c8_slice_grad_w(
    const C8Params& s, double k_log, const std::optional<C8RawSvi>& raw_conv,
    const std::optional<std::array<std::array<double, 5>, 5>>&
        jac_conv) noexcept;

// ── eSSVI warm-start seed ─────────────────────────────────────────────────

// Seed a C8 slice from an eSSVI slice: sample the eSSVI backbone at inner /
// far-wing knots, fit the JW backbone closed-form (asymptotic wing slopes from
// the far pair), zero the bumps, and place the window scales at sigma_atm*sqrt(T).
// Returns nullopt when the eSSVI theta <= 0.
[[nodiscard]] std::optional<C8Params> c8_seed_from_essvi(
    const EssviParams& src) noexcept;

// ── No-arbitrage (Roper density) projection ───────────────────────────────

// Minimum of Roper's g(k) over a fixed 200-point grid spanning the bump
// supports. g(k) = (1 - k*w'/(2w))^2 - w'^2/4*(1/w + 1/4) + w''/2, with w', w''
// by central finite differences. Returns -inf if w(k) <= 0 anywhere on the grid.
[[nodiscard]] double c8_min_roper_g(const C8Params& s) noexcept;

// Damp the bumps until the slice is butterfly-arb-free. Each of (kappa, q_L,
// q_R) is independently bisected back toward its fitted value while min g >= 0;
// the smallest surviving fraction is written to `s.arb_damping_factor`. Always
// succeeds (all bumps zero = pure JW backbone is arb-free). Mutates `s`.
void c8_arb_project(C8Params& s) noexcept;

}  // namespace atx::vol
