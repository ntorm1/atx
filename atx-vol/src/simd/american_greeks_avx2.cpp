// AVX2 (4-lane f64) laned ANALYTIC American-PUT Greeks bundle (K3, the >=5x keystone).
//
// Built with -mavx2 -mfma (src/simd/*_avx2.cpp glob). Extends the K2 4-lane boundary
// pack from marks to the full analytic Greeks bundle, mirroring the scalar
// american_greeks_al (american.cpp): FIVE cold boundary solves per pack — base,
// sigma+/-, r+/- — with delta/gamma/speed from the SPOT-INDEPENDENT base boundary's
// spot stencils (re-priced, NO re-solve), vega/volga/vanna from the sigma+/- solves,
// rho from the r+/- solves, and theta/charm from the continuation-region PDE (analytic,
// no time-bumped solve). The lane axis is CROSS-CONTRACT (4 puts/pack); in-solve SIMD
// is a documented dead-end (sprint 11.1a, 6.6x slower).
//
// Parity design: the boundary solve + every spot-stencil price go through the SHARED
// K2 primitives (american_boundary_avx2_kernel.hpp) whose parity vs scalar
// andersen_lake is already gated; the Greek COMBINE arithmetic is done SCALAR per lane
// with the EXACT american_greeks_al formulas, so the ONLY laned-vs-scalar difference is
// the AVX2 transcendentals inside the 13 stencil prices (~1e-13 in price), amplified by
// the finite-difference denominators — the same economic-gate story as the K2 marks
// batch (no Greek is bit-identical to the scalar libm path; all within the documented
// gate). Any lane that is not genuine early-exercise on ALL five bump states, or whose
// r-hr crosses out of the American regime, is left for the scalar american_greeks_al
// patch (handled[l] = false) — exactly american_greeks_al's own all-or-nothing fallback.

#include "american_greeks_avx2.hpp"

#include "american_boundary_avx2_kernel.hpp"

#include "atx/vol/american.hpp" // AmericanGreeks, classify_regime, ExerciseRegime
#include "atx/vol/counters.hpp" // ledger::AlBoundarySolves — SIMD-invariant solve ledger

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "american_greeks_avx2.cpp requires -mavx2 -mfma (build via src/simd/*_avx2.cpp)"
#endif

#include <immintrin.h>

namespace atx::vol::simd::detail {

namespace {

// Solve one bump-state boundary for a pack and store the pack prices at the given spot
// offsets. `sig_state`/`r_state` are the per-lane bumped (sigma, r) the boundary is
// solved and priced at (matching american_greeks_al's px(...) which prices on each
// boundary at its own sigma/r). Returns per-lane eligibility (init Ok, American regime).
// `prices_at[k][l]` receives lane l's price at spot S[l] + spot_off[k].
struct SolveScratch {
    amer::AlBoundary bnd[4];
    amer::AlWorkspace ws[4];
};

} // namespace

void american_put_greeks_batch_avx2(const double* S, const double* K, const double* T,
                                    const double* sigma, const double* r, const double* q,
                                    std::size_t n, const std::optional<AlOpts>& opts,
                                    AmericanGreeks* out_greeks, bool* handled,
                                    GreekNeeds needs) noexcept {
    const amer::AlScheme sch = amer::scheme_from_opts(opts);
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    // K4 first-order tier: skip whole boundary solves the requested greeks don't need.
    // sigma+/- (2 solves) feed only vega/volga/vanna; r+/- (2 solves) only rho; the
    // wide S+/-2h stencils only charm's speed term. A hedge caller ({delta}) thus pays
    // 1 boundary solve instead of 5; a risk caller ({delta,vega}) pays 3.
    const bool need_vega = needs.vega;   // vega, volga, vanna
    const bool need_rho = needs.rho;
    const bool need_charm = needs.charm; // needs the 5-point speed stencil

    // Solve-ledger accounting (SIMD-invariance). This laned bundle performs the SAME
    // K4-tiered boundary solves as the scalar american_greeks_al oracle — base, plus
    // sigma+/- when vega is needed, plus r+/- when rho is needed (5/3/1) — but it drives
    // solve_put_boundary_pack_avx2, which lays its own BAW seed and therefore never
    // reaches al_seed_boundary's ledger tap. Without an explicit bump an AVX2-routed
    // bundle reads as ZERO boundary solves while doing identical work, silently voiding
    // the sprint's solve-economy gate (observed: full/risk/hedge 5/3/1 -> 0/0/0, and the
    // backtest ledger 12/12/6/6 -> 2/2/1/1). Mirrors the marks kernel's per-active-lane
    // bump in american_boundary_avx2.cpp. Bumped ONLY for lanes this kernel actually
    // completed: a lane left to the scalar patch is counted by that patch's own solves.
    const std::uint64_t bundle_solves =
        1u + (need_vega ? 2u : 0u) + (need_rho ? 2u : 0u);

    for (std::size_t base = 0; base < n; base += 4) {
        const std::size_t m = (n - base < 4) ? (n - base) : 4;
        for (std::size_t l = 0; l < m; ++l) {
            handled[base + l] = false;
        }

        // Per-lane FD steps (match american_greeks_al exactly).
        alignas(32) double Sl[4] = {1, 1, 1, 1}, Kl[4] = {1, 1, 1, 1};
        alignas(32) double Tl[4] = {1, 1, 1, 1}, sigl[4] = {0.2, 0.2, 0.2, 0.2};
        alignas(32) double rl[4] = {0.05, 0.05, 0.05, 0.05}, ql[4] = {0, 0, 0, 0};
        alignas(32) double hS[4] = {0, 0, 0, 0}, hv[4] = {0, 0, 0, 0}, hr[4] = {0, 0, 0, 0};
        // Bumped-state per-lane parameter arrays.
        alignas(32) double sig_p[4] = {0.2, 0.2, 0.2, 0.2}, sig_m[4] = {0.2, 0.2, 0.2, 0.2};
        alignas(32) double r_p[4] = {0.05, 0.05, 0.05, 0.05}, r_m[4] = {0.05, 0.05, 0.05, 0.05};
        bool lane_ok[4] = {false, false, false, false};
        for (std::size_t l = 0; l < m; ++l) {
            Sl[l] = S[base + l];
            Kl[l] = K[base + l];
            Tl[l] = T[base + l];
            sigl[l] = sigma[base + l];
            rl[l] = r[base + l];
            ql[l] = q[base + l];
            hS[l] = 1.0e-3 * Sl[l];
            hv[l] = (sigl[l] - 1.0e-3 <= 0.0) ? 0.5 * sigl[l] : 1.0e-3;
            hr[l] = 1.0e-4;
            sig_p[l] = sigl[l] + hv[l];
            sig_m[l] = sigl[l] - hv[l];
            r_p[l] = rl[l] + hr[l];
            r_m[l] = rl[l] - hr[l];
            // american_greeks_al eligibility: genuine early exercise (r>0, T, sigma) AND
            // r-hr>0 (else the down-rate stencil crosses out of the American regime and
            // the scalar bundle falls back to FD). Bump states stay American for r>hr.
            const bool amer_regime =
                atx::vol::detail::classify_regime(rl[l], ql[l]) ==
                atx::vol::detail::ExerciseRegime::American;
            lane_ok[l] = (Sl[l] > 0.0) && (Kl[l] > 0.0) && (Tl[l] > 1.0e-12) &&
                         (sigl[l] > 1.0e-8) && amer_regime && (rl[l] - hr[l] > 0.0);
        }

        // The 13 stencil prices (per lane), stored from the 4-wide solves/prices.
        alignas(32) double v0[4], vSp[4], vSm[4], vS2p[4], vS2m[4];
        alignas(32) double vvp[4], vSpVp[4], vSmVp[4];
        alignas(32) double vvm[4], vSpVm[4], vSmVm[4];
        alignas(32) double vrp[4], vrm[4];

        // Helper: build a per-lane spot vector S + off (off broadcast scalar).
        auto spot_vec = [&](double off) {
            alignas(32) double sp[4] = {Sl[0] + off * hS[0], Sl[1] + off * hS[1],
                                        Sl[2] + off * hS[2], Sl[3] + off * hS[3]};
            return _mm256_load_pd(sp);
        };

        SolveScratch sc;
        PutPackBoundary pk;
        bool elig[4];
        int ref = -1;

        // Base boundary (sigma, r): price 5 spot stencils.
        solve_put_boundary_pack_avx2(Sl, Kl, Tl, sigl, rl, ql, m, sch, sc.bnd, sc.ws, pk, elig, ref);
        for (std::size_t l = 0; l < m; ++l) {
            lane_ok[l] = lane_ok[l] && elig[l];
        }
        if (ref < 0) {
            continue; // no eligible lane in this pack; all patched by the caller.
        }
        _mm256_store_pd(v0, price_put_pack_avx2(pk, spot_vec(0.0)));
        _mm256_store_pd(vSp, price_put_pack_avx2(pk, spot_vec(+1.0)));
        _mm256_store_pd(vSm, price_put_pack_avx2(pk, spot_vec(-1.0)));
        if (need_charm) { // wide S+/-2h speed stencils feed only charm
            _mm256_store_pd(vS2p, price_put_pack_avx2(pk, spot_vec(+2.0)));
            _mm256_store_pd(vS2m, price_put_pack_avx2(pk, spot_vec(-2.0)));
        }

        if (need_vega) {
            // sigma+ boundary: price at S, S+hS, S-hS (vega, vanna+ leg).
            solve_put_boundary_pack_avx2(Sl, Kl, Tl, sig_p, rl, ql, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vvp, price_put_pack_avx2(pk, spot_vec(0.0)));
            _mm256_store_pd(vSpVp, price_put_pack_avx2(pk, spot_vec(+1.0)));
            _mm256_store_pd(vSmVp, price_put_pack_avx2(pk, spot_vec(-1.0)));

            // sigma- boundary.
            solve_put_boundary_pack_avx2(Sl, Kl, Tl, sig_m, rl, ql, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vvm, price_put_pack_avx2(pk, spot_vec(0.0)));
            _mm256_store_pd(vSpVm, price_put_pack_avx2(pk, spot_vec(+1.0)));
            _mm256_store_pd(vSmVm, price_put_pack_avx2(pk, spot_vec(-1.0)));
        }

        if (need_rho) {
            // r+ boundary: price at S.
            solve_put_boundary_pack_avx2(Sl, Kl, Tl, sigl, r_p, ql, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vrp, price_put_pack_avx2(pk, spot_vec(0.0)));

            // r- boundary: price at S.
            solve_put_boundary_pack_avx2(Sl, Kl, Tl, sigl, r_m, ql, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vrm, price_put_pack_avx2(pk, spot_vec(0.0)));
        }

        // ── Combine per lane — EXACT american_greeks_al formulas (scalar) ──
        for (std::size_t l = 0; l < m; ++l) {
            if (!lane_ok[l]) {
                continue; // caller patches via scalar american_greeks_al
            }
            const double hSl = hS[l], hvl = hv[l], hrl = hr[l];
            const double Sc = Sl[l], Kc = Kl[l], sc_ = sigl[l], rc = rl[l], qc = ql[l];
            AmericanGreeks g; // unrequested greeks stay 0 (the caller's mask skips them)
            g.price = v0[l];
            g.delta = (vSp[l] - vSm[l]) / (2.0 * hSl);
            g.gamma = (vSp[l] - 2.0 * v0[l] + vSm[l]) / (hSl * hSl);
            // theta rides the base boundary (v0/delta/gamma) via the continuation-region
            // PDE — always available, no extra solve. charm additionally needs speed.
            const double intr0 = Kc - Sc; // put intrinsic
            const bool exercised = (v0[l] <= intr0 + 1.0e-9 * Kc) && (intr0 > 0.0);
            g.theta = exercised ? 0.0
                                : rc * v0[l] - (rc - qc) * Sc * g.delta -
                                      0.5 * sc_ * sc_ * Sc * Sc * g.gamma;
            if (need_vega) {
                g.vega = (vvp[l] - vvm[l]) / (2.0 * hvl);
                g.volga = (vvp[l] - 2.0 * v0[l] + vvm[l]) / (hvl * hvl);
                g.vanna = (vSpVp[l] - vSpVm[l] - vSmVp[l] + vSmVm[l]) / (4.0 * hSl * hvl);
            }
            if (need_rho) {
                g.rho = (vrp[l] - vrm[l]) / (2.0 * hrl);
            }
            if (need_charm) {
                if (exercised) {
                    g.charm = 0.0;
                } else {
                    const double speed =
                        (vS2p[l] - 2.0 * vSp[l] + 2.0 * vSm[l] - vS2m[l]) / (2.0 * hSl * hSl * hSl);
                    g.charm = rc * g.delta - (rc - qc) * (g.delta + Sc * g.gamma) -
                              0.5 * sc_ * sc_ * (2.0 * Sc * g.gamma + Sc * Sc * speed);
                }
            }
            // Non-finite guard on the COMPUTED greeks only: fall back to the scalar patch
            // rather than store NaN in a requested column.
            bool ok = std::isfinite(g.price) && std::isfinite(g.delta) &&
                      std::isfinite(g.gamma) && std::isfinite(g.theta);
            if (need_vega) {
                ok = ok && std::isfinite(g.vega) && std::isfinite(g.volga) &&
                     std::isfinite(g.vanna);
            }
            if (need_rho) {
                ok = ok && std::isfinite(g.rho);
            }
            if (need_charm) {
                ok = ok && std::isfinite(g.charm);
            }
            if (!ok) {
                continue;
            }
            out_greeks[base + l] = g;
            handled[base + l] = true;
            counters::ledger::bump(counters::ledger::Solve::AlBoundarySolves, bundle_solves);
        }
        (void)kNaN;
    }
}

// ── P1b: laned ANALYTIC American-CALL Greeks bundle ─────────────────────────
//
// Call-native mirror of american_put_greeks_batch_avx2 via the McDonald-Schroder map
// C(S,K,r,q) = P(K,S,q,r). Per lane the internal put has: internal-strike = S (the call
// spot), internal-rate = q (the call yield), internal-yield = r (the call rate), priced
// at internal-spot = K (the fixed call strike). The five bump-state boundaries are solved
// exactly as for a put, only with the (rate,yield) swapped and the r± stencils bumping the
// internal YIELD (= r). A call spot stencil varies the internal STRIKE = S ± k·hS; by
// strike homogeneity the dimensionless boundary Y[] is strike-invariant and xmax rescales
// linearly (xmax(S2) = XMAX_base·S2/S), so one solve per bump state serves every stencil —
// price_put_pack_at_avx2 takes the (spot=K, strike=S2, xmax) triple. theta/charm come from
// the continuation-region PDE in the ORIGINAL (S,r,q) with the call intrinsic S-K, exactly
// as scalar american_greeks_al(...,Side::Call). Same economic gate as the put kernel: the
// only laned-vs-scalar difference is the AVX2 transcendentals in the stencil prices (plus
// the ~1 ULP xmax-rescale), amplified by the FD denominators.
void american_call_greeks_batch_avx2(const double* S, const double* K, const double* T,
                                     const double* sigma, const double* r, const double* q,
                                     std::size_t n, const std::optional<AlOpts>& opts,
                                     AmericanGreeks* out_greeks, bool* handled,
                                     GreekNeeds needs) noexcept {
    const amer::AlScheme sch = amer::scheme_from_opts(opts);
    const bool need_vega = needs.vega;
    const bool need_rho = needs.rho;
    const bool need_charm = needs.charm;

    // Solve-ledger accounting — see the put kernel above (same 5/3/1 K4 tier, same
    // per-completed-lane bump so the ledger stays SIMD-invariant).
    const std::uint64_t bundle_solves =
        1u + (need_vega ? 2u : 0u) + (need_rho ? 2u : 0u);

    for (std::size_t base = 0; base < n; base += 4) {
        const std::size_t m = (n - base < 4) ? (n - base) : 4;
        for (std::size_t l = 0; l < m; ++l) {
            handled[base + l] = false;
        }

        // Original CALL params per lane. Defaults (1,1,1,0.2,0.05,0.05) keep the tail
        // lanes (l >= m) numerically safe — Sl>0 so the xmax strike-rescale never divides
        // by zero even though the tail results are discarded.
        alignas(32) double Sl[4] = {1, 1, 1, 1}, Kl[4] = {1, 1, 1, 1};
        alignas(32) double Tl[4] = {1, 1, 1, 1}, sigl[4] = {0.2, 0.2, 0.2, 0.2};
        alignas(32) double rl[4] = {0.05, 0.05, 0.05, 0.05}, ql[4] = {0.05, 0.05, 0.05, 0.05};
        alignas(32) double hS[4] = {0, 0, 0, 0}, hv[4] = {0, 0, 0, 0}, hr[4] = {0, 0, 0, 0};
        alignas(32) double sig_p[4] = {0.2, 0.2, 0.2, 0.2}, sig_m[4] = {0.2, 0.2, 0.2, 0.2};
        // Internal-put yield (= call rate r) bumped ±hr for rho. The internal rate (= call
        // yield q) is NEVER bumped, so classify_regime stays American for every stencil.
        alignas(32) double yld_p[4] = {0.05, 0.05, 0.05, 0.05}, yld_m[4] = {0.05, 0.05, 0.05, 0.05};
        bool lane_ok[4] = {false, false, false, false};
        for (std::size_t l = 0; l < m; ++l) {
            Sl[l] = S[base + l];
            Kl[l] = K[base + l];
            Tl[l] = T[base + l];
            sigl[l] = sigma[base + l];
            rl[l] = r[base + l];
            ql[l] = q[base + l];
            hS[l] = 1.0e-3 * Sl[l];
            hv[l] = (sigl[l] - 1.0e-3 <= 0.0) ? 0.5 * sigl[l] : 1.0e-3;
            hr[l] = 1.0e-4;
            sig_p[l] = sigl[l] + hv[l];
            sig_m[l] = sigl[l] - hv[l];
            yld_p[l] = rl[l] + hr[l];
            yld_m[l] = rl[l] - hr[l];
            // American iff the internal-put short rate (= call yield q) > 0. The r±
            // stencils bump only the internal yield, so a call never crosses out of the
            // American regime the way a put's r-hr can (no rate-bump regime guard needed).
            const bool amer_regime =
                atx::vol::detail::classify_regime(ql[l], rl[l]) ==
                atx::vol::detail::ExerciseRegime::American;
            lane_ok[l] = (Sl[l] > 0.0) && (Kl[l] > 0.0) && (Tl[l] > 1.0e-12) &&
                         (sigl[l] > 1.0e-8) && amer_regime;
        }

        // The 13 stencil prices (per lane).
        alignas(32) double v0[4], vSp[4], vSm[4], vS2p[4], vS2m[4];
        alignas(32) double vvp[4], vSpVp[4], vSmVp[4];
        alignas(32) double vvm[4], vSpVm[4], vSmVm[4];
        alignas(32) double vrp[4], vrm[4];

        const __m256d Slv = _mm256_load_pd(Sl); // base internal-strike (= call spot)
        const __m256d Kcv = _mm256_load_pd(Kl); // internal spot (= call strike), fixed

        // Price a call spot stencil against a solved boundary: internal put at spot=K
        // (fixed), strike = S + koff·hS (per lane), xmax rescaled by strike homogeneity.
        auto price_call_stencil = [&](const PutPackBoundary& pk_b, double koff) -> __m256d {
            alignas(32) double s2[4] = {Sl[0] + koff * hS[0], Sl[1] + koff * hS[1],
                                        Sl[2] + koff * hS[2], Sl[3] + koff * hS[3]};
            const __m256d S2v = _mm256_load_pd(s2);
            const __m256d xmaxv = _mm256_mul_pd(pk_b.XMAX, _mm256_div_pd(S2v, Slv));
            return price_put_pack_at_avx2(pk_b, Kcv, S2v, xmaxv);
        };

        SolveScratch sc;
        PutPackBoundary pk;
        bool elig[4];
        int ref = -1;

        // Base boundary: internal-strike = S, rate = q (call yield), yield = r (call rate).
        solve_put_boundary_pack_avx2(Sl, Sl, Tl, sigl, ql, rl, m, sch, sc.bnd, sc.ws, pk, elig, ref);
        for (std::size_t l = 0; l < m; ++l) {
            lane_ok[l] = lane_ok[l] && elig[l];
        }
        if (ref < 0) {
            continue; // no eligible lane in this pack; all patched by the caller.
        }
        _mm256_store_pd(v0, price_call_stencil(pk, 0.0));
        _mm256_store_pd(vSp, price_call_stencil(pk, +1.0));
        _mm256_store_pd(vSm, price_call_stencil(pk, -1.0));
        if (need_charm) {
            _mm256_store_pd(vS2p, price_call_stencil(pk, +2.0));
            _mm256_store_pd(vS2m, price_call_stencil(pk, -2.0));
        }

        if (need_vega) {
            // sigma+ boundary (rate = q, yield = r, sigma+).
            solve_put_boundary_pack_avx2(Sl, Sl, Tl, sig_p, ql, rl, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vvp, price_call_stencil(pk, 0.0));
            _mm256_store_pd(vSpVp, price_call_stencil(pk, +1.0));
            _mm256_store_pd(vSmVp, price_call_stencil(pk, -1.0));

            // sigma- boundary.
            solve_put_boundary_pack_avx2(Sl, Sl, Tl, sig_m, ql, rl, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vvm, price_call_stencil(pk, 0.0));
            _mm256_store_pd(vSpVm, price_call_stencil(pk, +1.0));
            _mm256_store_pd(vSmVm, price_call_stencil(pk, -1.0));
        }

        if (need_rho) {
            // r+ boundary: internal yield = r + hr (rate = q unchanged). Price at base S.
            solve_put_boundary_pack_avx2(Sl, Sl, Tl, sigl, ql, yld_p, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vrp, price_call_stencil(pk, 0.0));

            // r- boundary: internal yield = r - hr.
            solve_put_boundary_pack_avx2(Sl, Sl, Tl, sigl, ql, yld_m, m, sch, sc.bnd, sc.ws, pk, elig, ref);
            for (std::size_t l = 0; l < m; ++l) {
                lane_ok[l] = lane_ok[l] && elig[l];
            }
            _mm256_store_pd(vrm, price_call_stencil(pk, 0.0));
        }

        // ── Combine per lane — EXACT american_greeks_al(Side::Call) formulas (scalar) ──
        for (std::size_t l = 0; l < m; ++l) {
            if (!lane_ok[l]) {
                continue; // caller patches via scalar american_greeks_al
            }
            const double hSl = hS[l], hvl = hv[l], hrl = hr[l];
            const double Sc = Sl[l], Kc = Kl[l], sc_ = sigl[l], rc = rl[l], qc = ql[l];
            AmericanGreeks g;
            g.price = v0[l];
            g.delta = (vSp[l] - vSm[l]) / (2.0 * hSl);
            g.gamma = (vSp[l] - 2.0 * v0[l] + vSm[l]) / (hSl * hSl);
            // theta / charm from the continuation-region PDE in the ORIGINAL (S,r,q); call
            // intrinsic S-K. Side-agnostic given the correct call V/delta/gamma/speed.
            const double intr0 = Sc - Kc; // call intrinsic
            const bool exercised = (v0[l] <= intr0 + 1.0e-9 * Kc) && (intr0 > 0.0);
            g.theta = exercised ? 0.0
                                : rc * v0[l] - (rc - qc) * Sc * g.delta -
                                      0.5 * sc_ * sc_ * Sc * Sc * g.gamma;
            if (need_vega) {
                g.vega = (vvp[l] - vvm[l]) / (2.0 * hvl);
                g.volga = (vvp[l] - 2.0 * v0[l] + vvm[l]) / (hvl * hvl);
                g.vanna = (vSpVp[l] - vSpVm[l] - vSmVp[l] + vSmVm[l]) / (4.0 * hSl * hvl);
            }
            if (need_rho) {
                g.rho = (vrp[l] - vrm[l]) / (2.0 * hrl);
            }
            if (need_charm) {
                if (exercised) {
                    g.charm = 0.0;
                } else {
                    const double speed =
                        (vS2p[l] - 2.0 * vSp[l] + 2.0 * vSm[l] - vS2m[l]) / (2.0 * hSl * hSl * hSl);
                    g.charm = rc * g.delta - (rc - qc) * (g.delta + Sc * g.gamma) -
                              0.5 * sc_ * sc_ * (2.0 * Sc * g.gamma + Sc * Sc * speed);
                }
            }
            bool ok = std::isfinite(g.price) && std::isfinite(g.delta) &&
                      std::isfinite(g.gamma) && std::isfinite(g.theta);
            if (need_vega) {
                ok = ok && std::isfinite(g.vega) && std::isfinite(g.volga) &&
                     std::isfinite(g.vanna);
            }
            if (need_rho) {
                ok = ok && std::isfinite(g.rho);
            }
            if (need_charm) {
                ok = ok && std::isfinite(g.charm);
            }
            if (!ok) {
                continue;
            }
            out_greeks[base + l] = g;
            handled[base + l] = true;
            counters::ledger::bump(counters::ledger::Solve::AlBoundarySolves, bundle_solves);
        }
    }
}

} // namespace atx::vol::simd::detail
