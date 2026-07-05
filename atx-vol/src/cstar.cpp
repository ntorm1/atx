#include "atx/vol/cstar.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/error.hpp"

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

// True iff mode j is active in the bitmask. Shift on `unsigned` so the
// signedness-changing implicit conversions of `int >> size_t` are avoided
// (/Wsign-conversion clean).
[[nodiscard]] bool mode_active(std::uint16_t active_modes,
                               std::size_t j) noexcept {
  return ((static_cast<unsigned>(active_modes) >> j) & 1u) != 0u;
}

// w, w', w'' by central finite differences (ats_vol_cstar_arb.c w_and_derivs_).
struct WDerivs {
  double w{};
  double wp{};
  double wpp{};
};

[[nodiscard]] WDerivs w_and_derivs(const CStarParams& s, double k) noexcept {
  constexpr double h = 1.0e-4;
  const double w0 = cstar_slice_w(s, k);
  const double wpl = cstar_slice_w(s, k + h);
  const double wmn = cstar_slice_w(s, k - h);
  return {w0, (wpl - wmn) / (2.0 * h), (wpl - 2.0 * w0 + wmn) / (h * h)};
}

// Roper g(k): butterfly no-arb functional; g ≥ 0 everywhere ⇔ arb-free.
[[nodiscard]] double roper_g_at(const CStarParams& s, double k) noexcept {
  const WDerivs d = w_and_derivs(s, k);
  if (!(d.w > 0.0)) {
    return -kInf;
  }
  const double t = 1.0 - k * d.wp / (2.0 * d.w);
  return t * t - 0.25 * d.wp * d.wp * (1.0 / d.w + 0.25) + 0.5 * d.wpp;
}

// Largest uniform damping factor in [0, 1] on the group's modal coefficients
// keeping min Roper g ≥ 0 (bisection). Non-group betas are held at `saved`;
// group betas are set to original[j] · factor. (ats_vol_cstar_arb.c)
[[nodiscard]] double bisect_group_damping(
    CStarParams& s, std::uint16_t group_mask,
    const std::array<double, kCStarNModes>& original) noexcept {
  std::array<double, kCStarNModes> saved = s.beta;

  if (cstar_min_roper_g(s) >= 0.0) {
    return 1.0;
  }

  double lo = 0.0;
  double hi = 1.0;
  for (int it = 0; it < 30; ++it) {
    const double mid = 0.5 * (lo + hi);
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      s.beta[j] = mode_active(group_mask, j) ? original[j] * mid : saved[j];
    }
    if (cstar_min_roper_g(s) >= 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    s.beta[j] = mode_active(group_mask, j) ? original[j] * lo : saved[j];
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

std::optional<std::array<double, kCStarNParams>> cstar_slice_grad_w(
    const CStarParams& s, double k_log) noexcept {
  if (!(s.theta > 0.0)) {
    return std::nullopt;
  }
  const double sqrt_theta = std::sqrt(s.theta);
  const double z = k_log / sqrt_theta;

  // f(z) and f'(z) by central FD on the full shape (base + active modes).
  constexpr double h_z = 1.0e-4;
  const double f0 = cstar_base(z, s.s2, s.c2, s.C_left, s.C_right);
  const double fp = cstar_base(z + h_z, s.s2, s.c2, s.C_left, s.C_right);
  const double fm = cstar_base(z - h_z, s.s2, s.c2, s.C_left, s.C_right);
  double f_full = f0;
  double fprime = (fp - fm) / (2.0 * h_z);

  if (s.active_modes != 0u) {
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      if (mode_active(s.active_modes, j)) {
        const int jj = static_cast<int>(j);
        const double bz = cstar_basis(jj, z);
        f_full += s.beta[j] * bz;
        const double bp = cstar_basis(jj, z + h_z);
        const double bm = cstar_basis(jj, z - h_z);
        fprime += s.beta[j] * (bp - bm) / (2.0 * h_z);
      }
    }
  }

  std::array<double, kCStarNParams> grad{};
  grad[0] = f_full - 0.5 * z * fprime;                  // ∂w/∂theta

  const double w_atm = atm_window(z);
  grad[1] = s.theta * 2.0 * z * w_atm;                  // ∂w/∂s2
  grad[2] = s.theta * z * z * w_atm;                    // ∂w/∂c2

  const double w_L = left_wing_window(z);
  const double w_R = right_wing_window(z);
  grad[3] = s.theta * w_L * (-z);                       // ∂w/∂C_left
  grad[4] = s.theta * w_R * z;                          // ∂w/∂C_right

  for (std::size_t j = 0; j < kCStarNModes; ++j) {      // ∂w/∂beta_j
    grad[kCStarNBase + j] = s.theta * cstar_basis(static_cast<int>(j), z);
  }
  return grad;
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

double cstar_min_roper_g(const CStarParams& s) noexcept {
  if (!(s.theta > 0.0)) {
    return -kInf;
  }
  const double sqrt_theta = std::sqrt(s.theta);
  const double k_lo = kArbZLo * sqrt_theta;
  const double k_hi = kArbZHi * sqrt_theta;
  double g_min = kInf;
  for (int i = 0; i < kArbGridN; ++i) {
    const double k = k_lo + (k_hi - k_lo) * static_cast<double>(i) /
                                static_cast<double>(kArbGridN - 1);
    const double g = roper_g_at(s, k);
    if (g < g_min) {
      g_min = g;
    }
  }
  return g_min;
}

Status cstar_arb_project(CStarParams& s) {
  if (!(s.theta > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "cstar_arb_project: theta must be > 0");
  }

  if (cstar_min_roper_g(s) >= 0.0) {
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
    if (cstar_min_roper_g(s) >= 0.0) {
      s.arb_damping = damping_min;
      return Ok();
    }
  }

  // All modes damped and still violating: reduce base curvature toward 0.
  double c2_lo = 0.0;
  double c2_hi = s.c2;
  for (int it = 0; it < 30; ++it) {
    s.c2 = 0.5 * (c2_lo + c2_hi);
    if (cstar_min_roper_g(s) >= 0.0) {
      c2_hi = s.c2;
    } else {
      c2_lo = s.c2;
    }
  }
  s.c2 = c2_hi;
  s.arb_damping = 0.0;
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
