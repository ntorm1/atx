// ── AlSigmaFamily analytics: analytic Greeks, the exact Newton slope, chains, shocks ──
//
// L3g family-greeks (2026-10-20). Contract on the entry points in
// api/pricing/american.hpp (the block after AlSigmaFamily). This TU owns ONLY the
// analytics on a prepared family (and its detached copy): it reads the family's
// State through pricing/american_sigma_family.hpp and never solves a boundary.
//
// THE MAP. For the internal put (puts directly; calls through McDonald-Schroder, the
// call's spot being the internal strike) the family serves
//   P(S, K, σ) = clamp( E(S, K, σ) + Π ),   Π = ½√T · Σ_j w_j · I_j,
//   I_j = 2 z_j [ r K e^{-r t_j} Φ(−d−_j) − q S e^{-q t_j} Φ(−d+_j) ],
//   d±_j = ln( S e^{-q t_j} / (B_j e^{-r t_j}) ) / v_j ± v_j / 2,  v_j = σ √t_j,
//   B_j = xmax · exp(−√y(T − t_j)),  y(u) = Σ_k ℓ_k(u) y_k(σ)  (τ-barycentric),
//   y_k(σ) = Σ_s b_s(z(σ)) Y_{k,s}                               (σ-barycentric),
// exactly the expressions american.cpp's premium_integrand_put / al_put_premium_impl /
// al_boundary_at evaluate (the price accumulated here mirrors them operation for
// operation, so it is the family's price to the bit — pinned by the tests; the
// detached copy relies on it).
//
// THE DERIVATIVES (every one exact for that map; the sprint note measures them).
//   S:   B_j does not depend on S, so ∂I/∂S, ∂²I/∂S², ∂³I/∂S³ differentiate d± in S at
//        fixed B (∂d±/∂S = 1/(S v)); φ(d±) is shared across all of them.
//   σ:   explicit terms (∂d+/∂σ = −d−/σ, ∂d−/∂σ = −d+/σ) PLUS the boundary motion:
//        with s_j = √y_j, B_j = xmax e^{−s_j}, B'_j = −B_j s'_j, B''_j = B_j (s'_j² − s''_j),
//        s' = y'/(2s), s'' = y''/(2s) − y'²/(4 s³) and y' = Σ_k ℓ_k dy_k/dσ. Every
//        boundary term has a factor ∂I/∂B ∝ 1/B that cancels B, so only s', s'' enter.
//   dy_k/dσ: the σ-interpolant's derivative. NOT the quotient rule on the barycentric
//        formula (y' = (N' − y D')/D loses O(1/|z − z_s|) digits near a node and is
//        0/0 at one — the exact-node erratum of Berrut & Trefethen, SIAM Review 46
//        (2004) p. 510). Instead the nodal derivatives Y'_{k,s} = Σ_t D⁽¹⁾_{st} Y_{k,t}
//        (D⁽¹⁾_{st} = (w_t/w_s)/(z_s − z_t), D⁽¹⁾_{ss} = −Σ_{t≠s} D⁽¹⁾_{st}: eq. (9.4)
//        of that paper with the negative-sum diagonal) are interpolated by the same
//        barycentric basis: y'(z) = Σ_s b_s(z) Y'_s — an identity for the degree-(n−1)
//        polynomial (its derivative has degree n−2 and is reproduced by n nodes), so it
//        is exact, and the basis evaluation is the forward-stable one (Higham 2004).
//        y'' likewise through D⁽²⁾. The paper's printed D⁽²⁾ carries a sign typo; the
//        form used is Welfert's (SIAM J. Numer. Anal. 34 (1997)) D⁽²⁾_{st} =
//        2 D⁽¹⁾_{st} (D⁽¹⁾_{ss} − 1/(z_s − z_t)), D⁽²⁾_{ss} = −Σ_{t≠s} D⁽²⁾_{st}, verified
//        through the served volga against the Richardson σ-difference of the family's
//        own price (AlSigmaFamilyGreeks.GreeksMatchRichardsonOnTheFamilyMapWithinBound:
//        a sign error in D⁽²⁾ is an O(1) volga gap there). At an exact node hit
//        b = e_hit and the formula reduces to the nodal derivative, so the Greeks are
//        continuous through the nodes (GreeksAreContinuousThroughASigmaNode).
//   r,q: frozen-boundary terms (frozen y: discounts, the drift in d± and the explicit
//        xmax(K, rate, yield) = K min(1, rate/yield) rescale of B) PLUS, only with a
//        carry set, the boundary term Σ_j ∂I_j/∂B_j · dB_j/dθ (θ = rate or yield) with
//        dB_j/dθ = −B_j s'_θ, s'_θ = y'_θ/(2s), y'_θ the τ-barycentric value of the
//        carry set's dY_k/dθ (central differences of the r ± h / q ± h families'
//        series) — the vega boundary term with dY/dσ replaced. The frozen part ALONE is
//        up to 2.4× off the re-solved rho (sprint note §4), so without a carry set
//        rho / phi are refused (NaN, NoCarryBumps) and the frozen part is not computed.
//   θ, charm: the continuation-region PDE identity (no τ axis), as RecordGreeks.
// Calls: the internal put's bundle is mapped by homogeneity — P(S_int, K_int) =
// K_int p(S_int / K_int) holds EXACTLY for the served map (y is strike-free, xmax and
// the clamps are linear in K_int) — ∂P/∂K_int = (P − S_int P_S)/K_int,
// ∂²P/∂K_int² = x² P_SS, ∂³P/∂K_int³ = −x³ P_SSS − 3 x² P_SS / K_int (x = S_int/K_int),
// vanna_call = (vega − S_int vanna_int)/K_int, rho_call = ∂P/∂yield, phi_call = ∂P/∂rate.
//
// COST. One pass over the p premium nodes (p = 48 accurate): the price's log, two
// exp, two Φ per node plus two φ and ~60 flops; the σ tables (n_σ² + n_b n_σ FMAs)
// once per call. The nodal differentiation matrices depend on n_σ alone (the
// Chebyshev-Lobatto grid is the same for every family with that n_σ) and live in a
// process-wide table built on first use.
//
// THREADS. Everything is const and stack-only (one AlBoundary ≈ 1.3 KB plus ≈ 1 KB of
// derivative scratch per call); N threads on one const family touch only const
// memory — the L2h race-freedom argument, re-tested here for the Greeks entry.
// The solve ledger is not bumped (nothing default calls this TU).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "atx/core/math.hpp" // norm_cdf / norm_pdf / clamp (the Reference kernel's Φ, φ)
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"   // black76_price (the Reference European leg)
#include "pricing/american_boundary.hpp"     // amer::AlBoundary / al_xmax_put / kAlMaxNodes
#include "pricing/american_sigma_family.hpp" // AlSigmaFamily::State + the σ-axis evaluator
#include "pricing/boundary_interp.hpp"       // detail::internal_put_coords
#include "pricing/scalar_erfc.hpp"           // detail::norm_cdf_erfc / norm_pdf_cody (Fast)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using sigma_family_detail::axis_of;
using sigma_family_detail::bary_eval;
using sigma_family_detail::cheb_node;
using sigma_family_detail::kMaxSigmaNodes;
using sigma_family_detail::kNaN;

namespace sigma_family_detail {

// What the fused kernel reads: the family's State or a detached copy. Named (not in
// the anonymous namespace) so FamilyAccess, the family's friend, can hand it out.
struct FamilyView {
  Side side = Side::Put;
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  double u_lo = 0.0;
  double u_hi = 0.0;
  unsigned n_sigma = 0;
  unsigned n_boundary = 0;
  double rate = 0.0;
  double yield = 0.0;
  const double *sz = nullptr;
  const double *sw = nullptr;
  const double *series = nullptr; // [k * kMaxSigmaNodes + s]
  const amer::AlBoundary *grid = nullptr;
  const double *qx = nullptr; // the workspace header the premium quadrature reads
  const double *qw = nullptr;
  unsigned nq = 0;
  bool fast = false;
};

// The carry set's series derivatives on the INTERNAL put's axes (a put's rate is r and
// its yield q; a call's rate is q and its yield r), [k * kMaxSigmaNodes + s]; null =
// that axis is not served by this call.
struct CarryView {
  const double *d_rate = nullptr;
  const double *d_yield = nullptr;
};

// ── Nodal differentiation matrices on the Chebyshev-Lobatto grid ──────────────────
//
// Declared in american_sigma_family.hpp. K1 (r5, 2026-10-22) moved them out of this
// TU's anonymous namespace so the (σ, q) family's fused vega shares them on both of its
// axes (american_sigma_q_family.cpp); the bodies are L3g's, unchanged.
// D1[s][t] = (w_t / w_s) / (z_s − z_t) (s ≠ t), D1[s][s] = −Σ_{t≠s} D1[s][t];
// D2[s][t] = 2 D1[s][t] (D1[s][s] − 1/(z_s − z_t)) (s ≠ t), D2[s][s] = −Σ_{t≠s} D2[s][t].
// The grid and weights are the family's own (cheb_node, (−1)^s halved at the ends),
// so a table depends on n alone. Built once per process for n in [2, kMaxSigmaNodes]
// (L3g built [kAlSigmaFamilyMinNodes, kMax]; the q axis also needs n = 2, 3 — every
// table the σ family already read is the same bits).
namespace {

[[nodiscard]] DiffTables build_diff_tables(unsigned n) noexcept {
  DiffTables t{};
  double z[kMaxSigmaNodes] = {};
  double w[kMaxSigmaNodes] = {};
  for (unsigned s = 0; s < n; ++s) {
    z[s] = cheb_node(s, n);
    double ws = (s & 1u) ? -1.0 : 1.0;
    if (s == 0 || s + 1 == n) {
      ws *= 0.5;
    }
    w[s] = ws;
  }
  for (unsigned s = 0; s < n; ++s) {
    double diag = 0.0;
    for (unsigned k = 0; k < n; ++k) {
      if (k == s) {
        continue;
      }
      const double d = (w[k] / w[s]) / (z[s] - z[k]);
      t.d1[s][k] = d;
      diag -= d;
    }
    t.d1[s][s] = diag;
  }
  for (unsigned s = 0; s < n; ++s) {
    double diag = 0.0;
    for (unsigned k = 0; k < n; ++k) {
      if (k == s) {
        continue;
      }
      const double d = 2.0 * t.d1[s][k] * (t.d1[s][s] - 1.0 / (z[s] - z[k]));
      t.d2[s][k] = d;
      diag -= d;
    }
    t.d2[s][s] = diag;
  }
  return t;
}

} // namespace

const DiffTables &diff_tables(unsigned n) noexcept {
  static const std::array<DiffTables, kMaxSigmaNodes + 1> all = [] {
    std::array<DiffTables, kMaxSigmaNodes + 1> a{};
    for (unsigned m = 2; m <= kMaxSigmaNodes; ++m) {
      a[m] = build_diff_tables(m);
    }
    return a;
  }();
  return all[n];
}

} // namespace sigma_family_detail

namespace {

using Mask = AlSigmaFamilyGreeksMask;
using sigma_family_detail::CarryView;
using sigma_family_detail::FamilyView;

using sigma_family_detail::diff_tables;
using sigma_family_detail::DiffTables;

// Which internal derivatives a call needs (derived from the mask and the side).
struct Need {
  bool edge = false;  // per-Greek admission; price+vega inversion needs no log
  bool s1 = false;    // P_S
  bool s2 = false;    // P_SS
  bool s3 = false;    // P_SSS
  bool sig1 = false;  // dP/dσ (total)
  bool sig2 = false;  // d²P/dσ²
  bool cross = false; // d²P/dS dσ
  bool rate = false;  // ∂P/∂rate (frozen y)
  bool yield = false; // ∂P/∂yield (frozen y)
  [[nodiscard]] bool any_sigma() const noexcept { return sig1 || sig2 || cross; }
};

// `carry`: rho / phi are served (a carry set is attached and matches); without one the
// frozen-boundary terms are not computed at all (they are never served).
[[nodiscard]] Need need_of(Mask m, Side side, bool carry) noexcept {
  const bool call = side == Side::Call;
  const bool theta = m.has(Mask::kTheta);
  const bool charm = m.has(Mask::kCharm);
  Need n;
  n.edge = m.bits != 0;
  n.s1 = m.has(Mask::kDelta) || theta || charm;
  n.s2 = m.has(Mask::kGamma) || theta || charm;
  n.s3 = charm;
  n.sig1 = m.has(Mask::kVega) || (call && m.has(Mask::kVanna));
  n.sig2 = m.has(Mask::kVolga);
  n.cross = m.has(Mask::kVanna);
  n.rate = carry && (call ? m.has(Mask::kPhi) : m.has(Mask::kRho));
  n.yield = carry && (call ? m.has(Mask::kRho) : m.has(Mask::kPhi));
  return n;
}


// The internal put's bundle at (S_int, K_int, σ).
struct Internal {
  double price = 0.0; // the served (clamped) price — the family's price to the bit
  double edge_sd = kNaN;
  double dS = 0.0;
  double dSS = 0.0;
  double dSSS = 0.0;
  double dsig = 0.0;
  double dsig2 = 0.0;
  double dSsig = 0.0;
  double drate = 0.0;      // frozen y, xmax term included, + the carry boundary term
  double dyield = 0.0;
  double drate_xmax = 0.0; // the xmax term alone (for the measurement hook)
  double dyield_xmax = 0.0;
  double drate_bt = 0.0;   // the carry boundary term alone (0 without a carry view)
  double dyield_bt = 0.0;
  bool exercised = false;
  bool floored = false;
};

// σ -> z on the family's axis, with dz/dσ and d²z/dσ² (the clamp is a rounding guard
// at the box edges, so the derivative is the unclamped one).
struct AxisZ {
  double z = 0.0;
  double dz = 0.0;
  double d2z = 0.0;
};

[[nodiscard]] AxisZ axis_z(const FamilyView &v, double sigma) noexcept {
  AxisZ a;
  const double scale = 2.0 / (v.u_hi - v.u_lo);
  double z = 2.0 * (axis_of(v.axis, sigma) - v.u_lo) / (v.u_hi - v.u_lo) - 1.0;
  z = (z < -1.0) ? -1.0 : ((z > 1.0) ? 1.0 : z); // rounding at the box edges only
  a.z = z;
  if (v.axis == AlSigmaFamilyAxis::LogSigma) {
    a.dz = scale / sigma;
    a.d2z = -scale / (sigma * sigma);
  } else {
    a.dz = scale;
    a.d2z = 0.0;
  }
  return a;
}

// The barycentric basis b_s(z) on the σ grid (exact node hit -> e_hit).
void sigma_basis(const FamilyView &v, double z, double *b) noexcept {
  double q[kMaxSigmaNodes] = {};
  double den = 0.0;
  for (unsigned s = 0; s < v.n_sigma; ++s) {
    const double dz = z - v.sz[s];
    if (dz == 0.0) {
      for (unsigned t = 0; t < v.n_sigma; ++t) {
        b[t] = (t == s) ? 1.0 : 0.0;
      }
      return;
    }
    q[s] = v.sw[s] / dz;
    den += q[s];
  }
  for (unsigned s = 0; s < v.n_sigma; ++s) {
    b[s] = q[s] / den;
  }
}

// dy_k/dσ and d²y_k/dσ² for every collocation node k: c = bᵀD1, c2 = bᵀD2 once, then
// the series dotted with them, then the chain rule through z(σ).
void sigma_series_derivatives(const FamilyView &v, const AxisZ &az, bool second, double *dy,
                              double *d2y) noexcept {
  double b[kMaxSigmaNodes] = {};
  sigma_basis(v, az.z, b);
  const DiffTables &dt = diff_tables(v.n_sigma);
  double c[kMaxSigmaNodes] = {};
  double c2[kMaxSigmaNodes] = {};
  for (unsigned s = 0; s < v.n_sigma; ++s) {
    const double bs = b[s];
    for (unsigned t = 0; t < v.n_sigma; ++t) {
      c[t] += bs * dt.d1[s][t];
      if (second) {
        c2[t] += bs * dt.d2[s][t];
      }
    }
  }
  for (unsigned k = 0; k < v.n_boundary; ++k) {
    const double *row = &v.series[k * kMaxSigmaNodes];
    double yz = 0.0;
    double yzz = 0.0;
    for (unsigned t = 0; t < v.n_sigma; ++t) {
      yz += c[t] * row[t];
      if (second) {
        yzz += c2[t] * row[t];
      }
    }
    dy[k] = yz * az.dz;
    d2y[k] = second ? (yzz * az.dz * az.dz + yz * az.d2z) : 0.0;
  }
}

// Σ_s b_s(z) D[k][s] for every collocation node k: the σ-interpolant of a carry series
// (linear in the series, so it is the (r, q)-derivative of the interpolated boundary).
void carry_series_at(const FamilyView &v, const AxisZ &az, const double *d, double *out) noexcept {
  double b[kMaxSigmaNodes] = {};
  sigma_basis(v, az.z, b);
  for (unsigned k = 0; k < v.n_boundary; ++k) {
    const double *row = &d[k * kMaxSigmaNodes];
    double acc = 0.0;
    for (unsigned s = 0; s < v.n_sigma; ++s) {
      acc += b[s] * row[s];
    }
    out[k] = acc;
  }
}

// ∂ ln xmax / ∂rate and ∂ ln xmax / ∂yield for xmax = K min(1, rate/yield) (the
// American-regime branches of al_xmax_put; every other branch is flat in both).
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

// The fused pass: price (mirrored) + the needed internal derivatives. `cv` (nullable)
// adds the (rate, yield) boundary terms to drate / dyield where need.rate / need.yield.
void fused_internal(const FamilyView &v, double S, double K, double sigma, const Need &need,
                    const CarryView *cv, Internal &out) noexcept {
  const double r = v.rate;
  const double q = v.yield;
  const amer::AlBoundary &grid = *v.grid;
  const double T = grid.T;
  const unsigned nb = v.n_boundary;

  // ── σ-interpolate the boundary (exactly as State::price_at) ──
  const AxisZ az = axis_z(v, sigma);
  amer::AlBoundary bnd = grid; // stack scratch
  for (unsigned k = 0; k < nb; ++k) {
    bnd.y[k] = bary_eval(v.sz, v.sw, &v.series[k * kMaxSigmaNodes], v.n_sigma, az.z);
  }
  bnd.K = K;
  bnd.xmax = amer::al_xmax_put(K, r, q);
  double dy[amer::kAlMaxNodes] = {};
  double d2y[amer::kAlMaxNodes] = {};
  const bool want_sigma = need.any_sigma();
  if (want_sigma) {
    sigma_series_derivatives(v, az, need.sig2, dy, d2y);
  }
  const XmaxLog xl = xmax_log_derivs(r, q);
  const bool want_xmax = (need.rate || need.yield) && (xl.d_rate != 0.0 || xl.d_yield != 0.0);
  // The carry boundary terms: dY/d(rate), dY/d(yield) interpolated at σ, per node k.
  const bool bt_rate = need.rate && cv != nullptr && cv->d_rate != nullptr;
  const bool bt_yield = need.yield && cv != nullptr && cv->d_yield != nullptr;
  double dyr[amer::kAlMaxNodes] = {};
  double dyq[amer::kAlMaxNodes] = {};
  if (bt_rate) {
    carry_series_at(v, az, cv->d_rate, dyr);
  }
  if (bt_yield) {
    carry_series_at(v, az, cv->d_yield, dyq);
  }

  // ── European leg (bit-identical to euro_put_sk / euro_put_sk_fast) ──
  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const double sqrtT = std::sqrt(T);
  const double vT = sigma * sqrtT;
  double euro = 0.0;
  double d1 = 0.0;
  if (v.fast) {
    // black76_put_fast, term for term.
    d1 = (std::log(F / K) + 0.5 * vT * vT) / vT;
    const double d2 = d1 - vT;
    euro = df * (K * detail::norm_cdf_erfc(-d2) - F * detail::norm_cdf_erfc(-d1));
  } else {
    euro = black76_price(F, K, T, sigma, df, Side::Put);
    d1 = (std::log(F / K) + 0.5 * vT * vT) / vT;
  }
  const double d2 = d1 - vT;
  const double eq = std::exp(-q * T);
  const double Phi1 = v.fast ? detail::norm_cdf_erfc(-d1) : atx::core::norm_cdf(-d1);
  const double Phi2 = v.fast ? detail::norm_cdf_erfc(-d2) : atx::core::norm_cdf(-d2);
  const double phi1 = v.fast ? detail::norm_pdf_cody(d1) : atx::core::norm_pdf(d1);
  double eS = -eq * Phi1;
  double eSS = eq * phi1 / (S * vT);
  double eSSS = -eSS * (1.0 + d1 / vT) / S;
  double esig = S * eq * phi1 * sqrtT;
  double esig2 = esig * d1 * d2 / sigma;
  double eSsig = -eq * phi1 * d2 / sigma;
  double erate = -K * T * df * Phi2;
  double eyield = S * T * eq * Phi1;

  // ── premium quadrature: the kernel's accumulation + the derivative accumulators ──
  const double half_sqrtT = 0.5 * sqrtT;
  const double lsx = v.fast ? std::log(S / bnd.xmax) : 0.0;
  double total = 0.0;
  double aS = 0.0, aSS = 0.0, aSSS = 0.0;
  double asig = 0.0, asig2 = 0.0, aSsig = 0.0;
  double arate = 0.0, ayield = 0.0, arate_x = 0.0, ayield_x = 0.0;
  double arate_b = 0.0, ayield_b = 0.0;
  for (unsigned i = 0; i < v.nq; ++i) {
    const double zi = half_sqrtT * (1.0 + v.qx[i]);
    const double t = zi * zi;
    double I = 0.0;
    if (t <= 1.0e-14) {
      total += v.qw[i] * I;
      continue;
    }
    const double rem = T - t;
    // al_boundary_at(bnd, rem) — the τ-barycentric value (and its σ-derivatives on
    // the same weights), mirrored expression for expression.
    double yj = 0.0;
    double dyj = 0.0;
    double d2yj = 0.0;
    double dyj_r = 0.0; // carry: dy/d(rate), dy/d(yield) at this node
    double dyj_q = 0.0;
    if (rem > 0.0) {
      const double u_eff = (rem >= T) ? T : rem;
      const double zq = 2.0 * std::sqrt(u_eff / T) - 1.0;
      const double zc = atx::core::clamp(zq, -1.0, 1.0);
      double num = 0.0;
      double den = 0.0;
      double num1 = 0.0;
      double num2 = 0.0;
      double num_r = 0.0;
      double num_q = 0.0;
      int hit = -1;
      for (unsigned k = 0; k < nb; ++k) {
        const double dz = zc - bnd.z[k];
        if (dz == 0.0) {
          hit = static_cast<int>(k);
          break;
        }
        const double qq = bnd.wbary[k] / dz;
        num += qq * bnd.y[k];
        den += qq;
        num1 += qq * dy[k];
        num2 += qq * d2y[k];
        num_r += qq * dyr[k];
        num_q += qq * dyq[k];
      }
      if (hit >= 0) {
        yj = bnd.y[static_cast<unsigned>(hit)];
        dyj = dy[static_cast<unsigned>(hit)];
        d2yj = d2y[static_cast<unsigned>(hit)];
        dyj_r = dyr[static_cast<unsigned>(hit)];
        dyj_q = dyq[static_cast<unsigned>(hit)];
      } else {
        yj = num / den;
        dyj = num1 / den;
        d2yj = num2 / den;
        dyj_r = num_r / den;
        dyj_q = num_q / den;
      }
    }
    // Boundary at the node and its log-form derivatives in σ.
    double b_t = 0.0;
    double sy = 0.0;
    bool skip = false;
    if (v.fast) {
      if (rem > 0.0) {
        sy = std::sqrt((yj > 0.0) ? yj : 0.0);
        if (!(sy < 745.0)) { // kFastBoundaryExpLimit
          skip = true;
        }
      }
    } else {
      if (rem > 0.0) {
        const double yv = (yj > 0.0) ? yj : 0.0;
        b_t = bnd.xmax * std::exp(-std::sqrt(yv));
      } else {
        b_t = bnd.K;
      }
      if (!(b_t > 0.0)) {
        skip = true;
      }
    }
    if (skip) {
      total += v.qw[i] * I;
      continue;
    }
    const double v_ = sigma * std::sqrt(t);
    const double dq = std::exp(-q * t);
    const double dr = std::exp(-r * t);
    double dp = 0.0;
    // The integrand's Φ(−d−), Φ(−d+), reused by the derivative accumulators below
    // (review-L3g m3, A0 r6: they were evaluated a second time there). Bit for bit the
    // old Φ(−dm): −dp + v and −(dp − v) both round v − dp (negation is exact and
    // round-to-nearest is symmetric), and I's expression is unchanged.
    double Phm = 0.0;
    double Php = 0.0;
    if (v.fast) {
      const double ld = (rem > 0.0) ? lsx + sy + (r - q) * t : std::log(S * dq / (bnd.K * dr));
      dp = ld / v_ + 0.5 * v_;
      Phm = detail::norm_cdf_erfc(-dp + v_);
      Php = detail::norm_cdf_erfc(-dp);
    } else {
      dp = std::log(S * dq / (b_t * dr)) / v_ + 0.5 * v_;
      Phm = atx::core::norm_cdf(-dp + v_);
      Php = atx::core::norm_cdf(-dp);
    }
    I = 2.0 * zi * (r * bnd.K * dr * Phm - q * S * dq * Php);
    total += v.qw[i] * I;

    // ── derivative accumulators (per unit 2 z_i w_i) ──
    const double dm = dp - v_;
    const double a = r * bnd.K * dr;
    const double b = q * S * dq;
    const double phm = v.fast ? detail::norm_pdf_cody(dm) : atx::core::norm_pdf(dm);
    const double php = v.fast ? detail::norm_pdf_cody(dp) : atx::core::norm_pdf(dp);
    const double wgt = 2.0 * zi * v.qw[i];
    const double u = 1.0 / (S * v_);
    const double G = a * phm - b * php;        // ∂I/∂B · B v / (2z)
    const double qdq = q * dq;
    if (need.s1) {
      aS += wgt * (-a * phm * u - qdq * Php + qdq * php / v_);
    }
    if (need.s2) {
      aSS += wgt * (a * phm * dp / (S * S * v_ * v_) + qdq * php / (S * v_) -
                    qdq * dp * php / (S * v_ * v_));
    }
    if (need.s3) {
      const double S2 = S * S;
      aSSS += wgt * (a * phm / (S2 * S * v_ * v_) * ((1.0 - dm * dp) / v_ - 2.0 * dp) -
                     qdq * php / (S2 * v_) * (dp / v_ + 1.0) -
                     qdq * php / (S2 * v_ * v_) * ((1.0 - dp * dp) / v_ - dp));
    }
    // Boundary motion in σ through s = √y.
    double sp = 0.0;
    double spp = 0.0;
    if (want_sigma && rem > 0.0 && yj > 0.0) {
      const double s = std::sqrt(yj);
      sp = dyj / (2.0 * s);
      if (need.sig2) {
        spp = d2yj / (2.0 * s) - dyj * dyj / (4.0 * s * s * s);
      }
    }
    if (need.sig1) {
      asig += wgt * ((a * dp * phm - b * dm * php) / sigma - G * sp / v_);
    }
    if (need.sig2) {
      const double expl = (a * dm * phm * (dp * dp - 1.0) - b * dp * php * (dm * dm - 1.0) -
                           a * dp * phm + b * dm * php) /
                          (sigma * sigma);
      const double cross_b = 2.0 * sp * (1.0 - dp * dm) * G / (sigma * v_);
      const double bb = ((a * dm * phm - b * dp * php) / (v_ * v_) - G / v_) * sp * sp;
      const double b2 = G * (sp * sp - spp) / v_;
      asig2 += wgt * (expl + cross_b + bb + b2);
    }
    if (need.cross) {
      const double dGdS = -a * dm * phm * u - qdq * php + b * dp * php * u;
      const double expl = (a * u * phm * (1.0 - dp * dm) - qdq * dm * php -
                           b * u * php * (1.0 - dp * dm)) /
                          sigma;
      aSsig += wgt * (expl - sp * dGdS / v_);
    }
    if (need.rate || need.yield) {
      const double rho_d = t / v_; // ∂d±/∂rate = −∂d±/∂yield
      if (need.rate) {
        arate += wgt * (bnd.K * dr * (1.0 - r * t) * Phm - rho_d * G);
      }
      if (need.yield) {
        ayield += wgt * (rho_d * G - S * dq * (1.0 - q * t) * Php);
      }
      if (want_xmax) {
        arate_x += wgt * (G / v_) * xl.d_rate;
        ayield_x += wgt * (G / v_) * xl.d_yield;
      }
      // The carry boundary terms: ∂I/∂B · dB/dθ = −(2z) G s'_θ / v per unit weight, the
      // vega boundary term's shape with s'_θ = y'_θ / (2√y) (B = K at rem <= 0: none).
      if ((bt_rate || bt_yield) && rem > 0.0 && yj > 0.0) {
        const double s2 = 2.0 * std::sqrt(yj);
        if (bt_rate) {
          arate_b += wgt * (-G * (dyj_r / s2) / v_);
        }
        if (bt_yield) {
          ayield_b += wgt * (-G * (dyj_q / s2) / v_);
        }
      }
    }
  }
  total *= half_sqrtT;
  const double prem = (total > 0.0) ? total : 0.0;
  const bool prem_floored = !(total > 0.0);
  if (prem_floored) {
    aS = aSS = aSSS = asig = asig2 = aSsig = arate = ayield = arate_x = ayield_x = 0.0;
    arate_b = ayield_b = 0.0;
  }

  // ── clamps (al_put_price_from_boundary's order) ──
  double price = euro + prem;
  const double intr = K - S;
  bool clamped = false;
  if (intr > price) {
    price = intr;
    clamped = true;
  }
  if (euro > price) {
    price = euro; // unreachable with prem >= 0; kept for the mirror
  }
  if (price < 0.0) {
    price = 0.0;
  }
  out.price = price;
  // The exercise regime (RecordGreeks' semantics): the spot is on the exercise side of
  // the CURRENT edge — al_boundary_now = al_boundary_at(bnd, T), an exact hit on the
  // last collocation node (z = 1) — or the served price is intrinsic to the bundle's
  // own tolerance (american_greeks_al: v0 <= intr + 1e-9 K). Deep in the exercise
  // region the premium decomposition reproduces K − S exactly, so the kernel's clamp
  // is a rounding coin-flip there; just above the edge the decomposition sits within
  // quadrature noise of intrinsic and the clamp flickers along a spot ladder (the
  // served price is piecewise max(decomposition, intrinsic) — no smooth derivative
  // exists). Both cases serve the EXERCISE Greeks with the (unchanged) served price.
  const double yT = bnd.y[nb - 1];
  const double b_now = bnd.xmax * std::exp(-std::sqrt((yT > 0.0) ? yT : 0.0));
  if (need.edge) {
    out.edge_sd = std::log(S / b_now) / (sigma * sqrtT);
  }
  const bool inside = S <= b_now;
  const bool at_intrinsic = intr > 0.0 && price <= intr + 1.0e-9 * K;
  if (clamped || at_intrinsic || inside) {
    out.dS = -1.0;
    out.dSS = out.dSSS = out.dsig = out.dsig2 = out.dSsig = 0.0;
    out.drate = out.dyield = out.drate_xmax = out.dyield_xmax = 0.0;
    out.drate_bt = out.dyield_bt = 0.0;
    out.exercised = true;
    out.floored = !inside;
    return;
  }
  out.exercised = false;
  out.floored = false;
  out.dS = eS + half_sqrtT * aS;
  out.dSS = eSS + half_sqrtT * aSS;
  out.dSSS = eSSS + half_sqrtT * aSSS;
  out.dsig = esig + half_sqrtT * asig;
  out.dsig2 = esig2 + half_sqrtT * asig2;
  out.dSsig = eSsig + half_sqrtT * aSsig;
  out.drate_xmax = half_sqrtT * arate_x;
  out.dyield_xmax = half_sqrtT * ayield_x;
  out.drate_bt = half_sqrtT * arate_b;
  out.dyield_bt = half_sqrtT * ayield_b;
  out.drate = erate + half_sqrtT * arate + out.drate_xmax + out.drate_bt;
  out.dyield = eyield + half_sqrtT * ayield + out.dyield_xmax + out.dyield_bt;
}

// Complete per-field validity without changing the served price or doing another
// numerical evaluation. The mask bits and field array have the public enum order.
void qualify_fields(AlSigmaFamilyGreeks &g, Mask m) noexcept {
  using Reason = AlSigmaFamilyGreekReason;
  g.requested = m;
  const std::array<double *, kAlSigmaFamilyGreekCount> fields = {
      &g.delta, &g.gamma, &g.vega, &g.theta, &g.rho, &g.phi, &g.vanna, &g.volga, &g.charm};
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto bit = static_cast<std::uint16_t>(1u << i);
    if (!m.has(bit)) {
      continue;
    }
    Reason why = Reason::Served;
    if (g.status != AlSigmaFamilyStatus::Ok) {
      why = Reason::Refused;
    } else if ((bit & (Mask::kRho | Mask::kPhi)) != 0 &&
               g.carry != AlSigmaFamilyCarryStatus::Ok) {
      why = g.carry == AlSigmaFamilyCarryStatus::NoCarryBumps
                ? Reason::NoCarryBumps : Reason::CarryBumpsMismatch;
    } else if (g.exercised) {
      why = Reason::Exercised;
    } else if ((bit & (Mask::kGamma | Mask::kTheta | Mask::kCharm)) != 0 &&
               (g.near_edge || std::isnan(g.edge_sd))) {
      why = g.near_edge ? Reason::NearExerciseEdge : Reason::Refused;
    }
    if ((why == Reason::Served || why == Reason::Exercised) &&
        !std::isfinite(*fields[i])) {
      why = Reason::Refused;
    }
    g.reason[i] = why;
    if (why == Reason::Served || why == Reason::Exercised) {
      g.supported.bits |= bit;
    } else {
      *fields[i] = kNaN;
    }
  }
}

// Map the internal put's bundle to the option's Greeks (homogeneity for calls, the
// PDE identity for θ / charm) and fill only what the mask asked for. `carry` is Ok
// when rho / phi were computed with the carry boundary terms; any other value is the
// refusal a requested rho / phi carries (NaN).
[[nodiscard]] AlSigmaFamilyGreeks map_to_option(const Internal &in, Side side, double S_opt,
                                                double r_opt, double q_opt, double sigma,
                                                double Sp, double Kp, Mask m,
                                                AlSigmaFamilyCarryStatus carry) noexcept {
  AlSigmaFamilyGreeks g;
  g.status = AlSigmaFamilyStatus::Ok;
  g.price = in.price;
  g.exercised = in.exercised;
  g.floored = in.floored;
  g.edge_sd = in.edge_sd;
  g.near_edge = !in.exercised && in.edge_sd < kAlFamilyGreekEdgeBand;
  g.boundary_term_included = m.has(Mask::kVega) || m.has(Mask::kVanna) || m.has(Mask::kVolga);
  double delta = in.dS;
  double gamma = in.dSS;
  double speed = in.dSSS;
  double vanna = in.dSsig;
  double rho = in.drate;
  double phi = in.dyield;
  if (side == Side::Call) {
    const double x = Sp / Kp;
    delta = (in.price - Sp * in.dS) / Kp;
    gamma = x * x * in.dSS;
    speed = -x * x * x * in.dSSS - 3.0 * x * x * in.dSS / Kp;
    vanna = (in.dsig - Sp * in.dSsig) / Kp;
    rho = in.dyield;
    phi = in.drate;
    if (in.exercised) {
      delta = 1.0;
      gamma = speed = vanna = 0.0;
    }
  }
  const double vega = in.dsig;
  const double volga = in.dsig2;
  double theta = 0.0;
  double charm = 0.0;
  if (!in.exercised) {
    theta = r_opt * in.price - (r_opt - q_opt) * S_opt * delta -
            0.5 * sigma * sigma * S_opt * S_opt * gamma;
    charm = r_opt * delta - (r_opt - q_opt) * (delta + S_opt * gamma) -
            0.5 * sigma * sigma * (2.0 * S_opt * gamma + S_opt * S_opt * speed);
  }
  if (m.has(Mask::kDelta)) {
    g.delta = delta;
  }
  if (m.has(Mask::kGamma)) {
    g.gamma = gamma;
  }
  if (m.has(Mask::kVega)) {
    g.vega = vega;
  }
  if (m.has(Mask::kTheta)) {
    g.theta = theta;
  }
  if (m.has(Mask::kRho) || m.has(Mask::kPhi)) {
    const bool served = carry == AlSigmaFamilyCarryStatus::Ok;
    g.carry = carry;
    if (m.has(Mask::kRho)) {
      g.rho = served ? rho : kNaN;
    }
    if (m.has(Mask::kPhi)) {
      g.phi = served ? phi : kNaN;
    }
  }
  if (m.has(Mask::kVanna)) {
    g.vanna = vanna;
  }
  if (m.has(Mask::kVolga)) {
    g.volga = volga;
  }
  if (m.has(Mask::kCharm)) {
    g.charm = charm;
  }
  qualify_fields(g, m);
  return g;
}

[[nodiscard]] AlSigmaFamilyGreeks refused(AlSigmaFamilyStatus st, Mask m) noexcept {
  AlSigmaFamilyGreeks g;
  g.status = st;
  g.price = kNaN;
  if (m.has(Mask::kDelta)) {
    g.delta = kNaN;
  }
  if (m.has(Mask::kGamma)) {
    g.gamma = kNaN;
  }
  if (m.has(Mask::kVega)) {
    g.vega = kNaN;
  }
  if (m.has(Mask::kTheta)) {
    g.theta = kNaN;
  }
  if (m.has(Mask::kRho)) {
    g.rho = kNaN;
  }
  if (m.has(Mask::kPhi)) {
    g.phi = kNaN;
  }
  if (m.has(Mask::kVanna)) {
    g.vanna = kNaN;
  }
  if (m.has(Mask::kVolga)) {
    g.volga = kNaN;
  }
  if (m.has(Mask::kCharm)) {
    g.charm = kNaN;
  }
  qualify_fields(g, m);
  return g;
}

// Greeks on a view whose admit tree already said Ok. `cv` non-null: rho / phi served
// with the carry boundary terms; null: a requested rho / phi is refused with `why`.
[[nodiscard]] AlSigmaFamilyGreeks greeks_on_view(const FamilyView &v, double S, double K,
                                                 double sigma, Mask m, const CarryView *cv,
                                                 AlSigmaFamilyCarryStatus why) noexcept {
  const detail::InternalPutCoords c = detail::internal_put_coords(v.side, S, K);
  const Need need = need_of(m, v.side, cv != nullptr);
  Internal in;
  fused_internal(v, c.Sp, c.Kp, sigma, need, cv, in);
  // The option's (r, q): the internal (rate, yield) for a put, swapped for a call.
  const double r_opt = (v.side == Side::Call) ? v.yield : v.rate;
  const double q_opt = (v.side == Side::Call) ? v.rate : v.yield;
  return map_to_option(in, v.side, S, r_opt, q_opt, sigma, c.Sp, c.Kp, m,
                       cv != nullptr ? AlSigmaFamilyCarryStatus::Ok : why);
}

[[nodiscard]] AlSigmaFamilyPriceVega price_vega_on_view(const FamilyView &v, double S, double K,
                                                        double sigma) noexcept {
  const detail::InternalPutCoords c = detail::internal_put_coords(v.side, S, K);
  Need need;
  need.sig1 = true;
  Internal in;
  fused_internal(v, c.Sp, c.Kp, sigma, need, nullptr, in);
  // review-L3g m4 (A0 r6): the exercise regime's vega is 0.0 by construction — say so.
  return AlSigmaFamilyPriceVega{in.price, in.dsig, AlSigmaFamilyStatus::Ok, in.exercised};
}

// The chain / shock column writer.
void store_row(const AlSigmaFamilyGreekColumns &out, std::size_t i,
               const AlSigmaFamilyGreeks &g) noexcept {
  if (!out.price.empty()) {
    out.price[i] = g.price;
  }
  if (!out.delta.empty()) {
    out.delta[i] = g.delta;
  }
  if (!out.gamma.empty()) {
    out.gamma[i] = g.gamma;
  }
  if (!out.vega.empty()) {
    out.vega[i] = g.vega;
  }
  if (!out.theta.empty()) {
    out.theta[i] = g.theta;
  }
  if (!out.rho.empty()) {
    out.rho[i] = g.rho;
  }
  if (!out.phi.empty()) {
    out.phi[i] = g.phi;
  }
  if (!out.vanna.empty()) {
    out.vanna[i] = g.vanna;
  }
  if (!out.volga.empty()) {
    out.volga[i] = g.volga;
  }
  if (!out.charm.empty()) {
    out.charm[i] = g.charm;
  }
  if (!out.supported.empty()) {
    out.supported[i] = g.supported;
  }
  if (!out.reason.empty()) {
    out.reason[i] = g.reason;
  }
  if (!out.edge_sd.empty()) {
    out.edge_sd[i] = g.edge_sd;
  }
  if (!out.flags.empty()) {
    std::uint8_t f = 0;
    if (g.exercised) {
      f |= 1u;
    }
    if (g.floored) {
      f |= 2u;
    }
    if (g.boundary_term_included) {
      f |= 4u;
    }
    if (g.carry == AlSigmaFamilyCarryStatus::NoCarryBumps ||
        g.carry == AlSigmaFamilyCarryStatus::CarryBumpsMismatch) {
      f |= 8u;
    }
    if (g.near_edge) {
      f |= 16u;
    }
    out.flags[i] = f;
  }
}

// A refused row / cell: NaN in every stored column.
void store_refused(const AlSigmaFamilyGreekColumns &out, std::size_t i,
                   AlSigmaFamilyStatus st, Mask mask) noexcept {
  AlSigmaFamilyGreeks g = refused(st, Mask::all());
  // Preserve the legacy all-stored-columns-NaN refusal, but report only the
  // actual request in per-field metadata (unrequested reasons stay NotRequested).
  g.requested = mask;
  for (std::size_t j = 0; j < g.reason.size(); ++j) {
    if (!mask.has(static_cast<std::uint16_t>(1u << j))) {
      g.reason[j] = AlSigmaFamilyGreekReason::NotRequested;
    }
  }
  store_row(out, i, g);
}

[[nodiscard]] bool columns_fit(const AlSigmaFamilyGreekColumns &out, std::size_t n) noexcept {
  const auto fits = [n](std::span<const double> s) { return s.empty() || s.size() == n; };
  return fits(out.price) && fits(out.delta) && fits(out.gamma) && fits(out.vega) &&
         fits(out.theta) && fits(out.rho) && fits(out.phi) && fits(out.vanna) &&
         fits(out.volga) && fits(out.charm) && fits(out.edge_sd) &&
         (out.flags.empty() || out.flags.size() == n) &&
         (out.supported.empty() || out.supported.size() == n) &&
         (out.reason.empty() || out.reason.size() == n);
}

} // namespace

// ── The carry set's state ──────────────────────────────────────────────────────
//
// The base family's identity (checked by value on every call — the set does not
// observe the family) and the series derivatives on the INTERNAL put's axes.
struct AlSigmaFamilyCarryBumps::State {
  bool ok = false;
  Side side = Side::Put;
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  double anchor = 0.0;
  double T = 0.0;
  double r = 0.0;
  double q = 0.0;
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  std::uint16_t n_sigma = 0;
  std::uint16_t n_boundary = 0;
  unsigned nq = 0;
  bool fast = false;
  bool has_rho = false;
  bool has_phi = false;
  bool rho_one_sided = false;
  bool phi_one_sided = false;
  // Sentinels of the base series (the last and the middle collocation rows): a family
  // built on other AlOpts (sweep budget, tolerance) has other y and fails the check.
  double sentinel_last[kMaxSigmaNodes] = {};
  double sentinel_mid[kMaxSigmaNodes] = {};
  // dY_{k,s}/d(internal rate) and /d(internal yield), [k * kMaxSigmaNodes + s].
  bool has_rate_int = false;
  bool has_yield_int = false;
  double d_rate[amer::kAlMaxNodes * kMaxSigmaNodes] = {};
  double d_yield[amer::kAlMaxNodes * kMaxSigmaNodes] = {};
};

// ── The family's one friend: const reads of the prepared state ─────────────────
//
// Declared in american.hpp (forward) and defined here only. Every member is a const
// read; nothing writes through it. A moved-from family has no state (alive() false);
// a family whose admit() refused is never viewed.
struct sigma_family_detail::FamilyAccess {
  [[nodiscard]] static bool alive(const AlSigmaFamily &f) noexcept { return f.st_ != nullptr; }
  // Precondition: alive(f).
  [[nodiscard]] static AlSigmaFamilyStatus admit(const AlSigmaFamily &f, double S, double K,
                                                 double sigma) noexcept {
    return f.st_->admit(S, K, sigma);
  }
  // Precondition: admit(f, S, K, sigma) == Ok.
  [[nodiscard]] static double price_at(const AlSigmaFamily &f, double S, double K,
                                       double sigma) noexcept {
    return f.st_->price_at(S, K, sigma);
  }
  // Precondition: admit(f, ...) == Ok (so the state is prepared and the geometry live).
  [[nodiscard]] static FamilyView view(const AlSigmaFamily &f) noexcept {
    const AlSigmaFamily::State &st = *f.st_;
    FamilyView v;
    v.side = st.side;
    v.axis = st.axis;
    v.u_lo = st.u_lo;
    v.u_hi = st.u_hi;
    v.n_sigma = st.n_sigma;
    v.n_boundary = st.n_boundary;
    v.rate = st.rate;
    v.yield = st.yield;
    v.sz = st.sz;
    v.sw = st.sw;
    v.series = st.series;
    v.grid = &st.grid;
    v.qx = st.ws->qx_price;
    v.qw = st.ws->qw_price;
    v.nq = st.ws->n_quad_price;
    v.fast = st.ws->fast;
    return v;
  }
  // Copies the prepared state into a detached copy; false (nothing written) when the
  // family is not prepared or its geometry was re-prepared.
  [[nodiscard]] static bool detach_into(const AlSigmaFamily &f,
                                        AlSigmaFamilyDetached::State &c) noexcept;
  // Builds the carry set of `base` into `c` (AlSigmaFamilyCarryBumps::prepare's body).
  [[nodiscard]] static Status prepare_carry(const AlSigmaFamily &base, bool rho, bool phi,
                                            AlSigmaFamilyCarryBumps::State &c);
  // The carry view for (f, carry, mask): Ok and `cv` filled when `carry` was built from
  // `f` with every requested axis; otherwise the refusal (cv untouched).
  // Precondition: admit(f, ...) == Ok.
  [[nodiscard]] static AlSigmaFamilyCarryStatus carry_view(const AlSigmaFamily &f,
                                                           const AlSigmaFamilyCarryBumps &carry,
                                                           AlSigmaFamilyGreeksMask m,
                                                           CarryView &cv) noexcept;
};

using sigma_family_detail::FamilyAccess;

// ── Public entries on the family ──────────────────────────────────────────────

AlSigmaFamilyGreeks family_greeks(const AlSigmaFamily &fam, double S, double K, double sigma,
                                  AlSigmaFamilyGreeksMask mask) noexcept {
  if (!FamilyAccess::alive(fam)) {
    return refused(AlSigmaFamilyStatus::NotPrepared, mask);
  }
  const AlSigmaFamilyStatus a = FamilyAccess::admit(fam, S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return refused(a, mask);
  }
  if (mask.bits == 0) {
    // Price-only IS price(): the same quadrature, nothing else.
    AlSigmaFamilyGreeks g;
    g.status = AlSigmaFamilyStatus::Ok;
    g.price = FamilyAccess::price_at(fam, S, K, sigma);
    return g;
  }
  return greeks_on_view(FamilyAccess::view(fam), S, K, sigma, mask, nullptr,
                        AlSigmaFamilyCarryStatus::NoCarryBumps);
}

AlSigmaFamilyGreeks family_greeks(const AlSigmaFamily &fam, const AlSigmaFamilyCarryBumps &carry,
                                  double S, double K, double sigma,
                                  AlSigmaFamilyGreeksMask mask) noexcept {
  if (!mask.has(Mask::kRho) && !mask.has(Mask::kPhi)) {
    return family_greeks(fam, S, K, sigma, mask); // the carry set has nothing to add
  }
  if (!FamilyAccess::alive(fam)) {
    return refused(AlSigmaFamilyStatus::NotPrepared, mask);
  }
  const AlSigmaFamilyStatus a = FamilyAccess::admit(fam, S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return refused(a, mask);
  }
  CarryView cv;
  const AlSigmaFamilyCarryStatus cs = FamilyAccess::carry_view(fam, carry, mask, cv);
  return greeks_on_view(FamilyAccess::view(fam), S, K, sigma, mask,
                        cs == AlSigmaFamilyCarryStatus::Ok ? &cv : nullptr, cs);
}

AlSigmaFamilyPriceVega family_price_and_vega(const AlSigmaFamily &fam, double S, double K,
                                             double sigma) noexcept {
  if (!FamilyAccess::alive(fam)) {
    return AlSigmaFamilyPriceVega{kNaN, kNaN, AlSigmaFamilyStatus::NotPrepared};
  }
  const AlSigmaFamilyStatus a = FamilyAccess::admit(fam, S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return AlSigmaFamilyPriceVega{kNaN, kNaN, a};
  }
  return price_vega_on_view(FamilyAccess::view(fam), S, K, sigma);
}

Status family_chain_greeks(const AlSigmaFamily *put, const AlSigmaFamily *call, double S,
                           std::span<const double> K, std::span<const double> sigma,
                           std::span<const Side> side, AlSigmaFamilyGreeksMask mask,
                           const AlSigmaFamilyGreekColumns &out,
                           std::span<AlSigmaFamilyStatus> status,
                           const AlSigmaFamilyCarryBumps *put_carry,
                           const AlSigmaFamilyCarryBumps *call_carry) {
  const std::size_t n = K.size();
  if (sigma.size() != n || side.size() != n || status.size() != n || !columns_fit(out, n)) {
    // Keep the refusal allocation-free (same small-string message as sigma/q).
    return Err(ErrorCode::InvalidArgument, "span lengths");
  }
  for (std::size_t i = 0; i < n; ++i) {
    const AlSigmaFamily *fam = (side[i] == Side::Call) ? call : put;
    if (fam == nullptr) {
      status[i] = AlSigmaFamilyStatus::NotPrepared;
      store_refused(out, i, status[i], mask);
      continue;
    }
    const AlSigmaFamilyCarryBumps *carry = (side[i] == Side::Call) ? call_carry : put_carry;
    const AlSigmaFamilyGreeks g = carry != nullptr
                                      ? family_greeks(*fam, *carry, S, K[i], sigma[i], mask)
                                      : family_greeks(*fam, S, K[i], sigma[i], mask);
    status[i] = g.status;
    if (g.status != AlSigmaFamilyStatus::Ok) {
      store_refused(out, i, g.status, mask);
      continue;
    }
    store_row(out, i, g);
  }
  return Ok();
}

Status family_shock_grid(const AlSigmaFamily &fam, double S, double K, double sigma,
                         std::span<const double> spot_pct, std::span<const double> vol_bump,
                         AlSigmaFamilyGreeksMask mask, const AlSigmaFamilyGreekColumns &out,
                         std::span<AlSigmaFamilyStatus> status) {
  const std::size_t n = spot_pct.size() * vol_bump.size();
  if (status.size() != n || !columns_fit(out, n)) {
    return Err(ErrorCode::InvalidArgument, "span lengths");
  }
  for (std::size_t i = 0; i < spot_pct.size(); ++i) {
    const double Si = S * (1.0 + spot_pct[i]);
    for (std::size_t j = 0; j < vol_bump.size(); ++j) {
      const std::size_t idx = i * vol_bump.size() + j;
      const AlSigmaFamilyGreeks g = family_greeks(fam, Si, K, sigma + vol_bump[j], mask);
      status[idx] = g.status;
      if (g.status != AlSigmaFamilyStatus::Ok) {
        store_refused(out, idx, g.status, mask);
        continue;
      }
      store_row(out, idx, g);
    }
  }
  return Ok();
}

// ── Detached serve copy ───────────────────────────────────────────────────────

struct AlSigmaFamilyDetached::State {
  bool ok = false;
  Side side = Side::Put;
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double u_lo = 0.0;
  double u_hi = 0.0;
  std::uint16_t n_sigma = 0;
  std::uint16_t n_boundary = 0;
  double rate = 0.0;
  double yield = 0.0;
  // The workspace header the premium quadrature reads. qx / qw point at the
  // process-wide Gauss-Legendre tables (american.cpp binds every workspace to them),
  // so they outlive any geometry.
  const double *qx = nullptr;
  const double *qw = nullptr;
  unsigned nq = 0;
  bool fast = false;
  amer::AlBoundary grid{};
  double sz[kMaxSigmaNodes] = {};
  double sw[kMaxSigmaNodes] = {};
  double series[amer::kAlMaxNodes * kMaxSigmaNodes] = {};

  [[nodiscard]] AlSigmaFamilyStatus admit(double S, double K, double sigma) const noexcept {
    if (!ok) {
      return AlSigmaFamilyStatus::NotPrepared;
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
  [[nodiscard]] FamilyView view() const noexcept {
    FamilyView v;
    v.side = side;
    v.axis = axis;
    v.u_lo = u_lo;
    v.u_hi = u_hi;
    v.n_sigma = n_sigma;
    v.n_boundary = n_boundary;
    v.rate = rate;
    v.yield = yield;
    v.sz = sz;
    v.sw = sw;
    v.series = series;
    v.grid = &grid;
    v.qx = qx;
    v.qw = qw;
    v.nq = nq;
    v.fast = fast;
    return v;
  }
};

AlSigmaFamilyDetached::AlSigmaFamilyDetached() = default;
AlSigmaFamilyDetached::~AlSigmaFamilyDetached() = default;
AlSigmaFamilyDetached::AlSigmaFamilyDetached(AlSigmaFamilyDetached &&) noexcept = default;
AlSigmaFamilyDetached &AlSigmaFamilyDetached::operator=(AlSigmaFamilyDetached &&) noexcept = default;

std::size_t AlSigmaFamilyDetached::state_bytes() noexcept { return sizeof(State); }
bool AlSigmaFamilyDetached::prepared() const noexcept { return st_ && st_->ok; }
double AlSigmaFamilyDetached::sigma_lo() const noexcept { return prepared() ? st_->sigma_lo : kNaN; }
double AlSigmaFamilyDetached::sigma_hi() const noexcept { return prepared() ? st_->sigma_hi : kNaN; }
Side AlSigmaFamilyDetached::side() const noexcept { return st_ ? st_->side : Side::Call; }

AlSigmaFamilyPrice AlSigmaFamilyDetached::price(double S, double K, double sigma) const noexcept {
  if (!st_) {
    return AlSigmaFamilyPrice{kNaN, AlSigmaFamilyStatus::NotPrepared};
  }
  const AlSigmaFamilyStatus a = st_->admit(S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return AlSigmaFamilyPrice{kNaN, a};
  }
  const FamilyView v = st_->view();
  const detail::InternalPutCoords c = detail::internal_put_coords(v.side, S, K);
  Internal in;
  fused_internal(v, c.Sp, c.Kp, sigma, Need{}, nullptr, in);
  return AlSigmaFamilyPrice{in.price, AlSigmaFamilyStatus::Ok};
}

AlSigmaFamilyGreeks AlSigmaFamilyDetached::greeks(double S, double K, double sigma,
                                                  AlSigmaFamilyGreeksMask mask) const noexcept {
  if (!st_) {
    return refused(AlSigmaFamilyStatus::NotPrepared, mask);
  }
  const AlSigmaFamilyStatus a = st_->admit(S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return refused(a, mask);
  }
  return greeks_on_view(st_->view(), S, K, sigma, mask, nullptr,
                        AlSigmaFamilyCarryStatus::NoCarryBumps);
}

AlSigmaFamilyPriceVega AlSigmaFamilyDetached::price_and_vega(double S, double K,
                                                             double sigma) const noexcept {
  if (!st_) {
    return AlSigmaFamilyPriceVega{kNaN, kNaN, AlSigmaFamilyStatus::NotPrepared};
  }
  const AlSigmaFamilyStatus a = st_->admit(S, K, sigma);
  if (a != AlSigmaFamilyStatus::Ok) {
    return AlSigmaFamilyPriceVega{kNaN, kNaN, a};
  }
  return price_vega_on_view(st_->view(), S, K, sigma);
}

bool sigma_family_detail::FamilyAccess::detach_into(const AlSigmaFamily &f,
                                                    AlSigmaFamilyDetached::State &c) noexcept {
  if (f.st_ == nullptr) {
    return false;
  }
  const AlSigmaFamily::State &st = *f.st_;
  if (!st.ok || st.g == nullptr || st.g->generation != st.generation || st.ws == nullptr) {
    return false;
  }
  c.side = st.side;
  c.axis = st.axis;
  c.sigma_lo = st.sigma_lo;
  c.sigma_hi = st.sigma_hi;
  c.u_lo = st.u_lo;
  c.u_hi = st.u_hi;
  c.n_sigma = st.n_sigma;
  c.n_boundary = st.n_boundary;
  c.rate = st.rate;
  c.yield = st.yield;
  c.qx = st.ws->qx_price;
  c.qw = st.ws->qw_price;
  c.nq = st.ws->n_quad_price;
  c.fast = st.ws->fast;
  c.grid = st.grid;
  for (unsigned s = 0; s < kMaxSigmaNodes; ++s) {
    c.sz[s] = st.sz[s];
    c.sw[s] = st.sw[s];
  }
  for (std::size_t i = 0; i < amer::kAlMaxNodes * kMaxSigmaNodes; ++i) {
    c.series[i] = st.series[i];
  }
  c.ok = true;
  return true;
}

AlSigmaFamilyDetached family_detach(const AlSigmaFamily &fam) {
  AlSigmaFamilyDetached d;
  d.st_ = std::make_unique<AlSigmaFamilyDetached::State>();
  if (!FamilyAccess::detach_into(fam, *d.st_)) {
    d.st_->ok = false; // unprepared copy: every call refuses with NotPrepared
  }
  return d;
}

// ── The carry set: second families at r ± h, q ± h ────────────────────────────

AlSigmaFamilyCarryBumps::AlSigmaFamilyCarryBumps() = default;
AlSigmaFamilyCarryBumps::~AlSigmaFamilyCarryBumps() = default;
AlSigmaFamilyCarryBumps::AlSigmaFamilyCarryBumps(AlSigmaFamilyCarryBumps &&) noexcept = default;
AlSigmaFamilyCarryBumps &
AlSigmaFamilyCarryBumps::operator=(AlSigmaFamilyCarryBumps &&) noexcept = default;

std::size_t AlSigmaFamilyCarryBumps::state_bytes() noexcept { return sizeof(State); }
bool AlSigmaFamilyCarryBumps::prepared() const noexcept { return st_ && st_->ok; }
bool AlSigmaFamilyCarryBumps::has_rho() const noexcept { return prepared() && st_->has_rho; }
bool AlSigmaFamilyCarryBumps::has_phi() const noexcept { return prepared() && st_->has_phi; }
bool AlSigmaFamilyCarryBumps::rho_one_sided() const noexcept {
  return has_rho() && st_->rho_one_sided;
}
bool AlSigmaFamilyCarryBumps::phi_one_sided() const noexcept {
  return has_phi() && st_->phi_one_sided;
}

Status AlSigmaFamilyCarryBumps::prepare(const AlSigmaFamily &base, AlSigmaFamilyGreeksMask axes) {
  st_.reset(); // strong guarantee: unprepared unless this call succeeds
  const bool rho = axes.has(Mask::kRho);
  const bool phi = axes.has(Mask::kPhi);
  if (!rho && !phi) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaFamilyCarryBumps::prepare: axes selects neither kRho nor kPhi");
  }
  auto fresh = std::make_unique<State>();
  Status s = FamilyAccess::prepare_carry(base, rho, phi, *fresh);
  if (!s) {
    return s;
  }
  fresh->ok = true;
  st_ = std::move(fresh);
  return Ok();
}

Status sigma_family_detail::FamilyAccess::prepare_carry(const AlSigmaFamily &base, bool rho,
                                                        bool phi,
                                                        AlSigmaFamilyCarryBumps::State &c) {
  if (base.st_ == nullptr) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaFamilyCarryBumps::prepare: the base family is moved-from");
  }
  const AlSigmaFamily::State &st = *base.st_;
  if (!st.ok || st.g == nullptr || st.g->generation != st.generation || st.ws == nullptr) {
    return Err(ErrorCode::InvalidArgument,
               "AlSigmaFamilyCarryBumps::prepare: the base family is not prepared (or its "
               "geometry was re-prepared)");
  }
  const AlExpiryGeometry::Impl &g = *st.g;
  const double h = kAlSigmaFamilyCarryBump;
  constexpr std::size_t kSeries = amer::kAlMaxNodes * kMaxSigmaNodes;
  // One bumped family (same opts, side, anchor, box, nodes, axis) -> its σ-series.
  const auto bumped = [&](double dr, double dq, std::vector<double> &out) -> Status {
    const AlExpiryGeometry gb = prepare_al_geometry(g.T, g.r + dr, g.q + dq, g.opts);
    AlSigmaFamily fb(gb, st.side, st.anchor);
    Status ps = fb.prepare(st.sigma_lo, st.sigma_hi, st.n_sigma, st.axis);
    if (!ps) {
      return ps;
    }
    const AlSigmaFamily::State &sb = *fb.st_;
    bool same_grid = sb.n_boundary == st.n_boundary && sb.n_sigma == st.n_sigma &&
                     sb.grid.T == st.grid.T && sb.ws->n_quad_price == st.ws->n_quad_price &&
                     sb.ws->fast == st.ws->fast;
    for (unsigned k = 0; same_grid && k < st.n_boundary; ++k) {
      same_grid = sb.grid.z[k] == st.grid.z[k];
    }
    if (!same_grid) {
      return Err(ErrorCode::NotImplemented,
                 "AlSigmaFamilyCarryBumps::prepare: a bumped family's collocation grid differs "
                 "from the base's");
    }
    out.assign(sb.series, sb.series + kSeries);
    return Ok();
  };
  // dY/dθ of one option axis into d: central when the down bump prices, else (the A5
  // rule: the down bump left the American regime -> NotImplemented) forward vs the base.
  std::vector<double> up;
  std::vector<double> dn;
  const auto axis_diff = [&](double ur, double uq, double *d, bool &one_sided) -> Status {
    Status su = bumped(ur, uq, up);
    if (!su) {
      return su;
    }
    Status sd = bumped(-ur, -uq, dn);
    if (sd) {
      for (std::size_t i = 0; i < kSeries; ++i) {
        d[i] = (up[i] - dn[i]) / (2.0 * h);
      }
      one_sided = false;
      return Ok();
    }
    if (sd.error().code() != ErrorCode::NotImplemented) {
      return sd;
    }
    for (std::size_t i = 0; i < kSeries; ++i) {
      d[i] = (up[i] - st.series[i]) / h;
    }
    one_sided = true;
    return Ok();
  };
  // Option r is a put's internal RATE and a call's internal YIELD; option q the reverse.
  const bool call = st.side == Side::Call;
  if (rho) {
    Status s = axis_diff(h, 0.0, call ? c.d_yield : c.d_rate, c.rho_one_sided);
    if (!s) {
      return s;
    }
    (call ? c.has_yield_int : c.has_rate_int) = true;
    c.has_rho = true;
  }
  if (phi) {
    Status s = axis_diff(0.0, h, call ? c.d_rate : c.d_yield, c.phi_one_sided);
    if (!s) {
      return s;
    }
    (call ? c.has_rate_int : c.has_yield_int) = true;
    c.has_phi = true;
  }
  c.side = st.side;
  c.axis = st.axis;
  c.anchor = st.anchor;
  c.T = g.T;
  c.r = g.r;
  c.q = g.q;
  c.sigma_lo = st.sigma_lo;
  c.sigma_hi = st.sigma_hi;
  c.n_sigma = st.n_sigma;
  c.n_boundary = st.n_boundary;
  c.nq = st.ws->n_quad_price;
  c.fast = st.ws->fast;
  const unsigned last = st.n_boundary - 1u;
  const unsigned mid = st.n_boundary / 2u;
  for (unsigned s = 0; s < kMaxSigmaNodes; ++s) {
    c.sentinel_last[s] = st.series[last * kMaxSigmaNodes + s];
    c.sentinel_mid[s] = st.series[mid * kMaxSigmaNodes + s];
  }
  return Ok();
}

AlSigmaFamilyCarryStatus
sigma_family_detail::FamilyAccess::carry_view(const AlSigmaFamily &f,
                                              const AlSigmaFamilyCarryBumps &carry,
                                              AlSigmaFamilyGreeksMask m, CarryView &cv) noexcept {
  if (!carry.prepared()) {
    return AlSigmaFamilyCarryStatus::CarryBumpsMismatch;
  }
  const AlSigmaFamilyCarryBumps::State &c = *carry.st_;
  const AlSigmaFamily::State &st = *f.st_;
  if ((m.has(Mask::kRho) && !c.has_rho) || (m.has(Mask::kPhi) && !c.has_phi)) {
    return AlSigmaFamilyCarryStatus::CarryBumpsMismatch;
  }
  bool same = c.side == st.side && c.axis == st.axis && c.anchor == st.anchor &&
              c.T == st.g->T && c.r == st.g->r && c.q == st.g->q && c.sigma_lo == st.sigma_lo &&
              c.sigma_hi == st.sigma_hi && c.n_sigma == st.n_sigma &&
              c.n_boundary == st.n_boundary && c.nq == st.ws->n_quad_price &&
              c.fast == st.ws->fast;
  const unsigned last = st.n_boundary - 1u;
  const unsigned mid = st.n_boundary / 2u;
  for (unsigned s = 0; same && s < st.n_sigma; ++s) {
    same = c.sentinel_last[s] == st.series[last * kMaxSigmaNodes + s] &&
           c.sentinel_mid[s] == st.series[mid * kMaxSigmaNodes + s];
  }
  if (!same) {
    return AlSigmaFamilyCarryStatus::CarryBumpsMismatch;
  }
  cv.d_rate = c.has_rate_int ? c.d_rate : nullptr;
  cv.d_yield = c.has_yield_int ? c.d_yield : nullptr;
  return AlSigmaFamilyCarryStatus::Ok;
}

// ── Measurement hook (private header): rho / phi with and without the xmax term ──

namespace sigma_family_detail {

FamilyRhoVariants family_rho_variants(const AlSigmaFamily &fam, double S, double K,
                                      double sigma) noexcept {
  FamilyRhoVariants out;
  if (!FamilyAccess::alive(fam)) {
    out.status = AlSigmaFamilyStatus::NotPrepared;
    return out;
  }
  out.status = FamilyAccess::admit(fam, S, K, sigma);
  if (out.status != AlSigmaFamilyStatus::Ok) {
    return out;
  }
  const FamilyView v = FamilyAccess::view(fam);
  const detail::InternalPutCoords c = detail::internal_put_coords(v.side, S, K);
  Need need;
  need.rate = true;
  need.yield = true;
  Internal in;
  fused_internal(v, c.Sp, c.Kp, sigma, need, nullptr, in);
  const bool call = v.side == Side::Call;
  const double rho_y = call ? in.dyield : in.drate;
  const double phi_y = call ? in.drate : in.dyield;
  const double rho_x = call ? in.dyield_xmax : in.drate_xmax;
  const double phi_x = call ? in.drate_xmax : in.dyield_xmax;
  out.rho_frozen_y = rho_y;
  out.phi_frozen_y = phi_y;
  out.rho_frozen_b = rho_y - rho_x;
  out.phi_frozen_b = phi_y - phi_x;
  out.exercised = in.exercised;
  return out;
}

} // namespace sigma_family_detail

} // namespace atx::vol
