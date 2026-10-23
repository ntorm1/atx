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
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_sigma_q_family.hpp" // the K1 fused entries (internal)

// K1 (r5, 2026-10-22): the (σ, q) family's fused price + analytic vega (the K1 block of
// src/pricing/american_sigma_q_family.hpp). Three kinds of claim, kept apart (deep
// review F11):
//   * IDENTITY — the fused price is price()'s map to rounding (the brief's 1e-12
//     relative is the pin); the strike vector is the scalar entry bit for bit; the
//     precontracted table path is the family path to rounding.
//   * FAMILY-MAP DERIVATIVE — vega against Richardson-refined differences of the family's
//     OWN price(), each row held to the ladder's own error estimate plus its rounding
//     (4 |R_l − R_{l−1}| + gain · rounding; the observed order is reported, not used);
//     the boundary partials likewise against the interpolant's own differences. Exact
//     node hits included. A relative claim is made on RESOLVED rows only (tolerance <=
//     1e-9 of the reference); the rest are held to gap <= tolerance and their tolerance
//     sizes are printed. At σ = 0.05 the full-price differences resolve almost nothing,
//     so a separate check differences the premium alone there (section 3b).
//   * MODEL DISTANCE — vega against the converged Andersen-Lake vega: an error TABLE,
//     guarded only against gross breakage (it is not the family's tolerance).
// Measured tables: the lane K1 report (.superpowers/sdd/2026-10-18-vol-sota/).

namespace {

using atx::core::Result;
using atx::core::Status;
using atx::vol::AlExpiryGeometry;
using atx::vol::AlOpts;
using atx::vol::AlSigmaFamilyAxis;
using atx::vol::AlSigmaQFamily;
using atx::vol::AlSigmaQFamilyPrice;
using atx::vol::AlSigmaQFamilyStatus;
using atx::vol::andersen_lake;
using atx::vol::kAlSigmaFamilyMinSigma;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;
using atx::vol::simd::SimdIsa;
namespace sq = atx::vol::sigma_q_family_detail;
using sq::FusedPriceVega;
using sq::FusedStatus;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// The process-wide SimdIsa override for one scope (the family BUILD's route switch).
class IsaScope {
public:
  explicit IsaScope(SimdIsa isa) : prev_(atx::vol::simd::simd_isa_override()) {
    atx::vol::simd::set_simd_isa_override(isa);
  }
  ~IsaScope() { atx::vol::simd::set_simd_isa_override(prev_); }
  IsaScope(const IsaScope &) = delete;
  IsaScope &operator=(const IsaScope &) = delete;

private:
  SimdIsa prev_;
};

// One (expiry, side) family and the probe it is validated on.
struct Spec {
  const char *name;
  double S;
  double T;
  double r;
  double q_lo;
  double q_hi;
  Side side;
  double s_lo;
  double s_hi;
  std::uint16_t n_sigma;
  std::uint16_t n_q;
  std::vector<double> sigmas;  // interior, off-node σ probes
  std::vector<double> strikes; // K
};

// The family and its geometry (the family borrows the geometry: built in place).
struct Built {
  explicit Built(const Spec &s)
      : spec(s), g(prepare_al_geometry(s.T, s.r, 0.5 * (s.q_lo + s.q_hi))), f(g, s.side, s.S) {}
  Spec spec;
  AlExpiryGeometry g;
  AlSigmaQFamily f;
};

[[nodiscard]] std::unique_ptr<Built> build(const Spec &s) {
  auto b = std::make_unique<Built>(s);
  const Status st = b->f.prepare(s.s_lo, s.s_hi, s.n_sigma, s.q_lo, s.q_hi, s.n_q);
  if (!st) {
    ADD_FAILURE() << s.name << ": prepare refused: " << st.error().to_string();
    return nullptr;
  }
  return b;
}

[[nodiscard]] std::vector<double> span_of(double lo, double hi, unsigned n) {
  std::vector<double> v;
  for (unsigned i = 0; i < n; ++i) {
    v.push_back(lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(n - 1));
  }
  return v;
}

[[nodiscard]] std::vector<double> strikes_around(double S, double lo, double hi, unsigned n) {
  std::vector<double> v = span_of(lo, hi, n);
  for (double &k : v) {
    k *= S;
  }
  return v;
}

// SPY 2026-11-06 (23 d) under funding-pv (the L2h-b fixture): S 772.99, r 4.496 %,
// q in [0.001589, 0.001678]; and SPY 2028-12-15 (2.17 y): r 5.153 %, q < 0 (puts only).
constexpr double kSpyS = 772.99;
constexpr double kSpyT = 0.0682;
constexpr double kSpyR = 0.04496;
constexpr double kSpyQlo = 0.001589;
constexpr double kSpyQhi = 0.001678;

// The derivative-validation classes (brief deliverable 5a): small / mid / large σ, a
// long-dated put, a wide q box at n_q 6 / 8; strikes reach deep ITM (near exercise).
[[nodiscard]] std::vector<Spec> derivative_specs() {
  std::vector<Spec> v;
  for (const Side side : {Side::Put, Side::Call}) {
    const bool put = side == Side::Put;
    v.push_back(Spec{put ? "spy23-mid-put" : "spy23-mid-call", kSpyS, kSpyT, kSpyR, kSpyQlo,
                     kSpyQhi, side, 0.10, 0.50, 12, 4, {0.12, 0.2, 0.3, 0.45},
                     strikes_around(kSpyS, put ? 0.8 : 0.7, put ? 1.3 : 1.2, 11)});
    v.push_back(Spec{put ? "spy23-small-put" : "spy23-small-call", kSpyS, kSpyT, kSpyR, kSpyQlo,
                     kSpyQhi, side, 0.03, 0.08, 12, 4, {0.04, 0.05, 0.07},
                     strikes_around(kSpyS, put ? 0.94 : 0.96, put ? 1.06 : 1.04, 11)});
    v.push_back(Spec{put ? "spy23-large-put" : "spy23-large-call", kSpyS, kSpyT, kSpyR, kSpyQlo,
                     kSpyQhi, side, 1.0, 2.0, 12, 4, {1.2, 1.5, 1.8},
                     strikes_around(kSpyS, 0.5, 1.5, 11)});
  }
  v.push_back(Spec{"spylong-put", 772.98, 2.1673, 0.05153, -0.003298, -0.002272, Side::Put, 0.10,
                   0.45, 16, 4, {0.15, 0.3}, strikes_around(772.98, 0.6, 1.6, 11)});
  v.push_back(Spec{"wideq-put-nq6", 100.0, 1.0, 0.05, 0.0, 0.01, Side::Put, 0.10, 0.60, 12, 6,
                   {0.15, 0.35}, strikes_around(100.0, 0.7, 1.4, 8)});
  v.push_back(Spec{"wideq-call-nq8", 100.0, 1.0, 0.05, 0.005, 0.015, Side::Call, 0.10, 0.60, 12, 8,
                   {0.15, 0.35}, strikes_around(100.0, 0.7, 1.4, 8)});
  return v;
}

// q probes: both box ends (q nodes, exact hits) and two interior points.
[[nodiscard]] std::vector<double> q_probes(const Spec &s) {
  return {s.q_lo, s.q_lo + 0.37 * (s.q_hi - s.q_lo), s.q_lo + 0.81 * (s.q_hi - s.q_lo), s.q_hi};
}

// ── Richardson-refined differences ─────────────────────────────────────────────────

struct Rich {
  double value = kNaN; // the extrapolated derivative R_l at the selected level
  double err = kNaN;   // its error estimate |R_l − R_{l−1}|
  double order = kNaN; // the observed order of the coarsest three levels (truncation regime)
  double gain = kNaN;  // Σ|stencil coefficient| / h at that level: rounding amplification
  bool ok = false;     // every stencil evaluation served
};

// d f / d x at x by a step-refinement ladder: fourth-order differences D_l at h_l = h0 /
// 2^l (`dir` 0 = the 5-point central stencil, ±1 = the 5-point one-sided stencil for a
// box end), extrapolated pairwise R_l = D_l + (D_l − D_{l−1}) / 15. The level whose R_l
// agrees best with R_{l−1} is taken — the truncation / rounding crossover — so a strongly
// curved row (tiny vega, large volga near the exercise clamp) refines its step until the
// expansion is asymptotic instead of trusting one fixed h. The observed order log2(|D_0 −
// D_1| / |D_1 − D_2|) is reported (4 where the coarse levels are truncation-dominated).
// f(x, ok) clears ok on a refusal. A caller's tolerance is 4 err + gain · (f's rounding).
template <class F>
[[nodiscard]] Rich richardson(F &&f, double x, double h0, int dir = 0, int levels = 10) {
  constexpr int kMaxLevels = 10;
  const int n_levels = std::clamp(levels, 3, kMaxLevels);
  Rich r;
  bool ok = true;
  double D[kMaxLevels] = {};
  double R[kMaxLevels] = {};
  for (int l = 0; l < n_levels; ++l) {
    const double hl = h0 / static_cast<double>(1 << l);
    if (dir == 0) {
      D[l] = (-f(x + 2.0 * hl, ok) + 8.0 * f(x + hl, ok) - 8.0 * f(x - hl, ok) +
              f(x - 2.0 * hl, ok)) /
             (12.0 * hl);
    } else {
      const double d = static_cast<double>(dir) * hl;
      D[l] = (-25.0 * f(x, ok) + 48.0 * f(x + d, ok) - 36.0 * f(x + 2.0 * d, ok) +
              16.0 * f(x + 3.0 * d, ok) - 3.0 * f(x + 4.0 * d, ok)) /
             (12.0 * d);
    }
    R[l] = (l == 0) ? D[0] : D[l] + (D[l] - D[l - 1]) / 15.0;
  }
  int best = 2;
  double best_err = std::numeric_limits<double>::infinity();
  for (int l = 2; l < n_levels; ++l) {
    const double e = std::abs(R[l] - R[l - 1]);
    if (e < best_err) {
      best_err = e;
      best = l;
    }
  }
  const double e1 = std::abs(D[0] - D[1]);
  const double e2 = std::abs(D[1] - D[2]);
  const double h = h0 / static_cast<double>(1 << best);
  r.order = (e1 > 0.0 && e2 > 0.0) ? std::log2(e1 / e2) : kNaN;
  r.value = R[best];
  r.err = best_err;
  // R_l = (16 D_l − D_{l−1}) / 15 over two levels' stencils: (16 Σ|c| / h + Σ|c| / 2h) / 15.
  const double coef = (dir == 0) ? 18.0 / 12.0 : 128.0 / 12.0;
  r.gain = coef * (16.0 / h + 1.0 / (2.0 * h)) / 15.0;
  r.ok = ok;
  return r;
}

// Bookkeeping for an error table row: worst relative gap, worst gap / tolerance, orders.
// A row is RESOLVED when its reference's own tolerance is at most kResolved of the
// reference value: only there can a relative gap below that level be claimed at all (a
// tiny vega next to a large price drowns in the price's rounding / h — such rows are
// held to gap <= tol only).
constexpr double kResolved = 1.0e-9;
constexpr double kEps = std::numeric_limits<double>::epsilon();

// A price's rounding: the European leg's two terms and the premium are each O(S + K), so
// a few ulps of S + K + P (measured on the noisiest rows below: ~0.3 eps (S + K)).
[[nodiscard]] double price_rounding(double S, double K, double P) noexcept {
  return 4.0 * kEps * (S + K + std::abs(P));
}

struct Table {
  std::size_t n = 0;
  std::size_t n_resolved = 0;
  double worst_rel = 0.0;   // max |gap| / |ref| over the RESOLVED rows
  double worst_ratio = 0.0; // max |gap| / tolerance over every row
  std::vector<double> orders;
  std::vector<double> unresolved; // tol / |ref| of every row that does not resolve
  std::string at;       // where worst_rel is
  std::string ratio_at; // where worst_ratio is
  // gap = |analytic − reference|; tol = the reference's tolerance (0: always resolved).
  void add(double gap, double ref, double tol, double order, const std::string &where) {
    ++n;
    if (std::isfinite(order)) {
      orders.push_back(order);
    }
    const double ratio = (tol > 0.0) ? gap / tol : 0.0;
    if (ratio > worst_ratio) {
      worst_ratio = ratio;
      ratio_at = where;
    }
    if (tol > kResolved * std::abs(ref)) {
      unresolved.push_back(ref != 0.0 ? tol / std::abs(ref)
                                      : std::numeric_limits<double>::infinity());
      return;
    }
    ++n_resolved;
    const double rel = gap / std::abs(ref);
    if (rel > worst_rel) {
      worst_rel = rel;
      at = where;
    }
  }
  void merge(const Table &o) {
    n += o.n;
    n_resolved += o.n_resolved;
    if (o.worst_ratio > worst_ratio) {
      worst_ratio = o.worst_ratio;
      ratio_at = o.ratio_at;
    }
    orders.insert(orders.end(), o.orders.begin(), o.orders.end());
    unresolved.insert(unresolved.end(), o.unresolved.begin(), o.unresolved.end());
    if (o.worst_rel > worst_rel) {
      worst_rel = o.worst_rel;
      at = o.at;
    }
  }
  [[nodiscard]] double median_order() const {
    if (orders.empty()) {
      return kNaN;
    }
    std::vector<double> o = orders;
    std::sort(o.begin(), o.end());
    return o[o.size() / 2];
  }
  // The p-quantile of the unresolved rows' tol / |ref| (NaN when every row resolves).
  [[nodiscard]] double unresolved_quantile(double p) const {
    if (unresolved.empty()) {
      return kNaN;
    }
    std::vector<double> o = unresolved;
    std::sort(o.begin(), o.end());
    const auto i = static_cast<std::size_t>(p * static_cast<double>(o.size() - 1u));
    return o[i];
  }
  // "resolved a/b, unresolved tol/|ref| p50 x max y" — the resolution a claim rests on.
  [[nodiscard]] std::string resolution() const {
    std::ostringstream os;
    os << "resolved " << n_resolved << "/" << n;
    if (!unresolved.empty()) {
      os << " (unresolved tol/|ref| p50 " << unresolved_quantile(0.5) << " max "
         << unresolved_quantile(1.0) << ")";
    }
    return os.str();
  }
};

[[nodiscard]] std::string describe(const Spec &s, double K, double sigma, double q) {
  std::ostringstream os;
  os << std::setprecision(17) << s.name << " K=" << K << " sigma=" << sigma << " q=" << q;
  return os.str();
}

// ── 1. The boundary partials (deliverable 2) ───────────────────────────────────────

TEST(AlSigmaQFamilyFused, BoundaryPartialsAreTheInterpolantsOwnIncludingExactNodeHits) {
  // dy_k/dσ and dy_k/dq from interpolate_derivs vs Richardson differences of
  // interpolate() itself (a polynomial in (z_σ, z_q)), at off-node points, at the box
  // corners (exact node hits on both axes: z = ±1 exactly) and ulps away from them (the
  // quotient rule would lose O(1/|z − z_s|) digits there; the nodal-derivative form
  // must not). The stencils' asymptotic order is 4, but the observed medians are −1.1 to
  // −0.75 (dy/dσ) and −1.2 to 4 (dy/dq): differences of a low-degree polynomial carry
  // almost no truncation error, so the coarse levels already sit at the rounding floor,
  // whose ∝ 1/h growth gives log2(|D0 − D1| / |D1 − D2|) ≈ −1. The ladder's error
  // estimate is then rounding, not truncation; the order column says which regime.
  // Resolved rows (the differences' own tolerance <= 1e-9 of the partial): measured worst
  // 2.15e-11 (dy/dσ, wideq-put-nq6) / 2.08e-11 (dy/dq, spy23); every row within the
  // differences' tolerance (measured worst gap/tol 0.045).
  constexpr double kRel = 2.2e-11;
  std::size_t resolved_q = 0;
  for (const Spec &s : {derivative_specs()[0], derivative_specs()[6], derivative_specs()[7]}) {
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    const auto *state = sq::QFamilyAccess::state(b->f);
    ASSERT_NE(state, nullptr);
    const unsigned nb = state->n_boundary;
    Table ts;
    Table tq;
    std::vector<double> sig_pts = s.sigmas;
    sig_pts.push_back(s.s_lo);
    sig_pts.push_back(s.s_hi);
    sig_pts.push_back(std::nextafter(s.s_lo, 1.0));
    sig_pts.push_back(std::nextafter(s.s_hi, 0.0));
    for (const double sigma : sig_pts) {
      for (const double q : q_probes(s)) {
        double y[32] = {};
        double dys[32] = {};
        double dyq[32] = {};
        const bool has_q = state->interpolate_derivs(sigma, q, y, dys, dyq);
        EXPECT_EQ(has_q, state->n_q > 1);
        atx::vol::amer::AlBoundary ref = state->grid;
        state->interpolate(sigma, q, ref);
        for (unsigned k = 0; k < nb; ++k) {
          ASSERT_TRUE(std::isfinite(y[k]) && std::isfinite(dys[k]) && std::isfinite(dyq[k]))
              << describe(s, 0.0, sigma, q) << " k=" << k;
          EXPECT_NEAR(y[k], ref.y[k], 1.0e-14 * std::max(std::abs(ref.y[k]), 1.0));
        }
        // σ partial: central inside, one-sided at a box end.
        const double hs = 1.0e-3 * sigma;
        const int sdir = (sigma - 4.0 * hs < s.s_lo) ? 1 : ((sigma + 4.0 * hs > s.s_hi) ? -1 : 0);
        const double hq = 0.05 * (s.q_hi - s.q_lo);
        const int qdir = (q - 4.0 * hq < s.q_lo) ? 1 : ((q + 4.0 * hq > s.q_hi) ? -1 : 0);
        for (unsigned k = 1; k < nb; ++k) { // k = 0 is τ = 0: y = 0 at every node
          const auto ys = [&](double x, bool &) {
            atx::vol::amer::AlBoundary bb = state->grid;
            state->interpolate(x, q, bb);
            return bb.y[k];
          };
          const Rich rs = richardson(ys, sigma, hs, sdir);
          ts.add(std::abs(dys[k] - rs.value), rs.value,
                 4.0 * rs.err + rs.gain * 32.0 * kEps * std::abs(y[k]), rs.order,
                 describe(s, 0.0, sigma, q) + " k=" + std::to_string(k));
          if (!has_q) {
            continue;
          }
          const auto yq = [&](double x, bool &) {
            atx::vol::amer::AlBoundary bb = state->grid;
            state->interpolate(sigma, x, bb);
            return bb.y[k];
          };
          const Rich rq = richardson(yq, q, hq, qdir);
          tq.add(std::abs(dyq[k] - rq.value), rq.value,
                 4.0 * rq.err + rq.gain * 32.0 * kEps * std::abs(y[k]), rq.order,
                 describe(s, 0.0, sigma, q) + " k=" + std::to_string(k));
        }
      }
    }
    for (const auto &[name, t] : {std::pair<const char *, const Table *>{"dy/dsigma", &ts},
                                  std::pair<const char *, const Table *>{"dy/dq    ", &tq}}) {
      std::cout << "[AlSigmaQFamilyFused] " << name << " " << s.name << ": n=" << t->n
                << " resolved=" << t->n_resolved << " worst rel (resolved)=" << t->worst_rel
                << " worst gap/tol=" << t->worst_ratio << " median order=" << t->median_order()
                << "\n  rel at " << t->at << "\n  gap/tol at " << t->ratio_at << "\n";
      EXPECT_LE(t->worst_rel, kRel) << t->at;
      EXPECT_LE(t->worst_ratio, 1.0) << t->ratio_at;
    }
    EXPECT_GE(ts.n_resolved, ts.n / 2u);
    resolved_q += tq.n_resolved;
  }
  // A 9e-5-wide q box (SPY 23 d) moves y by less than the differences resolve; the wider
  // boxes carry the dy/dq claim.
  EXPECT_GE(resolved_q, 300u);
  // Continuity through a node hit: an ulp off the box end is the node value's
  // neighbour, not a 0/0 (the nodal-derivative form evaluates both stably).
  const std::unique_ptr<Built> b = build(derivative_specs()[0]);
  ASSERT_NE(b, nullptr);
  const auto *state = sq::QFamilyAccess::state(b->f);
  double y0[32] = {};
  double d0[32] = {};
  double y1[32] = {};
  double d1[32] = {};
  (void)state->interpolate_derivs(0.10, kSpyQlo, y0, d0, nullptr);
  (void)state->interpolate_derivs(std::nextafter(0.10, 1.0), kSpyQlo, y1, d1, nullptr);
  for (unsigned k = 1; k < state->n_boundary; ++k) {
    EXPECT_NEAR(d1[k], d0[k], 1.0e-9 * std::abs(d0[k])) << "k=" << k;
  }
  // A collapsed box has no q coordinate: dy_dq is not written.
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaQFamily c(g, Side::Put, 100.0);
  ASSERT_TRUE(c.prepare(0.1, 0.6, 12, 0.02, 0.02, 4));
  double yc[32] = {};
  double dc[32] = {};
  double qc[32] = {};
  qc[3] = 42.0;
  EXPECT_FALSE(sq::QFamilyAccess::state(c)->interpolate_derivs(0.3, 0.02, yc, dc, qc));
  EXPECT_EQ(qc[3], 42.0);
}

// ── 2. The fused price is price()'s map to rounding (deliverable 4) ─────────────────

TEST(AlSigmaQFamilyFused, FusedPriceIsPriceOnToRoundingOnTheSpyProbeGrid) {
  // The brief's probe: SPY-shaped, 300 strikes x 5 σ x 3 q, calls and puts, at the
  // fitter's (12, 4) and (16, 4) shapes; the fused one-shot, the strike vector and the
  // table path against price(). Pin: the brief's own rounding threshold (1e-12 relative
  // to max(P, 1)) — a larger gap would be an operation-order change beyond rounding.
  constexpr double kRoundingRel = 1.0e-12;
  const std::vector<double> K = strikes_around(kSpyS, 0.7, 1.3, 300);
  const std::vector<double> sigmas = {0.11, 0.17, 0.26, 0.37, 0.49};
  const std::vector<double> qs = {kSpyQlo, 0.5 * (kSpyQlo + kSpyQhi), kSpyQhi};
  double worst_abs = 0.0;
  double worst_scaled = 0.0;
  double worst_tab = 0.0;
  std::size_t n = 0;
  std::size_t strike_vector_mismatch = 0;
  std::size_t exercised = 0;
  std::string at;
  for (const std::uint16_t ns : {std::uint16_t{12}, std::uint16_t{16}}) {
    for (const Side side : {Side::Put, Side::Call}) {
      Spec s{"spy23-probe", kSpyS, kSpyT, kSpyR, kSpyQlo, kSpyQhi, side, 0.10, 0.50, ns, 4, {},
             K};
      const std::unique_ptr<Built> b = build(s);
      ASSERT_NE(b, nullptr);
      sq::PremiumTable table;
      ASSERT_EQ(sq::build_premium_table(b->f, table), FusedStatus::Ok);
      for (const double q : qs) {
        sq::QTile tile;
        ASSERT_EQ(sq::contract_q(table, q, tile), FusedStatus::Ok);
        for (const double sigma : sigmas) {
          std::vector<double> vp(K.size(), 0.0);
          std::vector<double> vv(K.size(), 0.0);
          ASSERT_EQ(sq::price_vega_strikes(b->f, kSpyS, K, sigma, q, vp, vv), FusedStatus::Ok);
          for (std::size_t i = 0; i < K.size(); ++i) {
            const AlSigmaQFamilyPrice p = b->f.price(kSpyS, K[i], sigma, q);
            const FusedPriceVega fv = sq::price_and_vega(b->f, kSpyS, K[i], sigma, q);
            ASSERT_EQ(p.status, AlSigmaQFamilyStatus::Ok);
            ASSERT_EQ(fv.status, FusedStatus::Ok) << describe(s, K[i], sigma, q);
            ++n;
            exercised += fv.exercised ? 1u : 0u;
            const double d = std::abs(fv.price - p.price);
            worst_abs = std::max(worst_abs, d);
            if (d / std::max(p.price, 1.0) > worst_scaled) {
              worst_scaled = d / std::max(p.price, 1.0);
              std::ostringstream os;
              os << std::setprecision(17) << describe(s, K[i], sigma, q) << " ns=" << ns
                 << " price=" << p.price << " fused=" << fv.price;
              at = os.str();
            }
            if (!same_bits(vp[i], fv.price) || !same_bits(vv[i], fv.vega)) {
              ++strike_vector_mismatch;
            }
            const FusedPriceVega tv = sq::price_vega_on(table, tile, kSpyS, K[i], sigma);
            ASSERT_EQ(tv.status, FusedStatus::Ok);
            worst_tab = std::max(worst_tab, std::abs(tv.price - p.price) / std::max(p.price, 1.0));
            worst_tab = std::max(worst_tab,
                                 std::abs(tv.vega - fv.vega) / std::max(std::abs(fv.vega), 1.0));
          }
        }
      }
    }
  }
  std::cout << "[AlSigmaQFamilyFused] probe grid n=" << n << " (exercised " << exercised
            << "): fused vs price() max |dP|=" << worst_abs
            << " max |dP|/max(P,1)=" << worst_scaled << " | table vs price()"
            << " / fused vega, max scaled=" << worst_tab << "\n  worst at " << at << "\n";
  EXPECT_EQ(n, 2u * 2u * 300u * 5u * 3u);
  EXPECT_LE(worst_scaled, kRoundingRel) << at;
  EXPECT_LE(worst_tab, kRoundingRel);
  EXPECT_EQ(strike_vector_mismatch, 0u);
}

// ── 3. Fused vega vs the family's own map (deliverable 5a) ─────────────────────────

struct MapResult {
  Table smooth;    // rows farther than one σ-node spacing from the exercise regime
  Table node_hits; // the same, σ exactly at a box-end node (one-sided stencils)
  Table near_edge; // exercised within one σ-node spacing, or a stencil across the clamp
  std::size_t clamped = 0;
};

[[nodiscard]] double intrinsic(Side side, double S, double K) noexcept {
  return std::max(side == Side::Put ? K - S : S - K, 0.0);
}

// One σ-node spacing at sigma: the widest Chebyshev-Lobatto gap of the LogSigma axis,
// (ln σ_hi − ln σ_lo) / 2 · π / (n − 1), mapped to σ.
[[nodiscard]] double sigma_node_spacing(const Spec &s, double sigma) noexcept {
  const double pi = 3.14159265358979323846;
  return sigma * 0.5 * (std::log(s.s_hi) - std::log(s.s_lo)) * pi /
         static_cast<double>(s.n_sigma - 1u);
}

void map_rows(const Spec &s, const AlSigmaQFamily &f, MapResult &out) {
  std::vector<double> sig_pts = s.sigmas;
  const std::size_t n_interior = sig_pts.size();
  sig_pts.push_back(s.s_lo); // exact node hits (z = -1 / +1)
  sig_pts.push_back(s.s_hi);
  for (std::size_t si = 0; si < sig_pts.size(); ++si) {
    const double sigma = sig_pts[si];
    const bool node_hit = si >= n_interior;
    const int dir = !node_hit ? 0 : (sigma == s.s_lo ? 1 : -1);
    const double h0 = 2.0e-3 * sigma;
    const double gap_sigma = sigma_node_spacing(s, sigma);
    for (const double q : q_probes(s)) {
      for (const double K : s.strikes) {
        const FusedPriceVega fv = sq::price_and_vega(f, s.S, K, sigma, q);
        ASSERT_EQ(fv.status, FusedStatus::Ok) << describe(s, K, sigma, q);
        const double intr = intrinsic(s.side, s.S, K);
        bool crosses = false;
        const bool center_clamped = fv.price == intr && intr > 0.0;
        const auto P = [&](double x, bool &ok) {
          const AlSigmaQFamilyPrice p = f.price(s.S, K, x, q);
          ok = ok && p.status == AlSigmaQFamilyStatus::Ok;
          crosses = crosses || ((p.price == intr && intr > 0.0) != center_clamped);
          return p.price;
        };
        const Rich r = richardson(P, sigma, h0, dir);
        ASSERT_TRUE(r.ok) << describe(s, K, sigma, q);
        if (center_clamped) {
          ++out.clamped;
          EXPECT_EQ(fv.vega, 0.0) << describe(s, K, sigma, q); // the clamp serves intrinsic
          EXPECT_TRUE(fv.exercised);
        }
        // Within one σ-node spacing of the exercise regime (either way along σ, inside
        // the box): the brief's "near exercise" band, reported separately.
        bool near_exercise = fv.exercised || crosses;
        for (const double x : {sigma - gap_sigma, sigma + gap_sigma}) {
          const double xc = std::clamp(x, s.s_lo, s.s_hi);
          near_exercise = near_exercise || sq::price_and_vega(f, s.S, K, xc, q).exercised;
        }
        const double gap = std::abs(fv.vega - r.value);
        const double tol = 4.0 * r.err + r.gain * price_rounding(s.S, K, fv.price);
        Table &t = near_exercise ? out.near_edge : (node_hit ? out.node_hits : out.smooth);
        t.add(gap, r.value, tol, r.order, describe(s, K, sigma, q));
      }
    }
  }
}

void run_family_map(SimdIsa isa, const char *isa_name) {
  // Pins (2-figure round-ups of the measured worst per class; the printout carries
  // them): the smooth region's relative gap on RESOLVED rows and gap / tolerance on
  // every row; node hits the same with one-sided stencils; rows within one σ-node
  // spacing of exercise, or whose stencil crosses the clamp, are reported only.
  // Measured worst over the resolved rows, both builds: smooth 2.3814e-11 (spy23-large-call
  // K 463.8 σ 1.2), node hits 2.94055e-11 (spy23-large-call K 386.5 σ 2.0); gap/tol 0.061 /
  // 0.042 / near-exercise 0.066. kRatio = 1 is the derived tolerance itself.
  // Resolution (measured, scalar build): smooth 516/930 and node hits 226/636 overall;
  // small σ (box [0.03, 0.08]) 6/212 and 0/140 — the full price's rounding ε (S + K)
  // over the step swamps a σ = 0.05 vega except next to the money, so the relative pins
  // say nothing there (section 3b resolves it on the premium). The per-class lines
  // print each class's resolved / total and the unresolved rows' tol / |ref|. Medians of
  // the observed order are 2.0 (smooth) and 2.4 (node hits), not the stencil's 4: at h0
  // = 2e-3 σ most rows' coarse levels mix truncation with the price's rounding (whose
  // ∝ 1/h growth pulls the log2 ratio below 4, to −1 when it dominates); only spylong
  // and wideq (3.5-3.9) are in the asymptotic regime at the coarse levels.
  constexpr double kSmoothRel = 2.4e-11;
  constexpr double kNodeRel = 3.0e-11;
  constexpr double kRatio = 1.0;
  const IsaScope scope(isa);
  MapResult all;
  MapResult by_class[3]; // small σ (box [0.03, 0.08]), mid, large σ (box [1, 2])
  for (const Spec &s : derivative_specs()) {
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    MapResult one;
    map_rows(s, b->f, one);
    MapResult &cls = by_class[s.s_hi <= 0.1 ? 0 : (s.s_lo >= 1.0 ? 2 : 1)];
    cls.smooth.merge(one.smooth);
    cls.node_hits.merge(one.node_hits);
    cls.near_edge.merge(one.near_edge);
    const auto row = [](const char *what, const Table &t) {
      std::ostringstream os;
      os << what << " n=" << t.n << " resolved=" << t.n_resolved << " rel=" << t.worst_rel
         << " gap/tol=" << t.worst_ratio << " order~" << t.median_order();
      return os.str();
    };
    std::cout << "[AlSigmaQFamilyFused] family-map " << isa_name << " " << s.name << ": "
              << row("smooth", one.smooth) << " | " << row("node-hits", one.node_hits) << " | "
              << row("near-exercise", one.near_edge) << " clamped=" << one.clamped << "\n";
    all.smooth.merge(one.smooth);
    all.node_hits.merge(one.node_hits);
    all.near_edge.merge(one.near_edge);
    all.clamped += one.clamped;
  }
  for (const auto &[name, t] :
       {std::pair<const char *, const Table *>{"smooth", &all.smooth},
        std::pair<const char *, const Table *>{"node hits", &all.node_hits},
        std::pair<const char *, const Table *>{"near-exercise (reported)", &all.near_edge}}) {
    std::cout << "[AlSigmaQFamilyFused] family-map " << isa_name << " ALL " << name
              << ": n=" << t->n << " resolved=" << t->n_resolved
              << " worst rel (resolved)=" << t->worst_rel << " worst gap/tol=" << t->worst_ratio
              << " median order=" << t->median_order() << "\n  rel at " << t->at
              << "\n  gap/tol at " << t->ratio_at << "\n";
  }
  std::cout << "[AlSigmaQFamilyFused] family-map " << isa_name << " clamped rows (vega 0) "
            << all.clamped << "\n";
  const char *class_names[3] = {"small sigma [0.03, 0.08]", "mid sigma", "large sigma [1, 2]"};
  for (int c = 0; c < 3; ++c) {
    std::cout << "[AlSigmaQFamilyFused] family-map " << isa_name << " class " << class_names[c]
              << ": smooth " << by_class[c].smooth.resolution() << " | node-hits "
              << by_class[c].node_hits.resolution() << " | near-exercise "
              << by_class[c].near_edge.resolution() << "\n";
  }
  std::cout << "[AlSigmaQFamilyFused] family-map " << isa_name << " ALL: smooth "
            << all.smooth.resolution() << " | node-hits " << all.node_hits.resolution()
            << " | near-exercise " << all.near_edge.resolution() << "\n";
  EXPECT_GE(all.smooth.n_resolved, 400u);
  EXPECT_GE(all.node_hits.n_resolved, 150u);
  EXPECT_GE(all.clamped, 1u);
  EXPECT_LE(all.smooth.worst_rel, kSmoothRel) << all.smooth.at;
  EXPECT_LE(all.smooth.worst_ratio, kRatio) << all.smooth.ratio_at;
  EXPECT_LE(all.node_hits.worst_rel, kNodeRel) << all.node_hits.at;
  EXPECT_LE(all.node_hits.worst_ratio, kRatio) << all.node_hits.ratio_at;
  // Near exercise the differences rarely resolve 1e-9, but the vega still sits inside
  // their own tolerance.
  EXPECT_LE(all.near_edge.worst_ratio, kRatio) << all.near_edge.ratio_at;
}

TEST(AlSigmaQFamilyFused, VegaIsTheFamilyMapsDerivativeOnTheScalarBuild) {
  run_family_map(SimdIsa::ForceScalar, "scalar");
}

TEST(AlSigmaQFamilyFused, VegaIsTheFamilyMapsDerivativeOnTheAvx2Build) {
  if (!atx::vol::simd::have_avx2()) {
    GTEST_SKIP() << "host has no AVX2 (the AVX2 family build is unreachable)";
  }
  run_family_map(SimdIsa::ForceAvx2, "avx2");
}

// ── 3b. Small σ: the premium's own differences (review MJ2) ────────────────────────

// The fused pass's European leg of the internal put at (S, K, sigma, q), bit for bit
// (euro_leg<false>: black76_value_and_vega on the same stage data), so P − E is the fused
// map's premium with one rounding of P instead of the European leg's ε (S + K).
[[nodiscard]] atx::vol::Black76ValueVega euro_leg_of(const AlSigmaQFamily &f, Side side,
                                                     double S, double K, double sigma,
                                                     double q) {
  const auto *st = sq::QFamilyAccess::state(f);
  sq::PremiumNodes pn;
  (void)st->lay_premium_nodes(pn);
  sq::QCarry c;
  sq::carry_at(pn, side, st->rate, q, c);
  const bool call = side == Side::Call;
  const double Sp = call ? K : S;
  const double Kp = call ? S : K;
  const double F = Sp * std::exp((c.rp - c.qp) * pn.T);
  const double df = std::exp(-c.rp * pn.T);
  return atx::vol::black76_value_and_vega(F, Kp, pn.T, sigma, df, Side::Put, pn.sqrtT);
}

void run_small_sigma(SimdIsa isa, const char *isa_name) {
  // At σ = 0.05 on SPY 23 d (σ√T = 0.013) run_family_map's full-price differences resolve
  // almost nothing: the price's rounding, ε (S + K) ≈ 3e-13, over a step small enough
  // for the curvature leaves a tolerance far above 1e-9 of the vega. Here the reference
  // is E's analytic vega (black76_value_and_vega, the leg the fused pass sums) plus
  // Richardson differences of the premium alone, Π(σ) = P(σ) − E(σ), whose rounding is
  // a few ε |P|. Strikes concentrate at the money (|ln K/S| <= 1.5 %, about 1.2 σ√T),
  // where a σ = 0.05 vega is large; three boxes put σ = 0.05 in the interior (central
  // stencil) and exactly on each box end (one-sided stencil). Rows within one σ-node
  // spacing of exercise (or a stencil that exercises) are reported only, and so are rows
  // whose premium is identically 0 over the stencil (the SPY calls: q << r floors the
  // premium, the vega is the European leg's and the check is vacuous there).
  // Pins (2-figure round-ups of the measured worst over the resolved rows, both builds):
  // smooth 1.5239e-12 (avx2), node hits 1.14068e-11 (avx2, low box end); resolved
  // smooth 24/24 and node hits 47-48/52 put rows (the other 4-5: tol/|ref| <= 4.9e-7).
  constexpr double kSmallSmoothRel = 1.6e-12;
  constexpr double kSmallNodeRel = 1.2e-11;
  struct Box {
    const char *name;
    double s_lo;
    double s_hi;
    int dir; // the stencil at σ = 0.05: 0 central, ±1 one-sided from that box end
  };
  constexpr double kSigma = 0.05;
  const Box boxes[] = {{"smooth [0.03, 0.08]", 0.03, 0.08, 0},
                       {"node-hit low end [0.05, 0.12]", 0.05, 0.12, 1},
                       {"node-hit high end [0.025, 0.05]", 0.025, 0.05, -1}};
  const IsaScope scope(isa);
  Table smooth;
  Table node_hits;
  Table near_edge;
  Table premium_free; // premium identically 0 over the stencil: vega is E's (vacuous)
  Table premium_rel;  // the claim rows again, gap / |premium vega| (the differenced part)
  for (const Box &box : boxes) {
    for (const Side side : {Side::Put, Side::Call}) {
      Spec s{box.name, kSpyS, kSpyT, kSpyR, kSpyQlo, kSpyQhi, side, box.s_lo, box.s_hi, 12, 4,
             {kSigma}, strikes_around(kSpyS, 0.985, 1.015, 7)};
      const std::unique_ptr<Built> b = build(s);
      ASSERT_NE(b, nullptr);
      ASSERT_FALSE(sq::QFamilyAccess::state(b->f)->ws->fast); // the Reference arm's E
      Table one;
      const double gap_sigma = sigma_node_spacing(s, kSigma);
      for (const double q : q_probes(s)) {
        for (const double K : s.strikes) {
          const FusedPriceVega fv = sq::price_and_vega(b->f, s.S, K, kSigma, q);
          ASSERT_EQ(fv.status, FusedStatus::Ok) << describe(s, K, kSigma, q);
          bool exercised_in_stencil = false;
          const auto premium = [&](double x, bool &ok) {
            const FusedPriceVega p = sq::price_and_vega(b->f, s.S, K, x, q);
            ok = ok && p.status == FusedStatus::Ok;
            exercised_in_stencil = exercised_in_stencil || p.exercised;
            return p.price - euro_leg_of(b->f, side, s.S, K, x, q).price;
          };
          const Rich r = richardson(premium, kSigma, 2.0e-3 * kSigma, box.dir);
          ASSERT_TRUE(r.ok) << describe(s, K, kSigma, q);
          bool near_exercise = fv.exercised || exercised_in_stencil;
          for (const double x : {kSigma - gap_sigma, kSigma + gap_sigma}) {
            const double xc = std::clamp(x, s.s_lo, s.s_hi);
            near_exercise = near_exercise || sq::price_and_vega(b->f, s.S, K, xc, q).exercised;
          }
          const atx::vol::Black76ValueVega e = euro_leg_of(b->f, side, s.S, K, kSigma, q);
          const double ref = e.vega + r.value;
          const double gap = std::abs(fv.vega - ref);
          const double tol = 4.0 * r.err + r.gain * 4.0 * kEps * std::abs(fv.price);
          const std::string at = std::string(isa_name) + " " +
                                 (side == Side::Put ? "put " : "call ") +
                                 describe(s, K, kSigma, q);
          const bool no_premium = r.value == 0.0 && r.err == 0.0;
          Table &t = near_exercise ? near_edge
                                   : (no_premium ? premium_free
                                                 : (box.dir == 0 ? smooth : node_hits));
          t.add(gap, ref, tol, r.order, at);
          one.add(gap, ref, tol, r.order, at);
          if (!near_exercise && !no_premium) {
            premium_rel.add(gap, r.value, tol, r.order, at);
          }
        }
      }
      std::cout << "[AlSigmaQFamilyFused] small-sigma " << isa_name << " " << box.name << " "
                << (side == Side::Put ? "put" : "call") << ": n=" << one.n << " "
                << one.resolution() << " worst rel (resolved)=" << one.worst_rel
                << " gap/tol=" << one.worst_ratio << " order~" << one.median_order() << "\n";
    }
  }
  for (const auto &[name, t] :
       {std::pair<const char *, const Table *>{"smooth", &smooth},
        std::pair<const char *, const Table *>{"node hits", &node_hits},
        std::pair<const char *, const Table *>{"near-exercise (reported)", &near_edge},
        std::pair<const char *, const Table *>{"premium-free (reported)", &premium_free},
        std::pair<const char *, const Table *>{"vs premium vega (reported)", &premium_rel}}) {
    std::cout << "[AlSigmaQFamilyFused] small-sigma " << isa_name << " ALL " << name
              << ": n=" << t->n << " " << t->resolution()
              << " worst rel (resolved)=" << t->worst_rel << " worst gap/tol=" << t->worst_ratio
              << " median order=" << t->median_order() << "\n  rel at " << t->at
              << "\n  gap/tol at " << t->ratio_at << "\n";
  }
  EXPECT_GE(smooth.n_resolved, 24u);
  EXPECT_GE(node_hits.n_resolved, 47u);
  EXPECT_LE(smooth.worst_rel, kSmallSmoothRel) << smooth.at;
  EXPECT_LE(node_hits.worst_rel, kSmallNodeRel) << node_hits.at;
  EXPECT_LE(smooth.worst_ratio, 1.0) << smooth.ratio_at;
  EXPECT_LE(node_hits.worst_ratio, 1.0) << node_hits.ratio_at;
  EXPECT_LE(near_edge.worst_ratio, 1.0) << near_edge.ratio_at;
  EXPECT_LE(premium_free.worst_ratio, 1.0) << premium_free.ratio_at;
}

TEST(AlSigmaQFamilyFused, VegaAtSmallSigmaMatchesThePremiumsOwnDifferencesOnTheScalarBuild) {
  run_small_sigma(SimdIsa::ForceScalar, "scalar");
}

TEST(AlSigmaQFamilyFused, VegaAtSmallSigmaMatchesThePremiumsOwnDifferencesOnTheAvx2Build) {
  if (!atx::vol::simd::have_avx2()) {
    GTEST_SKIP() << "host has no AVX2 (the AVX2 family build is unreachable)";
  }
  run_small_sigma(SimdIsa::ForceAvx2, "avx2");
}

// ── 4. Fused vega vs the converged model (deliverable 5b: a table, F11) ─────────────

TEST(AlSigmaQFamilyFused, VegaVersusTheConvergedAndersenLakeModelIsReported) {
  // Columns: fused vega vs the converged Andersen-Lake vega (Richardson σ-differences of
  // andersen_lake at {32, 64} nodes, 16 sweeps, tol 1e-13 — the canonical-premium test's
  // scheme), vs the Reference-scheme vega (the family's own build target, {12, 24, 48}),
  // and the Reference scheme's own distance to the converged one (the model share of the
  // gap). Rows farther than one σ-node spacing from the exercise regime with vega >= 1e-3
  // max(P, 1). Not the family's tolerance: the only assertion is a gross-breakage guard on
  // the distance to the family's own build target (a formula error is O(1) there).
  constexpr double kGross = 1.0e-4;
  const AlOpts converged{.n_collocation = 32, .n_quadrature = 64, .max_newton_iter = 16,
                         .tol = 1.0e-13};
  std::vector<Spec> specs;
  for (const Spec &s : derivative_specs()) {
    if (std::string(s.name) == "spy23-mid-put" || std::string(s.name) == "spy23-mid-call" ||
        std::string(s.name) == "spylong-put") {
      Spec t = s;
      t.strikes = strikes_around(s.S, 0.85, 1.15, 7);
      specs.push_back(t);
    }
  }
  for (const Spec &s : specs) {
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    Table vs_conv;
    Table vs_ref;
    Table ref_vs_conv;
    for (const double sigma : s.sigmas) {
      const double q = 0.5 * (s.q_lo + s.q_hi);
      for (const double K : s.strikes) {
        const FusedPriceVega fv = sq::price_and_vega(b->f, s.S, K, sigma, q);
        ASSERT_EQ(fv.status, FusedStatus::Ok);
        const double gap_sigma = sigma_node_spacing(s, sigma);
        bool near_exercise = fv.exercised;
        for (const double x : {sigma - gap_sigma, sigma + gap_sigma}) {
          const double xc = std::clamp(x, s.s_lo, s.s_hi);
          near_exercise = near_exercise || sq::price_and_vega(b->f, s.S, K, xc, q).exercised;
        }
        if (near_exercise || !(fv.vega > 1.0e-3 * std::max(fv.price, 1.0))) {
          continue;
        }
        const auto conv = [&](double x, bool &ok) {
          const Result<double> p = andersen_lake(s.S, K, s.T, x, s.r, q, s.side, converged);
          ok = ok && p.has_value();
          return p.has_value() ? *p : kNaN;
        };
        const auto ref = [&](double x, bool &ok) {
          const Result<double> p = andersen_lake(s.S, K, s.T, x, s.r, q, s.side);
          ok = ok && p.has_value();
          return p.has_value() ? *p : kNaN;
        };
        const Rich rc = richardson(conv, sigma, 2.0e-3 * sigma, 0, 4);
        const Rich rr = richardson(ref, sigma, 2.0e-3 * sigma, 0, 4);
        ASSERT_TRUE(rc.ok && rr.ok) << describe(s, K, sigma, q);
        const std::string at = describe(s, K, sigma, q);
        const double tc = 4.0 * rc.err + rc.gain * price_rounding(s.S, K, fv.price);
        const double tr = 4.0 * rr.err + rr.gain * price_rounding(s.S, K, fv.price);
        vs_conv.add(std::abs(fv.vega - rc.value), rc.value, tc, rc.order, at);
        vs_ref.add(std::abs(fv.vega - rr.value), rr.value, tr, rr.order, at);
        ref_vs_conv.add(std::abs(rr.value - rc.value), rc.value, tc + tr, rc.order, at);
      }
    }
    std::cout << "[AlSigmaQFamilyFused] model table " << s.name << " (n=" << vs_conv.n
              << ", resolved " << vs_conv.n_resolved << " / " << vs_ref.n_resolved
              << "): fused vs converged worst rel=" << vs_conv.worst_rel
              << " | fused vs Reference-scheme=" << vs_ref.worst_rel
              << " | Reference vs converged=" << ref_vs_conv.worst_rel << "\n  worst at "
              << vs_conv.at << "\n";
    EXPECT_GE(vs_conv.n, 10u);
    EXPECT_LE(vs_ref.worst_rel, kGross) << vs_ref.at;
  }
}

// ── 5. Exercise, refusals, degeneracy ──────────────────────────────────────────────

TEST(AlSigmaQFamilyFused, ClampedRowsServeIntrinsicWithZeroVegaAndTheFlag) {
  const std::unique_ptr<Built> b = build(derivative_specs()[0]); // spy23-mid-put
  ASSERT_NE(b, nullptr);
  // Deep ITM put at low σ: the decomposition is below intrinsic, the clamp serves K − S.
  const double K = 1.3 * kSpyS;
  const FusedPriceVega fv = sq::price_and_vega(b->f, kSpyS, K, 0.12, kSpyQlo);
  ASSERT_EQ(fv.status, FusedStatus::Ok);
  EXPECT_TRUE(fv.exercised);
  EXPECT_EQ(fv.price, K - kSpyS);
  EXPECT_EQ(fv.vega, 0.0);
  EXPECT_EQ(fv.price, b->f.price(kSpyS, K, 0.12, kSpyQlo).price);
  // An ATM row is in the continuation region: unflagged, positive vega.
  const FusedPriceVega atm = sq::price_and_vega(b->f, kSpyS, kSpyS, 0.2, kSpyQlo);
  ASSERT_EQ(atm.status, FusedStatus::Ok);
  EXPECT_FALSE(atm.exercised);
  EXPECT_GT(atm.vega, 0.0);
}

TEST(AlSigmaQFamilyFused, RefusalsAreTypedAndCarryNaN) {
  AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaQFamily f(g, Side::Put, 100.0);
  // Unprepared.
  FusedPriceVega u = sq::price_and_vega(f, 100.0, 100.0, 0.3, 0.02);
  EXPECT_EQ(u.status, FusedStatus::Unsupported);
  EXPECT_EQ(u.reason, AlSigmaQFamilyStatus::NotPrepared);
  EXPECT_TRUE(std::isnan(u.price) && std::isnan(u.vega));
  ASSERT_TRUE(f.prepare(0.1, 0.5, 12, 0.019, 0.021, 4));
  const auto refused = [&](double S, double K, double sigma, double q, FusedStatus want,
                           AlSigmaQFamilyStatus why) {
    const FusedPriceVega r = sq::price_and_vega(f, S, K, sigma, q);
    EXPECT_EQ(r.status, want) << "S=" << S << " K=" << K << " sigma=" << sigma << " q=" << q;
    EXPECT_EQ(r.reason, why);
    EXPECT_TRUE(std::isnan(r.price) && std::isnan(r.vega));
    EXPECT_EQ(r.reason, f.price(S, K, sigma, q).status); // price()'s refusal tree
  };
  refused(100.0, 100.0, 0.5 + 1.0e-9, 0.02, FusedStatus::OutsideBox,
          AlSigmaQFamilyStatus::SigmaOutsideBox);
  refused(100.0, 100.0, 0.5 * kAlSigmaFamilyMinSigma, 0.02, FusedStatus::OutsideBox,
          AlSigmaQFamilyStatus::SigmaBelowGuard);
  refused(100.0, 100.0, 0.3, 0.021 + 1.0e-12, FusedStatus::OutsideBox,
          AlSigmaQFamilyStatus::YieldOutsideBox);
  refused(0.0, 100.0, 0.3, 0.02, FusedStatus::Unsupported, AlSigmaQFamilyStatus::InvalidArgument);
  refused(100.0, -1.0, 0.3, 0.02, FusedStatus::Unsupported, AlSigmaQFamilyStatus::InvalidArgument);
  refused(100.0, 100.0, kNaN, 0.02, FusedStatus::Unsupported,
          AlSigmaQFamilyStatus::InvalidArgument);
  refused(100.0, 100.0, 0.3, kNaN, FusedStatus::Unsupported, AlSigmaQFamilyStatus::InvalidArgument);
  // The box corners serve (exact node hits on both axes).
  for (const double s : {0.1, 0.5}) {
    for (const double q : {0.019, 0.021}) {
      const FusedPriceVega c = sq::price_and_vega(f, 100.0, 100.0, s, q);
      EXPECT_EQ(c.status, FusedStatus::Ok);
      EXPECT_TRUE(std::isfinite(c.vega));
    }
  }
  // Strike vector: length mismatch writes nothing; a bad strike is NaN, the rest priced.
  const std::vector<double> K3 = {90.0, -1.0, 110.0};
  std::vector<double> p3(3, 7.0);
  std::vector<double> v3(3, 7.0);
  std::vector<double> v2(2, 7.0);
  EXPECT_EQ(sq::price_vega_strikes(f, 100.0, K3, 0.3, 0.02, p3, v2), FusedStatus::Unsupported);
  EXPECT_EQ(p3[0], 7.0);
  EXPECT_EQ(sq::price_vega_strikes(f, 100.0, K3, 0.3, 0.02, p3, v3), FusedStatus::Unsupported);
  EXPECT_TRUE(std::isnan(p3[1]) && std::isnan(v3[1]));
  EXPECT_TRUE(same_bits(p3[0], sq::price_and_vega(f, 100.0, 90.0, 0.3, 0.02).price));
  EXPECT_TRUE(same_bits(v3[2], sq::price_and_vega(f, 100.0, 110.0, 0.3, 0.02).vega));
  EXPECT_EQ(sq::price_vega_strikes(f, 100.0, K3, 0.3, 0.5, p3, v3), FusedStatus::OutsideBox);
  EXPECT_TRUE(std::isnan(p3[0]) && std::isnan(v3[2]));
  // Tables and tiles: a tile outside the box, a tile of another build, a stale table.
  sq::PremiumTable t;
  ASSERT_EQ(sq::build_premium_table(f, t), FusedStatus::Ok);
  sq::QTile tile;
  EXPECT_EQ(sq::contract_q(t, 0.5, tile), FusedStatus::OutsideBox);
  EXPECT_FALSE(tile.ok);
  EXPECT_EQ(sq::contract_q(t, kNaN, tile), FusedStatus::Unsupported);
  ASSERT_EQ(sq::contract_q(t, 0.02, tile), FusedStatus::Ok);
  EXPECT_EQ(sq::price_vega_on(t, tile, 100.0, 100.0, 0.3).status, FusedStatus::Ok);
  EXPECT_EQ(sq::price_vega_on(t, tile, 100.0, 100.0, 0.6).status, FusedStatus::OutsideBox);
  EXPECT_EQ(sq::price_vega_on(t, tile, 100.0, kNaN, 0.3).status, FusedStatus::Unsupported);
  sq::PremiumTable t2;
  ASSERT_EQ(sq::build_premium_table(f, t2), FusedStatus::Ok);
  EXPECT_EQ(sq::price_vega_on(t2, tile, 100.0, 100.0, 0.3).status, FusedStatus::Unsupported);
  // The geometry re-prepared: the family refuses, its table (a snapshot) still serves the
  // map it was built from — bit for bit what it served before.
  const FusedPriceVega before = sq::price_vega_on(t, tile, 100.0, 95.0, 0.3);
  g.prepare(0.75, 0.05, 0.02);
  refused(100.0, 100.0, 0.3, 0.02, FusedStatus::Unsupported,
          AlSigmaQFamilyStatus::GeometryRePrepared);
  sq::PremiumTable t3;
  EXPECT_EQ(sq::build_premium_table(f, t3), FusedStatus::Unsupported);
  EXPECT_FALSE(t3.ok);
  const FusedPriceVega after = sq::price_vega_on(t, tile, 100.0, 95.0, 0.3);
  EXPECT_TRUE(same_bits(before.price, after.price) && same_bits(before.vega, after.vega));
  // Moved-from family.
  ASSERT_TRUE(f.prepare(0.1, 0.5, 12, 0.019, 0.021, 4));
  AlSigmaQFamily moved(std::move(f));
  EXPECT_EQ(sq::price_and_vega(moved, 100.0, 100.0, 0.3, 0.02).status, FusedStatus::Ok);
  EXPECT_EQ(sq::price_and_vega(f, 100.0, 100.0, 0.3, 0.02).status, // NOLINT: moved-from
            FusedStatus::Unsupported);
}

TEST(AlSigmaQFamilyFused, ATransformedBoundaryAtItsZeroIsNearNodeDegenerate) {
  // s = √max(y, 0) has no derivative where y crosses 0 with a nonzero slope; a premium
  // node whose interpolated y sits there (within the interpolant's rounding) types the
  // call NearNodeDegenerate: the price is served, vega is NaN (never a silent 0 / ∞).
  const std::unique_ptr<Built> b = build(derivative_specs()[0]);
  ASSERT_NE(b, nullptr);
  const auto *state = sq::QFamilyAccess::state(b->f);
  sq::PremiumNodes pn;
  ASSERT_TRUE(state->lay_premium_nodes(pn));
  sq::NodeBoundary good;
  state->boundary_at_nodes(pn, 0.2, kSpyQlo, good);
  EXPECT_FALSE(good.degenerate);
  // The same nodes with one boundary node's y forced to the transform's zero.
  double y[sq::kMaxPremiumNodes] = {};
  double dy[sq::kMaxPremiumNodes] = {};
  for (unsigned i = 0; i < pn.n; ++i) {
    y[i] = good.s[i] * good.s[i];
    dy[i] = 2.0 * good.s[i] * good.ds[i];
  }
  unsigned first = 0;
  while (first < pn.n && pn.kind[first] != sq::PremiumNodeKind::Boundary) {
    ++first;
  }
  ASSERT_LT(first, pn.n);
  y[first] = 0.0;
  dy[first] = 1.0;
  sq::NodeBoundary bad;
  sq::finish_node_boundary(pn, y, dy, 1.0, 0.2, bad);
  EXPECT_TRUE(bad.degenerate);
  sq::QCarry c;
  sq::carry_at(pn, Side::Put, kSpyR, kSpyQlo, c);
  const FusedPriceVega r = sq::premium_pass(pn, c, bad, kSpyS, kSpyS, false, true);
  EXPECT_EQ(r.status, FusedStatus::NearNodeDegenerate);
  EXPECT_TRUE(std::isfinite(r.price));
  EXPECT_TRUE(std::isnan(r.vega));
  // dy = 0 at y = 0 is a flat transform: no degeneracy.
  dy[first] = 0.0;
  sq::NodeBoundary flat;
  sq::finish_node_boundary(pn, y, dy, 1.0, 0.2, flat);
  EXPECT_FALSE(flat.degenerate);
}

// ── 6. Tables, memory, threads ─────────────────────────────────────────────────────

TEST(AlSigmaQFamilyFused, PremiumTableBytesArePinned) {
  // (n_prem + 1) · n_q · n_sigma doubles of precontracted tensor + the inline block (the
  // cost table's memory row); the table keeps no copy of the family's τ-node series.
  const std::unique_ptr<Built> b = build(derivative_specs()[0]); // (12, 4)
  ASSERT_NE(b, nullptr);
  sq::PremiumTable t;
  ASSERT_EQ(sq::build_premium_table(b->f, t), FusedStatus::Ok);
  EXPECT_EQ(t.nodes.n, 48u);
  EXPECT_EQ(t.h_size, 49u * 4u * 12u);
  EXPECT_EQ(t.bytes(), sizeof(sq::PremiumTable) + t.h_size * sizeof(double));
  std::cout << "[AlSigmaQFamilyFused] premium table (12, 4) bytes=" << t.bytes()
            << " (inline " << sizeof(sq::PremiumTable) << "), QTile " << sizeof(sq::QTile)
            << " B, NodeBoundary " << sizeof(sq::NodeBoundary) << " B, family state "
            << b->f.state_bytes() << " B\n";
}

TEST(AlSigmaQFamilyFused, AMovedFromPremiumTableIsUnpreparedAndRebuildsLikeAFreshOne) {
  // K2 holds tables across batches and reuses a scratch table after moving it out
  // (review MJ1). A move hands the snapshot over — stamp included, so the source's tiles
  // serve on the target — and leaves the source a default table: refused by every entry,
  // and rebuilt into at the same size exactly like a fresh table (no write through a
  // null tensor). The moved-from object is the subject of this test.
  const std::unique_ptr<Built> b = build(derivative_specs()[0]); // spy23-mid-put (12, 4)
  ASSERT_NE(b, nullptr);
  const std::vector<double> K = strikes_around(kSpyS, 0.85, 1.15, 13);
  const auto serve = [&](const sq::PremiumTable &t, const sq::QTile &tile) {
    std::vector<double> out;
    for (const double sigma : {0.12, 0.27, 0.45}) {
      for (const double k : K) {
        const FusedPriceVega fv = sq::price_vega_on(t, tile, kSpyS, k, sigma);
        EXPECT_EQ(fv.status, FusedStatus::Ok);
        out.push_back(fv.price);
        out.push_back(fv.vega);
      }
    }
    return out;
  };
  const auto same = [](const std::vector<double> &x, const std::vector<double> &y) {
    if (x.size() != y.size()) {
      return false;
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
      if (!same_bits(x[i], y[i])) {
        return false;
      }
    }
    return true;
  };
  sq::PremiumTable fresh;
  ASSERT_EQ(sq::build_premium_table(b->f, fresh), FusedStatus::Ok);
  sq::QTile fresh_tile;
  ASSERT_EQ(sq::contract_q(fresh, kSpyQhi, fresh_tile), FusedStatus::Ok);
  const std::vector<double> want = serve(fresh, fresh_tile);
  ASSERT_EQ(want.size(), 2u * 3u * K.size());

  sq::PremiumTable src;
  ASSERT_EQ(sq::build_premium_table(b->f, src), FusedStatus::Ok);
  sq::QTile src_tile;
  ASSERT_EQ(sq::contract_q(src, kSpyQhi, src_tile), FusedStatus::Ok);
  const std::size_t size = src.h_size;
  const std::uint64_t stamp = src.stamp;
  const auto default_state = [&](const sq::PremiumTable &t) {
    EXPECT_FALSE(t.ok);
    EXPECT_EQ(t.stamp, 0u);
    EXPECT_EQ(t.h.get(), nullptr);
    EXPECT_EQ(t.h_size, 0u);
    EXPECT_EQ(t.bytes(), sizeof(sq::PremiumTable));
    sq::QTile tile;
    EXPECT_EQ(sq::contract_q(t, kSpyQhi, tile), FusedStatus::Unsupported);
    EXPECT_FALSE(tile.ok);
    EXPECT_EQ(sq::price_vega_on(t, src_tile, kSpyS, kSpyS, 0.27).status,
              FusedStatus::Unsupported);
  };

  // Move construction: the target serves the source's tile bit for bit.
  sq::PremiumTable target(std::move(src));
  EXPECT_TRUE(target.ok);
  EXPECT_EQ(target.stamp, stamp);
  EXPECT_EQ(target.h_size, size);
  ASSERT_NE(target.h.get(), nullptr);
  EXPECT_TRUE(same(serve(target, src_tile), want));
  default_state(src);

  // Rebuild into the moved-from table at the same size: it allocates afresh and serves
  // the fresh table's numbers; its old tile belongs to another build now.
  ASSERT_EQ(sq::build_premium_table(b->f, src), FusedStatus::Ok);
  EXPECT_TRUE(src.ok);
  EXPECT_EQ(src.h_size, size);
  ASSERT_NE(src.h.get(), nullptr);
  EXPECT_NE(src.stamp, stamp);
  sq::QTile rebuilt_tile;
  ASSERT_EQ(sq::contract_q(src, kSpyQhi, rebuilt_tile), FusedStatus::Ok);
  EXPECT_TRUE(same(serve(src, rebuilt_tile), want));
  EXPECT_EQ(sq::price_vega_on(src, src_tile, kSpyS, kSpyS, 0.27).status,
            FusedStatus::Unsupported);

  // Move assignment over a built table: the same contract, and the same rebuild.
  target = std::move(src);
  EXPECT_TRUE(same(serve(target, rebuilt_tile), want));
  default_state(src);
  ASSERT_EQ(sq::build_premium_table(b->f, src), FusedStatus::Ok);
  sq::QTile again_tile;
  ASSERT_EQ(sq::contract_q(src, kSpyQhi, again_tile), FusedStatus::Ok);
  EXPECT_TRUE(same(serve(src, again_tile), want));

  // reset(): the default state on demand.
  target.reset();
  default_state(target);
}

TEST(AlSigmaQFamilyFused, TheExposedTileStagesComposeToPriceVegaOnBitForBit) {
  // The lane-kernel contract (review m4): node_boundary_from_tile + premium_pass on a
  // table's nodes and a tile's carry is price_vega_on, and sigma_z / bary_basis /
  // bary_basis_derivative are the helpers it runs (exact node hit: the unit vector).
  const std::unique_ptr<Built> b = build(derivative_specs()[3]); // spy23-mid-call
  ASSERT_NE(b, nullptr);
  sq::PremiumTable t;
  ASSERT_EQ(sq::build_premium_table(b->f, t), FusedStatus::Ok);
  sq::QTile tile;
  ASSERT_EQ(sq::contract_q(t, kSpyQlo, tile), FusedStatus::Ok);
  std::size_t n = 0;
  for (const double sigma : {0.10, 0.19, 0.33, 0.50}) {
    sq::NodeBoundary nb;
    sq::node_boundary_from_tile(t, tile, sigma, nb);
    for (const double K : strikes_around(kSpyS, 0.8, 1.2, 9)) {
      const FusedPriceVega want = sq::price_vega_on(t, tile, kSpyS, K, sigma);
      // Calls in internal-put coordinates: (S', K') = (K, S).
      const FusedPriceVega got = sq::premium_pass(t.nodes, tile.carry, nb, K, kSpyS, t.fast,
                                                  true);
      EXPECT_EQ(got.status, want.status);
      EXPECT_TRUE(same_bits(got.price, want.price) && same_bits(got.vega, want.vega))
          << "sigma=" << sigma << " K=" << K;
      ++n;
    }
  }
  EXPECT_EQ(n, 36u);
  const sq::SigmaZ lo = sq::sigma_z(t.axis, t.u_lo, t.u_hi, t.sigma_lo);
  EXPECT_EQ(lo.z, -1.0);
  EXPECT_DOUBLE_EQ(lo.dz, 2.0 / (t.u_hi - t.u_lo) / t.sigma_lo);
  double basis[16] = {};
  sq::bary_basis(t.sz, t.sw, t.n_sigma, lo.z, basis);
  double deriv[16] = {};
  sq::bary_basis_derivative(basis, t.n_sigma, deriv);
  double sum = 0.0;
  double dsum = 0.0;
  for (unsigned s = 0; s < t.n_sigma; ++s) {
    EXPECT_EQ(basis[s], t.sz[s] == -1.0 ? 1.0 : 0.0) << "s=" << s;
    sum += basis[s];
    dsum += deriv[s];
  }
  EXPECT_EQ(sum, 1.0);
  EXPECT_NEAR(dsum, 0.0, 1.0e-12); // the derivative of a partition of unity
}

TEST(AlSigmaQFamilyFused, ConcurrentFusedCallsAreBitIdenticalToTheSerialLoop) {
  const std::unique_ptr<Built> b = build(derivative_specs()[3]); // spy23-mid-call
  ASSERT_NE(b, nullptr);
  const std::vector<double> K = strikes_around(kSpyS, 0.8, 1.2, 40);
  std::vector<double> serial;
  for (const double k : K) {
    const FusedPriceVega fv = sq::price_and_vega(b->f, kSpyS, k, 0.23, kSpyQhi);
    serial.push_back(fv.price);
    serial.push_back(fv.vega);
  }
  constexpr unsigned kThreads = 4;
  std::vector<std::vector<double>> got(kThreads);
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < kThreads; ++t) {
    pool.emplace_back([&, t] {
      for (const double k : K) {
        const FusedPriceVega fv = sq::price_and_vega(b->f, kSpyS, k, 0.23, kSpyQhi);
        got[t].push_back(fv.price);
        got[t].push_back(fv.vega);
      }
    });
  }
  for (std::thread &th : pool) {
    th.join();
  }
  for (unsigned t = 0; t < kThreads; ++t) {
    ASSERT_EQ(got[t].size(), serial.size());
    for (std::size_t i = 0; i < serial.size(); ++i) {
      EXPECT_TRUE(same_bits(got[t][i], serial[i])) << "thread " << t << " i " << i;
    }
  }
}

} // namespace
