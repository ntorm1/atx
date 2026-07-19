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
// SPEED gate — STILL DEFERRED (this host stays contended). Best-of-3 on the 4096-put
// batch: pre-A5 (scalar seed) 1.77× / 2.15× / 2.04×; post-A5 (vector seed) 2.15× /
// 2.90× / 2.36× — BUT the post-A5 runs landed under heavy concurrent load (absolute
// scalar 375–455 µs/op vs ~140 µs/op minutes earlier; avx2 132–193 µs/op vs ~69),
// which inflates the ratio because the scalar baseline suffers disproportionately.
// That is NOT the required quiet-host, robust ≥2.0× (Sprint I measured 1.87× quiet).
// So per the ship-gate rule Auto stays SCALAR. The vector seed is the durable value:
// on a quiet host it should push the honest ratio from ~1.6–1.87× toward/over 2.0×;
// flip this to true only when a quiet-host best-of-3 robustly clears 2.0×.
//
// The kernel, the ISA-override seam, and the validated economic-parity tests all ship;
// ForceAvx2 routes to the kernel so those tests exercise it (incl. the vector seed)
// every run. The scalar put_batch_scalar path below is the untouched source of truth.
inline constexpr bool kShipAvx2Boundary = false;

} // namespace

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
    bool avx2 = false;
    switch (isa) {
        case SimdIsa::ForceScalar:
            avx2 = false;
            break;
        case SimdIsa::ForceAvx2:
            avx2 = have_avx2();
            break;
        case SimdIsa::Auto:
        default:
            avx2 = kShipAvx2Boundary && have_avx2();
            break;
    }
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

// K3 ship gate — DARK, mirrors kShipAvx2Boundary. The laned analytic Greeks bundle is
// correct (economic-parity-gated vs american_greeks_al) and selectable via ForceAvx2,
// but Auto stays SCALAR until a quiet-window A/B ratifies its speedup. The PM flips
// this after the quiet-window; do NOT flip it from a worktree. See kernel-stage2.md.
inline constexpr bool kShipAvx2Greeks = false;

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
                // A9 (simd-review finding 9): the vector-handled lanes leave the
                // unrequested greeks at 0 (K4 first-order tier). The scalar oracle
                // returns a FULL bundle — and american_greeks_al may itself route to
                // american_greeks_fd, which ignores the needs mask — so zero the
                // unrequested columns here to keep the laned bundle internally
                // consistent across handled and patched lanes.
                AmericanGreeks& gp = out_greeks[off + i];
                if (!need_vega) {
                    gp.vega = gp.volga = gp.vanna = 0.0;
                }
                if (!need_rho) {
                    gp.rho = 0.0;
                }
                if (!need_charm) {
                    gp.charm = 0.0;
                }
            }
        }
    }
    return SimdRoute::Avx2;
}

} // namespace atx::vol::simd
