#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"

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

}  // namespace
