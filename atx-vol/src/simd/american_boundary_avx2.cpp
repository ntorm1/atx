// AVX2 (4-lane f64) batched Andersen-Lake AMERICAN-PUT boundary solve + price.
//
// Built with -mavx2 -mfma (src/simd/*_avx2.cpp CMake glob). Called only when the
// dispatch layer confirms use_avx2() at runtime. The boundary solve + price math
// now lives in american_boundary_avx2_kernel.hpp as two reusable laned primitives
// (solve_put_boundary_pack_avx2 / price_put_pack_avx2) so the K3 laned greeks kernel
// (american_greeks_avx2.cpp) rides the SAME proven solve/price code; this file is the
// thin per-pack driver that K2 shipped. The AvxBoundary + SchemeMapping parity tests
// validate the extraction as behavior-preserving. Design decisions (mask-blend Cody
// Φ single-source with scalar; scalar patch-out for degenerate / non-American /
// collapse / non-finite lanes and the n % 4 tail; per-lane serial quadrature
// reduction order matching the scalar accumulation) are documented at the primitives.

#include "american_boundary_avx2.hpp"

#include "american_boundary_avx2_kernel.hpp"

#include "atx/vol/api/pricing/american.hpp" // andersen_lake
#include "fitting/counters.hpp" // BoundarySolves / AlBoundarySolves — per-contract ledger

#include <cstddef>
#include <limits>
#include <optional>

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "american_boundary_avx2.cpp requires -mavx2 -mfma (build via src/simd/*_avx2.cpp)"
#endif

#include <immintrin.h>

namespace atx::vol::simd::detail {

namespace {

// Scalar cold reference for a single put lane (the patch target).
[[nodiscard]] double scalar_put(double S, double K, double T, double sigma,
                                double r, double q,
                                const std::optional<AlOpts>& opts) noexcept {
    const Result<double> res = andersen_lake(S, K, T, sigma, r, q, Side::Put, opts);
    return res.has_value() ? *res : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

void american_put_boundary_batch_avx2(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n,
                                      const std::optional<AlOpts>& opts) noexcept {
    const amer::AlScheme sch = amer::scheme_from_opts(opts);

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        amer::AlBoundary bnd[4];
        amer::AlWorkspace ws[4];
        PutPackBoundary pack;
        bool eligible[4];
        int ref = -1;
        solve_put_boundary_pack_avx2(S + i, K + i, T + i, sigma + i, r + i, q + i,
                                     /*n=*/4, sch, bnd, ws, pack, eligible, ref);

        // All four lanes ineligible → nothing to vectorize; patch each.
        if (ref < 0) {
            for (int l = 0; l < 4; ++l) {
                const std::size_t idx = i + static_cast<std::size_t>(l);
                price_out[idx] =
                    scalar_put(S[idx], K[idx], T[idx], sigma[idx], r[idx], q[idx], opts);
            }
            continue;
        }

        alignas(32) double spot[4];
        for (int l = 0; l < 4; ++l) {
            spot[l] = S[i + static_cast<std::size_t>(l)];
        }
        const __m256d price = price_put_pack_avx2(pack, _mm256_load_pd(spot));
        alignas(32) double pr[4];
        _mm256_store_pd(pr, price);
        for (int l = 0; l < 4; ++l) {
            const std::size_t idx = i + static_cast<std::size_t>(l);
            const bool patch = !eligible[l] || !std::isfinite(pr[l]);
            if (patch) {
                // Scalar patch: scalar_put -> andersen_lake -> al_seed_boundary already
                // bumps BoundarySolves + AlBoundarySolves for this lane.
                price_out[idx] =
                    scalar_put(S[idx], K[idx], T[idx], sigma[idx], r[idx], q[idx], opts);
            } else {
                // A lane the vectorized boundary solve actually priced. Count it as ONE
                // boundary solve so the solve ledger is SIMD-invariant: the 4-wide pack
                // does not call al_seed_boundary (it lays the BAW seed down itself), so
                // without this bump an AVX2-routed mark would under-count the boundary
                // work vs. the per-contract scalar path (ledger 10 -> 2 per the flip
                // triage). Bumping per active lane keeps BoundarySolves == #contracts
                // whether the mark routed scalar or AVX2 — the sprint's solve-economy
                // thesis (11 -> 6 s/u) must read the same on both routes.
                ATX_VOL_COUNT(BoundarySolves);
                counters::ledger::bump(counters::ledger::Solve::AlBoundarySolves);
                price_out[idx] = pr[l];
            }
        }
    }

    // Scalar tail (n % 4): exact scalar path, matching the *_batch_avx2 idiom.
    for (; i < n; ++i) {
        price_out[i] = scalar_put(S[i], K[i], T[i], sigma[i], r[i], q[i], opts);
    }
}

void bary_eval_pack_avx2(const double* znodes, const double* wbary, const double* y,
                         unsigned nb, const double* zq, double* out) noexcept {
    __m256d Y[amer::kAlMaxNodes];
    const unsigned n_use = (nb < amer::kAlMaxNodes) ? nb : amer::kAlMaxNodes;
    for (unsigned j = 0; j < n_use; ++j) {
        Y[j] = _mm256_set1_pd(y[j]);
    }
    _mm256_storeu_pd(out, cheb_eval_pd(znodes, wbary, Y, n_use, _mm256_loadu_pd(zq)));
}

} // namespace atx::vol::simd::detail
