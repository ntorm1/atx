#pragma once

// Batched (SoA) Andersen-Lake AMERICAN-PUT boundary solve + price — the P3.2
// AVX2 vectorization across INDEPENDENT OPTIONS (AoSoA<4>, one __m256d lane per
// contract).
//
// Unlike the downstream European/greeks/iv/essvi/pnl batch kernels (which
// vectorize a closed-form pricer across contracts), this kernel vectorizes the
// ITERATIVE Andersen-Lake exercise-boundary solve: 4 independent puts run their
// Jacobi-Newton + fixed-point sweeps in lockstep, sharing the hand-tuned AVX2
// transcendentals (detail/vector_math.hpp). The BAW seed stays scalar and
// per-lane (it is the reference seed, bit-identical to the cold solver); only the
// transcendental-bound sweep + premium quadrature are vectorized.
//
// Scope (T13): American PUTS only, one homogeneous ACCURATE scheme per call.
// Calls (McDonald-Schroder put map) and the full public american_*_batch API +
// PreparedPortfolio integration are T15. Each lane is priced as an American put:
//     price_out[i] ≈ andersen_lake(S[i],K[i],T[i],sigma[i],r[i],q[i], Side::Put)
// to the P3 accuracy gate (≲1e-6 USD normal per the default-shift immateriality
// policy — measured ~6.4e-7 from the FastDeterministic vector Φ; ≤1e-3 USD
// stress). Degenerate
// (T≤1e-12 ∨ σ≤1e-8), non-American-regime, boundary-collapse, deep-wing, and any
// non-finite lane PATCH through the exact scalar andersen_lake, so parity holds
// everywhere (exactly the idiom the *_batch_avx2 kernels use for their tails).
//
// Layout: structure-of-arrays, each input length n, outputs length n and must not
// alias inputs. n == 0 is a no-op. noexcept + allocation-free (the per-pack state
// is stack std::array); safe to call concurrently.

#include <cstddef>

namespace atx::vol::simd {

// Which path a call actually executed — exposed so a bench/test can assert the
// dispatch (the P3.1 "expose the selected ISA" requirement).
enum class SimdRoute { Scalar, Avx2 };

// Price a homogeneous span of American puts. Returns the route taken (Avx2 when
// use_avx2() is true, else Scalar). The scalar route calls andersen_lake per
// contract and is the numerical source of truth; the AVX2 route reproduces it to
// the accuracy gate with edge lanes patched through the same scalar kernel.
SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n) noexcept;

} // namespace atx::vol::simd
