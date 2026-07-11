// Parity gate for the vectorized (AVX2) eSSVI/SVI batch evaluators.
//
// The batch entry points dispatch to the 4-lane AVX2 path when the host supports
// it (this CI/dev box does — Alder Lake). These tests assert that the vectorized
// result reproduces the scalar per-strike kernels in atx/vol/vol_surface.hpp
// (essvi_backbone_w / svi_total_w) to near machine precision — the backbone is
// pure arithmetic + one sqrt, so parity is essentially exact (the only slack is
// the FMA-contracted inner term, ~1 ULP). Covered: symmetric eSSVI slices, the
// asymmetric-rho blend (rho_scale > 0, rho_R != rho — whole-batch scalar
// fallback), raw SVI, the derived backbone sigma, and every n % 4 tail residue.
// If AVX2 is absent the batch runs the scalar loop and these become identity
// checks — still valid, just trivially exact.

#include "atx/vol/simd/essvi_batch.hpp"

#include "atx/vol/simd/cpu.hpp"
#include "atx/vol/vol_surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
  // Asymmetric-rho blend active (rho_scale > 0 AND rho_R != rho) => scalar
  // fallback path inside the AVX2 kernel.
  {
    EssviParams s;
    s.theta = 0.06; s.phi = 1.8; s.rho = -0.45; s.rho_R = 0.10;
    s.rho_scale = 0.25; s.T = 0.75; s.F = 120.0;
    v.push_back(s);
  }
  return v;
}

std::vector<SviParams> make_svi_slices() {
  std::vector<SviParams> v;
  { SviParams s; s.a = 0.02; s.b = 0.15; s.rho = -0.4; s.m = 0.0; s.sigma = 0.1;
    s.T = 0.25; s.F = 100.0; v.push_back(s); }
  { SviParams s; s.a = 0.05; s.b = 0.30; s.rho = 0.25; s.m = -0.05; s.sigma = 0.2;
    s.T = 1.0; s.F = 250.0; v.push_back(s); }
  return v;
}

// Combined abs+rel comparison; ~1e-12 given the arithmetic-only kernel.
void expect_close(double got, double want, const char* ctx, std::size_t i) {
  constexpr double kAbs = 1e-12;
  constexpr double kRel = 1e-12;
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

TEST(SimdEssviBatch, ZeroLengthIsNoOp) {
  EssviParams s;
  s.theta = 0.04; s.phi = 1.2; s.rho = -0.3; s.T = 0.25;
  double sentinel = 42.0;
  const double k = 0.0;
  simd::essvi_backbone_w_batch(s, &k, &sentinel, 0);
  EXPECT_EQ(sentinel, 42.0);
}

} // namespace
} // namespace atx::vol
