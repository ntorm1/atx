#include "atx/vol/simd/iv_batch.hpp"

#include "atx/vol/implied_vol.hpp"

#include <limits>

namespace atx::vol::simd {

namespace {

// Unpack a scalar implied_vol Result into the (iv, ok) SoA outputs: the value
// with ok = 1 on success, NaN with ok = 0 on any failure (non-finite input,
// out-of-band price, or non-convergence). This is the numerical source of truth
// the AVX2 kernel patches through.
void iv_batch_scalar(const double* price, const double* F, const double* K,
                     const double* T, const double* df, const Side* side,
                     double* iv_out, std::uint8_t* ok_out,
                     std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Result<double> r =
            implied_vol(price[i], F[i], K[i], T[i], df[i], side[i]);
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
    // R-24 (routing decision — measured after K2 on this AVX2 ISA, rel-avx2,
    // best-of-3 under the concurrent-host caveat): the 4-lane AVX2 IV batch is NOT
    // ≥1.2× the scalar per-contract loop. It runs at ~parity or slightly slower —
    // the SR-2017 seed plus two Halley steps plus the (now machine-accurate but
    // exp+division-bearing) Cody-erfc Φ leave no vector headroom, and every
    // degenerate / deep-wing / ill-conditioned lane patches back to scalar. The
    // decision is decisive: AVX2 never approached 1.2× in any run.
    //
    // Per the R-24 finding, both public IV-batch entry points now route SCALAR
    // under one rationale: this raw-pointer entry, and the span-based
    // atx::vol::implied_vol_batch (already scalar). detail::implied_vol_batch_avx2
    // is retained (exercised by the IV shootout bench and a direct parity test)
    // for a future wider ISA (AVX-512), where the 8-lane batch is expected to win.
    iv_batch_scalar(price, F, K, T, df, side, iv_out, ok_out, n);
}

} // namespace atx::vol::simd
