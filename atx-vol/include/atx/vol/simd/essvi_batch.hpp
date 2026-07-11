#pragma once

// Batched eSSVI/SVI total-variance evaluators — the calibration fit hot path.
//
// A calibrator's inner loop evaluates ONE slice's total-variance backbone at
// MANY log-moneyness strikes (residual + Jacobian passes over the quote grid).
// These entry points do that whole strike sweep in one call, dispatching to a
// 4-lane AVX2 kernel when the host supports it (atx::vol::simd::have_avx2())
// and to a scalar loop otherwise.
//
// The scalar loop calls the exact per-strike kernels in atx/vol/vol_surface.hpp
// (essvi_backbone_w / svi_total_w), so it is the numerical source of truth; the
// AVX2 path is pure arithmetic + one sqrt and reproduces it per strike to ~1e-12
// (combined abs+rel). The eSSVI backbone's asymmetric-rho blend (rho_scale > 0
// AND rho_R != rho) needs a transcendental (tanh) with no bit-exact 4-lane form,
// so that regime falls the WHOLE batch back to the scalar kernel; the common
// symmetric backbone (a constant effective rho) is fully vectorized.
//
// Layout: a single slice by value, plus an array of `n` contiguous doubles of
// log-moneyness `k_log`; the length-`n` output must not alias the input. Passing
// n == 0 is a no-op. All functions are noexcept and allocation-free — safe to
// call concurrently from any threads (no shared mutable state; the CPUID cache
// is init-once).

#include <cstddef>

#include "atx/vol/vol_surface.hpp" // EssviParams, SviParams

namespace atx::vol::simd {

// eSSVI backbone total variance for each strike:
// w_out[i] = essvi_backbone_w(slice, k_log[i]). Backbone only (no wing residual),
// matching the scalar bare evaluator. NaN in => NaN out.
void essvi_backbone_w_batch(const EssviParams& slice, const double* k_log,
                            double* w_out, std::size_t n) noexcept;

// Raw-SVI total variance for each strike:
// w_out[i] = svi_total_w(slice, k_log[i]). Pure arithmetic, always vectorized.
void svi_total_w_batch(const SviParams& slice, const double* k_log,
                       double* w_out, std::size_t n) noexcept;

// eSSVI backbone implied vol for each strike:
// sigma_out[i] = sqrt(max(essvi_backbone_w(slice, k_log[i]), 0) / slice.T).
// Shares the backbone work, then one extra max/div/sqrt — the shape a
// residual-vs-market-vol fit consumes directly. Assumes slice.T > 0 and finite
// inputs (the calibration regime); on those it matches the scalar reference to
// the same ~1e-12 tolerance.
void essvi_backbone_sigma_batch(const EssviParams& slice, const double* k_log,
                                double* sigma_out, std::size_t n) noexcept;

} // namespace atx::vol::simd
