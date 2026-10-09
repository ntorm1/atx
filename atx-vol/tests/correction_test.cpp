#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/fitting/correction.hpp"
#include "fitting/correction_detail.hpp" // Chebyshev primitives (kChebMaxNodes, cheb_*)
#include "fitting/counters.hpp"
#include "atx/vol/api/pricing/greeks.hpp"
#include "support/isa_golden_tol.hpp"

// American-correction cache coverage, ported from the C ats-vol tests
// test_correction_cache.c and test_amer_clamp_policy.c:
//   - Chebyshev DCT-II round-trip and Clenshaw vs a direct polynomial sum,
//   - the derivative-coefficient recurrence and 3D partial derivative,
//   - populate + eval recovering the Andersen-Lake correction on a grid,
//   - non-negativity, unpopulated-reads-zero, and build input validation,
//   - the hot-path cached price matching cold Andersen-Lake,
//   - the out-of-box extrapolation policies.

namespace {

using atx::vol::andersen_lake;
using atx::vol::black76_price;
using atx::vol::CorrectionBlend;
using atx::vol::CorrectionCache;
using atx::vol::CorrPartials;
using atx::vol::CorrResult;
using atx::vol::CorrSecondOrder;
using atx::vol::ExtrapPolicy;
using atx::vol::Side;
namespace detail = atx::vol::detail;

// ── Chebyshev primitives ────────────────────────────────────────────────

TEST(Chebyshev, Dct2Roundtrip_AtNodes_IsExact) {
  constexpr std::uint16_t N = 8;
  std::array<double, N> vals{};
  std::array<double, N> coefs{};
  for (std::uint16_t j = 0; j < N; ++j) {
    const double x = detail::cheb_node(j, N);
    vals[j] = 0.5 + 1.5 * x + 2.0 * x * x - 0.7 * x * x * x;
  }
  detail::cheb_dct2(vals.data(), coefs.data(), N);
  for (std::uint16_t j = 0; j < N; ++j) {
    const double x = detail::cheb_node(j, N);
    EXPECT_LT(std::fabs(detail::cheb_clenshaw1d(coefs.data(), N, x) - vals[j]), 1.0e-13);
  }
}

TEST(Chebyshev, Dct2Roundtrip_SmoothFunction_IsSpectral) {
  constexpr std::uint16_t N = 16;
  std::array<double, N> vals{};
  std::array<double, N> coefs{};
  for (std::uint16_t j = 0; j < N; ++j) {
    const double x = detail::cheb_node(j, N);
    vals[j] = std::exp(0.5 * x) * std::cos(2.0 * x);
  }
  detail::cheb_dct2(vals.data(), coefs.data(), N);
  for (int i = 0; i < 20; ++i) {
    const double x = -1.0 + 0.1 * static_cast<double>(i);
    const double truth = std::exp(0.5 * x) * std::cos(2.0 * x);
    EXPECT_LT(std::fabs(detail::cheb_clenshaw1d(coefs.data(), N, x) - truth), 1.0e-10);
  }
}

TEST(Chebyshev, Clenshaw_MatchesDirectSum) {
  constexpr std::uint16_t N = 12;
  std::array<double, N> coefs{};
  for (std::uint16_t k = 0; k < N; ++k) {
    coefs[k] = 0.1 * std::sin(static_cast<double>(k) + 1.0) - 0.05 * static_cast<double>(k);
  }
  const auto cheb_T = [](std::uint16_t k, double x) {
    if (k == 0)
      return 1.0;
    if (k == 1)
      return x;
    double t0 = 1.0, t1 = x, t2 = 0.0;
    for (std::uint16_t i = 2; i <= k; ++i) {
      t2 = 2.0 * x * t1 - t0;
      t0 = t1;
      t1 = t2;
    }
    return t1;
  };
  for (int i = -10; i <= 10; ++i) {
    const double x = 0.1 * static_cast<double>(i);
    double direct = coefs[0];
    for (std::uint16_t k = 1; k < N; ++k) {
      direct += coefs[k] * cheb_T(k, x);
    }
    EXPECT_LT(std::fabs(direct - detail::cheb_clenshaw1d(coefs.data(), N, x)), 1.0e-13);
  }
}

TEST(Chebyshev, DiffCoefs_KnownPolynomial) {
  // T_3' = 12ξ² - 3 = 3 T_0 + 6 T_2 in the full-c0 Clenshaw convention.
  const std::array<double, 4> c{0.0, 0.0, 0.0, 1.0};
  std::array<double, 4> d{};
  detail::cheb_diff_coefs(c.data(), d.data(), 4u, 1.0);
  EXPECT_LT(std::fabs(d[0] - 3.0), 1.0e-14);
  EXPECT_LT(std::fabs(d[1] - 0.0), 1.0e-14);
  EXPECT_LT(std::fabs(d[2] - 6.0), 1.0e-14);
  EXPECT_LT(std::fabs(d[3] - 0.0), 1.0e-14);
}

TEST(Chebyshev, DiffCoefs_MatchesAnalyticDerivative) {
  constexpr std::uint16_t N = 16;
  std::array<double, N> vals{};
  std::array<double, N> coefs{};
  std::array<double, N> dcoefs{};
  for (std::uint16_t j = 0; j < N; ++j) {
    vals[j] = std::sin(2.0 * detail::cheb_node(j, N));
  }
  detail::cheb_dct2(vals.data(), coefs.data(), N);
  detail::cheb_diff_coefs(coefs.data(), dcoefs.data(), N, 1.0);
  for (int i = -9; i <= 9; ++i) {
    const double x = 0.1 * static_cast<double>(i);
    EXPECT_LT(std::fabs(2.0 * std::cos(2.0 * x) - detail::cheb_clenshaw1d(dcoefs.data(), N, x)),
              1.0e-9);
  }
}

TEST(Chebyshev, Clenshaw3dPartial_MatchesAnalytic) {
  constexpr std::uint16_t n_k = 12, n_T = 10, n_s = 8;
  std::array<double, 12 * 10 * 8> vals{};
  // f = exp(0.3 ξ_i) cos(ξ_j) (1 + 0.2 ξ_k²).
  for (std::uint16_t j = 0; j < n_T; ++j)
    for (std::uint16_t k = 0; k < n_s; ++k)
      for (std::uint16_t i = 0; i < n_k; ++i) {
        const double xi = detail::cheb_node(i, n_k);
        const double xj = detail::cheb_node(j, n_T);
        const double xk = detail::cheb_node(k, n_s);
        vals[detail::cheb_idx(i, j, k, n_k, n_s)] =
            std::exp(0.3 * xi) * std::cos(xj) * (1.0 + 0.2 * xk * xk);
      }

  // Separable DCT-II along each axis.
  std::array<double, 16> in_buf{};
  std::array<double, 16> out_buf{};
  for (std::uint16_t j = 0; j < n_T; ++j)
    for (std::uint16_t k = 0; k < n_s; ++k) {
      for (std::uint16_t i = 0; i < n_k; ++i)
        in_buf[i] = vals[detail::cheb_idx(i, j, k, n_k, n_s)];
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_k);
      for (std::uint16_t i = 0; i < n_k; ++i)
        vals[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[i];
    }
  for (std::uint16_t i = 0; i < n_k; ++i)
    for (std::uint16_t k = 0; k < n_s; ++k) {
      for (std::uint16_t j = 0; j < n_T; ++j)
        in_buf[j] = vals[detail::cheb_idx(i, j, k, n_k, n_s)];
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_T);
      for (std::uint16_t j = 0; j < n_T; ++j)
        vals[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[j];
    }
  for (std::uint16_t i = 0; i < n_k; ++i)
    for (std::uint16_t j = 0; j < n_T; ++j) {
      for (std::uint16_t k = 0; k < n_s; ++k)
        in_buf[k] = vals[detail::cheb_idx(i, j, k, n_k, n_s)];
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_s);
      for (std::uint16_t k = 0; k < n_s; ++k)
        vals[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[k];
    }

  std::array<double, 64 * 64> tmp{};
  const double xi = 0.3, xj = -0.2, xk = 0.1;

  const double exact_di = 0.3 * std::exp(0.3 * xi) * std::cos(xj) * (1.0 + 0.2 * xk * xk);
  EXPECT_LT(std::fabs(detail::cheb_clenshaw3d_partial(vals.data(), n_k, n_T, n_s, xi, xj, xk, 0,
                                                      1.0, tmp.data()) -
                      exact_di),
            1.0e-8);

  const double exact_dj = -std::exp(0.3 * xi) * std::sin(xj) * (1.0 + 0.2 * xk * xk);
  EXPECT_LT(std::fabs(detail::cheb_clenshaw3d_partial(vals.data(), n_k, n_T, n_s, xi, xj, xk, 1,
                                                      1.0, tmp.data()) -
                      exact_dj),
            1.0e-8);

  const double exact_dk = 0.4 * xk * std::exp(0.3 * xi) * std::cos(xj);
  EXPECT_LT(std::fabs(detail::cheb_clenshaw3d_partial(vals.data(), n_k, n_T, n_s, xi, xj, xk, 2,
                                                      1.0, tmp.data()) -
                      exact_dk),
            1.0e-8);
}

// ── T16b: precomputed k_log-axis derivative tensor (C_k) bit-identity ─────
//
// T16b hoists the k_log partial's per-query row differentiation to BUILD time:
// each innermost i-row (contiguous k_log fiber) is run through cheb_diff_coefs
// ONCE at build, and eval_partials reads dC/dk_log as a PLAIN value Clenshaw
// (cheb_clenshaw3d) over that precomputed tensor instead of re-differentiating
// every query via cheb_clenshaw3d_partial(diff_axis==0).
//
// Because the k_log differentiation is the INNERMOST operation — applied to the
// raw coefficient rows before any Clenshaw collapse — diff-at-build then Clenshaw
// is BIT-IDENTICAL to the in-pass diff-then-Clenshaw. This test locks that: over a
// grid, cheb_clenshaw3d(C_k) must equal cheb_clenshaw3d_partial(coefs, axis 0)
// bit-for-bit. (The T and sigma partials differentiate a Clenshaw-COLLAPSED,
// query-dependent vector; moving their diff to build reorders the summation and
// shifts bits by hundreds of ULP, so those axes are deliberately NOT hoisted and
// stay on the reference cheb_clenshaw3d_partial path.)
TEST(Chebyshev, DerivTensors_EvalPartialsBitIdenticalToLive) {
  constexpr std::uint16_t n_k = 16, n_T = 8, n_s = 12;
  std::array<double, n_k * n_T * n_s> coefs{};
  // A generic (non-symmetric) coefficient tensor, built through the same DCT-II
  // path the cache uses, so C_k has the exact coefficient structure eval sees.
  for (std::uint16_t j = 0; j < n_T; ++j)
    for (std::uint16_t k = 0; k < n_s; ++k)
      for (std::uint16_t i = 0; i < n_k; ++i) {
        const double xi = detail::cheb_node(i, n_k);
        const double xj = detail::cheb_node(j, n_T);
        const double xk = detail::cheb_node(k, n_s);
        coefs[detail::cheb_idx(i, j, k, n_k, n_s)] =
            std::exp(0.4 * xi) * (1.0 + 0.3 * xj) * std::cos(0.7 * xk) + 0.11 * xi * xk;
      }
  std::array<double, 16> in_buf{}, out_buf{};
  for (std::uint16_t j = 0; j < n_T; ++j)
    for (std::uint16_t k = 0; k < n_s; ++k) {
      for (std::uint16_t i = 0; i < n_k; ++i)
        in_buf[i] = coefs[detail::cheb_idx(i, j, k, n_k, n_s)];
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_k);
      for (std::uint16_t i = 0; i < n_k; ++i)
        coefs[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[i];
    }
  for (std::uint16_t i = 0; i < n_k; ++i)
    for (std::uint16_t k = 0; k < n_s; ++k) {
      for (std::uint16_t j = 0; j < n_T; ++j)
        in_buf[j] = coefs[detail::cheb_idx(i, j, k, n_k, n_s)];
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_T);
      for (std::uint16_t j = 0; j < n_T; ++j)
        coefs[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[j];
    }
  for (std::uint16_t i = 0; i < n_k; ++i)
    for (std::uint16_t j = 0; j < n_T; ++j) {
      for (std::uint16_t k = 0; k < n_s; ++k)
        in_buf[k] = coefs[detail::cheb_idx(i, j, k, n_k, n_s)];
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_s);
      for (std::uint16_t k = 0; k < n_s; ++k)
        coefs[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[k];
    }

  // Build C_k EXACTLY as CorrectionCache::build does: diff each contiguous i-row
  // over the k_log axis, with the box axis scale folded in.
  const double scale_k = 2.0 / (0.5 - (-0.5));
  std::array<double, n_k * n_T * n_s> Ck{};
  std::array<double, detail::kChebMaxNodes> drow{};
  for (std::uint16_t j = 0; j < n_T; ++j)
    for (std::uint16_t k = 0; k < n_s; ++k) {
      const double *row = coefs.data() + detail::cheb_idx(0, j, k, n_k, n_s);
      detail::cheb_diff_coefs(row, drow.data(), n_k, scale_k);
      double *orow = Ck.data() + detail::cheb_idx(0, j, k, n_k, n_s);
      for (std::uint16_t i = 0; i < n_k; ++i)
        orow[i] = drow[i];
    }

  std::array<double, 64 * 64> tmp{};
  for (int a = -5; a <= 5; ++a)
    for (int b = -5; b <= 5; ++b)
      for (int c = -5; c <= 5; ++c) {
        const double xi = 0.2 * static_cast<double>(a);
        const double xj = 0.2 * static_cast<double>(b);
        const double xk = 0.2 * static_cast<double>(c);
        const double live = detail::cheb_clenshaw3d_partial(coefs.data(), n_k, n_T, n_s, xi, xj, xk,
                                                            0, scale_k, tmp.data());
        const double pre =
            detail::cheb_clenshaw3d(Ck.data(), n_k, n_T, n_s, xi, xj, xk, tmp.data());
        EXPECT_EQ(std::bit_cast<std::uint64_t>(pre), std::bit_cast<std::uint64_t>(live))
            << "k_log partial precompute vs live @(" << xi << "," << xj << "," << xk << ")";
      }
}

// ── Cache build / eval ──────────────────────────────────────────────────

TEST(CorrectionCache, PopulateEval_MatchesAndersenLake_PutGrid) {
  const double r = 0.05, q = 0.0;
  auto built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5, 30.0 / 365.25, 2.0, 0.10, 0.80,
                                      Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  std::uint64_t seed = 0x1234567890abcdefULL;
  double max_abs_err = 0.0;
  double sum_sq_err = 0.0;
  int n_eval = 0;
  for (int i = 0; i < 200; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const double u1 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const double u2 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const double u3 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;

    const double k_log = -0.45 + 0.90 * u1;
    const double T = 0.10 + 1.9 * u2;
    const double sigma = 0.12 + 0.65 * u3;

    const double S = std::exp(-(r - q) * T);
    const double K = std::exp(k_log);
    const auto amer = andersen_lake(S, K, T, sigma, r, q, Side::Put);
    if (!amer) {
      continue;
    }
    const double euro = black76_price(1.0, K, T, sigma, std::exp(-r * T), Side::Put);
    const double truth = *amer - euro;
    const double err = tbl.eval(k_log, T, sigma) - truth;
    max_abs_err = std::fmax(max_abs_err, std::fabs(err));
    sum_sq_err += err * err;
    ++n_eval;
  }
  ASSERT_GT(n_eval, 0);
  const double rmse = std::sqrt(sum_sq_err / static_cast<double>(n_eval));
  EXPECT_LT(max_abs_err, 1.0e-3);
  EXPECT_LT(rmse, 3.0e-4);
}

TEST(CorrectionCache, PopulateEval_MatchesAndersenLake_CallWithDividend) {
  const double r = 0.02, q = 0.05;
  auto built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5, 30.0 / 365.25, 2.0, 0.10, 0.80,
                                      Side::Call);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  std::uint64_t seed = 0xdeadbeef12345678ULL;
  double max_abs_err = 0.0;
  int n_eval = 0;
  for (int i = 0; i < 200; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const double u1 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const double u2 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const double u3 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;

    const double k_log = -0.45 + 0.90 * u1;
    const double T = 0.10 + 1.9 * u2;
    const double sigma = 0.12 + 0.65 * u3;

    const double S = std::exp(-(r - q) * T);
    const double K = std::exp(k_log);
    const auto amer = andersen_lake(S, K, T, sigma, r, q, Side::Call);
    if (!amer) {
      continue;
    }
    const double euro = black76_price(1.0, K, T, sigma, std::exp(-r * T), Side::Call);
    max_abs_err = std::fmax(max_abs_err, std::fabs(tbl.eval(k_log, T, sigma) - (*amer - euro)));
    ++n_eval;
  }
  ASSERT_GT(n_eval, 0);
  EXPECT_LT(max_abs_err, 1.0e-3);
}

TEST(CorrectionCache, Eval_IsNonNegativeAcrossBox) {
  auto built = CorrectionCache::build(24, 16, 12, 0.05, 0.0, -0.5, 0.5, 30.0 / 365.25, 2.0, 0.10,
                                      0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);
  for (int a = 0; a < 5; ++a)
    for (int b = 0; b < 5; ++b)
      for (int c = 0; c < 5; ++c) {
        const double k_log = -0.5 + 0.25 * static_cast<double>(a);
        const double T = 0.1 + 0.45 * static_cast<double>(b);
        const double sigma = 0.12 + 0.16 * static_cast<double>(c);
        EXPECT_GE(tbl.eval(k_log, T, sigma), 0.0);
      }
}

TEST(CorrectionCache, DefaultConstructed_ReadsZero) {
  const CorrectionCache tbl;
  EXPECT_FALSE(tbl.populated());
  EXPECT_LT(std::fabs(tbl.eval(0.0, 0.5, 0.3)), 1.0e-15);
}

TEST(CorrectionCache, Build_RejectsZeroDimension) {
  auto r = CorrectionCache::build(0, 8, 8, 0.05, 0.0, -0.5, 0.5, 0.1, 1.0, 0.1, 0.5, Side::Put);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(CorrectionCache, Build_RejectsOversizeDimension) {
  auto r = CorrectionCache::build(65, 8, 8, 0.05, 0.0, -0.5, 0.5, 0.1, 1.0, 0.1, 0.5, Side::Put);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(CorrectionCache, Build_RejectsInvertedBox) {
  // k_log_max < k_log_min.
  auto r1 = CorrectionCache::build(8, 8, 8, 0.05, 0.0, 0.5, -0.5, 0.1, 1.0, 0.1, 0.5, Side::Put);
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error().code(), atx::core::ErrorCode::InvalidArgument);
  // T_min <= 0.
  auto r2 = CorrectionCache::build(8, 8, 8, 0.05, 0.0, -0.5, 0.5, 0.0, 1.0, 0.1, 0.5, Side::Put);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error().code(), atx::core::ErrorCode::InvalidArgument);
  // sigma_min <= 0.
  auto r3 = CorrectionCache::build(8, 8, 8, 0.05, 0.0, -0.5, 0.5, 0.1, 1.0, 0.0, 0.5, Side::Put);
  ASSERT_FALSE(r3.has_value());
  EXPECT_EQ(r3.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── Extrapolation policy (out-of-box query behaviour) ───────────────────

CorrectionCache make_small_correction() {
  auto built =
      CorrectionCache::build(8, 6, 6, 0.05, 0.0, -0.3, 0.3, 0.10, 1.0, 0.15, 0.60, Side::Put);
  EXPECT_TRUE(built.has_value());
  return built ? std::move(*built) : CorrectionCache{};
}

TEST(CorrectionCacheExtrap, DefaultPolicy_IsClamp) {
  const CorrectionCache tbl = make_small_correction();
  EXPECT_EQ(tbl.extrap_policy(), ExtrapPolicy::Clamp);

  const auto in_box = tbl.query(0.05, 0.4, 0.30, CorrPartials::Value);
  ASSERT_TRUE(in_box.has_value());
  EXPECT_TRUE(std::isfinite(in_box->value));

  // Out-of-box under CLAMP returns a finite box-edge value.
  const auto oob = tbl.query(0.5, 0.4, 0.30, CorrPartials::Value);
  ASSERT_TRUE(oob.has_value());
  EXPECT_TRUE(std::isfinite(oob->value));
}

TEST(CorrectionCacheExtrap, NanOutside_ReturnsNanValue) {
  CorrectionCache tbl = make_small_correction();
  ASSERT_TRUE(tbl.set_extrap_policy(ExtrapPolicy::NanOutside).has_value());
  EXPECT_EQ(tbl.extrap_policy(), ExtrapPolicy::NanOutside);

  const auto in_box = tbl.query(0.05, 0.4, 0.30, CorrPartials::Value | CorrPartials::Dsigma);
  ASSERT_TRUE(in_box.has_value());
  EXPECT_TRUE(std::isfinite(in_box->value));

  const auto oob_k = tbl.query(0.5, 0.4, 0.30, CorrPartials::Value | CorrPartials::Dsigma);
  ASSERT_TRUE(oob_k.has_value());
  EXPECT_TRUE(std::isnan(oob_k->value));
  EXPECT_EQ(oob_k->dsigma, 0.0);
  EXPECT_EQ(oob_k->mask_filled, CorrPartials::Value | CorrPartials::Dsigma);

  EXPECT_TRUE(std::isnan(tbl.query(0.05, 0.05, 0.30, CorrPartials::Value)->value));
  EXPECT_TRUE(std::isnan(tbl.query(0.05, 0.4, 0.80, CorrPartials::Value)->value));
}

TEST(CorrectionCacheExtrap, ErrorOutside_ReturnsOutOfRange) {
  CorrectionCache tbl = make_small_correction();
  ASSERT_TRUE(tbl.set_extrap_policy(ExtrapPolicy::ErrorOutside).has_value());

  const auto in_box = tbl.query(0.05, 0.4, 0.30, CorrPartials::Value);
  ASSERT_TRUE(in_box.has_value());
  EXPECT_TRUE(std::isfinite(in_box->value));

  const auto oob = tbl.query(-0.5, 0.4, 0.30, CorrPartials::Value);
  ASSERT_FALSE(oob.has_value());
  EXPECT_EQ(oob.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(CorrectionCacheExtrap, SetPolicy_RejectsInvalid) {
  CorrectionCache tbl = make_small_correction();
  const auto rc = tbl.set_extrap_policy(static_cast<ExtrapPolicy>(99));
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── Hot-path cached price matches cold Andersen-Lake ────────────────────

TEST(CorrectionCache, CachedPrice_MatchesColdAndersenLake) {
  const double r = 0.05, q = 0.0;
  auto built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5, 30.0 / 365.25, 2.0, 0.10, 0.80,
                                      Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  const double S = 100.0, K = 105.0, T = 0.5, sigma = 0.25;
  const double cold = *andersen_lake(S, K, T, sigma, r, q, Side::Put);
  const double hot = atx::vol::american_price_cached(S, K, T, sigma, r, q, Side::Put, &tbl);
  EXPECT_LT(std::fabs(hot - cold) / cold, 1.0e-3);
}

// T16a: the put-side cache builder collapses each (T, sigma) k_log row onto ONE
// early-exercise boundary solve via andersen_lake_put_slice (strike homogeneity),
// mirroring the call side. A valid American-put box (r>0, q=0, T/sigma away from
// the degenerate guards) routes every (T, sigma) row through the American slice
// branch => exactly n_T*n_s BoundarySolves for the whole build, NOT n_T*n_s*n_k
// as the scalar per-node path costs. This is the measured win.
TEST(CorrectionCache, PutRowCollapse_SolveCount) {
  constexpr std::uint16_t n_k = 12, n_T = 6, n_s = 4;
  if constexpr (!atx::vol::counters::counters_enabled()) {
    auto built = CorrectionCache::build(n_k, n_T, n_s, 0.05, 0.0, -0.5, 0.5, 30.0 / 365.25, 2.0,
                                        0.10, 0.80, Side::Put);
    ASSERT_TRUE(built.has_value());
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  } else {
    atx::vol::counters::reset();
    auto built = CorrectionCache::build(n_k, n_T, n_s, 0.05, 0.0, -0.5, 0.5, 30.0 / 365.25, 2.0,
                                        0.10, 0.80, Side::Put);
    ASSERT_TRUE(built.has_value());
    const auto snap = atx::vol::counters::snapshot();
    EXPECT_TRUE(snap.enabled);
    // One cold boundary solve per (T, sigma) row — the put-row collapse.
    EXPECT_EQ(snap.get(atx::vol::counters::Counter::BoundarySolves),
              static_cast<std::uint64_t>(n_T) * static_cast<std::uint64_t>(n_s));
    // Strictly below the scalar per-node cost (n_T*n_s*n_k) it replaces.
    EXPECT_LT(snap.get(atx::vol::counters::Counter::BoundarySolves),
              static_cast<std::uint64_t>(n_T) * static_cast<std::uint64_t>(n_s) *
                  static_cast<std::uint64_t>(n_k));
  }
}

// T16b: the k_log partial's per-query row differentiation is hoisted to build
// time (precomputed C_k tensor), so requesting ONLY dk_log now runs a plain value
// Clenshaw with ZERO cheb_diff_coefs calls — the whole n_T*n_s per-query diff cost
// the k_log partial used to pay is gone. The T and sigma partials differentiate a
// query-dependent, Clenshaw-collapsed vector (n_s + 1 diffs) and stay at query
// time (hoisting them would reorder the summation and break the frozen bit pins),
// so a full three-partial eval keeps exactly n_s + 1 diffs — strictly below the
// pre-hoist n_T*n_s + n_s + 1.
TEST(CorrectionCache, DerivTensors_KLogPartial_NoPerQueryDiffCoefs) {
  constexpr std::uint16_t n_k = 12, n_T = 6, n_s = 8;
  auto built = CorrectionCache::build(n_k, n_T, n_s, 0.05, 0.0, -0.5, 0.5, 30.0 / 365.25, 2.0, 0.10,
                                      0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  if constexpr (!atx::vol::counters::counters_enabled()) {
    double dk = 0.0;
    tbl.eval_partials(0.0, 0.5, 0.3, &dk, nullptr, nullptr);
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  } else {
    using atx::vol::counters::Counter;
    // Requesting only dk_log: the precomputed C_k tensor is read via a plain value
    // Clenshaw — zero per-query differentiation.
    atx::vol::counters::reset();
    double dk = 0.0;
    tbl.eval_partials(0.0, 0.5, 0.3, &dk, nullptr, nullptr);
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::ChebDiffCoefs),
              static_cast<std::uint64_t>(0));

    // A full three-partial eval differentiates only the (non-hoistable) T and
    // sigma axes: n_s (per-sigma-column T diff) + 1 (single sigma diff).
    atx::vol::counters::reset();
    double gk = 0.0, gT = 0.0, gs = 0.0;
    tbl.eval_partials(0.0, 0.5, 0.3, &gk, &gT, &gs);
    const std::uint64_t full = atx::vol::counters::snapshot().get(Counter::ChebDiffCoefs);
    EXPECT_EQ(full, static_cast<std::uint64_t>(n_s) + 1u);
    // Strictly below the pre-hoist per-query cost (k_log added n_T*n_s more).
    EXPECT_LT(full, static_cast<std::uint64_t>(n_T) * static_cast<std::uint64_t>(n_s) +
                        static_cast<std::uint64_t>(n_s) + 1u);
  }
}

// Fix-wave 1d: baking a cache at a fixed (r, q, side) that lands in the
// double-continuation regime would sample only NotImplemented (floored to 0),
// silently encoding a pure-European surface. Reject the build up front.
TEST(CorrectionCache, Build_RejectsUnsupportedRegime) {
  // Double-continuation PUT (q < r <= 0).
  auto rp =
      CorrectionCache::build(8, 8, 8, -0.005, -0.02, -0.5, 0.5, 0.1, 1.0, 0.1, 0.5, Side::Put);
  ASSERT_FALSE(rp.has_value());
  EXPECT_EQ(rp.error().code(), atx::core::ErrorCode::NotImplemented);
  // Double-continuation CALL (r < q <= 0).
  auto rc =
      CorrectionCache::build(8, 8, 8, -0.02, -0.005, -0.5, 0.5, 0.1, 1.0, 0.1, 0.5, Side::Call);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), atx::core::ErrorCode::NotImplemented);
  // European corner (put r <= 0 && r <= q) is NOT unsupported — it still builds.
  auto re =
      CorrectionCache::build(8, 8, 8, -0.02, -0.005, -0.5, 0.5, 0.1, 1.0, 0.1, 0.5, Side::Put);
  EXPECT_TRUE(re.has_value());
}

// Fix-wave 1b: the POPULATED-cache hot path must return NaN when queried in the
// Unsupported regime (the guard keys off the query's (r, q), not the cache's).
TEST(CorrectionCache, CachedPrice_UnsupportedRegime_ReturnsNaN) {
  const double r = 0.05, q = 0.0; // American put — a valid, populated cache
  auto built = CorrectionCache::build(16, 12, 8, r, q, -0.5, 0.5, 0.1, 2.0, 0.1, 0.8, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);
  // Query at an Unsupported (r, q): guard must surface NaN, not euro+F*corr.
  const double px =
      atx::vol::american_price_cached(100.0, 100.0, 1.0, 0.30, -0.005, -0.02, Side::Put, &tbl);
  EXPECT_TRUE(std::isnan(px));
}

// ── A2 (core-review finding 2): intrinsic floor on the cached hot path ───────
//
// Every COLD path clamps its served price to max(price, intrinsic, euro, 0)
// (al_put_price_from_boundary :1376-1393, AloPricer::price). The cached path
// returned raw euro + F*corr with NO floor. Deep-ITM puts (r>0) whose k_log lands
// OUTSIDE the correction box get a correction CLAMPED to the box-edge value; the
// shortfall vs the true early-exercise premium grows ~linearly with moneyness, so
// the served mark dips below intrinsic — an arbitrageable sub-intrinsic price.
// The floor removes it. Only marks that were previously BELOW intrinsic change.
TEST(CorrectionCache, CachedPrice_DeepItmPutBeyondBox_FlooredAtIntrinsic) {
  const double r = 0.08, q = 0.0;
  auto built = CorrectionCache::build(24, 16, 12, r, q, /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
                                      /*T_min=*/0.1, /*T_max=*/2.0, /*sigma_min=*/0.10,
                                      /*sigma_max=*/0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);
  EXPECT_EQ(tbl.extrap_policy(), ExtrapPolicy::Clamp); // default: raw eval clamps to the box edge

  // Deep-ITM put: S << K puts k_log = log(K/F) well beyond k_log_max = 0.5.
  const double K = 100.0, T = 1.5, sigma = 0.20;
  for (const double S : {40.0, 30.0, 20.0, 12.0}) {
    const double F = S * std::exp((r - q) * T);
    const double k_log = std::log(K / F);
    ASSERT_GT(k_log, tbl.k_log_max()); // genuinely out of the box (Clamp region)
    const double intr = K - S;
    const double hot = atx::vol::american_price_cached(S, K, T, sigma, r, q, Side::Put, &tbl);
    EXPECT_GE(hot, intr) << "sub-intrinsic cached mark at S=" << S;
  }
}

// The greeks-bundle SERVED price (out.price) is floored identically; the greek
// sensitivity fields are left at their smooth analytic values (documented kink).
TEST(CorrectionCache, CachedGreeksPrice_DeepItmPutBeyondBox_FlooredAtIntrinsic) {
  const double r = 0.08, q = 0.0;
  auto built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5, 0.1, 2.0, 0.10, 0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);
  const double S = 20.0, K = 100.0, T = 1.5, sigma = 0.20;
  const auto g = atx::vol::american_greeks(S, K, T, sigma, r, q, Side::Put, &tbl);
  ASSERT_TRUE(g.has_value());
  EXPECT_GE(g->price, K - S);
}

// In-box property sweep: no served cached mark may sit below intrinsic anywhere
// inside the interpolation box, for either side (interpolation error can dip a
// raw euro+F*corr fractionally below intrinsic near the ITM edge).
TEST(CorrectionCache, CachedPrice_InBoxSweep_NoSubIntrinsicMarks) {
  const double r = 0.06, q = 0.0;
  auto put_built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5, 0.1, 2.0, 0.10, 0.80,
                                          Side::Put);
  ASSERT_TRUE(put_built.has_value());
  const CorrectionCache put = std::move(*put_built);
  auto call_built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5, 0.1, 2.0, 0.10, 0.80,
                                           Side::Call);
  ASSERT_TRUE(call_built.has_value());
  const CorrectionCache call = std::move(*call_built);

  const double K = 100.0;
  for (const double T : {0.15, 0.5, 1.0, 1.8}) {
    for (const double sigma : {0.12, 0.25, 0.5, 0.75}) {
      for (const double k_log : {-0.45, -0.2, 0.0, 0.2, 0.45}) { // strictly inside [-0.5, 0.5]
        // k_log = log(K/F) => F = K*exp(-k_log); S = F*exp(-(r-q)T).
        const double F = K * std::exp(-k_log);
        const double S = F * std::exp(-(r - q) * T);
        ASSERT_LE(std::fabs(std::log(K / (S * std::exp((r - q) * T)))), put.k_log_max());
        const double put_px =
            atx::vol::american_price_cached(S, K, T, sigma, r, q, Side::Put, &put);
        EXPECT_GE(put_px, K - S) << "put sub-intrinsic S=" << S << " T=" << T << " sig=" << sigma;
        const double call_px =
            atx::vol::american_price_cached(S, K, T, sigma, r, q, Side::Call, &call);
        EXPECT_GE(call_px, S - K) << "call sub-intrinsic S=" << S << " T=" << T << " sig=" << sigma;
      }
    }
  }
}

// ── Fused second-order correction jet ───────────────────────────────────────
TEST(CorrectionCache, SecondOrderJet_MatchesIndependentFiniteDifferences) {
  auto built = CorrectionCache::build(/*n_k=*/16, /*n_T=*/8, /*n_s=*/12,
                                      /*r=*/0.05, /*q=*/0.0,
                                      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
                                      /*T_min=*/0.05, /*T_max=*/2.0,
                                      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  constexpr double k = -0.08;
  constexpr double T = 0.65;
  constexpr double sigma = 0.32;
  constexpr double hk = 1.0e-5;
  constexpr double hT = 1.0e-5;
  constexpr double hs = 1.0e-5;
  const auto jet = tbl.eval_second_order(k, T, sigma);

  double dk = 0.0;
  double dT = 0.0;
  double ds = 0.0;
  const double value = tbl.eval_grad(k, T, sigma, &dk, &dT, &ds);
  EXPECT_NEAR(jet.value, value, 1.0e-14);
  EXPECT_NEAR(jet.dk_log, dk, 1.0e-12);
  EXPECT_NEAR(jet.dT, dT, 1.0e-12);
  EXPECT_NEAR(jet.dsigma, ds, 1.0e-12);

  const auto partials = [&tbl](double qk, double qT, double qs) {
    std::array<double, 3> out{};
    tbl.eval_partials(qk, qT, qs, &out[0], &out[1], &out[2]);
    return out;
  };
  const auto k_up = partials(k + hk, T, sigma);
  const auto k_dn = partials(k - hk, T, sigma);
  const auto T_up = partials(k, T + hT, sigma);
  const auto T_dn = partials(k, T - hT, sigma);
  const auto s_up = partials(k, T, sigma + hs);
  const auto s_dn = partials(k, T, sigma - hs);

  EXPECT_NEAR(jet.dkk, (k_up[0] - k_dn[0]) / (2.0 * hk), 1.0e-7);
  EXPECT_NEAR(jet.dk_dT, (T_up[0] - T_dn[0]) / (2.0 * hT), 1.0e-7);
  EXPECT_NEAR(jet.dk_dsigma, (s_up[0] - s_dn[0]) / (2.0 * hs), 1.0e-7);
  EXPECT_NEAR(jet.dsigma2, (s_up[2] - s_dn[2]) / (2.0 * hs), 1.0e-7);

  const auto clamped = tbl.eval_second_order(/*k_log=*/0.8, /*T=*/0.01,
                                             /*sigma=*/1.0);
  EXPECT_EQ(clamped.value, tbl.eval(0.8, 0.01, 1.0));
  EXPECT_EQ(clamped.dk_log, 0.0);
  EXPECT_EQ(clamped.dT, 0.0);
  EXPECT_EQ(clamped.dsigma, 0.0);
  EXPECT_EQ(clamped.dkk, 0.0);
  EXPECT_EQ(clamped.dk_dT, 0.0);
  EXPECT_EQ(clamped.dk_dsigma, 0.0);
  EXPECT_EQ(clamped.dsigma2, 0.0);
}

TEST(CorrectionCache, SecondOrderJet_UnpopulatedAndSingleAxisOobContracts) {
  const CorrSecondOrder empty =
      CorrectionCache{}.eval_second_order(/*k_log=*/0.0, /*T=*/0.5, /*sigma=*/0.3);
  EXPECT_EQ(empty.value, 0.0);
  EXPECT_EQ(empty.dk_log, 0.0);
  EXPECT_EQ(empty.dT, 0.0);
  EXPECT_EQ(empty.dsigma, 0.0);
  EXPECT_EQ(empty.dkk, 0.0);
  EXPECT_EQ(empty.dk_dT, 0.0);
  EXPECT_EQ(empty.dk_dsigma, 0.0);
  EXPECT_EQ(empty.dsigma2, 0.0);

  auto built = CorrectionCache::build(/*n_k=*/16, /*n_T=*/8, /*n_s=*/12,
                                      /*r=*/0.05, /*q=*/0.0,
                                      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
                                      /*T_min=*/0.05, /*T_max=*/2.0,
                                      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  const CorrSecondOrder k_oob = tbl.eval_second_order(0.8, 0.5, 0.3);
  EXPECT_EQ(k_oob.value, tbl.eval(0.8, 0.5, 0.3));
  EXPECT_EQ(k_oob.dk_log, 0.0);
  EXPECT_EQ(k_oob.dkk, 0.0);
  EXPECT_EQ(k_oob.dk_dT, 0.0);
  EXPECT_EQ(k_oob.dk_dsigma, 0.0);

  const CorrSecondOrder T_oob = tbl.eval_second_order(0.0, 0.01, 0.3);
  EXPECT_EQ(T_oob.value, tbl.eval(0.0, 0.01, 0.3));
  EXPECT_EQ(T_oob.dT, 0.0);
  EXPECT_EQ(T_oob.dk_dT, 0.0);

  const CorrSecondOrder sigma_oob = tbl.eval_second_order(0.0, 0.5, 1.0);
  EXPECT_EQ(sigma_oob.value, tbl.eval(0.0, 0.5, 1.0));
  EXPECT_EQ(sigma_oob.dsigma, 0.0);
  EXPECT_EQ(sigma_oob.dk_dsigma, 0.0);
  EXPECT_EQ(sigma_oob.dsigma2, 0.0);
}

TEST(CorrectionCache, ContainsRequiresPopulatedFinitePointInsideClosedBox) {
  const CorrectionCache empty;
  EXPECT_FALSE(empty.contains(0.0, 0.5, 0.3));

  auto built = CorrectionCache::build(/*n_k=*/8, /*n_T=*/6, /*n_s=*/6,
                                      /*r=*/0.05, /*q=*/0.01,
                                      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
                                      /*T_min=*/0.05, /*T_max=*/1.0,
                                      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache cache = std::move(*built);

  EXPECT_TRUE(cache.contains(-0.5, 0.05, 0.10));
  EXPECT_TRUE(cache.contains(0.5, 1.0, 0.80));
  EXPECT_TRUE(cache.contains(0.0, 0.5, 0.30));
  EXPECT_FALSE(cache.contains(std::nextafter(-0.5, -1.0), 0.5, 0.30));
  EXPECT_FALSE(cache.contains(std::nextafter(0.5, 1.0), 0.5, 0.30));
  EXPECT_FALSE(cache.contains(0.0, std::nextafter(0.05, 0.0), 0.30));
  EXPECT_FALSE(cache.contains(0.0, 0.5, std::nextafter(0.80, 1.0)));
  EXPECT_FALSE(cache.contains(std::numeric_limits<double>::quiet_NaN(), 0.5, 0.30));
  EXPECT_FALSE(cache.contains(0.0, std::numeric_limits<double>::infinity(), 0.30));
}

TEST(CorrectionCache, ClampedZeroCorrectionHasZeroServedDerivatives) {
  // A non-dividend-paying call has no early-exercise premium. This gives a
  // naturally zero correction surface and locks the derivative contract to the
  // max(0, polynomial) value actually served by eval()/cached pricing.
  auto built = CorrectionCache::build(
      /*n_k=*/8, /*n_T=*/6, /*n_s=*/6, /*r=*/0.05, /*q=*/0.0,
      /*k_log_min=*/-0.3, /*k_log_max=*/0.3,
      /*T_min=*/0.05, /*T_max=*/1.0,
      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Call);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache cache = std::move(*built);
  constexpr double k = -0.08;
  constexpr double T = 0.50;
  constexpr double sigma = 0.30;
  ASSERT_EQ(cache.eval(k, T, sigma), 0.0);

  const CorrSecondOrder jet = cache.eval_second_order(k, T, sigma);
  EXPECT_EQ(jet.value, 0.0);
  EXPECT_EQ(jet.dk_log, 0.0);
  EXPECT_EQ(jet.dT, 0.0);
  EXPECT_EQ(jet.dsigma, 0.0);
  EXPECT_EQ(jet.dkk, 0.0);
  EXPECT_EQ(jet.dk_dT, 0.0);
  EXPECT_EQ(jet.dk_dsigma, 0.0);
  EXPECT_EQ(jet.dsigma2, 0.0);

  const CorrectionBlend single = CorrectionBlend::single(&cache);
  EXPECT_EQ(single.eval_dsigma(k, T, sigma), 0.0);

  constexpr double S = 100.0;
  const double F = S * std::exp(0.05 * T);
  const double K = F * std::exp(k);
  const double df = std::exp(-0.05 * T);
  const double black_vega =
      atx::vol::black76_greeks(F, K, T, sigma, 0.05, df, Side::Call).greeks.vega;
  EXPECT_EQ(atx::vol::american_vega(S, K, T, sigma, 0.05, 0.0, Side::Call, &cache), black_vega);
}

TEST(CorrectionBlend, EndpointsAndMidpointBlendEveryDerivative) {
  auto lower_result = CorrectionCache::build(
      /*n_k=*/16, /*n_T=*/8, /*n_s=*/12, /*r=*/0.04, /*q=*/0.00,
      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
      /*T_min=*/0.05, /*T_max=*/2.0,
      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  auto upper_result = CorrectionCache::build(
      /*n_k=*/16, /*n_T=*/8, /*n_s=*/12, /*r=*/0.06, /*q=*/0.03,
      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
      /*T_min=*/0.05, /*T_max=*/2.0,
      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  ASSERT_TRUE(lower_result.has_value());
  ASSERT_TRUE(upper_result.has_value());
  const CorrectionCache lower = std::move(*lower_result);
  const CorrectionCache upper = std::move(*upper_result);
  constexpr double k = -0.08;
  constexpr double T = 0.65;
  constexpr double sigma = 0.32;

  const CorrectionBlend single = CorrectionBlend::single(&lower);
  ASSERT_TRUE(single.usable(Side::Put));
  EXPECT_EQ(single.eval(k, T, sigma), lower.eval(k, T, sigma));
  EXPECT_EQ(single.eval_second_order(k, T, sigma).value,
            lower.eval_second_order(k, T, sigma).value);

  const CorrectionBlend upper_endpoint{&lower, &upper, 1.0};
  ASSERT_TRUE(upper_endpoint.usable(Side::Put));
  EXPECT_EQ(upper_endpoint.eval(k, T, sigma), upper.eval(k, T, sigma));

  constexpr double weight = 0.35;
  const CorrectionBlend blend{&lower, &upper, weight};
  ASSERT_TRUE(blend.usable(Side::Put));
  const CorrSecondOrder lo = lower.eval_second_order(k, T, sigma);
  const CorrSecondOrder hi = upper.eval_second_order(k, T, sigma);
  const CorrSecondOrder got = blend.eval_second_order(k, T, sigma);
  const auto expected = [](double a, double b) { return a + weight * (b - a); };
  EXPECT_EQ(got.value, expected(lo.value, hi.value));
  EXPECT_EQ(got.dk_log, expected(lo.dk_log, hi.dk_log));
  EXPECT_EQ(got.dT, expected(lo.dT, hi.dT));
  EXPECT_EQ(got.dsigma, expected(lo.dsigma, hi.dsigma));
  EXPECT_EQ(got.dkk, expected(lo.dkk, hi.dkk));
  EXPECT_EQ(got.dk_dT, expected(lo.dk_dT, hi.dk_dT));
  EXPECT_EQ(got.dk_dsigma, expected(lo.dk_dsigma, hi.dk_dsigma));
  EXPECT_EQ(got.dsigma2, expected(lo.dsigma2, hi.dsigma2));

  double lo_dk = 0.0;
  double blended_dk = 0.0;
  EXPECT_NEAR(lower.eval_value_dk(k, T, sigma, &lo_dk), lo.value, 1.0e-13);
  EXPECT_NEAR(lo_dk, lo.dk_log, 1.0e-12);
  EXPECT_NEAR(blend.eval_value_dk(k, T, sigma, &blended_dk), got.value, 1.0e-13);
  EXPECT_NEAR(blended_dk, got.dk_log, 1.0e-12);

  double lo_dsigma = 0.0;
  double hi_dsigma = 0.0;
  lower.eval_partials(k, T, sigma, nullptr, nullptr, &lo_dsigma);
  upper.eval_partials(k, T, sigma, nullptr, nullptr, &hi_dsigma);
  EXPECT_EQ(blend.eval_dsigma(k, T, sigma), expected(lo_dsigma, hi_dsigma));

  constexpr double hT = 1.0e-5;
  const double finite_difference =
      (blend.eval(k, T + hT, sigma) - blend.eval(k, T - hT, sigma)) / (2.0 * hT);
  EXPECT_NEAR(got.dT, finite_difference, 1.0e-7);
}

// Perf review F1 stage (b): eval_value_and_dsigma is a SINGLE fused Clenshaw pass
// (sigma is the last collapse axis). The VALUE stays byte-for-byte eval() (so the
// Newton residual is unchanged), while the sigma partial is the value+derivative
// recurrence — bit-identical to eval_second_order()'s dsigma (same recurrence) and
// economically equal to eval_partials()'s differentiate-then-evaluate dsigma (same
// analytic derivative, different accumulation). Interior points plus out-of-box on
// each axis (value clamps, partial zeroes out of the sigma box).
TEST(CorrectionCache, EvalValueAndDsigmaFusedPass_ValueExact_DsigmaParity) {
  auto built = CorrectionCache::build(
      /*n_k=*/16, /*n_T=*/8, /*n_s=*/12, /*r=*/0.05, /*q=*/0.00,
      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
      /*T_min=*/0.05, /*T_max=*/2.0,
      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache cache = std::move(*built);

  for (double k : {-0.6, -0.2, 0.0, 0.15, 0.55}) {
    for (double T : {0.02, 0.2, 0.9, 2.4}) {
      for (double sigma : {0.05, 0.18, 0.5, 0.9}) {
        double ds_partials = 0.0;
        const double v_ref = cache.eval_grad(k, T, sigma, nullptr, nullptr, &ds_partials);
        double ds_fused = 0.0;
        const double v_fused = cache.eval_value_and_dsigma(k, T, sigma, &ds_fused);
        // Value: bit-identical to eval()/eval_grad (the residual cannot move).
        EXPECT_EQ(v_fused, v_ref) << "k=" << k << " T=" << T << " sig=" << sigma;
        EXPECT_EQ(v_fused, cache.eval(k, T, sigma));
        // dsigma: economic-parity with eval_partials' differentiate-then-evaluate.
        EXPECT_NEAR(ds_fused, ds_partials, 1.0e-9 * std::fabs(ds_partials) + 1.0e-12)
            << "k=" << k << " T=" << T << " sig=" << sigma;
        // dsigma is bit-identical to the second-order jet's dsigma (same recurrence)
        // where both are served (correction active AND sigma in the box).
        const bool sigma_in_box = (sigma >= 0.10 && sigma <= 0.80);
        if (v_fused > 0.0 && sigma_in_box) {
          EXPECT_EQ(ds_fused, cache.eval_second_order(k, T, sigma).dsigma)
              << "k=" << k << " T=" << T << " sig=" << sigma;
        }
      }
    }
  }
}

// The blend fused entry's VALUE equals eval() byte-for-byte (underwrites the IV
// "blend endpoint == single cache exactly" pin); its dsigma economically matches
// eval_dsigma() (stage b: the fused derivative recurrence, ULP-different accumulation
// of the same analytic derivative). Single-cache (weight 0/1, identical pointers)
// and interior-blend cases.
TEST(CorrectionBlend, EvalValueAndDsigmaFusedPass_ValueExact_DsigmaParity) {
  auto lower_result =
      CorrectionCache::build(16, 8, 12, 0.04, 0.00, -0.5, 0.5, 0.05, 2.0, 0.10, 0.80, Side::Put);
  auto upper_result =
      CorrectionCache::build(16, 8, 12, 0.06, 0.03, -0.5, 0.5, 0.05, 2.0, 0.10, 0.80, Side::Put);
  ASSERT_TRUE(lower_result.has_value());
  ASSERT_TRUE(upper_result.has_value());
  const CorrectionCache lower = std::move(*lower_result);
  const CorrectionCache upper = std::move(*upper_result);

  const CorrectionBlend single = CorrectionBlend::single(&lower);
  const CorrectionBlend hi_endpoint{&lower, &upper, 1.0};
  const CorrectionBlend interior{&lower, &upper, 0.35};
  for (const CorrectionBlend *blend : {&single, &hi_endpoint, &interior}) {
    for (double k : {-0.6, -0.1, 0.0, 0.2, 0.55}) {
      for (double T : {0.02, 0.35, 1.5}) {
        for (double sigma : {0.05, 0.25, 0.9}) {
          double ds_fused = 0.0;
          const double v_fused = blend->eval_value_and_dsigma(k, T, sigma, &ds_fused);
          EXPECT_EQ(v_fused, blend->eval(k, T, sigma)) << "k=" << k << " T=" << T;
          const double ds_ref = blend->eval_dsigma(k, T, sigma);
          EXPECT_NEAR(ds_fused, ds_ref, 1.0e-9 * std::fabs(ds_ref) + 1.0e-12)
              << "k=" << k << " T=" << T;
        }
      }
    }
  }
}

TEST(CorrectionBlend, RejectsInvalidWeightMissingEndpointAndMixedSide) {
  auto put_result =
      CorrectionCache::build(8, 6, 6, 0.04, 0.0, -0.3, 0.3, 0.05, 1.0, 0.1, 0.8, Side::Put);
  auto call_result =
      CorrectionCache::build(8, 6, 6, 0.04, 0.02, -0.3, 0.3, 0.05, 1.0, 0.1, 0.8, Side::Call);
  ASSERT_TRUE(put_result.has_value());
  ASSERT_TRUE(call_result.has_value());
  const CorrectionCache put = std::move(*put_result);
  const CorrectionCache call = std::move(*call_result);

  EXPECT_FALSE((CorrectionBlend{&put, nullptr, 0.5}).usable(Side::Put));
  EXPECT_FALSE((CorrectionBlend{&put, &call, 0.5}).usable(Side::Put));
  EXPECT_FALSE((CorrectionBlend{&put, &put, -0.1}).usable(Side::Put));
  EXPECT_FALSE(
      (CorrectionBlend{&put, &put, std::numeric_limits<double>::quiet_NaN()}).usable(Side::Put));
}

// ── P5 (perf review F4): equal-T cached ladder batch ─────────────────────────
//
// american_price_cached_ladder pre-collapses the correction tensor's T (j) axis
// ONCE per usable endpoint and prices each strike with a 2-D Clenshaw over the
// (k_log, sigma) plane. The T-first collapse reorders the tensor summation, so it
// is ECONOMIC-parity — not bit-identical — to the per-strike american_price_cached:
// |Δprice| < 1e-12·K (the sprint's ladder-parity budget).

// A smile-ish per-strike vol that stays inside the caches' [sigma_min, sigma_max].
[[nodiscard]] double p5_ladder_sigma(double k_log) noexcept { return 0.28 + 0.12 * k_log * k_log; }

TEST(CorrectionCache, LadderBatchEconomicParityToPerStrikeCached) {
  struct Case {
    double r, q;
    Side side;
  };
  const std::array<Case, 2> cases = {Case{0.05, 0.00, Side::Put}, Case{0.045, 0.02, Side::Call}};
  const double S = 100.0;

  double worst_rel = 0.0;
  for (const Case c : cases) {
    auto built =
        CorrectionCache::build(16, 12, 8, c.r, c.q, -0.4, 0.4, 0.05, 1.0, 0.10, 0.60, c.side);
    ASSERT_TRUE(built.has_value());
    const CorrectionCache cache = std::move(*built);
    const CorrectionBlend blend = CorrectionBlend::single(&cache);

    // Strikes span in-box AND beyond both wings (exercising the k_log clamp region,
    // where scalar and batch clamp identically so parity must still hold).
    std::vector<double> strikes;
    for (double K = 55.0; K <= 175.0; K += 2.5) {
      strikes.push_back(K);
    }
    for (const double T : {0.12, 0.55, 0.98}) {
      const double F = S * std::exp((c.r - c.q) * T);
      std::vector<double> sigmas;
      sigmas.reserve(strikes.size());
      for (const double K : strikes) {
        sigmas.push_back(p5_ladder_sigma(std::log(K / F)));
      }
      std::vector<double> batch(strikes.size(), 0.0);
      const auto st = atx::vol::american_price_cached_ladder(S, strikes, sigmas, T, c.r, c.q, c.side,
                                                             blend, batch);
      ASSERT_TRUE(st.has_value()) << st.error().to_string();

      for (std::size_t i = 0; i < strikes.size(); ++i) {
        const double scalar = atx::vol::american_price_cached(S, strikes[i], T, sigmas[i], c.r, c.q,
                                                              c.side, blend);
        ASSERT_TRUE(std::isfinite(batch[i])) << "non-finite batch price @ K=" << strikes[i];
        const double rel = std::fabs(batch[i] - scalar) / strikes[i];
        worst_rel = std::max(worst_rel, rel);
        EXPECT_LT(rel, 1.0e-12) << "side=" << (c.side == Side::Put ? "put" : "call") << " T=" << T
                                << " K=" << strikes[i];
      }
    }
  }
  std::cout << "[P5 F4 parity] single-cache ladder max|Δprice|/K = " << worst_rel << "\n";
}

TEST(CorrectionCache, LadderBatchInteriorBlendParityAndFallbacks) {
  const double S = 100.0, T = 0.5, r = 0.05, q = 0.0;
  // Two put caches at neighbouring baked carries -> an interior (0<w<1) blend, the
  // C2 cross-carry cache-reuse shape. Query carry sits between the two.
  auto lo = CorrectionCache::build(16, 12, 8, 0.04, 0.0, -0.4, 0.4, 0.05, 1.0, 0.10, 0.60, Side::Put);
  auto hi = CorrectionCache::build(16, 12, 8, 0.06, 0.0, -0.4, 0.4, 0.05, 1.0, 0.10, 0.60, Side::Put);
  ASSERT_TRUE(lo.has_value() && hi.has_value());
  const CorrectionCache lo_c = std::move(*lo);
  const CorrectionCache hi_c = std::move(*hi);
  const CorrectionBlend blend{&lo_c, &hi_c, 0.5};
  ASSERT_TRUE(blend.usable(Side::Put));

  std::vector<double> strikes;
  for (double K = 70.0; K <= 135.0; K += 2.5) {
    strikes.push_back(K);
  }
  const double F = S * std::exp((r - q) * T);
  std::vector<double> sigmas;
  for (const double K : strikes) {
    sigmas.push_back(p5_ladder_sigma(std::log(K / F)));
  }
  std::vector<double> batch(strikes.size(), 0.0);
  ASSERT_TRUE(
      atx::vol::american_price_cached_ladder(S, strikes, sigmas, T, r, q, Side::Put, blend, batch)
          .has_value());
  double worst_rel = 0.0;
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double scalar =
        atx::vol::american_price_cached(S, strikes[i], T, sigmas[i], r, q, Side::Put, blend);
    worst_rel = std::max(worst_rel, std::fabs(batch[i] - scalar) / strikes[i]);
  }
  EXPECT_LT(worst_rel, 1.0e-12) << "interior blend max|Δprice|/K=" << worst_rel;
  std::cout << "[P5 F4 parity] interior-blend ladder max|Δprice|/K = " << worst_rel << "\n";

  // Bad strikes (<=0 / NaN) become NaN in place, preserving row order.
  const std::vector<double> mixed_strikes = {90.0, -1.0, 110.0,
                                             std::numeric_limits<double>::quiet_NaN()};
  const std::vector<double> mixed_sigmas = {0.30, 0.30, 0.30, 0.30};
  std::vector<double> mixed_out(4, 0.0);
  ASSERT_TRUE(atx::vol::american_price_cached_ladder(S, mixed_strikes, mixed_sigmas, T, r, q,
                                                     Side::Put, blend, mixed_out)
                  .has_value());
  EXPECT_TRUE(std::isfinite(mixed_out[0]));
  EXPECT_TRUE(std::isnan(mixed_out[1]));
  EXPECT_TRUE(std::isfinite(mixed_out[2]));
  EXPECT_TRUE(std::isnan(mixed_out[3]));

  // An unusable blend (wrong side) delegates to the scalar cached entry, so the
  // batch is BIT-for-bit the scalar there (same function, no plane path taken).
  const CorrectionBlend put_only = CorrectionBlend::single(&lo_c);
  ASSERT_FALSE(put_only.usable(Side::Call));
  std::vector<double> call_out(strikes.size(), 0.0);
  ASSERT_TRUE(atx::vol::american_price_cached_ladder(S, strikes, sigmas, T, r, q, Side::Call,
                                                     put_only, call_out)
                  .has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double scalar =
        atx::vol::american_price_cached(S, strikes[i], T, sigmas[i], r, q, Side::Call, put_only);
    if (std::isnan(scalar)) {
      EXPECT_TRUE(std::isnan(call_out[i]));
    } else {
      EXPECT_DOUBLE_EQ(call_out[i], scalar);
    }
  }

  // Structural rejection: strikes/price length mismatch.
  std::vector<double> short_out(strikes.size() - 1, 0.0);
  EXPECT_FALSE(atx::vol::american_price_cached_ladder(S, strikes, sigmas, T, r, q, Side::Put, blend,
                                                      short_out)
                   .has_value());
}

// Counter gate (perf review F4): the equal-T ladder does ONE full-tensor traversal
// (the T-collapse) per usable endpoint regardless of strike count, where the
// per-strike scalar path does one full 3-D Clenshaw sweep PER strike. Cite the
// ClenshawSweeps delta; skip on an OFF build so the counters-owner confirms on ON.
TEST(CorrectionCache, LadderBatchClenshawSweepCountIsStrikeCountIndependent) {
  using atx::vol::counters::Counter;
  if constexpr (!atx::vol::counters::counters_enabled()) {
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  }
  auto built = CorrectionCache::build(16, 12, 8, 0.05, 0.0, -0.4, 0.4, 0.05, 1.0, 0.10, 0.60,
                                      Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache cache = std::move(*built);
  const CorrectionBlend blend = CorrectionBlend::single(&cache);
  const double S = 100.0, r = 0.05, q = 0.0, T = 0.5;
  const double F = S * std::exp((r - q) * T);
  std::vector<double> strikes;
  std::vector<double> sigmas;
  for (double K = 70.0; K <= 130.0; K += 2.0) {
    strikes.push_back(K);
    sigmas.push_back(p5_ladder_sigma(std::log(K / F)));
  }
  const std::uint64_t n = strikes.size();

  atx::vol::counters::reset();
  std::vector<double> scalar(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    scalar[i] = atx::vol::american_price_cached(S, strikes[i], T, sigmas[i], r, q, Side::Put, blend);
  }
  const std::uint64_t scalar_sweeps = atx::vol::counters::snapshot().get(Counter::ClenshawSweeps);

  atx::vol::counters::reset();
  std::vector<double> batch(n, 0.0);
  ASSERT_TRUE(
      atx::vol::american_price_cached_ladder(S, strikes, sigmas, T, r, q, Side::Put, blend, batch)
          .has_value());
  const std::uint64_t batch_sweeps = atx::vol::counters::snapshot().get(Counter::ClenshawSweeps);

  std::cout << "[P5 F4 counters] " << n << " single-cache strikes: scalar " << scalar_sweeps
            << " ClenshawSweeps vs ladder " << batch_sweeps
            << " (per-strike 3-D sweep -> one T-collapse per ladder)\n";
  EXPECT_EQ(scalar_sweeps, n); // one full 3-D Clenshaw sweep per strike
  EXPECT_EQ(batch_sweeps, 1u); // one T-collapse per (ladder, endpoint); plane evals uncounted
}

// Frozen value/first-partial pins remain rounding-scale references. Their live
// algebraic contracts are checked independently below because harmless changes
// to Andersen-Lake instrumentation can alter register allocation while building
// the cache and move the resulting coefficients by rounding-scale amounts. The
// former Greek bundle bits are likewise a reference because the second-order jet
// replaces its finite-difference approximation with analytic interpolant derivatives.
namespace pin {

using atx::vol::american_greeks;
using atx::vol::AmericanGreeks;

[[nodiscard]] std::uint64_t bits(double d) noexcept { return std::bit_cast<std::uint64_t>(d); }

[[nodiscard]] double rounding_tolerance(double reference) noexcept {
  const double base = 4.0 * std::numeric_limits<double>::epsilon() * std::fmax(1.0, std::fabs(reference));
  // M4: 4 ULP on the SSE2 source-of-truth ISA; widened to 32 ULP under FMA-
  // contracting builds (rel-avx2), where the interpolant's dsigma partial fuses
  // a*b+c and drifts ~3e-15 (~13 ULP) from the pinned SSE2 value — contraction,
  // not a regression. See support/isa_golden_tol.hpp. golden_isa_tol() returns 0
  // on the reference ISA, so the SSE2 gate keeps its 4-ULP strictness.
  return std::fmax(base, atx::vol::test::golden_isa_tol(reference));
}

// Deterministic put cache at production-shaped dims (16 x 8 x 12), r>0/q>=0 carry
// (a valid American regime, per Task 1). build() is deterministic given its args.
[[nodiscard]] CorrectionCache make_pin_cache() {
  auto built = CorrectionCache::build(/*n_k=*/16, /*n_T=*/8, /*n_s=*/12,
                                      /*r=*/0.05, /*q=*/0.0, /*k_log_min=*/-0.5,
                                      /*k_log_max=*/0.5, /*T_min=*/30.0 / 365.25,
                                      /*T_max=*/2.0, /*sigma_min=*/0.10,
                                      /*sigma_max=*/0.80, Side::Put);
  EXPECT_TRUE(built.has_value());
  return std::move(*built);
}

// Query points: three interior, then one out-of-box per axis (pins the clamp +
// oob-partial-zeroing that eval_partials must reproduce).
struct QPt {
  double k_log, T, sigma;
};
constexpr std::array<QPt, 6> kQ = {{
    {0.00, 0.25, 0.30},
    {-0.30, 1.00, 0.50},
    {0.40, 0.15, 0.20},
    {0.80, 0.25, 0.30}, // k_log > box  -> oob_k
    {0.00, 0.05, 0.30}, // T < box      -> oob_T
    {0.00, 0.25, 0.95}, // sigma > box  -> oob_s
}};

// american_greeks points (r=0.05, q=0.0, Put — matches the cache); all land the
// internal k_log = log(K/F) inside the box (the common cached path).
struct GPt {
  double S, K, T, sigma;
};
constexpr std::array<GPt, 3> kG = {{
    {100.0, 100.0, 0.25, 0.30},
    {100.0, 110.0, 0.50, 0.45},
    {100.0, 90.0, 0.15, 0.25},
}};

// Historical reference patterns captured from matching Debug/Release builds
// (SSE2, no fast-math). Columns: {eval, eval_grad_value, dk, dT, dsigma}. Rows
// 3/4/5 pin the out-of-box clamp + zeroed partial on each axis.
//
// T16a repin: the Side::Put cache builder now collapses each k_log row onto ONE
// andersen_lake_put_slice boundary solve (was the scalar per-node andersen_lake).
// The reused boundary is homogeneity-exact in ℝ but ~a few ULP off a fresh
// per-strike solve in IEEE, so the sampled put correction shifts ~1e-7 and these
// pins moved by a few ULP each (e.g. row-0 eval …7c56 -> …7c52). Recaptured from
// the slice-built cache and cross-validated to the §9 gates by the tolerance
// anchors that ALSO run on this cache: PopulateEval_MatchesAndersenLake_PutGrid
// and CachedPrice_MatchesColdAndersenLake (correction, vs cold andersen_lake),
// and the AmericanGreeks.*_MatchesFd_* bundle-accuracy tests (american_test) —
// all green with margin on the new cache.
// A1 REPIN (core-review finding 1): the pin cache samples the cold andersen_lake
// put, whose BAW critical-price seed sign was fixed; the more-accurate seed shifts
// the sampled correction ~1e-7 (rel), so these eval/partial pins moved a few ULP
// each (e.g. row-0 eval …7c52 -> …7856). The cache is still correct — the tolerance
// anchors CorrectionCache.PopulateEval_MatchesAndersenLake_* and
// CachedPrice_MatchesColdAndersenLake (vs cold andersen_lake) stay green on it.
// Recaptured on the SSE2 reference ISA (dev preset).
constexpr std::array<std::array<std::uint64_t, 5>, 6> kEvalPins = {{
    {{0x3f53d82af89b7856, 0x3f53d82af89b7856, 0x3f8d4b33a257384e, 0x3f772527166308ae,
      0xbf62be052db50530}},
    {{0x3f5ae5631f0a9a20, 0x3f5ae5631f0a9a20, 0x3f8026c9eb79d812, 0x3f69f661e8a21785,
      0x3f7085dfed71640e}},
    {{0x3f86c3267b896d77, 0x3f86c3267b896d77, 0x3f7a56ab925bc3d4, 0x3fb32d452abf5401,
      0x3f5ffd3e95176812}},
    {{0x3f94f6c10e86dc8e, 0x3f94f6c10e86dc8e, 0x0000000000000000, 0x3fb4b7187e5644e4,
      0xbf402073e933ef60}},
    {{0x3f31df73a9da15d1, 0x3f31df73a9da15d1, 0x3f78419947faa0de, 0x0000000000000000,
      0x3f6746ac5e5cee78}},
    {{0x3f50fa372a2953b2, 0x3f50fa372a2953b2, 0x3f74c8b6e0179649, 0x3f7598068a6f8e6a,
      0x0000000000000000}},
}};

// american_greeks bundle bits: {delta,gamma,vega,theta,rho,vanna,volga,charm,price}.
// T16a-repinned (see kEvalPins note); validated against the AmericanGreeks.*_MatchesFd_*
// accuracy tests, which recompute the bundle vs finite differences on this same cache.
// A1 REPIN (core-review finding 1, same cause as kEvalPins): the cached greek
// bundle differentiates the shifted correction interpolant, so the pinned fields
// moved — first-order greeks a few ULP, the second-order vanna/volga more (a mixed/
// second sigma derivative amplifies the ~1e-7 cache shift, e.g. volga ~42 moved
// ~2.6e-3, ~6e-5 rel, well inside its 1e-4 pin tol). Charm (col 7) is unpinned in
// the test — its algorithmic contract is checked against a cross-difference of the
// price served by THIS cache. Values recaptured on the SSE2 reference ISA and
// corroborated by the AmericanGreeks.*_MatchesFd_* bundle-accuracy tests.
constexpr std::array<std::array<std::uint64_t, 9>, 3> kGreekPins = {{
    {{0xbfdcc14396d1f4dd, 0x3f9bc19bd999da7f, 0x40338baa061f4cab, 0xc023a6148a4d1e7c,
      0xc029229e40e5b617, 0x3faa97f8de455e4f, 0xbfeed99e59e5b1c0, 0xbfb7bcb29e0073e4,
      0x4015c8e2f34b2703}},
    {{0xbfe15513afd342ae, 0x3f8b8257542f5e57, 0x403bbc0faf6e555a, 0xc023f8276667ba3e,
      0xc041ce3ecb05eced, 0x3fd91d3883b1d011, 0x4022fbda8e00fdfb, 0xbfc90fcf4d746e0f,
      0x403173bba96aedd1}},
    {{0xbfbd51251392730b, 0x3f9436fbb7ba4035, 0x401d22055f3e865f, 0xc0164b73e077465b,
      0xbffcce24a9f65ce5, 0xbfea64c976d97cb8, 0x404512a9813feb05, 0x3fe39cbbbebe53ae,
      0x3fe1e55889976930}},
}};

TEST(Pin, EvalAndEvalGradMatchPinnedValuesWithinRounding) {
  const CorrectionCache tbl = make_pin_cache();
  for (std::size_t i = 0; i < kQ.size(); ++i) {
    const QPt p = kQ[i];
    const double v = tbl.eval(p.k_log, p.T, p.sigma);
    double dk = 0, dT = 0, ds = 0;
    const double vg = tbl.eval_grad(p.k_log, p.T, p.sigma, &dk, &dT, &ds);
    const double expected_v = std::bit_cast<double>(kEvalPins[i][0]);
    const double expected_vg = std::bit_cast<double>(kEvalPins[i][1]);
    const double expected_dk = std::bit_cast<double>(kEvalPins[i][2]);
    const double expected_dT = std::bit_cast<double>(kEvalPins[i][3]);
    const double expected_ds = std::bit_cast<double>(kEvalPins[i][4]);
    EXPECT_NEAR(v, expected_v, rounding_tolerance(expected_v)) << "eval @" << i;
    EXPECT_NEAR(vg, expected_vg, rounding_tolerance(expected_vg)) << "eval_grad value @" << i;
    EXPECT_NEAR(dk, expected_dk, rounding_tolerance(expected_dk)) << "dk @" << i;
    EXPECT_NEAR(dT, expected_dT, rounding_tolerance(expected_dT)) << "dT @" << i;
    EXPECT_NEAR(ds, expected_ds, rounding_tolerance(expected_ds)) << "dsigma @" << i;
    EXPECT_EQ(bits(v), bits(vg)) << "eval vs eval_grad value @" << i; // public contract
  }
}

TEST(Pin, AmericanGreeksSecondOrderJetMatchesStablePinsAndCachedPriceCharm) {
  const CorrectionCache tbl = make_pin_cache();
  // The fused second-order jet differentiates the interpolant analytically;
  // stable fields remain constrained to their former values. Charm is a mixed
  // second derivative and is sensitive to rounding-scale changes in the cache
  // coefficients, so its algorithmic contract is checked against an independent
  // cross-difference of the price served by this cache instead of a stale pin.
  constexpr std::array<double, 9> tolerance = {1.0e-9, 1.0e-7, 1.0e-9, 1.0e-9, 1.0e-9,
                                               1.0e-6, 1.0e-4, 0.0,    1.0e-10};
  for (std::size_t i = 0; i < kG.size(); ++i) {
    const GPt g = kG[i];
    const auto res = american_greeks(g.S, g.K, g.T, g.sigma, 0.05, 0.0, Side::Put, &tbl);
    ASSERT_TRUE(res.has_value());
    const AmericanGreeks &a = *res;
    const std::array<double, 9> got = {a.delta, a.gamma, a.vega,  a.theta, a.rho,
                                       a.vanna, a.volga, a.charm, a.price};
    for (std::size_t f = 0; f < got.size(); ++f) {
      if (f == 7u) {
        continue;
      }
      const double expected = std::bit_cast<double>(kGreekPins[i][f]);
      EXPECT_NEAR(got[f], expected, tolerance[f]) << "field " << f << " @pt " << i;
    }

    constexpr double hS = 0.02;
    constexpr double hT = 1.0e-4;
    const auto price = [&](double spot, double time) {
      return atx::vol::american_price_cached(spot, g.K, time, g.sigma, 0.05, 0.0, Side::Put, &tbl);
    };
    const double charm_fd = -(price(g.S + hS, g.T + hT) - price(g.S + hS, g.T - hT) -
                              price(g.S - hS, g.T + hT) + price(g.S - hS, g.T - hT)) /
                            (4.0 * hS * hT);
    EXPECT_NEAR(a.charm, charm_fd, 2.0e-3) << "charm @pt " << i;
  }
}

// eval_partials must write bit-identical partials to live eval_grad. The frozen
// pre-change pins (columns 2..4) are a rounding-scale guard, and nullptr-skip is
// checked independently.
TEST(Pin, EvalPartialsMatchesEvalGradAndPinnedScale) {
  const CorrectionCache tbl = make_pin_cache();
  for (std::size_t i = 0; i < kQ.size(); ++i) {
    const QPt p = kQ[i];
    double gk = 0, gT = 0, gs = 0;
    tbl.eval_grad(p.k_log, p.T, p.sigma, &gk, &gT, &gs); // reference partials
    double pk = 0, pT = 0, ps = 0;
    tbl.eval_partials(p.k_log, p.T, p.sigma, &pk, &pT, &ps);
    const double expected_k = std::bit_cast<double>(kEvalPins[i][2]);
    const double expected_T = std::bit_cast<double>(kEvalPins[i][3]);
    const double expected_s = std::bit_cast<double>(kEvalPins[i][4]);
    EXPECT_NEAR(pk, expected_k, rounding_tolerance(expected_k)) << "dk pin @" << i;
    EXPECT_NEAR(pT, expected_T, rounding_tolerance(expected_T)) << "dT pin @" << i;
    EXPECT_NEAR(ps, expected_s, rounding_tolerance(expected_s)) << "dsigma pin @" << i;
    EXPECT_EQ(bits(pk), bits(gk)) << "dk vs eval_grad @" << i;
    EXPECT_EQ(bits(pT), bits(gT)) << "dT vs eval_grad @" << i;
    EXPECT_EQ(bits(ps), bits(gs)) << "dsigma vs eval_grad @" << i;

    // nullptr-skip: requesting only dsigma writes exactly the same dsigma bits and
    // touches nothing else (matches the american_greeks FD call sites).
    double only_s = 12345.0;
    tbl.eval_partials(p.k_log, p.T, p.sigma, nullptr, nullptr, &only_s);
    EXPECT_EQ(bits(only_s), bits(ps)) << "dsigma-only @" << i;
  }
}

} // namespace pin

} // namespace
