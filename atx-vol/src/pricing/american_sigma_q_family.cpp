// ── AlSigmaQFamily: the (σ, q) boundary family on a prepared expiry geometry ──────
//
// L2h-b sigma-q family (2026-10-21). Contract and design notes are on the class in
// api/pricing/american.hpp. This TU owns only the family: every node solve is the L2b
// prepared-geometry seam (amer::al_solve_put_boundary_prepared) on an orientation
// prepared at that node's (rate, yield) — the same per-node builder AlSigmaFamily uses
// — and every price is amer::al_put_price_from_boundary on a stack copy of the node
// grid at the strike's own carry, the premium quadrature a cold solve ends with.
// L2h-c (2026-10-21): review-L2hb's minors (m1 quadrature header from either
// orientation, m2-m3 weight helpers, m5 prepare() split, m6 the q-axis accuracy guard),
// price_strikes (one interpolation per (σ, q) for a strike vector) and the opt-in
// OffNode self-check; THE BUILD: the nodes go four at a time through the AVX2 boundary
// kernel (simd/american_boundary_nodes_avx2.hpp) when the host and the scheme allow,
// the scalar prepared solve otherwise and for every lane the kernel declines.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>

#include "atx/core/math.hpp"                  // norm_cdf / norm_pdf (K1: the Reference Φ, φ)
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"     // black76_value_and_vega (K1: the European leg)
#include "atx/vol/api/simd/cpu.hpp"            // simd::use_avx2 (THE BUILD's route)
#include "pricing/american_boundary.hpp"       // amer:: seam + AlExpiryGeometry::Impl
#include "pricing/american_detail.hpp"         // detail::AlSeedMode
#include "pricing/american_sigma_family.hpp"   // sigma_family_detail::cheb_node / axis_of
#include "pricing/american_sigma_q_family.hpp" // State
#include "pricing/boundary_interp.hpp"         // detail::internal_put_coords / _rates (R-31)
#include "pricing/scalar_erfc.hpp"             // detail::norm_cdf_erfc / norm_pdf_cody (Fast)
#include "simd/american_boundary_nodes_avx2.hpp" // the 4-lane node solve (L2h-c)

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

// A coefficient set reduced to a plain weight vector with a separate denominator: an
// exact hit becomes the unit vector with denominator 1 (so the tensor product below
// needs no branch per term).
void bary_weights(const BaryCoef &b, double *wts, double &den) noexcept {
  for (unsigned i = 0; i < b.n; ++i) {
    wts[i] = (b.hit < 0) ? b.c[i] : (static_cast<int>(i) == b.hit ? 1.0 : 0.0);
  }
  den = (b.hit < 0) ? b.den : 1.0;
}

[[nodiscard]] double clamp_unit(double z) noexcept {
  return (z < -1.0) ? -1.0 : ((z > 1.0) ? 1.0 : z); // rounding at the box edges only
}

// Chebyshev-Lobatto nodes in [-1, 1] and their 2nd-kind weights ((-1)^i, endpoints
// halved — AlSigmaFamily's form) for n of `cap` slots; the unused slots are zeroed.
void lay_lobatto_axis(unsigned n, unsigned cap, double *z, double *w) noexcept {
  for (unsigned i = 0; i < cap; ++i) {
    double wi = (i & 1u) ? -1.0 : 1.0;
    if (i == 0 || i + 1 == n) {
      wi *= 0.5;
    }
    z[i] = (i < n) ? cheb_node(i, n) : 0.0;
    w[i] = (i < n) ? wi : 0.0;
  }
}

// The Bernstein-ellipse factor of the q box [q_lo, q_hi] against q = r, the singular
// point of the boundary in q on both sides (THE q BOX in american.hpp): 1 when the box
// touches or contains r. Precondition: q_hi > q_lo.
[[nodiscard]] double q_axis_rho(double r, double q_lo, double q_hi) noexcept {
  const double half = 0.5 * (q_hi - q_lo);
  const double a = std::abs(0.5 * (q_lo + q_hi) - r) / half;
  return (a > 1.0) ? a + std::sqrt(a * a - 1.0) : 1.0;
}

// The orientation whose quadrature header the family prices with: `side`'s, else the
// other side's (review-L2hb m1: the header is q- and side-free), else none.
[[nodiscard]] const amer::AlOrientedGeometry *
quadrature_orientation(const AlExpiryGeometry::Impl &g, Side side) noexcept {
  const amer::AlOrientedGeometry &own = (side == Side::Put) ? g.put : g.call;
  const amer::AlOrientedGeometry &other = (side == Side::Put) ? g.call : g.put;
  if (own.id != 0 && own.tables_ok) {
    return &own;
  }
  if (other.id != 0 && other.tables_ok) {
    return &other;
  }
  return nullptr;
}

// One block of prepare-time scratch: the orientation of the current q node, the
// retained solve state, one AVX2 pack's four node slots and its hoisted sweep geometry
// (heap: ~0.4 MB would crowd a worker's stack). The pack workspaces and the hoist table
// are default-initialised on purpose: like the put batch driver's stack
// `AlWorkspace ws[4]`, the kernel's init writes every workspace field it reads (table
// pointers, counts, flags) and never touches the large static-bind arrays, and the
// hoist writes every table entry before the sweeps read it, so zero-filling ~0.3 MB per
// prepare would buy nothing (allocated with make_unique_for_overwrite).
struct PrepareScratch {
  amer::AlOrientedGeometry geo{};
  amer::AlPreparedScratch solve{};
  int geo_node = -1; // the q node `geo` is prepared for, -1 = none yet
  amer::AlBoundary pack_bnd[4]{};
  amer::AlWorkspace pack_ws[4];
  simd::detail::NodePackScratch pack_geo;
};

// THE BUILD's route (american.hpp): AVX2 packs iff the host / the SimdIsa override allow
// it and the scheme is the one the AVX2 kernel implements (Reference arithmetic, the
// Barone-Adesi-Whaley seed); the scalar prepared solve otherwise.
[[nodiscard]] bool batched_build(const amer::AlScheme &sch) noexcept {
  return simd::use_avx2() && sch.profile == AlProfile::Reference &&
         sch.seed == detail::AlSeedMode::Baw;
}

// The node solves' inputs (names no State: the anonymous namespace cannot).
struct NodePlan {
  const AlExpiryGeometry::Impl *g = nullptr;
  Side side = Side::Put;
  double anchor = 0.0;
  const double *sig = nullptr; // σ node values
  unsigned n_sigma = 0;
  const double *qn = nullptr; // q node values
  unsigned nq = 0;
  double *series = nullptr; // (k * nq + j) * n_sigma + s
};

// Node (j, s)'s solved boundary `b` into the series; `grid` receives the node structure
// ((n, T)-only: z, wbary, x, tau — y / K / xmax are rewritten per price) from the first
// node stored, whichever route solved it.
void store_node(const NodePlan &p, unsigned j, unsigned s, const amer::AlBoundary &b,
                amer::AlBoundary &grid, bool &have_grid) noexcept {
  if (!have_grid) {
    grid = b;
    have_grid = true;
  }
  assert(b.n == grid.n);
  for (unsigned k = 0; k < b.n; ++k) {
    p.series[(static_cast<std::size_t>(k) * p.nq + j) * p.n_sigma + s] = b.y[k];
  }
}

// Node (j, s) on the scalar prepared solve (lazily preparing q node j's orientation);
// its boundary into the series.
[[nodiscard]] Status solve_node_scalar(const NodePlan &p, unsigned j, unsigned s,
                                       PrepareScratch &w, amer::AlBoundary &grid,
                                       bool &have_grid) {
  if (w.geo_node != static_cast<int>(j)) {
    const detail::InternalPutRates rq = detail::internal_put_rates(p.side, p.g->r, p.qn[j]);
    amer::al_prepare_oriented_geometry(p.g->T, rq.rp, rq.qp, p.g->sch, w.geo);
    if (w.geo.id == 0 || !w.geo.tables_ok) {
      return node_solve_error(amer::AlSolveStatus::TableMissing);
    }
    w.geo_node = static_cast<int>(j);
  }
  amer::AlPreparedScratch &sc = w.solve;
  const amer::AlSolveStatus stv = amer::al_solve_put_boundary_prepared(w.geo, p.anchor, p.sig[s],
                                                                       sc.bnd, sc.ws, &sc.loaded);
  if (stv != amer::AlSolveStatus::Ok) {
    return node_solve_error(stv);
  }
  store_node(p, j, s, sc.bnd, grid, have_grid);
  return Ok();
}

// Nodes t0 .. t0 + 3 (t = j * n_sigma + s) through one AVX2 pack at the anchor; a lane
// the kernel declines goes through the scalar prepared solve, which reports its status.
[[nodiscard]] Status solve_pack_avx2(const NodePlan &p, unsigned t0, PrepareScratch &w,
                                     amer::AlBoundary &grid, bool &have_grid) {
  double K4[4] = {};
  double s4[4] = {};
  double r4[4] = {};
  double q4[4] = {};
  for (unsigned l = 0; l < 4; ++l) {
    const unsigned t = t0 + l;
    const detail::InternalPutRates rq =
        detail::internal_put_rates(p.side, p.g->r, p.qn[t / p.n_sigma]);
    K4[l] = p.anchor;
    s4[l] = p.sig[t % p.n_sigma];
    r4[l] = rq.rp;
    q4[l] = rq.qp;
  }
  const unsigned mask = simd::detail::solve_put_boundaries4_avx2(K4, p.g->T, s4, r4, q4, p.g->sch,
                                                                 w.pack_bnd, w.pack_ws, w.pack_geo);
  for (unsigned l = 0; l < 4; ++l) {
    const unsigned t = t0 + l;
    const unsigned j = t / p.n_sigma;
    const unsigned s = t % p.n_sigma;
    if ((mask & (1u << l)) != 0u) {
      store_node(p, j, s, w.pack_bnd[l], grid, have_grid);
    } else if (Status ns = solve_node_scalar(p, j, s, w, grid, have_grid); !ns) {
      return ns;
    }
  }
  return Ok();
}

} // namespace

AlSigmaQFamilyStatus AlSigmaQFamily::State::admit_point(double S, double sigma,
                                                        double q) const noexcept {
  if (!ok) {
    return AlSigmaQFamilyStatus::NotPrepared;
  }
  if (g->generation != generation) {
    return AlSigmaQFamilyStatus::GeometryRePrepared;
  }
  if (!(std::isfinite(S) && S > 0.0 && std::isfinite(sigma) && std::isfinite(q))) {
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

AlSigmaQFamilyStatus AlSigmaQFamily::State::admit(double S, double K, double sigma,
                                                  double q) const noexcept {
  const AlSigmaQFamilyStatus a = admit_point(S, sigma, q);
  if (a == AlSigmaQFamilyStatus::NotPrepared || a == AlSigmaQFamilyStatus::GeometryRePrepared) {
    return a;
  }
  // The strike's check sits with S / sigma / q's, ahead of the σ guard and the boxes.
  if (!(std::isfinite(K) && K > 0.0)) {
    return AlSigmaQFamilyStatus::InvalidArgument;
  }
  return a;
}

void AlSigmaQFamily::State::interpolate(double sigma, double q,
                                        amer::AlBoundary &bnd) const noexcept {
  const double z = clamp_unit(2.0 * (axis_of(axis, sigma) - u_lo) / (u_hi - u_lo) - 1.0);
  const BaryCoef cs = bary_coef(sz, sw, n_sigma, z);
  if (n_q == 1) {
    // Collapsed q axis: exactly AlSigmaFamily's per-node σ evaluation.
    for (unsigned k = 0; k < n_boundary; ++k) {
      bnd.y[k] = bary_apply(cs, &series[static_cast<std::size_t>(k) * n_sigma]);
    }
    return;
  }
  // Tensor 2nd-kind barycentric, factored: y_k = sum_j wq_j (sum_s ws_s y_kjs) /
  // (den_q den_s). Zero-filled weight vectors (house style §3; the loops read the first
  // n_sigma / n_q only).
  const BaryCoef cq = bary_coef(qz, qw, n_q, clamp_unit((q - q_lo) * q_scale - 1.0));
  double ws_s[kMaxSigmaNodes] = {};
  double wq_j[kMaxQNodes] = {};
  double den_s = 1.0;
  double den_q = 1.0;
  bary_weights(cs, ws_s, den_s);
  bary_weights(cq, wq_j, den_q);
  const std::size_t nt = static_cast<std::size_t>(n_q) * n_sigma;
  const double inv = 1.0 / (den_q * den_s);
  for (unsigned k = 0; k < n_boundary; ++k) {
    const double *yk = &series[static_cast<std::size_t>(k) * nt];
    double acc = 0.0;
    for (unsigned j = 0; j < n_q; ++j) {
      const double *ykj = yk + static_cast<std::size_t>(j) * n_sigma;
      double inner = 0.0;
      for (unsigned s = 0; s < n_sigma; ++s) {
        inner += ws_s[s] * ykj[s];
      }
      acc += wq_j[j] * inner;
    }
    bnd.y[k] = acc * inv;
  }
}

double AlSigmaQFamily::State::price_on(amer::AlBoundary &bnd, double S, double K, double sigma,
                                       double q) const noexcept {
  const detail::InternalPutCoords c = detail::internal_put_coords(side, S, K);
  const detail::InternalPutRates carry = detail::internal_put_rates(side, rate, q);
  bnd.K = c.Kp;
  bnd.xmax = amer::al_xmax_put(c.Kp, carry.rp, carry.qp);
  return amer::al_put_price_from_boundary(bnd, *ws, c.Sp, c.Kp, grid.T, sigma, carry.rp,
                                          carry.qp);
}

double AlSigmaQFamily::State::price_at(double S, double K, double sigma,
                                       double q) const noexcept {
  amer::AlBoundary bnd = grid; // stack scratch: the only per-call state
  interpolate(sigma, q, bnd);
  return price_on(bnd, S, K, sigma, q);
}

AlSigmaQFamilySelfCheck
AlSigmaQFamily::State::measure_self_check(amer::AlOrientedGeometry &geo,
                                          amer::AlPreparedScratch &sc) const noexcept {
  // Check points: the midpoints of the first and the central cell of each axis.
  const unsigned sig_cells[2] = {0u, (n_sigma - 1u) / 2u};
  const double q_half = 0.5 * (q_hi - q_lo);
  double q_pts[2] = {0.5 * (q_lo + q_hi), 0.5 * (q_lo + q_hi)}; // a collapsed box: its node
  if (n_q > 1) {
    const unsigned q_cells[2] = {0u, (n_q - 1u) / 2u};
    for (unsigned i = 0; i < 2; ++i) {
      const unsigned c = q_cells[i];
      q_pts[i] = q_lo + q_half * (0.5 * (qz[c] + qz[c + 1]) + 1.0);
    }
  }
  const double half_span = 0.5 * (u_hi - u_lo);
  double max_abs = 0.0;
  double max_rel = 0.0;
  unsigned prices = 0;
  for (const double qc : q_pts) {
    const detail::InternalPutRates rq = detail::internal_put_rates(side, rate, qc);
    amer::al_prepare_oriented_geometry(grid.T, rq.rp, rq.qp, g->sch, geo);
    if (geo.id == 0 || !geo.tables_ok) {
      continue;
    }
    for (const unsigned s : sig_cells) {
      const double u = u_lo + half_span * (0.5 * (sz[s] + sz[s + 1]) + 1.0);
      const double sig = (axis == AlSigmaFamilyAxis::LogSigma) ? std::exp(u) : u;
      if (amer::al_solve_put_boundary_prepared(geo, anchor, sig, sc.bnd, sc.ws, &sc.loaded) !=
          amer::AlSolveStatus::Ok) {
        continue;
      }
      amer::AlBoundary fam = grid;
      interpolate(sig, qc, fam);
      fam.K = anchor;
      fam.xmax = amer::al_xmax_put(anchor, rq.rp, rq.qp);
      for (const double m : {0.8, 1.0, 1.25}) {
        const double Sp = m * anchor;
        const double cold = amer::al_put_price_from_boundary(sc.bnd, sc.ws, Sp, anchor, grid.T,
                                                             sig, rq.rp, rq.qp);
        const double got = amer::al_put_price_from_boundary(fam, *ws, Sp, anchor, grid.T, sig,
                                                            rq.rp, rq.qp);
        const double d = std::abs(got - cold);
        max_abs = std::max(max_abs, d / anchor);
        if (cold >= 1.0e-4 * anchor) {
          max_rel = std::max(max_rel, d / cold);
        }
        ++prices;
      }
    }
  }
  AlSigmaQFamilySelfCheck c;
  if (prices > 0) {
    c.max_abs_per_strike = max_abs;
    c.max_rel = max_rel;
    c.prices = static_cast<std::uint16_t>(prices);
    c.status = AlSigmaQFamilyStatus::Ok;
  }
  return c;
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

std::size_t AlSigmaQFamily::state_bytes() const noexcept {
  return st_ ? sizeof(State) + st_->series_size * sizeof(double) : 0u;
}

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
  if (quadrature_orientation(*g, side) == nullptr) {
    return Err(ErrorCode::NotImplemented,
               "AlSigmaQFamily: the geometry has no American orientation on either side "
               "(no premium quadrature to price with)");
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
  if (!degenerate &&
      std::pow(q_axis_rho(g->r, q_lo, q_hi), -static_cast<double>(n_q)) >
          kAlSigmaQFamilyMaxQAxisEstimate) {
    return Err(ErrorCode::NotImplemented,
               "AlSigmaQFamily: the q box is too wide for its distance to q = r (accuracy "
               "guard rho^-n_q > kAlSigmaQFamilyMaxQAxisEstimate); split it or raise n_q");
  }
  return Ok();
}

} // namespace

Status AlSigmaQFamily::prepare(double sigma_lo, double sigma_hi, std::uint16_t n_sigma,
                               double q_lo, double q_hi, std::uint16_t n_q,
                               AlSigmaFamilyAxis axis, AlSigmaQFamilyCheck check) {
  if (!st_) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaQFamily: moved-from family");
  }
  State &st = *st_;
  st.ok = false; // a refusal below leaves the family unprepared, never half-built
  st.check = AlSigmaQFamilySelfCheck{};
  // A refusal also releases the previous node tensor (state_bytes() then reports the
  // bare state block); a success keeps it for a re-prepare of the same size.
  struct DiscardTensorOnRefusal {
    explicit DiscardTensorOnRefusal(State &s) noexcept : state(s) {}
    DiscardTensorOnRefusal(const DiscardTensorOnRefusal &) = delete;
    DiscardTensorOnRefusal &operator=(const DiscardTensorOnRefusal &) = delete;
    ~DiscardTensorOnRefusal() {
      if (!keep) {
        state.series.reset();
        state.series_size = 0;
      }
    }
    State &state;
    bool keep = false;
  };
  DiscardTensorOnRefusal discard(st);
  if (Status in = check_prepare_inputs(st.g, st.side, st.anchor, sigma_lo, sigma_hi, n_sigma,
                                       q_lo, q_hi, n_q);
      !in) {
    return in;
  }
  const AlExpiryGeometry::Impl &g = *st.g;
  const bool degenerate = (q_hi - q_lo) <= kAlSigmaQFamilyDegenerateWidth;
  const std::uint16_t nq = degenerate ? std::uint16_t{1} : n_q;
  lay_lobatto_axis(n_sigma, kMaxSigmaNodes, st.sz, st.sw);
  lay_lobatto_axis(nq, kMaxQNodes, st.qz, st.qw);
  const double u_lo = axis_of(axis, sigma_lo);
  const double u_hi = axis_of(axis, sigma_hi);

  // Node values: σ on the axis coordinate, q linear with the box ends laid down exactly
  // (a price at q_hi hits the last node).
  double sig[kMaxSigmaNodes] = {};
  for (unsigned s = 0; s < n_sigma; ++s) {
    const double u = u_lo + 0.5 * (u_hi - u_lo) * (st.sz[s] + 1.0);
    sig[s] = (axis == AlSigmaFamilyAxis::LogSigma) ? std::exp(u) : u;
  }
  double qn[kMaxQNodes] = {};
  for (unsigned j = 0; j < nq; ++j) {
    qn[j] = q_lo + 0.5 * (q_hi - q_lo) * (st.qz[j] + 1.0);
    if (degenerate) {
      qn[j] = 0.5 * (q_lo + q_hi);
    } else if (j == 0) {
      qn[j] = q_lo;
    } else if (j + 1 == nq) {
      qn[j] = q_hi;
    }
    const detail::InternalPutRates rq = detail::internal_put_rates(st.side, g.r, qn[j]);
    if (!(amer::al_xmax_put(st.anchor, rq.rp, rq.qp) > 0.0)) {
      return Err(ErrorCode::NotImplemented,
                 "AlSigmaQFamily: asymptotic boundary collapsed (xmax <= 0) at the anchor");
    }
  }

  // n_q x n_sigma solves at the anchor, at the geometry's AlOpts: four at a time through
  // the AVX2 kernel when batched_build(), the remainder (< 4) on the scalar solve.
  const std::unique_ptr<PrepareScratch> work = std::make_unique_for_overwrite<PrepareScratch>();
  const std::size_t tensor = static_cast<std::size_t>(g.sch.n_boundary) * nq * n_sigma;
  if (st.series_size != tensor) {
    st.series = std::make_unique<double[]>(tensor); // exact (L2h-c); every slot is written
    st.series_size = tensor;
  }
  const NodePlan plan{st.g, st.side, st.anchor, sig, n_sigma, qn, nq, st.series.get()};
  bool have_grid = false;
  const unsigned n_nodes = static_cast<unsigned>(nq) * n_sigma;
  unsigned t = 0;
  if (batched_build(g.sch)) {
    for (; t + 4u <= n_nodes; t += 4u) {
      if (Status ns = solve_pack_avx2(plan, t, *work, st.grid, have_grid); !ns) {
        return ns;
      }
    }
  }
  for (; t < n_nodes; ++t) {
    if (Status ns = solve_node_scalar(plan, t / n_sigma, t % n_sigma, *work, st.grid, have_grid);
        !ns) {
      return ns;
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
  st.ws = &quadrature_orientation(g, st.side)->ws;
  assert(st.grid.n == g.sch.n_boundary); // the tensor's n_boundary is the grid's
  if (check == AlSigmaQFamilyCheck::OffNode) {
    st.check = st.measure_self_check(work->geo, work->solve);
  }
  st.ok = true;
  discard.keep = true;
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

AlSigmaQFamilyStatus AlSigmaQFamily::price_strikes(double S, std::span<const double> K,
                                                   double sigma, double q,
                                                   std::span<double> out) const noexcept {
  if (K.size() != out.size()) {
    return AlSigmaQFamilyStatus::InvalidArgument;
  }
  const State *st = st_.get();
  const AlSigmaQFamilyStatus a =
      (st == nullptr) ? AlSigmaQFamilyStatus::NotPrepared : st->admit_point(S, sigma, q);
  if (a != AlSigmaQFamilyStatus::Ok) {
    for (double &o : out) {
      o = kNaN;
    }
    return a;
  }
  amer::AlBoundary bnd = st->grid; // one interpolation serves every strike
  st->interpolate(sigma, q, bnd);
  AlSigmaQFamilyStatus result = AlSigmaQFamilyStatus::Ok;
  for (std::size_t i = 0; i < K.size(); ++i) {
    if (!(std::isfinite(K[i]) && K[i] > 0.0)) {
      out[i] = kNaN;
      result = AlSigmaQFamilyStatus::InvalidArgument;
      continue;
    }
    out[i] = st->price_on(bnd, S, K[i], sigma, q);
  }
  return result;
}

AlSigmaQFamilySelfCheck AlSigmaQFamily::self_check() const noexcept {
  const State *st = st_.get();
  if (st == nullptr || !st->ok) {
    return AlSigmaQFamilySelfCheck{};
  }
  if (st->g->generation != st->generation) {
    AlSigmaQFamilySelfCheck c;
    c.status = AlSigmaQFamilyStatus::GeometryRePrepared;
    return c;
  }
  return st->check;
}

// ── K1 (r5, 2026-10-22): fused price + analytic vega (contract in the internal header) ──

namespace {

using sigma_family_detail::diff_tables;
using sigma_family_detail::DiffTables;
using sigma_q_family_detail::FusedPriceVega;
using sigma_q_family_detail::FusedStatus;
using sigma_q_family_detail::kMaxPremiumNodes;
using sigma_q_family_detail::NodeBoundary;
using sigma_q_family_detail::PremiumNodeKind;
using sigma_q_family_detail::PremiumNodes;
using sigma_q_family_detail::PremiumTable;
using sigma_q_family_detail::QCarry;
using sigma_q_family_detail::QTile;

// The normalised barycentric basis b_s(zq) on nodes z[] / weights w[] (n <= kAlMaxNodes):
// b_s = (w_s / (zq − z_s)) / Σ_t (w_t / (zq − z_t)); an exact node hit is the unit vector.
void bary_basis(const double *z, const double *w, unsigned n, double zq, double *b) noexcept {
  double qq[amer::kAlMaxNodes] = {};
  double den = 0.0;
  for (unsigned s = 0; s < n; ++s) {
    const double dz = zq - z[s];
    if (dz == 0.0) {
      for (unsigned t = 0; t < n; ++t) {
        b[t] = (t == s) ? 1.0 : 0.0;
      }
      return;
    }
    qq[s] = w[s] / dz;
    den += qq[s];
  }
  const double inv = 1.0 / den;
  for (unsigned s = 0; s < n; ++s) {
    b[s] = qq[s] * inv;
  }
}

// c_t = Σ_s b_s D1[s][t] — the basis's z-derivative through the nodal differentiation
// matrix (the L3g form: no quotient rule, so no near-node cancellation and no 0/0 at a
// hit). Precondition: 2 <= n <= kMaxSigmaNodes.
void bary_basis_derivative(const double *b, unsigned n, double *c) noexcept {
  const DiffTables &dt = diff_tables(n);
  for (unsigned t = 0; t < n; ++t) {
    c[t] = 0.0;
  }
  for (unsigned s = 0; s < n; ++s) {
    const double bs = b[s];
    for (unsigned t = 0; t < n; ++t) {
      c[t] += bs * dt.d1[s][t];
    }
  }
}

// σ -> the family z coordinate (clamped at the box edges for rounding only, as
// interpolate() clamps) and dz/dσ (the unclamped chain).
struct SigmaZ {
  double z = 0.0;
  double dz = 0.0;
};

[[nodiscard]] SigmaZ sigma_z(AlSigmaFamilyAxis axis, double u_lo, double u_hi,
                             double sigma) noexcept {
  const double span = u_hi - u_lo;
  SigmaZ a;
  a.z = clamp_unit(2.0 * (axis_of(axis, sigma) - u_lo) / span - 1.0);
  a.dz = (axis == AlSigmaFamilyAxis::LogSigma) ? (2.0 / span) / sigma : 2.0 / span;
  return a;
}

// The τ barycentric at every Boundary premium node — the al_cheb_eval loop (num / den, an
// exact hit returns the node value) — applied to y and dy/dσ on the same weights.
void tau_interpolate(const double *z, const double *w, unsigned nb, const PremiumNodes &pn,
                     const double *yk, const double *dk, double *y, double *dy) noexcept {
  for (unsigned i = 0; i < pn.n; ++i) {
    if (pn.kind[i] != PremiumNodeKind::Boundary) {
      y[i] = 0.0;
      dy[i] = 0.0;
      continue;
    }
    const double zc = pn.zc[i];
    double num = 0.0;
    double num1 = 0.0;
    double den = 0.0;
    int hit = -1;
    for (unsigned k = 0; k < nb; ++k) {
      const double dz = zc - z[k];
      if (dz == 0.0) {
        hit = static_cast<int>(k);
        break;
      }
      const double qq = w[k] / dz;
      num += qq * yk[k];
      num1 += qq * dk[k];
      den += qq;
    }
    if (hit >= 0) {
      y[i] = yk[static_cast<unsigned>(hit)];
      dy[i] = dk[static_cast<unsigned>(hit)];
    } else {
      y[i] = num / den;
      dy[i] = num1 / den;
    }
  }
}

template <bool kFast> [[nodiscard]] double cdf_of(double x) noexcept {
  if constexpr (kFast) {
    return detail::norm_cdf_erfc(x);
  } else {
    return atx::core::norm_cdf(x);
  }
}

template <bool kFast> [[nodiscard]] double pdf_of(double x) noexcept {
  if constexpr (kFast) {
    return detail::norm_pdf_cody(x);
  } else {
    return atx::core::norm_pdf(x);
  }
}

// The European put leg and its vega: the Reference arm is black76_value_and_vega (the
// black76_price expressions, so the euro_put_sk bits); the Fast arm is black76_put_fast
// term for term with the Cody Φ.
struct EuroLeg {
  double price = 0.0;
  double vega = 0.0;
};

template <bool kFast>
[[nodiscard]] EuroLeg euro_leg(const PremiumNodes &pn, double Sp, double Kp, double sigma,
                               double rp, double qp) noexcept {
  const double F = Sp * std::exp((rp - qp) * pn.T);
  const double df = std::exp(-rp * pn.T);
  if constexpr (kFast) {
    const double v = sigma * pn.sqrtT;
    const double d1 = (std::log(F / Kp) + 0.5 * v * v) / v;
    const double d2 = d1 - v;
    return EuroLeg{df * (Kp * cdf_of<true>(-d2) - F * cdf_of<true>(-d1)),
                   F * df * pdf_of<true>(d1) * pn.sqrtT};
  } else {
    const Black76ValueVega e =
        black76_value_and_vega(F, Kp, pn.T, sigma, df, Side::Put, pn.sqrtT);
    return EuroLeg{e.price, e.vega};
  }
}

// One premium pass over the nodes: the price accumulation (log-form boundary) and, with
// kVega, the σ-derivative accumulator of the same integrand — explicit terms (∂d±/∂σ =
// −d∓/σ) plus the boundary motion (∂d±/∂s = 1/v, s' = nb.ds).
template <bool kFast, bool kVega>
[[nodiscard]] FusedPriceVega premium_pass_t(const PremiumNodes &pn, const QCarry &c,
                                            const NodeBoundary &nb, double Sp,
                                            double Kp) noexcept {
  const double sigma = nb.sigma;
  const double rp = c.rp;
  const double qp = c.qp;
  const double xmax = amer::al_xmax_put(Kp, rp, qp);
  const double lsx = std::log(Sp / xmax);
  const double drift = rp - qp;
  double total = 0.0;
  double asig = 0.0;
  for (unsigned i = 0; i < pn.n; ++i) {
    if (nb.skip[i]) {
      continue;
    }
    const double ld = (pn.kind[i] == PremiumNodeKind::Boundary)
                          ? lsx + nb.s[i] + drift * pn.t[i]
                          : std::log(Sp * c.dq[i] / (Kp * c.dr[i])); // B = K (rem <= 0)
    const double v = nb.v[i];
    const double dp = ld / v + 0.5 * v;
    const double dm = dp - v;
    const double a = rp * Kp * c.dr[i];
    const double b = qp * Sp * c.dq[i];
    total += pn.wz2[i] * (a * cdf_of<kFast>(-dm) - b * cdf_of<kFast>(-dp));
    if constexpr (kVega) {
      const double phm = pdf_of<kFast>(dm);
      const double php = pdf_of<kFast>(dp);
      asig += pn.wz2[i] *
              ((a * dp * phm - b * dm * php) / sigma - (a * phm - b * php) * nb.ds[i] / v);
    }
  }
  total *= pn.half_sqrtT;
  const bool floored = !(total > 0.0);
  const double prem = floored ? 0.0 : total;
  const EuroLeg euro = euro_leg<kFast>(pn, Sp, Kp, sigma, rp, qp);
  // The al_put_price_from_boundary clamp order.
  double price = euro.price + prem;
  const double intr = Kp - Sp;
  bool clamped = false;
  if (intr > price) {
    price = intr;
    clamped = true;
  }
  if (euro.price > price) {
    price = euro.price; // unreachable with prem >= 0; mirrored
  }
  if (price < 0.0) {
    price = 0.0;
    clamped = true;
  }
  FusedPriceVega out;
  out.price = price;
  out.reason = AlSigmaQFamilyStatus::Ok;
  out.status = FusedStatus::Ok;
  out.exercised = clamped || (intr > 0.0 && price <= intr + 1.0e-9 * Kp) ||
                  Sp <= xmax * nb.now_factor;
  if constexpr (kVega) {
    if (clamped) {
      out.vega = 0.0; // the clamp serves intrinsic: flat in σ
    } else if (nb.degenerate) {
      out.status = FusedStatus::NearNodeDegenerate; // vega stays NaN
    } else {
      out.vega = euro.vega + (floored ? 0.0 : pn.half_sqrtT * asig);
    }
  }
  return out;
}

[[nodiscard]] FusedPriceVega fused_refusal(AlSigmaQFamilyStatus why) noexcept {
  FusedPriceVega r;
  r.reason = why;
  r.status = sigma_q_family_detail::fused_status_of(why);
  return r;
}

[[nodiscard]] FusedPriceVega fused_unsupported(AlSigmaQFamilyStatus why) noexcept {
  FusedPriceVega r;
  r.reason = why;
  r.status = FusedStatus::Unsupported;
  return r;
}

// From a table tile at sigma to the strike-free NodeBoundary.
void boundary_from_tile(const PremiumTable &t, const QTile &tile, double sigma,
                        NodeBoundary &out) noexcept {
  const SigmaZ az = sigma_z(t.axis, t.u_lo, t.u_hi, sigma);
  double bs[kMaxSigmaNodes] = {};
  double cs[kMaxSigmaNodes] = {};
  bary_basis(t.sz, t.sw, t.n_sigma, az.z, bs);
  bary_basis_derivative(bs, t.n_sigma, cs);
  const unsigned ns = t.n_sigma;
  double y[kMaxPremiumNodes] = {};
  double dy[kMaxPremiumNodes] = {};
  double y_now = 0.0;
  // The τ interpolation is folded into g: two n_sigma-dots per premium node.
  const unsigned n = t.nodes.n;
  for (unsigned i = 0; i <= n; ++i) {
    const double *row = &tile.g[static_cast<std::size_t>(i) * ns];
    double a = 0.0;
    double d = 0.0;
    for (unsigned s = 0; s < ns; ++s) {
      a += bs[s] * row[s];
      d += cs[s] * row[s];
    }
    if (i < n) {
      y[i] = a;
      dy[i] = d * az.dz;
    } else {
      y_now = a;
    }
  }
  sigma_q_family_detail::finish_node_boundary(t.nodes, y, dy, y_now, sigma, out);
}

// Process-unique table stamps (a tile is valid for exactly the build it was cut from).
std::atomic<std::uint64_t> g_table_stamp{0};

} // namespace

bool AlSigmaQFamily::State::interpolate_derivs(double sigma, double q, double *y,
                                               double *dy_dsigma, double *dy_dq) const noexcept {
  const SigmaZ az = sigma_z(axis, u_lo, u_hi, sigma);
  double bs[kMaxSigmaNodes] = {};
  double cs[kMaxSigmaNodes] = {};
  bary_basis(sz, sw, n_sigma, az.z, bs);
  bary_basis_derivative(bs, n_sigma, cs);
  if (n_q == 1) {
    for (unsigned k = 0; k < n_boundary; ++k) {
      const double *row = &series[static_cast<std::size_t>(k) * n_sigma];
      double a = 0.0;
      double d = 0.0;
      for (unsigned s = 0; s < n_sigma; ++s) {
        a += bs[s] * row[s];
        d += cs[s] * row[s];
      }
      y[k] = a;
      dy_dsigma[k] = d * az.dz;
    }
    return false; // a collapsed box has no q coordinate
  }
  double bq[kMaxQNodes] = {};
  double cq[kMaxQNodes] = {};
  bary_basis(qz, qw, n_q, clamp_unit((q - q_lo) * q_scale - 1.0), bq);
  const bool want_q = dy_dq != nullptr;
  if (want_q) {
    bary_basis_derivative(bq, n_q, cq);
  }
  const std::size_t nt = static_cast<std::size_t>(n_q) * n_sigma;
  for (unsigned k = 0; k < n_boundary; ++k) {
    const double *yk = &series[static_cast<std::size_t>(k) * nt];
    // q contracted first (with its q-derivative), then the σ basis and its derivative.
    double yt[kMaxSigmaNodes] = {};
    double yqt[kMaxSigmaNodes] = {};
    for (unsigned j = 0; j < n_q; ++j) {
      const double *row = yk + static_cast<std::size_t>(j) * n_sigma;
      const double wj = bq[j];
      const double vj = cq[j];
      for (unsigned s = 0; s < n_sigma; ++s) {
        yt[s] += wj * row[s];
        yqt[s] += vj * row[s];
      }
    }
    double a = 0.0;
    double d = 0.0;
    double e = 0.0;
    for (unsigned s = 0; s < n_sigma; ++s) {
      a += bs[s] * yt[s];
      d += cs[s] * yt[s];
      e += bs[s] * yqt[s];
    }
    y[k] = a;
    dy_dsigma[k] = d * az.dz;
    if (want_q) {
      dy_dq[k] = e * q_scale;
    }
  }
  return want_q;
}

bool AlSigmaQFamily::State::lay_premium_nodes(
    sigma_q_family_detail::PremiumNodes &pn) const noexcept {
  const unsigned n = ws->n_quad_price;
  if (n > kMaxPremiumNodes || ws->qx_price == nullptr || ws->qw_price == nullptr) {
    pn.n = 0;
    return false;
  }
  const double T = grid.T;
  pn.n = n;
  pn.T = T;
  pn.sqrtT = std::sqrt(T);
  pn.half_sqrtT = 0.5 * pn.sqrtT;
  for (unsigned i = 0; i < n; ++i) {
    // The premium_integrand_put node: z = ½√T (1 + x_i), t = z², rem = T − t.
    const double zi = pn.half_sqrtT * (1.0 + ws->qx_price[i]);
    const double t = zi * zi;
    pn.t[i] = t;
    pn.sqrt_t[i] = std::sqrt(t);
    pn.wz2[i] = 2.0 * zi * ws->qw_price[i];
    pn.zc[i] = 0.0;
    if (t <= 1.0e-14) {
      pn.kind[i] = PremiumNodeKind::Dead;
      continue;
    }
    const double rem = T - t;
    if (rem > 0.0) {
      // The al_boundary_at τ coordinate, expression for expression.
      const double u_eff = (rem >= T) ? T : rem;
      pn.zc[i] = clamp_unit(2.0 * std::sqrt(u_eff / T) - 1.0);
      pn.kind[i] = PremiumNodeKind::Boundary;
    } else {
      pn.kind[i] = PremiumNodeKind::Strike;
    }
  }
  return true;
}

void AlSigmaQFamily::State::boundary_at_nodes(
    const sigma_q_family_detail::PremiumNodes &pn, double sigma, double q,
    sigma_q_family_detail::NodeBoundary &out) const noexcept {
  double yk[amer::kAlMaxNodes] = {};
  double dk[amer::kAlMaxNodes] = {};
  (void)interpolate_derivs(sigma, q, yk, dk, nullptr);
  double y[kMaxPremiumNodes] = {};
  double dy[kMaxPremiumNodes] = {};
  tau_interpolate(grid.z.data(), grid.wbary.data(), n_boundary, pn, yk, dk, y, dy);
  sigma_q_family_detail::finish_node_boundary(pn, y, dy, yk[n_boundary - 1u], sigma, out);
}

namespace sigma_q_family_detail {

FusedStatus fused_status_of(AlSigmaQFamilyStatus a) noexcept {
  switch (a) {
  case AlSigmaQFamilyStatus::Ok:
    return FusedStatus::Ok;
  case AlSigmaQFamilyStatus::SigmaBelowGuard:
  case AlSigmaQFamilyStatus::SigmaOutsideBox:
  case AlSigmaQFamilyStatus::YieldOutsideBox:
    return FusedStatus::OutsideBox;
  case AlSigmaQFamilyStatus::NotPrepared:
  case AlSigmaQFamilyStatus::GeometryRePrepared:
  case AlSigmaQFamilyStatus::InvalidArgument:
    return FusedStatus::Unsupported;
  }
  return FusedStatus::Unsupported;
}

void carry_at(const PremiumNodes &pn, Side side, double r, double q, QCarry &out) noexcept {
  const detail::InternalPutRates rq = detail::internal_put_rates(side, r, q);
  out.q = q;
  out.rp = rq.rp;
  out.qp = rq.qp;
  for (unsigned i = 0; i < pn.n; ++i) {
    if (pn.kind[i] == PremiumNodeKind::Dead) {
      out.dq[i] = 0.0;
      out.dr[i] = 0.0;
      continue;
    }
    out.dq[i] = std::exp(-rq.qp * pn.t[i]);
    out.dr[i] = std::exp(-rq.rp * pn.t[i]);
  }
}

void finish_node_boundary(const PremiumNodes &pn, const double *y, const double *dy, double y_now,
                          double sigma, NodeBoundary &out) noexcept {
  out.sigma = sigma;
  double ymax = std::abs(y_now);
  for (unsigned i = 0; i < pn.n; ++i) {
    if (pn.kind[i] == PremiumNodeKind::Boundary) {
      ymax = std::max(ymax, std::abs(y[i]));
    }
  }
  const double kink = kFusedKinkRel * ymax;
  bool degenerate = false;
  for (unsigned i = 0; i < pn.n; ++i) {
    out.v[i] = sigma * pn.sqrt_t[i];
    out.s[i] = 0.0;
    out.ds[i] = 0.0;
    switch (pn.kind[i]) {
    case PremiumNodeKind::Dead:
      out.skip[i] = true;
      break;
    case PremiumNodeKind::Strike:
      out.skip[i] = false;
      break;
    case PremiumNodeKind::Boundary: {
      const double yi = y[i];
      const double s = std::sqrt((yi > 0.0) ? yi : 0.0);
      out.s[i] = s;
      // A boundary that underflows contributes nothing (both kernels skip it).
      out.skip[i] = !(s < 745.0);
      out.ds[i] = (yi > 0.0) ? dy[i] / (2.0 * s) : 0.0;
      if (std::abs(yi) <= kink && dy[i] != 0.0) {
        degenerate = true;
      }
      break;
    }
    }
  }
  out.now_factor = std::exp(-std::sqrt((y_now > 0.0) ? y_now : 0.0));
  out.degenerate = degenerate;
}

FusedPriceVega premium_pass(const PremiumNodes &pn, const QCarry &c, const NodeBoundary &nb,
                            double Sp, double Kp, bool fast, bool want_vega) noexcept {
  if (fast) {
    return want_vega ? premium_pass_t<true, true>(pn, c, nb, Sp, Kp)
                     : premium_pass_t<true, false>(pn, c, nb, Sp, Kp);
  }
  return want_vega ? premium_pass_t<false, true>(pn, c, nb, Sp, Kp)
                   : premium_pass_t<false, false>(pn, c, nb, Sp, Kp);
}

FusedPriceVega price_and_vega(const AlSigmaQFamily &f, double S, double K, double sigma,
                              double q) noexcept {
  const auto *st = QFamilyAccess::state(f);
  if (st == nullptr) {
    return fused_refusal(AlSigmaQFamilyStatus::NotPrepared);
  }
  const AlSigmaQFamilyStatus a = st->admit(S, K, sigma, q);
  if (a != AlSigmaQFamilyStatus::Ok) {
    return fused_refusal(a);
  }
  PremiumNodes pn;
  if (!st->lay_premium_nodes(pn)) {
    return fused_unsupported(AlSigmaQFamilyStatus::Ok);
  }
  QCarry c;
  carry_at(pn, st->side, st->rate, q, c);
  NodeBoundary nb;
  st->boundary_at_nodes(pn, sigma, q, nb);
  const detail::InternalPutCoords ic = detail::internal_put_coords(st->side, S, K);
  return premium_pass(pn, c, nb, ic.Sp, ic.Kp, st->ws->fast, true);
}

FusedStatus price_vega_strikes(const AlSigmaQFamily &f, double S, std::span<const double> K,
                               double sigma, double q, std::span<double> out_price,
                               std::span<double> out_vega) noexcept {
  if (K.size() != out_price.size() || K.size() != out_vega.size()) {
    return FusedStatus::Unsupported;
  }
  const auto fill_nan = [&] {
    for (std::size_t i = 0; i < K.size(); ++i) {
      out_price[i] = kNaN;
      out_vega[i] = kNaN;
    }
  };
  const auto *st = QFamilyAccess::state(f);
  const AlSigmaQFamilyStatus a =
      (st == nullptr) ? AlSigmaQFamilyStatus::NotPrepared : st->admit_point(S, sigma, q);
  if (a != AlSigmaQFamilyStatus::Ok) {
    fill_nan();
    return fused_status_of(a);
  }
  PremiumNodes pn;
  if (!st->lay_premium_nodes(pn)) {
    fill_nan();
    return FusedStatus::Unsupported;
  }
  // The strike-free work once: the node data, the carry and the boundary at the nodes.
  QCarry c;
  carry_at(pn, st->side, st->rate, q, c);
  NodeBoundary nb;
  st->boundary_at_nodes(pn, sigma, q, nb);
  const bool fast = st->ws->fast;
  FusedStatus result = FusedStatus::Ok;
  for (std::size_t i = 0; i < K.size(); ++i) {
    if (!(std::isfinite(K[i]) && K[i] > 0.0)) {
      out_price[i] = kNaN;
      out_vega[i] = kNaN;
      result = FusedStatus::Unsupported;
      continue;
    }
    const detail::InternalPutCoords ic = detail::internal_put_coords(st->side, S, K[i]);
    const FusedPriceVega r = premium_pass(pn, c, nb, ic.Sp, ic.Kp, fast, true);
    out_price[i] = r.price;
    out_vega[i] = r.vega;
    if (r.status == FusedStatus::NearNodeDegenerate && result == FusedStatus::Ok) {
      result = FusedStatus::NearNodeDegenerate;
    }
  }
  return result;
}

std::size_t PremiumTable::bytes() const noexcept {
  return sizeof(PremiumTable) + h_size * sizeof(double);
}

FusedStatus build_premium_table(const AlSigmaQFamily &f, PremiumTable &out) {
  out.ok = false;
  const auto *st = QFamilyAccess::state(f);
  if (st == nullptr || !st->ok || st->g->generation != st->generation) {
    return FusedStatus::Unsupported;
  }
  if (!st->lay_premium_nodes(out.nodes)) {
    return FusedStatus::Unsupported;
  }
  out.side = st->side;
  out.axis = st->axis;
  out.fast = st->ws->fast;
  out.sigma_lo = st->sigma_lo;
  out.sigma_hi = st->sigma_hi;
  out.u_lo = st->u_lo;
  out.u_hi = st->u_hi;
  out.q_lo = st->q_lo;
  out.q_hi = st->q_hi;
  out.q_scale = st->q_scale;
  out.rate = st->rate;
  out.n_sigma = st->n_sigma;
  out.n_q = st->n_q;
  out.n_boundary = st->n_boundary;
  std::copy(std::begin(st->sz), std::end(st->sz), std::begin(out.sz));
  std::copy(std::begin(st->sw), std::end(st->sw), std::begin(out.sw));
  std::copy(std::begin(st->qz), std::end(st->qz), std::begin(out.qz));
  std::copy(std::begin(st->qw), std::end(st->qw), std::begin(out.qw));
  const unsigned nb = st->n_boundary;
  const unsigned n = out.nodes.n;
  const std::size_t per = static_cast<std::size_t>(st->n_q) * st->n_sigma;
  const std::size_t hs = static_cast<std::size_t>(n + 1u) * per;
  if (out.h_size != hs) {
    out.h = std::make_unique<double[]>(hs);
    out.h_size = hs;
  }
  const double *series = st->series.get();
  // h row i = Σ_k ℓ_k(zc_i) · Y[k, ·, ·]; the last row is the current edge (τ = T).
  for (unsigned i = 0; i < n; ++i) {
    double *row = &out.h[static_cast<std::size_t>(i) * per];
    std::fill(row, row + per, 0.0);
    if (out.nodes.kind[i] != PremiumNodeKind::Boundary) {
      continue;
    }
    double ell[amer::kAlMaxNodes] = {};
    bary_basis(st->grid.z.data(), st->grid.wbary.data(), nb, out.nodes.zc[i], ell);
    for (unsigned k = 0; k < nb; ++k) {
      const double lk = ell[k];
      const double *yk = series + static_cast<std::size_t>(k) * per;
      for (std::size_t idx = 0; idx < per; ++idx) {
        row[idx] += lk * yk[idx];
      }
    }
  }
  const double *last = series + static_cast<std::size_t>(nb - 1u) * per;
  std::copy(last, last + per, &out.h[static_cast<std::size_t>(n) * per]);
  out.stamp = g_table_stamp.fetch_add(1u) + 1u;
  out.ok = true;
  return FusedStatus::Ok;
}

FusedStatus contract_q(const PremiumTable &t, double q, QTile &tile) noexcept {
  tile.ok = false;
  if (!t.ok || !std::isfinite(q)) {
    return FusedStatus::Unsupported;
  }
  if (q < t.q_lo || q > t.q_hi) {
    return FusedStatus::OutsideBox;
  }
  carry_at(t.nodes, t.side, t.rate, q, tile.carry);
  double bq[kMaxQNodes] = {};
  if (t.n_q == 1) {
    bq[0] = 1.0;
  } else {
    bary_basis(t.qz, t.qw, t.n_q, clamp_unit((q - t.q_lo) * t.q_scale - 1.0), bq);
  }
  const unsigned ns = t.n_sigma;
  const std::size_t per = static_cast<std::size_t>(t.n_q) * ns;
  const unsigned rows = t.nodes.n + 1u;
  const double *src = t.h.get();
  for (unsigned r = 0; r < rows; ++r) {
    double *g = &tile.g[static_cast<std::size_t>(r) * ns];
    const double *yr = src + static_cast<std::size_t>(r) * per;
    for (unsigned s = 0; s < ns; ++s) {
      g[s] = 0.0;
    }
    for (unsigned j = 0; j < t.n_q; ++j) {
      const double wj = bq[j];
      const double *row = yr + static_cast<std::size_t>(j) * ns;
      for (unsigned s = 0; s < ns; ++s) {
        g[s] += wj * row[s];
      }
    }
  }
  tile.stamp = t.stamp;
  tile.ok = true;
  return FusedStatus::Ok;
}

FusedPriceVega price_vega_on(const PremiumTable &t, const QTile &tile, double S, double K,
                             double sigma) noexcept {
  if (!t.ok || !tile.ok || tile.stamp != t.stamp) {
    return fused_unsupported(AlSigmaQFamilyStatus::NotPrepared);
  }
  if (!(std::isfinite(S) && S > 0.0 && std::isfinite(K) && K > 0.0 && std::isfinite(sigma))) {
    return fused_unsupported(AlSigmaQFamilyStatus::InvalidArgument);
  }
  if (sigma < kAlSigmaFamilyMinSigma) {
    return fused_refusal(AlSigmaQFamilyStatus::SigmaBelowGuard);
  }
  if (sigma < t.sigma_lo || sigma > t.sigma_hi) {
    return fused_refusal(AlSigmaQFamilyStatus::SigmaOutsideBox);
  }
  NodeBoundary nb;
  boundary_from_tile(t, tile, sigma, nb);
  const detail::InternalPutCoords ic = detail::internal_put_coords(t.side, S, K);
  return premium_pass(t.nodes, tile.carry, nb, ic.Sp, ic.Kp, t.fast, true);
}

} // namespace sigma_q_family_detail

} // namespace atx::vol
