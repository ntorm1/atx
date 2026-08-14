#pragma once

// Batched (SoA) Black-76 Greeks — the vectorized risk hot path.
//
// Companion to atx/vol/simd/black76_batch.hpp: for a whole array of contracts
// this returns, in one call, the premium AND all eight analytic sensitivities
// (atx::vol::Greeks) per contract. It dispatches to a 4-lane AVX2 kernel when
// the host supports it (atx::vol::simd::have_avx2()) and to a scalar loop
// otherwise. The scalar loop calls the exact per-contract atx::vol::black76_greeks,
// so it is the numerical source of truth; the AVX2 path (full-range Cody rational-erfc
// Φ) reproduces it to ~1e-9 absolute and computes even the deep wings on the vector
// path — only degenerate / non-finite lanes are patched through the scalar kernel.
//
// Layout: structure-of-arrays. Each input array has length n; F/K/T/sigma/r/df
// are contiguous doubles, side is one Side per contract. greeks_out holds n
// Greeks and price_out n doubles; the outputs must not alias the inputs.
// Passing n == 0 is a no-op.
//
// The function is noexcept and allocation-free — safe to call concurrently from
// any threads (no shared mutable state; the CPUID cache is init-once).

#include <cstddef>

#include "atx/vol/api/pricing/greeks.hpp" // Greeks
#include "atx/vol/api/core/types.hpp"  // Side

namespace atx::vol::simd {

// ── SoA output view (P3.4) ────────────────────────────────────────────────
//
// The NATURAL vector output shape: nine independent per-greek columns, each of
// length n, instead of the AoS `Greeks[]`. A null column pointer is SKIPPED —
// the vector core computes every field but stores only the requested ones, which
// is how a GreekFieldMask selects columns without a second pass. Columns must not
// alias the inputs or each other.
//
// This is the shape `black76_greeks_batch_soa` writes DIRECTLY (no AoS scatter),
// and the shape `american_greeks_batch` (american_batch.hpp) fills for its SoA
// risk surface. Non-owning by design (trivially copyable), so both the noexcept
// SIMD kernel and a higher-level owning container can hand it the same view.
struct GreeksBatchSoA {
  double* delta{nullptr};
  double* gamma{nullptr};
  double* vega{nullptr};
  double* theta{nullptr};
  double* rho{nullptr};
  double* vanna{nullptr};
  double* volga{nullptr};
  double* charm{nullptr};
  double* price{nullptr};
};

// Black-76 Greeks + premium, computed DIRECTLY into the per-greek SoA columns of
// `out` (the vectorized-risk hot path's native output — no AoS scatter):
//   out.delta[i] = black76_greeks(F[i],...).greeks.delta, etc.
// Bit-identical to black76_greeks_batch field-for-field: the AoS entry below and
// this SoA entry share ONE AVX2 vector core, differing only in the final scatter.
// A null column in `out` is not written. Degenerate/deep-wing lanes patch through
// the exact scalar kernel. noexcept + allocation-free; n == 0 is a no-op.
void black76_greeks_batch_soa(const double* F, const double* K, const double* T,
                              const double* sigma, const double* r,
                              const double* df, const Side* side,
                              const GreeksBatchSoA& out, std::size_t n) noexcept;

// Black-76 Greeks + premium for each contract (AoS output):
//   {greeks_out[i], price_out[i]} = black76_greeks(F[i], K[i], T[i], sigma[i],
//                                                   r[i], df[i], side[i]).
// A thin scatter over the shared vector core (see black76_greeks_batch_soa) —
// byte-for-byte identical to the pre-SoA kernel. Degenerate (T <= 0 or sigma <= 0)
// collapses to the intrinsic-step delta with the other Greeks zero, exactly as the
// scalar kernel.
void black76_greeks_batch(const double* F, const double* K, const double* T,
                          const double* sigma, const double* r,
                          const double* df, const Side* side,
                          Greeks* greeks_out, double* price_out,
                          std::size_t n) noexcept;

} // namespace atx::vol::simd
