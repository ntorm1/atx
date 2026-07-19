#include "atx/vol/simd/american_boundary_batch.hpp"

#include "atx/vol/american.hpp"
#include "atx/vol/simd/cpu.hpp"

#include "american_boundary_avx2.hpp"
#include "american_greeks_avx2.hpp"

#include <limits>

namespace atx::vol::simd {

namespace {

// Scalar reference: price every put through the exact cold andersen_lake. This is
// both the fallback path (no AVX2 / ForceScalar) and the numerical source of
// truth the AVX2 route is validated against. On an error result (e.g. the
// double-continuation corner) the price is NaN — the caller feeds only valid
// American puts through the batch.
void put_batch_scalar(const double* S, const double* K, const double* T,
                      const double* sigma, const double* r, const double* q,
                      double* price_out, std::size_t n,
                      const std::optional<AlOpts>& opts) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Result<double> res =
            andersen_lake(S[i], K[i], T[i], sigma[i], r[i], q[i], Side::Put, opts);
        price_out[i] =
            res.has_value() ? *res : std::numeric_limits<double>::quiet_NaN();
    }
}

// KEEP-SCALAR decision (T13 / P3.2), by the controller's default-shift policy.
//
// The policy: DEFAULT the AVX2 path iff it clears ≥2.0× AND the gap is immaterial
// (normal ≲1e-6 USD, stress ≤1e-3). ACCURACY is fine here and would NOT force a
// flag: the FastDeterministic Chebyshev-Φ path (detail/vector_math.hpp) is bounded
// at max |AVX2−scalar| ≈ 6.4e-7 USD on the normal grid (< the 1e-6 immateriality
// threshold) and ≈3.3e-9 on the stress grid (« 1e-3) — an immaterial, algorithmic
// shift the policy explicitly permits.
//
// It is the SPEED gate that is missed. On a homogeneous 4096-put batch (build-rel,
// i7-1260P dev box) the measured throughput ratio is ~1.6–1.7× (median 1.66×,
// uncontended min-based 1.60×; only outlier runs with a contended scalar baseline
// cross 2.0×). The kernel is transcendental-bound AND keeps the per-lane BAW SEED
// scalar, so only the sweep + premium quadrature are vectorized. That unvectorized
// seed caps the blended speedup well under the 4× lane width, landing at ~1.6×.
//
// Sub-Sprint A / Task A1 update (pure-refactor): the AVX2 seed stopped paying the
// sweep-invariant geometry precompute (al_bind_geometry) but kept the DOMINANT
// serialization — the per-node scalar Barone-Adesi-Whaley Newton root-finds — scalar,
// so the blended speedup stayed capped at ~1.6×.
//
// Sub-Sprint A / Task A5 update: that scalar per-lane BAW Newton is now VECTORIZED
// 4-wide (american_boundary_avx2.cpp step 2.5; al_init_put_boundary supplies the node
// grid, the kernel lays down the seed itself). This BREAKS bit-parity with the scalar
// seed, so the gate is now ECONOMIC-BOUND, not byte-identical — and it holds with
// margin to spare: AvxBoundary parity max |AVX2−scalar| = 4.1e-13 normal / 1.1e-13
// stress (« kNormalGate 1e-6 / stress 1e-3); ForceScalar-vs-ForceAvx2 parity green
// across all AmericanBoundaryBatchSchemeMapping schemes.
//
// SHIP DECISION (2026-07-19 fit+backtest SOTA sprint, PM integration ruling): OPT-IN,
// NOT default-on. atx-vol is bit-reproducible-by-default, so the DEFAULT Auto path must
// return the exact scalar boundary solve — the grouped-price fingerprint and the
// evaluate_batch == per-entry evaluate contracts must hold bit-for-bit on AVX2 and
// non-AVX2 hosts alike, which an Auto->AVX2 (~1e-13-shifted, 4-lane-pack-dependent) mark
// route silently breaks. The AVX2 marks kernel stays a validated PERF opt-in engaged ONLY
// under ForceAvx2: it clears parity with orders of magnitude to spare (ForceScalar-vs-
// ForceAvx2 max |dev| 4.1e-13 normal / 1.1e-13 stress, << kNormalGate 1e-6; see AvxBoundary.*
// in simd_american_test.cpp) and wins ~2.5-3.1x on this dev box (WS-K bench), so a caller
// that accepts the shift opts in explicitly. This mirrors the greeks posture (the laned
// analytic greeks engage only under an explicit ForceAvx2 request, gated at their
// priced_surface caller) and main (kShipAvx2Boundary == false there). WS-K briefly flipped
// this ON (Auto->AVX2); the PM reverted it to opt-in to restore cross-host reproducibility.
//
// USER OVERRIDE (2026-07-19, solve-wall sprint): the above opt-in ruling is EXPLICITLY
// overridden by the user, who elected Auto default-ON after being shown the cross-host
// reproducibility cost. The quiet-window A/B ratified the marks speedup (ql_fast 3.97x,
// CV 3.92/4.67%, best-of-5). The thread-count non-determinism is fixed (the H0/H5 tile
// schedule keeps AVX2 pack membership worker-count-invariant). The RESIDUAL cost the user
// accepted: default results are AVX2-host-dependent at ~1e-13 (differ from the scalar oracle
// and across microarchitectures) — economically nil (10+ orders below a tick) but it relaxes
// bit-reproducible-by-default to reproducible-per-host. Documented per the PM epsilon license.
inline constexpr bool kShipAvx2Boundary = true;

} // namespace

// Whether the AVX2 boundary route is selected for `isa` on this host (ForceAvx2 =>
// AVX2 iff supported; Auto => scalar, the bit-reproducible default; ForceScalar => never).
// Mirrors avx2_greeks_selected. Exposed so a threaded marks caller (portfolio_pricer)
// can gate its H0 invariant-pack-membership tile schedule on the SAME predicate the
// dispatch uses — the tile schedule engages exactly when this is true (i.e. under the
// opt-in ForceAvx2), keeping the AVX2 marks pack membership thread-count bit-identical.
bool avx2_boundary_selected(SimdIsa isa) noexcept {
    switch (isa) {
        case SimdIsa::ForceScalar:
            return false;
        case SimdIsa::ForceAvx2:
            return have_avx2();
        case SimdIsa::Auto:
        default:
            return kShipAvx2Boundary && have_avx2();
    }
}

SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n,
                                      SimdIsa isa) noexcept {
    return american_put_boundary_batch(S, K, T, sigma, r, q, price_out, n,
                                       std::nullopt, isa);
}

SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n,
                                      const std::optional<AlOpts>& opts,
                                      SimdIsa isa) noexcept {
    const bool avx2 = avx2_boundary_selected(isa);
    if (avx2) {
        detail::american_put_boundary_batch_avx2(S, K, T, sigma, r, q, price_out,
                                                 n, opts);
        return SimdRoute::Avx2;
    }
    put_batch_scalar(S, K, T, sigma, r, q, price_out, n, opts);
    return SimdRoute::Scalar;
}

SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n) noexcept {
    return american_put_boundary_batch(S, K, T, sigma, r, q, price_out, n,
                                       simd_isa_override());
}

namespace {

// K3 ship gate — mirrors kShipAvx2Boundary. SHIP DECISION (WS-K, 2026-07-19): DEFAULT ON.
// The laned analytic Greeks bundle is economic-parity-gated vs the scalar american_greeks_al
// oracle (AmericanPutGreeksBatchAvx2.MatchesScalarAl measured rel-dev: price 1.6e-10,
// delta 5.7e-9, gamma 2.1e-6, vega 1.4e-8, rho 3.1e-7, vanna 5.6e-7, volga 4.7e-6,
// theta 1.5e-7, charm 3.0e-5 — all the AVX2-transcendental ~1e-13 USD price delta amplified
// by the FD denominators, economically negligible). Same sprint re-scope as the boundary
// gate: parity-green + faster-than-scalar ships it (WS-K report: laned greeks beat the scalar
// american_greeks_al bundle on this host). Auto selects AVX2 on capable CPUs; the scalar
// american_greeks_al bundle stays the oracle + fallback (ForceScalar, non-AVX2 hosts, and
// every non-early-exercise / non-finite lane patch). NOTE: the portfolio/priced_surface
// greeks path does not yet dispatch american_greeks_batch (WS-H wires that) — flipping this
// only activates AVX2 for the direct american_greeks_batch / american_put_greeks_batch
// callers today.
inline constexpr bool kShipAvx2Greeks = true;

// Scalar oracle: american_greeks_al per contract (NaN-filled on error, matching the
// batch's per-lane fallback contract).
void greeks_scalar_lane(const double* S, const double* K, const double* T,
                        const double* sigma, const double* r, const double* q,
                        const std::optional<AlOpts>& opts, AmericanGreeks* out,
                        std::size_t i) noexcept {
    const Result<AmericanGreeks> g =
        american_greeks_al(S[i], K[i], T[i], sigma[i], r[i], q[i], Side::Put, opts);
    if (g.has_value()) {
        out[i] = *g;
    } else {
        const double kNaN = std::numeric_limits<double>::quiet_NaN();
        out[i] = AmericanGreeks{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN};
    }
}

} // namespace

bool avx2_greeks_selected(SimdIsa isa) noexcept {
    switch (isa) {
        case SimdIsa::ForceScalar:
            return false;
        case SimdIsa::ForceAvx2:
            return have_avx2();
        case SimdIsa::Auto:
        default:
            return kShipAvx2Greeks && have_avx2();
    }
}

SimdRoute american_put_greeks_batch(const double* S, const double* K, const double* T,
                                    const double* sigma, const double* r, const double* q,
                                    std::size_t n, const std::optional<AlOpts>& opts,
                                    AmericanGreeks* out_greeks, SimdIsa isa,
                                    bool need_vega, bool need_rho,
                                    bool need_charm) noexcept {
    const bool avx2 = avx2_greeks_selected(isa);
    if (n == 0) {
        return avx2 ? SimdRoute::Avx2 : SimdRoute::Scalar;
    }
    if (!avx2) {
        // Scalar oracle stays the full american_greeks_al bundle (correctness first; the
        // first-order solve-skip win is on the laned majority, not the scalar patch).
        for (std::size_t i = 0; i < n; ++i) {
            greeks_scalar_lane(S, K, T, sigma, r, q, opts, out_greeks, i);
        }
        return SimdRoute::Scalar;
    }
    const detail::GreekNeeds needs{need_vega, need_rho, need_charm};
    // AVX2 route: lane the bundle, then patch the lanes the kernel could not handle
    // (non-early-exercise on any needed bump state, or non-finite) through the scalar
    // oracle. Chunked with a stack `handled` buffer to stay allocation-free.
    constexpr std::size_t kChunk = 512;
    for (std::size_t off = 0; off < n; off += kChunk) {
        const std::size_t cn = (n - off < kChunk) ? (n - off) : kChunk;
        bool handled[kChunk];
        detail::american_put_greeks_batch_avx2(S + off, K + off, T + off, sigma + off,
                                               r + off, q + off, cn, opts,
                                               out_greeks + off, handled, needs);
        for (std::size_t i = 0; i < cn; ++i) {
            if (!handled[i]) {
                greeks_scalar_lane(S + off, K + off, T + off, sigma + off, r + off, q + off,
                                   opts, out_greeks + off, i);
            }
        }
    }
    return SimdRoute::Avx2;
}

} // namespace atx::vol::simd
