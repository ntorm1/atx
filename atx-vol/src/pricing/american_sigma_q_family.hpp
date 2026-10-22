#pragma once

// ── AlSigmaQFamily::State — the (σ, q) boundary family's prepared state ──────────
//
// L2h-b sigma-q family (2026-10-21). Kept in its own internal header (as L3g moved
// AlSigmaFamily::State into american_sigma_family.hpp) so a later analytics TU can read
// the prepared tensor without touching american_sigma_q_family.cpp. The σ axis reuses
// sigma_family_detail's cheb_node / axis_of verbatim (the P2.5 twin note lives there);
// the q axis is the same Chebyshev-Lobatto construction, linear in q.
// L2h-c (2026-10-21): State is private to the class; QFamilyAccess (the class's friend,
// below) is the one door an internal TU reads it through (review-L2hb m4).
//
// NOT a public API. Not installed. No stability guarantees.

#include <cstddef>
#include <cstdint>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_boundary.hpp"     // amer::AlBoundary / AlWorkspace / Impl
#include "pricing/american_sigma_family.hpp" // sigma_family_detail::kMaxSigmaNodes

namespace atx::vol {

namespace sigma_q_family_detail {

inline constexpr std::uint16_t kMaxQNodes = kAlSigmaQFamilyMaxQNodes;
inline constexpr std::size_t kSeriesCapacity = static_cast<std::size_t>(amer::kAlMaxNodes) *
                                               kMaxQNodes * sigma_family_detail::kMaxSigmaNodes;

} // namespace sigma_q_family_detail

// Immutable after a successful prepare(); read-only in price(). No mutable scratch: the
// interpolated boundary and the barycentric weights live on the caller's stack, so N
// threads pricing through one const family touch only const memory (no write after
// prepare(); prepare() and the geometry's prepare() are documented as exclusive).
struct AlSigmaQFamily::State {
  const AlExpiryGeometry::Impl *g = nullptr;
  Side side = Side::Call;
  double anchor = 0.0;
  // ── set by prepare() ──
  bool ok = false;
  std::uint64_t generation = 0; // the geometry's generation at prepare()
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double u_lo = 0.0; // the σ box on the axis coordinate
  double u_hi = 0.0;
  double q_lo = 0.0; // the q box as given
  double q_hi = 0.0;
  double q_scale = 0.0; // 2 / (q_hi - q_lo) on a non-degenerate box, else 0
  double rate = 0.0;    // the expiry's r (the geometry's)
  std::uint16_t n_sigma = 0;
  std::uint16_t n_q = 0; // 1 on a degenerate box
  std::uint16_t n_boundary = 0;
  // The premium quadrature's workspace header (qx/qw_price, n_quad_price, fast,
  // specialize): a geometry orientation's — `side`'s, or the other side's when `side` is
  // European at the geometry's q (review-L2hb m1). It is q- and side-free (the
  // quadrature takes rate / yield as arguments; both orientations share the geometry's
  // scheme and the process-wide tables), owned by the geometry and immutable until its
  // next prepare(), which `generation` detects.
  const amer::AlWorkspace *ws = nullptr;
  amer::AlBoundary grid{}; // node structure (n, T, z, wbary, x, tau); y/K/xmax per price
  double sz[sigma_family_detail::kMaxSigmaNodes] = {}; // σ Chebyshev-Lobatto nodes in [-1, 1]
  double sw[sigma_family_detail::kMaxSigmaNodes] = {}; // 2nd-kind weights (endpoints halved)
  double qz[sigma_q_family_detail::kMaxQNodes] = {};   // q Chebyshev-Lobatto nodes in [-1, 1]
  double qw[sigma_q_family_detail::kMaxQNodes] = {};
  AlSigmaQFamilySelfCheck check{}; // prepare(…, OffNode)'s measurement (else NotPrepared)
  // series[(k * n_q + j) * n_sigma + s] = dimensionless y[k] at σ-node s, q-node j
  // (packed on the prepared counts so one boundary node's tensor is contiguous).
  double series[sigma_q_family_detail::kSeriesCapacity] = {};

  // The refusal tree shared by every price entry (one sigma at a time).
  [[nodiscard]] AlSigmaQFamilyStatus admit(double S, double K, double sigma,
                                           double q) const noexcept;
  // admit()'s (S, sigma, q) part — every check but the strike's.
  [[nodiscard]] AlSigmaQFamilyStatus admit_point(double S, double sigma, double q) const noexcept;
  // The interpolated dimensionless boundary at (sigma, q) into bnd.y (bnd = a copy of
  // grid). Precondition: admit_point() == Ok.
  void interpolate(double sigma, double q, amer::AlBoundary &bnd) const noexcept;
  // Rescale an interpolated boundary to the contract's internal strike (T8 homogeneity;
  // xmax = K min(1, rate/yield) at the STRIKE's own carry) and run the European leg +
  // premium quadrature at that carry. Writes bnd.K / bnd.xmax only.
  [[nodiscard]] double price_on(amer::AlBoundary &bnd, double S, double K, double sigma,
                                double q) const noexcept;
  // interpolate + price_on. Precondition: admit() == Ok.
  [[nodiscard]] double price_at(double S, double K, double sigma, double q) const noexcept;
  // The OffNode self-check on a fully published state (AlSigmaQFamilySelfCheck); `geo` /
  // `sc` are prepare()'s scratch, reused for the check points' cold solves.
  [[nodiscard]] AlSigmaQFamilySelfCheck
  measure_self_check(amer::AlOrientedGeometry &geo, amer::AlPreparedScratch &sc) const noexcept;
};

namespace sigma_q_family_detail {

// The class's friend: hands an internal TU the prepared state (const; null when the
// family is moved-from). L2h-c (review-L2hb m4).
struct QFamilyAccess {
  [[nodiscard]] static const AlSigmaQFamily::State *state(const AlSigmaQFamily &f) noexcept {
    return f.st_.get();
  }
};

} // namespace sigma_q_family_detail

} // namespace atx::vol
