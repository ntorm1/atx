#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/counters.hpp"

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
using atx::vol::CorrectionCache;
using atx::vol::CorrPartials;
using atx::vol::CorrResult;
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
    if (k == 0) return 1.0;
    if (k == 1) return x;
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
    EXPECT_LT(std::fabs(2.0 * std::cos(2.0 * x) -
                        detail::cheb_clenshaw1d(dcoefs.data(), N, x)),
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
  EXPECT_LT(std::fabs(detail::cheb_clenshaw3d_partial(vals.data(), n_k, n_T, n_s,
                                                      xi, xj, xk, 0, 1.0, tmp.data()) -
                      exact_di),
            1.0e-8);

  const double exact_dj = -std::exp(0.3 * xi) * std::sin(xj) * (1.0 + 0.2 * xk * xk);
  EXPECT_LT(std::fabs(detail::cheb_clenshaw3d_partial(vals.data(), n_k, n_T, n_s,
                                                      xi, xj, xk, 1, 1.0, tmp.data()) -
                      exact_dj),
            1.0e-8);

  const double exact_dk = 0.4 * xk * std::exp(0.3 * xi) * std::cos(xj);
  EXPECT_LT(std::fabs(detail::cheb_clenshaw3d_partial(vals.data(), n_k, n_T, n_s,
                                                      xi, xj, xk, 2, 1.0, tmp.data()) -
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
      const double* row = coefs.data() + detail::cheb_idx(0, j, k, n_k, n_s);
      detail::cheb_diff_coefs(row, drow.data(), n_k, scale_k);
      double* orow = Ck.data() + detail::cheb_idx(0, j, k, n_k, n_s);
      for (std::uint16_t i = 0; i < n_k; ++i) orow[i] = drow[i];
    }

  std::array<double, 64 * 64> tmp{};
  for (int a = -5; a <= 5; ++a)
    for (int b = -5; b <= 5; ++b)
      for (int c = -5; c <= 5; ++c) {
        const double xi = 0.2 * static_cast<double>(a);
        const double xj = 0.2 * static_cast<double>(b);
        const double xk = 0.2 * static_cast<double>(c);
        const double live = detail::cheb_clenshaw3d_partial(
            coefs.data(), n_k, n_T, n_s, xi, xj, xk, 0, scale_k, tmp.data());
        const double pre =
            detail::cheb_clenshaw3d(Ck.data(), n_k, n_T, n_s, xi, xj, xk, tmp.data());
        EXPECT_EQ(std::bit_cast<std::uint64_t>(pre), std::bit_cast<std::uint64_t>(live))
            << "k_log partial precompute vs live @(" << xi << "," << xj << "," << xk << ")";
      }
}

// ── Cache build / eval ──────────────────────────────────────────────────

TEST(CorrectionCache, PopulateEval_MatchesAndersenLake_PutGrid) {
  const double r = 0.05, q = 0.0;
  auto built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5,
                                      30.0 / 365.25, 2.0, 0.10, 0.80, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  std::uint64_t seed = 0x1234567890abcdefULL;
  double max_abs_err = 0.0;
  double sum_sq_err = 0.0;
  int n_eval = 0;
  for (int i = 0; i < 200; ++i) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    const double u1 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    const double u2 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
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
  auto built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5,
                                      30.0 / 365.25, 2.0, 0.10, 0.80, Side::Call);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);

  std::uint64_t seed = 0xdeadbeef12345678ULL;
  double max_abs_err = 0.0;
  int n_eval = 0;
  for (int i = 0; i < 200; ++i) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    const double u1 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    const double u2 = static_cast<double>(seed & 0xFFFFFFFFu) / 4294967296.0;
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
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
  auto built = CorrectionCache::build(24, 16, 12, 0.05, 0.0, -0.5, 0.5,
                                      30.0 / 365.25, 2.0, 0.10, 0.80, Side::Put);
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
  auto built = CorrectionCache::build(8, 6, 6, 0.05, 0.0, -0.3, 0.3, 0.10, 1.0,
                                      0.15, 0.60, Side::Put);
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

  const auto in_box =
      tbl.query(0.05, 0.4, 0.30, CorrPartials::Value | CorrPartials::Dsigma);
  ASSERT_TRUE(in_box.has_value());
  EXPECT_TRUE(std::isfinite(in_box->value));

  const auto oob_k =
      tbl.query(0.5, 0.4, 0.30, CorrPartials::Value | CorrPartials::Dsigma);
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
  auto built = CorrectionCache::build(24, 16, 12, r, q, -0.5, 0.5,
                                      30.0 / 365.25, 2.0, 0.10, 0.80, Side::Put);
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
    auto built = CorrectionCache::build(n_k, n_T, n_s, 0.05, 0.0, -0.5, 0.5,
                                        30.0 / 365.25, 2.0, 0.10, 0.80, Side::Put);
    ASSERT_TRUE(built.has_value());
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  } else {
    atx::vol::counters::reset();
    auto built = CorrectionCache::build(n_k, n_T, n_s, 0.05, 0.0, -0.5, 0.5,
                                        30.0 / 365.25, 2.0, 0.10, 0.80, Side::Put);
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
  auto built = CorrectionCache::build(n_k, n_T, n_s, 0.05, 0.0, -0.5, 0.5,
                                      30.0 / 365.25, 2.0, 0.10, 0.80, Side::Put);
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
  auto rp = CorrectionCache::build(8, 8, 8, -0.005, -0.02, -0.5, 0.5, 0.1, 1.0,
                                   0.1, 0.5, Side::Put);
  ASSERT_FALSE(rp.has_value());
  EXPECT_EQ(rp.error().code(), atx::core::ErrorCode::NotImplemented);
  // Double-continuation CALL (r < q <= 0).
  auto rc = CorrectionCache::build(8, 8, 8, -0.02, -0.005, -0.5, 0.5, 0.1, 1.0,
                                   0.1, 0.5, Side::Call);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), atx::core::ErrorCode::NotImplemented);
  // European corner (put r <= 0 && r <= q) is NOT unsupported — it still builds.
  auto re = CorrectionCache::build(8, 8, 8, -0.02, -0.005, -0.5, 0.5, 0.1, 1.0,
                                   0.1, 0.5, Side::Put);
  EXPECT_TRUE(re.has_value());
}

// Fix-wave 1b: the POPULATED-cache hot path must return NaN when queried in the
// Unsupported regime (the guard keys off the query's (r, q), not the cache's).
TEST(CorrectionCache, CachedPrice_UnsupportedRegime_ReturnsNaN) {
  const double r = 0.05, q = 0.0;  // American put — a valid, populated cache
  auto built = CorrectionCache::build(16, 12, 8, r, q, -0.5, 0.5, 0.1, 2.0, 0.1,
                                      0.8, Side::Put);
  ASSERT_TRUE(built.has_value());
  const CorrectionCache tbl = std::move(*built);
  // Query at an Unsupported (r, q): guard must surface NaN, not euro+F*corr.
  const double px = atx::vol::american_price_cached(100.0, 100.0, 1.0, 0.30,
                                                    -0.005, -0.02, Side::Put, &tbl);
  EXPECT_TRUE(std::isnan(px));
}

// ── Task 3: bit-identity pins for the value-sweep / scratch waste removal ──
//
// These lock the exact double bit patterns of eval / eval_grad / eval_partials
// and the american_greeks bundle so that removing the discarded value sweep and
// right-sizing the Clenshaw scratch is provably output-preserving. The expected
// hex was captured from the PRE-change Release/Debug build; if any of these move,
// the "pure waste removal" claim is false.
namespace pin {

using atx::vol::american_greeks;
using atx::vol::AmericanGreeks;

[[nodiscard]] std::uint64_t bits(double d) noexcept {
  return std::bit_cast<std::uint64_t>(d);
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
struct QPt { double k_log, T, sigma; };
constexpr std::array<QPt, 6> kQ = {{
    {0.00, 0.25, 0.30}, {-0.30, 1.00, 0.50}, {0.40, 0.15, 0.20},
    {0.80, 0.25, 0.30},  // k_log > box  -> oob_k
    {0.00, 0.05, 0.30},  // T < box      -> oob_T
    {0.00, 0.25, 0.95},  // sigma > box  -> oob_s
}};

// american_greeks points (r=0.05, q=0.0, Put — matches the cache); all land the
// internal k_log = log(K/F) inside the box (the common cached path).
struct GPt { double S, K, T, sigma; };
constexpr std::array<GPt, 3> kG = {{
    {100.0, 100.0, 0.25, 0.30}, {100.0, 110.0, 0.50, 0.45}, {100.0, 90.0, 0.15, 0.25},
}};

// Pre-change bit patterns (captured from the current Debug build; the Release
// build agrees — SSE2, no fast-math). Columns: {eval, eval_grad_value, dk, dT,
// dsigma}. Rows 3/4/5 pin the out-of-box clamp + zeroed partial on each axis.
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
constexpr std::array<std::array<std::uint64_t, 5>, 6> kEvalPins = {{
    {{0x3f53d821e0eb7c52, 0x3f53d821e0eb7c52, 0x3f8d4b35b7aa3c34, 0x3f77248f99846d58, 0xbf62bdf240536700}},
    {{0x3f5ae5637f2e32a9, 0x3f5ae5637f2e32a9, 0x3f8026cccd3c2462, 0x3f69f6635748411c, 0x3f7085ebc3eda166}},
    {{0x3f86c328003cc841, 0x3f86c328003cc841, 0x3f7a56efd8898000, 0x3fb32d46c233181f, 0x3f5ffdd6731a9f52}},
    {{0x3f94f6c1485f287d, 0x3f94f6c1485f287d, 0x0000000000000000, 0x3fb4b71771002121, 0xbf401eec38c25338}},
    {{0x3f31deddf8f2afd8, 0x3f31deddf8f2afd8, 0x3f7841a8f90cc72c, 0x0000000000000000, 0x3f674b203f881a5c}},
    {{0x3f50fa36d6f1989c, 0x3f50fa36d6f1989c, 0x3f74c8b97424267c, 0x3f7597b918916db8, 0x0000000000000000}},
}};

// american_greeks bundle bits: {delta,gamma,vega,theta,rho,vanna,volga,charm,price}.
// T16a-repinned (see kEvalPins note); validated against the AmericanGreeks.*_MatchesFd_*
// accuracy tests, which recompute the bundle vs finite differences on this same cache.
constexpr std::array<std::array<std::uint64_t, 9>, 3> kGreekPins = {{
    {{0xbfdcc1435ba70a4e, 0x3f9bc19fa8b9f349, 0x40338baa7f016d54, 0xc023a60cf9ad4fb9, 0xc029229e12ac2ed7, 0x3faa9866f415ee56, 0xbfeed34a5db9f3dc, 0x3f94d45e9eede270, 0x4015c8e2bbd78fd5}},
    {{0xbfe15513b06efcdc, 0x3f8b8257a7c397d9, 0x403bbc10d4dc0673, 0xc023f8298c7d39bf, 0xc041ce3ecb7f9661, 0x3fd91d32f03b6ddd, 0x4022fbf470c613e7, 0xbfc3eea44a94656e, 0x403173bba4a7ef99}},
    {{0xbfbd512b7c16460e, 0x3f94370020cc8635, 0x401d22186848da37, 0xc0164b736804f874, 0xbffcce2aabf1f2b8, 0xbfea6505d7bc7734, 0x40451252fa86eec7, 0x3fe6966c66d6a066, 0x3fe1e55a0dcb12ae}},
}};

TEST(Pin, EvalAndEvalGradBitIdentical) {
  const CorrectionCache tbl = make_pin_cache();
  for (std::size_t i = 0; i < kQ.size(); ++i) {
    const QPt p = kQ[i];
    const double v = tbl.eval(p.k_log, p.T, p.sigma);
    double dk = 0, dT = 0, ds = 0;
    const double vg = tbl.eval_grad(p.k_log, p.T, p.sigma, &dk, &dT, &ds);
    EXPECT_EQ(bits(v), kEvalPins[i][0]) << "eval @" << i;
    EXPECT_EQ(bits(vg), kEvalPins[i][1]) << "eval_grad value @" << i;
    EXPECT_EQ(bits(dk), kEvalPins[i][2]) << "dk @" << i;
    EXPECT_EQ(bits(dT), kEvalPins[i][3]) << "dT @" << i;
    EXPECT_EQ(bits(ds), kEvalPins[i][4]) << "dsigma @" << i;
    EXPECT_EQ(bits(v), bits(vg)) << "eval vs eval_grad value @" << i;  // public contract
  }
}

TEST(Pin, AmericanGreeksBundleBitIdentical) {
  const CorrectionCache tbl = make_pin_cache();
  for (std::size_t i = 0; i < kG.size(); ++i) {
    const GPt g = kG[i];
    const auto res = american_greeks(g.S, g.K, g.T, g.sigma, 0.05, 0.0, Side::Put, &tbl);
    ASSERT_TRUE(res.has_value());
    const AmericanGreeks& a = *res;
    const std::array<std::uint64_t, 9> got = {
        bits(a.delta), bits(a.gamma), bits(a.vega), bits(a.theta), bits(a.rho),
        bits(a.vanna), bits(a.volga), bits(a.charm), bits(a.price)};
    for (std::size_t f = 0; f < got.size(); ++f) {
      EXPECT_EQ(got[f], kGreekPins[i][f]) << "field " << f << " @pt " << i;
    }
  }
}

// eval_partials must write bit-identical partials to eval_grad — both the frozen
// pre-change pins (columns 2..4) and, independently, the live eval_grad output at
// the same point (robust to any future re-pinning). Also checks nullptr-skip.
TEST(Pin, EvalPartialsMatchesEvalGrad) {
  const CorrectionCache tbl = make_pin_cache();
  for (std::size_t i = 0; i < kQ.size(); ++i) {
    const QPt p = kQ[i];
    double gk = 0, gT = 0, gs = 0;
    tbl.eval_grad(p.k_log, p.T, p.sigma, &gk, &gT, &gs);  // reference partials
    double pk = 0, pT = 0, ps = 0;
    tbl.eval_partials(p.k_log, p.T, p.sigma, &pk, &pT, &ps);
    EXPECT_EQ(bits(pk), kEvalPins[i][2]) << "dk pin @" << i;
    EXPECT_EQ(bits(pT), kEvalPins[i][3]) << "dT pin @" << i;
    EXPECT_EQ(bits(ps), kEvalPins[i][4]) << "dsigma pin @" << i;
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

}  // namespace pin

}  // namespace
