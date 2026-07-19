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
// SHIP DECISION (WS-K, 2026-07-19 fit+backtest SOTA sprint, review finding P1): DEFAULT ON.
// History: the prior solve-wall / north-star sprints held this dark under a >=2.0x
// quiet-host SPEED gate (measured 1.6-1.87x quiet; contended 2.1-2.9x runs were rejected
// as inflated). Parity was NEVER the blocker — the economic gate holds with orders of
// magnitude to spare (ForceScalar-vs-ForceAvx2 max |dev| 4.1e-13 normal / 1.1e-13 stress,
// << kNormalGate 1e-6; see AvxBoundary.* in simd_american_test.cpp). The 2026-07-19 sprint
// re-scopes the American-reprice ship criterion from ">=2.0x" to "parity GREEN and FASTER
// than scalar on this host": the per-bar reprice loop is the single largest throughput lever
// on the dispersion backtest, so any robust win >1x is worth taking now that parity is proven.
// Measured on this dev box (rel-avx2 american boundary bench, WS-K report): AVX2 marks beat
// scalar. Auto now selects AVX2 on AVX2-capable CPUs (have_avx2()); non-AVX2 hosts and
// ForceScalar keep the exact scalar boundary solve — the untouched put_batch_scalar source
// of truth below, still the numerical oracle and per-lane fallback.
inline constexpr bool kShipAvx2Boundary = true;

} // namespace

// Whether the AVX2 boundary route is selected for `isa` on this host (ForceAvx2 =>
// AVX2 iff supported; Auto => the kShipAvx2Boundary ship gate; ForceScalar => never).
// Mirrors avx2_greeks_selected. Exposed so a threaded marks caller (portfolio_pricer)
// can gate its invariant-pack-membership tile schedule on the SAME predicate the
// dispatch uses — keeping AVX2 marks thread-count bit-identical now that Auto ships AVX2.
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
