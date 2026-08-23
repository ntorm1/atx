// UB / NaN-safety gates for the hand-written AVX2 kernels (lane L4).
//
// This repo builds no sanitizer, no clang-tidy and no static analysis (see
// .agents/cpp/agent.md §8), so the classes of defect a sanitizer would have caught in
// src/simd/** have to be pinned by hand. Every test below covers a path that had never
// been exercised: a non-finite argument reaching a vector transcendental, an exact hit
// on a Chebyshev collocation node, a bump-state solve that refused, and a slice whose
// time axis was never validated.
//
// The shared property under test is NOT "does it produce a NaN". It is:
//
//   a kernel that cannot answer must not answer with a FINITE number.
//
// A finite wrong value is strictly worse than a NaN here, because every downstream
// guard in this tree (nonfinite_mask, std::isfinite patch-outs, the eligibility masks)
// tests finiteness and nothing else. exp_pd(NaN) returning -2.0 — which it did — is
// invisible to all of them.
//
// AVX2 IS REQUIRED. These kernels only exist on the vector path; on a host without
// AVX2 the suites below SKIP and prove nothing. The skip messages say so.

#include "simd/american_boundary_avx2.hpp" // bary_eval_pack_avx2 test probe
#include "simd/essvi_batch.hpp"
#include "simd/vector_math_probe.hpp"

#include "atx/vol/api/fitting/vol_surface.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp"
#include "atx/vol/api/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// RAII: restore Auto dispatch after a test pins the ISA.
struct IsaGuard {
    ~IsaGuard() { simd::set_simd_isa_override(simd::SimdIsa::Auto); }
};

// ── T1: exp_pd / norm_pdf_pd must not answer a non-finite argument with a number ──
//
// exp_pd clamps with _mm256_max_pd(x, kExpLo) / _mm256_min_pd(·, kExpHi). x86 MAXPD /
// MINPD return their SECOND operand whenever EITHER operand is NaN, so with the
// argument first a NaN lane was replaced by the clamp bound: N = -1075, biased = -52,
// and the 2^N reconstruction _mm256_slli_epi64(-52, 52) rebuilt the double -2.0. The
// separate `underflow` compare could not see it — _CMP_LT_OQ is false for NaN.
//
// Consequence at the surface: φ(NaN) came back as 0.3989·(−2.0) = −0.7979 — a finite
// NEGATIVE probability density. Both are reachable from the American boundary kernel
// (res_deriv / eqn_b_ND pass unmasked tau- and b-derived arguments) and from the Cody
// erfc Φ.
TEST(SimdNanSafety, ExpPd_NonFiniteArgument_IsNotFinite) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — exp_pd never runs, this proves nothing";
    }
    const std::vector<double> x = {kNaN, -kNaN, kNaN, kNaN};
    std::vector<double> got(x.size(), 0.0);
    simd::fd_exp_batch(x.data(), got.data(), x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        EXPECT_TRUE(std::isnan(got[i]))
            << "exp_pd(NaN) returned the FINITE value " << got[i] << " at lane " << i;
    }
}

TEST(SimdNanSafety, NormPdfPd_NonFiniteArgument_IsNotFinite) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — norm_pdf_pd never runs, proves nothing";
    }
    const std::vector<double> x = {kNaN, 0.0, kNaN, 1.0};
    std::vector<double> got(x.size(), 0.0);
    simd::fd_norm_pdf_batch(x.data(), got.data(), x.size());
    EXPECT_TRUE(std::isnan(got[0])) << "phi(NaN) returned " << got[0];
    EXPECT_TRUE(std::isnan(got[2])) << "phi(NaN) returned " << got[2];
    // A density is never negative — the pre-fix failure mode was exactly -0.7979.
    EXPECT_FALSE(got[0] < 0.0);
    EXPECT_FALSE(got[2] < 0.0);
    // The finite neighbours in the same pack are untouched.
    EXPECT_NEAR(got[1], 0.3989422804014327, 1e-15);
    EXPECT_NEAR(got[3], 0.24197072451914337, 1e-15);
}

// The finite domain is BIT-IDENTICAL to before the NaN fix: the clamp operand swap
// only changes which operand MAXPD/MINPD returns when one of them is NaN, and for
// equal finite operands both orders return the same bits.
TEST(SimdNanSafety, ExpPd_FiniteDomain_MatchesLibmClosely) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — exp_pd never runs, this proves nothing";
    }
    std::vector<double> x;
    for (double v = -745.0; v <= 700.0; v += 0.25) {
        x.push_back(v);
    }
    // The exact clamp bounds and their neighbourhood — the lanes the operand swap
    // touches if it touches anything.
    x.push_back(-745.13321910194);
    x.push_back(-708.3964185322641);
    x.push_back(0.0);
    x.push_back(-0.0);
    std::vector<double> got(x.size(), 0.0);
    simd::fd_exp_batch(x.data(), got.data(), x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double want = std::exp(x[i]);
        // Below ln(DBL_MIN) exp_pd flushes to exactly 0 by design (the 2^N
        // reconstruction cannot build a denormal) — that contract is documented at
        // exp_pd and predates this lane.
        if (x[i] < -708.3964185322641) {
            EXPECT_EQ(got[i], 0.0) << "x=" << x[i];
        } else {
            EXPECT_LE(std::fabs(got[i] - want) / want, 1e-13) << "x=" << x[i];
        }
    }
    // +/-inf keep IEEE exp semantics.
    const std::vector<double> ends = {kInf, -kInf, kInf, -kInf};
    std::vector<double> eg(4, 0.0);
    simd::fd_exp_batch(ends.data(), eg.data(), 4);
    EXPECT_EQ(eg[0], kInf);
    EXPECT_EQ(eg[1], 0.0);
}

// ── T2: the AVX2 barycentric interpolant at an EXACT collocation node ────────────
//
// The scalar al_cheb_eval_t (american.cpp) and bary_eval (boundary_interp.cpp) both
// early-return y[i] when zq - z[i] == 0. The two AVX2 copies divided by that zero, so
// qq = +/-inf and num/den = NaN — and the caller's b_from_y_pd(NaN) then quietly
// degraded to XMAX, because _mm256_max_pd(NaN, 0) returns its second operand. A finite
// wrong boundary, invisible downstream.
//
// It is reachable: clamp01() saturates to EXACTLY +/-1.0 and the Chebyshev-Lobatto
// grid's endpoints ARE exactly -/+1.0 (al_cheb_node returns the literals).
namespace {

// Chebyshev-Lobatto nodes + second-kind barycentric weights, matching al_cheb_node.
void lobatto(unsigned n, std::vector<double>& z, std::vector<double>& w) {
    constexpr double kPi = 3.14159265358979323846;
    z.assign(n, 0.0);
    w.assign(n, 0.0);
    for (unsigned i = 0; i < n; ++i) {
        if (i == 0) {
            z[i] = -1.0;
        } else if (i == n - 1) {
            z[i] = 1.0;
        } else {
            z[i] = -std::cos(kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        }
        const double half = (i == 0 || i == n - 1) ? 0.5 : 1.0;
        w[i] = ((i % 2u) == 0u ? 1.0 : -1.0) * half;
    }
}

// Scalar reference — the exact scheme american.cpp's al_cheb_eval_t<0> uses.
double bary_eval_ref(const std::vector<double>& z, const std::vector<double>& w,
                     const std::vector<double>& y, double zq) {
    double num = 0.0;
    double den = 0.0;
    for (std::size_t i = 0; i < z.size(); ++i) {
        const double dz = zq - z[i];
        if (dz == 0.0) {
            return y[i];
        }
        const double qq = w[i] / dz;
        num += qq * y[i];
        den += qq;
    }
    return num / den;
}

} // namespace

TEST(SimdNanSafety, BaryEvalPd_ExactNodeHit_ReturnsNodeValue) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — the vector interpolant never runs";
    }
    constexpr unsigned kN = 7;
    std::vector<double> z, w;
    lobatto(kN, z, w);
    const std::vector<double> y = {0.0, 0.11, 0.27, 0.41, 0.58, 0.73, 0.94};
    ASSERT_EQ(y.size(), kN);

    // Lane 0: the RIGHT endpoint, the value clamp01() saturates a too-large zz to.
    // Lane 1: the LEFT endpoint, what clamp01() produces from a NaN or negative zz.
    // Lane 2: an interior node.
    // Lane 3: a genuine interior query — must stay bit-identical to the old kernel.
    const double zq[4] = {1.0, -1.0, z[3], 0.317};
    double got[4] = {0.0, 0.0, 0.0, 0.0};
    simd::detail::bary_eval_pack_avx2(z.data(), w.data(), y.data(), kN, zq, got);

    EXPECT_EQ(got[0], y[kN - 1]) << "z == +1.0 hits the last Lobatto node";
    EXPECT_EQ(got[1], y[0]) << "z == -1.0 hits the first Lobatto node";
    EXPECT_EQ(got[2], y[3]) << "an interior node must return its own value";
    // The non-hit lane runs the unmodified division chain.
    EXPECT_EQ(got[3], bary_eval_ref(z, w, y, 0.317));
}

// Every node of every supported grid size, one at a time — no size may hit a node
// with an unguarded divide.
TEST(SimdNanSafety, BaryEvalPd_EveryNodeOfEveryGrid_IsFinite) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — the vector interpolant never runs";
    }
    for (unsigned n = 3; n <= 13; n += 2) {
        std::vector<double> z, w;
        lobatto(n, z, w);
        std::vector<double> y(n, 0.0);
        for (unsigned i = 0; i < n; ++i) {
            y[i] = 0.05 * static_cast<double>(i) + 0.01;
        }
        for (unsigned j = 0; j < n; ++j) {
            const double zq[4] = {z[j], z[j], z[j], z[j]};
            double got[4] = {0.0, 0.0, 0.0, 0.0};
            simd::detail::bary_eval_pack_avx2(z.data(), w.data(), y.data(), n, zq, got);
            for (int l = 0; l < 4; ++l) {
                EXPECT_EQ(got[l], y[j]) << "n=" << n << " j=" << j << " lane=" << l;
            }
        }
    }
}

// ── T2 (end to end): a pack that reaches the exact-node case through the solver ──
//
// A lane whose T sits just above the 1e-12 degeneracy floor puts the first interior
// collocation node at tau <= 1e-14, so the sweep substitutes safe_tau = 1.0 and the
// quadrature's u_eff = min(0.5(1+xs), T) saturates at T — giving zz == +1.0 exactly.
// The AVX2 route must still land on the scalar andersen_lake answer.
TEST(SimdNanSafety, AmericanBoundaryBatch_TinyTenor_MatchesScalarRoute) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — the AVX2 boundary route never runs";
    }
    IsaGuard guard;
    const std::vector<double> S = {100.0, 100.0, 95.0, 105.0};
    const std::vector<double> K = {100.0, 110.0, 100.0, 100.0};
    const std::vector<double> T = {1.1e-12, 2.0e-12, 1.5e-12, 5.0e-12};
    const std::vector<double> sg = {0.25, 0.25, 0.35, 0.15};
    const std::vector<double> r = {0.05, 0.05, 0.05, 0.05};
    const std::vector<double> q = {0.01, 0.01, 0.01, 0.01};
    const std::size_t n = S.size();

    simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
    std::vector<double> want(n, 0.0);
    ASSERT_EQ(simd::american_put_boundary_batch(S.data(), K.data(), T.data(), sg.data(),
                                                r.data(), q.data(), want.data(), n),
              simd::SimdRoute::Scalar);

    simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);
    std::vector<double> got(n, 0.0);
    ASSERT_EQ(simd::american_put_boundary_batch(S.data(), K.data(), T.data(), sg.data(),
                                                r.data(), q.data(), got.data(), n),
              simd::SimdRoute::Avx2);

    for (std::size_t i = 0; i < n; ++i) {
        ASSERT_TRUE(std::isfinite(got[i])) << "i=" << i << " AVX2 price " << got[i];
        EXPECT_NEAR(got[i], want[i], 1e-9 * K[i]) << "i=" << i;
    }
}

// ── T3: a bump-state boundary solve that refuses must not be priced ──────────────
//
// solve_put_boundary_pack_avx2 returns EARLY, without writing `out`, when no lane of
// the pack is eligible. Only the BASE solve checked that; the four bump-state solves
// did not, so price_put_pack_avx2 then priced the sigma+/- and r+/- stencils against
// the stale BASE boundary and stored base-sigma prices into vvp/vvm/vrp/vrm.
//
// sigma just above the kernel's 1e-8 floor makes hv = 0.5*sigma, so sigma - hv lands
// AT 0.5*sigma <= 1e-8 and every lane of the sigma- solve refuses. The bundle must
// still equal the scalar american_greeks_al oracle.
TEST(SimdNanSafety, AmericanPutGreeksBatch_RefusedBumpSolve_MatchesScalarOracle) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — the laned greeks route never runs";
    }
    IsaGuard guard;
    // 1.1e-8 clears the lane_ok sigma > 1e-8 test; sig_m = 5.5e-9 does not.
    const std::vector<double> S = {100.0, 100.0, 100.0, 100.0};
    const std::vector<double> K = {100.0, 105.0, 95.0, 110.0};
    const std::vector<double> T = {1.0, 0.5, 2.0, 0.25};
    const std::vector<double> sg = {1.1e-8, 1.2e-8, 1.5e-8, 1.05e-8};
    const std::vector<double> r = {0.05, 0.05, 0.05, 0.05};
    const std::vector<double> q = {0.01, 0.01, 0.01, 0.01};
    const std::size_t n = S.size();

    std::vector<AmericanGreeks> got(n);
    simd::american_put_greeks_batch(S.data(), K.data(), T.data(), sg.data(), r.data(),
                                    q.data(), n, std::nullopt, got.data(),
                                    simd::SimdIsa::ForceAvx2);

    for (std::size_t i = 0; i < n; ++i) {
        const Result<AmericanGreeks> want =
            american_greeks_al(S[i], K[i], T[i], sg[i], r[i], q[i], Side::Put);
        ASSERT_TRUE(want.has_value()) << "i=" << i;
        // Vega rides the sigma+/- solves — the pair whose refusal was unguarded.
        EXPECT_NEAR(got[i].price, want->price, 1e-9 * K[i]) << "i=" << i;
        EXPECT_NEAR(got[i].vega, want->vega, 1e-6 + 1e-6 * std::fabs(want->vega))
            << "i=" << i;
        EXPECT_NEAR(got[i].rho, want->rho, 1e-6 + 1e-6 * std::fabs(want->rho)) << "i=" << i;
    }
}

// ── T5: an eSSVI slice with an invalid time axis must be REFUSED, not served ─────
//
// slice_vector_admissible validated theta, phi and rho but never T, while the sigma
// batch divides by slice.T: T == 0 served +inf as a volatility and T < 0 served NaN,
// neither flagged. Both routes must now refuse explicitly, and identically.
namespace {

EssviParams make_slice(double T) {
    EssviParams s;
    s.theta = 0.04;
    s.phi = 0.9;
    s.rho = -0.25;
    s.T = T;
    s.F = 100.0;
    return s;
}

void expect_sigma_refused(double T, simd::SimdIsa isa) {
    IsaGuard guard;
    simd::set_simd_isa_override(isa);
    const EssviParams s = make_slice(T);
    // 7 strikes: exercises the 4-wide body AND the n % 4 == 3 tail.
    const std::vector<double> k = {-0.4, -0.2, -0.05, 0.0, 0.05, 0.2, 0.4};
    std::vector<double> sig(k.size(), 0.0);
    simd::essvi_backbone_sigma_batch(s, k.data(), sig.data(), k.size());
    for (std::size_t i = 0; i < sig.size(); ++i) {
        EXPECT_TRUE(std::isnan(sig[i]))
            << "T=" << T << " isa=" << static_cast<int>(isa) << " i=" << i
            << " served the finite-or-infinite vol " << sig[i];
    }
}

} // namespace

TEST(SimdNanSafety, EssviSigmaBatch_NonPositiveT_RefusedOnBothRoutes) {
    for (const double T : {0.0, -0.5, -1e-18}) {
        expect_sigma_refused(T, simd::SimdIsa::ForceScalar);
        if (simd::have_avx2()) {
            expect_sigma_refused(T, simd::SimdIsa::ForceAvx2);
        }
    }
}

TEST(SimdNanSafety, EssviSigmaBatch_NonFiniteT_RefusedOnBothRoutes) {
    for (const double T : {kNaN, kInf, -kInf}) {
        expect_sigma_refused(T, simd::SimdIsa::ForceScalar);
        if (simd::have_avx2()) {
            expect_sigma_refused(T, simd::SimdIsa::ForceAvx2);
        }
    }
}

// A valid slice is untouched by the added admissibility term, on both routes.
TEST(SimdNanSafety, EssviSigmaBatch_ValidT_UnchangedAndRouteIdentical) {
    IsaGuard guard;
    const EssviParams s = make_slice(0.75);
    const std::vector<double> k = {-0.4, -0.2, -0.05, 0.0, 0.05, 0.2, 0.4};

    simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
    std::vector<double> want(k.size(), 0.0);
    simd::essvi_backbone_sigma_batch(s, k.data(), want.data(), k.size());

    for (std::size_t i = 0; i < k.size(); ++i) {
        const double w = essvi_backbone_w(s, k[i]);
        ASSERT_TRUE(std::isfinite(want[i])) << "i=" << i;
        EXPECT_EQ(want[i], std::sqrt(std::max(w, 0.0) / s.T)) << "i=" << i;
    }

    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 — the vector leg of this parity check did not run";
    }
    simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);
    std::vector<double> got(k.size(), 0.0);
    simd::essvi_backbone_sigma_batch(s, k.data(), got.data(), k.size());
    for (std::size_t i = 0; i < k.size(); ++i) {
        EXPECT_NEAR(got[i], want[i], 1e-14) << "i=" << i;
    }
}

// ── T6 premise: log_pd decodes a non-normal ratio as FINITE garbage ──────────────
//
// log_pd's exponent/mantissa decode assumes a positive NORMAL argument. A denormal,
// a zero or an infinity therefore comes back near +/-709 instead of +/-inf — finite,
// so nonfinite_mask cannot see it. This is exactly why black76_batch_avx2 and
// greeks_batch_avx2 carry a |ln(F/K)| >= 708 escape, and why iv_batch_avx2 now does
// too. Pin the premise so the escape cannot be "simplified" away later.
TEST(SimdNanSafety, LogPd_NonNormalArgument_ReturnsFiniteGarbageInsideTheEscapeBand) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — log_pd never runs, this proves nothing";
    }
    const std::vector<double> x = {1e-310, 0.0, kInf, 5e-324};
    std::vector<double> got(x.size(), 0.0);
    simd::fd_log_batch(x.data(), got.data(), x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        EXPECT_TRUE(std::isfinite(got[i]))
            << "i=" << i << " — if this ever becomes non-finite the |lnFK| >= 708 "
                           "escape can be replaced by nonfinite_mask";
        EXPECT_GE(std::fabs(got[i]), 708.0)
            << "i=" << i << " garbage must stay inside the 708 escape band, got "
            << got[i];
    }
}

} // namespace
} // namespace atx::vol
