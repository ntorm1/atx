#pragma once

// Scalar-backed batch/SIMD kernel layer for atx-vol.
//
// Ported from the C `ats-vol` library's hand-written AVX2 batch kernels
// (ats_pricer_b76_avx2.c, ats_pricer_b76_from_lnFK_avx2.c,
// ats_pricer_b76_value_and_vega_avx2.c, ats_pricer_iv_avx2.c,
// ats_greeks_b76_avx2.c, ats_vol_essvi_avx2.c) and their public *_batch
// declarations (ats_b76.h, ats_iv.h, ats_greeks.h, ats_vol_surface.h).
//
// The C kernels carried one CONTRACT that this port preserves exactly:
//
//     batch result == scalar result (to machine precision).
//
// The C's own SIMD paths honoured that contract by patching every lane that
// the vectorized Chebyshev-Φ path could not price to full accuracy (|d| > 6
// wings, T <= 0 / sigma <= 0 degenerates) back through the scalar kernel, and
// its test (test_pricer_simd.c) asserted the batch matched the scalar
// reference lane-by-lane. This port takes that contract to its logical
// conclusion: each batch entry is a bounded loop over the ALREADY-PORTED
// scalar kernel, so the batch output is bit-identical to the scalar output on
// every lane by construction. The hand-written AVX2 vectorization is a
// documented performance follow-on (see the PORT NOTE in batch.cpp and
// atx-vol/README.md); it is a throughput optimization, not a numerical one —
// the scalar kernels here remain the numerical source of truth.
//
// Shape conventions mirror the C's *_batch signatures: struct-of-arrays
// (SoA) inputs/outputs as spans, with the per-slice-shared scalars (T, sqrt_t,
// df) passed by value where the C shared them across lanes (the from-lnFK and
// value+vega kernels key on a single expiry slice). Every entry validates that
// all per-lane spans have equal length and returns InvalidArgument otherwise
// (agent profile §4: validate at the boundary).
//
// Thread-safety: every entry is a pure function of its arguments over
// disjoint output storage — the underlying scalar kernels are stateless, so
// concurrent calls on non-overlapping outputs from any threads are safe.

#include <span>

#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/surface.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

// ── Black-76 price (SoA) ─────────────────────────────────────────────────
//
// Mirrors the C `ats_pricer_b76_batch`. Prices `price_out[i] =
// black76_price(F[i], K[i], T[i], sigma[i], df[i], side[i])` for every lane.
// All spans must share one length; degenerate lanes (T <= 0 / sigma <= 0)
// collapse to discounted intrinsic exactly as the scalar kernel does.
//
// @return InvalidArgument if the span lengths differ.
[[nodiscard]] Status black76_price_batch(std::span<const double> F,
                                         std::span<const double> K,
                                         std::span<const double> T,
                                         std::span<const double> sigma,
                                         std::span<const double> df,
                                         std::span<const Side> side,
                                         std::span<double> price_out);

// ── Black-76 price from precomputed log-moneyness (SoA, shared slice) ─────
//
// Mirrors the C `ats_pricer_b76_from_lnFK_batch`: `T`, `sqrt_t`, and `df` are
// shared across all lanes (one expiry slice); `F`, `K`, `sigma`, `ln_fk`, and
// `side` are per-lane. Each lane calls `black76_price_from_lnfk`.
//
// @return InvalidArgument if the per-lane span lengths differ.
[[nodiscard]] Status black76_price_from_lnfk_batch(
    std::span<const double> F, std::span<const double> K, double T,
    double sqrt_t, std::span<const double> sigma, double df,
    std::span<const double> ln_fk, std::span<const Side> side,
    std::span<double> price_out);

// ── Black-76 fused value + vega (SoA, shared T) ──────────────────────────
//
// Mirrors the C `ats_pricer_b76_value_and_vega_batch4` generalized from the
// fixed 4-lane kernel to an n-lane batch. `T` and `sqrt_t_in` are shared
// across lanes; `F`, `K`, `sigma`, `df`, and `side` are per-lane. Writes the
// premium to `value_out[i]` and vega to `vega_out[i]` via
// `black76_value_and_vega`. `sqrt_t_in >= 0` is used as-is; a negative value
// (the default) is the sentinel for "compute sqrt(T) internally", exactly as
// the scalar kernel.
//
// @return InvalidArgument if the per-lane span lengths differ.
[[nodiscard]] Status black76_value_and_vega_batch(
    std::span<const double> F, std::span<const double> K, double T,
    std::span<const double> sigma, std::span<const double> df,
    std::span<const Side> side, std::span<double> value_out,
    std::span<double> vega_out, double sqrt_t_in = -1.0);

// ── Implied-volatility inversion (SoA, parallel status) ──────────────────
//
// Mirrors the C `ats_pricer_iv_batch`, whose failure convention is a PARALLEL
// status array plus NaN in the value slot. For every lane this calls the
// scalar `implied_vol`; on success it writes the IV to `iv_out[i]` and Ok() to
// `status_out[i]`, and on failure it writes NaN to `iv_out[i]` and the
// scalar's Error to `status_out[i]`. The function's own return reports only
// argument validation — Ok() means the spans were well-formed; inspect
// `status_out` for per-lane outcomes.
//
// @return InvalidArgument if the span lengths differ.
[[nodiscard]] Status implied_vol_batch(
    std::span<const double> price, std::span<const double> F,
    std::span<const double> K, std::span<const double> T,
    std::span<const double> df, std::span<const Side> side,
    std::span<double> iv_out, std::span<Status> status_out);

// ── Analytic Black-76 Greeks (SoA in, AoS Greeks out) ────────────────────
//
// Mirrors the C `ats_greeks_b76_batch`. Writes the eight sensitivities to
// `greeks_out[i]` via the scalar `black76_greeks`. `price_out` is optional:
// pass an empty span to skip the premium (matches the C's nullable
// `out_price`); when non-empty it must match the input length and receives the
// per-lane premium.
//
// PORT NOTE (arg order): the C scalar `ats_greeks_b76` takes (..., r, df, ...)
// while the C `ats_greeks_b76_batch` took (..., df, r, ...) — the C batch was
// itself inconsistent with its scalar. This port follows the ported scalar
// `black76_greeks(F, K, T, sigma, r, df, side)` — the numerical source of
// truth it calls — so the (r, df) order is uniform across atx-vol.
//
// @return InvalidArgument if the span lengths differ, or if a non-empty
//         `price_out` does not match the input length.
[[nodiscard]] Status black76_greeks_batch(
    std::span<const double> F, std::span<const double> K,
    std::span<const double> T, std::span<const double> sigma,
    std::span<const double> r, std::span<const double> df,
    std::span<const Side> side, std::span<Greeks> greeks_out,
    std::span<double> price_out = {});

// ── eSSVI total variance over a k-grid (single slice, SoA) ───────────────
//
// Mirrors the C `ats_vol_essvi_w_batch4_symmetric` generalized to an n-lane
// batch: one eSSVI slice, many lanes of log-moneyness. Writes `w_out[i] =
// essvi_w(slice, k_log[i])` — the base 3-parameter Gatheral-Jacquier backbone,
// the exact symmetric-no-residual form the C fast path evaluated.
//
// @return InvalidArgument if `k_log` and `w_out` differ in length.
[[nodiscard]] Status essvi_w_batch(const EssviSlice& slice,
                                   std::span<const double> k_log,
                                   std::span<double> w_out);

}  // namespace atx::vol
