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
// (combined abs+rel). An ARMED asymmetric-rho blend (rho_scale > 0 AND
// rho_R != rho) routes the WHOLE batch to the scalar kernel, which REFUSES it
// with NaN: the blend was retired (T9), so both lanes decline the same slices.
// rho_R / rho_scale are reserved-zero wire vocabulary, not an evaluated feature.
//
// Layout: a single slice by value, plus an array of `n` contiguous doubles of
// log-moneyness `k_log`; the length-`n` output must not alias the input. Passing
// n == 0 is a no-op. All functions are noexcept and allocation-free — safe to
// call concurrently from any threads (no shared mutable state; the CPUID cache
// is init-once).

#include <cstddef>

#include "atx/vol/api/fitting/vol_surface.hpp" // EssviParams, SviParams

namespace atx::vol::simd {

// eSSVI backbone total variance for each strike:
// w_out[i] = essvi_backbone_w(slice, k_log[i]). Backbone only (no wing residual),
// matching the scalar bare evaluator. NaN in => NaN out.
void essvi_backbone_w_batch(const EssviParams& slice, const double* k_log,
                            double* w_out, std::size_t n) noexcept;

// eSSVI backbone total variance AND its natural-parameter gradient in ONE pass,
// for each strike:
//   w_out[i]     = essvi_backbone_w(slice, k_log[i])
//   dw_dtheta[i] = ∂w/∂θ,  dw_dphi[i] = ∂w/∂φ,  dw_drho[i] = ∂w/∂ρ
//                = essvi_w_grad3(slice, k_log[i])  == {[0], [1], [2]}
// The LM residual/Jacobian build needs both w (for the residual) and the natural
// gradient (mapped into cube coords by the caller) at every quote strike; this
// kernel shares the backbone subexpressions (pk / inner / sqrt) between
// w and the three partials — one evaluation tree, not the scalar path's two
// (essvi_backbone_w THEN essvi_w_grad3, each recomputing the same tree). The
// scalar fallback composes those exact two source-of-truth calls per element, so
// a non-AVX2 host reproduces today's numbers bit-for-bit; the AVX2 path matches
// them to ~1e-12 (combined abs+rel). An ARMED asymmetric-rho blend routes the
// WHOLE batch to the scalar kernels, which refuse it with NaN, exactly like
// essvi_backbone_w_batch. The four length-`n` outputs
// must not alias each other or the input; n == 0 is a no-op.
void essvi_backbone_w_grad_batch(const EssviParams& slice, const double* k_log,
                                 double* w_out, double* dw_dtheta,
                                 double* dw_dphi, double* dw_drho,
                                 std::size_t n) noexcept;

// Raw-SVI total variance for each strike:
// w_out[i] = svi_total_w(slice, k_log[i]). Pure arithmetic, always vectorized.
void svi_total_w_batch(const SviParams& slice, const double* k_log,
                       double* w_out, std::size_t n) noexcept;

// Raw-SVI quasi-explicit rotated basis (u, v) for each strike at fixed (m, sigma)
// — the De Marco-Martini fitter's per-strike hot loop (svi_calib.cpp's
// build_and_solve_normal / svi_qe_sse):
//   y        = (k[i] - m) / sigma
//   z        = sqrt(y*y + 1)
//   u_out[i] = (y + z) / sqrt(2)
//   v_out[i] = (z - y) / sqrt(2)
// VALUES-ONLY: this fills the basis arrays; the weighted normal-equation
// accumulation stays a scalar loop in the caller (summation order preserved).
// Pure arithmetic + one sqrt; always vectorized (no data-dependent fallback).
// Ops are op-for-op the scalar loop (div, mul+add — NOT fma — then sqrt), so the
// non-AVX2 path is bit-identical and the AVX2 path matches to ~1e-12. Assumes
// sigma != 0 (the fit's sigma_min floor guarantees it). The two length-`n`
// outputs must not alias each other or the input; n == 0 is a no-op.
void svi_qe_basis_batch(double m, double sigma, const double* k, double* u_out,
                        double* v_out, std::size_t n) noexcept;

// eSSVI backbone implied vol for each strike:
// sigma_out[i] = sqrt(max(essvi_backbone_w(slice, k_log[i]), 0) / slice.T).
// Shares the backbone work, then one extra max/div/sqrt — the shape a
// residual-vs-market-vol fit consumes directly. Assumes slice.T > 0 and finite
// inputs (the calibration regime); on those it matches the scalar reference to
// the same ~1e-12 tolerance.
void essvi_backbone_sigma_batch(const EssviParams& slice, const double* k_log,
                                double* sigma_out, std::size_t n) noexcept;

} // namespace atx::vol::simd
