// Parity gate for the vectorized (AVX2) eSSVI/SVI batch evaluators.
//
// The batch entry points dispatch to the 4-lane AVX2 path when the host supports
// it (this CI/dev box does — Alder Lake). These tests assert that the vectorized
// result reproduces the scalar per-strike kernels in atx/vol/vol_surface.hpp
// (essvi_backbone_w / svi_total_w) to near machine precision — the backbone is
// pure arithmetic + one sqrt, so parity is essentially exact (the only slack is
// the FMA-contracted inner term, ~1 ULP). Covered: symmetric eSSVI slices, a
// slice ARMING the retired asymmetric-rho blend (rho_scale > 0, rho_R != rho —
// whole-batch scalar fallback, and now a refusal in both lanes), raw SVI, the
// derived backbone sigma, and every n % 4 tail residue.
// If AVX2 is absent the batch runs the scalar loop and these become identity
// checks — still valid, just trivially exact.
//
// PARITY INCLUDES REFUSALS. The scalar kernel returns NaN for an armed slice
// (vol_surface.hpp, `essvi_rho_blend_armed`), so "SIMD == scalar" on such a
// slice means "both lanes refuse". `expect_close` therefore treats NaN-vs-NaN
// as agreement and NaN-vs-finite as a failure — a lane that quietly produced a
// number where the other refused is the exact defect this file guards.

#include "simd/essvi_batch.hpp"

#include "atx/vol/api/simd/cpu.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// A broad, deterministic log-moneyness grid spanning both deep wings and ATM.
std::vector<double> make_k_grid() {
  std::vector<double> k;
  for (int i = -200; i <= 200; ++i) {
    k.push_back(0.01 * static_cast<double>(i)); // k in [-2, 2], step 0.01
  }
  return k;
}

// Representative eSSVI slices: symmetric (blend off two ways) and asymmetric.
std::vector<EssviParams> make_essvi_slices() {
  std::vector<EssviParams> v;
  // Symmetric: rho_scale <= 0 (blend disabled outright).
  {
    EssviParams s;
    s.theta = 0.04; s.phi = 1.2; s.rho = -0.3; s.T = 0.25; s.F = 100.0;
    v.push_back(s);
  }
  // Symmetric via a steeper backbone, still no blend.
  {
    EssviParams s;
    s.theta = 0.09; s.phi = 3.5; s.rho = 0.55; s.T = 1.0; s.F = 250.0;
    v.push_back(s);
  }
  // rho_scale > 0 but rho_R == rho => rho_eff collapses to rho (still constant,
  // so the kernel vectorizes it; must match scalar exactly).
  {
    EssviParams s;
    s.theta = 0.05; s.phi = 2.0; s.rho = -0.2; s.rho_R = -0.2;
    s.rho_scale = 0.3; s.T = 0.5; s.F = 50.0;
    v.push_back(s);
  }
  // ARMED retired rho blend (rho_scale > 0 AND rho_R != rho) => scalar fallback
  // path inside the AVX2 kernel, and the scalar kernel refuses with NaN. Kept in
  // the fixture precisely because it is the state whose contract changed: both
  // lanes must refuse, and refuse identically.
  {
    EssviParams s;
    s.theta = 0.06; s.phi = 1.8; s.rho = -0.45; s.rho_R = 0.10;
    s.rho_scale = 0.25; s.T = 0.75; s.F = 120.0;
    v.push_back(s);
  }
  return v;
}

// The armed slice above, by itself.
EssviParams make_armed_slice() { return make_essvi_slices().back(); }

std::vector<SviParams> make_svi_slices() {
  std::vector<SviParams> v;
  { SviParams s; s.a = 0.02; s.b = 0.15; s.rho = -0.4; s.m = 0.0; s.sigma = 0.1;
    s.T = 0.25; s.F = 100.0; v.push_back(s); }
  { SviParams s; s.a = 0.05; s.b = 0.30; s.rho = 0.25; s.m = -0.05; s.sigma = 0.2;
    s.T = 1.0; s.F = 250.0; v.push_back(s); }
  return v;
}

// Combined abs+rel comparison; ~1e-12 given the arithmetic-only kernel. A
// refusal is a result: both-NaN agrees, one-NaN does not (a plain EXPECT_LE
// would fail on NaN-vs-NaN and so could never express "both lanes refused").
void expect_close(double got, double want, const char* ctx, std::size_t i) {
  constexpr double kAbs = 1e-12;
  constexpr double kRel = 1e-12;
  if (std::isnan(got) || std::isnan(want)) {
    EXPECT_TRUE(std::isnan(got) && std::isnan(want))
        << ctx << " i=" << i << " got=" << got << " want=" << want
        << " (one lane refused and the other did not)";
    return;
  }
  EXPECT_LE(std::abs(got - want), kAbs + kRel * std::abs(want))
      << ctx << " i=" << i << " got=" << got << " want=" << want;
}

TEST(SimdEssviBatch, BackboneMatchesScalarAllSlices) {
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  for (const EssviParams& s : make_essvi_slices()) {
    std::vector<double> got(n, 0.0);
    simd::essvi_backbone_w_batch(s, k.data(), got.data(), n);
    double max_abs = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double want = essvi_backbone_w(s, k[i]);
      max_abs = std::max(max_abs, std::abs(got[i] - want));
      expect_close(got[i], want, "essvi_backbone", i);
    }
    EXPECT_LT(max_abs, 1e-11);
  }
}

TEST(SimdEssviBatch, SviMatchesScalarAllSlices) {
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  for (const SviParams& s : make_svi_slices()) {
    std::vector<double> got(n, 0.0);
    simd::svi_total_w_batch(s, k.data(), got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      expect_close(got[i], svi_total_w(s, k[i]), "svi_total", i);
    }
  }
}

TEST(SimdEssviBatch, BackboneSigmaMatchesScalarAllSlices) {
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  for (const EssviParams& s : make_essvi_slices()) {
    std::vector<double> got(n, 0.0);
    simd::essvi_backbone_sigma_batch(s, k.data(), got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      const double w = essvi_backbone_w(s, k[i]);
      const double want = std::sqrt(std::max(w, 0.0) / s.T);
      expect_close(got[i], want, "essvi_sigma", i);
    }
  }
}

// The scalar tail (n % 4 != 0) must be handled for every residue class, on both
// a symmetric (vectorized) and an asymmetric (scalar-fallback) slice.
TEST(SimdEssviBatch, HandlesEveryTailResidue) {
  const std::vector<double> k = make_k_grid();
  const std::vector<EssviParams> slices = make_essvi_slices();
  for (const EssviParams& s : {slices.front(), slices.back()}) {
    for (std::size_t n = 1; n <= 11; ++n) {
      std::vector<double> got(n, 0.0);
      simd::essvi_backbone_w_batch(s, k.data(), got.data(), n);
      for (std::size_t i = 0; i < n; ++i) {
        expect_close(got[i], essvi_backbone_w(s, k[i]), "tail", i);
      }
    }
  }
}

// WHY an armed slice comes back all-NaN, for the reader who hits it: the
// rho_R/rho_scale blend is retired (vol_surface.hpp, `essvi_rho_blend_armed`)
// because a strike-dependent rho voids every published SSVI butterfly and
// calendar condition. The refusal must be uniform — a vectorized lane that
// answered with a number would serve an uncertified curve on exactly the slices
// the refusal exists to stop.
TEST(SimdEssviBatch, ArmedRhoBlendSliceRefusedByBothLanes) {
  const EssviParams s = make_armed_slice();
  ASSERT_TRUE(atx::vol::essvi_rho_blend_armed(s));

  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  std::vector<double> w(n, 0.0), sig(n, 0.0);
  std::vector<double> dth(n, 0.0), dphi(n, 0.0), drho(n, 0.0);
  simd::essvi_backbone_w_batch(s, k.data(), w.data(), n);
  simd::essvi_backbone_sigma_batch(s, k.data(), sig.data(), n);
  simd::essvi_backbone_w_grad_batch(s, k.data(), w.data(), dth.data(), dphi.data(), drho.data(),
                                    n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_TRUE(std::isnan(essvi_backbone_w(s, k[i]))) << "scalar i=" << i;
    EXPECT_TRUE(std::isnan(w[i])) << "batch w i=" << i;
    EXPECT_TRUE(std::isnan(sig[i])) << "batch sigma i=" << i;
    EXPECT_TRUE(std::isnan(dth[i])) << "batch dtheta i=" << i;
    EXPECT_TRUE(std::isnan(dphi[i])) << "batch dphi i=" << i;
    EXPECT_TRUE(std::isnan(drho[i])) << "batch drho i=" << i;
  }
}

// An UNARMED slice that merely carries a non-zero rho_scale (rho_R == rho) is
// still a legal, vectorizable, constant-rho curve — the refusal must not widen
// into "any slice that touches the retired fields".
TEST(SimdEssviBatch, ScaleWithoutDistinctRightRhoStillEvaluates) {
  EssviParams s;
  s.theta = 0.05; s.phi = 2.0; s.rho = -0.2; s.rho_R = -0.2;
  s.rho_scale = 0.3; s.T = 0.5; s.F = 50.0;
  ASSERT_FALSE(atx::vol::essvi_rho_blend_armed(s));

  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  std::vector<double> w(n, 0.0);
  simd::essvi_backbone_w_batch(s, k.data(), w.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(std::isfinite(w[i])) << "i=" << i;
    expect_close(w[i], essvi_backbone_w(s, k[i]), "unarmed", i);
  }
}

TEST(SimdEssviBatch, ZeroLengthIsNoOp) {
  EssviParams s;
  s.theta = 0.04; s.phi = 1.2; s.rho = -0.3; s.T = 0.25;
  double sentinel = 42.0;
  const double k = 0.0;
  simd::essvi_backbone_w_batch(s, &k, &sentinel, 0);
  EXPECT_EQ(sentinel, 42.0);
}

// ── Fused w + natural-gradient batch (essvi_backbone_w_grad_batch) ──────────
//
// One kernel produces w AND {∂w/∂θ, ∂w/∂φ, ∂w/∂ρ} sharing the backbone tree.
// It must reproduce the scalar source-of-truth pair essvi_backbone_w /
// essvi_w_grad3 to ~1e-12 on every slice class (symmetric vectorized + the
// asymmetric whole-batch scalar fallback), for w and all three partials.
TEST(SimdEssviBatch, GradBatchMatchesScalarAllSlices) {
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  for (const EssviParams& s : make_essvi_slices()) {
    std::vector<double> w(n, 0.0), dth(n, 0.0), dphi(n, 0.0), drho(n, 0.0);
    simd::essvi_backbone_w_grad_batch(s, k.data(), w.data(), dth.data(),
                                      dphi.data(), drho.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      expect_close(w[i], essvi_backbone_w(s, k[i]), "wgrad.w", i);
      const std::array<double, 3> g = essvi_w_grad3(s, k[i]);
      expect_close(dth[i], g[0], "wgrad.dtheta", i);
      expect_close(dphi[i], g[1], "wgrad.dphi", i);
      expect_close(drho[i], g[2], "wgrad.drho", i);
    }
  }
}

// Every n % 4 tail residue, on a symmetric (vectorized) and an asymmetric
// (scalar-fallback) slice — w and all three partials.
TEST(SimdEssviBatch, GradBatchHandlesEveryTailResidue) {
  const std::vector<double> k = make_k_grid();
  const std::vector<EssviParams> slices = make_essvi_slices();
  for (const EssviParams& s : {slices.front(), slices.back()}) {
    for (std::size_t n = 1; n <= 11; ++n) {
      std::vector<double> w(n, 0.0), dth(n, 0.0), dphi(n, 0.0), drho(n, 0.0);
      simd::essvi_backbone_w_grad_batch(s, k.data(), w.data(), dth.data(),
                                        dphi.data(), drho.data(), n);
      for (std::size_t i = 0; i < n; ++i) {
        expect_close(w[i], essvi_backbone_w(s, k[i]), "wgrad.tail.w", i);
        const std::array<double, 3> g = essvi_w_grad3(s, k[i]);
        expect_close(dth[i], g[0], "wgrad.tail.dtheta", i);
        expect_close(dphi[i], g[1], "wgrad.tail.dphi", i);
        expect_close(drho[i], g[2], "wgrad.tail.drho", i);
      }
    }
  }
}

TEST(SimdEssviBatch, GradBatchZeroLengthIsNoOp) {
  EssviParams s;
  s.theta = 0.04; s.phi = 1.2; s.rho = -0.3; s.T = 0.25;
  double w = 1.0, dth = 2.0, dphi = 3.0, drho = 4.0;
  const double k = 0.0;
  simd::essvi_backbone_w_grad_batch(s, &k, &w, &dth, &dphi, &drho, 0);
  EXPECT_EQ(w, 1.0);
  EXPECT_EQ(dth, 2.0);
  EXPECT_EQ(dphi, 3.0);
  EXPECT_EQ(drho, 4.0);
}

// ── A9 (simd-review finding 6): grad/sigma batch guard coverage ─────────────
//
// The w-batch refuses the vector path on a non-admissible slice (|rho| >= 1,
// non-positive theta/phi) and patches non-finite-k lanes from scalar. The grad and
// sigma batches only checked blend_active, so a non-admissible slice / non-finite k
// leaked NaN by accident. These tests lock the mirrored guards: a non-admissible
// slice must reproduce the scalar per-strike kernel BIT-FOR-BIT (the whole-batch
// scalar fallback), and a non-finite-k lane must match scalar exactly (patched).

// Exact match with NaN treated as equal (both paths produce the SAME NaN once they
// route to the scalar kernel).
[[nodiscard]] bool bit_match(double a, double b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  return a == b;
}

// A non-admissible slice: |rho| >= 1 (blend inactive). The vector path would use an
// fmadd where the scalar rounds mul-then-add — a 1-ulp divergence on the finite
// lanes — so a bit-exact check is RED without the admissibility refusal and GREEN
// with it (the batch runs the exact scalar loop).
EssviParams make_non_admissible_slice() {
  EssviParams s;
  s.theta = 0.04; s.phi = 1.2; s.rho = 1.5; s.T = 0.25; s.F = 100.0;
  return s;
}

// A grid mixing finite k with +inf/-inf/NaN, length 10 (two vector blocks + a tail).
std::vector<double> make_nonfinite_k_grid() {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return {0.10, inf, -0.20, -inf, 0.30, nan, 0.05, -0.15, 0.20, inf};
}

TEST(SimdEssviBatch, GradBatchNonAdmissibleRhoMatchesScalarBitExact) {
  const EssviParams s = make_non_admissible_slice();
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  std::vector<double> w(n, 0.0), dth(n, 0.0), dphi(n, 0.0), drho(n, 0.0);
  simd::essvi_backbone_w_grad_batch(s, k.data(), w.data(), dth.data(), dphi.data(), drho.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::array<double, 3> g = essvi_w_grad3(s, k[i]);
    EXPECT_TRUE(bit_match(w[i], essvi_backbone_w(s, k[i]))) << "w i=" << i;
    EXPECT_TRUE(bit_match(dth[i], g[0])) << "dtheta i=" << i;
    EXPECT_TRUE(bit_match(dphi[i], g[1])) << "dphi i=" << i;
    EXPECT_TRUE(bit_match(drho[i], g[2])) << "drho i=" << i;
  }
}

TEST(SimdEssviBatch, SigmaBatchNonAdmissibleRhoMatchesScalarBitExact) {
  const EssviParams s = make_non_admissible_slice();
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  std::vector<double> sig(n, 0.0);
  simd::essvi_backbone_sigma_batch(s, k.data(), sig.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const double w = essvi_backbone_w(s, k[i]);
    const double want = std::sqrt(std::max(w, 0.0) / s.T);
    EXPECT_TRUE(bit_match(sig[i], want)) << "sigma i=" << i;
  }
}

TEST(SimdEssviBatch, GradBatchNonFiniteKMatchesScalar) {
  EssviParams s;
  s.theta = 0.05; s.phi = 1.8; s.rho = -0.3; s.T = 0.5; s.F = 100.0; // admissible
  const std::vector<double> k = make_nonfinite_k_grid();
  const std::size_t n = k.size();
  std::vector<double> w(n, 0.0), dth(n, 0.0), dphi(n, 0.0), drho(n, 0.0);
  simd::essvi_backbone_w_grad_batch(s, k.data(), w.data(), dth.data(), dphi.data(), drho.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::array<double, 3> g = essvi_w_grad3(s, k[i]);
    if (std::isfinite(k[i])) {
      expect_close(w[i], essvi_backbone_w(s, k[i]), "nfk.w", i);
      expect_close(dth[i], g[0], "nfk.dtheta", i);
      expect_close(dphi[i], g[1], "nfk.dphi", i);
      expect_close(drho[i], g[2], "nfk.drho", i);
    } else {
      // Non-finite lane must be patched exactly from scalar.
      EXPECT_TRUE(bit_match(w[i], essvi_backbone_w(s, k[i]))) << "nfk.w i=" << i;
      EXPECT_TRUE(bit_match(dth[i], g[0])) << "nfk.dtheta i=" << i;
      EXPECT_TRUE(bit_match(dphi[i], g[1])) << "nfk.dphi i=" << i;
      EXPECT_TRUE(bit_match(drho[i], g[2])) << "nfk.drho i=" << i;
    }
  }
}

TEST(SimdEssviBatch, SigmaBatchNonFiniteKMatchesScalar) {
  EssviParams s;
  s.theta = 0.05; s.phi = 1.8; s.rho = -0.3; s.T = 0.5; s.F = 100.0; // admissible
  const std::vector<double> k = make_nonfinite_k_grid();
  const std::size_t n = k.size();
  std::vector<double> sig(n, 0.0);
  simd::essvi_backbone_sigma_batch(s, k.data(), sig.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const double w = essvi_backbone_w(s, k[i]);
    const double want = std::sqrt(std::max(w, 0.0) / s.T);
    if (std::isfinite(k[i])) {
      expect_close(sig[i], want, "nfk.sigma", i);
    } else {
      EXPECT_TRUE(bit_match(sig[i], want)) << "nfk.sigma i=" << i;
    }
  }
}

// ── Quasi-explicit rotated basis (svi_qe_basis_batch) ───────────────────────
//
// The raw-SVI quasi-explicit fitter's per-strike (u, v) basis at fixed (m,
// sigma). This kernel IS the numerical source of truth (there is no pre-existing
// scalar per-strike function it wraps), so the oracle here is an independent
// op-for-op reimplementation of svi_calib.cpp's build_and_solve_normal loop.
// Parity target ~1e-12 (in practice bit-exact: div, mul+add, sqrt are all
// correctly-rounded IEEE and the kernel avoids FMA on y*y+1).

// 1/sqrt(2) as spelled in svi_calib.cpp (kInvSqrt2) — the exact constant.
constexpr double kQeInvSqrt2 = 0.70710678118654752440;

// Independent op-for-op reference for one strike (mirrors svi_calib.cpp:149-152).
void qe_basis_ref(double m, double sigma, double k, double& u, double& v) {
  const double y = (k - m) / sigma;
  const double z = std::sqrt(y * y + 1.0);
  u = (y + z) * kQeInvSqrt2;
  v = (z - y) * kQeInvSqrt2;
}

// (m, sigma) grid spanning ATM/shifted centers and a range of widths incl. a
// tiny sigma (the fit's sigma_min floor is 1e-3).
std::vector<std::array<double, 2>> make_ms_grid() {
  std::vector<std::array<double, 2>> g;
  for (const double m : {-0.35, -0.05, 0.0, 0.08, 0.30}) {
    for (const double sigma : {1.0e-3, 0.02, 0.10, 0.25, 0.60}) {
      g.push_back({m, sigma});
    }
  }
  return g;
}

TEST(SviQeBasisBatch, MatchesScalarAllSlices) {
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();
  double max_abs = 0.0;
  for (const std::array<double, 2>& ms : make_ms_grid()) {
    std::vector<double> u(n, 0.0), v(n, 0.0);
    simd::svi_qe_basis_batch(ms[0], ms[1], k.data(), u.data(), v.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      double uref = 0.0, vref = 0.0;
      qe_basis_ref(ms[0], ms[1], k[i], uref, vref);
      max_abs = std::max(max_abs, std::abs(u[i] - uref));
      max_abs = std::max(max_abs, std::abs(v[i] - vref));
      expect_close(u[i], uref, "qe.u", i);
      expect_close(v[i], vref, "qe.v", i);
    }
  }
  EXPECT_LT(max_abs, 1e-11);
}

// Every n % 4 tail residue on a representative (m, sigma), for both outputs.
TEST(SviQeBasisBatch, HandlesEveryTailResidue) {
  const std::vector<double> k = make_k_grid();
  for (const std::array<double, 2>& ms : {std::array<double, 2>{0.0, 0.10},
                                          std::array<double, 2>{-0.12, 1.0e-3}}) {
    for (std::size_t n = 1; n <= 11; ++n) {
      std::vector<double> u(n, 0.0), v(n, 0.0);
      simd::svi_qe_basis_batch(ms[0], ms[1], k.data(), u.data(), v.data(), n);
      for (std::size_t i = 0; i < n; ++i) {
        double uref = 0.0, vref = 0.0;
        qe_basis_ref(ms[0], ms[1], k[i], uref, vref);
        expect_close(u[i], uref, "qe.tail.u", i);
        expect_close(v[i], vref, "qe.tail.v", i);
      }
    }
  }
}

TEST(SviQeBasisBatch, ZeroLengthIsNoOp) {
  double u = 7.0, v = 9.0;
  const double k = 0.0;
  simd::svi_qe_basis_batch(0.0, 0.10, &k, &u, &v, 0);
  EXPECT_EQ(u, 7.0);
  EXPECT_EQ(v, 9.0);
}

} // namespace
} // namespace atx::vol
