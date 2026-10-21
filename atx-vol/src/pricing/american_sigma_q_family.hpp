#pragma once

// ── AlSigmaQFamily::State — the (σ, q) boundary family's prepared state ──────────
//
// L2h-b sigma-q family (2026-10-21). Kept in its own internal header (as L3g moved
// AlSigmaFamily::State into american_sigma_family.hpp) so a later analytics TU can read
// the prepared tensor without touching american_sigma_q_family.cpp. The σ axis reuses
// sigma_family_detail's cheb_node / axis_of verbatim (the P2.5 twin note lives there);
// the q axis is the same Chebyshev-Lobatto construction, linear in q.
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
  // specialize): the geometry's orientation for `side`; it is q-free (the quadrature
  // takes rate / yield as arguments), owned by the geometry and immutable until its next
  // prepare(), which `generation` detects.
  const amer::AlWorkspace *ws = nullptr;
  amer::AlBoundary grid{}; // node structure (n, T, z, wbary, x, tau); y/K/xmax per price
  double sz[sigma_family_detail::kMaxSigmaNodes] = {}; // σ Chebyshev-Lobatto nodes in [-1, 1]
  double sw[sigma_family_detail::kMaxSigmaNodes] = {}; // 2nd-kind weights (endpoints halved)
  double qz[sigma_q_family_detail::kMaxQNodes] = {};   // q Chebyshev-Lobatto nodes in [-1, 1]
  double qw[sigma_q_family_detail::kMaxQNodes] = {};
  // series[(k * n_q + j) * n_sigma + s] = dimensionless y[k] at σ-node s, q-node j
  // (packed on the prepared counts so one boundary node's tensor is contiguous).
  double series[sigma_q_family_detail::kSeriesCapacity] = {};

  // The refusal tree shared by price() and price_and_chord() (one sigma at a time).
  [[nodiscard]] AlSigmaQFamilyStatus admit(double S, double K, double sigma,
                                           double q) const noexcept;
  // Interpolate y[] at (sigma, q), rescale to the contract's internal strike (T8
  // homogeneity; xmax = K min(1, rate/yield) at the STRIKE's own carry) and run the
  // European leg + premium quadrature at that carry. Precondition: admit() == Ok.
  [[nodiscard]] double price_at(double S, double K, double sigma, double q) const noexcept;
};

} // namespace atx::vol
