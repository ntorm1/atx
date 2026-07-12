#pragma once

// Calibration-grade volatility-surface representation for atx-vol.
//
// This is the FULL-FIDELITY per-slice type that downstream calibrators,
// arbitrage validators, and the surface archive all target — the shared
// contract they read from and write to. It is deliberately distinct from
// (and complementary to) `surface.hpp`, which carries only the minimal
// 3-parameter Gatheral-Jacquier evaluator (`SviSlice`/`EssviSlice`,
// `svi_w`/`essvi_w`, `Surface<>`) used on the pure pricing hot path.
//
// Ported from the C `ats-vol` library (ats_vol_svi.c, ats_vol_essvi.c,
// ats_vol_surface.c/.h). Relative to `surface.hpp`, the eSSVI slice here
// additionally carries the Sprint-15 asymmetric-rho blend (`rho_R`/
// `rho_scale`), the Sprint-11/12 wing residual (`resid_coef`/`resid_scale`/
// `resid_basis_kind`), and the Mingone cube reparametrization coordinates
// (`psi`/`p`/`lambda`/`lambda_R`) — the calibration-adjacent extensions that
// `surface.hpp` intentionally omits. The closed-form evaluators are:
//
//   Backbone eSSVI (base Gatheral-Jacquier form):
//     w(k) = (theta/2) * (1 + rho*phi*k + sqrt((phi*k + rho)^2 + (1 - rho^2)))
//   with rho replaced by an asymmetric left/right blend rho_eff(k) when
//   rho_scale > 0.
//
//   Wing residual (added to the backbone when resid_scale > 0), HINGE_QUAD
//   basis: a symmetric pair of clamped-hinge + hinge-squared terms outside a
//   dead band, scaled by resid_coef[].
//
//   Raw SVI (Gatheral form):
//     w(k) = a + b * (rho*(k - m) + sqrt((k - m)^2 + sigma^2))
//
// where k = log(K/F) is log-moneyness and w = sigma^2 * T is total variance.
//
// ── Naming (ODR) note ────────────────────────────────────────────────────
// To avoid one-definition-rule collisions with `surface.hpp` (same
// namespace, translation units may include both), every symbol here is
// distinct: `EssviParams`/`SviParams` (full slices), `essvi_backbone_w`/
// `essvi_residual_w`/`essvi_total_w`/`svi_total_w`/`essvi_w_grad3`/
// `essvi_w_grad4`, the reparam helpers, and the `VolSurface` container.
//
// ── Time interpolation ───────────────────────────────────────────────────
// `VolSurface` answers w(k, T) / iv(k, T) by interpolating LINEARLY IN TOTAL
// VARIANCE across the two bracketing slices by T (never in sigma directly),
// with the same Sprint-26 no-extrapolation guards as `Surface<>`: a query
// past the longest slice, or more than 50% below the shortest, returns NaN.
//
// Thread-safety: `VolSurface` is a plain value type with no cross-instance
// shared state. Concurrent reads (w/iv/find_exact_T/iv_on_slice) against one
// instance are safe; the set_slice_* mutators must not run concurrently with
// any other access to the same instance (the C's "many readers OR one
// writer" contract).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/types.hpp"

namespace atx::vol {

// ── Parametrization tag ──────────────────────────────────────────────────
//
// Numeric values line up with the C `AtsVolParam` enum (ESSVI=0, SVI=1,
// WING=2, SVI_MM=3, C8=4, CSTAR16M=5). `Wing` is carried purely so the
// underlying values match the C; `C8`/`CStar16M` are tag-only markers whose
// slice evaluators are not ported (w/iv on those surfaces return NaN).
enum class Parametrization : std::uint8_t {
  Essvi = 0,
  Svi = 1,
  Wing = 2,
  SviMm = 3,
  C8 = 4,
  CStar16M = 5,
};

// ── Wing-residual basis kind ─────────────────────────────────────────────
//
// Selects the basis used by the additive wing residual. `None`, `HingeQuad`,
// and `C2Bspline` are supported exactly by both fit and evaluation. The other
// values remain persisted vocabulary for archive compatibility; calibration
// validation rejects them until their matching fitter/evaluator is implemented.
enum class ResidualBasisKind : std::uint8_t {
  None = 0,
  HingeQuad = 1,
  C2Bspline = 2,
  Chebyshev = 3,
  WingBspline = 4,
  Fengler = 5,
};

// ── Full eSSVI slice (ports the C `AtsVolSliceESSVI`) ────────────────────
//
// The backbone shape lives in (theta, phi, rho); rho_R/rho_scale add the
// asymmetric left/right skew blend; psi/p/lambda/lambda_R are the Mingone
// cube coordinates the optimizer works in; resid_* carry the additive wing
// residual. T/F/expiry_* place the slice on the surface's time axis.
//
// Aggregate; trivially copyable; all members value-initialized.
struct EssviParams {
  double theta{};                   // ATM total variance (> 0)
  double phi{};                     // curvature (> 0)
  double rho{};                     // base / left-wing skew, in (-1, 1)
  double rho_R{};                   // right-wing skew target for the blend
  double rho_scale{};               // blend width; <= 0 disables the blend
  double psi{};                     // Mingone cube: theta level, in [0, 1]
  double p{};                       // Mingone cube: phi fraction, in [0, 1]
  double lambda{};                  // Mingone cube: rho position, in [0, 1]
  double lambda_R{};                // Mingone cube: right-wing rho position
  double T{};                       // year-fraction to expiry (time axis)
  double F{};                       // forward level at expiry
  std::int64_t expiry_ns{};         // expiry timestamp (epoch ns)
  std::uint16_t expiry_id{};        // dense expiry index within the surface
  // Mirrors the C `double resid_coef[16]` (std::array: identical layout,
  // bounds-friendly). Wing-residual basis coefficients.
  std::array<double, 16> resid_coef{};
  double resid_scale{};             // residual log-moneyness scale; <= 0 => off
  ResidualBasisKind resid_basis_kind{ResidualBasisKind::None};
  std::uint8_t resid_n_basis{};     // active basis count (0 => default of 5)
};

// ── Full raw-SVI slice (ports the C `AtsVolSliceSVI`) ────────────────────
//
// w(k) = a + b * (rho*(k - m) + sqrt((k - m)^2 + sigma^2)). T/F/expiry_*
// place the slice on the surface time axis, exactly as the eSSVI slice.
struct SviParams {
  double a{};
  double b{};
  double rho{};
  double m{};
  double sigma{};
  double T{};
  double F{};
  std::int64_t expiry_ns{};
  std::uint16_t expiry_id{};
};

// ── Reparam multi-output value types (no out-pointers) ───────────────────

// Natural eSSVI backbone parameters.
struct EssviNatural {
  double theta{};
  double phi{};
  double rho{};
};

// Mingone cube coordinates (each in [0, 1] on admissible inputs).
struct EssviCube {
  double psi{};
  double p{};
  double lambda{};
};

// ── eSSVI closed-form evaluators ─────────────────────────────────────────

// eSSVI backbone total variance at log-moneyness `k_log` (asymmetric-rho
// blend applied when rho_scale > 0). ~12 FLOPs; no domain checks (bare
// arithmetic evaluator, matches the C). NaN in => NaN out.
[[nodiscard]] double essvi_backbone_w(const EssviParams& slice,
                                      double k_log) noexcept;

// Additive wing residual dw(k) in total-variance units. Returns 0 when
// resid_scale <= 0. HINGE_QUAD basis; see the PORT NOTE in the source for
// the non-hinge fallback.
[[nodiscard]] double essvi_residual_w(const EssviParams& slice,
                                      double k_log) noexcept;

// Backbone + residual with a hot-path positivity net (w floored to 1e-12 if
// the residual drives it non-positive). Equals the backbone exactly when
// resid_scale <= 0. Propagates a non-finite backbone unchanged.
[[nodiscard]] double essvi_total_w(const EssviParams& slice,
                                   double k_log) noexcept;

// Gradient of the BACKBONE total variance w.r.t. (theta, phi, rho) at fixed
// k_log — the closed form the IRLS/Newton calibrator consumes. Returned as
// {dtheta, dphi, drho}.
[[nodiscard]] std::array<double, 3> essvi_w_grad3(const EssviParams& slice,
                                                  double k_log) noexcept;

// Gradient of the BACKBONE w.r.t. (theta, phi, rho_L, rho_R) — the chain
// rule through the asymmetric-rho blend. Returned as {dtheta, dphi, drhoL,
// drhoR}; drhoR == 0 in symmetric mode (rho_scale <= 0), collapsing to
// essvi_w_grad3 on the first three components.
[[nodiscard]] std::array<double, 4> essvi_w_grad4(const EssviParams& slice,
                                                  double k_log) noexcept;

// ── Raw-SVI closed-form evaluator ────────────────────────────────────────

// Raw-SVI total variance at log-moneyness `k_log`. Closed-form, ~5 FLOPs, no
// domain checks (matches the C bare evaluator).
[[nodiscard]] double svi_total_w(const SviParams& slice, double k_log) noexcept;

// ── Mingone cube reparametrization (natural <-> cube) ────────────────────
//
// The optimizer works in the unit cube (psi, p, lambda) to keep the eSSVI
// no-arbitrage box a simple [0,1]^3. These map to/from the natural backbone
// parameters. `T` sets the theta scaling band; T <= 0 defaults to 1/365.25.

// Cube -> natural. Inputs are clamped to [0, 1].
[[nodiscard]] EssviNatural essvi_reparam_to_natural(double psi, double p,
                                                    double lambda,
                                                    double T) noexcept;

// Natural -> cube (inverse of essvi_reparam_to_natural on admissible inputs).
[[nodiscard]] EssviCube essvi_natural_to_reparam(double theta, double phi,
                                                 double rho, double T) noexcept;

// Gatheral-Jacquier butterfly bound on phi given (theta, rho): the largest
// phi keeping the slice free of butterfly arbitrage. Returns 0 for a
// degenerate theta <= 0 or |rho| >= 1.
[[nodiscard]] double essvi_phi_max(double theta, double rho) noexcept;

// Scalar rho <-> lambda maps (the rho coordinate of the cube). Both clamp.
[[nodiscard]] double essvi_rho_from_lambda(double lambda) noexcept;
[[nodiscard]] double essvi_lambda_from_rho(double rho) noexcept;

// ── VolSurface: full-fidelity fitted-surface container ───────────────────
//
// Holds one parametrization's slices (only the matching vector is
// populated), sorted by ascending T, plus fit provenance and calibration
// diagnostics. Answers w/iv by linear-in-total-variance time interpolation
// with the Sprint-26 no-extrapolation guards.
//
// Precondition (documented, not verified — matches the C): slices are
// written in ascending-T order.
class VolSurface {
 public:
  // Fit-quality summary the calibrator stamps onto the surface.
  struct Diagnostics {
    double rmse_vol{};
    double max_residual_vol{};
    std::uint32_t n_quotes_used{};
    std::uint32_t n_quotes_dropped{};
  };

  // Construct an empty surface for `param` with capacity for `cap_slices`
  // slices (the matching slice vector is reserved). Returns InvalidArgument
  // if cap_slices == 0.
  [[nodiscard]] static Result<VolSurface> create(std::uint32_t uid,
                                                 Parametrization param,
                                                 std::size_t cap_slices);

  // Write the eSSVI slice at `idx`, growing the active count to idx+1 if it
  // is at/past the current high-water mark. OutOfRange if idx >= capacity();
  // InvalidArgument if this surface is not eSSVI-parametrized.
  [[nodiscard]] Status set_slice_essvi(std::size_t idx,
                                       const EssviParams& slice);

  // As above for a raw-SVI (or SVI-MM) slice. InvalidArgument unless the
  // surface is Svi/SviMm-parametrized.
  [[nodiscard]] Status set_slice_svi(std::size_t idx, const SviParams& slice);

  [[nodiscard]] Parametrization param() const noexcept { return param_; }
  [[nodiscard]] std::uint32_t uid() const noexcept { return uid_; }
  [[nodiscard]] std::int64_t fit_ts_ns() const noexcept { return fit_ts_ns_; }
  void set_fit_ts_ns(std::int64_t ts_ns) noexcept { fit_ts_ns_ = ts_ns; }

  // Active slice count (high-water mark of the matching vector); 0 for the
  // tag-only parametrizations (Wing/C8/CStar16M).
  [[nodiscard]] std::size_t n_slices() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return cap_slices_; }

  [[nodiscard]] std::span<const EssviParams> essvi_slices() const noexcept {
    return essvi_;
  }
  [[nodiscard]] std::span<const SviParams> svi_slices() const noexcept {
    return svi_;
  }

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diag_;
  }
  void set_diagnostics(const Diagnostics& diag) noexcept { diag_ = diag; }

  // Total variance w = sigma^2 * T at (k_log, T), linear-in-w across the two
  // bracketing slices by T. T is floored to kTMinEval for bracketing. NaN
  // when there are no slices, when T exceeds the last slice's T, when T
  // (post-floor) sits more than 50% below the first slice's T, or on a
  // tag-only parametrization.
  [[nodiscard]] double w(double k_log, double T) const noexcept;

  // Implied vol sigma = sqrt(w(k_log, T) / T) — divides by the CALLER's
  // un-floored T (matches the C). NaN wherever w() is NaN / non-positive.
  [[nodiscard]] double iv(double k_log, double T) const noexcept;

  // Index of the slice whose T equals `T_query` within a one-second tick
  // tolerance, or 0xFFFF if none. Tolerance = 1/(252*6.5*60) year-fractions.
  [[nodiscard]] std::uint16_t find_exact_T(double T_query) const noexcept;

  // Implied vol from slice `slice_idx` alone: sqrt(w_slice / T_slice), using
  // the slice's own T. NaN if slice_idx is out of range, T_slice <= 0, or
  // the slice variance is non-finite / non-positive.
  [[nodiscard]] double iv_on_slice(std::uint16_t slice_idx,
                                   double k_log) const noexcept;

 private:
  // Constructed only via create(); Rule of Zero otherwise (movable/copyable).
  VolSurface() = default;

  [[nodiscard]] double eval_slice_w(std::size_t idx, double k_log) const noexcept;
  [[nodiscard]] double slice_T(std::size_t idx) const noexcept;

  Parametrization param_{Parametrization::Essvi};
  std::uint32_t uid_{};
  std::int64_t fit_ts_ns_{};
  std::vector<EssviParams> essvi_{};
  std::vector<SviParams> svi_{};
  Diagnostics diag_{};
  std::size_t cap_slices_{};
};

}  // namespace atx::vol
