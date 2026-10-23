#pragma once

// ── AlSigmaFamily::State — the σ-Chebyshev boundary family's prepared state ──
//
// L3g family-greeks (2026-10-20): PURE CODE MOTION out of american_sigma_family.cpp
// (L2h sigma-family, 2026-10-19) so a SECOND translation unit — american_family_greeks.cpp,
// the analytic Greeks / chain / shock-grid kernel on the same family — can read the
// prepared σ-series. The struct, the file-local σ-axis evaluator (cheb_node /
// bary_eval / axis_of) and the three constants moved here verbatim; `prepare`,
// `price_at` and `admit` stay in the TU, byte for byte (the AlSigmaFamily.* suite and
// the AL fingerprint are the proof). The Greeks TU reaches the family's private
// pointer through sigma_family_detail::FamilyAccess, the one friend american.hpp
// declares for it (the private NAME AlSigmaFamily::State cannot be spelled outside
// the class or its friends, so a nested-class accessor could not have served).
//
// NOT a public API. Not installed. No stability guarantees.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_boundary.hpp" // amer::AlBoundary / AlWorkspace / AlExpiryGeometry::Impl

namespace atx::vol {

namespace sigma_family_detail {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline constexpr std::uint16_t kMaxSigmaNodes = kAlSigmaFamilyMaxNodes;

// TWIN NOTE (L2h brief item 6, option b). cheb_node / bary_eval below are IDENTICAL in
// scheme and in floating-point expression to boundary_interp.cpp's file-local
// cheb_node / bary_eval (the P2.5 σ-axis evaluator), which in turn re-express
// american.cpp's τ-axis al_cheb_node / al_cheb_eval_t<0>. They are duplicated rather
// than factored out so andersen_lake_{put,call}_slice_sigma stays bit-identical by
// construction (no edit to boundary_interp.* in this lane). A change to one twin
// must be mirrored in the other; AlSigmaFamily.NodePricesMatchTheColdSolveAtTheAnchor
// pins this copy's exactness at the nodes.

// σ-axis Chebyshev-Lobatto node i of n, in [-1, 1].
[[nodiscard]] inline double cheb_node(unsigned i, unsigned n) noexcept {
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

// 2nd-kind barycentric Lagrange interpolation of y[] on nodes z[] with weights w[],
// evaluated at zq (exact node hit returns the node value).
[[nodiscard]] inline double bary_eval(const double *z, const double *w, const double *y,
                                      unsigned n, double zq) noexcept {
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

// The family's σ -> axis coordinate (σ or log σ).
[[nodiscard]] inline double axis_of(AlSigmaFamilyAxis axis, double sigma) noexcept {
  return axis == AlSigmaFamilyAxis::LogSigma ? std::log(sigma) : sigma;
}

// L3g's nodal differentiation matrices on the n-node Chebyshev-Lobatto grid above
// (cheb_node, 2nd-kind weights (−1)^s halved at the ends): D1 / D2 map nodal values to
// the nodal first / second derivatives of their interpolant, so b(z)ᵀD1 is the
// derivative of the barycentric basis without the quotient rule's near-node
// cancellation (formulas and references at the definition, american_family_greeks.cpp).
// K1 (r5, 2026-10-22): shared with the (σ, q) family's fused vega, which uses them on
// both of its axes. Rows / columns past n are zero.
struct DiffTables {
  std::array<std::array<double, kMaxSigmaNodes>, kMaxSigmaNodes> d1{};
  std::array<std::array<double, kMaxSigmaNodes>, kMaxSigmaNodes> d2{};
};

// The tables for n in [2, kMaxSigmaNodes], built once per process (thread-safe static
// initialisation); allocation-free and const afterwards. Precondition: 2 <= n <=
// kMaxSigmaNodes.
[[nodiscard]] const DiffTables &diff_tables(unsigned n) noexcept;

} // namespace sigma_family_detail

// Immutable after a successful prepare(); read-only in price(). No mutable scratch:
// the interpolated boundary is a stack AlBoundary per call, so N threads pricing
// through one const family touch only const memory (the race-freedom argument the
// house style asks for in place of a TSan run: no write after prepare(), and
// prepare() / the geometry's prepare() are documented as exclusive).
struct AlSigmaFamily::State {
  const AlExpiryGeometry::Impl *g = nullptr;
  Side side = Side::Call;
  double anchor = 0.0;
  // ── set by prepare() ──
  bool ok = false;
  std::uint64_t generation = 0; // the geometry's generation at prepare()
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double u_lo = 0.0; // the box on the axis coordinate
  double u_hi = 0.0;
  std::uint16_t n_sigma = 0;
  std::uint16_t n_boundary = 0;
  double rate = 0.0;  // the internal put's (rate, yield): (r, q) for puts, (q, r) calls
  double yield = 0.0;
  // The orientation's prepared workspace (const tables + flags the premium
  // quadrature reads: qx/qw_price, n_quad_price, fast, specialize). Owned by the
  // geometry, immutable until its next prepare(), which `generation` detects.
  const amer::AlWorkspace *ws = nullptr;
  amer::AlBoundary grid{}; // node structure (n, T, z, wbary, x, tau); y/K/xmax per price
  double sz[sigma_family_detail::kMaxSigmaNodes] = {}; // σ Chebyshev-Lobatto nodes in [-1, 1]
  double sw[sigma_family_detail::kMaxSigmaNodes] = {}; // 2nd-kind barycentric weights (endpoints halved)
  // series[k * kMaxSigmaNodes + s] = dimensionless y[k] at σ-node s.
  double series[amer::kAlMaxNodes * sigma_family_detail::kMaxSigmaNodes] = {};

  // The refusal tree shared by price() and price_and_chord() (one sigma at a time).
  [[nodiscard]] AlSigmaFamilyStatus admit(double S, double K, double sigma) const noexcept;
  // Interpolate y[] at sigma, rescale to the contract's internal strike (T8
  // homogeneity: y is strike-free; K and xmax = K min(1, r/q) carry the strike) and
  // run the premium quadrature. Precondition: admit() == Ok.
  [[nodiscard]] double price_at(double S, double K, double sigma) const noexcept;
};

namespace sigma_family_detail {

// FamilyAccess (forward-declared in american.hpp as the family's one friend) is DEFINED
// in american_family_greeks.cpp: the Greeks TU's const reads of `st_` go through it.

// L3g measurement hook (bench / tests only): the frozen-boundary rho / phi of the
// option in two conventions — `*_frozen_y` differentiates the map with the
// dimensionless boundary y held (the explicit xmax(K, rate, yield) rescale of B IS
// differentiated: the frozen PART of the rho / phi family_greeks serves with a carry
// set, which adds the boundary term; it is never served alone — A0 r6, review-L3g m6),
// `*_frozen_b` holds the absolute boundary B (no xmax term). Both omit dy/dr, dy/dq.
// Refuses as price() does.
struct FamilyRhoVariants {
  double rho_frozen_y = 0.0;
  double phi_frozen_y = 0.0;
  double rho_frozen_b = 0.0;
  double phi_frozen_b = 0.0;
  bool exercised = false;
  AlSigmaFamilyStatus status = AlSigmaFamilyStatus::NotPrepared;
};
[[nodiscard]] FamilyRhoVariants family_rho_variants(const AlSigmaFamily &fam, double S, double K,
                                                    double sigma) noexcept;

} // namespace sigma_family_detail

} // namespace atx::vol
