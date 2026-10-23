// ── AlSigmaQFamily analytics (A0 r6, 2026-10-23): the primary Greek bundle ──────────
//
// Contract on the entries in api/pricing/american.hpp (the A0 block) and on the stages
// in pricing/american_sigma_q_family.hpp. This TU owns ONLY the analytics on a prepared
// (σ, q) family and its rate-bump set: it reads the family through
// sigma_q_family_detail::QFamilyAccess and K1's plain-array stages, and never solves a
// boundary except inside the rate-bump set's prepare() (two whole families at r ± h).
//
// THE MAP (K1's fused map; internal put coordinates (S', K', r', q') — a call is the put
// (K, S; q, r) by McDonald-Schroder): P = clamp(E + Π), Π = ½√T Σ_i wz2_i I_i,
//   I_i = a_i Φ(−d−_i) − b_i Φ(−d+_i),  a_i = r' K' e^{−r' t_i},  b_i = q' S' e^{−q' t_i},
//   d+_i = (ln(S'/xmax) + s_i + (r' − q') t_i) / v_i + v_i / 2,  v_i = σ √t_i,  s_i = √y_i,
// y_i the τ-barycentric value, at premium node i, of the node tensor interpolated in
// (σ, q); xmax = K' min(1, r'/q'). A Strike node (rem <= 0, unreachable for interior
// Gauss-Legendre nodes, mirrored as K1 does) reads B = K'.
//
// THE DERIVATIVES, per unit wz2_i, with G_i = a φ(d−) − b φ(d+) (so that
// ∂I/∂ln(S'/B) = −G / v):
//   S'   ∂I/∂S' = −a φ(d−)/(S' v) − q' e^{−q't} Φ(−d+) + q' e^{−q't} φ(d+)/v at FIXED
//        boundary (the internal put's boundary does not depend on its spot), ∂²I/∂S'²
//        the derivative of that (L3g's expressions, american_family_greeks.cpp);
//   σ    (a d+ φ(d−) − b d− φ(d+))/σ − G s'_σ / v — K1's accumulator, bit for bit;
//   r'   K' e^{−r't}(1 − r't) Φ(−d−) − (t/v) G + (G/v)(∂ln xmax/∂r' − s'_{r'});
//   q'   (t/v) G − S' e^{−q't}(1 − q't) Φ(−d+) + (G/v)(∂ln xmax/∂q' − s'_{q'});
// where s'_θ = y'_θ / (2s) is the boundary's motion in θ (0 where y <= 0; the xmax and
// motion terms only at Boundary nodes). The option's q is the internal YIELD of a put
// and the internal RATE of a call, so the tensor's q axis (dy/dq by the nodal
// differentiation matrix, exact for the interpolant) feeds s'_{q'} of a put and s'_{r'}
// of a call; the rate-bump set's dY/dr (central difference of two families at r ± h)
// feeds the other. The European leg adds its closed forms. A floored premium (Π <= 0)
// contributes no derivative (the served Π is the constant 0 there), as in K1 / L3g.
// Calls: Δ = (P − S' P_S')/K', Γ = (S'/K')² P_S'S' — homogeneity of degree 1, exact for
// the served map (y is strike-free; xmax and the clamps are linear in K'). θ: the PDE
// identity with the option's (r, q) — NOT a derivative of the map; next to the edge
// (edge_sd < kAlFamilyGreekEdgeBand, the header's near-edge band) θ and Γ are
// NearExerciseEdge (review-A0 MJ1). Exercise: K1's rule (FusedPriceVega::exercised),
// read as is (PM r6 note: its semantics belong to K1 / K4) — the exercise Greeks.
//
// COST. The strike-free stage is K1's boundary_at_nodes plus one more q-contraction and
// (rho only) one contraction of the rate-bump tensor; the pass is K1's plus the S' /
// carry accumulators. Everything is const and stack-only (≈ 6 KB of node scratch per
// call); N threads on one const family touch only const memory.

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include "atx/core/math.hpp" // norm_cdf / norm_pdf (the Reference Φ, φ)
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"     // black76_value_and_vega (the European leg)
#include "pricing/american_boundary.hpp"       // amer::al_xmax_put / kAlMaxNodes
#include "pricing/american_sigma_family.hpp"   // sigma_family_detail::kMaxSigmaNodes / kNaN
#include "pricing/american_sigma_q_family.hpp" // K1's stages + QFamilyAccess
#include "pricing/boundary_interp.hpp"         // detail::internal_put_coords
#include "pricing/scalar_erfc.hpp"             // detail::norm_cdf_erfc / norm_pdf_cody (Fast)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using sigma_family_detail::kMaxSigmaNodes;
using sigma_family_detail::kNaN;
using sigma_q_family_detail::kMaxQNodes;

// ── The rate-bump set's state ────────────────────────────────────────────────────
//
// The base family's identity (checked by value on every call — the set does not observe
// the family) and dY/dr on the family's tensor layout.
struct AlSigmaQFamilyRateBumps::State {
  bool ok = false;
  Side side = Side::Put;
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  double anchor = 0.0;
  double T = 0.0;
  double r = 0.0;
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double q_lo = 0.0;
  double q_hi = 0.0;
  std::uint16_t n_sigma = 0;
  std::uint16_t n_q = 0;
  std::uint16_t n_boundary = 0;
  unsigned n_prem = 0;
  bool fast = false;
  std::size_t tensor = 0; // n_boundary · n_q · n_sigma
  std::size_t per = 0;    // n_q · n_sigma (one collocation row)
  // dY/dr[(k · n_q + j) · n_sigma + s], the central difference of the r ± h families.
  std::unique_ptr<double[]> d_rate{};
  // The base tensor's last and middle collocation rows (2 · per): a family built on other
  // AlOpts (sweep budget, tolerance) or another ISA route has other y and fails the check.
  std::unique_ptr<double[]> sentinel{};
};

namespace {

using sigma_q_family_detail::bary_basis;
using sigma_q_family_detail::carry_at;
using sigma_q_family_detail::finish_node_boundary;
using sigma_q_family_detail::GreeksContext;
using sigma_q_family_detail::GreeksInternal;
using sigma_q_family_detail::GreeksNeed;
using sigma_q_family_detail::kFusedKinkRel;
using sigma_q_family_detail::kMaxPremiumNodes;
using sigma_q_family_detail::NodeBoundary;
using sigma_q_family_detail::NodeCarryMotion;
using sigma_q_family_detail::PremiumNodeKind;
using sigma_q_family_detail::PremiumNodes;
using sigma_q_family_detail::QCarry;
using sigma_q_family_detail::QFamilyAccess;
using sigma_q_family_detail::sigma_z;
using sigma_q_family_detail::SigmaZ;

using Greek = AlFamilyGreek;
using Reason = AlFamilyGreekReason;

// The family's private State type, obtained through its friend's accessor (access control
// applies to names; the type itself is what K1's TU and the bench already read through
// `auto`).
using QState = std::remove_cv_t<std::remove_pointer_t<decltype(QFamilyAccess::state(
    std::declval<const AlSigmaQFamily &>()))>>;

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

[[nodiscard]] double clamp_unit(double z) noexcept {
  return (z < -1.0) ? -1.0 : ((z > 1.0) ? 1.0 : z);
}

// ∂ln xmax/∂r' and ∂ln xmax/∂q' for xmax = K' min(1, r'/q') (the American-regime branch
// of al_xmax_put that depends on the carry; every other branch is flat in both — the
// L3g helper, verbatim).
struct XmaxLog {
  double d_rate = 0.0;
  double d_yield = 0.0;
};
[[nodiscard]] XmaxLog xmax_log_derivs(double rate, double yield) noexcept {
  XmaxLog x;
  if (rate > 0.0 && yield > 0.0 && rate < yield) {
    x.d_rate = 1.0 / rate;
    x.d_yield = -1.0 / yield;
  }
  return x;
}

// Σ_j b_j(q) Σ_s b_s(σ) D[(k · n_q + j) · n_sigma + s] for every collocation node k: the
// (σ, q)-interpolant of a tensor on the family's layout (linear in the tensor, so for D =
// dY/dr it is the r-derivative of the interpolated boundary).
void contract_at(const QState &st, double sigma, double q, const double *D,
                 double *out) noexcept {
  const SigmaZ az = sigma_z(st.axis, st.u_lo, st.u_hi, sigma);
  double bs[kMaxSigmaNodes] = {};
  bary_basis(st.sz, st.sw, st.n_sigma, az.z, bs);
  double bq[kMaxQNodes] = {};
  if (st.n_q == 1) {
    bq[0] = 1.0;
  } else {
    bary_basis(st.qz, st.qw, st.n_q, clamp_unit((q - st.q_lo) * st.q_scale - 1.0), bq);
  }
  const std::size_t nt = static_cast<std::size_t>(st.n_q) * st.n_sigma;
  for (unsigned k = 0; k < st.n_boundary; ++k) {
    const double *dk = D + static_cast<std::size_t>(k) * nt;
    double acc = 0.0;
    for (unsigned j = 0; j < st.n_q; ++j) {
      const double *row = dk + static_cast<std::size_t>(j) * st.n_sigma;
      double inner = 0.0;
      for (unsigned s = 0; s < st.n_sigma; ++s) {
        inner += bs[s] * row[s];
      }
      acc += bq[j] * inner;
    }
    out[k] = acc;
  }
}

// The τ barycentric at every Boundary premium node — K1's tau_interpolate loop, expression
// for expression for y and dy/dσ (so their bits are boundary_at_nodes'), with dy/dq and
// dy/dr accumulated on the same weights.
void tau_interpolate4(const double *z, const double *w, unsigned nb, const PremiumNodes &pn,
                      const double *yk, const double *dk, const double *qk, const double *rk,
                      double *y, double *dy, double *dyq, double *dyr) noexcept {
  for (unsigned i = 0; i < pn.n; ++i) {
    if (pn.kind[i] != PremiumNodeKind::Boundary) {
      y[i] = 0.0;
      dy[i] = 0.0;
      dyq[i] = 0.0;
      dyr[i] = 0.0;
      continue;
    }
    const double zc = pn.zc[i];
    double num = 0.0;
    double num1 = 0.0;
    double den = 0.0;
    double numq = 0.0;
    double numr = 0.0;
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
      numq += qq * qk[k];
      numr += qq * rk[k];
    }
    if (hit >= 0) {
      const auto h = static_cast<unsigned>(hit);
      y[i] = yk[h];
      dy[i] = dk[h];
      dyq[i] = qk[h];
      dyr[i] = rk[h];
    } else {
      y[i] = num / den;
      dy[i] = num1 / den;
      dyq[i] = numq / den;
      dyr[i] = numr / den;
    }
  }
}

// The strike-free stage at one (σ, q): K1's boundary_at_nodes (the same interpolate_derivs
// and τ loop, so nb is its bits) plus the boundary's motion in the internal put's carry —
// dy/dq from the tensor's q axis when `want_q` and the axis exists, dy/dr from `d_rate`
// when non-null — as s' = y'/(2s) per Boundary node, with the transform-zero test.
void stage_boundary(const QState &st, const PremiumNodes &pn, double sigma, double q,
                    bool want_q, const double *d_rate, NodeBoundary &nb,
                    NodeCarryMotion &m) noexcept {
  const unsigned nk = st.n_boundary;
  double yk[amer::kAlMaxNodes] = {};
  double dk[amer::kAlMaxNodes] = {};
  double qk[amer::kAlMaxNodes] = {};
  double rk[amer::kAlMaxNodes] = {};
  const bool have_q = st.interpolate_derivs(sigma, q, yk, dk, want_q ? qk : nullptr);
  const bool have_r = d_rate != nullptr;
  if (have_r) {
    contract_at(st, sigma, q, d_rate, rk);
  }
  double y[kMaxPremiumNodes] = {};
  double dy[kMaxPremiumNodes] = {};
  double dyq[kMaxPremiumNodes] = {};
  double dyr[kMaxPremiumNodes] = {};
  tau_interpolate4(st.grid.z.data(), st.grid.wbary.data(), nk, pn, yk, dk, qk, rk, y, dy, dyq,
                   dyr);
  finish_node_boundary(pn, y, dy, yk[nk - 1u], sigma, nb);
  // The option's q is the internal yield of a put and the internal rate of a call.
  const bool call = st.side == Side::Call;
  double *ds_q = call ? m.ds_rate : m.ds_yield;
  double *ds_r = call ? m.ds_yield : m.ds_rate;
  double ymax = std::abs(yk[nk - 1u]);
  for (unsigned i = 0; i < pn.n; ++i) {
    if (pn.kind[i] == PremiumNodeKind::Boundary) {
      ymax = std::max(ymax, std::abs(y[i]));
    }
  }
  const double kink = kFusedKinkRel * ymax;
  bool q_degenerate = false;
  bool r_degenerate = false;
  for (unsigned i = 0; i < pn.n; ++i) {
    ds_q[i] = 0.0;
    ds_r[i] = 0.0;
    if (pn.kind[i] != PremiumNodeKind::Boundary) {
      continue;
    }
    const double yi = y[i];
    const double two_s = 2.0 * nb.s[i];
    const bool at_zero = std::abs(yi) <= kink;
    if (have_q) {
      ds_q[i] = (yi > 0.0) ? dyq[i] / two_s : 0.0;
      q_degenerate = q_degenerate || (at_zero && dyq[i] != 0.0);
    }
    if (have_r) {
      ds_r[i] = (yi > 0.0) ? dyr[i] / two_s : 0.0;
      r_degenerate = r_degenerate || (at_zero && dyr[i] != 0.0);
    }
  }
  m.have_rate = call ? have_q : have_r;
  m.have_yield = call ? have_r : have_q;
  m.rate_degenerate = call ? q_degenerate : r_degenerate;
  m.yield_degenerate = call ? r_degenerate : q_degenerate;
}

// The European put leg: price and vega exactly as K1's euro_leg (black76_value_and_vega
// on the Reference arm, black76_put_fast term for term on the Fast arm), plus its S' and
// carry derivatives in closed form when the pass needs them.
struct EuroGreeks {
  double price = 0.0;
  double vega = 0.0;
  double dS = 0.0;
  double dSS = 0.0;
  double drate = 0.0;
  double dyield = 0.0;
};

template <bool kFast>
[[nodiscard]] EuroGreeks euro_greeks(const PremiumNodes &pn, double Sp, double Kp, double sigma,
                                     double rp, double qp, const GreeksNeed &need) noexcept {
  const double F = Sp * std::exp((rp - qp) * pn.T);
  const double df = std::exp(-rp * pn.T);
  const double v = sigma * pn.sqrtT;
  EuroGreeks e;
  double d1 = 0.0;
  if constexpr (kFast) {
    d1 = (std::log(F / Kp) + 0.5 * v * v) / v;
    const double d2 = d1 - v;
    e.price = df * (Kp * cdf_of<true>(-d2) - F * cdf_of<true>(-d1));
    e.vega = F * df * pdf_of<true>(d1) * pn.sqrtT;
  } else {
    const Black76ValueVega b = black76_value_and_vega(F, Kp, pn.T, sigma, df, Side::Put, pn.sqrtT);
    e.price = b.price;
    e.vega = b.vega;
    d1 = (std::log(F / Kp) + 0.5 * v * v) / v;
  }
  if (need.s1 || need.s2 || need.rate || need.yield) {
    const double d2 = d1 - v;
    const double eq = std::exp(-qp * pn.T);
    const double Phi1 = cdf_of<kFast>(-d1);
    e.dS = -eq * Phi1;
    e.dSS = eq * pdf_of<kFast>(d1) / (Sp * v);
    e.drate = -Kp * pn.T * df * cdf_of<kFast>(-d2);
    e.dyield = Sp * pn.T * eq * Phi1;
  }
  return e;
}

template <bool kFast>
void greeks_pass_t(const PremiumNodes &pn, const QCarry &c, const NodeBoundary &nb,
                   const NodeCarryMotion &m, double Sp, double Kp, const GreeksNeed &need,
                   GreeksInternal &out) noexcept {
  const double sigma = nb.sigma;
  const double rp = c.rp;
  const double qp = c.qp;
  const double xmax = amer::al_xmax_put(Kp, rp, qp);
  const double lsx = std::log(Sp / xmax);
  const double drift = rp - qp;
  const XmaxLog xl = xmax_log_derivs(rp, qp);
  const bool carry = need.rate || need.yield;
  const bool spot_or_carry = need.s1 || need.s2 || carry;
  double total = 0.0;
  double asig = 0.0;
  double aS = 0.0;
  double aSS = 0.0;
  double arate = 0.0;
  double ayield = 0.0;
  for (unsigned i = 0; i < pn.n; ++i) {
    if (nb.skip[i]) {
      continue;
    }
    const bool bnode = pn.kind[i] == PremiumNodeKind::Boundary;
    // K1's premium_pass_t, expression for expression (the price and vega bits).
    const double ld = bnode ? lsx + nb.s[i] + drift * pn.t[i]
                            : std::log(Sp * c.dq[i] / (Kp * c.dr[i])); // B = K (rem <= 0)
    const double v = nb.v[i];
    const double dp = ld / v + 0.5 * v;
    const double dm = dp - v;
    const double a = rp * Kp * c.dr[i];
    const double b = qp * Sp * c.dq[i];
    // The integrand's Φ values serve the S' / carry accumulators below too (review-L3g
    // m3, applied once the derivative contract was proven): the same expression, so the
    // price's bits are unchanged.
    const double Phm = cdf_of<kFast>(-dm);
    const double Php = cdf_of<kFast>(-dp);
    total += pn.wz2[i] * (a * Phm - b * Php);
    const double phm = pdf_of<kFast>(dm);
    const double php = pdf_of<kFast>(dp);
    if (need.sigma) {
      asig += pn.wz2[i] *
              ((a * dp * phm - b * dm * php) / sigma - (a * phm - b * php) * nb.ds[i] / v);
    }
    if (!spot_or_carry) {
      continue;
    }
    // ── the S' and carry accumulators (per unit 2 z_i w_i) ──
    const double wgt = pn.wz2[i];
    const double G = a * phm - b * php;
    const double qdq = qp * c.dq[i];
    if (need.s1) {
      const double u = 1.0 / (Sp * v);
      aS += wgt * (-a * phm * u - qdq * Php + qdq * php / v);
    }
    if (need.s2) {
      aSS += wgt * (a * phm * dp / (Sp * Sp * v * v) + qdq * php / (Sp * v) -
                    qdq * dp * php / (Sp * v * v));
    }
    if (carry) {
      const double t = pn.t[i];
      const double rho_d = t / v; // ∂d±/∂r' = −∂d±/∂q' at fixed boundary
      if (need.rate) {
        double term = Kp * c.dr[i] * (1.0 - rp * t) * Phm - rho_d * G;
        if (bnode) {
          term += G * (xl.d_rate - m.ds_rate[i]) / v;
        }
        arate += wgt * term;
      }
      if (need.yield) {
        double term = rho_d * G - Sp * c.dq[i] * (1.0 - qp * t) * Php;
        if (bnode) {
          term += G * (xl.d_yield - m.ds_yield[i]) / v;
        }
        ayield += wgt * term;
      }
    }
  }
  total *= pn.half_sqrtT;
  const bool prem_floored = !(total > 0.0);
  const double prem = prem_floored ? 0.0 : total;
  const EuroGreeks euro = euro_greeks<kFast>(pn, Sp, Kp, sigma, rp, qp, need);
  // The al_put_price_from_boundary clamp order (K1's).
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
  out.price = price;
  out.inside = Sp <= xmax * nb.now_factor;
  // The near-edge band's coordinate (review-A0 MJ1): the distance to that same edge in
  // σ√T, > 0 on the continuation side.
  out.edge_sd = std::log(Sp / (xmax * nb.now_factor)) / (sigma * pn.sqrtT);
  out.exercised = clamped || (intr > 0.0 && price <= intr + 1.0e-9 * Kp) || out.inside;
  out.sigma_degenerate = nb.degenerate;
  if (out.exercised) {
    out.dS = -1.0;
    out.dSS = out.dsig = out.drate = out.dyield = 0.0;
    return;
  }
  const double h = pn.half_sqrtT;
  out.dS = euro.dS + (prem_floored ? 0.0 : h * aS);
  out.dSS = euro.dSS + (prem_floored ? 0.0 : h * aSS);
  out.dsig = euro.vega + (prem_floored ? 0.0 : pn.half_sqrtT * asig); // K1's vega expression
  out.drate = euro.drate + (prem_floored ? 0.0 : h * arate);
  out.dyield = euro.dyield + (prem_floored ? 0.0 : h * ayield);
}

[[nodiscard]] bool convention_implemented(AlFamilyGreekHeldFixed h) noexcept {
  switch (h) {
  case AlFamilyGreekHeldFixed::ContractSigma:
  case AlFamilyGreekHeldFixed::StickyStrike:
    return true;
  case AlFamilyGreekHeldFixed::StickyMoneyness:
  case AlFamilyGreekHeldFixed::RefittedSurface:
    return false;
  }
  return false;
}

[[nodiscard]] AlFamilyGreekConvention convention_of(const AlSigmaQGreeksRequest &req) noexcept {
  AlFamilyGreekConvention c;
  c.held_fixed = req.held_fixed;
  c.surface_version = req.surface_version;
  return c;
}

[[nodiscard]] double &field_of(AlSigmaQFamilyGreeks &g, Greek k) noexcept {
  switch (k) {
  case Greek::Delta:
    return g.delta;
  case Greek::Gamma:
    return g.gamma;
  case Greek::Vega:
    return g.vega;
  case Greek::Theta:
    return g.theta;
  case Greek::Rho:
    return g.rho;
  case Greek::Phi:
    return g.phi;
  }
  return g.delta; // unreachable: every enumerator is handled above
}

constexpr Greek kGreeks[kAlFamilyGreekCount] = {Greek::Delta, Greek::Gamma, Greek::Vega,
                                                Greek::Theta, Greek::Rho,   Greek::Phi};

// A refused row: price NaN, every requested Greek NaN with Refused.
[[nodiscard]] AlSigmaQFamilyGreeks refused(AlSigmaQFamilyStatus st,
                                           const AlSigmaQGreeksRequest &req) noexcept {
  AlSigmaQFamilyGreeks g;
  g.status = st;
  g.price = kNaN;
  g.requested = req.mask;
  g.convention = convention_of(req);
  for (const Greek k : kGreeks) {
    if (req.mask.has(k)) {
      field_of(g, k) = kNaN;
      g.reason[static_cast<std::size_t>(k)] = Reason::Refused;
    }
  }
  return g;
}

// One admitted contract from the shared stages (the scalar entry and every lane of the
// SoA entry run exactly this).
[[nodiscard]] AlSigmaQFamilyGreeks bundle_at(const QState &st, const PremiumNodes &pn,
                                             const QCarry &c, const double *d_rate,
                                             Reason rho_source, double S, double K,
                                             double sigma, double q,
                                             const AlSigmaQGreeksRequest &req) noexcept {
  const detail::InternalPutCoords ic = detail::internal_put_coords(st.side, S, K);
  GreeksContext ctx;
  ctx.side = st.side;
  ctx.S = S;
  ctx.r = st.rate;
  ctx.q = q;
  ctx.sigma = sigma;
  ctx.Sp = ic.Sp;
  ctx.Kp = ic.Kp;
  ctx.q_axis = st.n_q > 1;
  ctx.rho_source = rho_source;
  const GreeksNeed need =
      sigma_q_family_detail::greeks_need(req, ctx, convention_implemented(req.held_fixed));
  const bool call = st.side == Side::Call;
  const bool want_q = call ? need.rate : need.yield;
  const bool want_r = call ? need.yield : need.rate;
  NodeBoundary nb;
  NodeCarryMotion m;
  stage_boundary(st, pn, sigma, q, want_q, want_r ? d_rate : nullptr, nb, m);
  GreeksInternal in;
  sigma_q_family_detail::greeks_pass(pn, c, nb, m, ic.Sp, ic.Kp, st.ws->fast, need, in);
  return sigma_q_family_detail::map_greeks(in, m, ctx, req);
}

// The column writer of the SoA entry.
void store_lane(const AlSigmaQGreekColumns &out, std::size_t i,
                const AlSigmaQFamilyGreeks &g) noexcept {
  const auto put = [i](std::span<double> col, double v) {
    if (!col.empty()) {
      col[i] = v;
    }
  };
  put(out.price, g.price);
  put(out.delta, g.delta);
  put(out.gamma, g.gamma);
  put(out.vega, g.vega);
  put(out.theta, g.theta);
  put(out.rho, g.rho);
  put(out.phi, g.phi);
  if (!out.status.empty()) {
    out.status[i] = g.status;
  }
  if (!out.supported.empty()) {
    out.supported[i] = g.supported;
  }
  if (!out.reason.empty()) {
    out.reason[i] = g.reason;
  }
  if (!out.flags.empty()) {
    std::uint8_t f = 0;
    if (g.exercised) {
      f |= 1u;
    }
    if (g.floored) {
      f |= 2u;
    }
    if (g.near_edge) {
      f |= 4u;
    }
    out.flags[i] = f;
  }
  put(out.edge_sd, g.edge_sd);
}

[[nodiscard]] bool columns_fit(const AlSigmaQGreekColumns &out, std::size_t n) noexcept {
  const auto fits = [n](std::size_t size) { return size == 0 || size == n; };
  return fits(out.price.size()) && fits(out.delta.size()) && fits(out.gamma.size()) &&
         fits(out.vega.size()) && fits(out.theta.size()) && fits(out.rho.size()) &&
         fits(out.phi.size()) && fits(out.status.size()) && fits(out.supported.size()) &&
         fits(out.reason.size()) && fits(out.flags.size()) && fits(out.edge_sd.size());
}

// The scalar entry's body: admit, lay the node data and the carry, run bundle_at.
[[nodiscard]] AlSigmaQFamilyGreeks greeks_entry(const AlSigmaQFamily &fam,
                                                const AlSigmaQFamilyRateBumps *rate, double S,
                                                double K, double sigma, double q,
                                                const AlSigmaQGreeksRequest &req) noexcept {
  const auto *st = QFamilyAccess::state(fam);
  if (st == nullptr) {
    return refused(AlSigmaQFamilyStatus::NotPrepared, req);
  }
  const AlSigmaQFamilyStatus a = st->admit(S, K, sigma, q);
  if (a != AlSigmaQFamilyStatus::Ok) {
    return refused(a, req);
  }
  PremiumNodes pn;
  if (!st->lay_premium_nodes(pn)) {
    // Unreachable on a prepared family: the geometry caps the premium order at
    // kAlMaxQuad == kMaxPremiumNodes. Typed, never a silent value.
    assert(false && "premium order above kMaxPremiumNodes on a prepared family");
    return refused(AlSigmaQFamilyStatus::InvalidArgument, req);
  }
  Reason rho_source = Reason::NoCarryBumps;
  const double *d_rate = nullptr;
  if (req.mask.has(Greek::Rho)) {
    d_rate = QFamilyAccess::rate_tensor(fam, rate, rho_source);
  }
  QCarry c;
  carry_at(pn, st->side, st->rate, q, c);
  return bundle_at(*st, pn, c, d_rate, rho_source, S, K, sigma, q, req);
}

} // namespace

// ── The stages (declared in the internal header) ──────────────────────────────────

namespace sigma_q_family_detail {

void greeks_pass(const PremiumNodes &pn, const QCarry &c, const NodeBoundary &nb,
                 const NodeCarryMotion &m, double Sp, double Kp, bool fast,
                 const GreeksNeed &need, GreeksInternal &out) noexcept {
  if (fast) {
    greeks_pass_t<true>(pn, c, nb, m, Sp, Kp, need, out);
  } else {
    greeks_pass_t<false>(pn, c, nb, m, Sp, Kp, need, out);
  }
}

GreeksNeed greeks_need(const AlSigmaQGreeksRequest &req, const GreeksContext &ctx,
                       bool convention_ok) noexcept {
  GreeksNeed n;
  if (!convention_ok) {
    return n; // every requested Greek is ConventionUnsupported: the price alone
  }
  const AlFamilyGreekMask mk = req.mask;
  const bool theta = mk.has(Greek::Theta);
  n.s1 = mk.has(Greek::Delta) || theta;
  n.s2 = mk.has(Greek::Gamma) || theta;
  n.sigma = mk.has(Greek::Vega);
  const bool phi = mk.has(Greek::Phi) && ctx.q_axis;
  const bool rho = mk.has(Greek::Rho) && ctx.rho_source == Reason::Served;
  const bool call = ctx.side == Side::Call;
  n.rate = call ? phi : rho;
  n.yield = call ? rho : phi;
  return n;
}

AlSigmaQFamilyGreeks map_greeks(const GreeksInternal &in, const NodeCarryMotion &m,
                                const GreeksContext &ctx,
                                const AlSigmaQGreeksRequest &req) noexcept {
  AlSigmaQFamilyGreeks g;
  g.status = AlSigmaQFamilyStatus::Ok;
  g.price = in.price;
  g.requested = req.mask;
  g.convention = convention_of(req);
  g.exercised = in.exercised;
  g.floored = in.exercised && !in.inside;
  g.edge_sd = in.edge_sd;
  // The near-edge band (review-A0 MJ1): a continuation row within kAlFamilyGreekEdgeBand
  // σ√T of the current edge serves neither θ (the PDE identity cancels O(rK) terms there
  // and was measured up to 138 % off with the wrong sign) nor Γ (the term it amplifies,
  // not validated there).
  g.near_edge = !in.exercised && in.edge_sd < kAlFamilyGreekEdgeBand;
  const bool call = ctx.side == Side::Call;
  // The option's Greeks from the internal put's (homogeneity for a call).
  double delta = in.dS;
  double gamma = in.dSS;
  double rho = in.drate;
  double phi = in.dyield;
  if (call) {
    const double x = ctx.Sp / ctx.Kp;
    delta = (in.price - ctx.Sp * in.dS) / ctx.Kp;
    gamma = x * x * in.dSS;
    rho = in.dyield;
    phi = in.drate;
  }
  double vega = in.dsig;
  double theta = ctx.r * in.price - (ctx.r - ctx.q) * ctx.S * delta -
                 0.5 * ctx.sigma * ctx.sigma * ctx.S * ctx.S * gamma;
  if (in.exercised) {
    delta = call ? 1.0 : -1.0;
    gamma = vega = theta = rho = phi = 0.0;
  }
  const bool rho_degenerate = call ? m.yield_degenerate : m.rate_degenerate;
  const bool phi_degenerate = call ? m.rate_degenerate : m.yield_degenerate;
  const bool convention_ok = convention_implemented(req.held_fixed);
  for (const Greek k : kGreeks) {
    const auto idx = static_cast<std::size_t>(k);
    if (!req.mask.has(k)) {
      g.reason[idx] = Reason::NotRequested;
      continue;
    }
    Reason why = Reason::Served;
    double value = 0.0;
    switch (k) {
    case Greek::Delta:
      value = delta;
      break;
    case Greek::Gamma:
      value = gamma;
      why = g.near_edge ? Reason::NearExerciseEdge : Reason::Served;
      break;
    case Greek::Vega:
      value = vega;
      why = in.sigma_degenerate ? Reason::NearNodeDegenerate : Reason::Served;
      break;
    case Greek::Theta:
      value = theta;
      why = g.near_edge ? Reason::NearExerciseEdge : Reason::Served;
      break;
    case Greek::Rho:
      value = rho;
      why = (ctx.rho_source != Reason::Served) ? ctx.rho_source
            : rho_degenerate                    ? Reason::NearNodeDegenerate
                                                : Reason::Served;
      break;
    case Greek::Phi:
      value = phi;
      why = !ctx.q_axis       ? Reason::NoQAxis
            : phi_degenerate ? Reason::NearNodeDegenerate
                             : Reason::Served;
      break;
    }
    if (!convention_ok) {
      why = Reason::ConventionUnsupported;
    } else if (in.exercised) {
      why = Reason::Exercised; // the exercise value is known without any motion
    }
    const bool usable = why == Reason::Served || why == Reason::Exercised;
    field_of(g, k) = usable ? value : kNaN;
    g.reason[idx] = why;
    if (usable) {
      g.supported = g.supported.with(k);
    }
  }
  return g;
}

// ── QFamilyAccess: the rate-bump set ──────────────────────────────────────────────

const double *QFamilyAccess::rate_tensor(const AlSigmaQFamily &f,
                                         const AlSigmaQFamilyRateBumps *b,
                                         AlFamilyGreekReason &why) noexcept {
  if (b == nullptr) {
    why = Reason::NoCarryBumps;
    return nullptr;
  }
  const AlSigmaQFamilyRateBumps::State *c = b->st_.get();
  if (c == nullptr || !c->ok) {
    why = Reason::CarryBumpRefused;
    return nullptr;
  }
  const AlSigmaQFamily::State &st = *f.st_;
  bool same = c->side == st.side && c->axis == st.axis && c->anchor == st.anchor &&
              c->T == st.grid.T && c->r == st.rate && c->sigma_lo == st.sigma_lo &&
              c->sigma_hi == st.sigma_hi && c->q_lo == st.q_lo && c->q_hi == st.q_hi &&
              c->n_sigma == st.n_sigma && c->n_q == st.n_q && c->n_boundary == st.n_boundary &&
              c->n_prem == st.ws->n_quad_price && c->fast == st.ws->fast &&
              c->tensor == st.series_size;
  if (same) {
    const double *last = st.series.get() + (st.n_boundary - 1u) * c->per;
    const double *mid = st.series.get() + (st.n_boundary / 2u) * c->per;
    same = std::equal(last, last + c->per, c->sentinel.get()) &&
           std::equal(mid, mid + c->per, c->sentinel.get() + c->per);
  }
  if (!same) {
    why = Reason::CarryBumpRefused;
    return nullptr;
  }
  why = Reason::Served;
  return c->d_rate.get();
}

Status QFamilyAccess::prepare_rate_bumps(const AlSigmaQFamily &base, AlSigmaQFamilyRateBumps &out,
                                         double h) {
  out.st_.reset(); // strong guarantee: unprepared unless this call succeeds
  if (!(std::isfinite(h) && h > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "AlSigmaQFamilyRateBumps::prepare: bump not > 0");
  }
  const AlSigmaQFamily::State *st = base.st_.get();
  if (st == nullptr || !st->ok || st->g == nullptr || st->g->generation != st->generation ||
      st->ws == nullptr) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaQFamilyRateBumps::prepare: the base family is not prepared (or is "
               "moved-from, or its geometry was re-prepared)");
  }
  const AlExpiryGeometry::Impl &g = *st->g;
  auto fresh = std::make_unique<AlSigmaQFamilyRateBumps::State>();
  fresh->per = static_cast<std::size_t>(st->n_q) * st->n_sigma;
  fresh->tensor = st->series_size;
  fresh->d_rate = std::make_unique<double[]>(fresh->tensor);
  fresh->sentinel = std::make_unique<double[]>(2u * fresh->per);
  // One bumped family (the base's opts, side, anchor, boxes, nodes, axis) -> `apply` on
  // its node tensor; the bumped geometry and family die with this call.
  const auto bumped = [&](double dr, auto &&apply) -> Status {
    const AlExpiryGeometry gb = prepare_al_geometry(g.T, g.r + dr, g.q, g.opts);
    AlSigmaQFamily fb(gb, st->side, st->anchor);
    Status ps = fb.prepare(st->sigma_lo, st->sigma_hi, st->n_sigma, st->q_lo, st->q_hi,
                           st->n_q, st->axis);
    if (!ps) {
      return ps;
    }
    const AlSigmaQFamily::State &sb = *fb.st_;
    bool same_grid = sb.n_boundary == st->n_boundary && sb.n_sigma == st->n_sigma &&
                     sb.n_q == st->n_q && sb.series_size == st->series_size &&
                     sb.grid.T == st->grid.T && sb.ws->n_quad_price == st->ws->n_quad_price &&
                     sb.ws->fast == st->ws->fast;
    for (unsigned k = 0; same_grid && k < st->n_boundary; ++k) {
      same_grid = sb.grid.z[k] == st->grid.z[k];
    }
    if (!same_grid) {
      return Err(ErrorCode::NotImplemented,
                 "AlSigmaQFamilyRateBumps::prepare: a bumped family's collocation grid differs "
                 "from the base's");
    }
    apply(sb.series.get());
    return Ok();
  };
  double *d = fresh->d_rate.get();
  const std::size_t n = fresh->tensor;
  if (Status su = bumped(h, [d, n](const double *up) { std::copy(up, up + n, d); }); !su) {
    return su;
  }
  if (Status sd = bumped(-h,
                         [d, n, h](const double *dn) {
                           for (std::size_t i = 0; i < n; ++i) {
                             d[i] = (d[i] - dn[i]) / (2.0 * h);
                           }
                         });
      !sd) {
    return sd;
  }
  fresh->side = st->side;
  fresh->axis = st->axis;
  fresh->anchor = st->anchor;
  fresh->T = st->grid.T;
  fresh->r = st->rate;
  fresh->sigma_lo = st->sigma_lo;
  fresh->sigma_hi = st->sigma_hi;
  fresh->q_lo = st->q_lo;
  fresh->q_hi = st->q_hi;
  fresh->n_sigma = st->n_sigma;
  fresh->n_q = st->n_q;
  fresh->n_boundary = st->n_boundary;
  fresh->n_prem = st->ws->n_quad_price;
  fresh->fast = st->ws->fast;
  const double *last = st->series.get() + (st->n_boundary - 1u) * fresh->per;
  const double *mid = st->series.get() + (st->n_boundary / 2u) * fresh->per;
  std::copy(last, last + fresh->per, fresh->sentinel.get());
  std::copy(mid, mid + fresh->per, fresh->sentinel.get() + fresh->per);
  fresh->ok = true;
  out.st_ = std::move(fresh);
  return Ok();
}

} // namespace sigma_q_family_detail

// ── The rate-bump set ──────────────────────────────────────────────────────────────

AlSigmaQFamilyRateBumps::AlSigmaQFamilyRateBumps() = default;
AlSigmaQFamilyRateBumps::~AlSigmaQFamilyRateBumps() = default;
AlSigmaQFamilyRateBumps::AlSigmaQFamilyRateBumps(AlSigmaQFamilyRateBumps &&) noexcept = default;
AlSigmaQFamilyRateBumps &
AlSigmaQFamilyRateBumps::operator=(AlSigmaQFamilyRateBumps &&) noexcept = default;

Status AlSigmaQFamilyRateBumps::prepare(const AlSigmaQFamily &base) {
  return QFamilyAccess::prepare_rate_bumps(base, *this);
}

bool AlSigmaQFamilyRateBumps::prepared() const noexcept { return st_ && st_->ok; }

std::size_t AlSigmaQFamilyRateBumps::state_bytes() const noexcept {
  if (!st_) {
    return 0;
  }
  return sizeof(State) + (st_->tensor + 2u * st_->per) * sizeof(double);
}

// ── Public entries ─────────────────────────────────────────────────────────────────

AlSigmaQFamilyGreeks family_greeks(const AlSigmaQFamily &fam, double S, double K, double sigma,
                                   double q, const AlSigmaQGreeksRequest &req) noexcept {
  return greeks_entry(fam, nullptr, S, K, sigma, q, req);
}

AlSigmaQFamilyGreeks family_greeks(const AlSigmaQFamily &fam, const AlSigmaQFamilyRateBumps &rate,
                                   double S, double K, double sigma, double q,
                                   const AlSigmaQGreeksRequest &req) noexcept {
  return greeks_entry(fam, &rate, S, K, sigma, q, req);
}

Status family_greeks_many(const AlSigmaQFamily &fam, double S, std::span<const double> K,
                          std::span<const double> sigma, std::span<const double> q,
                          std::span<const std::uint8_t> active, const AlSigmaQGreekColumns &out,
                          const AlSigmaQGreeksRequest &req, const AlSigmaQFamilyRateBumps *rate) {
  const std::size_t n = K.size();
  if (sigma.size() != n || q.size() != n || (!active.empty() && active.size() != n) ||
      !columns_fit(out, n)) {
    return Err(ErrorCode::InvalidArgument,
               "family_greeks_many: K / sigma / q / active / column lengths differ");
  }
  const auto *st = QFamilyAccess::state(fam);
  // The strike-, σ- and q-free node data and the rate view, laid on the first admitted
  // lane (a family that admits no lane never reads them); the carry while q repeats.
  PremiumNodes pn;
  bool laid = false;
  Reason rho_source = Reason::NoCarryBumps;
  const double *d_rate = nullptr;
  QCarry c;
  bool have_carry = false;
  for (std::size_t i = 0; i < n; ++i) {
    if (!active.empty() && active[i] == 0) {
      continue;
    }
    if (st == nullptr) {
      store_lane(out, i, refused(AlSigmaQFamilyStatus::NotPrepared, req));
      continue;
    }
    const AlSigmaQFamilyStatus a = st->admit(S, K[i], sigma[i], q[i]);
    if (a != AlSigmaQFamilyStatus::Ok) {
      store_lane(out, i, refused(a, req));
      continue;
    }
    if (!laid) {
      if (!st->lay_premium_nodes(pn)) {
        assert(false && "premium order above kMaxPremiumNodes on a prepared family");
        store_lane(out, i, refused(AlSigmaQFamilyStatus::InvalidArgument, req));
        continue;
      }
      if (req.mask.has(Greek::Rho)) {
        d_rate = QFamilyAccess::rate_tensor(fam, rate, rho_source);
      }
      laid = true;
    }
    if (!have_carry || std::bit_cast<std::uint64_t>(c.q) != std::bit_cast<std::uint64_t>(q[i])) {
      carry_at(pn, st->side, st->rate, q[i], c);
      have_carry = true;
    }
    store_lane(out, i, bundle_at(*st, pn, c, d_rate, rho_source, S, K[i], sigma[i], q[i], req));
  }
  return Ok();
}

} // namespace atx::vol
