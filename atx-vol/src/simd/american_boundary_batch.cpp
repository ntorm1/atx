#include "atx/vol/simd/american_boundary_batch.hpp"

#include "atx/vol/american.hpp"
#include "atx/vol/simd/cpu.hpp"

#include "american_boundary_avx2.hpp"

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
// Sub-Sprint A / Task A1 update (pure-refactor, bit-parity preserved): the AVX2 seed
// no longer pays the sweep-invariant geometry precompute (al_bind_geometry) — it
// seeds via al_seed_put_boundary, which lays down the identical node grid + cold BAW
// y[] but SKIPS the ~n·nq exp+sqrt geometry bind the kernel never reads (it recomputes
// geometry inline). That removed wasted per-lane serial work with the AVX2 output
// bit-identical (AvxBoundary.ForceAvx2_MatchesScalar still green). But the DOMINANT
// seed serialization is the 12-node scalar Barone-Adesi-Whaley Newton root-finds,
// which stay scalar (vectorizing them alters the seed y[] via vector transcendentals
// and would break the bit-parity gate). Best-of-3 gate measurement under concurrent
// load (agent-k paused) was BORDERLINE and thermal-dominated: median ratios 2.21× /
// 1.46× / 1.94× / 1.66× across four samples, warm steady-state ~1.6–1.7× (the scalar
// baseline alone swung 566–930 ms median run-to-run). NOT all runs clear 2.0×, so
// per the ship-gate rule Auto stays SCALAR. See bench JSON:
// bench/baselines/i7-1260p-clang18-avx2-american-shootout-boundary-gate.json.
//
// The kernel, the ISA-override seam, and the validated-parity tests still ship;
// ForceAvx2 routes to the kernel so those tests exercise it every run. Remaining lever
// to clear 2.0×: vectorize the 12-node BAW Newton seed (breaks bit-parity → economic-
// bound parity + gate re-verification) AND re-measure on a quiet host — deferred to
// Sprint I. Flip this to true only when a quiet-host best-of-3 clears 2.0×.
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

} // namespace atx::vol::simd
