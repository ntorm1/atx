// Parity + ISA-seam gates for the AVX2 American-put boundary batch (P3.1/P3.2).
//
// american_put_boundary_batch vectorizes the Andersen-Lake exercise-boundary
// solve across 4 INDEPENDENT puts. These tests assert:
//   * the AVX2 4-lane prices reproduce the scalar cold andersen_lake to the P3
//     accuracy gate — the controller's default-shift policy sets the NORMAL-domain
//     immateriality bound at ~1e-6 USD (the FastDeterministic vector Φ/log/exp are
//     bounded at ~6.4e-7 here; libm-level 1e-8 is a T14 vector-math concern), and
//     ≤1e-3 USD on the stress grid — cross-checked vs a PDE oracle on the stress
//     grid;
//   * the P3.1 ISA-override seam works — ForceScalar runs the scalar path
//     BIT-IDENTICAL to andersen_lake (illegal-instruction-clean on any host), and
//     ForceAvx2 (AVX2 hosts) matches scalar to the gate;
//   * degenerate / non-American / collapse / deep-wing lanes patch to the exact
//     scalar value, and every n % 4 tail residue is handled.
//
// On a non-AVX2 host the AVX2-specific tests SKIP; the ForceScalar test still
// runs (identity check) so the scalar route is always exercised.

#include "atx/vol/simd/american_boundary_batch.hpp"

#include "atx/vol/american.hpp"
#include "atx/vol/simd/cpu.hpp"
#include "support/oracle_pricer_pde.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// Normal-domain AVX2-vs-scalar immateriality gate (USD). The controller's
// default-shift policy treats a ~1e-6 price shift as immaterial; the T13-default
// FastDeterministic vector transcendentals (detail/vector_math.hpp, ~1e-11
// interior) leave a measured max |AVX2−scalar| ≈ 6.4e-7 on this grid, compounded
// through the iterative boundary sweep. Tightening toward libm's 1e-8 is the T14
// vector-math bakeoff, not a T13 gate. Deterministic per ISA, so this bound is
// stable across Debug/Release.
inline constexpr double kNormalGate = 1e-6;

// A4 [S1] single-source-Φ regression lock. Before A4 the AVX2 boundary Φ was a
// degree-48 Chebyshev–Clenshaw Φ, leaving max |AVX2−scalar| ≈ 6.7e-9 (normal)
// / 3.3e-9 (stress) — Φ's ~1e-11 interior error compounded through the sweep.
// Migrating to the full-range Cody rational-erfc norm_cdf_erfc_pd2 — the SAME
// erfc the scalar andersen_lake reference evaluates — single-sources Φ and drops
// the gap ~4 orders to ~1e-13 (now bounded by log_pd/exp_pd ULP, not Φ). This
// bound is RED on the retired Chebyshev path (6.7e-9 > 1e-10) and GREEN on erfc
// (~4e-13, ~240× headroom), so it proves the accuracy-improving swap AND guards
// against any regression back to a coarser Φ. Class: accuracy-improving.
inline constexpr double kErfcSingleSourceGate = 1e-10;

// SoA batch of American puts.
struct PutBatch {
    std::vector<double> S, K, T, sigma, r, q;
    [[nodiscard]] std::size_t size() const { return S.size(); }
    void push(double s, double k, double t, double v, double rr, double qq) {
        S.push_back(s); K.push_back(k); T.push_back(t);
        sigma.push_back(v); r.push_back(rr); q.push_back(qq);
    }
};

// Reference: scalar cold andersen_lake put price (NaN on error).
double ref_put(double S, double K, double T, double sigma, double r, double q) {
    const Result<double> res = andersen_lake(S, K, T, sigma, r, q, Side::Put);
    return res.has_value() ? *res : std::numeric_limits<double>::quiet_NaN();
}

// A broad NORMAL-domain grid: American puts (r>0), moderate moneyness / tenor /
// vol, no deep wings, no degeneracies. This is where the kNormalGate applies.
PutBatch normal_grid() {
    PutBatch b;
    const double spots[] = {50.0, 100.0, 200.0};
    const double money[] = {0.75, 0.9, 1.0, 1.1, 1.3};
    const double tenors[] = {0.08, 0.25, 1.0, 2.0};
    const double vols[] = {0.12, 0.25, 0.45};
    const double rates[] = {0.02, 0.05};
    const double yields[] = {0.0, 0.03};
    for (double S : spots)
        for (double m : money)
            for (double T : tenors)
                for (double v : vols)
                    for (double rr : rates)
                        for (double qq : yields)
                            b.push(S, S * m, T, v, rr, qq);
    return b;
}

// Stress grid: deep wings, near-expiry, σ corners, r/q corners, plus explicit
// degenerate / European (non-American) lanes that MUST patch to scalar exactly.
PutBatch stress_grid() {
    PutBatch b;
    const double S = 100.0;
    // Deep ITM / OTM.
    b.push(S, 20.0, 1.0, 0.20, 0.05, 0.0);
    b.push(S, 400.0, 1.0, 0.20, 0.05, 0.0);
    b.push(S, 25.0, 0.5, 0.60, 0.03, 0.0);
    b.push(S, 300.0, 0.5, 0.60, 0.03, 0.0);
    // Near-expiry.
    b.push(S, 95.0, 1.0 / 365.0, 0.25, 0.05, 0.0);
    b.push(S, 105.0, 1.0 / 365.0, 0.25, 0.05, 0.02);
    b.push(S, 100.0, 3.0 / 365.0, 0.80, 0.05, 0.0);
    // σ corners.
    b.push(S, 100.0, 1.0, 0.02, 0.05, 0.0);
    b.push(S, 100.0, 1.0, 1.50, 0.05, 0.0);
    b.push(S, 110.0, 0.5, 2.50, 0.04, 0.01);
    // r / q corners (still American: r>0).
    b.push(S, 100.0, 1.0, 0.30, 0.001, 0.20);
    b.push(S, 100.0, 1.0, 0.30, 0.20, 0.0);
    b.push(S, 120.0, 2.0, 0.30, 0.15, 0.18);
    // Degenerate (patched to intrinsic) and European (r≤0, patched to scalar).
    b.push(S, 105.0, 0.0, 0.20, 0.05, 0.0);       // T=0
    b.push(S, 105.0, 1.0, 0.0, 0.05, 0.0);        // σ=0
    b.push(S, 105.0, 1.0, 0.20, 0.0, 0.03);       // r=0 → European
    b.push(S, 95.0, 1.0, 0.20, -0.01, 0.03);      // r<0 → European
    return b;
}

// RAII: restore Auto dispatch after a test twiddles the override.
struct IsaGuard {
    ~IsaGuard() { simd::set_simd_isa_override(simd::SimdIsa::Auto); }
};

std::vector<double> run_batch(const PutBatch& b) {
    std::vector<double> out(b.size(), 0.0);
    simd::american_put_boundary_batch(b.S.data(), b.K.data(), b.T.data(),
                                      b.sigma.data(), b.r.data(), b.q.data(),
                                      out.data(), b.size());
    return out;
}

// ── Accuracy: normal domain ≤ kNormalGate vs scalar cold path ────────────
TEST(AvxBoundary, MatchesScalar_NormalDomain) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host";
    }
    IsaGuard g;
    simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);

    const PutBatch b = normal_grid();
    const std::vector<double> got = run_batch(b);

    double max_abs = 0.0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double want = ref_put(b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i]);
        ASSERT_TRUE(std::isfinite(want)) << "i=" << i;
        const double e = std::abs(got[i] - want);
        max_abs = std::max(max_abs, e);
        EXPECT_LE(e, kNormalGate) << "i=" << i << " got=" << got[i] << " want=" << want;
    }
    RecordProperty("max_abs_gap_normal_e12", static_cast<int>(max_abs * 1e12));
    std::printf("[AvxBoundary] normal-domain max |AVX2-scalar| = %.3e (n=%zu)\n",
                max_abs, b.size());
}

// ── Accuracy: stress grid ≤ 1e-3 vs scalar, cross-checked vs PDE ──────────
TEST(AvxBoundary, StressGrid_WithinTol) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host";
    }
    IsaGuard g;
    simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);

    const PutBatch b = stress_grid();
    const std::vector<double> got = run_batch(b);

    double max_abs = 0.0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double want = ref_put(b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i]);
        if (!std::isfinite(want)) {
            continue; // unsupported corner: scalar itself errored
        }
        const double e = std::abs(got[i] - want);
        max_abs = std::max(max_abs, e);
        EXPECT_LE(e, 1e-3) << "i=" << i << " got=" << got[i] << " want=" << want;
    }
    RecordProperty("max_abs_gap_stress_e9", static_cast<int>(max_abs * 1e9));
    std::printf("[AvxBoundary] stress-grid max |AVX2-scalar| = %.3e\n", max_abs);

    // PDE cross-check on a few clean American-put points: the AVX2 batch must be
    // as close to the finite-difference oracle as the scalar pricer is.
    struct Pt { double S, K, T, sig, r, q; };
    const Pt pts[] = {
        {100.0, 100.0, 1.0, 0.25, 0.05, 0.0},
        {100.0, 90.0, 0.5, 0.30, 0.06, 0.02},
        {100.0, 110.0, 1.0, 0.20, 0.04, 0.0},
    };
    for (const Pt& p : pts) {
        std::vector<double> S{p.S}, K{p.K}, T{p.T}, sg{p.sig}, r{p.r}, q{p.q};
        std::vector<double> out(1, 0.0);
        simd::american_put_boundary_batch(S.data(), K.data(), T.data(), sg.data(),
                                          r.data(), q.data(), out.data(), 1);
        // n=1 → scalar tail path; force a full pack of 4 identical to exercise AVX2.
        std::vector<double> S4(4, p.S), K4(4, p.K), T4(4, p.T), sg4(4, p.sig),
            r4(4, p.r), q4(4, p.q), out4(4, 0.0);
        simd::american_put_boundary_batch(S4.data(), K4.data(), T4.data(),
                                          sg4.data(), r4.data(), q4.data(),
                                          out4.data(), 4);
        const double pde =
            test::oracle_pde_american(p.S, p.K, p.T, p.sig, p.r, p.q, Side::Put);
        ASSERT_TRUE(std::isfinite(pde));
        EXPECT_LE(std::abs(out4[0] - pde), 5e-3)
            << "AVX2 vs PDE K=" << p.K << " got=" << out4[0] << " pde=" << pde;
    }
}

// ── A4 [S1]: single-source Cody-erfc Φ tightens boundary parity ~4 orders ──
TEST(AvxBoundary, ErfcSingleSource_TightParity) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host";
    }
    IsaGuard g;
    simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);

    auto max_gap = [](const PutBatch& b) {
        const std::vector<double> got = run_batch(b);
        double m = 0.0;
        for (std::size_t i = 0; i < b.size(); ++i) {
            const double want = ref_put(b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i]);
            if (std::isfinite(want)) {
                m = std::max(m, std::abs(got[i] - want));
            }
        }
        return m;
    };

    const double gap_normal = max_gap(normal_grid());
    const double gap_stress = max_gap(stress_grid());
    std::printf("[AvxBoundary] erfc single-source max |AVX2-scalar|: normal=%.3e stress=%.3e\n",
                gap_normal, gap_stress);
    // RED on the retired Chebyshev Φ (~6.7e-9 / ~3.3e-9); GREEN on erfc (~4e-13 / ~1e-13).
    EXPECT_LE(gap_normal, kErfcSingleSourceGate) << "normal-grid parity regressed vs erfc Φ";
    EXPECT_LE(gap_stress, kErfcSingleSourceGate) << "stress-grid parity regressed vs erfc Φ";
}

// ── P3.1 force seam: ForceScalar is bit-identical + Scalar route ──────────
TEST(AvxBoundary, ForceScalar_NoAvx) {
    IsaGuard g;
    simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);

    const PutBatch b = stress_grid();
    std::vector<double> out(b.size(), 0.0);
    const simd::SimdRoute route = simd::american_put_boundary_batch(
        b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(),
        b.q.data(), out.data(), b.size());
    EXPECT_EQ(route, simd::SimdRoute::Scalar);

    // Scalar route == per-contract andersen_lake, bit-for-bit.
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double want = ref_put(b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i]);
        if (std::isnan(want)) {
            EXPECT_TRUE(std::isnan(out[i])) << "i=" << i;
        } else {
            EXPECT_EQ(out[i], want) << "i=" << i; // bit-identical
        }
    }
}

// ── P3.1 force seam: ForceAvx2 routes to AVX2 and matches scalar ──────────
TEST(AvxBoundary, ForceAvx2_MatchesScalar) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host";
    }
    IsaGuard g;
    simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);

    const PutBatch b = normal_grid();
    std::vector<double> out(b.size(), 0.0);
    const simd::SimdRoute route = simd::american_put_boundary_batch(
        b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(),
        b.q.data(), out.data(), b.size());
    EXPECT_EQ(route, simd::SimdRoute::Avx2);

    for (std::size_t i = 0; i < b.size(); ++i) {
        const double want = ref_put(b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i]);
        EXPECT_LE(std::abs(out[i] - want), kNormalGate) << "i=" << i;
    }
}

// ── Tail residues: every n % 4 ∈ {0,1,2,3} handled ───────────────────────
TEST(AvxBoundary, TailResidues) {
    IsaGuard g;
    // Under whatever the host supports (Auto) plus an explicit ForceAvx2 pass.
    const PutBatch full = normal_grid();
    for (const simd::SimdIsa mode :
         {simd::SimdIsa::ForceScalar, simd::SimdIsa::ForceAvx2}) {
        if (mode == simd::SimdIsa::ForceAvx2 && !simd::have_avx2()) {
            continue;
        }
        simd::set_simd_isa_override(mode);
        for (std::size_t n = 1; n <= 13; ++n) {
            std::vector<double> out(n, 0.0);
            simd::american_put_boundary_batch(full.S.data(), full.K.data(),
                                              full.T.data(), full.sigma.data(),
                                              full.r.data(), full.q.data(),
                                              out.data(), n);
            for (std::size_t i = 0; i < n; ++i) {
                const double want = ref_put(full.S[i], full.K[i], full.T[i],
                                            full.sigma[i], full.r[i], full.q[i]);
                // ForceScalar is bit-identical; ForceAvx2 within the immateriality gate.
                const double gate =
                    (mode == simd::SimdIsa::ForceScalar) ? 0.0 : kNormalGate;
                EXPECT_LE(std::abs(out[i] - want), gate)
                    << "mode=" << static_cast<int>(mode) << " n=" << n << " i=" << i;
            }
        }
    }
}

TEST(AvxBoundary, ZeroLengthIsNoOp) {
    IsaGuard g;
    double sentinel = 42.0;
    simd::american_put_boundary_batch(nullptr, nullptr, nullptr, nullptr, nullptr,
                                      nullptr, &sentinel, 0);
    EXPECT_EQ(sentinel, 42.0);
}

// ── Speedup: homogeneous batch, AVX2 vs scalar (measured on build-rel) ────
// Debug builds only sanity-check parity + record the ratio; the ≥2.0× gate is
// evaluated from the Release (build-rel) run of this same test.
TEST(AvxBoundary, Speedup) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host";
    }
    IsaGuard g;

    // A large HOMOGENEOUS batch (same scheme/side): ~ATM American puts, tiny
    // per-lane variation so every lane takes the full sweep budget.
    constexpr std::size_t kN = 4096;
    PutBatch b;
    for (std::size_t i = 0; i < kN; ++i) {
        const double m = 0.85 + 0.30 * static_cast<double>(i % 31) / 31.0;
        const double v = 0.15 + 0.25 * static_cast<double>(i % 17) / 17.0;
        b.push(100.0, 100.0 * m, 1.0, v, 0.05, 0.01);
    }
    std::vector<double> out_s(kN, 0.0), out_v(kN, 0.0);

    auto timeit = [&](simd::SimdIsa mode, std::vector<double>& out) {
        simd::set_simd_isa_override(mode);
        // warm
        simd::american_put_boundary_batch(b.S.data(), b.K.data(), b.T.data(),
                                          b.sigma.data(), b.r.data(), b.q.data(),
                                          out.data(), kN);
        constexpr int reps = 3;
        const auto t0 = std::chrono::steady_clock::now();
        for (int rr = 0; rr < reps; ++rr) {
            simd::american_put_boundary_batch(b.S.data(), b.K.data(), b.T.data(),
                                              b.sigma.data(), b.r.data(),
                                              b.q.data(), out.data(), kN);
        }
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(t1 - t0).count() / reps;
    };

    const double ts = timeit(simd::SimdIsa::ForceScalar, out_s);
    const double tv = timeit(simd::SimdIsa::ForceAvx2, out_v);
    const double speedup = ts / tv;

    double max_abs = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        max_abs = std::max(max_abs, std::abs(out_s[i] - out_v[i]));
    }
    std::printf("[AvxBoundary] speedup=%.3fx  scalar=%.3f ms  avx2=%.3f ms  "
                "max|Δ|=%.3e  (n=%zu)\n",
                speedup, ts * 1e3, tv * 1e3, max_abs, kN);
    RecordProperty("speedup_milli", static_cast<int>(speedup * 1000));

    // Parity always holds; the ratio is informational at Debug (gate on build-rel).
    EXPECT_LE(max_abs, kNormalGate);
    EXPECT_GT(speedup, 0.0);
}

} // namespace
} // namespace atx::vol
