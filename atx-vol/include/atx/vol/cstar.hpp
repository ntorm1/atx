#pragma once

// CStar (C16M "modal") parametric volatility family — a self-contained port of
// the C17 `ats-vol` CStar curve (ats_vol_cstar.h/.c, ats_vol_cstar_basis.c,
// ats_vol_cstar_arb.c) into idiomatic C++20 (agent profile .agents/cpp/agent.md).
//
// The "M" suffix is *modal*, not any vendor's suffix — the family plays the
// operational role of a next-generation C-family curve (nested complexity
// tiers C5/C8/C12/C16 + local shape modes + seed-from-eSSVI + ridge-LSQ
// calibration) without claiming formula compatibility with any proprietary
// curve.
//
// ── Shape ────────────────────────────────────────────────────────────────
//
//   z    = k / sqrt(theta)                       normalized log-moneyness
//   w(k) = theta · f(z)
//   f(z) = f_base(z; s2, c2, C_left, C_right)
//        + Σ_j  beta_j · B_j(z)                   over the ACTIVE modes j
//
// Base shape (5 DoF): a smooth interpolant matching an ATM level of 1
// (so w(0) = theta), ATM skew (via s2), ATM curvature (via c2), and linear
// asymptotic wing slopes (C_left, C_right) — a zero-mode candidate is itself
// arb-free and a reasonable smile.
//
// Modal basis (11 DoF): compact-support C2 quintic bumps B_j(z) = (1 - u²)³
// (u = (z - z_j)/h, h = 1) on the fixed non-uniform grid
//
//     z_centers = {-4, -3, -2, -1.25, -0.6, 0, +0.6, +1.25, +2, +3, +4}
//
// Active-mode tiers (a per-slice runtime decision, tagged in `fit_tier`):
//
//   C5  : 0 modes (base only)                 liquid simple smiles
//   C8  : modes {2, 5, 8}                      shoulder + ATM
//   C12 : modes {1, 3, 5, 7, 9}               shoulder + near-wing + ATM
//   C16 : all 11 modes                         SPX/SPY-class chains
//
// Inactive modes carry beta = 0 and contribute nothing; the active set lives
// in `active_modes` (a bitmask over the 11 modes).
//
// ── Thread-safety ──────────────────────────────────────────────────────────
// Every evaluator here is a pure function of `CStarParams` (a value type).
// `CStarSurface` is a plain value type with no cross-instance shared state:
// concurrent reads (w/iv/slices) against one instance are safe; the mutators
// (set_slice, mutable_slices + calendar projection) must not run concurrently
// with any other access (the C's "many readers OR one writer" contract).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/types.hpp"  // Result, Status, Side, ErrorCode

namespace atx::vol {

// ── Layout constants ───────────────────────────────────────────────────────

// Number of modal coefficients (C `ATS_VOL_CSTAR_N_MODES`).
inline constexpr std::size_t kCStarNModes = 11;
// Number of base parameters: theta, s2, c2, C_left, C_right (`ATS_VOL_CSTAR_N_BASE`).
inline constexpr std::size_t kCStarNBase = 5;
// Full parameter-vector dimension (base + modes) = 16.
inline constexpr std::size_t kCStarNParams = kCStarNBase + kCStarNModes;

// Mode half-width in normalized-strike space (`ATS_VOL_CSTAR_BASIS_H`). The
// non-uniform centers plus this support give overlapping C2 partition
// coverage across z ∈ [-5, +5].
inline constexpr double kCStarBasisH = 1.0;

// Seed-fit z-knot count spanning z ∈ [-5, +5] (`ATS_VOL_CSTAR_SEED_GRID_N`).
inline constexpr int kCStarSeedGridN = 41;

// Fixed modal centers (`C_CENTERS`). Symmetric, denser near ATM.
inline constexpr std::array<double, kCStarNModes> kCStarCenters = {
    {-4.0, -3.0, -2.0, -1.25, -0.6, 0.0, 0.6, 1.25, 2.0, 3.0, 4.0}};

// Per-mode Tikhonov ridge (`ats_vol_cstar_ridge_per_mode`): stronger on
// far-wing modes (less data support), weaker near ATM.
inline constexpr std::array<double, kCStarNModes> kCStarRidgePerMode = {
    {1.0e-1, 5.0e-2, 1.0e-2, 5.0e-3, 1.0e-3, 5.0e-4,
     1.0e-3, 5.0e-3, 1.0e-2, 5.0e-2, 1.0e-1}};

// ── Enumerations ───────────────────────────────────────────────────────────

// Active-tier tag (`AtsVolCStarTier`). Stored in `CStarParams::fit_tier`.
enum class CStarTier : std::uint8_t {
  C5 = 0,
  C8 = 1,
  C12 = 2,
  C16 = 3,
};

// Mode-group classification for the no-arb projection order (`AtsVolCStarModeGroup`).
// Lower group = damped first when butterfly-arb violations appear.
enum class CStarModeGroup : std::uint8_t {
  FarWing = 0,   // modes 0, 1, 9, 10
  Shoulder = 1,  // modes 2, 8
  NearWing = 2,  // modes 3, 7
  Atm = 3,       // modes 4, 5, 6
};

// Named sub-blocks of the parameter vector for block-coordinate LM
// (`AtsVolCStarBlock`).
enum class CStarBlock : std::uint8_t {
  Base = 0,   // (theta, s2, c2, C_left, C_right)
  Modal = 1,  // the active beta coefficients
  Full = 2,   // base then active modal
};

// Per-mode group classification (`ats_vol_cstar_mode_group`).
inline constexpr std::array<CStarModeGroup, kCStarNModes> kCStarModeGroup = {
    {CStarModeGroup::FarWing,   // -4
     CStarModeGroup::FarWing,   // -3
     CStarModeGroup::Shoulder,  // -2
     CStarModeGroup::NearWing,  // -1.25
     CStarModeGroup::Atm,       // -0.6
     CStarModeGroup::Atm,       //  0
     CStarModeGroup::Atm,       //  0.6
     CStarModeGroup::NearWing,  //  1.25
     CStarModeGroup::Shoulder,  //  2
     CStarModeGroup::FarWing,   //  3
     CStarModeGroup::FarWing}};  //  4

// Default active-mode bitmask for a tier (`ats_vol_cstar_tier_mask`).
[[nodiscard]] constexpr std::uint16_t cstar_tier_mask(CStarTier tier) noexcept {
  switch (tier) {
  case CStarTier::C5:
    return 0x0000u;
  case CStarTier::C8:  // bits 2, 5, 8
    return static_cast<std::uint16_t>((1u << 2) | (1u << 5) | (1u << 8));
  case CStarTier::C12:  // bits 1, 3, 5, 7, 9
    return static_cast<std::uint16_t>((1u << 1) | (1u << 3) | (1u << 5) |
                                      (1u << 7) | (1u << 9));
  case CStarTier::C16:  // all 11 modes
    return static_cast<std::uint16_t>((1u << 11) - 1u);
  }
  return 0x0000u;  // unreachable for valid enumerators
}

// ── Per-slice CStar parameters (ports `AtsVolSliceCStar16M`) ───────────────
//
// Aggregate; trivially copyable; all members value-initialized (Rule of Zero).
// T/F/expiry_* place the slice on a surface's time axis.
struct CStarParams {
  // Slice context.
  double T{};                 // year-fraction to expiry (time axis)
  double F{};                 // forward level at expiry
  std::int64_t expiry_ns{};   // expiry timestamp (epoch ns)
  std::uint16_t expiry_id{};  // dense expiry index within the surface

  // Base parameters (5).
  double theta{};    // ATM total variance, > 0 (sets z = k/sqrt(theta))
  double s2{};       // normalized vol-squared skew
  double c2{};       // normalized ATM curvature
  double C_left{};   // left asymptotic wing slope (k → −∞)
  double C_right{};  // right asymptotic wing slope (k → +∞)

  // Modal coefficients (up to 11) on the fixed-center compact basis in z.
  std::array<double, kCStarNModes> beta{};

  // Per-slice diagnostics.
  double rmse_price{};             // price-domain RMSE (calibrator)
  double rmse_z{};                 // diagnostic baseline RMSE (calibrator)
  double arb_damping{1.0};         // 1 = no damping, 0 = all modes reverted
  std::uint16_t active_modes{};    // bitmask: bit j set ⇔ mode j is active
  CStarTier fit_tier{CStarTier::C16};
  bool reverted_to_seed{false};    // quality-gate forced-revert flag
};

// ── Modal basis (z-space) ──────────────────────────────────────────────────

// The j'th compact-support C2 mode at z: B_j(z) = (1 - u²)³ for |u| ≤ 1,
// u = (z - z_center[j]) / h, else 0. Out-of-range j returns 0.
[[nodiscard]] double cstar_basis(int j, double z) noexcept;

// The j'th mode center (out-of-range j returns 0).
[[nodiscard]] double cstar_basis_center(int j) noexcept;

// Base shape f_base(z; s2, c2, C_left, C_right): f_base(0) = 1,
// ∂f/∂z|0 = 2·s2, ∂²f/∂z²|0 = 2·c2, linear |z| wings with slopes C_left/C_right.
[[nodiscard]] double cstar_base(double z, double s2, double c2, double C_left,
                                double C_right) noexcept;

// ── Slice evaluators ───────────────────────────────────────────────────────

// Per-slice total variance w = theta·f(z) at log-moneyness k_log. Floors to
// 1e-12 for a non-positive/non-finite result or theta ≤ 0 (matches the C).
[[nodiscard]] double cstar_slice_w(const CStarParams& s, double k_log) noexcept;

// Per-slice implied vol sqrt(w / T). NaN if T ≤ 0 or w non-positive.
[[nodiscard]] double cstar_slice_iv(const CStarParams& s, double k_log) noexcept;

// RAW total variance and its first two log-moneyness derivatives.
struct CStarWDerivs {
  double w{};    // θ·f(z)              (un-floored; may be ≤ 0 if degenerate)
  double wp{};   // ∂w/∂k = √θ·f'(z)
  double wpp{};  // ∂²w/∂k² = f''(z)
};

// Total variance w and its first two k-derivatives (w, w', w'') in closed form
// from the modal shape — the exact w'' used by the butterfly (Roper) no-arb
// gate, replacing the prior central-FD w'' that lost ~8 digits to cancellation.
// The variance is RAW (un-floored): distinct from cstar_slice_w's public 1e-12
// floor, so a degenerate shape reports w ≤ 0 rather than a masked positive.
// All three components NaN when theta ≤ 0.
[[nodiscard]] CStarWDerivs cstar_slice_w_derivs(const CStarParams& s,
                                                double k_log) noexcept;

// Gradient ∂w/∂(theta, s2, c2, C_left, C_right, beta[0..10]) — 16 partials in
// the canonical order. nullopt when theta ≤ 0 (undefined z). theta enters both
// as the multiplicative level and through z = k/√theta; both are accounted for.
[[nodiscard]] std::optional<std::array<double, kCStarNParams>>
cstar_slice_grad_w(const CStarParams& s, double k_log) noexcept;

// Floored total variance w AND the 16-partial gradient in a SINGLE shape
// evaluation — the fused form the LM normal-equations build consumes so each
// observation traverses the modal shape once per iteration instead of twice.
// `w` matches cstar_slice_w; `grad` matches cstar_slice_grad_w. nullopt when
// theta ≤ 0.
struct CStarWGrad {
  double w{};
  std::array<double, kCStarNParams> grad{};
};
[[nodiscard]] std::optional<CStarWGrad> cstar_slice_w_and_grad(
    const CStarParams& s, double k_log) noexcept;

// ── Block accessors (block-coordinate LM support) ──────────────────────────

// Compact `active_modes` into an ascending list of indices written to `out`
// (which must hold ≥ kCStarNModes ints). Returns the number of active modes.
[[nodiscard]] int cstar_modal_indices(std::uint16_t active_modes,
                                      std::span<int> out) noexcept;

// Extract the sub-vector of the full 16-gradient for `block`, compacting only
// active modes (ascending). Writes into `out` (≥ 16 doubles) and returns the
// count. BASE → 5, MODAL → popcount(active), FULL → 5 + popcount(active).
[[nodiscard]] int cstar_extract_block_grad(std::span<const double> grad_full,
                                           CStarBlock block,
                                           std::uint16_t active_modes,
                                           std::span<double> out) noexcept;

// Apply a block step `dx` (length = cstar_block_dim(block, active_modes)) to
// the slice. Clamps theta, C_left, C_right > 1e-6 after a BASE/FULL step.
void cstar_apply_block_step(CStarParams& s, CStarBlock block,
                            std::span<const double> dx) noexcept;

// Parameter-count of `block` given the active mode set.
[[nodiscard]] int cstar_block_dim(CStarBlock block,
                                  std::uint16_t active_modes) noexcept;

// ── No-arb (butterfly) projection ──────────────────────────────────────────

// Raw-shape validity predicate (distinct from the public variance floor): true
// iff theta > 0 and the un-floored raw variance θ·f(z) is finite and strictly
// positive across the no-arb grid. cstar_slice_w() floors to 1e-12 for safe
// evaluation; this predicate answers whether the model itself yields a valid
// (positive) variance without that floor. Used by the projection's
// post-completion validation to distinguish a degenerate shape (raw variance
// ≤ 0) from a residual butterfly violation.
[[nodiscard]] bool cstar_shape_valid(const CStarParams& s) noexcept;

// Minimum Roper g(k) over a dense z-grid (240 knots on z ∈ [-5.5, +5.5]).
// NaN for a null-ish (theta ≤ 0) slice's -infinity signal is folded here.
[[nodiscard]] double cstar_min_roper_g(const CStarParams& s) noexcept;

// Project the slice to no-arb by damping modal coefficients in priority order
// (far-wing → shoulder → near-wing → ATM), then base curvature as a fallback.
// Sets `arb_damping` ∈ [0, 1]. On completion runs a post-projection
// no-arbitrage validation and PROPAGATES failure (it no longer returns Ok
// unconditionally):
//   • InvalidArgument — theta ≤ 0 (precondition).
//   • OutOfRange      — raw shape variance non-positive after projection
//                       (degenerate model; cstar_shape_valid fails).
//   • Unavailable     — butterfly arbitrage persists after projection
//                       (min Roper g < 0 with a positive raw variance).
// On success the slice is butterfly-arb-free on the grid. Not noexcept: the
// error paths construct diagnostic strings.
[[nodiscard]] Status cstar_arb_project(CStarParams& s);

// ── Calendar projection (surface-level, operating on a slice span) ─────────

// Minimum w_curr − w_prev across consecutive slice pairs, sampled on a shared
// z-grid mapped to k via each `prev`'s sqrt(theta). A negative value flags
// calendar arbitrage. +infinity if fewer than 2 slices. `n_grid` is clamped to
// [1, 25]; 0 selects the default (25).
[[nodiscard]] double cstar_calendar_min_dw(std::span<const CStarParams> slices,
                                           std::uint32_t n_grid) noexcept;

// Repair calendar-arb violations across consecutive slice pairs: damp the
// later slice's modes (far-wing → shoulder → near-wing → ATM), then bump its
// theta (capped at `max_theta_bump`, default 1.5 when ≤ 1), re-projecting to
// no-arb. Iterates ≤ 6 passes until idempotent. Ok when clean/resolved;
// Unavailable (maps the C's ERR_ARBITRAGE) if violations persist after the cap.
[[nodiscard]] Status cstar_project_calendar(std::span<CStarParams> slices,
                                            std::uint32_t n_grid,
                                            double max_theta_bump);

// ── Standalone surface container ───────────────────────────────────────────
//
// Holds CStar slices sorted by ascending T. Answers w/iv by linear-in-total-
// variance time interpolation with the same Sprint-26 no-extrapolation guards
// as `VolSurface` (query past the longest slice, or > 50% below the shortest,
// returns NaN). Precondition (documented, not verified — matches the C):
// slices are written in ascending-T order.
class CStarSurface {
 public:
  // Construct an empty surface with capacity for `cap_slices` slices.
  // InvalidArgument if cap_slices == 0.
  [[nodiscard]] static Result<CStarSurface> create(std::uint32_t uid,
                                                   std::size_t cap_slices);

  // Write the slice at `idx`, growing the active count to idx+1 if at/past the
  // high-water mark. OutOfRange if idx >= capacity().
  [[nodiscard]] Status set_slice(std::size_t idx, const CStarParams& slice);

  [[nodiscard]] std::uint32_t uid() const noexcept { return uid_; }
  [[nodiscard]] std::size_t n_slices() const noexcept { return slices_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return cap_slices_; }

  [[nodiscard]] std::span<const CStarParams> slices() const noexcept {
    return slices_;
  }
  // Mutable view (for the calendar projection / in-place repair).
  [[nodiscard]] std::span<CStarParams> mutable_slices() noexcept {
    return slices_;
  }

  // Total variance at (k_log, T), linear-in-w across the two bracketing slices.
  [[nodiscard]] double w(double k_log, double T) const noexcept;
  // Implied vol sqrt(w / T), dividing by the caller's un-floored T.
  [[nodiscard]] double iv(double k_log, double T) const noexcept;

 private:
  CStarSurface() = default;  // constructed only via create(); Rule of Zero

  std::uint32_t uid_{};
  std::vector<CStarParams> slices_{};
  std::size_t cap_slices_{};
};

}  // namespace atx::vol
