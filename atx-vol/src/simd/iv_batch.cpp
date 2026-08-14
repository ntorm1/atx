#include "simd/iv_batch.hpp"

#include "atx/vol/api/pricing/implied_vol.hpp"

#include <limits>
#include <new> // std::bad_alloc (per-lane noexcept containment)

namespace atx::vol::simd {

namespace {

// SAFETY: `implied_vol` is NOT noexcept — a failing lane builds an Error whose
// message string is longer than any SSO buffer, so it allocates and can raise
// std::bad_alloc. `implied_vol_batch` is declared noexcept in the PUBLIC header,
// which makes that a process-killing promise rather than a compiler hint.
// Contain the throw at the lane granularity the header's contract already
// speaks in: an unsolvable lane is reported (NaN, ok = 0), exactly like a
// non-finite input or a non-converged root. Only bad_alloc is caught — any other
// exception would be a contract violation deeper in the inverter and should stay
// loud rather than be laundered into a lane failure.
[[nodiscard]] Result<double> invert_lane(double price, double F, double K, double T, double df,
                                         Side side) noexcept {
    try {
        return implied_vol(price, F, K, T, df, side);
    } catch (const std::bad_alloc&) {
        // No message: composing one is another allocation, which is what failed.
        return atx::core::Err(atx::core::ErrorCode::Internal);
    }
}

// Unpack a scalar implied_vol Result into the (iv, ok) SoA outputs: the value
// with ok = 1 on success, NaN with ok = 0 on any failure (non-finite input,
// out-of-band price, non-convergence, or allocation failure). This is the
// numerical source of truth the AVX2 kernel patches through.
void iv_batch_scalar(const double* price, const double* F, const double* K,
                     const double* T, const double* df, const Side* side,
                     double* iv_out, std::uint8_t* ok_out,
                     std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Result<double> r =
            invert_lane(price[i], F[i], K[i], T[i], df[i], side[i]);
        if (r) {
            iv_out[i] = *r;
            ok_out[i] = 1;
        } else {
            iv_out[i] = std::numeric_limits<double>::quiet_NaN();
            ok_out[i] = 0;
        }
    }
}

} // namespace

void implied_vol_batch(const double* price, const double* F, const double* K,
                       const double* T, const double* df, const Side* side,
                       double* iv_out, std::uint8_t* ok_out,
                       std::size_t n) noexcept {
    // R-24 (routing decision). The 4-lane AVX2 IV batch is NOT ≥1.2× the scalar
    // per-contract loop, so both public IV-batch entry points route SCALAR under
    // one rationale: this raw-pointer entry, and the span-based
    // atx::vol::implied_vol_batch. The SR-2017 seed plus two Halley steps plus the
    // machine-accurate but exp+division-bearing Cody-erfc Φ leave no vector
    // headroom, and every degenerate / non-finite / ill-conditioned lane patches
    // back to scalar.
    //
    // Re-measured at Sprint I after the K2 wing-patch tail (finite-wing patch
    // retired + exp_pd deep-underflow flush) — bench/simd_iv_bench.cpp,
    // rel-avx2, best-of-3, concurrent-host caveat: AVX2 ≈ 3.28 M items/s vs scalar
    // ≈ 3.38 M/s → ~0.95–0.97× (parity to slightly slower; the exp_pd guard adds a
    // hair to the Φ chain). Decisive: AVX2 never approached 1.2× in any run, so no
    // flip. detail::implied_vol_batch_avx2 is retained (IV shootout bench + a
    // direct parity test) for a future wider ISA (AVX-512), where the 8-lane batch
    // is expected to win; the dispatcher may re-confirm on a quiet host.
    iv_batch_scalar(price, F, K, T, df, side, iv_out, ok_out, n);
}

} // namespace atx::vol::simd
