// ── AlSigmaFamily: the σ-Chebyshev boundary family on a prepared expiry geometry ──
//
// L2h sigma-family (2026-10-19). Contract and the design notes are on the class in
// api/pricing/american.hpp. This TU owns only the family: the node solves run
// through the L2b prepared-geometry seam (amer::al_solve_put_boundary_prepared), the
// σ-axis Chebyshev evaluator is the P2.5 slice's scheme re-expressed here (see the
// twin note below), and every price is amer::al_put_price_from_boundary on a stack
// copy of the node grid — the same premium quadrature a cold solve ends with.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_boundary.hpp"     // amer:: seam + AlExpiryGeometry::Impl
#include "pricing/american_sigma_family.hpp" // State + the σ-axis evaluator (L3g code motion)
#include "pricing/boundary_interp.hpp"       // detail::internal_put_coords / _rates (R-31)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// L3g family-greeks (2026-10-20): State, cheb_node / bary_eval / axis_of and the three
// constants moved verbatim into pricing/american_sigma_family.hpp so the Greeks TU can
// read the series; the bodies below are unchanged.
using sigma_family_detail::axis_of;
using sigma_family_detail::bary_eval;
using sigma_family_detail::cheb_node;
using sigma_family_detail::kMaxSigmaNodes;
using sigma_family_detail::kNaN;

namespace {

// andersen_lake's error for a failed node solve (mirrors american.cpp's
// al_solve_status_error, which is file-local there).
[[nodiscard]] Status node_solve_error(amer::AlSolveStatus st) {
  switch (st) {
  case amer::AlSolveStatus::Collapsed:
    return Err(ErrorCode::NotImplemented,
               "AlSigmaFamily: asymptotic boundary collapsed (xmax <= 0) at a sigma node");
  case amer::AlSolveStatus::TableMissing:
    return Err(ErrorCode::Internal, "AlSigmaFamily: Gauss-Legendre table unavailable");
  case amer::AlSolveStatus::NotConverged:
    return Err(ErrorCode::NotImplemented,
               "AlSigmaFamily: a sigma node's boundary sweep could not move (frozen seed)");
  case amer::AlSolveStatus::Ok:
    break;
  }
  return Ok();
}

} // namespace

AlSigmaFamilyStatus AlSigmaFamily::State::admit(double S, double K, double sigma) const noexcept {
  if (!ok) {
    return AlSigmaFamilyStatus::NotPrepared;
  }
  if (g->generation != generation) {
    return AlSigmaFamilyStatus::GeometryRePrepared;
  }
  if (!(std::isfinite(S) && S > 0.0 && std::isfinite(K) && K > 0.0 && std::isfinite(sigma))) {
    return AlSigmaFamilyStatus::InvalidArgument;
  }
  if (sigma < kAlSigmaFamilyMinSigma) {
    return AlSigmaFamilyStatus::SigmaBelowGuard;
  }
  if (sigma < sigma_lo || sigma > sigma_hi) {
    return AlSigmaFamilyStatus::SigmaOutsideBox;
  }
  return AlSigmaFamilyStatus::Ok;
}

double AlSigmaFamily::State::price_at(double S, double K, double sigma) const noexcept {
  const detail::InternalPutCoords c = detail::internal_put_coords(side, S, K);
  double z = 2.0 * (axis_of(axis, sigma) - u_lo) / (u_hi - u_lo) - 1.0;
  z = (z < -1.0) ? -1.0 : ((z > 1.0) ? 1.0 : z); // rounding at the box edges only
  amer::AlBoundary bnd = grid; // stack scratch: the only per-call state
  for (unsigned k = 0; k < n_boundary; ++k) {
    bnd.y[k] = bary_eval(sz, sw, &series[k * kMaxSigmaNodes], n_sigma, z);
  }
  bnd.K = c.Kp;
  bnd.xmax = amer::al_xmax_put(c.Kp, rate, yield);
  return amer::al_put_price_from_boundary(bnd, *ws, c.Sp, c.Kp, grid.T, sigma, rate, yield);
}

AlSigmaFamily::AlSigmaFamily(const AlExpiryGeometry &geometry, Side side, double anchor)
    : st_(std::make_unique<State>()) {
  st_->g = geometry.impl();
  st_->side = side;
  st_->anchor = anchor;
}
AlSigmaFamily::~AlSigmaFamily() = default;
AlSigmaFamily::AlSigmaFamily(AlSigmaFamily &&) noexcept = default;
AlSigmaFamily &AlSigmaFamily::operator=(AlSigmaFamily &&) noexcept = default;

std::size_t AlSigmaFamily::state_bytes() noexcept { return sizeof(State); }

Status AlSigmaFamily::prepare(double sigma_lo, double sigma_hi, std::uint16_t n_sigma,
                              AlSigmaFamilyAxis axis) {
  if (!st_) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaFamily: moved-from family");
  }
  State &st = *st_;
  st.ok = false; // a refusal below leaves the family unprepared, never half-built
  if (st.g == nullptr || !st.g->prepared) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaFamily: expiry geometry is not prepared");
  }
  const AlExpiryGeometry::Impl &g = *st.g;
  if (!(std::isfinite(st.anchor) && st.anchor > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaFamily: anchor must be > 0");
  }
  if (!(std::isfinite(sigma_lo) && std::isfinite(sigma_hi)) || sigma_lo < kAlSigmaFamilyMinSigma ||
      !(sigma_hi - sigma_lo > 1.0e-9)) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaFamily: sigma box must satisfy kAlSigmaFamilyMinSigma <= sigma_lo < "
               "sigma_hi (finite, width > 1e-9)");
  }
  if (n_sigma < kAlSigmaFamilyMinNodes || n_sigma > kAlSigmaFamilyMaxNodes) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaFamily: n_sigma must lie in [kAlSigmaFamilyMinNodes, "
               "kAlSigmaFamilyMaxNodes]");
  }
  if (!(std::isfinite(g.T) && g.T >= kAlSigmaFamilyMinTau)) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaFamily: expiry below the near-expiry guard kAlSigmaFamilyMinTau");
  }
  // The geometry prepares an orientation only for a valid expiry whose side is in
  // the American regime (AlExpiryGeometry::prepare): id == 0 is the closed-form /
  // double-continuation case, which has no boundary to build a family of.
  const amer::AlOrientedGeometry &orient = (st.side == Side::Put) ? g.put : g.call;
  if (orient.id == 0 || !orient.tables_ok) {
    return Err(ErrorCode::NotImplemented,
               "AlSigmaFamily: side is not in the American regime on this expiry (no "
               "early-exercise boundary to interpolate)");
  }
  // R-31: the orientation's (rate, yield) is the single-sourced internal-put map.
  assert(detail::internal_put_rates(st.side, g.r, g.q).rp == orient.rate);
  assert(detail::internal_put_rates(st.side, g.r, g.q).qp == orient.yield);
  if (!(amer::al_xmax_put(st.anchor, orient.rate, orient.yield) > 0.0)) {
    return Err(ErrorCode::NotImplemented,
               "AlSigmaFamily: asymptotic boundary collapsed (xmax <= 0) at the anchor");
  }

  // σ Chebyshev-Lobatto nodes + weights on the axis coordinate.
  double sz[kMaxSigmaNodes] = {};
  double sw[kMaxSigmaNodes] = {};
  for (unsigned s = 0; s < n_sigma; ++s) {
    sz[s] = cheb_node(s, n_sigma);
    double w = (s & 1u) ? -1.0 : 1.0;
    if (s == 0 || s + 1 == n_sigma) {
      w *= 0.5;
    }
    sw[s] = w;
  }
  const double u_lo = axis_of(axis, sigma_lo);
  const double u_hi = axis_of(axis, sigma_hi);
  const double half_span = 0.5 * (u_hi - u_lo);

  // n_σ prepared solves at the anchor (the geometry's own AlOpts, so a Reference
  // geometry gives Reference-accuracy nodes). ~46 KB of solve scratch on the stack,
  // as the stateless andersen_lake(geometry, ...) overload uses.
  amer::AlPreparedScratch scratch;
  double series[amer::kAlMaxNodes * kMaxSigmaNodes] = {};
  amer::AlBoundary grid{};
  for (unsigned s = 0; s < n_sigma; ++s) {
    const double u = u_lo + half_span * (sz[s] + 1.0);
    const double sig_s = (axis == AlSigmaFamilyAxis::LogSigma) ? std::exp(u) : u;
    const amer::AlSolveStatus stv = amer::al_solve_put_boundary_prepared(
        orient, st.anchor, sig_s, scratch.bnd, scratch.ws, &scratch.loaded);
    if (stv != amer::AlSolveStatus::Ok) {
      return node_solve_error(stv);
    }
    if (s == 0) {
      grid = scratch.bnd; // K-independent node structure; y/K/xmax are rewritten per price
    }
    for (unsigned k = 0; k < scratch.bnd.n; ++k) {
      series[k * kMaxSigmaNodes + s] = scratch.bnd.y[k];
    }
  }

  // Publish (every field; the previous preparation is fully replaced).
  st.generation = g.generation;
  st.axis = axis;
  st.sigma_lo = sigma_lo;
  st.sigma_hi = sigma_hi;
  st.u_lo = u_lo;
  st.u_hi = u_hi;
  st.n_sigma = n_sigma;
  st.n_boundary = grid.n;
  st.rate = orient.rate;
  st.yield = orient.yield;
  st.ws = &orient.ws;
  st.grid = grid;
  for (unsigned s = 0; s < kMaxSigmaNodes; ++s) {
    st.sz[s] = sz[s];
    st.sw[s] = sw[s];
  }
  for (std::size_t i = 0; i < amer::kAlMaxNodes * kMaxSigmaNodes; ++i) {
    st.series[i] = series[i];
  }
  st.ok = true;
  return Ok();
}

bool AlSigmaFamily::prepared() const noexcept { return st_ && st_->ok; }
double AlSigmaFamily::sigma_lo() const noexcept { return prepared() ? st_->sigma_lo : kNaN; }
double AlSigmaFamily::sigma_hi() const noexcept { return prepared() ? st_->sigma_hi : kNaN; }
std::uint16_t AlSigmaFamily::n_sigma() const noexcept { return prepared() ? st_->n_sigma : 0; }
double AlSigmaFamily::anchor() const noexcept { return st_ ? st_->anchor : kNaN; }
Side AlSigmaFamily::side() const noexcept { return st_ ? st_->side : Side::Call; }

AlSigmaFamilyPrice AlSigmaFamily::price(double S, double K, double sigma) const noexcept {
  const State *st = st_.get();
  if (st == nullptr) {
    return AlSigmaFamilyPrice{kNaN, AlSigmaFamilyStatus::NotPrepared};
  }
  const AlSigmaFamilyStatus a = st->admit(S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return AlSigmaFamilyPrice{kNaN, a};
  }
  return AlSigmaFamilyPrice{st->price_at(S, K, sigma), AlSigmaFamilyStatus::Ok};
}

AlSigmaFamilyChord AlSigmaFamily::price_and_chord(double S, double K, double sigma) const noexcept {
  const State *st = st_.get();
  if (st == nullptr) {
    return AlSigmaFamilyChord{kNaN, kNaN, AlSigmaFamilyStatus::NotPrepared};
  }
  const AlSigmaFamilyStatus a = st->admit(S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return AlSigmaFamilyChord{kNaN, kNaN, a};
  }
  const double bumped = sigma + kAlPackSigmaBump;
  const AlSigmaFamilyStatus b = st->admit(S, K, bumped);
  if (b != AlSigmaFamilyStatus::Ok) {
    return AlSigmaFamilyChord{kNaN, kNaN, b};
  }
  return AlSigmaFamilyChord{st->price_at(S, K, sigma), st->price_at(S, K, bumped),
                            AlSigmaFamilyStatus::Ok};
}

} // namespace atx::vol
