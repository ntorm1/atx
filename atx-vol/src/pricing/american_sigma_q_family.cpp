// ── AlSigmaQFamily: the (σ, q) boundary family on a prepared expiry geometry ──────
//
// L2h-b sigma-q family (2026-10-21). Contract and design notes are on the class in
// api/pricing/american.hpp. This TU owns only the family: every node solve is the L2b
// prepared-geometry seam (amer::al_solve_put_boundary_prepared) on an orientation
// prepared at that node's (rate, yield) — the same per-node builder AlSigmaFamily uses
// — and every price is amer::al_put_price_from_boundary on a stack copy of the node
// grid at the strike's own carry, the premium quadrature a cold solve ends with.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_boundary.hpp"       // amer:: seam + AlExpiryGeometry::Impl
#include "pricing/american_sigma_family.hpp"   // sigma_family_detail::cheb_node / axis_of
#include "pricing/american_sigma_q_family.hpp" // State
#include "pricing/boundary_interp.hpp"         // detail::internal_put_coords / _rates (R-31)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using sigma_family_detail::axis_of;
using sigma_family_detail::cheb_node;
using sigma_family_detail::kMaxSigmaNodes;
using sigma_family_detail::kNaN;
using sigma_q_family_detail::kMaxQNodes;

namespace {

// andersen_lake's error for a failed node solve (the mapping AlSigmaFamily's
// node_solve_error and american.cpp's al_solve_status_error use; both are file-local).
[[nodiscard]] Status node_solve_error(amer::AlSolveStatus st) {
  switch (st) {
  case amer::AlSolveStatus::Collapsed:
    return Err(ErrorCode::NotImplemented,
               "AlSigmaQFamily: asymptotic boundary collapsed (xmax <= 0) at a node");
  case amer::AlSolveStatus::TableMissing:
    return Err(ErrorCode::Internal, "AlSigmaQFamily: Gauss-Legendre table unavailable");
  case amer::AlSolveStatus::NotConverged:
    return Err(ErrorCode::NotImplemented,
               "AlSigmaQFamily: a node's boundary sweep could not move (frozen seed)");
  case amer::AlSolveStatus::Ok:
    break;
  }
  return Ok();
}

// 2nd-kind barycentric coefficients at zq on nodes z[] / weights w[], or the exact node
// hit. bary_apply(coef(z, w, n, zq), y) reproduces sigma_family_detail::bary_eval(z, w,
// y, n, zq) BIT FOR BIT (same terms, same accumulation order, same final divide) — the
// collapsed-q identity with AlSigmaFamily rests on it — while letting one set of
// coefficients serve every boundary node of a price.
struct BaryCoef {
  double c[kMaxSigmaNodes] = {};
  double den = 0.0;
  unsigned n = 0;
  int hit = -1; // exact node index, or -1
};

[[nodiscard]] BaryCoef bary_coef(const double *z, const double *w, unsigned n,
                                 double zq) noexcept {
  BaryCoef b;
  b.n = n;
  for (unsigned i = 0; i < n; ++i) {
    const double dz = zq - z[i];
    if (dz == 0.0) {
      b.hit = static_cast<int>(i);
      return b;
    }
    const double qq = w[i] / dz;
    b.c[i] = qq;
    b.den += qq;
  }
  return b;
}

[[nodiscard]] double bary_apply(const BaryCoef &b, const double *y) noexcept {
  if (b.hit >= 0) {
    return y[b.hit];
  }
  double num = 0.0;
  for (unsigned i = 0; i < b.n; ++i) {
    num += b.c[i] * y[i];
  }
  return num / b.den;
}

// A coefficient set reduced to a plain weight vector with a separate scale: an exact
// hit becomes the unit vector with scale 1 (so the tensor product below needs no
// branch per term).
void bary_weights(const BaryCoef &b, double *wts, double &inv_den) noexcept {
  for (unsigned i = 0; i < b.n; ++i) {
    wts[i] = (b.hit < 0) ? b.c[i] : (static_cast<int>(i) == b.hit ? 1.0 : 0.0);
  }
  inv_den = (b.hit < 0) ? b.den : 1.0;
}

[[nodiscard]] double clamp_unit(double z) noexcept {
  return (z < -1.0) ? -1.0 : ((z > 1.0) ? 1.0 : z); // rounding at the box edges only
}

// One block of prepare-time scratch: the orientation of the current q node and the
// retained solve state (heap: ~2 x 46 KB would crowd a worker's stack).
struct PrepareScratch {
  amer::AlOrientedGeometry geo{};
  amer::AlPreparedScratch solve{};
};

} // namespace

AlSigmaQFamilyStatus AlSigmaQFamily::State::admit(double S, double K, double sigma,
                                                  double q) const noexcept {
  if (!ok) {
    return AlSigmaQFamilyStatus::NotPrepared;
  }
  if (g->generation != generation) {
    return AlSigmaQFamilyStatus::GeometryRePrepared;
  }
  if (!(std::isfinite(S) && S > 0.0 && std::isfinite(K) && K > 0.0 && std::isfinite(sigma) &&
        std::isfinite(q))) {
    return AlSigmaQFamilyStatus::InvalidArgument;
  }
  if (sigma < kAlSigmaFamilyMinSigma) {
    return AlSigmaQFamilyStatus::SigmaBelowGuard;
  }
  if (sigma < sigma_lo || sigma > sigma_hi) {
    return AlSigmaQFamilyStatus::SigmaOutsideBox;
  }
  if (q < q_lo || q > q_hi) {
    return AlSigmaQFamilyStatus::YieldOutsideBox;
  }
  return AlSigmaQFamilyStatus::Ok;
}

double AlSigmaQFamily::State::price_at(double S, double K, double sigma,
                                       double q) const noexcept {
  const detail::InternalPutCoords c = detail::internal_put_coords(side, S, K);
  const detail::InternalPutRates carry = detail::internal_put_rates(side, rate, q);
  const double z = clamp_unit(2.0 * (axis_of(axis, sigma) - u_lo) / (u_hi - u_lo) - 1.0);
  const BaryCoef cs = bary_coef(sz, sw, n_sigma, z);
  amer::AlBoundary bnd = grid; // stack scratch: the only per-call state
  if (n_q == 1) {
    // Collapsed q axis: exactly AlSigmaFamily's per-node σ evaluation.
    for (unsigned k = 0; k < n_boundary; ++k) {
      bnd.y[k] = bary_apply(cs, &series[static_cast<std::size_t>(k) * n_sigma]);
    }
  } else {
    // Tensor 2nd-kind barycentric: y_k = sum_{j,s} wq_j ws_s y_kjs / (den_q den_s).
    const BaryCoef cq = bary_coef(qz, qw, n_q, clamp_unit((q - q_lo) * q_scale - 1.0));
    double ws_s[kMaxSigmaNodes];
    double wq_j[kMaxQNodes];
    double den_s = 1.0;
    double den_q = 1.0;
    bary_weights(cs, ws_s, den_s);
    bary_weights(cq, wq_j, den_q);
    const unsigned nt = static_cast<unsigned>(n_q) * n_sigma;
    double wt[static_cast<std::size_t>(kMaxQNodes) * kMaxSigmaNodes];
    for (unsigned j = 0; j < n_q; ++j) {
      for (unsigned s = 0; s < n_sigma; ++s) {
        wt[j * n_sigma + s] = wq_j[j] * ws_s[s];
      }
    }
    const double inv = 1.0 / (den_q * den_s);
    for (unsigned k = 0; k < n_boundary; ++k) {
      const double *yk = &series[static_cast<std::size_t>(k) * nt];
      double acc = 0.0;
      for (unsigned t = 0; t < nt; ++t) {
        acc += wt[t] * yk[t];
      }
      bnd.y[k] = acc * inv;
    }
  }
  bnd.K = c.Kp;
  bnd.xmax = amer::al_xmax_put(c.Kp, carry.rp, carry.qp);
  return amer::al_put_price_from_boundary(bnd, *ws, c.Sp, c.Kp, grid.T, sigma, carry.rp,
                                          carry.qp);
}

AlSigmaQFamily::AlSigmaQFamily(const AlExpiryGeometry &geometry, Side side, double anchor)
    : st_(std::make_unique<State>()) {
  st_->g = geometry.impl();
  st_->side = side;
  st_->anchor = anchor;
}
AlSigmaQFamily::~AlSigmaQFamily() = default;
AlSigmaQFamily::AlSigmaQFamily(AlSigmaQFamily &&) noexcept = default;
AlSigmaQFamily &AlSigmaQFamily::operator=(AlSigmaQFamily &&) noexcept = default;

std::size_t AlSigmaQFamily::state_bytes() noexcept { return sizeof(State); }

namespace {

// The input checks of prepare() that need no solve. Ok() or the refusal.
[[nodiscard]] Status check_prepare_inputs(const AlExpiryGeometry::Impl *g, Side side,
                                          double anchor, double sigma_lo, double sigma_hi,
                                          std::uint16_t n_sigma, double q_lo, double q_hi,
                                          std::uint16_t n_q) {
  if (g == nullptr || !g->prepared) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaQFamily: expiry geometry is not prepared");
  }
  if (!(std::isfinite(anchor) && anchor > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaQFamily: anchor must be > 0");
  }
  if (!(std::isfinite(sigma_lo) && std::isfinite(sigma_hi)) || sigma_lo < kAlSigmaFamilyMinSigma ||
      !(sigma_hi - sigma_lo > 1.0e-9)) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaQFamily: sigma box must satisfy kAlSigmaFamilyMinSigma <= sigma_lo < "
               "sigma_hi (finite, width > 1e-9)");
  }
  if (n_sigma < kAlSigmaFamilyMinNodes || n_sigma > kAlSigmaFamilyMaxNodes) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaQFamily: n_sigma must lie in [kAlSigmaFamilyMinNodes, "
               "kAlSigmaFamilyMaxNodes]");
  }
  if (!(std::isfinite(q_lo) && std::isfinite(q_hi)) || q_lo > q_hi) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaQFamily: q box must be finite with q_lo <= q_hi");
  }
  const bool degenerate = (q_hi - q_lo) <= kAlSigmaQFamilyDegenerateWidth;
  if (!degenerate && (n_q < kAlSigmaQFamilyMinQNodes || n_q > kAlSigmaQFamilyMaxQNodes)) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaQFamily: n_q must lie in [kAlSigmaQFamilyMinQNodes, "
               "kAlSigmaQFamilyMaxQNodes] on a non-degenerate q box");
  }
  if (!(std::isfinite(g->T) && g->T >= kAlSigmaFamilyMinTau)) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaQFamily: expiry below the near-expiry guard kAlSigmaFamilyMinTau");
  }
  const amer::AlOrientedGeometry &orient = (side == Side::Put) ? g->put : g->call;
  if (orient.id == 0 || !orient.tables_ok) {
    return Err(ErrorCode::NotImplemented,
               "AlSigmaQFamily: the geometry has no American orientation for this side at "
               "its own q (build it at a q inside the box)");
  }
  // The American set of an internal put is convex in q for a fixed r (puts: every q
  // when r > 0; calls: q > 0, or q == 0 with r < 0), so both ends cover the box.
  for (const double qe : {q_lo, q_hi}) {
    const detail::InternalPutRates rq = detail::internal_put_rates(side, g->r, qe);
    if (detail::classify_regime(rq.rp, rq.qp) != detail::ExerciseRegime::American) {
      return Err(ErrorCode::NotImplemented,
                 "AlSigmaQFamily: side is not American over the whole q box (a call needs "
                 "q > 0; price q <= 0 calls in closed form)");
    }
  }
  if (q_lo < g->r && g->r < q_hi) {
    return Err(ErrorCode::NotImplemented,
               "AlSigmaQFamily: the q box straddles q = r, where the boundary scale "
               "K min(1, r/q) has a kink; split the box at r");
  }
  return Ok();
}

} // namespace

Status AlSigmaQFamily::prepare(double sigma_lo, double sigma_hi, std::uint16_t n_sigma,
                               double q_lo, double q_hi, std::uint16_t n_q,
                               AlSigmaFamilyAxis axis) {
  if (!st_) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaQFamily: moved-from family");
  }
  State &st = *st_;
  st.ok = false; // a refusal below leaves the family unprepared, never half-built
  if (Status in = check_prepare_inputs(st.g, st.side, st.anchor, sigma_lo, sigma_hi, n_sigma,
                                       q_lo, q_hi, n_q);
      !in) {
    return in;
  }
  const AlExpiryGeometry::Impl &g = *st.g;
  const amer::AlOrientedGeometry &orient = (st.side == Side::Put) ? g.put : g.call;
  // R-31: the orientation's (rate, yield) is the single-sourced internal-put map.
  assert(detail::internal_put_rates(st.side, g.r, g.q).rp == orient.rate);
  assert(detail::internal_put_rates(st.side, g.r, g.q).qp == orient.yield);
  const bool degenerate = (q_hi - q_lo) <= kAlSigmaQFamilyDegenerateWidth;
  const std::uint16_t nq = degenerate ? std::uint16_t{1} : n_q;

  // Chebyshev-Lobatto nodes + 2nd-kind weights on both axes (AlSigmaFamily's form).
  for (unsigned s = 0; s < kMaxSigmaNodes; ++s) {
    st.sz[s] = (s < n_sigma) ? cheb_node(s, n_sigma) : 0.0;
    double w = (s & 1u) ? -1.0 : 1.0;
    if (s == 0 || s + 1 == n_sigma) {
      w *= 0.5;
    }
    st.sw[s] = (s < n_sigma) ? w : 0.0;
  }
  for (unsigned j = 0; j < kMaxQNodes; ++j) {
    st.qz[j] = (j < nq) ? cheb_node(j, nq) : 0.0;
    double w = (j & 1u) ? -1.0 : 1.0;
    if (j == 0 || j + 1 == nq) {
      w *= 0.5;
    }
    st.qw[j] = (j < nq) ? w : 0.0;
  }
  const double u_lo = axis_of(axis, sigma_lo);
  const double u_hi = axis_of(axis, sigma_hi);
  const double half_span = 0.5 * (u_hi - u_lo);
  const double q_half = 0.5 * (q_hi - q_lo);

  // n_q orientations x n_sigma prepared solves at the anchor, at the geometry's AlOpts.
  const std::unique_ptr<PrepareScratch> work = std::make_unique<PrepareScratch>();
  for (unsigned j = 0; j < nq; ++j) {
    double q_j = q_lo + q_half * (st.qz[j] + 1.0);
    if (degenerate) {
      q_j = 0.5 * (q_lo + q_hi);
    } else if (j == 0) {
      q_j = q_lo;
    } else if (j + 1 == nq) {
      q_j = q_hi; // the box ends are nodes exactly (a price at q_hi hits this node)
    }
    const detail::InternalPutRates rq = detail::internal_put_rates(st.side, g.r, q_j);
    if (!(amer::al_xmax_put(st.anchor, rq.rp, rq.qp) > 0.0)) {
      return Err(ErrorCode::NotImplemented,
                 "AlSigmaQFamily: asymptotic boundary collapsed (xmax <= 0) at the anchor");
    }
    amer::al_prepare_oriented_geometry(g.T, rq.rp, rq.qp, g.sch, work->geo);
    if (work->geo.id == 0 || !work->geo.tables_ok) {
      return node_solve_error(amer::AlSolveStatus::TableMissing);
    }
    for (unsigned s = 0; s < n_sigma; ++s) {
      const double u = u_lo + half_span * (st.sz[s] + 1.0);
      const double sig_s = (axis == AlSigmaFamilyAxis::LogSigma) ? std::exp(u) : u;
      amer::AlPreparedScratch &sc = work->solve;
      const amer::AlSolveStatus stv =
          amer::al_solve_put_boundary_prepared(work->geo, st.anchor, sig_s, sc.bnd, sc.ws,
                                               &sc.loaded);
      if (stv != amer::AlSolveStatus::Ok) {
        return node_solve_error(stv);
      }
      if (j == 0 && s == 0) {
        st.grid = sc.bnd; // K- and carry-independent node structure; y/K/xmax per price
      }
      assert(sc.bnd.n == st.grid.n);
      for (unsigned k = 0; k < sc.bnd.n; ++k) {
        st.series[(static_cast<std::size_t>(k) * nq + j) * n_sigma + s] = sc.bnd.y[k];
      }
    }
  }

  // Publish (every field; the previous preparation is fully replaced).
  st.generation = g.generation;
  st.axis = axis;
  st.sigma_lo = sigma_lo;
  st.sigma_hi = sigma_hi;
  st.u_lo = u_lo;
  st.u_hi = u_hi;
  st.q_lo = q_lo;
  st.q_hi = q_hi;
  st.q_scale = degenerate ? 0.0 : 2.0 / (q_hi - q_lo);
  st.rate = g.r;
  st.n_sigma = n_sigma;
  st.n_q = nq;
  st.n_boundary = st.grid.n;
  st.ws = &orient.ws;
  st.ok = true;
  return Ok();
}

bool AlSigmaQFamily::prepared() const noexcept { return st_ && st_->ok; }
double AlSigmaQFamily::sigma_lo() const noexcept { return prepared() ? st_->sigma_lo : kNaN; }
double AlSigmaQFamily::sigma_hi() const noexcept { return prepared() ? st_->sigma_hi : kNaN; }
double AlSigmaQFamily::q_lo() const noexcept { return prepared() ? st_->q_lo : kNaN; }
double AlSigmaQFamily::q_hi() const noexcept { return prepared() ? st_->q_hi : kNaN; }
std::uint16_t AlSigmaQFamily::n_sigma() const noexcept { return prepared() ? st_->n_sigma : 0; }
std::uint16_t AlSigmaQFamily::n_q() const noexcept { return prepared() ? st_->n_q : 0; }
double AlSigmaQFamily::anchor() const noexcept { return st_ ? st_->anchor : kNaN; }
Side AlSigmaQFamily::side() const noexcept { return st_ ? st_->side : Side::Call; }

AlSigmaQFamilyPrice AlSigmaQFamily::price(double S, double K, double sigma,
                                          double q) const noexcept {
  const State *st = st_.get();
  if (st == nullptr) {
    return AlSigmaQFamilyPrice{kNaN, AlSigmaQFamilyStatus::NotPrepared};
  }
  const AlSigmaQFamilyStatus a = st->admit(S, K, sigma, q);
  if (a != AlSigmaQFamilyStatus::Ok) {
    return AlSigmaQFamilyPrice{kNaN, a};
  }
  return AlSigmaQFamilyPrice{st->price_at(S, K, sigma, q), AlSigmaQFamilyStatus::Ok};
}

AlSigmaQFamilyChord AlSigmaQFamily::price_and_chord(double S, double K, double sigma,
                                                    double q) const noexcept {
  const State *st = st_.get();
  if (st == nullptr) {
    return AlSigmaQFamilyChord{kNaN, kNaN, AlSigmaQFamilyStatus::NotPrepared};
  }
  const AlSigmaQFamilyStatus a = st->admit(S, K, sigma, q);
  if (a != AlSigmaQFamilyStatus::Ok) {
    return AlSigmaQFamilyChord{kNaN, kNaN, a};
  }
  const double bumped = sigma + kAlPackSigmaBump;
  const AlSigmaQFamilyStatus b = st->admit(S, K, bumped, q);
  if (b != AlSigmaQFamilyStatus::Ok) {
    return AlSigmaQFamilyChord{kNaN, kNaN, b};
  }
  return AlSigmaQFamilyChord{st->price_at(S, K, sigma, q), st->price_at(S, K, bumped, q),
                             AlSigmaQFamilyStatus::Ok};
}

} // namespace atx::vol
