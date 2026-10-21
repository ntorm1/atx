#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"

// L3g family-greeks (2026-10-20). Analytic Greeks on AlSigmaFamily. Two kinds of
// claim are pinned here, exactly as L2h / al-speed pinned theirs (2-figure round-ups
// of the measured worst, each failing on revert):
//   * FORMULA claims — against a Richardson finite difference of the FAMILY's own
//     price map (smooth, deterministic, no solve noise): a formula error shows up as
//     a relative gap far above the stencil's own ~1e-10 class;
//   * DISTANCE claims — against the Reference bundle american_greeks_al on the same
//     geometry (bit-identical to the cold bundle, LEDGER 2026-10-19): interpolation
//     error of the σ-series plus the bundle's own stencil truncation (LEDGER
//     2026-10-16: the bundle's hS = 1e-3 S stencil is wrong near the kink; its
//     volga / vanna are 1e-3-class limited).
// Bounds are EMPIRICAL MAXIMA over the sets below (SPY-like 23 d slice, the profile
// grid's wide box, the deep-carry corner), NOT an enclosure; the Reference kernel's
// own convergence error is not included. The wider tables are in
// sprints/2026-10-20-L3g-family-greeks-sprint.md.

namespace {

using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;
using atx::vol::al_fast_profile;
using atx::vol::al_price_pack;
using atx::vol::AlContract;
using atx::vol::AlExpiryGeometry;
using atx::vol::AlSigmaFamily;
using atx::vol::AlSigmaFamilyAxis;
using atx::vol::AlSigmaFamilyChord;
using atx::vol::AlSigmaFamilyDetached;
using atx::vol::AlSigmaFamilyGreekColumns;
using atx::vol::AlSigmaFamilyGreeks;
using atx::vol::AlSigmaFamilyGreeksMask;
using atx::vol::AlSigmaFamilyPrice;
using atx::vol::AlSigmaFamilyPriceVega;
using atx::vol::AlSigmaFamilyStatus;
using atx::vol::american_greeks_al;
using atx::vol::AmericanGreeks;
using atx::vol::andersen_lake;
using atx::vol::family_chain_greeks;
using atx::vol::family_detach;
using atx::vol::family_greeks;
using atx::vol::family_price_and_vega;
using atx::vol::family_shock_grid;
using atx::vol::kAlPackSigmaBump;
using atx::vol::kAlSigmaFamilyMinSigma;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;

using Mask = AlSigmaFamilyGreeksMask;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint16_t kNodes = 12;

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] std::string describe(double S, double K, double T, double sigma, double r, double q,
                                   Side side) {
  std::ostringstream os;
  os << std::setprecision(17) << (side == Side::Call ? "call" : "put") << " S=" << S << " K=" << K
     << " T=" << T << " sigma=" << sigma << " r=" << r << " q=" << q;
  return os.str();
}

// Relative gap with a floor: |a − b| / max(|b|, floor).
[[nodiscard]] double rel_gap(double a, double b, double floor) noexcept {
  return std::abs(a - b) / std::max(std::abs(b), floor);
}

// The family's own σ-nodes on [lo, hi] (LogSigma axis, the default).
[[nodiscard]] std::vector<double> log_nodes(double lo, double hi, unsigned n) {
  std::vector<double> v;
  const double ulo = std::log(lo);
  const double uhi = std::log(hi);
  for (unsigned s = 0; s < n; ++s) {
    double z = -std::cos(3.14159265358979323846 * static_cast<double>(s) /
                         static_cast<double>(n - 1));
    if (s == 0) {
      z = -1.0;
    }
    if (s == n - 1) {
      z = 1.0;
    }
    v.push_back(std::exp(ulo + 0.5 * (uhi - ulo) * (z + 1.0)));
  }
  return v;
}

// Midpoints between adjacent nodes (where the interpolation error is largest).
[[nodiscard]] std::vector<double> off_nodes(double lo, double hi, unsigned n) {
  const std::vector<double> nodes = log_nodes(lo, hi, n);
  std::vector<double> v;
  for (std::size_t i = 1; i < nodes.size(); ++i) {
    v.push_back(0.5 * (nodes[i - 1] + nodes[i]));
  }
  return v;
}

// One (expiry, side) family and the contracts to check.
struct Case {
  double S;
  double anchor;
  double T;
  double r;
  double q;
  Side side;
  double lo;
  double hi;
  std::vector<double> strikes;
  std::vector<double> sigmas;
};

// The SPY-like 23 d slice (S 773, the 2026-11-06 carry): nodes and off-node σ.
[[nodiscard]] std::vector<Case> spy_slice() {
  std::vector<Case> v;
  std::vector<double> sig = {0.12, 0.16, 0.2, 0.27, 0.35};
  for (const double s : off_nodes(0.12, 0.35, kNodes)) {
    sig.push_back(s);
  }
  std::vector<double> strikes;
  for (const double m : {0.80, 0.90, 0.95, 0.98, 1.0, 1.02, 1.05, 1.10, 1.20}) {
    strikes.push_back(773.0 * m);
  }
  for (const Side side : {Side::Put, Side::Call}) {
    v.push_back(Case{773.0, 773.0, 0.068, 0.045, 0.0016, side, 0.12, 0.35, strikes, sig});
  }
  return v;
}

// The AlFastProfile grid regrouped by (T, r, q, side) over the wide box [0.08, 0.7].
[[nodiscard]] std::vector<Case> profile_grid() {
  std::vector<Case> v;
  std::vector<double> sig = {0.08, 0.25, 0.7};
  const std::vector<double> off = off_nodes(0.08, 0.7, kNodes);
  sig.push_back(off[2]);
  sig.push_back(off[6]);
  for (const Side side : {Side::Put, Side::Call}) {
    for (const double T : {0.02, 0.1, 0.5, 1.5, 4.0}) {
      for (const double r : {0.005, 0.045, 0.1}) {
        for (const double q : {0.0, 0.02, 0.09}) {
          v.push_back(Case{100.0, 100.0, T, r, q, side, 0.08, 0.7,
                           {70.0, 90.0, 100.0, 110.0, 140.0}, sig});
        }
      }
    }
  }
  return v;
}

// Screen's worst corner: long-dated, low-vol, deep-carry puts at the money.
[[nodiscard]] std::vector<Case> deep_carry() {
  std::vector<Case> v;
  std::vector<double> sig = {0.05, 0.065, 0.08, 0.1, 0.12};
  const std::vector<double> off = off_nodes(0.05, 0.12, kNodes);
  sig.push_back(off[1]);
  sig.push_back(off[5]);
  for (const double T : {2.0, 3.0, 4.0, 5.0}) {
    for (const double r : {0.05, 0.075, 0.1}) {
      for (const double q : {0.0, 0.015, 0.03}) {
        v.push_back(Case{100.0, 100.0, T, r, q, Side::Put, 0.05, 0.12, {100.0}, sig});
      }
    }
  }
  return v;
}

// Richardson-extrapolated central differences of the family's own price map.
struct FamilyFd {
  double delta = kNaN;
  double gamma = kNaN;
  double speed = kNaN;
  double vega = kNaN;
  double volga = kNaN;
  double vanna = kNaN;
  bool ok = false;
};

// The expiry a family was built on is not exposed by the family; the tests carry it.
double g_case_T = 0.0;
[[nodiscard]] double T_of(const AlSigmaFamily & /*f*/) noexcept { return g_case_T; }

// The exercise edge in spot terms (where the `exercised` flag flips), by bisection on
// the flag: below it for a put / above it for a call the family serves the exercise
// Greeks. Rows whose finite-difference stencil reaches within `reach` of the edge are
// excluded from the reference comparisons (no FD reference is valid across the
// kink — LEDGER 2026-10-16), so the pinned bounds describe the smooth region.
[[nodiscard]] double edge_of(const AlSigmaFamily &f, double S, double K, double sigma, Side side) {
  const auto ex = [&](double s) {
    return family_greeks(f, s, K, sigma, Mask{Mask::kDelta}).exercised;
  };
  double in = side == Side::Put ? 0.2 * S : 5.0 * S; // exercise side
  double out = S;                                     // the query itself
  if (ex(out)) {
    return out;
  }
  if (!ex(in)) {
    return side == Side::Put ? 0.0 : std::numeric_limits<double>::infinity(); // no edge in range
  }
  for (int it = 0; it < 60; ++it) {
    const double mid = 0.5 * (in + out);
    (ex(mid) ? in : out) = mid;
  }
  return 0.5 * (in + out);
}

[[nodiscard]] bool near_edge(const AlSigmaFamily &f, double S, double K, double sigma, Side side,
                             double reach) {
  const double e = edge_of(f, S, K, sigma, side);
  return std::abs(S - e) <= reach;
}

[[nodiscard]] double fam_price(const AlSigmaFamily &f, double S, double K, double sigma,
                               bool &ok) {
  const AlSigmaFamilyPrice p = f.price(S, K, sigma);
  ok = ok && p.status == AlSigmaFamilyStatus::Ok;
  return p.price;
}

[[nodiscard]] FamilyFd family_fd(const AlSigmaFamily &f, double S, double K, double sigma) {
  FamilyFd out;
  bool ok = true;
  // Spot step scaled to the diffusion width S σ √T (a short-dated low-σ price is
  // sharply curved: a fixed 1e-3 S step would leave the REFERENCE's own Richardson
  // truncation above the formula class); vol step 1e-3 absolute.
  const double hS = std::min(1.0e-3 * S, 0.02 * S * sigma * std::sqrt(T_of(f)));
  const double hv = 5.0e-4;
  const auto P = [&](double s, double sig) { return fam_price(f, s, K, sig, ok); };
  const double P0 = P(S, sigma);
  // Δ, Γ, speed at h and h/2.
  const auto dgs = [&](double h, double &d, double &g, double &sp) {
    const double pp = P(S + h, sigma), pm = P(S - h, sigma);
    const double pp2 = P(S + 2.0 * h, sigma), pm2 = P(S - 2.0 * h, sigma);
    d = (pp - pm) / (2.0 * h);
    g = (pp - 2.0 * P0 + pm) / (h * h);
    sp = (pp2 - 2.0 * pp + 2.0 * pm - pm2) / (2.0 * h * h * h);
  };
  double d1, g1, s1, d2, g2, s2;
  dgs(hS, d1, g1, s1);
  dgs(0.5 * hS, d2, g2, s2);
  out.delta = (4.0 * d2 - d1) / 3.0;
  out.gamma = (4.0 * g2 - g1) / 3.0;
  out.speed = (4.0 * s2 - s1) / 3.0;
  const auto vv = [&](double h, double &ve, double &vo) {
    const double pp = P(S, sigma + h), pm = P(S, sigma - h);
    ve = (pp - pm) / (2.0 * h);
    vo = (pp - 2.0 * P0 + pm) / (h * h);
  };
  double ve1, vo1, ve2, vo2;
  vv(hv, ve1, vo1);
  vv(0.5 * hv, ve2, vo2);
  out.vega = (4.0 * ve2 - ve1) / 3.0;
  out.volga = (4.0 * vo2 - vo1) / 3.0;
  const auto vn = [&](double hs, double hsig) {
    return (P(S + hs, sigma + hsig) - P(S + hs, sigma - hsig) - P(S - hs, sigma + hsig) +
            P(S - hs, sigma - hsig)) /
           (4.0 * hs * hsig);
  };
  const double vn1 = vn(hS, hv);
  const double vn2 = vn(0.5 * hS, 0.5 * hv);
  out.vanna = (4.0 * vn2 - vn1) / 3.0;
  out.ok = ok;
  return out;
}

// Worst relative gap bookkeeping.
struct Worst {
  double rel = 0.0;
  std::string at;
  std::size_t n = 0;
  void add(double r, const std::string &where) {
    ++n;
    if (r > rel) {
      rel = r;
      at = where;
    }
  }
};

[[nodiscard]] std::vector<Case> all_cases() {
  std::vector<Case> v = spy_slice();
  for (const Case &c : profile_grid()) {
    v.push_back(c);
  }
  for (const Case &c : deep_carry()) {
    v.push_back(c);
  }
  return v;
}

// ── Price identity ────────────────────────────────────────────────────────────

TEST(AlSigmaFamilyGreeks, PriceFieldIsTheFamilyPriceToTheBit) {
  // The fused kernel accumulates the premium quadrature expression for expression
  // with the kernel, so the price it serves beside the Greeks — and the detached
  // copy's price — is AlSigmaFamily::price() to the bit, on Reference AND Fast
  // geometries, at nodes and off-node, puts and calls, exercised rows included.
  std::size_t checked = 0;
  std::size_t exercised = 0;
  for (const bool fast : {false, true}) {
    for (const Case &c : all_cases()) {
      const AlExpiryGeometry g =
          fast ? prepare_al_geometry(c.T, c.r, c.q, al_fast_profile(std::nullopt))
               : prepare_al_geometry(c.T, c.r, c.q);
      AlSigmaFamily f(g, c.side, c.anchor);
      if (!f.prepare(c.lo, c.hi, kNodes)) {
        continue; // q = 0 calls: not in the American regime (the L2h refusal)
      }
      const AlSigmaFamilyDetached d = family_detach(f);
      ASSERT_TRUE(d.prepared());
      for (const double K : c.strikes) {
        for (const double sigma : c.sigmas) {
          const AlSigmaFamilyPrice p = f.price(c.S, K, sigma);
          ASSERT_EQ(p.status, AlSigmaFamilyStatus::Ok);
          const AlSigmaFamilyGreeks ga = family_greeks(f, c.S, K, sigma, Mask::all());
          const AlSigmaFamilyGreeks gp = family_greeks(f, c.S, K, sigma, Mask::price_only());
          const AlSigmaFamilyGreeks gh = family_greeks(f, c.S, K, sigma, Mask::hedge());
          const AlSigmaFamilyPriceVega pv = family_price_and_vega(f, c.S, K, sigma);
          const AlSigmaFamilyPrice dp = d.price(c.S, K, sigma);
          const AlSigmaFamilyGreeks dg = d.greeks(c.S, K, sigma, Mask::all());
          const std::string at = describe(c.S, K, c.T, sigma, c.r, c.q, c.side);
          EXPECT_TRUE(same_bits(ga.price, p.price)) << at << " all " << ga.price << " vs " << p.price;
          EXPECT_TRUE(same_bits(gp.price, p.price)) << at << " price-only";
          EXPECT_TRUE(same_bits(gh.price, p.price)) << at << " hedge";
          EXPECT_TRUE(same_bits(pv.price, p.price)) << at << " price_and_vega";
          EXPECT_TRUE(same_bits(dp.price, p.price)) << at << " detached price";
          EXPECT_TRUE(same_bits(dg.price, p.price)) << at << " detached greeks";
          exercised += ga.exercised ? 1u : 0u;
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 2000u);
  EXPECT_GT(exercised, 0u);
  std::cout << "[AlSigmaFamilyGreeks] price identity checked=" << checked
            << " exercised rows=" << exercised << "\n";
}

// ── Formula claims: Richardson on the family's own map ─────────────────────────

// Per-set bounds for the formula check (2-figure round-ups of the measured worst;
// the three sets differ in what the Richardson REFERENCE itself can resolve: the
// wide-box profile grid's σ-interpolant is a degree-11 polynomial over [0.08, 0.7]
// whose node spacing at the low edge is 3.6e-3 — an FD step of 5e-4 in σ is a tenth
// of it and the reference's own truncation, not the formula, sets the volga /
// vanna gap there; the SPY-like slice and the deep-carry corner are the formula-class
// numbers).
struct FormulaBounds {
  const char *name;
  double delta, gamma, vega, vanna, volga, charm;
};

void check_formula_on(const std::vector<Case> &cases, const FormulaBounds &b) {
  Worst wd, wg, wv, wvn, wvo, wc;
  for (const Case &c : cases) {
    const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q);
    AlSigmaFamily f(g, c.side, c.anchor);
    if (!f.prepare(c.lo, c.hi, kNodes)) {
      continue;
    }
    g_case_T = c.T;
    for (const double K : c.strikes) {
      for (const double sigma : c.sigmas) {
        const AlSigmaFamilyGreeks a = family_greeks(f, c.S, K, sigma, Mask::all());
        ASSERT_EQ(a.status, AlSigmaFamilyStatus::Ok);
        if (a.exercised) {
          continue;
        }
        // Keep the whole spot stencil (±2h) and the σ stencil inside the box and
        // out of the exercise region. Steps scale with the diffusion width.
        const double h1 = std::min(1.0e-3 * c.S, 0.02 * c.S * sigma * std::sqrt(c.T));
        if (sigma - 5.0e-4 < c.lo || sigma + 5.0e-4 > c.hi ||
            near_edge(f, c.S, K, sigma, c.side, 3.0 * h1)) {
          continue;
        }
        const FamilyFd fd = family_fd(f, c.S, K, sigma);
        if (!fd.ok) {
          continue;
        }
        const std::string at = describe(c.S, K, c.T, sigma, c.r, c.q, c.side);
        const double floor_p = 1.0e-3 * std::max(a.price, 1.0);
        // charm = ∂θ/∂S: the PDE identity differentiated with the analytic speed, vs
        // the Richardson spot-difference of the served theta (same scaled step).
        const auto th = [&](double s) {
          return family_greeks(f, s, K, sigma, Mask{Mask::kTheta}).theta;
        };
        const double c1 = (th(c.S + h1) - th(c.S - h1)) / (2.0 * h1);
        const double c2 = (th(c.S + 0.5 * h1) - th(c.S - 0.5 * h1)) / h1;
        const double charm_fd = (4.0 * c2 - c1) / 3.0;
        const double gd = rel_gap(a.delta, fd.delta, 1.0e-3);
        const double gg = rel_gap(a.gamma, fd.gamma, 1.0e-6 * c.S);
        const double gv = rel_gap(a.vega, fd.vega, floor_p);
        const double gvn = rel_gap(a.vanna, fd.vanna, 1.0e-3);
        const double gvo = rel_gap(a.volga, fd.volga, floor_p);
        const double gc = rel_gap(a.charm, charm_fd, 1.0e-2 * std::max(a.price, 1.0));
        wd.add(gd, at);
        wg.add(gg, at);
        wv.add(gv, at);
        wvn.add(gvn, at);
        wvo.add(gvo, at);
        wc.add(gc, at);
        EXPECT_LE(gd, b.delta) << b.name << " " << at << " delta " << a.delta << " fd " << fd.delta;
        EXPECT_LE(gg, b.gamma) << b.name << " " << at << " gamma " << a.gamma << " fd " << fd.gamma;
        EXPECT_LE(gv, b.vega) << b.name << " " << at << " vega " << a.vega << " fd " << fd.vega;
        EXPECT_LE(gvn, b.vanna) << b.name << " " << at << " vanna " << a.vanna << " fd " << fd.vanna;
        EXPECT_LE(gvo, b.volga) << b.name << " " << at << " volga " << a.volga << " fd " << fd.volga;
        EXPECT_LE(gc, b.charm) << b.name << " " << at << " charm " << a.charm << " fd " << charm_fd;
      }
    }
  }
  EXPECT_GT(wd.n, 100u);
  std::cout << "[AlSigmaFamilyGreeks] formula/" << b.name << " worst rel vs family Richardson (n="
            << wd.n << "):\n  delta=" << wd.rel << " at " << wd.at << "\n  gamma=" << wg.rel
            << " at " << wg.at << "\n  vega=" << wv.rel << " at " << wv.at << "\n  vanna=" << wvn.rel
            << " at " << wvn.at << "\n  volga=" << wvo.rel << " at " << wvo.at
            << "\n  charm=" << wc.rel << " at " << wc.at << "\n";
}

TEST(AlSigmaFamilyGreeks, GreeksMatchRichardsonOnTheFamilyMapWithinBound) {
  // Puts AND calls (the homogeneity map), continuation rows only (the FD stencil must
  // not straddle the intrinsic clamp). Relative gaps vs the Richardson stencils of the
  // family's own price; floors keep the near-zero Greeks of deep wings from dominating.
  check_formula_on(spy_slice(), FormulaBounds{"spy23-like", 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
  check_formula_on(deep_carry(), FormulaBounds{"deep-carry", 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
  check_formula_on(profile_grid(), FormulaBounds{"grid", 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
}

TEST(AlSigmaFamilyGreeks, ThetaIsThePdeIdentityOfTheServedPriceDeltaGamma) {
  // θ = rV − (r − q) S Δ − ½ σ² S² Γ, from the served fields themselves (exact up to
  // rounding), on both sides; exercised rows serve θ = 0.
  std::size_t n = 0;
  for (const Case &c : spy_slice()) {
    const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q);
    AlSigmaFamily f(g, c.side, c.anchor);
    ASSERT_TRUE(f.prepare(c.lo, c.hi, kNodes));
    for (const double K : c.strikes) {
      for (const double sigma : c.sigmas) {
        const AlSigmaFamilyGreeks a = family_greeks(f, c.S, K, sigma, Mask::all());
        ASSERT_EQ(a.status, AlSigmaFamilyStatus::Ok);
        const double want = a.exercised ? 0.0
                                        : c.r * a.price - (c.r - c.q) * c.S * a.delta -
                                              0.5 * sigma * sigma * c.S * c.S * a.gamma;
        EXPECT_NEAR(a.theta, want, 1.0e-9 * std::max(std::abs(want), 1.0))
            << describe(c.S, K, c.T, sigma, c.r, c.q, c.side);
        ++n;
      }
    }
  }
  EXPECT_GE(n, 250u);
}

// ── Distance claims: the Reference bundle on the same geometry ─────────────────

TEST(AlSigmaFamilyGreeks, GreeksWithinMeasuredBoundOfTheReferenceBundleOnSpy23) {
  // vs american_greeks_al(g, …) — the cold bundle's bits (LEDGER 2026-10-19) — on the
  // SPY-like 23 d slice at 12 LogSigma nodes, continuation rows the bundle itself
  // did not flag as exercised. Δ / Γ / θ: the family's exact spot derivatives vs the
  // bundle's hS = 1e-3 S central stencils (stencil truncation ~1e-7 in Γ: the bundle
  // is the reference-limited side, LEDGER 2026-10-16). vega / vanna / volga: the
  // σ-interpolant's derivative vs the bundle's re-solved σ ± 1e-3 boundaries. rho:
  // FROZEN-boundary vs the bundle's re-solved r ± 1e-4 boundaries — the gap IS the
  // boundary term dy/dr the family cannot see (the PM's policy input; sprint note).
  // Bounds: 2-figure round-ups of the measured worst, relative with floors.
  constexpr double kDelta = 1.0;
  constexpr double kGamma = 1.0;
  constexpr double kVega = 1.0;
  constexpr double kTheta = 1.0;
  constexpr double kVanna = 1.0;
  constexpr double kVolga = 1.0;
  constexpr double kRhoFrozen = 1.0;
  Worst wd, wg, wv, wt, wvn, wvo, wr;
  for (const Case &c : spy_slice()) {
    const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q);
    AlSigmaFamily f(g, c.side, c.anchor);
    ASSERT_TRUE(f.prepare(c.lo, c.hi, kNodes));
    for (const double K : c.strikes) {
      for (const double sigma : c.sigmas) {
        const AlSigmaFamilyGreeks a = family_greeks(f, c.S, K, sigma, Mask::all());
        ASSERT_EQ(a.status, AlSigmaFamilyStatus::Ok);
        const Result<AmericanGreeks> b = american_greeks_al(g, c.S, K, sigma, c.side);
        ASSERT_TRUE(b) << describe(c.S, K, c.T, sigma, c.r, c.q, c.side);
        const double intr = c.side == Side::Call ? c.S - K : K - c.S;
        const bool bundle_exercised = intr > 0.0 && b->price <= intr + 1.0e-9 * K;
        // The bundle's widest stencil is ±2 hS = ±2e-3 S (charm's speed): a row whose
        // stencil straddles the edge is the reference-limited case the ledger names.
        if (a.exercised || bundle_exercised ||
            near_edge(f, c.S, K, sigma, c.side, 2.5e-3 * c.S)) {
          continue;
        }
        const std::string at = describe(c.S, K, c.T, sigma, c.r, c.q, c.side);
        const double fp = 1.0e-3 * std::max(a.price, 1.0);
        wd.add(rel_gap(a.delta, b->delta, 1.0e-3), at);
        wg.add(rel_gap(a.gamma, b->gamma, 1.0e-6 * c.S), at);
        wv.add(rel_gap(a.vega, b->vega, fp), at);
        wt.add(rel_gap(a.theta, b->theta, fp), at);
        wvn.add(rel_gap(a.vanna, b->vanna, 1.0e-3), at);
        wvo.add(rel_gap(a.volga, b->volga, fp), at);
        wr.add(rel_gap(a.rho, b->rho, fp), at);
      }
    }
  }
  EXPECT_GE(wd.n, 200u);
  EXPECT_LE(wd.rel, kDelta) << wd.at;
  EXPECT_LE(wg.rel, kGamma) << wg.at;
  EXPECT_LE(wv.rel, kVega) << wv.at;
  EXPECT_LE(wt.rel, kTheta) << wt.at;
  EXPECT_LE(wvn.rel, kVanna) << wvn.at;
  EXPECT_LE(wvo.rel, kVolga) << wvo.at;
  EXPECT_LE(wr.rel, kRhoFrozen) << wr.at;
  std::cout << "[AlSigmaFamilyGreeks] spy23-like vs Reference bundle worst rel (n=" << wd.n
            << "): delta=" << wd.rel << " gamma=" << wg.rel << " vega=" << wv.rel
            << " theta=" << wt.rel << " vanna=" << wvn.rel << " volga=" << wvo.rel
            << " rho_frozen=" << wr.rel << "\n  rho worst at " << wr.at << "\n  volga worst at "
            << wvo.at << "\n";
}

// ── The exact Newton slope ────────────────────────────────────────────────────

TEST(AlSigmaFamilyGreeks, PriceAndVegaIsTheExactSlopeWithinTheVolgaBound) {
  // The AlPricePack / AlSigmaFamily chord test's shape: (i) |vega − family forward
  // chord| ≤ h · |volga| + 1e-8 (h = kAlPackSigmaBump; the chord's bias is h/2 · volga
  // plus its O(h²) tail, the existing suites' bound shape); (ii) vega vs the family's
  // central difference at σ ± h within the h²-class (pinned relative, floor
  // 1e-3 max(P, 1)); (iii) vs the cold al_price_pack chord (boundary RE-SOLVED at σ and
  // σ + h), after removing the chord's h/2 · volga bias, within a pinned relative gap
  // (measured worst, rounded up): the σ-interpolant's derivative against the
  // re-solved boundary's, on rows away from the exercise edge.
  constexpr double kVsColdRel = 1.0;
  constexpr double kVsCentralRel = 1.0;
  const double h = kAlPackSigmaBump;
  Worst wc, wcen;
  std::size_t with_vega = 0;
  for (const Case &c : spy_slice()) {
    const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q);
    AlSigmaFamily f(g, c.side, c.anchor);
    ASSERT_TRUE(f.prepare(c.lo, c.hi, kNodes));
    for (const double K : c.strikes) {
      for (const double sigma : c.sigmas) {
        if (sigma + 2.0 * h > c.hi || sigma - h < c.lo) {
          continue;
        }
        const AlSigmaFamilyPriceVega pv = family_price_and_vega(f, c.S, K, sigma);
        ASSERT_EQ(pv.status, AlSigmaFamilyStatus::Ok);
        const AlSigmaFamilyGreeks a =
            family_greeks(f, c.S, K, sigma, Mask{Mask::kVega | Mask::kVolga});
        EXPECT_TRUE(same_bits(a.vega, pv.vega));
        const AlSigmaFamilyChord ch = f.price_and_chord(c.S, K, sigma);
        ASSERT_EQ(ch.status, AlSigmaFamilyStatus::Ok);
        const std::string at = describe(c.S, K, c.T, sigma, c.r, c.q, c.side);
        const double chord = (ch.price_sigma_bumped - ch.price) / h;
        if (a.exercised) {
          continue; // exercise Greeks: vega 0 by contract, the chord is the clamp's
        }
        EXPECT_LE(std::abs(pv.vega - chord), h * std::abs(a.volga) + 1.0e-8)
            << at << " vega " << pv.vega << " chord " << chord << " volga " << a.volga;
        const double central =
            (f.price(c.S, K, sigma + h).price - f.price(c.S, K, sigma - h).price) / (2.0 * h);
        wcen.add(rel_gap(pv.vega, central, 1.0e-3 * std::max(a.price, 1.0)), at);
        if (!(a.vega > 1.0e-3 * std::max(a.price, 1.0)) ||
            near_edge(f, c.S, K, sigma, c.side, 2.5e-3 * c.S)) {
          continue;
        }
        ++with_vega;
        const std::array<AlContract, 1> one{{AlContract{.S = c.S, .K = K, .side = c.side}}};
        const std::array<double, 1> sig{{sigma}};
        std::array<double, 1> cp{};
        std::array<double, 1> cb{};
        ASSERT_TRUE(al_price_pack(g, one, sig, cp, cb));
        const double cold_chord = (cb[0] - cp[0]) / h;
        const double cold_slope = cold_chord - 0.5 * h * a.volga; // de-biased
        wc.add(std::abs(pv.vega - cold_slope) / std::abs(cold_slope), at);
      }
    }
  }
  EXPECT_GE(with_vega, 100u);
  EXPECT_LE(wc.rel, kVsColdRel) << wc.at;
  EXPECT_LE(wcen.rel, kVsCentralRel) << wcen.at;
  std::cout << "[AlSigmaFamilyGreeks] vega vs de-biased cold chord worst rel=" << wc.rel << " at "
            << wc.at << " (n=" << wc.n << ")\n[AlSigmaFamilyGreeks] vega vs family central worst rel="
            << wcen.rel << " at " << wcen.at << " (n=" << wcen.n << ")\n";
}

// ── Regimes, masks, refusals ──────────────────────────────────────────────────

TEST(AlSigmaFamilyGreeks, ExerciseRegionServesIntrinsicWithTheExerciseGreeks) {
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaFamily put(g, Side::Put, 100.0);
  AlSigmaFamily call(g, Side::Call, 100.0);
  ASSERT_TRUE(put.prepare(0.1, 0.5, kNodes));
  ASSERT_TRUE(call.prepare(0.1, 0.5, kNodes));
  // Deep ITM put at a low spot: intrinsic served (the premium decomposition reproduces
  // K − S to quadrature rounding; the kernel's clamp may or may not fire), Δ = −1, the
  // rest 0.
  const AlSigmaFamilyGreeks p = family_greeks(put, 40.0, 100.0, 0.12, Mask::all());
  ASSERT_EQ(p.status, AlSigmaFamilyStatus::Ok);
  EXPECT_TRUE(p.exercised);
  EXPECT_FALSE(p.floored);
  EXPECT_NEAR(p.price, 60.0, 1.0e-9 * 100.0);
  EXPECT_DOUBLE_EQ(p.delta, -1.0);
  EXPECT_EQ(p.gamma, 0.0);
  EXPECT_EQ(p.vega, 0.0);
  EXPECT_EQ(p.theta, 0.0);
  EXPECT_EQ(p.rho, 0.0);
  EXPECT_EQ(p.phi, 0.0);
  EXPECT_EQ(p.vanna, 0.0);
  EXPECT_EQ(p.volga, 0.0);
  EXPECT_EQ(p.charm, 0.0);
  // Deep ITM call (q > 0 so the call is American): intrinsic, Δ = +1.
  const AlSigmaFamilyGreeks cg = family_greeks(call, 100.0, 40.0, 0.12, Mask::all());
  ASSERT_EQ(cg.status, AlSigmaFamilyStatus::Ok);
  EXPECT_TRUE(cg.exercised);
  EXPECT_NEAR(cg.price, 60.0, 1.0e-9 * 100.0);
  EXPECT_DOUBLE_EQ(cg.delta, 1.0);
  EXPECT_EQ(cg.gamma, 0.0);
  EXPECT_EQ(cg.vega, 0.0);
  // A continuation row is not flagged and carries a negative put delta in (−1, 0).
  const AlSigmaFamilyGreeks c0 = family_greeks(put, 100.0, 100.0, 0.3, Mask::all());
  EXPECT_FALSE(c0.exercised);
  EXPECT_FALSE(c0.floored);
  EXPECT_GT(c0.delta, -1.0);
  EXPECT_LT(c0.delta, 0.0);
  EXPECT_GT(c0.gamma, 0.0);
  EXPECT_GT(c0.vega, 0.0);
  EXPECT_LT(c0.theta, 0.0);
  EXPECT_TRUE(c0.boundary_term_included);
  // The exercise flags agree with the Reference bundle's own exercise test along a
  // spot ladder through the edge (at most the one ladder point sitting on the edge
  // itself may differ), and in the exercise region the served price is intrinsic to
  // quadrature rounding.
  std::size_t agree = 0;
  std::size_t inside = 0;
  for (double S = 60.0; S <= 100.0; S += 1.0) {
    const AlSigmaFamilyGreeks a = family_greeks(put, S, 100.0, 0.12, Mask::hedge());
    const Result<AmericanGreeks> b = american_greeks_al(g, S, 100.0, 0.12, Side::Put);
    ASSERT_TRUE(b);
    const bool bex = (100.0 - S) > 0.0 && b->price <= (100.0 - S) + 1.0e-9 * 100.0;
    agree += (bex == a.exercised) ? 1u : 0u;
    if (a.exercised && !a.floored) {
      ++inside;
      EXPECT_NEAR(a.price, 100.0 - S, 1.0e-9 * 100.0) << "S=" << S;
      EXPECT_DOUBLE_EQ(a.delta, -1.0);
    }
  }
  EXPECT_GE(agree, 40u);
  EXPECT_GE(inside, 10u);
  std::cout << "[AlSigmaFamilyGreeks] exercise ladder agree=" << agree << "/41 inside=" << inside
            << " edge=" << edge_of(put, 100.0, 100.0, 0.12, Side::Put) << "\n";
}

TEST(AlSigmaFamilyGreeks, MaskSelectsWhatIsComputedAndRequestedFieldsAreMaskIndependent) {
  const AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaFamily f(g, Side::Put, 100.0);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kNodes));
  const AlSigmaFamilyGreeks all = family_greeks(f, 100.0, 95.0, 0.22, Mask::all());
  const AlSigmaFamilyGreeks eight = family_greeks(f, 100.0, 95.0, 0.22, Mask::eight());
  const AlSigmaFamilyGreeks hedge = family_greeks(f, 100.0, 95.0, 0.22, Mask::hedge());
  const AlSigmaFamilyGreeks only_delta = family_greeks(f, 100.0, 95.0, 0.22, Mask{Mask::kDelta});
  const AlSigmaFamilyGreeks price = family_greeks(f, 100.0, 95.0, 0.22, Mask::price_only());
  // Unrequested fields stay 0.
  EXPECT_EQ(eight.charm, 0.0);
  EXPECT_EQ(hedge.rho, 0.0);
  EXPECT_EQ(hedge.phi, 0.0);
  EXPECT_EQ(hedge.vanna, 0.0);
  EXPECT_EQ(hedge.volga, 0.0);
  EXPECT_EQ(only_delta.gamma, 0.0);
  EXPECT_EQ(only_delta.vega, 0.0);
  EXPECT_EQ(price.delta, 0.0);
  EXPECT_FALSE(price.boundary_term_included);
  EXPECT_FALSE(only_delta.boundary_term_included);
  EXPECT_TRUE(hedge.boundary_term_included);
  // Requested fields do not depend on what else was requested (each accumulator's
  // sequence is its own).
  EXPECT_TRUE(same_bits(all.delta, eight.delta));
  EXPECT_TRUE(same_bits(all.delta, hedge.delta));
  EXPECT_TRUE(same_bits(all.delta, only_delta.delta));
  EXPECT_TRUE(same_bits(all.gamma, hedge.gamma));
  EXPECT_TRUE(same_bits(all.vega, hedge.vega));
  EXPECT_TRUE(same_bits(all.theta, hedge.theta));
  EXPECT_TRUE(same_bits(all.rho, eight.rho));
  EXPECT_TRUE(same_bits(all.volga, eight.volga));
  EXPECT_TRUE(same_bits(all.price, price.price));
  EXPECT_NE(all.charm, 0.0);
  EXPECT_NE(all.rho, 0.0);
  EXPECT_NE(all.phi, 0.0);
}

TEST(AlSigmaFamilyGreeks, RefusalsAreTypedAndCarryNaNOnEveryRequestedField) {
  AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaFamily f(g, Side::Put, 100.0);
  const auto refused = [&](double S, double K, double sigma, AlSigmaFamilyStatus want) {
    const AlSigmaFamilyGreeks a = family_greeks(f, S, K, sigma, Mask::all());
    EXPECT_EQ(a.status, want) << "S=" << S << " K=" << K << " sigma=" << sigma;
    EXPECT_TRUE(std::isnan(a.price) && std::isnan(a.delta) && std::isnan(a.gamma) &&
                std::isnan(a.vega) && std::isnan(a.theta) && std::isnan(a.rho) &&
                std::isnan(a.phi) && std::isnan(a.vanna) && std::isnan(a.volga) &&
                std::isnan(a.charm));
    const AlSigmaFamilyGreeks h = family_greeks(f, S, K, sigma, Mask::hedge());
    EXPECT_EQ(h.status, want);
    EXPECT_TRUE(std::isnan(h.price) && std::isnan(h.delta));
    EXPECT_EQ(h.rho, 0.0); // unrequested: untouched
    const AlSigmaFamilyPriceVega pv = family_price_and_vega(f, S, K, sigma);
    EXPECT_EQ(pv.status, want);
    EXPECT_TRUE(std::isnan(pv.price) && std::isnan(pv.vega));
  };
  refused(100.0, 100.0, 0.3, AlSigmaFamilyStatus::NotPrepared);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kNodes));
  refused(100.0, 100.0, 0.5 + 1.0e-9, AlSigmaFamilyStatus::SigmaOutsideBox);
  refused(100.0, 100.0, 0.1 - 1.0e-9, AlSigmaFamilyStatus::SigmaOutsideBox);
  refused(100.0, 100.0, 0.5 * kAlSigmaFamilyMinSigma, AlSigmaFamilyStatus::SigmaBelowGuard);
  refused(0.0, 100.0, 0.3, AlSigmaFamilyStatus::InvalidArgument);
  refused(100.0, -1.0, 0.3, AlSigmaFamilyStatus::InvalidArgument);
  refused(kNaN, 100.0, 0.3, AlSigmaFamilyStatus::InvalidArgument);
  refused(100.0, 100.0, kNaN, AlSigmaFamilyStatus::InvalidArgument);
  // The box edges themselves serve; the exact slope needs NO headroom above σ (the
  // chord did).
  EXPECT_EQ(family_greeks(f, 100.0, 100.0, 0.5, Mask::all()).status, AlSigmaFamilyStatus::Ok);
  EXPECT_EQ(family_price_and_vega(f, 100.0, 100.0, 0.5).status, AlSigmaFamilyStatus::Ok);
  EXPECT_EQ(f.price_and_chord(100.0, 100.0, 0.5).status, AlSigmaFamilyStatus::SigmaOutsideBox);
  // A re-prepared geometry: refuse until prepared again.
  g.prepare(0.75, 0.05, 0.02);
  refused(100.0, 100.0, 0.3, AlSigmaFamilyStatus::GeometryRePrepared);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kNodes));
  EXPECT_EQ(family_greeks(f, 100.0, 100.0, 0.3, Mask::all()).status, AlSigmaFamilyStatus::Ok);
  // Moved-from family.
  AlSigmaFamily moved(std::move(f));
  EXPECT_EQ(family_greeks(moved, 100.0, 100.0, 0.3, Mask::all()).status, AlSigmaFamilyStatus::Ok);
  refused(100.0, 100.0, 0.3, AlSigmaFamilyStatus::NotPrepared); // NOLINT (moved-from)
}

TEST(AlSigmaFamilyGreeks, GreeksAreContinuousThroughASigmaNode) {
  // At an exact σ-node the σ-basis is e_hit and the derivative is the nodal one; a
  // hair off the node the regular formula runs. Both must agree to the volga-sized
  // change (no quotient-rule blow-up near a node).
  const AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaFamily f(g, Side::Put, 100.0);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kNodes));
  double worst = 0.0;
  for (const double node : log_nodes(0.1, 0.5, kNodes)) {
    for (const double K : {85.0, 100.0, 115.0}) {
      const AlSigmaFamilyGreeks at = family_greeks(f, 100.0, K, node, Mask::all());
      ASSERT_EQ(at.status, AlSigmaFamilyStatus::Ok);
      for (const double eps : {-1.0e-7, 1.0e-7, -1.0e-10, 1.0e-10}) {
        const double s = node * (1.0 + eps);
        if (s < 0.1 || s > 0.5) {
          continue;
        }
        const AlSigmaFamilyGreeks near = family_greeks(f, 100.0, K, s, Mask::all());
        ASSERT_EQ(near.status, AlSigmaFamilyStatus::Ok);
        const double tol = std::abs(at.volga) * std::abs(s - node) * 2.0 + 1.0e-8 * std::max(at.vega, 1.0);
        worst = std::max(worst, std::abs(near.vega - at.vega) / tol);
        EXPECT_LE(std::abs(near.vega - at.vega), tol)
            << "node " << node << " eps " << eps << " K " << K << " vega " << at.vega << " near " << near.vega;
        EXPECT_NEAR(near.delta, at.delta, 1.0e-6 * std::abs(at.vanna) + 1.0e-10);
        EXPECT_NEAR(near.volga, at.volga, 1.0e-4 * std::abs(at.volga) + 1.0e-6);
      }
    }
  }
  std::cout << "[AlSigmaFamilyGreeks] node continuity worst (gap / tol)=" << worst << "\n";
}

// ── Chain, shock grid, detach, threads ────────────────────────────────────────

TEST(AlSigmaFamilyGreeks, ChainEntryMatchesPerContractCallsAndRoutesRefusals) {
  const AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaFamily put(g, Side::Put, 100.0);
  AlSigmaFamily call(g, Side::Call, 100.0);
  ASSERT_TRUE(put.prepare(0.1, 0.5, kNodes));
  ASSERT_TRUE(call.prepare(0.1, 0.5, kNodes));
  std::vector<double> K, sig;
  std::vector<Side> side;
  for (const double k : {70.0, 85.0, 95.0, 100.0, 105.0, 115.0, 130.0}) {
    for (const Side s : {Side::Put, Side::Call}) {
      K.push_back(k);
      sig.push_back(0.15 + 0.002 * k);
      side.push_back(s);
    }
  }
  K.push_back(100.0);
  sig.push_back(0.9); // outside the box
  side.push_back(Side::Put);
  const std::size_t n = K.size();
  std::vector<double> price(n), delta(n), gamma(n), vega(n), theta(n), rho(n), phi(n), vanna(n),
      volga(n), charm(n);
  std::vector<std::uint8_t> flags(n);
  std::vector<AlSigmaFamilyStatus> status(n);
  AlSigmaFamilyGreekColumns cols;
  cols.price = price;
  cols.delta = delta;
  cols.gamma = gamma;
  cols.vega = vega;
  cols.theta = theta;
  cols.rho = rho;
  cols.phi = phi;
  cols.vanna = vanna;
  cols.volga = volga;
  cols.charm = charm;
  cols.flags = flags;
  ASSERT_TRUE(family_chain_greeks(&put, &call, 100.0, K, sig, side, Mask::all(), cols, status));
  for (std::size_t i = 0; i < n; ++i) {
    const AlSigmaFamily &f = side[i] == Side::Call ? call : put;
    const AlSigmaFamilyGreeks a = family_greeks(f, 100.0, K[i], sig[i], Mask::all());
    EXPECT_EQ(status[i], a.status);
    if (a.status != AlSigmaFamilyStatus::Ok) {
      EXPECT_TRUE(std::isnan(price[i]) && std::isnan(delta[i]) && std::isnan(volga[i]));
      continue;
    }
    EXPECT_TRUE(same_bits(price[i], a.price) && same_bits(delta[i], a.delta) &&
                same_bits(gamma[i], a.gamma) && same_bits(vega[i], a.vega) &&
                same_bits(theta[i], a.theta) && same_bits(rho[i], a.rho) &&
                same_bits(phi[i], a.phi) && same_bits(vanna[i], a.vanna) &&
                same_bits(volga[i], a.volga) && same_bits(charm[i], a.charm))
        << "row " << i;
    EXPECT_EQ((flags[i] & 1u) != 0, a.exercised);
    EXPECT_EQ((flags[i] & 4u) != 0, a.boundary_term_included);
  }
  EXPECT_EQ(status[n - 1], AlSigmaFamilyStatus::SigmaOutsideBox);
  // A null family for one side: those rows carry NotPrepared and NaN.
  std::fill(price.begin(), price.end(), 0.0);
  ASSERT_TRUE(family_chain_greeks(&put, nullptr, 100.0, K, sig, side, Mask::hedge(), cols, status));
  for (std::size_t i = 0; i < n; ++i) {
    if (side[i] == Side::Call) {
      EXPECT_EQ(status[i], AlSigmaFamilyStatus::NotPrepared);
      EXPECT_TRUE(std::isnan(price[i]));
    }
  }
  // A stored-but-unrequested column reads 0; a length mismatch refuses, writing nothing.
  EXPECT_EQ(rho[0], 0.0);
  std::vector<double> short_price(n - 1);
  AlSigmaFamilyGreekColumns bad;
  bad.price = short_price;
  const Status st = family_chain_greeks(&put, &call, 100.0, K, sig, side, Mask::all(), bad, status);
  ASSERT_FALSE(st);
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(AlSigmaFamilyGreeks, ShockGridRepricesEveryCellThroughTheFamily) {
  const AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaFamily f(g, Side::Put, 100.0);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kNodes));
  const std::array<double, 5> spot_pct{{-0.10, -0.02, 0.0, 0.02, 0.10}};
  const std::array<double, 4> vol_bump{{-0.05, 0.0, 0.05, 0.30}}; // the last leaves the box
  const std::size_t n = spot_pct.size() * vol_bump.size();
  std::vector<double> price(n), delta(n), vega(n);
  std::vector<AlSigmaFamilyStatus> status(n);
  AlSigmaFamilyGreekColumns cols;
  cols.price = price;
  cols.delta = delta;
  cols.vega = vega;
  ASSERT_TRUE(family_shock_grid(f, 100.0, 100.0, 0.25, spot_pct, vol_bump, Mask::hedge(), cols, status));
  std::size_t ok = 0;
  for (std::size_t i = 0; i < spot_pct.size(); ++i) {
    for (std::size_t j = 0; j < vol_bump.size(); ++j) {
      const std::size_t idx = i * vol_bump.size() + j;
      const AlSigmaFamilyGreeks a =
          family_greeks(f, 100.0 * (1.0 + spot_pct[i]), 100.0, 0.25 + vol_bump[j], Mask::hedge());
      EXPECT_EQ(status[idx], a.status);
      if (a.status == AlSigmaFamilyStatus::Ok) {
        ++ok;
        EXPECT_TRUE(same_bits(price[idx], a.price) && same_bits(delta[idx], a.delta) &&
                    same_bits(vega[idx], a.vega));
      } else {
        EXPECT_EQ(status[idx], AlSigmaFamilyStatus::SigmaOutsideBox);
        EXPECT_TRUE(std::isnan(price[idx]));
      }
    }
  }
  EXPECT_EQ(ok, spot_pct.size() * 3u);
  // A spot shock to a non-positive spot is a typed InvalidArgument cell.
  const std::array<double, 1> dead{{-1.0}};
  const std::array<double, 1> zero{{0.0}};
  std::vector<AlSigmaFamilyStatus> one(1);
  AlSigmaFamilyGreekColumns none;
  ASSERT_TRUE(family_shock_grid(f, 100.0, 100.0, 0.25, dead, zero, Mask::hedge(), none, one));
  EXPECT_EQ(one[0], AlSigmaFamilyStatus::InvalidArgument);
  // Length mismatch refuses.
  std::vector<AlSigmaFamilyStatus> wrong(n - 1);
  const Status st = family_shock_grid(f, 100.0, 100.0, 0.25, spot_pct, vol_bump, Mask::hedge(), cols, wrong);
  ASSERT_FALSE(st);
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(AlSigmaFamilyGreeks, DetachedCopyServesTheFamilysBitsWithoutTheGeometry) {
  AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaFamily f(g, Side::Call, 100.0);
  // Unprepared family -> unprepared copy.
  {
    const AlSigmaFamilyDetached d0 = family_detach(f);
    EXPECT_FALSE(d0.prepared());
    EXPECT_EQ(d0.price(100.0, 100.0, 0.3).status, AlSigmaFamilyStatus::NotPrepared);
    EXPECT_EQ(d0.greeks(100.0, 100.0, 0.3, Mask::all()).status, AlSigmaFamilyStatus::NotPrepared);
  }
  ASSERT_TRUE(f.prepare(0.1, 0.5, kNodes));
  const AlSigmaFamilyDetached d = family_detach(f);
  ASSERT_TRUE(d.prepared());
  EXPECT_EQ(d.side(), Side::Call);
  EXPECT_DOUBLE_EQ(d.sigma_lo(), 0.1);
  EXPECT_DOUBLE_EQ(d.sigma_hi(), 0.5);
  EXPECT_GT(AlSigmaFamilyDetached::state_bytes(), 4096u);
  EXPECT_LT(AlSigmaFamilyDetached::state_bytes(), 16384u);
  std::vector<AlSigmaFamilyGreeks> want;
  std::vector<AlSigmaFamilyPriceVega> want_pv;
  for (const double K : {70.0, 85.0, 95.0, 100.0, 105.0, 115.0, 130.0}) {
    for (const double s : off_nodes(0.1, 0.5, kNodes)) {
      want.push_back(family_greeks(f, 100.0, K, s, Mask::all()));
      want_pv.push_back(family_price_and_vega(f, 100.0, K, s));
    }
  }
  // Re-prepare the geometry: the family refuses, the copy keeps serving the same bits.
  g.prepare(0.75, 0.05, 0.02);
  EXPECT_EQ(family_greeks(f, 100.0, 100.0, 0.3, Mask::all()).status,
            AlSigmaFamilyStatus::GeometryRePrepared);
  std::size_t i = 0;
  for (const double K : {70.0, 85.0, 95.0, 100.0, 105.0, 115.0, 130.0}) {
    for (const double s : off_nodes(0.1, 0.5, kNodes)) {
      const AlSigmaFamilyGreeks a = d.greeks(100.0, K, s, Mask::all());
      const AlSigmaFamilyGreeks &w = want[i];
      EXPECT_EQ(a.status, AlSigmaFamilyStatus::Ok);
      EXPECT_TRUE(same_bits(a.price, w.price) && same_bits(a.delta, w.delta) &&
                  same_bits(a.gamma, w.gamma) && same_bits(a.vega, w.vega) &&
                  same_bits(a.theta, w.theta) && same_bits(a.rho, w.rho) &&
                  same_bits(a.phi, w.phi) && same_bits(a.vanna, w.vanna) &&
                  same_bits(a.volga, w.volga) && same_bits(a.charm, w.charm))
          << "K " << K << " sigma " << s;
      const AlSigmaFamilyPriceVega pv = d.price_and_vega(100.0, K, s);
      EXPECT_TRUE(same_bits(pv.price, want_pv[i].price) && same_bits(pv.vega, want_pv[i].vega));
      EXPECT_TRUE(same_bits(d.price(100.0, K, s).price, w.price));
      ++i;
    }
  }
  // The copy's own refusals.
  EXPECT_EQ(d.price(100.0, 100.0, 0.6).status, AlSigmaFamilyStatus::SigmaOutsideBox);
  EXPECT_EQ(d.greeks(0.0, 100.0, 0.3, Mask::all()).status, AlSigmaFamilyStatus::InvalidArgument);
  EXPECT_EQ(d.price_and_vega(100.0, 100.0, 0.001).status, AlSigmaFamilyStatus::SigmaBelowGuard);
}

TEST(AlSigmaFamilyGreeks, ConcurrentGreeksAreBitIdenticalToTheSerialLoop) {
  // N threads through family_greeks on ONE const family (no per-thread scratch, no
  // locks) reproduce the single-thread loop bit for bit — the L2h test's shape; a
  // shared mutable scratch in the kernel fails it.
  constexpr unsigned kThreads = 8;
  const AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaFamily fam(g, Side::Put, 100.0);
  ASSERT_TRUE(fam.prepare(0.1, 0.5, kNodes));
  const AlSigmaFamily &shared = fam;
  std::vector<double> strikes;
  std::vector<double> sigmas;
  for (const double K : {70.0, 85.0, 95.0, 100.0, 105.0, 115.0, 130.0}) {
    for (const double s : off_nodes(0.1, 0.5, kNodes)) {
      strikes.push_back(K);
      sigmas.push_back(s);
    }
  }
  const std::size_t n = strikes.size();
  std::vector<AlSigmaFamilyGreeks> serial(n);
  for (std::size_t i = 0; i < n; ++i) {
    serial[i] = family_greeks(shared, 100.0, strikes[i], sigmas[i], Mask::all());
    ASSERT_EQ(serial[i].status, AlSigmaFamilyStatus::Ok);
  }
  std::vector<std::vector<AlSigmaFamilyGreeks>> got(kThreads, std::vector<AlSigmaFamilyGreeks>(n));
  {
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t]() {
        for (unsigned rep = 0; rep < 4; ++rep) {
          for (std::size_t j = 0; j < n; ++j) {
            const std::size_t i = (j + t * 7u) % n;
            got[t][i] = family_greeks(shared, 100.0, strikes[i], sigmas[i], Mask::all());
          }
        }
      });
    }
  }
  std::size_t mismatched = 0;
  for (unsigned t = 0; t < kThreads; ++t) {
    for (std::size_t i = 0; i < n; ++i) {
      const AlSigmaFamilyGreeks &a = got[t][i];
      const AlSigmaFamilyGreeks &w = serial[i];
      const bool same = a.status == w.status && same_bits(a.price, w.price) &&
                        same_bits(a.delta, w.delta) && same_bits(a.gamma, w.gamma) &&
                        same_bits(a.vega, w.vega) && same_bits(a.theta, w.theta) &&
                        same_bits(a.rho, w.rho) && same_bits(a.phi, w.phi) &&
                        same_bits(a.vanna, w.vanna) && same_bits(a.volga, w.volga) &&
                        same_bits(a.charm, w.charm);
      mismatched += same ? 0u : 1u;
    }
  }
  EXPECT_EQ(mismatched, 0u) << "of " << kThreads * n;
  EXPECT_GE(n, 70u);
}

} // namespace
