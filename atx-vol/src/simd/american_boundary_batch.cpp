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
                      double* price_out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Result<double> res =
            andersen_lake(S[i], K[i], T[i], sigma[i], r[i], q[i], Side::Put);
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
// cross 2.0×). The kernel is transcendental-bound AND — per this brief's design —
// keeps the per-lane BAW SEED scalar (the exact cold reference seed), so only the
// sweep + premium quadrature are vectorized. That unvectorized seed caps the
// blended speedup well under the 4× lane width, landing at ~1.6×. Below the 2.0×
// gate, so Auto stays SCALAR — a measured "AVX2 not worth it here yet" (T10/T12-
// style), the honest outcome the policy calls out.
//
// The kernel, the ISA-override seam, and the validated-parity tests still ship;
// ForceAvx2 routes to the kernel so those tests exercise it every run. Flip this to
// true if a future change (T14 vector-math bakeoff, or a vectorized/cheaper seed)
// pushes the homogeneous batch past 2.0× — accuracy already clears the policy.
inline constexpr bool kShipAvx2Boundary = false;

} // namespace

SimdRoute american_put_boundary_batch(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n) noexcept {
    bool avx2;
    switch (simd_isa_override()) {
        case SimdIsa::ForceScalar:
            avx2 = false;
            break;
        case SimdIsa::ForceAvx2:
            avx2 = true; // explicit test/bench force; caller guards have_avx2()
            break;
        case SimdIsa::Auto:
        default:
            avx2 = kShipAvx2Boundary && have_avx2();
            break;
    }
    if (avx2) {
        detail::american_put_boundary_batch_avx2(S, K, T, sigma, r, q, price_out,
                                                 n);
        return SimdRoute::Avx2;
    }
    put_batch_scalar(S, K, T, sigma, r, q, price_out, n);
    return SimdRoute::Scalar;
}

} // namespace atx::vol::simd
