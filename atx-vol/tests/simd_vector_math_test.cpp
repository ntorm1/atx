// P3.3 math-mode gates: the FastDeterministic vector transcendentals stay within a
// STATED bound of the scalar-libm Reference, and the MathMode knob actually selects
// which path the kernels run.
//
//   VectorMath_FastDeterministic_BoundedVsReference
//       Grades detail/vector_math.hpp's log_pd / exp_pd / norm_cdf_pd (via the
//       vector_math_probe surface) against std::log / std::exp / atx::core::norm_cdf
//       over a broad grid incl. the Φ wings. Asserts the FastDeterministic contract
//       from math_mode.hpp: Φ interior (|x| ≤ kNormCdfWing) ≤ kFastDeterministicPhiBound,
//       log/exp relative error ≤ kFastDeterministicLogExpRelBound. The Φ wing is only
//       recorded (the kernels patch |d| > kNormCdfWing to the exact scalar path).
//
//   VectorMath_ModeSelection
//       The MathMode -> SimdIsa mapping is correct, and driving the American boundary
//       batch under each mode makes the kernel HONOR it: Reference => Scalar route,
//       bit-identical to andersen_lake; FastDeterministic => Avx2 route, within
//       kFastDeterministicPriceBound.
//
// AVX2-specific expectations SKIP on a non-AVX2 host; the mode-mapping + Reference
// bit-identity checks always run (they need no AVX2).

#include "atx/vol/simd/math_mode.hpp"
#include "atx/vol/simd/vector_math_probe.hpp"

#include "atx/core/math.hpp"                       // atx::core::norm_cdf (Reference Φ)
#include "atx/vol/american.hpp"                    // andersen_lake
#include "atx/vol/detail/norm_cdf_cheb.hpp"        // kNormCdfWing, kNormCdfHalfRange
#include "atx/vol/simd/american_boundary_batch.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// RAII: restore Auto dispatch after a test twiddles the override/mode.
struct ModeGuard {
    ~ModeGuard() { simd::set_simd_isa_override(simd::SimdIsa::Auto); }
};

double ref_put(double S, double K, double T, double sigma, double r, double q) {
    const Result<double> res = andersen_lake(S, K, T, sigma, r, q, Side::Put);
    return res.has_value() ? *res : std::numeric_limits<double>::quiet_NaN();
}

// ── FastDeterministic vector Φ/log/exp vs scalar libm Reference ──────────────
TEST(VectorMath, FastDeterministic_BoundedVsReference) {
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 on this host — FastDeterministic == Reference here";
    }

    // ---- Φ: interior (asserted) + wing (recorded) ----
    // Fine sweep across the full clamped range so both the trusted interior and the
    // patched wing are covered.
    std::vector<double> xs;
    for (double x = -detail::kNormCdfHalfRange; x <= detail::kNormCdfHalfRange;
         x += 1.0 / 512.0) {
        xs.push_back(x);
    }
    std::vector<double> got(xs.size(), 0.0);
    simd::fd_norm_cdf_batch(xs.data(), got.data(), xs.size());

    double max_interior = 0.0;
    double max_wing = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double want = atx::core::norm_cdf(xs[i]);
        const double e = std::abs(got[i] - want);
        if (std::abs(xs[i]) <= detail::kNormCdfWing) {
            max_interior = std::max(max_interior, e);
        } else {
            max_wing = std::max(max_wing, e);
        }
    }
    std::printf("[VectorMath] Phi interior max|fd-ref|=%.3e (bound %.1e), "
                "wing max=%.3e (recorded; kernels patch)\n",
                max_interior, simd::kFastDeterministicPhiBound, max_wing);
    EXPECT_LE(max_interior, simd::kFastDeterministicPhiBound);

    // ---- log: positive domain, log-spaced across many decades ----
    std::vector<double> lx;
    for (double e = -8.0; e <= 8.0; e += 1.0 / 64.0) {
        lx.push_back(std::pow(10.0, e));
    }
    std::vector<double> lg(lx.size(), 0.0);
    simd::fd_log_batch(lx.data(), lg.data(), lx.size());
    double max_log_rel = 0.0;
    for (std::size_t i = 0; i < lx.size(); ++i) {
        const double want = std::log(lx[i]);
        const double denom = std::max(std::abs(want), 1.0);
        max_log_rel = std::max(max_log_rel, std::abs(lg[i] - want) / denom);
    }
    std::printf("[VectorMath] log max rel err=%.3e (bound %.1e)\n", max_log_rel,
                simd::kFastDeterministicLogExpRelBound);
    EXPECT_LE(max_log_rel, simd::kFastDeterministicLogExpRelBound);

    // ---- exp: broad symmetric domain within the representable range ----
    std::vector<double> ex;
    for (double x = -80.0; x <= 80.0; x += 1.0 / 32.0) {
        ex.push_back(x);
    }
    std::vector<double> ee(ex.size(), 0.0);
    simd::fd_exp_batch(ex.data(), ee.data(), ex.size());
    double max_exp_rel = 0.0;
    for (std::size_t i = 0; i < ex.size(); ++i) {
        const double want = std::exp(ex[i]);
        max_exp_rel = std::max(max_exp_rel, std::abs(ee[i] - want) / want);
    }
    std::printf("[VectorMath] exp max rel err=%.3e (bound %.1e)\n", max_exp_rel,
                simd::kFastDeterministicLogExpRelBound);
    EXPECT_LE(max_exp_rel, simd::kFastDeterministicLogExpRelBound);
}

// ── The MathMode knob maps onto the ISA seam and the kernels honor it ────────
TEST(VectorMath, ModeSelection) {
    ModeGuard g;

    // Mapping contract (host-independent for Reference; ISA-aware for Fast).
    EXPECT_EQ(simd::isa_for_math_mode(simd::MathMode::Reference),
              simd::SimdIsa::ForceScalar);
    EXPECT_EQ(simd::isa_for_math_mode(simd::MathMode::FastDeterministic),
              simd::have_avx2() ? simd::SimdIsa::ForceAvx2
                                : simd::SimdIsa::ForceScalar);
    EXPECT_STREQ(simd::math_mode_name(simd::MathMode::Reference), "Reference");
    EXPECT_STREQ(simd::math_mode_name(simd::MathMode::FastDeterministic),
                 "FastDeterministic");

    simd::set_math_mode(simd::MathMode::Reference);
    EXPECT_EQ(simd::active_math_mode(), simd::MathMode::Reference);

    // A compact NORMAL-domain American-put grid (r>0, moderate corners).
    struct Put { double S, K, T, sig, r, q; };
    std::vector<Put> grid;
    const double spots[] = {80.0, 100.0, 130.0};
    const double money[] = {0.8, 0.95, 1.0, 1.1, 1.25};
    const double tenors[] = {0.1, 0.5, 1.5};
    const double vols[] = {0.15, 0.30, 0.5};
    for (double S : spots)
        for (double m : money)
            for (double T : tenors)
                for (double v : vols)
                    grid.push_back({S, S * m, T, v, 0.05, 0.01});

    std::vector<double> S(grid.size()), K(grid.size()), T(grid.size()),
        sg(grid.size()), r(grid.size()), q(grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
        S[i] = grid[i].S; K[i] = grid[i].K; T[i] = grid[i].T;
        sg[i] = grid[i].sig; r[i] = grid[i].r; q[i] = grid[i].q;
    }

    // Reference mode -> Scalar route, BIT-IDENTICAL to andersen_lake.
    simd::set_math_mode(simd::MathMode::Reference);
    std::vector<double> out_ref(grid.size(), 0.0);
    const simd::SimdRoute route_ref = simd::american_put_boundary_batch(
        S.data(), K.data(), T.data(), sg.data(), r.data(), q.data(),
        out_ref.data(), grid.size());
    EXPECT_EQ(route_ref, simd::SimdRoute::Scalar);
    for (std::size_t i = 0; i < grid.size(); ++i) {
        const double want = ref_put(S[i], K[i], T[i], sg[i], r[i], q[i]);
        ASSERT_TRUE(std::isfinite(want)) << "i=" << i;
        EXPECT_EQ(out_ref[i], want) << "i=" << i; // bit-for-bit
    }

    // FastDeterministic mode -> Avx2 route, within the stated price bound.
    if (!simd::have_avx2()) {
        GTEST_SKIP() << "no AVX2 — FastDeterministic route not exercisable here";
    }
    simd::set_math_mode(simd::MathMode::FastDeterministic);
    EXPECT_EQ(simd::active_math_mode(), simd::MathMode::FastDeterministic);
    std::vector<double> out_fd(grid.size(), 0.0);
    const simd::SimdRoute route_fd = simd::american_put_boundary_batch(
        S.data(), K.data(), T.data(), sg.data(), r.data(), q.data(),
        out_fd.data(), grid.size());
    EXPECT_EQ(route_fd, simd::SimdRoute::Avx2);

    double max_price = 0.0;
    for (std::size_t i = 0; i < grid.size(); ++i) {
        max_price = std::max(max_price, std::abs(out_fd[i] - out_ref[i]));
    }
    std::printf("[VectorMath] FastDeterministic vs Reference boundary-batch "
                "max|Δprice|=%.3e (bound %.1e)\n",
                max_price, simd::kFastDeterministicPriceBound);
    EXPECT_LE(max_price, simd::kFastDeterministicPriceBound);
}

} // namespace
} // namespace atx::vol
