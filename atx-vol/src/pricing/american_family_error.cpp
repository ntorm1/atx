// Error envelope of the (sigma, q) family (lane K3, r5 2026-10-22). Contract, units and
// the price bound's derivation: american_family_error.hpp. This TU reads the family's
// prepared node tensor through its internal accessor (QFamilyAccess) and never solves.
//
// CONSTRUCTION, per premium quadrature node i (tau_i = T - t_i, t_i the node's time):
//  1. Y_i[j][s]: node (sigma_s, q_j)'s boundary at tau_i, by the pricing path's own time
//     interpolation (bary_eval on the collocation nodes; the twin of al_cheb_eval).
//  2. The tensor's Chebyshev-Lobatto coefficients A_i[n][m] (DCT-I on each axis); column
//     sums M_m = sum_n |A[n][m]| (sigma), row sums R_n = sum_m |A[n][m]| (q).
//  3. sigma tail: pair sums E2 = M_{N-1} + M_{N-2}, E1 = M_{N-3} + M_{N-4}, E0 = M_{N-5} +
//     M_{N-6} (pairs absorb the even / odd split of the coefficients). The order seen on the
//     coarser truncation (E1 / E0) must hold on the finer one (E2 / E1): both two-step ratios
//     <= kTailRatioMax^2, and the tail beyond the nodes is extrapolated with the slower one,
//     E2 r^2 / (1 - r^2). A tail already at the noise floor is resolved (E2 itself).
//     Otherwise the family is Uncovered (SigmaOrderNotVerified).
//  4. q tail: the a-priori Bernstein ratio of the box against q = r (the family's own guard)
//     must agree with the observed last-coefficient ratio; the slower of the two
//     extrapolates R_{N-1} r / (1 - r) (QOrderNotVerified otherwise, unless resolved).
//  5. bound_y[i] = 2 (sigma tail + q tail) (Lobatto aliasing doubles a truncation tail)
//       + Lebesgue_t (Lebesgue_sigma Lebesgue_q kBuildParityY + kReferenceNoiseY)
//       + (1 + Lebesgue_sigma Lebesgue_q) 64 eps max|Y_i|.
// No solve, no allocation (stack tensors at the family's capacity).

#include "pricing/american_family_error.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>

#include "atx/core/math.hpp" // norm_cdf / norm_pdf
#include "pricing/american_boundary.hpp"
#include "pricing/american_sigma_family.hpp"   // bary_eval (the time-interpolation twin)
#include "pricing/american_sigma_q_family.hpp" // State, QFamilyAccess
#include "pricing/boundary_interp.hpp"         // internal_put_coords / internal_put_rates

namespace atx::vol {

namespace {

using sigma_family_detail::bary_eval;
using sigma_q_family_detail::QFamilyAccess;

// The family's prepared state, named through its accessor (the State type is private).
using StatePtr = decltype(QFamilyAccess::state(std::declval<const AlSigmaQFamily &>()));

constexpr unsigned kMaxS = sigma_family_detail::kMaxSigmaNodes;
constexpr unsigned kMaxQ = sigma_q_family_detail::kMaxQNodes;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kEps = std::numeric_limits<double>::epsilon();

// Per-coefficient decay ratio above which a geometric tail is not accepted (a slower
// decay than 0.75 per coefficient means the nodes do not resolve the function).
constexpr double kTailRatioMax = 0.75;
// Safety on an extrapolated tail, and the quadrature-node half-window whose worst tail each
// node takes (pass 2 of the construction).
constexpr double kTailSafety = 2.0;
constexpr unsigned kTailWindow = 2;
// The node-data and target-noise allowances (y): american_family_error.hpp.
constexpr double kBuildParityY = kFamilyBuildParityY;
constexpr double kReferenceNoiseY = kFamilyReferenceNoiseY;
// A tail whose last pair sums to less than this (relative to max|Y|, and at least the
// build parity) is resolved to noise: no order to verify.
constexpr double kNoiseFloorRel = 1.0e-13;
// Rounding budget of one family price and of the target's, relative to S + K of the
// internal put (a 48-term quadrature at unit roundoff, with margin).
constexpr double kPriceRoundingRel = 256.0 * kEps;

// Upper bound on the Lebesgue constant of n Chebyshev-Lobatto (extrema) points.
[[nodiscard]] double lobatto_lebesgue(unsigned n) noexcept {
  return n <= 2u ? 1.0 : 2.0 / std::numbers::pi * std::log(static_cast<double>(n - 1u)) + 1.0;
}

// The family's q-axis Bernstein ratio against q = r (american_sigma_q_family.cpp's guard).
[[nodiscard]] double q_axis_rho(double r, double q_lo, double q_hi) noexcept {
  const double half = 0.5 * (q_hi - q_lo);
  const double a = std::abs(0.5 * (q_lo + q_hi) - r) / half;
  return (a > 1.0) ? a + std::sqrt(a * a - 1.0) : 1.0;
}

// cos(m pi s / (n - 1)) and the DCT-I normalisation of n Lobatto points: the expansion
// coefficient of T_m is scale[m] * sum_s cosine[m][s] f_s (signs (-1)^m dropped: only
// magnitudes are used). One layout serves both axes (lay_dct(n_sigma) and lay_dct(n_q),
// n <= kMaxS and n <= kMaxQ checked before either), so its capacity must hold the q axis.
static_assert(kMaxQ <= kMaxS, "Dct is sized by kMaxS and also laid for the q axis (n_q <= kMaxQ)");
struct Dct {
  double cosine[kMaxS][kMaxS] = {};
  double edge[kMaxS] = {}; // 1/2 at s = 0 and s = n - 1, else 1
  double scale[kMaxS] = {};
  unsigned n = 0;
};

void lay_dct(unsigned n, Dct &d) noexcept {
  d.n = n;
  for (unsigned s = 0; s < n; ++s) {
    d.edge[s] = (s == 0 || s + 1 == n) ? 0.5 : 1.0;
  }
  for (unsigned m = 0; m < n; ++m) {
    d.scale[m] = n <= 1u ? 1.0 : (2.0 / static_cast<double>(n - 1u)) * d.edge[m];
    for (unsigned s = 0; s < n; ++s) {
      d.cosine[m][s] =
          n <= 1u ? 1.0
                  : std::cos(std::numbers::pi * static_cast<double>(m) * static_cast<double>(s) /
                             static_cast<double>(n - 1u));
    }
  }
}

// The time node of premium quadrature node x (al_put_premium_impl / premium_integrand_put):
// z = sqrt(T)/2 (1 + x), t = z^2, the boundary read at tau = T - t through al_boundary_at's
// transform. NaN when the integrand reads no boundary there (t <= 1e-14 or tau <= 0).
[[nodiscard]] double quad_zc(double x, double T) noexcept {
  const double z = 0.5 * std::sqrt(T) * (1.0 + x);
  const double t = z * z;
  const double rem = T - t;
  if (t <= 1.0e-14 || !(rem > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double zc = 2.0 * std::sqrt(rem / T) - 1.0;
  return zc < -1.0 ? -1.0 : (zc > 1.0 ? 1.0 : zc);
}

// The geometric tail of a decaying coefficient sequence's last terms; false when the order
// does not verify.
struct Tail {
  double value = 0.0;
  double ratio = 0.0;
  bool verified = false;
};

[[nodiscard]] Tail sigma_tail(const double *M, unsigned n, double floor) noexcept {
  const double e2 = M[n - 1] + M[n - 2];
  const double e1 = M[n - 3] + M[n - 4];
  const double e0 = M[n - 5] + M[n - 6];
  if (e2 <= floor) {
    return Tail{e2, 0.0, true}; // resolved to the noise floor
  }
  if (!(e1 > 0.0) || !(e0 > 0.0)) {
    return Tail{kInf, 1.0, false};
  }
  const double r2 = std::max(e2 / e1, e1 / e0);
  if (!(r2 <= kTailRatioMax * kTailRatioMax)) {
    return Tail{kInf, std::sqrt(r2), false};
  }
  return Tail{e2 * r2 / (1.0 - r2), std::sqrt(r2), true};
}

[[nodiscard]] Tail q_tail(const double *R, unsigned n, double r_apriori, double floor) noexcept {
  const double last = R[n - 1];
  if (last <= floor) {
    return Tail{last, r_apriori, true};
  }
  const double r_obs = R[n - 2] > 0.0 ? last / R[n - 2] : kInf;
  const double r = std::max(r_apriori, r_obs);
  if (!(r <= kTailRatioMax)) {
    return Tail{kInf, r, false};
  }
  return Tail{last * r / (1.0 - r), r, true};
}

[[nodiscard]] ValidatedErrorEnvelope uncovered(ValidatedErrorEnvelope env,
                                               FamilyEnvelopeStatus why) noexcept {
  env.error_class = FamilyErrorClass::Uncovered;
  env.status = why;
  env.bound_y.fill(kInf);
  return env;
}

// One quadrature node's bound (y), or +inf with `why` set.
struct NodeBound {
  double bound = 0.0;
  double interp = 0.0;
  double solve = 0.0;
  double reference = 0.0;
  double rounding = 0.0;
  double sigma_ratio = 0.0;
  double q_ratio = 0.0;
  double y_lo = 0.0; // lower bound of the interpolant over the box (A00 - sum |rest|)
  FamilyEnvelopeStatus why = FamilyEnvelopeStatus::Ok;
};

struct Layout {
  StatePtr st = nullptr;
  const Dct *ds = nullptr;
  const Dct *dq = nullptr;
  double lebesgue_sq = 1.0; // Lebesgue_sigma x Lebesgue_q
  double lebesgue_t = 1.0;
  double r_q = 0.0;         // a-priori q ratio (0 on a collapsed axis)
};

[[nodiscard]] NodeBound node_bound(const Layout &L, double zc) noexcept {
  const auto &st = *L.st;
  const unsigned ns = st.n_sigma;
  const unsigned nq = st.n_q;
  const unsigned nb = st.n_boundary;
  double Y[kMaxQ][kMaxS] = {};
  double ymax = 0.0;
  for (unsigned j = 0; j < nq; ++j) {
    for (unsigned s = 0; s < ns; ++s) {
      double yk[amer::kAlMaxNodes] = {};
      for (unsigned k = 0; k < nb; ++k) {
        yk[k] = st.series[(static_cast<std::size_t>(k) * nq + j) * ns + s];
      }
      Y[j][s] = bary_eval(st.grid.z.data(), st.grid.wbary.data(), yk, nb, zc);
      ymax = std::max(ymax, std::abs(Y[j][s]));
    }
  }
  double B[kMaxQ][kMaxS] = {}; // sigma transform of each q row
  for (unsigned j = 0; j < nq; ++j) {
    for (unsigned m = 0; m < ns; ++m) {
      double acc = 0.0;
      for (unsigned s = 0; s < ns; ++s) {
        acc += L.ds->edge[s] * L.ds->cosine[m][s] * Y[j][s];
      }
      B[j][m] = L.ds->scale[m] * acc;
    }
  }
  double M[kMaxS] = {};
  double R[kMaxQ] = {};
  double rest = 0.0;
  double a00 = 0.0;
  for (unsigned n = 0; n < nq; ++n) {
    for (unsigned m = 0; m < ns; ++m) {
      double acc = 0.0;
      for (unsigned j = 0; j < nq; ++j) {
        acc += L.dq->edge[j] * L.dq->cosine[n][j] * B[j][m];
      }
      const double a = std::abs(L.dq->scale[n] * acc);
      M[m] += a;
      R[n] += a;
      if (n == 0 && m == 0) {
        a00 = L.dq->scale[0] * acc; // signed: the mean level
      } else {
        rest += a;
      }
    }
  }
  NodeBound out;
  const double floor = std::max(kNoiseFloorRel * ymax, kBuildParityY);
  const Tail ts = sigma_tail(M, ns, floor);
  out.sigma_ratio = ts.ratio;
  if (!ts.verified) {
    out.why = FamilyEnvelopeStatus::SigmaOrderNotVerified;
    out.bound = kInf;
    return out;
  }
  Tail tq{0.0, 0.0, true};
  if (nq >= 2u) {
    tq = q_tail(R, nq, L.r_q, floor);
    out.q_ratio = tq.ratio;
    if (!tq.verified) {
      out.why = FamilyEnvelopeStatus::QOrderNotVerified;
      out.bound = kInf;
      return out;
    }
  }
  out.interp = 2.0 * (ts.value + tq.value);
  out.solve = L.lebesgue_t * L.lebesgue_sq * kBuildParityY;
  out.reference = L.lebesgue_t * kReferenceNoiseY;
  out.rounding = (1.0 + L.lebesgue_sq) * 64.0 * kEps * ymax;
  out.bound = out.interp + out.solve + out.reference + out.rounding;
  out.y_lo = a00 - rest; // the interpolant's lower bound over the box (pass 2 subtracts)
  return out;
}

// One premium quadrature node's bound on |g(B_family) - g(B_A)| (al_put_premium_impl's
// integrand g = 2 z (r K dr N(d2') - q S dq N(d1')) of the internal put, t = z^2): g is
// increasing in B on (0, xmax], so the TRIVIAL bound is g(xmax) - g(0+) = g(xmax), valid
// with no envelope; with a node bound delta, |ln B_f - ln B_A| <= d = min(sqrt(delta),
// delta / sqrt(y_f)) and the mean value theorem on dg/d ln B = 2 dr phi(d2) (r K - q B) /
// sigma over that interval gives the SENSITIVITY bound with phi at the closest |d2| the
// interval reaches (d2 moves by -d ln B / v). The smaller of the two.
[[nodiscard]] double node_term(const amer::AlBoundary &bnd, const ValidatedErrorEnvelope &env,
                               unsigned i, double z, double S, double K, double sigma, double r,
                               double q) noexcept {
  const double t = z * z;
  const double v = sigma * z;
  const double dq = std::exp(-q * t);
  const double dr = std::exp(-r * t);
  const double xmax = bnd.xmax;
  const double dp_x = std::log(S * dq / (xmax * dr)) / v + 0.5 * v;
  const double trivial =
      2.0 * z * (r * K * dr * core::norm_cdf(-dp_x + v) - q * S * dq * core::norm_cdf(-dp_x));
  const double box = std::max(trivial, 0.0);
  const double delta = env.bound_y[i];
  if (!(delta < kInf)) {
    return box;
  }
  if (!(delta > 0.0)) {
    return 0.0;
  }
  const double yf = bary_eval(bnd.z.data(), bnd.wbary.data(), bnd.y.data(), bnd.n, env.zc[i]);
  const double root_delta = std::sqrt(delta);
  const double d = yf > 0.0 ? std::min(root_delta, delta / std::sqrt(yf)) : root_delta;
  const double bf = xmax * std::exp(-std::sqrt(yf > 0.0 ? yf : 0.0));
  const double d2 = std::log(S * dq / (bf * dr)) / v - 0.5 * v;
  const double phi = core::norm_pdf(std::max(std::abs(d2) - d / v, 0.0));
  const double carry = std::abs(r * K - q * bf) + std::abs(q) * bf * std::expm1(d);
  const double sensitivity = 2.0 * dr * phi * carry * d / sigma;
  return std::min(box, sensitivity);
}

thread_local const ValidatedErrorEnvelope *t_family_envelope = nullptr;

} // namespace

ValidatedErrorEnvelope build_family_error_envelope(const AlSigmaQFamily &f) noexcept {
  ValidatedErrorEnvelope env;
  env.family = &f;
  env.bound_y.fill(kInf);
  env.y_lo.fill(0.0);
  const StatePtr st = QFamilyAccess::state(f);
  if (st == nullptr || !f.prepared() || st->g == nullptr ||
      st->g->generation != st->generation || st->ws == nullptr || st->series == nullptr) {
    return uncovered(env, FamilyEnvelopeStatus::NotPrepared);
  }
  env.preparation_stamp = st->preparation_stamp;
  env.domain = FamilyErrorDomain{st->sigma_lo, st->sigma_hi, st->q_lo, st->q_hi, st->grid.T,
                                 st->side};
  env.n_sigma = st->n_sigma;
  env.n_q = st->n_q;
  const unsigned nquad = st->ws->n_quad_price;
  if (st->n_sigma < 6u || st->n_sigma > kMaxS || st->n_q > kMaxQ ||
      nquad > kFamilyEnvelopeMaxQuad || st->n_boundary > amer::kAlMaxNodes) {
    return uncovered(env, FamilyEnvelopeStatus::UnsupportedShape);
  }
  if (st->n_q == 1u && st->q_hi != st->q_lo) {
    return uncovered(env, FamilyEnvelopeStatus::DegenerateQBox);
  }
  env.n_quad = static_cast<std::uint16_t>(nquad);
  Dct ds;
  Dct dq;
  lay_dct(st->n_sigma, ds);
  lay_dct(st->n_q, dq);
  Layout L;
  L.st = st;
  L.ds = &ds;
  L.dq = &dq;
  L.lebesgue_sq = lobatto_lebesgue(st->n_sigma) * lobatto_lebesgue(st->n_q);
  L.lebesgue_t = lobatto_lebesgue(st->n_boundary);
  L.r_q = st->n_q >= 2u ? 1.0 / q_axis_rho(st->rate, st->q_lo, st->q_hi) : 0.0;
  FamilyErrorContributions &c = env.contributions;
  // Pass 1: each node's own tail (2 x the extrapolated sigma + q tails; +inf where the
  // order does not verify) and its noise terms.
  std::array<NodeBound, kFamilyEnvelopeMaxQuad> raw{};
  for (unsigned i = 0; i < nquad; ++i) {
    const double zc = quad_zc(st->ws->qx_price[i], st->grid.T);
    env.zc[i] = zc;
    env.z[i] = 0.5 * std::sqrt(st->grid.T) * (1.0 + st->ws->qx_price[i]);
    env.w[i] = st->ws->qw_price[i];
    if (std::isnan(zc)) {
      continue; // the integrand reads no boundary at this node: bound_y 0 below
    }
    raw[i] = node_bound(L, zc);
    c.sigma_ratio = std::max(c.sigma_ratio, raw[i].sigma_ratio);
    c.q_ratio = std::max(c.q_ratio, raw[i].q_ratio);
  }
  // Pass 2: a tail measured at one time node can be accidentally small (a coefficient
  // crossing zero as tau moves), so each node takes kTailSafety x the worst tail of its
  // kTailWindow neighbours on either side; a neighbour whose order did not verify makes
  // the node unverified too (bound +inf: the price bound then uses the trivial box bound).
  for (unsigned i = 0; i < nquad; ++i) {
    if (std::isnan(env.zc[i])) {
      env.bound_y[i] = 0.0;
      continue;
    }
    double interp = 0.0;
    bool verified = true;
    const unsigned lo = i >= kTailWindow ? i - kTailWindow : 0u;
    const unsigned hi = std::min(nquad - 1u, i + kTailWindow);
    for (unsigned k = lo; k <= hi; ++k) {
      if (std::isnan(env.zc[k])) {
        continue;
      }
      verified = verified && raw[k].why == FamilyEnvelopeStatus::Ok;
      interp = std::max(interp, raw[k].interp);
    }
    if (!verified) {
      env.bound_y[i] = kInf;
      ++c.unverified_nodes;
      continue;
    }
    const NodeBound &nb = raw[i];
    const double tail = kTailSafety * interp;
    env.bound_y[i] = tail + nb.solve + nb.reference + nb.rounding;
    env.y_lo[i] = nb.y_lo - env.bound_y[i];
    c.interpolation_y = std::max(c.interpolation_y, tail);
    c.boundary_solve_y = std::max(c.boundary_solve_y, nb.solve);
    c.reference_y = std::max(c.reference_y, nb.reference);
    c.rounding_y = std::max(c.rounding_y, nb.rounding);
  }
  c.quadrature_usd = 0.0;
  c.rounding_rel = kPriceRoundingRel;
  env.error_class = FamilyErrorClass::RefinementValidated;
  env.status = FamilyEnvelopeStatus::Ok;
  return env;
}

FamilyBoundedPrice family_price_bounded(const AlSigmaQFamily &f, const ValidatedErrorEnvelope &env,
                                        double S, double K, double sigma, double q) noexcept {
  FamilyBoundedPrice out;
  const StatePtr st = QFamilyAccess::state(f);
  if (st == nullptr) {
    return out;
  }
  out.status = st->admit(S, K, sigma, q);
  if (out.status != AlSigmaQFamilyStatus::Ok) {
    return out;
  }
  amer::AlBoundary bnd = st->grid;
  st->interpolate(sigma, q, bnd);
  out.price = st->price_on(bnd, S, K, sigma, q); // == f.price(S, K, sigma, q).price
  if (!env.covered() || env.family != &f || env.preparation_stamp != st->preparation_stamp ||
      env.n_quad != st->ws->n_quad_price ||
      env.n_sigma != st->n_sigma || env.n_q != st->n_q || env.domain.sigma_lo != st->sigma_lo ||
      env.domain.sigma_hi != st->sigma_hi || env.domain.q_lo != st->q_lo ||
      env.domain.q_hi != st->q_hi || env.domain.T != st->grid.T || env.domain.side != st->side) {
    return out;
  }
  const detail::InternalPutCoords c = detail::internal_put_coords(st->side, S, K);
  const detail::InternalPutRates rq = detail::internal_put_rates(st->side, st->rate, q);
  const double half_sqrt_t = 0.5 * std::sqrt(st->grid.T);
  double sum = 0.0;
  for (unsigned i = 0; i < env.n_quad; ++i) {
    if (std::isnan(env.zc[i])) {
      continue; // the integrand reads no boundary at this node
    }
    sum += env.w[i] * node_term(bnd, env, i, env.z[i], c.Sp, c.Kp, sigma, rq.rp, rq.qp);
  }
  out.bound_usd = half_sqrt_t * sum + env.contributions.rounding_rel * (c.Sp + c.Kp);
  return out;
}

double family_domain_bound_usd(const ValidatedErrorEnvelope &env, double S, double K,
                               double rate) noexcept {
  if (!env.covered()) {
    return kInf;
  }
  const FamilyErrorDomain &d = env.domain;
  const detail::InternalPutCoords c = detail::internal_put_coords(d.side, S, K);
  const double inv_sqrt_2pi = 1.0 / std::sqrt(2.0 * std::numbers::pi);
  double worst = 0.0;
  for (const double q : {d.q_lo, d.q_hi}) {
    const detail::InternalPutRates rq = detail::internal_put_rates(d.side, rate, q);
    const double xmax = amer::al_xmax_put(c.Kp, rq.rp, rq.qp);
    // node_term with every point-dependent factor at its domain-wide worst, at node time
    // t = z^2. Box bound: g(xmax) = 2 z (r K e^{-rt} N2 - q S e^{-qt} N1) <= 2 z (max(r, 0) K
    // + max(-q, 0) S e^{max(-q, 0) t}) (N <= 1; e^{-rt} <= 1 when r >= 0, the r term <= 0
    // when r < 0; a NEGATIVE carry's e^{-qt} > 1, review-K3 mn-7). Sensitivity: phi <= 1 /
    // sqrt(2 pi), e^{-rt} <= e^{max(-r, 0) t}, |r K - q B| <= |r| K + |q| xmax, 1 / sigma
    // <= 1 / sigma_lo, y_f >= y_lo. Every node's term falls with a put's q below 0 and
    // rises with it above 0, and rises with a call's q (its internal r > 0), so the box's
    // two q ends carry the worst sum.
    const double carry = std::abs(rq.rp) * c.Kp + std::abs(rq.qp) * xmax;
    const double neg_q = std::max(-rq.qp, 0.0);
    const double neg_r = std::max(-rq.rp, 0.0);
    double sum = 0.0;
    for (unsigned i = 0; i < env.n_quad; ++i) {
      if (std::isnan(env.zc[i])) {
        continue;
      }
      const double t = env.z[i] * env.z[i];
      const double box =
          2.0 * env.z[i] * (std::max(rq.rp, 0.0) * c.Kp + neg_q * c.Sp * std::exp(neg_q * t));
      const double delta = env.bound_y[i];
      double term = box;
      if (delta < kInf && delta > 0.0) {
        const double root_delta = std::sqrt(delta);
        const double y = env.y_lo[i];
        const double dd = y > 0.0 ? std::min(root_delta, delta / std::sqrt(y)) : root_delta;
        const double sens =
            2.0 * inv_sqrt_2pi * std::exp(neg_r * t) * carry * std::exp(dd) * dd / d.sigma_lo;
        term = std::min(box, sens);
      } else if (!(delta > 0.0)) {
        term = 0.0;
      }
      sum += env.w[i] * term;
    }
    worst = std::max(worst, sum);
  }
  return 0.5 * std::sqrt(d.T) * worst + env.contributions.rounding_rel * (c.Sp + c.Kp);
}

double family_exercise_strike(const AlSigmaQFamily &f, double S, double sigma,
                              double q) noexcept {
  const StatePtr st = QFamilyAccess::state(f);
  if (st == nullptr || st->admit_point(S, sigma, q) != AlSigmaQFamilyStatus::Ok) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  amer::AlBoundary bnd = st->grid;
  st->interpolate(sigma, q, bnd);
  const double y_now = bnd.y[bnd.n - 1u]; // the last collocation node sits at tau = T
  const detail::InternalPutRates rq = detail::internal_put_rates(st->side, st->rate, q);
  // The boundary per unit internal strike: xmax(1) exp(-sqrt(y)) (homogeneity).
  const double beta =
      amer::al_xmax_put(1.0, rq.rp, rq.qp) * std::exp(-std::sqrt(std::max(y_now, 0.0)));
  if (!(beta > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // Put: exercised when S <= beta K. Call (internal put: strike S, spot K): when K <= beta S.
  return st->side == Side::Put ? S / beta : beta * S;
}

void record_family_probe_audit(ValidatedErrorEnvelope &env,
                               const SampledErrorEstimate &probes) noexcept {
  env.audit = probes;
  if (probes.violations > 0u && env.covered()) {
    env = uncovered(env, FamilyEnvelopeStatus::ProbeViolation);
    env.audit = probes;
  }
}

FamilyEnvelopeScope::FamilyEnvelopeScope(const ValidatedErrorEnvelope *env) noexcept
    : previous_(t_family_envelope) {
  t_family_envelope = env;
}

FamilyEnvelopeScope::~FamilyEnvelopeScope() { t_family_envelope = previous_; }

const ValidatedErrorEnvelope *current_family_envelope(const AlSigmaQFamily *family) noexcept {
  const ValidatedErrorEnvelope *env = t_family_envelope;
  if (env == nullptr || family == nullptr || env->family != family) return nullptr;
  const StatePtr st = QFamilyAccess::state(*family);
  if (st == nullptr || !st->ok || st->g == nullptr || st->g->generation != st->generation ||
      env->preparation_stamp != st->preparation_stamp ||
      env->n_sigma != family->n_sigma() || env->n_q != family->n_q() ||
      env->domain.sigma_lo != family->sigma_lo() || env->domain.sigma_hi != family->sigma_hi() ||
      env->domain.q_lo != family->q_lo() || env->domain.q_hi != family->q_hi()) {
    return nullptr;
  }
  return env;
}

} // namespace atx::vol
