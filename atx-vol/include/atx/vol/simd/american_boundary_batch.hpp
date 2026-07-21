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
// Scope (T13): American PUTS only, one homogeneous Andersen-Lake scheme per call.
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
#include <optional>

#include "atx/vol/american.hpp"
#include "atx/vol/simd/cpu.hpp"

namespace atx::vol::simd {

// Which path a call actually executed — exposed so a bench/test can assert the
// dispatch (the P3.1 "expose the selected ISA" requirement).
enum class SimdRoute { Scalar, Avx2 };

// Price a homogeneous span of American puts using the call-local ISA selection.
// Auto preserves the measured ship gate; ForceAvx2 uses AVX2 when the host
// supports it and safely falls back to scalar otherwise. This overload does not
// read or mutate the process-global ISA override and is safe to call concurrently
// with a different `isa` in another thread.
SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n,
                                      SimdIsa isa) noexcept;

// Option-aware call-local route. `opts` has exactly the same engagement and
// scheme-mapping semantics as andersen_lake: null selects the ACCURATE scheme;
// an engaged value selects the corresponding configured scheme. Every scalar
// patch receives the same optional unchanged.
SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n,
                                      const std::optional<AlOpts>& opts,
                                      SimdIsa isa) noexcept;

// Legacy coarse-control overload. Resolves the current process-global override
// once, then delegates to the call-local overload above. The scalar route calls
// andersen_lake per contract and is the numerical source of truth; the AVX2 route
// reproduces it to the accuracy gate with edge lanes patched through scalar.
SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n) noexcept;

// Whether the AVX2 boundary route is selected for `isa` on this host (ForceAvx2 =>
// AVX2 iff supported; Auto => the kShipAvx2Boundary ship gate; ForceScalar => never).
// Mirrors avx2_greeks_selected. Exposed so a threaded MARKS caller (portfolio_pricer)
// can gate its invariant-pack-membership tile schedule on the SAME predicate the
// dispatch uses: with kShipAvx2Boundary now ON, Auto marks ride AVX2, whose 4-lane pack
// composition must stay independent of the thread partition to remain thread-count
// bit-identical. A range-split (per-thread) marks batch is NOT thread-invariant under
// AVX2 — use the tile schedule whenever this predicate is true, not just for ForceAvx2.
[[nodiscard]] bool avx2_boundary_selected(SimdIsa isa) noexcept;

// ── K3: laned ANALYTIC American-PUT Greeks bundle (call-local ISA) ──────────
//
// Fills out_greeks[i] (length n) with the full 8-Greek analytic bundle + price for a
// span of American puts, matching scalar american_greeks_al. The scalar route is the
// per-contract oracle; the AVX2 route lanes 4 puts through the K3 kernel (one solve per
// bump state per pack) and patches any lane that is not genuine early-exercise on every
// state — or non-finite — through scalar american_greeks_al, so parity holds
// everywhere within the documented economic gate. Auto respects the (dark) ship gate.
// `opts` has andersen_lake engagement semantics. noexcept + allocation-free.
//
// `need_vega`/`need_rho`/`need_charm` (K4 first-order tier) skip the boundary solves the
// requested greeks don't need: price+delta+gamma+theta ride the base solve alone; vega/
// volga/vanna gate the sigma+/- solves; rho the r+/- solves; charm the wide speed
// stencils. A hedge caller ({delta}) thus pays 1 boundary solve instead of 5. Defaults
// keep the full bundle. Unrequested greeks are left 0 in out_greeks (the caller masks).
SimdRoute american_put_greeks_batch(const double* S, const double* K, const double* T,
                                    const double* sigma, const double* r, const double* q,
                                    std::size_t n, const std::optional<AlOpts>& opts,
                                    AmericanGreeks* out_greeks, SimdIsa isa,
                                    bool need_vega = true, bool need_rho = true,
                                    bool need_charm = true) noexcept;

// Call-native mirror of american_put_greeks_batch (P1b). Fills out_greeks[i] (length n)
// with the full analytic bundle + price for a span of American CALLS, matching scalar
// american_greeks_al(...,Side::Call) within the same documented economic gate. The AVX2
// route lanes 4 calls through the McDonald-Schroder internal-put kernel (one solve per
// bump state per pack, spot stencils vary the internal strike by homogeneity) and patches
// any lane not genuine early-exercise on every needed state — or non-finite — through the
// scalar call oracle. Auto respects the kShipAvx2Greeks ship gate. Same K4 first-order
// tier (need_vega/need_rho/need_charm) as the put batch. noexcept + allocation-free.
SimdRoute american_call_greeks_batch(const double* S, const double* K, const double* T,
                                     const double* sigma, const double* r, const double* q,
                                     std::size_t n, const std::optional<AlOpts>& opts,
                                     AmericanGreeks* out_greeks, SimdIsa isa,
                                     bool need_vega = true, bool need_rho = true,
                                     bool need_charm = true) noexcept;

// Whether the laned AVX2 Greeks route is selected for `isa` on this host (ForceAvx2 =>
// AVX2 iff supported; Auto => the dark ship gate; ForceScalar => never). Exposed so the
// american_greeks_batch SoA surface routes its analytic PUT lanes the SAME way the
// direct dispatch does, honoring one ship-gate constant.
[[nodiscard]] bool avx2_greeks_selected(SimdIsa isa) noexcept;

} // namespace atx::vol::simd
