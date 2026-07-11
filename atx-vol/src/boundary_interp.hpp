#pragma once

// ── σ-axis Chebyshev interpolation of the dimensionless AL boundary (P2.5) ─
//
// Task 11. When a FITTED SMILE is priced, every strike in an (expiry, r, q)
// slice carries its own market σ, so a 40-strike board pays ~40 cold
// Andersen-Lake boundary solves. But the *dimensionless* boundary y[] (T8's
// homogeneity coordinate y[k] = (log(b_k/xmax))², stored in amer::AlBoundary)
// depends only on (σ, r, q, τ) — NOT on strike. Across a smile ladder at one
// (τ, r, q), σ is the ONLY axis that varies, and y[] is smooth in σ.
//
// SigmaBoundaryInterp builds, per (Kp_ref, τ, rp, qp) internal-put slice, an
// n_σ-node Chebyshev-Lobatto interpolant of y[] in σ (n_σ cold solves shared by
// the whole ladder), then prices each strike by interpolating y[] at that
// strike's σ, rescaling to its K (T8's homogeneity), and running the existing
// premium quadrature (amer::al_put_price_from_boundary). The σ-axis reuses the
// SAME 2nd-kind barycentric Chebyshev machinery american.cpp uses on the τ-axis
// (al_cheb_node / al_cheb_eval); the small evaluator is re-expressed here so the
// hot τ-axis kernels stay untouched, but the scheme is identical.
//
// Internal to the atx-vol library. The public opt-in surface is
// andersen_lake_{put,call}_slice_sigma in american.hpp.

#include <cstdint>

#include "american_boundary.hpp"  // amer:: boundary structs + solve/price seam
#include "atx/vol/american.hpp"   // AlOpts

namespace atx::vol::detail {

// One (τ, rp, qp) internal-put slice's σ-Chebyshev boundary interpolant.
//
// build() does n_σ cold boundary solves (heap-free; the object is ~20 KB of
// stack). price_internal_put() is allocation-free: it interpolates y[] in σ,
// rescales to the queried strike, and runs the premium quadrature. Mutable eval
// scratch => NOT thread-safe; construct one per slice (like AloPricer).
class SigmaBoundaryInterp {
 public:
  static constexpr unsigned kSigmaMax = 16;  // hard cap on n_σ

  SigmaBoundaryInterp() = default;

  // Build the interpolant for internal-put strike Kp_ref, expiry T, internal
  // (rate=rp, yield=qp) over the σ box [sigma_lo, sigma_hi] with n_sigma
  // Chebyshev-Lobatto nodes. Returns false (caller falls back to cold) when the
  // box is degenerate, n_sigma is out of range, the regime is non-American, or
  // any node solve fails.
  [[nodiscard]] bool build(double Kp_ref, double T, double rp, double qp,
                           double sigma_lo, double sigma_hi, std::uint16_t n_sigma,
                           const amer::AlScheme& sch) noexcept;

  [[nodiscard]] bool ok() const noexcept { return ok_; }

  // Interpolated internal-put price at spot Sp, strike Kp, vol sigma. For the put
  // slice Kp varies (homogeneity rescale to Kp); for the call slice Kp == Kp_ref
  // is held fixed (no rescale — bit-structural match to andersen_lake_call_slice,
  // only the σ interpolation approximates). Allocation-free.
  [[nodiscard]] double price_internal_put(double Sp, double Kp, double sigma) noexcept;

  [[nodiscard]] double sigma_lo() const noexcept { return sigma_lo_; }
  [[nodiscard]] double sigma_hi() const noexcept { return sigma_hi_; }
  [[nodiscard]] std::uint16_t n_sigma() const noexcept { return n_sigma_; }

 private:
  amer::AlScheme sch_{};
  amer::AlWorkspace ws_{};      // price-quad binding (captured from a node solve)
  amer::AlBoundary scratch_{};  // node structure (z/wbary/x/tau/n/T) + eval y[]
  double T_ = 0.0;
  double rp_ = 0.0;
  double qp_ = 0.0;
  double sigma_lo_ = 0.0;
  double sigma_hi_ = 0.0;
  std::uint16_t n_sigma_ = 0;
  std::uint16_t n_boundary_ = 0;
  bool ok_ = false;
  double sz_[kSigmaMax] = {};  // σ Chebyshev-Lobatto z-nodes in [-1, 1]
  double sw_[kSigmaMax] = {};  // 2nd-kind barycentric weights for the σ grid
  // series_[k * kSigmaMax + s] = dimensionless y[k] at σ-node s.
  double series_[amer::kAlMaxNodes * kSigmaMax] = {};
};

}  // namespace atx::vol::detail
