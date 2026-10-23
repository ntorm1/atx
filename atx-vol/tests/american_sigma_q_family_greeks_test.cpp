#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_sigma_q_family.hpp" // the K1 fused entries + the A0 stages

// A0 (r6, 2026-10-23): the primary Greek bundle on the (σ, q) family (the A0 block of
// api/pricing/american.hpp). Claims, kept apart as K1 / L3g keep theirs (deep review F11):
//   * IDENTITY — the bundle's price is K1's fused price bit for bit on every admitted
//     row, and its vega K1's fused vega bit for bit on every served row; the SoA chain
//     entry is the scalar entry lane for lane; θ is the PDE identity of the served fields.
//   * FAMILY-MAP DERIVATIVE — Δ, Γ, vega, phi against Richardson-refined differences of
//     the bundle's OWN price field (the family's map), every row held to the ladder's own
//     error estimate plus rounding, relative claims on resolved rows only; rho against
//     the central difference of second families at r ± h (its definition, to O(h²)).
//   * MASKS — every AlFamilyGreekReason is reachable and carries what it promises; the
//     convention is populated on every bundle.
//   * BUILD ROUTE — AVX2- vs scalar-built families: the measured gap, pinned.
//   * OVERLAP — a collapsed q box against the L3g σ-family bundle.
// The distance to the CONVERGED Andersen-Lake model is the slow suite at the end
// (AlSigmaQFamilyGreeksModel, ATX_VOL_SLOW_FILTER). Tables: docs/sigma-q-family-greeks.md.

namespace {

using atx::core::Result;
using atx::core::Status;
using atx::vol::AlExpiryGeometry;
using atx::vol::AlFamilyGreek;
using atx::vol::AlFamilyGreekClock;
using atx::vol::AlFamilyGreekHeldFixed;
using atx::vol::AlFamilyGreekMask;
using atx::vol::AlFamilyGreekModel;
using atx::vol::AlFamilyGreekReason;
using atx::vol::AlFamilyGreekReasons;
using atx::vol::AlOpts;
using atx::vol::AlSigmaFamily;
using atx::vol::AlSigmaFamilyAxis;
using atx::vol::AlSigmaFamilyCarryBumps;
using atx::vol::AlSigmaFamilyCarryStatus;
using atx::vol::AlSigmaFamilyGreeks;
using atx::vol::AlSigmaFamilyGreeksMask;
using atx::vol::AlSigmaFamilyStatus;
using atx::vol::AlSigmaQFamily;
using atx::vol::AlSigmaQFamilyGreeks;
using atx::vol::AlSigmaQFamilyRateBumps;
using atx::vol::AlSigmaQFamilyStatus;
using atx::vol::AlSigmaQGreekColumns;
using atx::vol::AlSigmaQGreeksRequest;
using atx::vol::andersen_lake;
using atx::vol::family_greeks;
using atx::vol::family_greeks_many;
using atx::vol::kAlFamilyGreekCount;
using atx::vol::kAlSigmaFamilyCarryBump;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;
using atx::vol::simd::SimdIsa;
namespace sq = atx::vol::sigma_q_family_detail;
using Greek = AlFamilyGreek;
using Reason = AlFamilyGreekReason;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEps = std::numeric_limits<double>::epsilon();

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

[[nodiscard]] std::vector<double> strikes_around(double S, double lo, double hi, unsigned n) {
  std::vector<double> v;
  for (unsigned i = 0; i < n; ++i) {
    v.push_back(S * (lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(n - 1)));
  }
  return v;
}

// SPY 2026-11-06 (23 d) under funding-pv (the L2h-b / K1 fixture) and SPY 2028-12-15
// (2.17 y, q < 0, puts only); two wide q boxes (n_q 6 / 8) on a T = 1 grid.
constexpr double kSpyS = 772.99;
constexpr double kSpyT = 0.0682;
constexpr double kSpyR = 0.04496;
constexpr double kSpyQlo = 0.001589;
constexpr double kSpyQhi = 0.001678;

[[nodiscard]] std::vector<Spec> specs() {
  std::vector<Spec> v;
  v.push_back(Spec{"spy23-mid-put", kSpyS, kSpyT, kSpyR, kSpyQlo, kSpyQhi, Side::Put, 0.10, 0.50,
                   12, 4, {0.12, 0.2, 0.3, 0.45}, strikes_around(kSpyS, 0.8, 1.3, 11)});
  v.push_back(Spec{"spy23-mid-call", kSpyS, kSpyT, kSpyR, kSpyQlo, kSpyQhi, Side::Call, 0.10,
                   0.50, 12, 4, {0.12, 0.2, 0.3, 0.45}, strikes_around(kSpyS, 0.7, 1.2, 11)});
  v.push_back(Spec{"spylong-put", 772.98, 2.1673, 0.05153, -0.003298, -0.002272, Side::Put, 0.10,
                   0.45, 16, 4, {0.15, 0.3}, strikes_around(772.98, 0.6, 1.6, 11)});
  v.push_back(Spec{"wideq-put-nq6", 100.0, 1.0, 0.05, 0.0, 0.01, Side::Put, 0.10, 0.60, 12, 6,
                   {0.15, 0.35}, strikes_around(100.0, 0.7, 1.4, 8)});
  v.push_back(Spec{"wideq-call-nq8", 100.0, 1.0, 0.05, 0.005, 0.015, Side::Call, 0.10, 0.60, 12,
                   8, {0.15, 0.35}, strikes_around(100.0, 0.7, 1.4, 8)});
  return v;
}

// q probes: both box ends (q nodes, exact hits) and two interior points.
[[nodiscard]] std::vector<double> q_probes(const Spec &s) {
  return {s.q_lo, s.q_lo + 0.37 * (s.q_hi - s.q_lo), s.q_lo + 0.81 * (s.q_hi - s.q_lo), s.q_hi};
}

[[nodiscard]] std::string describe(const Spec &s, double K, double sigma, double q) {
  std::ostringstream os;
  os << std::setprecision(17) << s.name << " K=" << K << " sigma=" << sigma << " q=" << q;
  return os.str();
}

[[nodiscard]] AlSigmaQGreeksRequest request(AlFamilyGreekMask m) {
  AlSigmaQGreeksRequest r;
  r.mask = m;
  return r;
}

// The bundle's own price field (the fused map) — what every family-map difference reads.
[[nodiscard]] double map_price(const AlSigmaQFamily &f, double S, double K, double sigma, double q,
                               bool &ok) {
  const AlSigmaQFamilyGreeks g =
      family_greeks(f, S, K, sigma, q, request(AlFamilyGreekMask::none()));
  ok = ok && g.status == AlSigmaQFamilyStatus::Ok;
  return g.price;
}

// ── Richardson-refined differences (K1's ladder; a second-derivative twin) ─────────

struct Rich {
  double value = kNaN; // the extrapolated derivative at the selected level
  double err = kNaN;   // its error estimate |R_l − R_{l−1}|
  double gain = kNaN;  // rounding amplification of the selected combination
  bool ok = false;
};

// d f / d x (order 1) or d² f / d x² (order 2) at x by fourth-order stencils at h_l = h0 /
// 2^l, extrapolated pairwise R_l = D_l + (D_l − D_{l−1}) / 15; the level whose R_l agrees
// best with R_{l−1} is taken. `dir` ±1 = the 5-point one-sided first-derivative stencil
// (box ends). A caller's tolerance is 4 err + gain · (f's rounding).
template <class F>
[[nodiscard]] Rich richardson(F &&f, double x, double h0, int order = 1, int dir = 0,
                              int levels = 8) {
  constexpr int kMaxLevels = 8;
  const int kLevels = std::clamp(levels, 3, kMaxLevels);
  Rich r;
  bool ok = true;
  double D[kMaxLevels] = {};
  double R[kMaxLevels] = {};
  const double f0 = (order == 2 || dir != 0) ? f(x, ok) : 0.0;
  for (int l = 0; l < kLevels; ++l) {
    const double h = h0 / static_cast<double>(1 << l);
    if (order == 2) {
      D[l] = (-f(x + 2.0 * h, ok) + 16.0 * f(x + h, ok) - 30.0 * f0 + 16.0 * f(x - h, ok) -
              f(x - 2.0 * h, ok)) /
             (12.0 * h * h);
    } else if (dir == 0) {
      D[l] = (-f(x + 2.0 * h, ok) + 8.0 * f(x + h, ok) - 8.0 * f(x - h, ok) + f(x - 2.0 * h, ok)) /
             (12.0 * h);
    } else {
      const double d = static_cast<double>(dir) * h;
      D[l] = (-25.0 * f0 + 48.0 * f(x + d, ok) - 36.0 * f(x + 2.0 * d, ok) +
              16.0 * f(x + 3.0 * d, ok) - 3.0 * f(x + 4.0 * d, ok)) /
             (12.0 * d);
    }
    R[l] = (l == 0) ? D[0] : D[l] + (D[l] - D[l - 1]) / 15.0;
  }
  int best = 2;
  double best_err = std::numeric_limits<double>::infinity();
  for (int l = 2; l < kLevels; ++l) {
    const double e = std::abs(R[l] - R[l - 1]);
    if (e < best_err) {
      best_err = e;
      best = l;
    }
  }
  const double h = h0 / static_cast<double>(1 << best);
  const double coef = (order == 2) ? 64.0 / 12.0 : ((dir == 0) ? 18.0 / 12.0 : 128.0 / 12.0);
  const double hp = (order == 2) ? h * h : h;
  const double hp2 = (order == 2) ? 4.0 * h * h : 2.0 * h;
  r.value = R[best];
  r.err = best_err;
  r.gain = coef * (16.0 / hp + 1.0 / hp2) / 15.0;
  r.ok = ok;
  return r;
}

// A price's rounding: a few ulps of S + K + P (the European leg's two terms and the
// premium are each O(S + K)).
[[nodiscard]] double price_rounding(double S, double K, double P) noexcept {
  return 4.0 * kEps * (S + K + std::abs(P));
}

constexpr double kResolved = 1.0e-9;

// An error table: rows, resolved rows, worst relative gap (resolved), worst gap / tol.
struct Table {
  std::size_t n = 0;
  std::size_t n_resolved = 0;
  double worst_rel = 0.0;
  double worst_ratio = 0.0;
  double worst_unresolved = 0.0; // max tol / |ref| over the rows that do not resolve
  std::string at;
  std::string ratio_at;
  void add(double gap, double ref, double tol, const std::string &where) {
    ++n;
    const double ratio = (tol > 0.0) ? gap / tol : (gap > 0.0 ? kNaN : 0.0);
    if (!(ratio <= worst_ratio)) {
      worst_ratio = ratio;
      ratio_at = where;
    }
    if (tol > kResolved * std::abs(ref)) {
      worst_unresolved = std::max(worst_unresolved, tol / std::abs(ref));
      return;
    }
    ++n_resolved;
    const double rel = gap / std::abs(ref);
    if (rel > worst_rel) {
      worst_rel = rel;
      at = where;
    }
  }
  [[nodiscard]] std::string line() const {
    std::ostringstream os;
    os << "n=" << n << " resolved=" << n_resolved << " worst rel (resolved)=" << worst_rel
       << " worst gap/tol=" << worst_ratio << " unresolved max tol/|ref|=" << worst_unresolved;
    return os.str();
  }
};

// ── 1. Identity with K1's fused pass ───────────────────────────────────────────────

TEST(AlSigmaQFamilyGreeks, PriceAndVegaAreTheFusedPassBitsOnEveryServedRow) {
  // The bundle's price IS K1's fused price (sigma_q_family_detail::price_and_vega) on
  // every admitted row, whatever the mask; its vega is K1's vega bit for bit on every row
  // the bundle serves (reason Served). On the exercise regime the bundle serves the
  // exercise Greeks (vega 0.0, reason Exercised) where K1's slope may be nonzero.
  std::size_t served = 0;
  std::size_t exercised = 0;
  for (const Spec &s : specs()) {
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    for (const double sigma : s.sigmas) {
      for (const double q : q_probes(s)) {
        for (const double K : s.strikes) {
          const sq::FusedPriceVega fv = sq::price_and_vega(b->f, s.S, K, sigma, q);
          ASSERT_EQ(fv.status, sq::FusedStatus::Ok) << describe(s, K, sigma, q);
          for (const AlFamilyGreekMask m :
               {AlFamilyGreekMask::none(), AlFamilyGreekMask::primary(),
                AlFamilyGreekMask::none().with(Greek::Vega)}) {
            const AlSigmaQFamilyGreeks g = family_greeks(b->f, s.S, K, sigma, q, request(m));
            ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok);
            EXPECT_TRUE(same_bits(g.price, fv.price)) << describe(s, K, sigma, q);
            EXPECT_EQ(g.exercised, fv.exercised) << describe(s, K, sigma, q);
            if (!m.has(Greek::Vega)) {
              continue;
            }
            if (g.exercised) {
              EXPECT_EQ(g.reason_of(Greek::Vega), Reason::Exercised);
              EXPECT_EQ(g.vega, 0.0);
              ++exercised;
            } else {
              EXPECT_EQ(g.reason_of(Greek::Vega), Reason::Served);
              EXPECT_TRUE(same_bits(g.vega, fv.vega)) << describe(s, K, sigma, q);
              ++served;
            }
          }
        }
      }
    }
  }
  std::cout << "[AlSigmaQFamilyGreeks] identity: served vega rows=" << served
            << " exercised rows=" << exercised << "\n";
  EXPECT_GT(served, 500u);
  EXPECT_GT(exercised, 0u);
}

// ── 2. Δ, Γ, vega, phi against the family map's own differences ───────────────────

struct MapTables {
  Table delta, gamma, vega, phi;
  std::size_t skipped_near_exercise = 0;
  std::size_t gamma_near_edge = 0; // band rows: Γ is NearExerciseEdge (review-A0 MJ1)
};

void map_rows(const Spec &s, const AlSigmaQFamily &f, MapTables &t) {
  const double sqrtT = std::sqrt(s.T);
  for (const double sigma : s.sigmas) {
    for (const double q : q_probes(s)) {
      for (const double K : s.strikes) {
        const std::string at = describe(s, K, sigma, q);
        const AlSigmaQFamilyGreeks g = family_greeks(f, s.S, K, sigma, q);
        ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok) << at;
        // The S stencil spans ±2 h0; it must stay on the continuation side.
        const double hS = 0.01 * s.S * sigma * sqrtT;
        bool any_exercised = g.exercised;
        for (const double x : {s.S - 2.0 * hS, s.S + 2.0 * hS}) {
          any_exercised = any_exercised || family_greeks(f, x, K, sigma, q).exercised;
        }
        if (any_exercised) {
          ++t.skipped_near_exercise;
          continue;
        }
        const double rnd = price_rounding(s.S, K, g.price);
        const auto PS = [&](double x, bool &ok) { return map_price(f, x, K, sigma, q, ok); };
        const Rich d1 = richardson(PS, s.S, hS, 1);
        const Rich d2 = richardson(PS, s.S, hS, 2);
        ASSERT_TRUE(d1.ok && d2.ok) << at;
        t.delta.add(std::abs(g.delta - d1.value), d1.value, 4.0 * d1.err + d1.gain * rnd, at);
        if (g.near_edge) { // the near-edge band: Γ is not served there (review-A0 MJ1)
          ASSERT_EQ(g.reason_of(Greek::Gamma), Reason::NearExerciseEdge) << at;
          ASSERT_TRUE(std::isnan(g.gamma)) << at;
          ++t.gamma_near_edge;
        } else {
          t.gamma.add(std::abs(g.gamma - d2.value), d2.value, 4.0 * d2.err + d2.gain * rnd,
                      at);
        }
        const auto Psig = [&](double x, bool &ok) { return map_price(f, s.S, K, x, q, ok); };
        const Rich dv = richardson(Psig, sigma, 2.0e-3 * sigma, 1);
        ASSERT_TRUE(dv.ok) << at;
        t.vega.add(std::abs(g.vega - dv.value), dv.value, 4.0 * dv.err + dv.gain * rnd, at);
        // q: one-sided at the box ends (the q nodes; 4 h0 = 0.8 of the box), central inside
        // (2 h0 = 0.8 of the distance to the nearer end) — the widest steps the box allows:
        // the price's rounding over the step is this reference's floor on a narrow box.
        const int dir = (q == s.q_lo) ? 1 : ((q == s.q_hi) ? -1 : 0);
        const double hq = (dir != 0) ? 0.2 * (s.q_hi - s.q_lo)
                                     : 0.4 * std::min(q - s.q_lo, s.q_hi - q);
        const auto Pq = [&](double x, bool &ok) { return map_price(f, s.S, K, sigma, x, ok); };
        const Rich dq = richardson(Pq, q, hq, 1, dir);
        ASSERT_TRUE(dq.ok) << at;
        ASSERT_EQ(g.reason_of(Greek::Phi), Reason::Served) << at;
        t.phi.add(std::abs(g.phi - dq.value), dq.value, 4.0 * dq.err + dq.gain * rnd, at);
      }
    }
  }
}

TEST(AlSigmaQFamilyGreeks, DeltaGammaVegaPhiAreTheFamilyMapsDerivatives) {
  // Every row is held to gap <= the ladder's own tolerance (4 |R_l − R_{l−1}| + gain ·
  // rounding): a formula error (a dropped boundary-motion term, a wrong chain factor) is
  // O(1) relative and fails it. Rows whose S stencil reaches the exercise regime are
  // skipped (no smooth map there) and counted.
  MapTables all;
  for (const Spec &s : specs()) {
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    MapTables t;
    map_rows(s, b->f, t);
    std::cout << "[AlSigmaQFamilyGreeks] map " << s.name << " skipped(near exercise)="
              << t.skipped_near_exercise << " gamma NearExerciseEdge=" << t.gamma_near_edge
              << "\n  delta " << t.delta.line() << "\n  gamma "
              << t.gamma.line() << "\n  vega  " << t.vega.line() << "\n  phi   " << t.phi.line()
              << "\n";
    for (auto [dst, src] : {std::pair{&all.delta, &t.delta}, std::pair{&all.gamma, &t.gamma},
                            std::pair{&all.vega, &t.vega}, std::pair{&all.phi, &t.phi}}) {
      dst->n += src->n;
      dst->n_resolved += src->n_resolved;
      if (src->worst_rel > dst->worst_rel) {
        dst->worst_rel = src->worst_rel;
        dst->at = src->at;
      }
      if (!(src->worst_ratio <= dst->worst_ratio)) {
        dst->worst_ratio = src->worst_ratio;
        dst->ratio_at = src->ratio_at;
      }
      dst->worst_unresolved = std::max(dst->worst_unresolved, src->worst_unresolved);
    }
    all.skipped_near_exercise += t.skipped_near_exercise;
  }
  std::cout << "[AlSigmaQFamilyGreeks] map ALL\n  delta " << all.delta.line() << " at "
            << all.delta.at << "\n  gamma " << all.gamma.line() << " at " << all.gamma.at
            << "\n  vega  " << all.vega.line() << " at " << all.vega.at << "\n  phi   "
            << all.phi.line() << " at " << all.phi.at << "\n";
  EXPECT_LE(all.delta.worst_ratio, 1.0) << all.delta.ratio_at;
  EXPECT_LE(all.gamma.worst_ratio, 1.0) << all.gamma.ratio_at;
  EXPECT_LE(all.vega.worst_ratio, 1.0) << all.vega.ratio_at;
  EXPECT_LE(all.phi.worst_ratio, 1.0) << all.phi.ratio_at;
  // Relative pins on RESOLVED rows (2-figure round-ups of the measured worst, A0 r6:
  // Δ 1.60914e-11 of 395 / 472, vega 2.36096e-11 of 263, phi 3.14904e-11 of 104 — the
  // narrow SPY q box resolves no phi row: the price's rounding over a step that fits
  // the 8.9e-5-wide box is ~5e-8 of phi there, so those rows carry gap <= tol only).
  // Γ (a second difference) resolves no row at 1e-9 and carries gap <= tol only.
  EXPECT_LE(all.delta.worst_rel, 1.7e-11) << all.delta.at;
  EXPECT_LE(all.vega.worst_rel, 2.4e-11) << all.vega.at;
  EXPECT_LE(all.phi.worst_rel, 3.2e-11) << all.phi.at;
  EXPECT_GE(all.delta.n_resolved, 300u);
  EXPECT_GE(all.vega.n_resolved, 200u);
  EXPECT_GE(all.phi.n_resolved, 100u);
}

// ── 3. rho against the second families' own central difference ─────────────────────

// How far (in σ√T) the exercise FLAG's edge may sit on the continuation side of the
// boundary's own edge B'_now (the bundle's edge_sd): the floored zone, where the served
// price meets intrinsic just above the edge (A0 fix round 1: measured, see the tests).
constexpr double kFlooredZoneSd = 3.4e-3; // measured 3.36e-3 (the slow suite's widest)

// The option spot at which the family's exercise flag flips (bisection on S at fixed
// K, σ, q): the current exercise edge in option-spot terms. NaN when no flip lies in
// [S/4, 4S] (an edge too far to matter).
[[nodiscard]] double edge_spot(const AlSigmaQFamily &f, Side side, double S, double K,
                               double sigma, double q) {
  const auto ex = [&](double x) { return family_greeks(f, x, K, sigma, q).exercised; };
  double lo = 0.25 * S;
  double hi = 4.0 * S;
  // Puts exercise below the edge, calls above it.
  const bool put = side == Side::Put;
  if (ex(lo) != put || ex(hi) == put) {
    return kNaN;
  }
  for (int it = 0; it < 80; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (ex(mid) == put) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

// The near-boundary band (the error class the brief reports apart): the spot within a
// quarter standard deviation σ√T of the family's current exercise edge.
[[nodiscard]] bool in_band(const AlSigmaQFamily &f, Side side, double S, double K, double sigma,
                           double q, double T) {
  const double edge = edge_spot(f, side, S, K, sigma, q);
  return std::isfinite(edge) && std::abs(std::log(S / edge)) < 0.25 * sigma * std::sqrt(T);
}

TEST(AlSigmaQFamilyGreeks, RhoIsTheBumpedFamiliesCentralDifferenceToItsTruncation) {
  // The served rho is the frozen-boundary terms plus Σ ∂P/∂y · dY/dr with dY/dr the
  // central difference of families at r ± h, A(h) = f'(r) + P_y y''' h² / 6 + O(h⁴); the
  // central difference of those families' PRICES is D(h) = f'(r) + f''' h² / 6 + O(h⁴),
  // and R = (4 D(h/2) − D(h)) / 3 = f'(r) + O(h⁴) is the reference. A(h) − R is then the
  // rate-bump set's OWN truncation: the discriminator (review-L3g m1's, run on this
  // route) builds a second set at h/2 — if the gap is truncation it falls ~4x. Rows are
  // split by the near-boundary band.
  const double h = kAlSigmaFamilyCarryBump;
  double worst[2] = {0.0, 0.0};      // |A(h) − R| / |R|, [outside, inside the band]
  double worst_half[2] = {0.0, 0.0}; // |A(h/2) − R| / |R|
  double worst_dh[2] = {0.0, 0.0};   // |A(h) − D(h)| / |D(h)|
  std::string at[2];
  std::vector<double> ratio; // |A(h) − R| / |A(h/2) − R| where the gap is resolved
  std::size_t rows[2] = {0, 0};
  for (const Spec &s : specs()) {
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    AlSigmaQFamilyRateBumps bumps;
    ASSERT_TRUE(bumps.prepare(b->f)) << s.name;
    AlSigmaQFamilyRateBumps half;
    ASSERT_TRUE(sq::QFamilyAccess::prepare_rate_bumps(b->f, half, 0.5 * h)) << s.name;
    // The reference families at r ± h and r ± h/2 (same box, nodes, axis, ISA route).
    std::vector<std::unique_ptr<Built>> fam;
    for (const double dr : {h, -h, 0.5 * h, -0.5 * h}) {
      Spec t = s;
      t.r = s.r + dr;
      fam.push_back(build(t));
      ASSERT_NE(fam.back(), nullptr);
    }
    for (const double sigma : s.sigmas) {
      for (const double q : q_probes(s)) {
        for (const double K : s.strikes) {
          const std::string where = describe(s, K, sigma, q);
          const AlSigmaQFamilyGreeks g =
              family_greeks(b->f, bumps, s.S, K, sigma, q, request(AlFamilyGreekMask::all()));
          ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok) << where;
          bool ok = true;
          double P[4] = {};
          bool any_ex = g.exercised;
          for (std::size_t i = 0; i < 4; ++i) {
            P[i] = map_price(fam[i]->f, s.S, K, sigma, q, ok);
            any_ex = any_ex || family_greeks(fam[i]->f, s.S, K, sigma, q).exercised;
          }
          ASSERT_TRUE(ok) << where;
          if (any_ex) {
            EXPECT_EQ(g.reason_of(Greek::Rho), g.exercised ? Reason::Exercised : Reason::Served);
            continue;
          }
          ASSERT_EQ(g.reason_of(Greek::Rho), Reason::Served) << where;
          const AlSigmaQFamilyGreeks gh =
              family_greeks(b->f, half, s.S, K, sigma, q, request(AlFamilyGreekMask::all()));
          ASSERT_EQ(gh.reason_of(Greek::Rho), Reason::Served) << where;
          const double Dh = (P[0] - P[1]) / (2.0 * h);
          const double Dh2 = (P[2] - P[3]) / h;
          const double R = (4.0 * Dh2 - Dh) / 3.0;
          const double floor = 1.0e-3 * std::max(g.price, 1.0);
          const double den = std::max(std::abs(R), floor);
          const double e_h = std::abs(g.rho - R) / den;
          const double e_half = std::abs(gh.rho - R) / den;
          const int sb = in_band(b->f, s.side, s.S, K, sigma, q, s.T) ? 1 : 0;
          if (e_h > worst[sb]) {
            worst[sb] = e_h;
            at[sb] = where;
          }
          worst_half[sb] = std::max(worst_half[sb], e_half);
          worst_dh[sb] =
              std::max(worst_dh[sb], std::abs(g.rho - Dh) / std::max(std::abs(Dh), floor));
          if (e_h > 1.0e-7) { // resolved: above the references' own rounding
            ratio.push_back(e_h / std::max(e_half, 1.0e-300));
          }
          ++rows[sb];
        }
      }
    }
  }
  std::sort(ratio.begin(), ratio.end());
  const double med = ratio.empty() ? kNaN : ratio[ratio.size() / 2];
  for (int sb = 0; sb < 2; ++sb) {
    std::cout << "[AlSigmaQFamilyGreeks] rho " << (sb == 0 ? "OUTSIDE" : "INSIDE")
              << " the band: rows=" << rows[sb] << " worst |A(h) - R|/|R|=" << worst[sb]
              << " |A(h/2) - R|/|R|=" << worst_half[sb] << " |A(h) - D(h)|/|D(h)|="
              << worst_dh[sb] << "\n  at " << at[sb] << "\n";
  }
  std::cout << "[AlSigmaQFamilyGreeks] rho discriminator: rows with a resolved gap="
            << ratio.size() << " median |A(h) - R| / |A(h/2) - R|=" << med << " (4 = O(h^2))\n";
  // Measured (A0 r6, 2-figure round-ups): outside the band |A(h) − R| / |R| 2.27679e-6
  // (h/2: 5.69e-7), inside 7.63244e-5 (h/2: 1.98e-5); median ratio 3.99999 over 136
  // resolved rows — the residual is the bump's own O(h²) truncation, nothing else.
  EXPECT_GT(rows[0] + rows[1], 300u);
  EXPECT_LE(worst[0], 2.3e-6) << at[0];
  EXPECT_LE(worst[1], 7.7e-5) << at[1];
  EXPECT_GE(ratio.size(), 100u);
  EXPECT_GT(med, 3.5);
  EXPECT_LT(med, 4.5);
}

// ── 4. θ is the PDE identity of the served fields ──────────────────────────────────

TEST(AlSigmaQFamilyGreeks, ThetaIsThePdeIdentityOfTheServedPriceDeltaGamma) {
  std::size_t near_edge = 0;
  std::size_t identity = 0;
  for (const Spec &s : specs()) {
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    for (const double sigma : s.sigmas) {
      for (const double q : q_probes(s)) {
        for (const double K : s.strikes) {
          const AlSigmaQFamilyGreeks g = family_greeks(b->f, s.S, K, sigma, q);
          ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok);
          if (g.exercised) {
            EXPECT_EQ(g.theta, 0.0);
            EXPECT_EQ(g.reason_of(Greek::Theta), Reason::Exercised);
            continue;
          }
          // The near-edge band (review-A0 MJ1): θ and Γ are NaN with NearExerciseEdge.
          EXPECT_EQ(g.near_edge, g.edge_sd < atx::vol::kAlFamilyGreekEdgeBand)
              << describe(s, K, sigma, q);
          if (g.near_edge) {
            EXPECT_TRUE(std::isnan(g.theta) && std::isnan(g.gamma)) << describe(s, K, sigma, q);
            EXPECT_EQ(g.reason_of(Greek::Theta), Reason::NearExerciseEdge);
            EXPECT_EQ(g.reason_of(Greek::Gamma), Reason::NearExerciseEdge);
            ++near_edge;
            continue;
          }
          const double want = s.r * g.price - (s.r - q) * s.S * g.delta -
                              0.5 * sigma * sigma * s.S * s.S * g.gamma;
          EXPECT_NEAR(g.theta, want, 1.0e-13 * std::max(std::abs(want), 1.0))
              << describe(s, K, sigma, q);
          EXPECT_EQ(g.reason_of(Greek::Theta), Reason::Served);
          ++identity;
        }
      }
    }
  }
  std::cout << "[AlSigmaQFamilyGreeks] theta: identity rows=" << identity
            << " near-edge rows=" << near_edge << "\n";
  EXPECT_GT(identity, 300u); // of the 476 continuation rows (the rest are in the band)
  EXPECT_GT(near_edge, 0u);
}

// ── 5. Masks, reasons, convention ──────────────────────────────────────────────────

void expect_reason(const AlSigmaQFamilyGreeks &g, Greek k, Reason want, const char *what) {
  const double v = (k == Greek::Delta)   ? g.delta
                   : (k == Greek::Gamma) ? g.gamma
                   : (k == Greek::Vega)  ? g.vega
                   : (k == Greek::Theta) ? g.theta
                   : (k == Greek::Rho)   ? g.rho
                                         : g.phi;
  EXPECT_EQ(g.reason_of(k), want) << what << " greek " << static_cast<int>(k);
  const bool usable = want == Reason::Served || want == Reason::Exercised;
  EXPECT_EQ(g.supported.has(k), usable) << what << " greek " << static_cast<int>(k);
  if (want == Reason::NotRequested) {
    EXPECT_EQ(v, 0.0) << what;
  } else if (usable) {
    EXPECT_TRUE(std::isfinite(v)) << what;
  } else {
    EXPECT_TRUE(std::isnan(v)) << what << " greek " << static_cast<int>(k);
  }
}

// ── 5a. The near-edge band (review-A0 MJ1) ─────────────────────────────────────────

TEST(AlSigmaQFamilyGreeks, ThetaAndGammaNextToTheExerciseEdgeAreNearExerciseEdge) {
  // The slow suite's worst row (band-spy23-put: K 1.05 S, σ 0.12, 0.0071 σ√T from the
  // edge — the spy23-mid-put family): the PDE-identity θ there was +0.128 per year against
  // a converged calendar difference of −0.337 (an American θ = −∂V/∂T is <= 0), served
  // with reason Served. The row must say it is in the band; θ and Γ are NaN with
  // NearExerciseEdge; Δ, vega, rho and phi stay served.
  const std::unique_ptr<Built> b = build(specs()[0]);
  ASSERT_NE(b, nullptr);
  AlSigmaQFamilyRateBumps bumps;
  ASSERT_TRUE(bumps.prepare(b->f));
  const double q = kSpyQlo + 0.37 * (kSpyQhi - kSpyQlo);
  const double K = strikes_around(kSpyS, 1.02, 1.12, 11)[3];
  const double sigma = 0.12;
  const double sd = sigma * std::sqrt(kSpyT);
  const AlSigmaQGreeksRequest req = request(AlFamilyGreekMask::all());
  const AlSigmaQFamilyGreeks g = family_greeks(b->f, bumps, kSpyS, K, sigma, q, req);
  ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok);
  ASSERT_FALSE(g.exercised);
  // The converged model's calendar θ on this row is negative (central, h = 2e-3 T).
  const atx::vol::AlOpts conv{.n_collocation = 32, .n_quadrature = 64, .max_newton_iter = 16,
                              .tol = 1.0e-13};
  const double hT = 2.0e-3 * kSpyT;
  const Result<double> up = andersen_lake(kSpyS, K, kSpyT + hT, sigma, kSpyR, q, Side::Put, conv);
  const Result<double> dn = andersen_lake(kSpyS, K, kSpyT - hT, sigma, kSpyR, q, Side::Put, conv);
  ASSERT_TRUE(up && dn);
  const double theta_conv = -(*up - *dn) / (2.0 * hT);
  // The bundle's edge distance against an independent edge (bisection on the exercise
  // flag in option spot).
  const double edge = edge_spot(b->f, Side::Put, kSpyS, K, sigma, q);
  ASSERT_TRUE(std::isfinite(edge));
  const double edge_sd = std::log(kSpyS / edge) / sd;
  std::cout << "[AlSigmaQFamilyGreeks] near-edge row K=" << K << " sigma=" << sigma
            << ": edge_sd=" << g.edge_sd << " (bisection " << edge_sd << ") theta=" << g.theta
            << " (" << static_cast<int>(g.reason_of(Greek::Theta)) << ") gamma=" << g.gamma
            << " converged calendar theta=" << theta_conv
            << " gap to the flag's edge=" << g.edge_sd - edge_sd << "\n";
  EXPECT_LT(theta_conv, 0.0);
  EXPECT_FALSE(g.supported.has(Greek::Theta) && g.theta > 0.0) << "an American θ is <= 0";
  // The bundle's edge is the boundary's own (B'_now); the exercise FLAG flips a little on
  // the continuation side of it (the floored zone), so the independent bisection on the
  // flag sits at or inside the bundle's distance, by at most the floored zone.
  EXPECT_GE(g.edge_sd, edge_sd - 1.0e-9);
  EXPECT_LE(g.edge_sd - edge_sd, kFlooredZoneSd);
  EXPECT_GT(g.edge_sd, 0.0);
  EXPECT_LT(g.edge_sd, 0.02);
  EXPECT_TRUE(g.near_edge);
  expect_reason(g, Greek::Theta, Reason::NearExerciseEdge, "near edge");
  expect_reason(g, Greek::Gamma, Reason::NearExerciseEdge, "near edge");
  for (const Greek k : {Greek::Delta, Greek::Vega, Greek::Rho, Greek::Phi}) {
    expect_reason(g, k, Reason::Served, "near edge");
  }
  // The price and the served Greeks do not depend on the band.
  const AlSigmaQFamilyGreeks d = family_greeks(b->f, kSpyS, K, sigma, q,
                                               request(AlFamilyGreekMask::none()));
  EXPECT_TRUE(same_bits(d.price, g.price));
  EXPECT_TRUE(same_bits(d.edge_sd, g.edge_sd));
  EXPECT_TRUE(d.near_edge);
  // A strike walk across the band's border on the same family: in the band iff
  // 0 < edge_sd < 0.25 on a continuation row; both sides are reached; outside it θ and Γ
  // are served and θ <= 0 here.
  std::size_t inside = 0;
  std::size_t outside = 0;
  double floored_sd = 0.0;
  for (const double k : strikes_around(kSpyS, 0.95, 1.06, 45)) {
    const AlSigmaQFamilyGreeks w = family_greeks(b->f, kSpyS, k, sigma, q);
    ASSERT_EQ(w.status, AlSigmaQFamilyStatus::Ok);
    if (w.exercised) {
      EXPECT_FALSE(w.near_edge) << k;
      // On the exercise side of B'_now (edge_sd <= 0) or in the floored zone next to it.
      EXPECT_EQ(w.floored, w.edge_sd > 0.0) << k;
      EXPECT_LE(w.edge_sd, kFlooredZoneSd) << k;
      floored_sd = std::max(floored_sd, w.edge_sd);
      continue;
    }
    EXPECT_GT(w.edge_sd, 0.0) << k;
    EXPECT_EQ(w.near_edge, w.edge_sd < atx::vol::kAlFamilyGreekEdgeBand) << k;
    if (w.near_edge) {
      expect_reason(w, Greek::Theta, Reason::NearExerciseEdge, "walk, inside");
      expect_reason(w, Greek::Gamma, Reason::NearExerciseEdge, "walk, inside");
      ++inside;
    } else {
      expect_reason(w, Greek::Theta, Reason::Served, "walk, outside");
      expect_reason(w, Greek::Gamma, Reason::Served, "walk, outside");
      EXPECT_LE(w.theta, 0.0) << k;
      ++outside;
    }
  }
  std::cout << "[AlSigmaQFamilyGreeks] band walk: inside=" << inside << " outside=" << outside
            << " widest floored edge_sd=" << floored_sd << "\n";
  EXPECT_GT(inside, 0u);
  EXPECT_GT(outside, 0u);
}

TEST(AlSigmaQFamilyGreeks, ExercisedRowsServeTheExerciseGreeksWithTheMask) {
  // A deep-ITM put at low σ (the clamp serves K − S) and a deep-ITM call on a q > r box:
  // every requested Greek is the exercise one with reason Exercised — never Served with
  // 0.0 — and rho is served as 0.0 even without a rate-bump set (intrinsic does not move
  // with r).
  const std::unique_ptr<Built> put = build(specs()[0]);
  ASSERT_NE(put, nullptr);
  const double K = 1.3 * kSpyS;
  const AlSigmaQFamilyGreeks g =
      family_greeks(put->f, kSpyS, K, 0.12, kSpyQlo, request(AlFamilyGreekMask::all()));
  ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok);
  EXPECT_TRUE(g.exercised);
  EXPECT_EQ(g.price, K - kSpyS);
  EXPECT_EQ(g.delta, -1.0);
  EXPECT_EQ(g.supported.bits, AlFamilyGreekMask::all().bits);
  for (const Greek k : {Greek::Delta, Greek::Gamma, Greek::Vega, Greek::Theta, Greek::Rho,
                        Greek::Phi}) {
    expect_reason(g, k, Reason::Exercised, "deep ITM put");
  }
  EXPECT_EQ(g.gamma, 0.0);
  EXPECT_EQ(g.vega, 0.0);
  EXPECT_EQ(g.theta, 0.0);
  EXPECT_EQ(g.rho, 0.0);
  EXPECT_EQ(g.phi, 0.0);
  // A call that exercises early: q well above r.
  // (n_q 6: a 1 %-wide box 4.5 % from r passes the q-axis guard at n_q >= 5.)
  const Spec call{"hiq-call", 100.0, 1.0, 0.01, 0.05, 0.06, Side::Call, 0.10, 0.40, 12, 6,
                  {0.15}, {40.0}};
  const std::unique_ptr<Built> c = build(call);
  ASSERT_NE(c, nullptr);
  const AlSigmaQFamilyGreeks gc = family_greeks(c->f, 100.0, 40.0, 0.12, 0.055);
  ASSERT_EQ(gc.status, AlSigmaQFamilyStatus::Ok);
  EXPECT_TRUE(gc.exercised);
  EXPECT_EQ(gc.price, 60.0);
  EXPECT_EQ(gc.delta, 1.0);
  for (const Greek k : {Greek::Delta, Greek::Gamma, Greek::Vega, Greek::Theta, Greek::Phi}) {
    expect_reason(gc, k, Reason::Exercised, "deep ITM call");
  }
  expect_reason(gc, Greek::Rho, Reason::NotRequested, "deep ITM call (rho not requested)");
  // ATM: continuation, every primary Greek served.
  const AlSigmaQFamilyGreeks atm = family_greeks(put->f, kSpyS, kSpyS, 0.2, kSpyQlo);
  EXPECT_FALSE(atm.exercised);
  EXPECT_FALSE(atm.floored);
  EXPECT_EQ(atm.supported.bits, AlFamilyGreekMask::primary().bits);
}

TEST(AlSigmaQFamilyGreeks, EveryReasonIsTypedAndTheConventionIsPopulated) {
  const std::unique_ptr<Built> b = build(specs()[0]); // spy23-mid-put (12, 4)
  ASSERT_NE(b, nullptr);
  AlSigmaQGreeksRequest req;
  req.mask = AlFamilyGreekMask::all();
  req.surface_version = 0xA0A0;
  // Served (and NoCarryBumps: rho requested without a set).
  const AlSigmaQFamilyGreeks g = family_greeks(b->f, kSpyS, kSpyS, 0.2, kSpyQlo, req);
  ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok);
  for (const Greek k : {Greek::Delta, Greek::Gamma, Greek::Vega, Greek::Theta, Greek::Phi}) {
    expect_reason(g, k, Reason::Served, "served");
  }
  expect_reason(g, Greek::Rho, Reason::NoCarryBumps, "no set");
  EXPECT_EQ(g.requested.bits, AlFamilyGreekMask::all().bits);
  EXPECT_EQ(g.convention.held_fixed, AlFamilyGreekHeldFixed::ContractSigma);
  EXPECT_EQ(g.convention.model, AlFamilyGreekModel::SigmaQFamily);
  EXPECT_EQ(g.convention.clock, AlFamilyGreekClock::GeometryTime);
  EXPECT_EQ(g.convention.surface_version, 0xA0A0u);
  // NotRequested: the mask decides what is computed; requested fields are mask-independent.
  const AlSigmaQFamilyGreeks d =
      family_greeks(b->f, kSpyS, kSpyS, 0.2, kSpyQlo,
                    request(AlFamilyGreekMask::none().with(Greek::Delta)));
  expect_reason(d, Greek::Delta, Reason::Served, "delta only");
  for (const Greek k : {Greek::Gamma, Greek::Vega, Greek::Theta, Greek::Rho, Greek::Phi}) {
    expect_reason(d, k, Reason::NotRequested, "delta only");
  }
  EXPECT_TRUE(same_bits(d.delta, g.delta));
  EXPECT_TRUE(same_bits(d.price, g.price));
  // Refused: σ outside the box — status, NaN price, every requested Greek NaN with Refused,
  // the convention still populated.
  const AlSigmaQFamilyGreeks r = family_greeks(b->f, kSpyS, kSpyS, 0.6, kSpyQlo, req);
  EXPECT_EQ(r.status, AlSigmaQFamilyStatus::SigmaOutsideBox);
  EXPECT_TRUE(std::isnan(r.price));
  for (const Greek k : {Greek::Delta, Greek::Gamma, Greek::Vega, Greek::Theta, Greek::Rho,
                        Greek::Phi}) {
    expect_reason(r, k, Reason::Refused, "refused");
  }
  EXPECT_EQ(r.convention.surface_version, 0xA0A0u);
  EXPECT_EQ(family_greeks(b->f, kSpyS, kSpyS, 0.2, 0.5, req).status,
            AlSigmaQFamilyStatus::YieldOutsideBox);
  EXPECT_EQ(family_greeks(b->f, kSpyS, -1.0, 0.2, kSpyQlo, req).status,
            AlSigmaQFamilyStatus::InvalidArgument);
  AlSigmaQFamily moved(std::move(b->f));
  EXPECT_EQ(family_greeks(b->f, kSpyS, kSpyS, 0.2, kSpyQlo, req).status, // NOLINT: moved-from
            AlSigmaQFamilyStatus::NotPrepared);
  // CarryBumpRefused: an unprepared set, and a set prepared from another family.
  AlSigmaQFamilyRateBumps empty;
  expect_reason(family_greeks(moved, empty, kSpyS, kSpyS, 0.2, kSpyQlo, req), Greek::Rho,
                Reason::CarryBumpRefused, "unprepared set");
  Spec other_spec = specs()[0];
  other_spec.s_hi = 0.55;
  const std::unique_ptr<Built> other = build(other_spec);
  ASSERT_NE(other, nullptr);
  AlSigmaQFamilyRateBumps foreign;
  ASSERT_TRUE(foreign.prepare(other->f));
  const AlSigmaQFamilyGreeks fg = family_greeks(moved, foreign, kSpyS, kSpyS, 0.2, kSpyQlo, req);
  expect_reason(fg, Greek::Rho, Reason::CarryBumpRefused, "foreign set");
  EXPECT_TRUE(same_bits(fg.delta, g.delta)); // every other field is the plain overload's
  AlSigmaQFamilyRateBumps own;
  ASSERT_TRUE(own.prepare(moved));
  expect_reason(family_greeks(moved, own, kSpyS, kSpyS, 0.2, kSpyQlo, req), Greek::Rho,
                Reason::Served, "own set");
  // NoQAxis: a collapsed q box has no q coordinate.
  Spec flat = specs()[0];
  flat.q_lo = flat.q_hi = kSpyQlo;
  const std::unique_ptr<Built> fb = build(flat);
  ASSERT_NE(fb, nullptr);
  ASSERT_EQ(fb->f.n_q(), 1u);
  const AlSigmaQFamilyGreeks nq = family_greeks(fb->f, kSpyS, kSpyS, 0.2, kSpyQlo, req);
  expect_reason(nq, Greek::Phi, Reason::NoQAxis, "collapsed q box");
  expect_reason(nq, Greek::Vega, Reason::Served, "collapsed q box");
  // ConventionUnsupported: sticky moneyness and a refitted surface are not implemented;
  // the price is served, every requested Greek NaN; StickyStrike is ContractSigma's
  // numbers.
  for (const AlFamilyGreekHeldFixed h :
       {AlFamilyGreekHeldFixed::StickyMoneyness, AlFamilyGreekHeldFixed::RefittedSurface}) {
    AlSigmaQGreeksRequest u = req;
    u.held_fixed = h;
    const AlSigmaQFamilyGreeks cu = family_greeks(moved, own, kSpyS, kSpyS, 0.2, kSpyQlo, u);
    EXPECT_EQ(cu.status, AlSigmaQFamilyStatus::Ok);
    EXPECT_TRUE(same_bits(cu.price, g.price));
    EXPECT_EQ(cu.convention.held_fixed, h);
    EXPECT_EQ(cu.supported.bits, 0u);
    for (const Greek k : {Greek::Delta, Greek::Gamma, Greek::Vega, Greek::Theta, Greek::Rho,
                          Greek::Phi}) {
      expect_reason(cu, k, Reason::ConventionUnsupported, "unsupported convention");
    }
  }
  AlSigmaQGreeksRequest ss = req;
  ss.held_fixed = AlFamilyGreekHeldFixed::StickyStrike;
  const AlSigmaQFamilyGreeks gs = family_greeks(moved, own, kSpyS, kSpyS, 0.2, kSpyQlo, ss);
  EXPECT_EQ(gs.convention.held_fixed, AlFamilyGreekHeldFixed::StickyStrike);
  EXPECT_TRUE(same_bits(gs.delta, g.delta) && same_bits(gs.vega, g.vega) &&
              same_bits(gs.phi, g.phi));
}

TEST(AlSigmaQFamilyGreeks, ADegenerateTransformNodeTypesTheAffectedGreekNearNodeDegenerate) {
  // s = √max(y, 0) has no derivative where y crosses 0 with a nonzero slope. Forced at
  // the stage level (as K1 forces it for the fused vega): a σ-degenerate node makes vega
  // NaN with NearNodeDegenerate, a q-degenerate one phi, an r-degenerate one rho; Δ, Γ,
  // θ (fixed boundary) stay served.
  const std::unique_ptr<Built> b = build(specs()[0]);
  ASSERT_NE(b, nullptr);
  const auto *st = sq::QFamilyAccess::state(b->f);
  sq::PremiumNodes pn;
  ASSERT_TRUE(st->lay_premium_nodes(pn));
  sq::QCarry c;
  sq::carry_at(pn, Side::Put, kSpyR, kSpyQlo, c);
  sq::NodeBoundary nb;
  st->boundary_at_nodes(pn, 0.2, kSpyQlo, nb);
  ASSERT_FALSE(nb.degenerate);
  nb.degenerate = true; // what finish_node_boundary sets for such a node
  sq::NodeCarryMotion m;
  m.have_rate = m.have_yield = true;
  m.rate_degenerate = true;  // a put's rate is the option's r: rho
  m.yield_degenerate = true; // a put's yield is the option's q: phi
  sq::GreeksContext ctx;
  ctx.side = Side::Put;
  ctx.S = ctx.Sp = kSpyS;
  ctx.Kp = kSpyS;
  ctx.r = kSpyR;
  ctx.q = kSpyQlo;
  ctx.sigma = 0.2;
  ctx.q_axis = true;
  ctx.rho_source = Reason::Served;
  const AlSigmaQGreeksRequest req = request(AlFamilyGreekMask::all());
  const sq::GreeksNeed need = sq::greeks_need(req, ctx, true);
  sq::GreeksInternal in;
  sq::greeks_pass(pn, c, nb, m, kSpyS, kSpyS, false, need, in);
  ASSERT_FALSE(in.exercised);
  const AlSigmaQFamilyGreeks g = sq::map_greeks(in, m, ctx, req);
  expect_reason(g, Greek::Vega, Reason::NearNodeDegenerate, "sigma-degenerate");
  expect_reason(g, Greek::Phi, Reason::NearNodeDegenerate, "q-degenerate");
  expect_reason(g, Greek::Rho, Reason::NearNodeDegenerate, "r-degenerate");
  for (const Greek k : {Greek::Delta, Greek::Gamma, Greek::Theta}) {
    expect_reason(g, k, Reason::Served, "fixed boundary");
  }
  // A call maps the motions the other way round: its q is the internal rate.
  ctx.side = Side::Call;
  m.rate_degenerate = false;
  const AlSigmaQFamilyGreeks gc = sq::map_greeks(in, m, ctx, req);
  expect_reason(gc, Greek::Phi, Reason::Served, "call: rate motion clean");
  expect_reason(gc, Greek::Rho, Reason::NearNodeDegenerate, "call: yield motion degenerate");
}

// ── 6. The structure-of-arrays chain entry ─────────────────────────────────────────

TEST(AlSigmaQFamilyGreeks, ManyIsTheScalarEntryLaneForLaneWithRetiredAndRefusedLanes) {
  for (const std::size_t si : {std::size_t{0}, std::size_t{1}}) { // spy23 put, call
    const Spec s = specs()[si];
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    AlSigmaQFamilyRateBumps bumps;
    ASSERT_TRUE(bumps.prepare(b->f));
    constexpr std::size_t kN = 40;
    std::vector<double> K(kN), sig(kN), q(kN);
    std::vector<std::uint8_t> active(kN, 1);
    for (std::size_t i = 0; i < kN; ++i) {
      K[i] = s.S * (0.8 + 0.4 * static_cast<double>(i) / static_cast<double>(kN - 1));
      sig[i] = 0.11 + 0.37 * static_cast<double>((i * 7) % kN) / static_cast<double>(kN);
      q[i] = (i % 3 == 0) ? s.q_lo : s.q_lo + (s.q_hi - s.q_lo) * static_cast<double>(i % 5) / 4.0;
    }
    sig[5] = 0.7;  // σ outside the box: refused lane
    K[9] = -3.0;   // a bad strike: refused lane
    q[13] = 0.5;   // q outside the box: refused lane
    active[17] = 0;
    active[18] = 0;
    sig[18] = kNaN; // a retired lane's inputs are not read
    K[20] = 1.05 * s.S; // a put lane in the near-edge band (review-A0 MJ1)
    sig[20] = 0.12;
    for (const bool with_bumps : {false, true}) {
      AlSigmaQGreeksRequest req = request(AlFamilyGreekMask::all());
      req.surface_version = 7;
      std::vector<double> price(kN, 9.0), delta(kN, 9.0), gamma(kN, 9.0), vega(kN, 9.0),
          theta(kN, 9.0), rho(kN, 9.0), phi(kN, 9.0);
      std::vector<AlSigmaQFamilyStatus> status(kN, AlSigmaQFamilyStatus::NotPrepared);
      std::vector<AlFamilyGreekMask> supported(kN, AlFamilyGreekMask{0xFF});
      std::vector<AlFamilyGreekReasons> reason(kN);
      std::vector<std::uint8_t> flags(kN, 0xFF);
      std::vector<double> edge(kN, 9.0);
      AlSigmaQGreekColumns out;
      out.price = price;
      out.delta = delta;
      out.gamma = gamma;
      out.vega = vega;
      out.theta = theta;
      out.rho = rho;
      out.phi = phi;
      out.status = status;
      out.supported = supported;
      out.reason = reason;
      out.flags = flags;
      out.edge_sd = edge;
      const Status st = family_greeks_many(b->f, s.S, K, sig, q, active, out, req,
                                           with_bumps ? &bumps : nullptr);
      ASSERT_TRUE(st);
      for (std::size_t i = 0; i < kN; ++i) {
        if (active[i] == 0) {
          EXPECT_EQ(price[i], 9.0);
          EXPECT_EQ(flags[i], 0xFFu);
          EXPECT_EQ(edge[i], 9.0);
          EXPECT_EQ(supported[i].bits, 0xFFu);
          continue;
        }
        const AlSigmaQFamilyGreeks g =
            with_bumps ? family_greeks(b->f, bumps, s.S, K[i], sig[i], q[i], req)
                       : family_greeks(b->f, s.S, K[i], sig[i], q[i], req);
        EXPECT_EQ(status[i], g.status) << i;
        EXPECT_TRUE(same_bits(price[i], g.price)) << i;
        EXPECT_TRUE(same_bits(delta[i], g.delta)) << i;
        EXPECT_TRUE(same_bits(gamma[i], g.gamma)) << i;
        EXPECT_TRUE(same_bits(vega[i], g.vega)) << i;
        EXPECT_TRUE(same_bits(theta[i], g.theta)) << i;
        EXPECT_TRUE(same_bits(rho[i], g.rho)) << i;
        EXPECT_TRUE(same_bits(phi[i], g.phi)) << i;
        EXPECT_EQ(supported[i].bits, g.supported.bits) << i;
        EXPECT_EQ(reason[i], g.reason) << i;
        EXPECT_EQ(flags[i], static_cast<std::uint8_t>((g.exercised ? 1u : 0u) |
                                                      (g.floored ? 2u : 0u) |
                                                      (g.near_edge ? 4u : 0u)))
            << i;
        EXPECT_TRUE(same_bits(edge[i], g.edge_sd)) << i;
      }
      if (s.side == Side::Put) {
        EXPECT_EQ(flags[20], 4u); // in the band, not exercised
        EXPECT_EQ(reason[20][static_cast<std::size_t>(Greek::Theta)], Reason::NearExerciseEdge);
      }
      EXPECT_EQ(status[5], AlSigmaQFamilyStatus::SigmaOutsideBox);
      EXPECT_EQ(status[9], AlSigmaQFamilyStatus::InvalidArgument);
      EXPECT_EQ(status[13], AlSigmaQFamilyStatus::YieldOutsideBox);
      EXPECT_EQ(reason[0][static_cast<std::size_t>(Greek::Rho)],
                with_bumps ? Reason::Served : Reason::NoCarryBumps);
    }
    // Lengths that disagree: InvalidArgument, nothing written.
    std::vector<double> p2(kN, 5.0);
    std::vector<double> short_col(kN - 1, 5.0);
    AlSigmaQGreekColumns bad;
    bad.price = p2;
    bad.delta = short_col;
    EXPECT_FALSE(family_greeks_many(b->f, s.S, K, sig, q, {}, bad));
    EXPECT_EQ(p2[0], 5.0);
    std::vector<std::uint8_t> short_active(kN - 1, 1);
    AlSigmaQGreekColumns ok_cols;
    ok_cols.price = p2;
    EXPECT_FALSE(family_greeks_many(b->f, s.S, K, sig, q, short_active, ok_cols));
    EXPECT_FALSE(family_greeks_many(b->f, s.S, K, std::span<const double>(sig).first(kN - 1), q,
                                    {}, ok_cols));
    EXPECT_EQ(p2[0], 5.0);
    // Empty `active` = every lane; only the price column stored.
    ASSERT_TRUE(family_greeks_many(b->f, s.S, K, sig, q, {}, ok_cols));
    EXPECT_TRUE(same_bits(p2[18], family_greeks(b->f, s.S, K[18], sig[18], q[18]).price));
  }
}

// ── 7. The rate-bump set ───────────────────────────────────────────────────────────

TEST(AlSigmaQFamilyGreeks, RateBumpsRefuseAnUnusableBaseAndReportTheirBytes) {
  AlSigmaQFamilyRateBumps bumps;
  EXPECT_FALSE(bumps.prepared());
  EXPECT_EQ(bumps.state_bytes(), 0u);
  AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaQFamily f(g, Side::Put, 100.0);
  const Status unprepared = bumps.prepare(f);
  EXPECT_FALSE(unprepared);
  EXPECT_EQ(unprepared.error().code(), atx::core::ErrorCode::InvalidArgument);
  ASSERT_TRUE(f.prepare(0.1, 0.5, 12, 0.019, 0.021, 4));
  ASSERT_TRUE(bumps.prepare(f));
  EXPECT_TRUE(bumps.prepared());
  const std::size_t tensor = static_cast<std::size_t>(f.state_bytes()); // for the print
  std::cout << "[AlSigmaQFamilyGreeks] rate-bump set bytes=" << bumps.state_bytes()
            << " (family " << tensor << ")\n";
  EXPECT_GT(bumps.state_bytes(), 12u * 4u * sizeof(double));
  // The geometry re-prepared: the base no longer admits; a new prepare refuses and leaves
  // the set unprepared (strong guarantee).
  g.prepare(0.75, 0.05, 0.02);
  EXPECT_FALSE(bumps.prepare(f));
  EXPECT_FALSE(bumps.prepared());
  // A put box whose down bump leaves nothing to refuse still builds; a moved set moves.
  ASSERT_TRUE(f.prepare(0.1, 0.5, 12, 0.019, 0.021, 4));
  ASSERT_TRUE(bumps.prepare(f));
  AlSigmaQFamilyRateBumps moved(std::move(bumps));
  EXPECT_TRUE(moved.prepared());
  EXPECT_FALSE(bumps.prepared()); // NOLINT: moved-from
}

// ── 8. AVX2- vs scalar-built families ──────────────────────────────────────────────

TEST(AlSigmaQFamilyGreeks, Avx2AndScalarBuiltFamiliesAgreeToTheMeasuredBound) {
  // The bundle's evaluator is one scalar code path; the AVX2 route changes only the
  // family BUILD (the node boundaries, 4 lanes at a time, rounding-level apart: the
  // AvxBoundary parity) and, through it, the rate-bump set. So the two bundles differ by
  // the Greeks' sensitivity to a rounding-level change of the node tensor, measured here
  // per Greek as max |a − s| / max(|s|, floor).
  if (!atx::vol::simd::have_avx2()) {
    GTEST_SKIP() << "host has no AVX2 (the AVX2 family build is unreachable)";
  }
  std::array<double, kAlFamilyGreekCount + 1> worst{}; // + price
  std::size_t rows = 0;
  for (const Spec &s : specs()) {
    std::unique_ptr<Built> fs;
    std::unique_ptr<Built> fa;
    AlSigmaQFamilyRateBumps bs;
    AlSigmaQFamilyRateBumps ba;
    {
      const IsaScope scope(SimdIsa::ForceScalar);
      fs = build(s);
      ASSERT_NE(fs, nullptr);
      ASSERT_TRUE(bs.prepare(fs->f));
    }
    {
      const IsaScope scope(SimdIsa::ForceAvx2);
      fa = build(s);
      ASSERT_NE(fa, nullptr);
      ASSERT_TRUE(ba.prepare(fa->f));
    }
    for (const double sigma : s.sigmas) {
      for (const double q : q_probes(s)) {
        for (const double K : s.strikes) {
          const AlSigmaQGreeksRequest req = request(AlFamilyGreekMask::all());
          const AlSigmaQFamilyGreeks gs = family_greeks(fs->f, bs, s.S, K, sigma, q, req);
          const AlSigmaQFamilyGreeks ga = family_greeks(fa->f, ba, s.S, K, sigma, q, req);
          ASSERT_EQ(gs.status, AlSigmaQFamilyStatus::Ok);
          ASSERT_EQ(ga.status, AlSigmaQFamilyStatus::Ok);
          if (gs.exercised != ga.exercised || gs.near_edge != ga.near_edge ||
              gs.reason != ga.reason) {
            ADD_FAILURE() << "exercise / band / reason differs " << describe(s, K, sigma, q);
            continue;
          }
          const double floor = 1.0e-3 * std::max(gs.price, 1.0);
          const double a[] = {ga.delta, ga.gamma * s.S, ga.vega,  ga.theta,
                              ga.rho,   ga.phi,         ga.price};
          const double b[] = {gs.delta, gs.gamma * s.S, gs.vega,  gs.theta,
                              gs.rho,   gs.phi,         gs.price};
          for (std::size_t k = 0; k < worst.size(); ++k) {
            if (k < kAlFamilyGreekCount && !gs.supported.has(static_cast<Greek>(k))) {
              continue; // a NaN by contract (θ / Γ in the near-edge band): nothing to compare
            }
            const double fl = (k == 0) ? 1.0e-3 : floor;
            worst[k] = std::max(worst[k], std::abs(a[k] - b[k]) / std::max(std::abs(b[k]), fl));
          }
          ++rows;
        }
      }
    }
  }
  std::cout << "[AlSigmaQFamilyGreeks] avx2 vs scalar build (rows=" << rows
            << "): delta=" << worst[0] << " gamma=" << worst[1] << " vega=" << worst[2]
            << " theta=" << worst[3] << " rho=" << worst[4] << " phi=" << worst[5]
            << " price=" << worst[6] << "\n";
  // Pinned at the measured worst (A0 r6, 2-figure round-ups; order Δ, S·Γ, vega, θ, rho,
  // phi, price): the AVX2 node solve reorders the sweep arithmetic (4 lanes, FMA), so the
  // node tensor moves by its rounding (the AvxBoundary parity, <= ~4e-13 of y) and each
  // Greek by its sensitivity to y: Δ and the price ~1e-14, the σ- and carry-motion
  // Greeks 1e-11..1e-8 (phi's dy/dq differentiates the q interpolant of that rounding
  // across a box 1e-4..1e-2 wide), θ the PDE identity's cancellation of them. Re-pinned
  // in A0 fix round 1 over the SERVED rows only: S·Γ was 2.4e-12 and θ 8.0e-10 on rows
  // of the near-edge band, which now serve neither (NearExerciseEdge).
  constexpr std::array<double, kAlFamilyGreekCount + 1> kPin = {1.4e-14, 4.9e-14, 2.4e-11,
                                                                3.7e-13, 7.0e-10, 7.1e-9,
                                                                4.3e-15};
  EXPECT_GT(rows, 500u);
  for (std::size_t k = 0; k < worst.size(); ++k) {
    EXPECT_LE(worst[k], kPin[k]) << "greek " << k;
  }
}

// ── 9. Overlap with the L3g σ-family bundle ────────────────────────────────────────

TEST(AlSigmaQFamilyGreeks, ACollapsedQBoxAgreesWithTheSigmaFamilyBundle) {
  // A degenerate q box is AlSigmaFamily at that q BIT FOR BIT on the scalar build; the two
  // bundles then evaluate the same boundary through different arithmetic (the fused log
  // form vs L3g's exp form), so Δ, Γ, vega, θ agree to rounding, and the two rate-bump
  // routes (L3g's carry set, this rate-bump set) to theirs.
  const IsaScope scope(SimdIsa::ForceScalar);
  std::array<double, 5> worst{};
  std::size_t rows = 0;
  for (const Side side : {Side::Put, Side::Call}) {
    const double q = 0.5 * (kSpyQlo + kSpyQhi);
    const AlExpiryGeometry g = prepare_al_geometry(kSpyT, kSpyR, q);
    AlSigmaQFamily fq(g, side, kSpyS);
    ASSERT_TRUE(fq.prepare(0.10, 0.50, 12, q, q, 1));
    AlSigmaFamily fs(g, side, kSpyS);
    ASSERT_TRUE(fs.prepare(0.10, 0.50, 12));
    AlSigmaQFamilyRateBumps bq;
    ASSERT_TRUE(bq.prepare(fq));
    AlSigmaFamilyCarryBumps cs;
    ASSERT_TRUE(cs.prepare(fs, AlSigmaFamilyGreeksMask{AlSigmaFamilyGreeksMask::kRho}));
    for (const double sigma : {0.12, 0.2, 0.3, 0.45}) {
      for (const double K : strikes_around(kSpyS, 0.75, 1.25, 21)) {
        const AlSigmaQFamilyGreeks a =
            family_greeks(fq, bq, kSpyS, K, sigma, q, request(AlFamilyGreekMask::all()));
        const AlSigmaFamilyGreeks b =
            family_greeks(fs, cs, kSpyS, K, sigma,
                          AlSigmaFamilyGreeksMask{static_cast<std::uint16_t>(
                              AlSigmaFamilyGreeksMask::hedge().bits |
                              AlSigmaFamilyGreeksMask::kRho)});
        ASSERT_EQ(a.status, AlSigmaQFamilyStatus::Ok);
        ASSERT_EQ(b.status, AlSigmaFamilyStatus::Ok);
        ASSERT_EQ(b.carry, AlSigmaFamilyCarryStatus::Ok);
        if (a.exercised || b.exercised) {
          EXPECT_EQ(a.exercised, b.exercised) << K << " " << sigma;
          continue;
        }
        const double floor = 1.0e-3 * std::max(b.price, 1.0);
        const double av[] = {a.delta, a.gamma * kSpyS, a.vega, a.theta, a.rho};
        const double bv[] = {b.delta, b.gamma * kSpyS, b.vega, b.theta, b.rho};
        for (std::size_t k = 0; k < worst.size(); ++k) {
          if (!a.supported.has(static_cast<Greek>(k))) {
            // θ / Γ in the near-edge band: NaN here, still served by the L3g bundle (which
            // has no per-Greek reason — review-A0 m2).
            EXPECT_TRUE(a.near_edge) << K << " " << sigma << " greek " << k;
            continue;
          }
          const double fl = (k == 0) ? 1.0e-3 : floor;
          worst[k] = std::max(worst[k], std::abs(av[k] - bv[k]) / std::max(std::abs(bv[k]), fl));
        }
        EXPECT_NEAR(a.price, b.price, 1.0e-12 * std::max(b.price, 1.0));
        ++rows;
      }
    }
  }
  std::cout << "[AlSigmaQFamilyGreeks] collapsed q vs sigma family (rows=" << rows
            << "): delta=" << worst[0] << " gamma=" << worst[1] << " vega=" << worst[2]
            << " theta=" << worst[3] << " rho=" << worst[4] << "\n";
  // Pinned at the measured worst (A0 r6, 2-figure round-ups; order Δ, S·Γ, vega, θ, rho):
  // the same boundary through two arithmetics (log form vs exp form) and the same bumped
  // series through two contractions agree to rounding. Re-pinned in A0 fix round 1 over
  // the rows the (σ, q) bundle serves Γ and θ on: 7.8e-14 / 2.3e-11 were band rows.
  constexpr std::array<double, 5> kPin = {4.5e-16, 3.6e-16, 2.6e-13, 7.3e-16, 3.6e-13};
  EXPECT_GT(rows, 100u);
  for (std::size_t k = 0; k < worst.size(); ++k) {
    EXPECT_LE(worst[k], kPin[k]) << "greek " << k;
  }
}

// ── 10. Threads ────────────────────────────────────────────────────────────────────

TEST(AlSigmaQFamilyGreeks, ConcurrentBundlesAreBitIdenticalToTheSerialLoop) {
  const std::unique_ptr<Built> b = build(specs()[1]); // spy23-mid-call
  ASSERT_NE(b, nullptr);
  AlSigmaQFamilyRateBumps bumps;
  ASSERT_TRUE(bumps.prepare(b->f));
  const std::vector<double> K = strikes_around(kSpyS, 0.7, 1.2, 64);
  const AlSigmaQGreeksRequest req = request(AlFamilyGreekMask::all());
  std::vector<AlSigmaQFamilyGreeks> serial;
  for (const double k : K) {
    serial.push_back(family_greeks(b->f, bumps, kSpyS, k, 0.21, kSpyQlo, req));
  }
  constexpr unsigned kThreads = 4;
  std::vector<std::vector<AlSigmaQFamilyGreeks>> got(kThreads);
  {
    std::vector<std::jthread> pool;
    for (unsigned t = 0; t < kThreads; ++t) {
      pool.emplace_back([&, t] {
        for (const double k : K) {
          got[t].push_back(family_greeks(b->f, bumps, kSpyS, k, 0.21, kSpyQlo, req));
        }
      });
    }
  }
  for (unsigned t = 0; t < kThreads; ++t) {
    ASSERT_EQ(got[t].size(), serial.size());
    for (std::size_t i = 0; i < serial.size(); ++i) {
      EXPECT_TRUE(same_bits(got[t][i].price, serial[i].price) &&
                  same_bits(got[t][i].delta, serial[i].delta) &&
                  same_bits(got[t][i].gamma, serial[i].gamma) &&
                  same_bits(got[t][i].vega, serial[i].vega) &&
                  same_bits(got[t][i].theta, serial[i].theta) &&
                  same_bits(got[t][i].rho, serial[i].rho) &&
                  same_bits(got[t][i].phi, serial[i].phi))
          << t << " " << i;
    }
  }
}

// ── 11. SLOW: the converged model (AlSigmaQFamilyGreeksModel, ATX_VOL_SLOW_FILTER) ────
//
// Each served Greek against step-refined differences of the CONVERGED Andersen-Lake
// price (andersen_lake at 32 collocation / 64 quadrature nodes, 16 sweeps, tol 1e-13 —
// K1's model reference): Δ, Γ in S, vega in σ, θ = −∂P/∂T (the CALENDAR difference,
// independent of the PDE identity: review-L3g m5), rho in r, phi in q, each a 4-level
// Richardson ladder of 4th-order stencils. The reference's own uncertainty is the same
// ladder on one rung below (24 / 48): |D_conv − D_rung| is printed beside every error.
// Rows are split by their distance to the family's current exercise edge: the
// NEAR-BOUNDARY BAND is |ln(S / S_edge)| < 0.25 σ√T — the bundle's own edge_sd /
// near_edge (review-A0 MJ1), checked here against an independent bisection edge. There
// the bundle serves NO θ and NO Γ (NearExerciseEdge): the PDE-identity θ cancels O(rK)
// terms next to the edge and was measured up to 138 % off with the wrong sign — the
// L3g σ family's θ (the same identity, still served by that bundle) carries that
// measurement on. An S stencil that would cross the edge is taken one-sided away from
// it, and Γ is not differenced there.
// The family's error against the converged model is the family's interpolation error
// PLUS its build scheme's distance from convergence (the family is built at the
// geometry's Reference scheme) — a measured TABLE (F11), guarded against gross breakage
// only. ATX_VOL_A0_GREEKS_CSV=<path> writes every row.

const AlOpts kConverged{.n_collocation = 32, .n_quadrature = 64, .max_newton_iter = 16,
                        .tol = 1.0e-13};
const AlOpts kRungBelow{.n_collocation = 24, .n_quadrature = 48, .max_newton_iter = 16,
                        .tol = 1.0e-13};

struct ModelCase {
  Spec spec;
  double q;           // the contract yield probed (inside the box)
  bool band_set;      // the set was laid out to probe the near-boundary band
};

[[nodiscard]] std::vector<ModelCase> model_cases() {
  std::vector<ModelCase> v;
  const double qm = kSpyQlo + 0.37 * (kSpyQhi - kSpyQlo);
  // SPY-23-like, both sides.
  v.push_back({Spec{"spy23-put", kSpyS, kSpyT, kSpyR, kSpyQlo, kSpyQhi, Side::Put, 0.10, 0.50, 12,
                    4, {0.15, 0.3}, strikes_around(kSpyS, 0.88, 1.08, 9)},
               qm, false});
  v.push_back({Spec{"spy23-call", kSpyS, kSpyT, kSpyR, kSpyQlo, kSpyQhi, Side::Call, 0.10, 0.50,
                    12, 4, {0.15, 0.3}, strikes_around(kSpyS, 0.92, 1.12, 9)},
               qm, false});
  // High-vol / sparse: a 55-day name at 70-120 % vol on a wide q box.
  v.push_back({Spec{"hivol-put", 50.0, 0.15, 0.045, 0.0, 0.01, Side::Put, 0.5, 1.6, 12, 6,
                    {0.7, 1.2}, strikes_around(50.0, 0.6, 1.5, 9)},
               0.0037, false});
  v.push_back({Spec{"hivol-call", 50.0, 0.15, 0.045, 0.005, 0.015, Side::Call, 0.5, 1.6, 12, 8,
                    {0.7, 1.2}, strikes_around(50.0, 0.6, 1.5, 9)},
               0.0093, false});
  // The near-boundary band: in-the-money puts walking up to the edge (23 d at low σ, the
  // 2.17 y SPY put, a 1 y put on a wide q box).
  v.push_back({Spec{"band-spy23-put", kSpyS, kSpyT, kSpyR, kSpyQlo, kSpyQhi, Side::Put, 0.10,
                    0.50, 12, 4, {0.12, 0.2}, strikes_around(kSpyS, 1.02, 1.12, 11)},
               qm, true});
  v.push_back({Spec{"band-spylong-put", 772.98, 2.1673, 0.05153, -0.003298, -0.002272, Side::Put,
                    0.10, 0.45, 16, 4, {0.15, 0.3}, strikes_around(772.98, 1.1, 1.6, 9)},
               -0.0028, true});
  v.push_back({Spec{"band-wideq-put", 100.0, 1.0, 0.05, 0.0, 0.01, Side::Put, 0.10, 0.60, 12, 6,
                    {0.15, 0.35}, strikes_around(100.0, 1.1, 1.5, 9)},
               0.0041, true});
  return v;
}

struct GreekErrors {
  std::vector<double> rel;
  double worst = 0.0;
  double worst_ref_unc = 0.0; // the reference's own relative uncertainty on that row
  std::string at;
  void add(double fam, double ref, double ref_unc, double floor, const std::string &where) {
    const double r = std::abs(fam - ref) / std::max(std::abs(ref), floor);
    rel.push_back(r);
    if (r > worst) {
      worst = r;
      worst_ref_unc = ref_unc / std::max(std::abs(ref), floor);
      at = where;
    }
  }
  [[nodiscard]] double p99() const {
    if (rel.empty()) {
      return kNaN;
    }
    std::vector<double> v = rel;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(0.99 * static_cast<double>(v.size() - 1u))];
  }
};

// The opt-in CSV path (ATX_VOL_A0_GREEKS_CSV); empty when unset. The _dupenv_s form on
// Windows, as american_discrete_div_reference_matrix_test.cpp reads its own switch.
[[nodiscard]] std::string csv_path() {
#if defined(_WIN32)
  char *value = nullptr;
  std::size_t size = 0U;
  std::string path;
  if (::_dupenv_s(&value, &size, "ATX_VOL_A0_GREEKS_CSV") == 0 && value != nullptr) {
    path = value;
  }
  std::free(value);
  return path;
#else
  const char *value = std::getenv("ATX_VOL_A0_GREEKS_CSV");
  return value != nullptr ? std::string(value) : std::string();
#endif
}

constexpr std::size_t kModelGreeks = 6;
constexpr const char *kModelGreekName[kModelGreeks] = {"delta", "gamma", "vega",
                                                       "theta", "rho",   "phi"};

// One parameter's stencil direction for a row: central off the band; inside it one-sided
// toward the side where the exercise edge recedes (so the stencil stays in the
// continuation region the derivative is defined on). For a put the edge recedes with
// larger S, σ, T, q and smaller r; a call mirrors S, r and q.
struct BandDirs {
  int S = 0;
  int sigma = 0;
  int T = 0;
  int r = 0;
  int q = 0;
};

[[nodiscard]] BandDirs band_dirs(bool band, Side side) {
  BandDirs d;
  if (!band) {
    return d;
  }
  const bool put = side == Side::Put;
  d.S = put ? 1 : -1;
  d.sigma = 1;
  d.T = 1;
  d.r = put ? -1 : 1;
  d.q = put ? 1 : -1;
  return d;
}

TEST(AlSigmaQFamilyGreeksModel, GreeksAgainstTheConvergedModelInsideAndOutsideTheBand) {
  // Three references per Greek: the CONVERGED model (32 / 64), the rung below it (24 / 48:
  // the converged reference's own uncertainty is |conv − rung| plus its ladder tolerance)
  // and the family's OWN build scheme (the geometry's Reference preset, andersen_lake with
  // default options): family vs its build scheme isolates the interpolation error, build
  // scheme vs converged the scheme's distance from convergence — the two parts of the
  // family's distance to the model.
  std::ofstream csv;
  if (const std::string path = csv_path(); !path.empty()) {
    csv.open(path);
    csv << "case,side,S,K,T,r,q,sigma,edge_sd,band,greek,family,converged,conv_tol,conv_unc,"
           "build_scheme,rel_err_conv,rel_err_build,rel_build_vs_conv,l3g_theta,"
           "edge_sd_bisect,reason\n";
    csv << std::setprecision(17);
  }
  GreekErrors vs_conv[2][kModelGreeks]; // [outside band = 0 / inside = 1][greek]
  GreekErrors vs_build[2][kModelGreeks];
  GreekErrors build_vs_conv[2][kModelGreeks];
  GreekErrors l3g_theta[2];
  std::size_t rows[2] = {0, 0};
  std::size_t skipped_exercised = 0;
  std::size_t not_served[kModelGreeks] = {}; // band rows: θ / Γ NearExerciseEdge
  std::size_t l3g_positive_theta = 0;        // band rows where the L3g identity θ > 0
  std::size_t conv_positive_theta = 0;       // rows where the converged calendar θ > 0
  std::size_t served_positive_theta = 0;     // rows serving θ > 0 (must stay 0)
  double zone_sd = 0.0; // widest gap between the bundle's edge and the flag's (σ√T)
  for (const ModelCase &mc : model_cases()) {
    const Spec &s = mc.spec;
    const std::unique_ptr<Built> b = build(s);
    ASSERT_NE(b, nullptr);
    AlSigmaQFamilyRateBumps bumps;
    ASSERT_TRUE(bumps.prepare(b->f)) << s.name;
    // The L3g σ family at the contract's q (its θ is the PDE identity too: review-L3g m5).
    const AlExpiryGeometry gs = prepare_al_geometry(s.T, s.r, mc.q);
    AlSigmaFamily fs(gs, s.side, s.S);
    ASSERT_TRUE(fs.prepare(s.s_lo, s.s_hi, s.n_sigma)) << s.name;
    const double q = mc.q;
    for (const double sigma : s.sigmas) {
      const double sd = sigma * std::sqrt(s.T);
      for (const double K : s.strikes) {
        const std::string at = describe(s, K, sigma, q);
        const AlSigmaQFamilyGreeks g =
            family_greeks(b->f, bumps, s.S, K, sigma, q, request(AlFamilyGreekMask::all()));
        ASSERT_EQ(g.status, AlSigmaQFamilyStatus::Ok) << at;
        if (g.exercised) {
          ++skipped_exercised;
          continue;
        }
        const double edge = edge_spot(b->f, s.side, s.S, K, sigma, q);
        const double edge_sd = std::isfinite(edge) ? std::abs(std::log(s.S / edge)) / sd : kNaN;
        // The bundle types the band itself (review-A0 MJ1): its edge distance agrees with
        // the independent bisection edge, and so does its classification off the border.
        const double band_w = atx::vol::kAlFamilyGreekEdgeBand;
        if (std::isfinite(edge_sd)) {
          // The flag's edge sits at or inside B'_now by at most the floored zone.
          EXPECT_GE(g.edge_sd, edge_sd - 1.0e-9) << at;
          EXPECT_LE(g.edge_sd - edge_sd, kFlooredZoneSd) << at;
          zone_sd = std::max(zone_sd, g.edge_sd - edge_sd);
          if (std::abs(edge_sd - band_w) > kFlooredZoneSd) {
            EXPECT_EQ(g.near_edge, edge_sd < band_w) << at;
          }
        } else {
          EXPECT_FALSE(g.near_edge) << at;
        }
        const bool band = g.near_edge;
        const int sb = band ? 1 : 0;
        ++rows[sb];
        const BandDirs dir = band_dirs(band, s.side);
        const double hS = 0.01 * s.S * sd; // the S stencil reaches 2 h0 = 0.02 σ√T
        const auto model = [&s, K, sigma, q](const std::optional<AlOpts> &o) {
          return [&s, o, K, sigma, q](double dS, double dT, double dsig, double dr, double dq,
                                      bool &ok) {
            const Result<double> p =
                andersen_lake(s.S + dS, K, s.T + dT, sigma + dsig, s.r + dr, q + dq, s.side, o);
            ok = ok && p.has_value();
            return p.has_value() ? *p : kNaN;
          };
        };
        const auto ladder = [&](auto &&m, std::array<Rich, kModelGreeks> &out) {
          out[0] = richardson([&](double x, bool &ok) { return m(x - s.S, 0, 0, 0, 0, ok); }, s.S,
                              hS, 1, dir.S, 4);
          if (!band) { // no one-sided second difference: Γ is not differenced in the band
            out[1] = richardson([&](double x, bool &ok) { return m(x - s.S, 0, 0, 0, 0, ok); },
                                s.S, hS, 2, 0, 4);
          }
          out[2] = richardson([&](double x, bool &ok) { return m(0, 0, x - sigma, 0, 0, ok); },
                              sigma, 2.0e-3 * sigma, 1, dir.sigma, 4);
          // θ = dV/dt = −∂V/∂T (the calendar difference).
          out[3] = richardson([&](double x, bool &ok) { return -m(0, x - s.T, 0, 0, 0, ok); },
                              s.T, 2.0e-3 * s.T, 1, dir.T, 4);
          out[4] = richardson([&](double x, bool &ok) { return m(0, 0, 0, x - s.r, 0, ok); },
                              s.r, 0.02 * s.r, 1, dir.r, 4);
          out[5] = richardson([&](double x, bool &ok) { return m(0, 0, 0, 0, x - q, ok); }, q,
                              0.02 * std::max(std::abs(q), 1.0e-3), 1, dir.q, 4);
        };
        std::array<Rich, kModelGreeks> rc{};
        std::array<Rich, kModelGreeks> rr{};
        std::array<Rich, kModelGreeks> rb{};
        ladder(model(kConverged), rc);
        ladder(model(kRungBelow), rr);
        ladder(model(std::nullopt), rb);
        const double fam[kModelGreeks] = {g.delta, g.gamma, g.vega, g.theta, g.rho, g.phi};
        const double pf = 1.0e-3 * std::max(g.price, 1.0);
        const double floor_of[kModelGreeks] = {1.0e-3, pf / (s.S * s.S), pf, pf, pf, pf};
        const AlSigmaFamilyGreeks l3 =
            family_greeks(fs, s.S, K, sigma, AlSigmaFamilyGreeksMask::hedge());
        for (std::size_t k = 0; k < kModelGreeks; ++k) {
          const auto greek = static_cast<Greek>(k);
          const bool served = g.supported.has(greek);
          if (band && (k == 1 || k == 3)) {
            // θ and Γ in the band: NaN with NearExerciseEdge, by contract (review-A0 MJ1).
            EXPECT_FALSE(served) << at << " " << kModelGreekName[k];
            EXPECT_EQ(g.reason_of(greek), Reason::NearExerciseEdge) << at;
            EXPECT_TRUE(std::isnan(fam[k])) << at << " " << kModelGreekName[k];
            ++not_served[k];
            if (k == 1) {
              continue; // no one-sided second difference: Γ is not differenced in the band
            }
          } else {
            ASSERT_TRUE(served) << at << " " << kModelGreekName[k];
          }
          ASSERT_TRUE(rc[k].ok && rr[k].ok && rb[k].ok) << at << " " << kModelGreekName[k];
          const double rnd = price_rounding(s.S, K, g.price);
          const double tol = 4.0 * rc[k].err + rc[k].gain * rnd;
          const double unc = std::abs(rc[k].value - rr[k].value) + tol;
          const double fl = floor_of[k];
          if (served) {
            vs_conv[sb][k].add(fam[k], rc[k].value, unc, fl, at);
            vs_build[sb][k].add(fam[k], rb[k].value, 4.0 * rb[k].err + rb[k].gain * rnd, fl,
                                at);
          }
          build_vs_conv[sb][k].add(rb[k].value, rc[k].value, unc, fl, at);
          if (k == 3) {
            conv_positive_theta += rc[k].value > 0.0 ? 1u : 0u;
            served_positive_theta += (served && fam[k] > 0.0) ? 1u : 0u;
            if (l3.status == AlSigmaFamilyStatus::Ok && !l3.exercised) {
              l3g_theta[sb].add(l3.theta, rc[k].value, unc, fl, at);
              l3g_positive_theta += (band && l3.theta > 0.0) ? 1u : 0u;
            }
          }
          if (csv.is_open()) {
            const auto rel = [fl](double a, double ref) {
              return std::abs(a - ref) / std::max(std::abs(ref), fl);
            };
            csv << s.name << ',' << (s.side == Side::Put ? "put" : "call") << ',' << s.S << ','
                << K << ',' << s.T << ',' << s.r << ',' << q << ',' << sigma << ',' << g.edge_sd
                << ',' << sb << ',' << kModelGreekName[k] << ',' << fam[k] << ','
                << rc[k].value << ',' << tol << ',' << unc << ',' << rb[k].value << ','
                << rel(fam[k], rc[k].value) << ',' << rel(fam[k], rb[k].value) << ','
                << rel(rb[k].value, rc[k].value) << ',' << ((k == 3) ? l3.theta : kNaN) << ','
                << edge_sd << ',' << static_cast<int>(g.reason_of(greek)) << '\n';
          }
        }
      }
    }
  }
  for (int sb = 0; sb < 2; ++sb) {
    std::cout << "[AlSigmaQFamilyGreeksModel] " << (sb == 0 ? "OUTSIDE" : "INSIDE")
              << " the near-boundary band: rows=" << rows[sb] << "\n";
    for (std::size_t k = 0; k < kModelGreeks; ++k) {
      const GreekErrors &e = vs_conv[sb][k];
      std::cout << "  " << std::setw(5) << kModelGreekName[k] << " n=" << e.rel.size()
                << " vs converged max=" << e.worst << " p99=" << e.p99()
                << " (converged uncertainty there " << e.worst_ref_unc << ") | vs build scheme max="
                << vs_build[sb][k].worst << " p99=" << vs_build[sb][k].p99()
                << " | build scheme vs converged max=" << build_vs_conv[sb][k].worst
                << "\n      worst at " << e.at << "\n";
    }
    std::cout << "  L3g sigma-family theta (PDE identity) vs calendar: n="
              << l3g_theta[sb].rel.size() << " max rel=" << l3g_theta[sb].worst
              << " p99=" << l3g_theta[sb].p99() << " at " << l3g_theta[sb].at << "\n";
  }
  std::cout << "[AlSigmaQFamilyGreeksModel] exercised rows (exercise Greeks, not differenced)="
            << skipped_exercised << "; band rows with theta / gamma NearExerciseEdge="
            << not_served[3] << " / " << not_served[1]
            << "; band rows where the L3g identity theta > 0 (wrong sign)=" << l3g_positive_theta
            << "; rows where the converged calendar theta > 0=" << conv_positive_theta
            << "; widest bundle-edge vs flag-edge gap=" << zone_sd << " sigma*sqrt(T)\n";
  EXPECT_GT(rows[0], 50u);
  EXPECT_GT(rows[1], 5u);
  // Every band row withholds θ and Γ; no served θ is positive anywhere (an American θ is
  // <= 0: the converged calendar differences never are).
  EXPECT_EQ(not_served[3], rows[1]);
  EXPECT_EQ(not_served[1], rows[1]);
  EXPECT_EQ(vs_conv[1][3].rel.size(), 0u);
  EXPECT_EQ(conv_positive_theta, 0u);
  EXPECT_EQ(served_positive_theta, 0u);
  // Outside the band, pinned at the measured worst (A0 r6, 2-figure round-ups; order Δ, Γ,
  // vega, θ, rho, phi). Against the family's own build scheme the gap is the
  // interpolation error (θ: the PDE identity against that scheme's calendar
  // difference; rho: the bump's O(h²) truncation); against the converged model it is
  // the build scheme's own distance from convergence (the same rows' build-vs-converged
  // column carries it). Inside the band the table is reported, not pinned (the
  // separately reported error class). A dropped boundary-motion term is O(1) here.
  constexpr double kVsBuild[kModelGreeks] = {1.2e-8, 5.0e-8, 5.0e-8, 6.9e-5, 2.3e-6, 2.2e-7};
  constexpr double kVsConverged[kModelGreeks] = {1.9e-5, 2.8e-5, 7.0e-5,
                                                 1.8e-4, 1.9e-4, 1.8e-4};
  for (std::size_t k = 0; k < kModelGreeks; ++k) {
    EXPECT_LE(vs_build[0][k].worst, kVsBuild[k]) << kModelGreekName[k] << " at "
                                                 << vs_build[0][k].at;
    EXPECT_LE(vs_conv[0][k].worst, kVsConverged[k]) << kModelGreekName[k] << " at "
                                                    << vs_conv[0][k].at;
  }
}

} // namespace
