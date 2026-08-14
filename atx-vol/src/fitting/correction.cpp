#include "atx/vol/api/fitting/correction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/math.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "fitting/counters.hpp" // ATX_VOL_COUNT (opt-in P0.2; no-op when OFF)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// clang-cl / MSVC do not define M_PI; carry the extended literal explicitly.
inline constexpr double kPi = 3.14159265358979323846;

// Capacity of the 3D Clenshaw workspace: the pathological 64x64 (n_T*n_s) plane.
// Real caches are far smaller (production 16x8x12 -> 96 live doubles), and both
// eval and eval_partials zero only the live n_T*n_s prefix, so the 32 KB is a
// flat stack-frame reservation, not a per-call memset. A right-sizing that shrank
// this to an n_s vector (by interleaving the i/j collapses) was implemented and
// measured to cost ~80 ns/sweep on the hot path, so the plane capacity is kept.
inline constexpr std::size_t kTmpSize = static_cast<std::size_t>(detail::kChebMaxNodes) *
                                        static_cast<std::size_t>(detail::kChebMaxNodes);

// Correction sample at one (k_log, T, sigma) node, normalized to F = 1:
//   S = e^{-(r-q)T}, K = e^{k_log},  c = P_amer(S,K,...) - P_euro(S,K,...).
// Reuses the Andersen-Lake cold pricer and the Black-76 European kernel (with
// F = 1 the two agree with the C `euro_at`). Non-convergence / domain failures
// fall back to a zero correction, matching the C populator.
[[nodiscard]] double sample_correction(double k_log, double T, double sigma, double r, double q,
                                       Side side, const std::optional<AlOpts> &opts) {
  const double K = std::exp(k_log);
  const double S = std::exp(-(r - q) * T);
  const Result<double> p_amer = andersen_lake(S, K, T, sigma, r, q, side, opts);
  if (!p_amer) {
    return 0.0;
  }
  const double df = std::exp(-r * T);
  const double euro = black76_price(1.0, K, T, sigma, df, side);
  const double c = *p_amer - euro;
  return (c > 0.0) ? c : 0.0;
}

} // namespace

namespace detail {

double cheb_node(std::uint16_t j, std::uint16_t n) noexcept {
  if (n == 0) {
    return 0.0;
  }
  return std::cos(kPi * (2.0 * static_cast<double>(j) + 1.0) / (2.0 * static_cast<double>(n)));
}

void cheb_dct2(const double *vals, double *coefs, std::uint16_t n) noexcept {
  if (n == 0) {
    return;
  }
  const double inv_n = 1.0 / static_cast<double>(n);
  const double scale_pi_2n = kPi / (2.0 * static_cast<double>(n));
  for (std::uint16_t k = 0; k < n; ++k) {
    double s = 0.0;
    const double k_scale = static_cast<double>(k) * scale_pi_2n;
    for (std::uint16_t j = 0; j < n; ++j) {
      s += vals[j] * std::cos(k_scale * static_cast<double>(2u * j + 1u));
    }
    coefs[k] = (k == 0) ? s * inv_n : 2.0 * s * inv_n;
  }
}

double cheb_clenshaw1d(const double *coefs, std::uint16_t n, double x) noexcept {
  double bk1 = 0.0;
  double bk2 = 0.0;
  const double two_x = 2.0 * x;
  for (int k = static_cast<int>(n) - 1; k >= 1; --k) {
    const double bk = coefs[k] + two_x * bk1 - bk2;
    bk2 = bk1;
    bk1 = bk;
  }
  return coefs[0] + x * bk1 - bk2;
}

void cheb_diff_coefs(const double *c, double *d, std::uint16_t n, double scale) noexcept {
  ATX_VOL_COUNT(ChebDiffCoefs); // one derivative-coef transform (opt-in P0.2; no-op when OFF)
  if (n == 0u) {
    return;
  }
  if (n == 1u) {
    d[0] = 0.0;
    return;
  }
  d[n - 1u] = 0.0;
  d[n - 2u] = 2.0 * static_cast<double>(n - 1u) * c[n - 1u];
  for (int j = static_cast<int>(n) - 3; j >= 0; --j) {
    d[j] = d[j + 2] + 2.0 * static_cast<double>(j + 1) * c[j + 1];
  }
  d[0] *= 0.5; // Numerical Recipes halving to match the full-c0 Clenshaw form.
  if (scale != 1.0) {
    for (std::uint16_t k = 0u; k < n; ++k) {
      d[k] *= scale;
    }
  }
}

double cheb_clenshaw3d(const double *coefs, std::uint16_t n_k, std::uint16_t n_T, std::uint16_t n_s,
                       double xi, double xj, double xk, double *tmp_jk) noexcept {
  if (n_k == 0u || n_T == 0u || n_s == 0u) {
    return 0.0;
  }
  ATX_VOL_COUNT(ClenshawSweeps); // one value sweep (opt-in P0.2; no-op when OFF)
  const std::size_t nk = n_k;
  const std::size_t nT = n_T;
  const std::size_t ns = n_s;
  const double two_xi = 2.0 * xi;
  const double two_xj = 2.0 * xj;
  const double two_xk = 2.0 * xk;

  // Two-pass (plane) collapse. A single interleaved-collapse variant that folds
  // the i/j axes through a 1-D column (shrinking this n_T*n_s plane to an n_s
  // vector) was tried for the stack right-sizing but measured ~80 ns/sweep SLOWER
  // — it serializes the i- and j-recursions and loses the ILP the compiler
  // extracts from the fully-independent 1st pass below — so the plane form is
  // kept. See CorrectionCache::eval for the (already right-sized) zeroing.

  // 1st collapse: i-axis (k_log), innermost in memory.
  for (std::size_t j = 0; j < nT; ++j) {
    for (std::size_t k = 0; k < ns; ++k) {
      const double *row = coefs + j * ns * nk + k * nk;
      double bk1 = 0.0;
      double bk2 = 0.0;
      for (int i = static_cast<int>(n_k) - 1; i >= 1; --i) {
        const double bk = row[i] + two_xi * bk1 - bk2;
        bk2 = bk1;
        bk1 = bk;
      }
      tmp_jk[j * ns + k] = row[0] + xi * bk1 - bk2;
    }
  }

  // 2nd collapse: j-axis (T). Leaves the sigma-axis vector in tmp_jk[0..n_s-1].
  for (std::size_t k = 0; k < ns; ++k) {
    double bk1 = 0.0;
    double bk2 = 0.0;
    for (int j = static_cast<int>(n_T) - 1; j >= 1; --j) {
      const double bk = tmp_jk[static_cast<std::size_t>(j) * ns + k] + two_xj * bk1 - bk2;
      bk2 = bk1;
      bk1 = bk;
    }
    const double a0 = tmp_jk[k]; // j = 0 row, before overwrite
    tmp_jk[k] = a0 + xj * bk1 - bk2;
  }

  // 3rd collapse: k-axis (sigma).
  double bk1 = 0.0;
  double bk2 = 0.0;
  for (int k = static_cast<int>(n_s) - 1; k >= 1; --k) {
    const double bk = tmp_jk[k] + two_xk * bk1 - bk2;
    bk2 = bk1;
    bk1 = bk;
  }
  return tmp_jk[0] + xk * bk1 - bk2;
}

double cheb_clenshaw3d_partial(const double *coefs, std::uint16_t n_k, std::uint16_t n_T,
                               std::uint16_t n_s, double xi, double xj, double xk, int diff_axis,
                               double axis_scale, double *tmp_jk) noexcept {
  if (n_k == 0u || n_T == 0u || n_s == 0u) {
    return 0.0;
  }
  ATX_VOL_COUNT(ClenshawSweeps); // one partial sweep (opt-in P0.2; no-op when OFF)
  const std::size_t nk = n_k;
  const std::size_t nT = n_T;
  const std::size_t ns = n_s;
  std::array<double, kChebMaxNodes> dscratch{};

  const double two_xi = 2.0 * xi;
  const double two_xj = 2.0 * xj;
  const double two_xk = 2.0 * xk;

  // 1st collapse: i-axis (k_log). diff_axis == 0 differentiates each row first.
  for (std::size_t j = 0; j < nT; ++j) {
    for (std::size_t k = 0; k < ns; ++k) {
      const double *row = coefs + j * ns * nk + k * nk;
      const double *eval_row = row;
      if (diff_axis == 0) {
        cheb_diff_coefs(row, dscratch.data(), n_k, axis_scale);
        eval_row = dscratch.data();
      }
      double bk1 = 0.0;
      double bk2 = 0.0;
      for (int i = static_cast<int>(n_k) - 1; i >= 1; --i) {
        const double bk = eval_row[i] + two_xi * bk1 - bk2;
        bk2 = bk1;
        bk1 = bk;
      }
      tmp_jk[j * ns + k] = eval_row[0] + xi * bk1 - bk2;
    }
  }

  // 2nd collapse: j-axis (T). diff_axis == 1 differentiates the gathered column.
  for (std::size_t k = 0; k < ns; ++k) {
    if (diff_axis == 1) {
      std::array<double, kChebMaxNodes> col{};
      for (std::size_t j = 0; j < nT; ++j) {
        col[j] = tmp_jk[j * ns + k];
      }
      cheb_diff_coefs(col.data(), dscratch.data(), n_T, axis_scale);
      double bk1 = 0.0;
      double bk2 = 0.0;
      for (int j = static_cast<int>(n_T) - 1; j >= 1; --j) {
        const double bk = dscratch[static_cast<std::size_t>(j)] + two_xj * bk1 - bk2;
        bk2 = bk1;
        bk1 = bk;
      }
      tmp_jk[k] = dscratch[0] + xj * bk1 - bk2;
    } else {
      double bk1 = 0.0;
      double bk2 = 0.0;
      for (int j = static_cast<int>(n_T) - 1; j >= 1; --j) {
        const double bk = tmp_jk[static_cast<std::size_t>(j) * ns + k] + two_xj * bk1 - bk2;
        bk2 = bk1;
        bk1 = bk;
      }
      const double a0 = tmp_jk[k]; // j = 0 row, before overwrite
      tmp_jk[k] = a0 + xj * bk1 - bk2;
    }
  }

  // 3rd collapse: k-axis (sigma). diff_axis == 2 differentiates first.
  const double *eval_vec = tmp_jk;
  if (diff_axis == 2) {
    cheb_diff_coefs(tmp_jk, dscratch.data(), n_s, axis_scale);
    eval_vec = dscratch.data();
  }
  double bk1 = 0.0;
  double bk2 = 0.0;
  for (int k = static_cast<int>(n_s) - 1; k >= 1; --k) {
    const double bk = eval_vec[k] + two_xk * bk1 - bk2;
    bk2 = bk1;
    bk1 = bk;
  }
  return eval_vec[0] + xk * bk1 - bk2;
}

} // namespace detail

// ── CorrectionCache ─────────────────────────────────────────────────────

namespace {

struct ClenshawD1 {
  double value;
  double d1;
};

struct ClenshawD2 {
  double value;
  double d1;
  double d2;
};

[[nodiscard]] double clenshaw_value_strided(const double *coefs, std::size_t stride,
                                            std::uint16_t n, double x) noexcept {
  double bk1 = 0.0;
  double bk2 = 0.0;
  const double two_x = 2.0 * x;
  for (int i = static_cast<int>(n) - 1; i >= 1; --i) {
    const double bk = coefs[static_cast<std::size_t>(i) * stride] + two_x * bk1 - bk2;
    bk2 = bk1;
    bk1 = bk;
  }
  return coefs[0] + x * bk1 - bk2;
}

[[nodiscard]] ClenshawD1 clenshaw_d1_strided(const double *coefs, std::size_t stride,
                                             std::uint16_t n, double x) noexcept {
  double bk1 = 0.0;
  double bk2 = 0.0;
  double dbk1 = 0.0;
  double dbk2 = 0.0;
  const double two_x = 2.0 * x;
  for (int i = static_cast<int>(n) - 1; i >= 1; --i) {
    const double bk = coefs[static_cast<std::size_t>(i) * stride] + two_x * bk1 - bk2;
    const double dbk = 2.0 * bk1 + two_x * dbk1 - dbk2;
    bk2 = bk1;
    bk1 = bk;
    dbk2 = dbk1;
    dbk1 = dbk;
  }
  return ClenshawD1{coefs[0] + x * bk1 - bk2, bk1 + x * dbk1 - dbk2};
}

[[nodiscard]] ClenshawD2 clenshaw_d2_strided(const double *coefs, std::size_t stride,
                                             std::uint16_t n, double x) noexcept {
  double bk1 = 0.0;
  double bk2 = 0.0;
  double dbk1 = 0.0;
  double dbk2 = 0.0;
  double ddbk1 = 0.0;
  double ddbk2 = 0.0;
  const double two_x = 2.0 * x;
  for (int i = static_cast<int>(n) - 1; i >= 1; --i) {
    const double bk = coefs[static_cast<std::size_t>(i) * stride] + two_x * bk1 - bk2;
    const double dbk = 2.0 * bk1 + two_x * dbk1 - dbk2;
    const double ddbk = 4.0 * dbk1 + two_x * ddbk1 - ddbk2;
    bk2 = bk1;
    bk1 = bk;
    dbk2 = dbk1;
    dbk1 = dbk;
    ddbk2 = ddbk1;
    ddbk1 = ddbk;
  }
  return ClenshawD2{coefs[0] + x * bk1 - bk2, bk1 + x * dbk1 - dbk2,
                    2.0 * dbk1 + x * ddbk1 - ddbk2};
}

// Differentiate the nested i->j->k Clenshaw collapses in place. The expensive
// i-axis traversal reads every tensor coefficient once and produces value/dk/dkk
// together. The much smaller j/k collapses then add the mixed derivatives used
// by American gamma, vanna, volga, and charm.
[[nodiscard]] CorrSecondOrder cheb_clenshaw3d_second_order(const double *coefs, std::uint16_t n_k,
                                                           std::uint16_t n_T, std::uint16_t n_s,
                                                           double xi, double xj,
                                                           double xk) noexcept {
  ATX_VOL_COUNT(ClenshawSweeps);
  const std::size_t nk = n_k;
  const std::size_t nT = n_T;
  const std::size_t ns = n_s;

  std::array<double, detail::kChebMaxNodes> values_by_T{};
  std::array<double, detail::kChebMaxNodes> dk_values_by_T{};
  std::array<double, detail::kChebMaxNodes> dkk_values_by_T{};
  std::array<double, detail::kChebMaxNodes> values_T{};
  std::array<double, detail::kChebMaxNodes> dk_values_T{};
  std::array<double, detail::kChebMaxNodes> dkk_values_T{};
  std::array<double, detail::kChebMaxNodes> dT_values_T{};
  std::array<double, detail::kChebMaxNodes> dk_dT_values_T{};
  for (std::size_t k = 0; k < ns; ++k) {
    for (std::size_t j = 0; j < nT; ++j) {
      const double *row = coefs + j * ns * nk + k * nk;
      const ClenshawD2 jet = clenshaw_d2_strided(row, 1u, n_k, xi);
      values_by_T[j] = jet.value;
      dk_values_by_T[j] = jet.d1;
      dkk_values_by_T[j] = jet.d2;
    }
    const ClenshawD1 value_T = clenshaw_d1_strided(values_by_T.data(), 1u, n_T, xj);
    const ClenshawD1 dk_T = clenshaw_d1_strided(dk_values_by_T.data(), 1u, n_T, xj);
    values_T[k] = value_T.value;
    dk_values_T[k] = dk_T.value;
    dkk_values_T[k] = clenshaw_value_strided(dkk_values_by_T.data(), 1u, n_T, xj);
    dT_values_T[k] = value_T.d1;
    dk_dT_values_T[k] = dk_T.d1;
  }

  const ClenshawD2 value_sigma = clenshaw_d2_strided(values_T.data(), 1u, n_s, xk);
  const ClenshawD1 dk_sigma = clenshaw_d1_strided(dk_values_T.data(), 1u, n_s, xk);
  CorrSecondOrder out;
  out.value = value_sigma.value;
  out.dk_log = dk_sigma.value;
  out.dT = clenshaw_value_strided(dT_values_T.data(), 1u, n_s, xk);
  out.dsigma = value_sigma.d1;
  out.dkk = clenshaw_value_strided(dkk_values_T.data(), 1u, n_s, xk);
  out.dk_dT = clenshaw_value_strided(dk_dT_values_T.data(), 1u, n_s, xk);
  out.dk_dsigma = dk_sigma.d1;
  out.dsigma2 = value_sigma.d2;
  return out;
}

// Fused value + sigma-partial single pass (perf review F1 stage b). Sigma is the
// LAST collapse axis of the 3D Clenshaw, so its value AND d/dxk fall out of ONE
// derivative-recurrence sweep. The i (k_log) and j (T) collapses are byte-for-byte
// cheb_clenshaw3d — so the returned VALUE is bit-identical to cheb_clenshaw3d /
// eval() — and only the final sigma collapse changes: clenshaw_d1 emits value + the
// unit-space derivative together, replacing eval_partials()'s separate
// differentiate-coefficients-then-evaluate pass. The dsigma therefore differs from
// eval_partials() at the ULP level (a different accumulation of the SAME analytic
// derivative); it IS bit-identical to eval_second_order()'s dsigma (same recurrence).
// Counts as ONE Clenshaw sweep (vs eval + eval_partials' two).
[[nodiscard]] ClenshawD1 cheb_clenshaw3d_value_dsigma(const double *coefs, std::uint16_t n_k,
                                                      std::uint16_t n_T, std::uint16_t n_s, double xi,
                                                      double xj, double xk,
                                                      double *tmp_jk) noexcept {
  if (n_k == 0u || n_T == 0u || n_s == 0u) {
    return ClenshawD1{0.0, 0.0};
  }
  ATX_VOL_COUNT(ClenshawSweeps); // one fused value+dsigma sweep (opt-in P0.2)
  const std::size_t nk = n_k;
  const std::size_t nT = n_T;
  const std::size_t ns = n_s;
  const double two_xi = 2.0 * xi;
  const double two_xj = 2.0 * xj;

  // 1st collapse: i-axis (k_log). Byte-for-byte cheb_clenshaw3d.
  for (std::size_t j = 0; j < nT; ++j) {
    for (std::size_t k = 0; k < ns; ++k) {
      const double *row = coefs + j * ns * nk + k * nk;
      double bk1 = 0.0;
      double bk2 = 0.0;
      for (int i = static_cast<int>(n_k) - 1; i >= 1; --i) {
        const double bk = row[i] + two_xi * bk1 - bk2;
        bk2 = bk1;
        bk1 = bk;
      }
      tmp_jk[j * ns + k] = row[0] + xi * bk1 - bk2;
    }
  }

  // 2nd collapse: j-axis (T). Byte-for-byte cheb_clenshaw3d — leaves the sigma-axis
  // vector in tmp_jk[0..n_s-1].
  for (std::size_t k = 0; k < ns; ++k) {
    double bk1 = 0.0;
    double bk2 = 0.0;
    for (int j = static_cast<int>(n_T) - 1; j >= 1; --j) {
      const double bk = tmp_jk[static_cast<std::size_t>(j) * ns + k] + two_xj * bk1 - bk2;
      bk2 = bk1;
      bk1 = bk;
    }
    const double a0 = tmp_jk[k]; // j = 0 row, before overwrite
    tmp_jk[k] = a0 + xj * bk1 - bk2;
  }

  // 3rd collapse: k-axis (sigma). Value + d/dxk in one sweep. The value recurrence
  // is identical to cheb_clenshaw3d's, so tmp_jk[0] + xk*bk1 - bk2 is bit-identical.
  return clenshaw_d1_strided(tmp_jk, 1u, n_s, xk);
}

} // namespace

Result<CorrectionCache> CorrectionCache::build(std::uint16_t n_log_moneyness,
                                               std::uint16_t n_T_nodes, std::uint16_t n_sigma_nodes,
                                               double r, double q, double k_log_min,
                                               double k_log_max, double T_min, double T_max,
                                               double sigma_min, double sigma_max, Side side,
                                               const std::optional<AlOpts> &opts) {
  using detail::kChebMaxNodes;

  if (n_log_moneyness == 0 || n_T_nodes == 0 || n_sigma_nodes == 0) {
    return Err(ErrorCode::InvalidArgument, "CorrectionCache::build: zero grid dimension");
  }
  if (n_log_moneyness > kChebMaxNodes || n_T_nodes > kChebMaxNodes ||
      n_sigma_nodes > kChebMaxNodes) {
    return Err(ErrorCode::OutOfRange, "CorrectionCache::build: grid exceeds kChebMaxNodes");
  }
  if (!(k_log_max > k_log_min) || !(T_max > T_min) || !(sigma_max > sigma_min)) {
    return Err(ErrorCode::InvalidArgument, "CorrectionCache::build: inverted box");
  }
  if (!(T_min > 0.0) || !(sigma_min > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "CorrectionCache::build: non-positive T/sigma floor");
  }
  // The cache bakes the American-European correction at a FIXED (r, q, side). If
  // that lands in the double-continuation regime, every andersen_lake sample is
  // NotImplemented and sample_correction floors it to 0 — the cache would encode
  // a pure-European surface, i.e. the exact silent mispricing this whole guard
  // exists to prevent. Reject the build up front (single-source classifier from
  // american.hpp; internal-put rate/yield: rate=r for a put, rate=q for a call).
  if (detail::classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                              /*yield=*/(side == Side::Put) ? q : r) ==
      detail::ExerciseRegime::Unsupported) {
    return Err(ErrorCode::NotImplemented,
               "CorrectionCache::build: double-continuation regime (put q < r <= 0 "
               "/ call r < q <= 0) is not representable by the single-boundary "
               "Andersen-Lake scheme; see Andersen-Lake 2021 (double-boundary case)");
  }

  CorrectionCache cache;
  cache.n_k_ = n_log_moneyness;
  cache.n_T_ = n_T_nodes;
  cache.n_s_ = n_sigma_nodes;
  cache.k_log_min_ = k_log_min;
  cache.k_log_max_ = k_log_max;
  cache.T_min_ = T_min;
  cache.T_max_ = T_max;
  cache.sigma_min_ = sigma_min;
  cache.sigma_max_ = sigma_max;
  cache.scale_k_ = 2.0 / (k_log_max - k_log_min);
  cache.scale_T_ = 2.0 / (T_max - T_min);
  cache.scale_s_ = 2.0 / (sigma_max - sigma_min);
  cache.r_ = r;
  cache.q_ = q;
  cache.side_ = side;

  const std::uint16_t n_k = cache.n_k_;
  const std::uint16_t n_T = cache.n_T_;
  const std::uint16_t n_s = cache.n_s_;
  cache.coefs_.assign(static_cast<std::size_t>(n_k) * static_cast<std::size_t>(n_T) *
                          static_cast<std::size_t>(n_s),
                      0.0);
  double *coefs = cache.coefs_.data();

  // Chebyshev node grids mapped onto each physical axis.
  std::array<double, kChebMaxNodes> k_log_grid{};
  std::array<double, kChebMaxNodes> T_grid{};
  std::array<double, kChebMaxNodes> sigma_grid{};
  for (std::uint16_t i = 0; i < n_k; ++i) {
    k_log_grid[i] = detail::cheb_from_unit(detail::cheb_node(i, n_k), k_log_min, k_log_max);
  }
  for (std::uint16_t j = 0; j < n_T; ++j) {
    T_grid[j] = detail::cheb_from_unit(detail::cheb_node(j, n_T), T_min, T_max);
  }
  for (std::uint16_t k = 0; k < n_s; ++k) {
    sigma_grid[k] = detail::cheb_from_unit(detail::cheb_node(k, n_s), sigma_min, sigma_max);
  }

  // Step 1: sample the correction at every (i, j, k) into the final layout. The
  // innermost k_log axis at a fixed (T, sigma) is a set of strikes K = e^{k_log}
  // against a fixed S = e^{-(r-q)T}, so BOTH sides price the whole i-row with ONE
  // early-exercise boundary solve instead of n_k. The CALL side uses
  // andersen_lake_call_slice (internal-put strike Kp = S is fixed, so it is
  // bit-identical to per-node sample_correction). The PUT side uses
  // andersen_lake_put_slice (T16a): the reused boundary is homogeneity-exact in ℝ
  // but ~a few ULP off a fresh per-strike andersen_lake in IEEE, so the sampled
  // put row shifts ~1e-7 vs the scalar path — the accepted boundary-reuse policy
  // (validated to the §9 accuracy gates against cold andersen_lake). Either side
  // falls back to the scalar per-node sample_correction (the reference) for any
  // (T, sigma) row the slice rejects (Unsupported / collapsed boundary).
  std::array<double, kChebMaxNodes> strike_buf{};
  std::array<double, kChebMaxNodes> px_buf{};
  for (std::uint16_t j = 0; j < n_T; ++j) {
    const double Tj = T_grid[j];
    const double df_j = std::exp(-r * Tj);
    const double S_j = std::exp(-(r - q) * Tj);
    for (std::uint16_t k = 0; k < n_s; ++k) {
      const double sig = sigma_grid[k];
      double *row = coefs + detail::cheb_idx(0, j, k, n_k, n_s); // i-contiguous
      bool used_slice = false;
      if (side == Side::Call || side == Side::Put) {
        for (std::uint16_t i = 0; i < n_k; ++i) {
          strike_buf[i] = std::exp(k_log_grid[i]);
        }
        const Status st =
            (side == Side::Call)
                ? andersen_lake_call_slice(S_j, std::span<const double>(strike_buf.data(), n_k), Tj,
                                           sig, r, q, std::span<double>(px_buf.data(), n_k), opts)
                : andersen_lake_put_slice(S_j, std::span<const double>(strike_buf.data(), n_k), Tj,
                                          sig, r, q, std::span<double>(px_buf.data(), n_k), opts);
        if (st) {
          for (std::uint16_t i = 0; i < n_k; ++i) {
            const double euro = black76_price(1.0, strike_buf[i], Tj, sig, df_j, side);
            const double c = px_buf[i] - euro;
            row[i] = (c > 0.0) ? c : 0.0;
          }
          used_slice = true;
        }
      }
      if (!used_slice) {
        for (std::uint16_t i = 0; i < n_k; ++i) {
          row[i] = sample_correction(k_log_grid[i], Tj, sig, r, q, side, opts);
        }
      }
    }
  }

  // Step 2: separable DCT-II along each axis.
  std::array<double, kChebMaxNodes> in_buf{};
  std::array<double, kChebMaxNodes> out_buf{};

  // 2a: i-axis (k_log) — innermost-contiguous.
  for (std::uint16_t j = 0; j < n_T; ++j) {
    for (std::uint16_t k = 0; k < n_s; ++k) {
      double *row = coefs + detail::cheb_idx(0, j, k, n_k, n_s);
      for (std::uint16_t i = 0; i < n_k; ++i) {
        in_buf[i] = row[i];
      }
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_k);
      for (std::uint16_t i = 0; i < n_k; ++i) {
        row[i] = out_buf[i];
      }
    }
  }
  // 2b: j-axis (T).
  for (std::uint16_t i = 0; i < n_k; ++i) {
    for (std::uint16_t k = 0; k < n_s; ++k) {
      for (std::uint16_t j = 0; j < n_T; ++j) {
        in_buf[j] = coefs[detail::cheb_idx(i, j, k, n_k, n_s)];
      }
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_T);
      for (std::uint16_t j = 0; j < n_T; ++j) {
        coefs[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[j];
      }
    }
  }
  // 2c: k-axis (sigma).
  for (std::uint16_t i = 0; i < n_k; ++i) {
    for (std::uint16_t j = 0; j < n_T; ++j) {
      for (std::uint16_t k = 0; k < n_s; ++k) {
        in_buf[k] = coefs[detail::cheb_idx(i, j, k, n_k, n_s)];
      }
      detail::cheb_dct2(in_buf.data(), out_buf.data(), n_s);
      for (std::uint16_t k = 0; k < n_s; ++k) {
        coefs[detail::cheb_idx(i, j, k, n_k, n_s)] = out_buf[k];
      }
    }
  }

  // Step 3 (T16b): precompute the k_log-axis derivative-coefficient tensor C_k.
  // The k_log partial (cheb_clenshaw3d_partial diff_axis==0) differentiates each
  // innermost, contiguous i-row BEFORE any Clenshaw collapse, so the differentiated
  // rows are a fixed function of the build-time coefficient tensor. Run
  // cheb_diff_coefs over every i-row ONCE here — with the box axis scale
  // scale_k = 2/(k_log_max - k_log_min) folded in, exactly as the live partial
  // passed it — so eval_partials can read dC/dk_log as a PLAIN value Clenshaw over
  // C_k, eliminating the per-query n_T*n_s differentiation that dominated the
  // partial sweep. Because the differentiation is the innermost op (no earlier-axis
  // collapse to reorder the summation against), diff-at-build then Clenshaw is
  // BIT-IDENTICAL to the in-pass diff-then-Clenshaw (locked by
  // Chebyshev.DerivTensors_EvalPartialsBitIdenticalToLive and the eval_partials
  // dk_log pins). The T and sigma partials differentiate a Clenshaw-COLLAPSED,
  // query-dependent vector, so their diff cannot be hoisted without shifting bits;
  // they stay on the reference cheb_clenshaw3d_partial path.
  cache.dk_coefs_.assign(cache.coefs_.size(), 0.0);
  {
    std::array<double, kChebMaxNodes> drow{};
    for (std::uint16_t j = 0; j < n_T; ++j) {
      for (std::uint16_t k = 0; k < n_s; ++k) {
        const double *row = coefs + detail::cheb_idx(0, j, k, n_k, n_s);
        detail::cheb_diff_coefs(row, drow.data(), n_k, cache.scale_k_);
        double *drow_dst = cache.dk_coefs_.data() + detail::cheb_idx(0, j, k, n_k, n_s);
        for (std::uint16_t i = 0; i < n_k; ++i) {
          drow_dst[i] = drow[i];
        }
      }
    }
  }

  cache.populated_ = true;
  return Ok(std::move(cache));
}

bool CorrectionCache::contains(double k_log, double T, double sigma) const noexcept {
  return populated_ && std::isfinite(k_log) && std::isfinite(T) && std::isfinite(sigma) &&
         k_log >= k_log_min_ && k_log <= k_log_max_ && T >= T_min_ && T <= T_max_ &&
         sigma >= sigma_min_ && sigma <= sigma_max_;
}

double CorrectionCache::eval(double k_log, double T, double sigma) const noexcept {
  if (!populated_) {
    return 0.0;
  }
#if defined(ATX_VOL_COUNTERS)
  // The box test itself (not just the counter increment) only exists in the ON
  // build: with ATX_VOL_COUNTERS undefined this whole block is gone at the
  // preprocessor, not merely dead code left for the optimizer to remove.
  if ((k_log < k_log_min_) || (k_log > k_log_max_) || (T < T_min_) || (T > T_max_) ||
      (sigma < sigma_min_) || (sigma > sigma_max_)) {
    ATX_VOL_COUNT(CacheOutOfBoxClamps);
  }
#endif
  k_log = atx::core::clamp(k_log, k_log_min_, k_log_max_);
  T = atx::core::clamp(T, T_min_, T_max_);
  sigma = atx::core::clamp(sigma, sigma_min_, sigma_max_);

  const double xi = detail::cheb_to_unit(k_log, k_log_min_, k_log_max_);
  const double xj = detail::cheb_to_unit(T, T_min_, T_max_);
  const double xk = detail::cheb_to_unit(sigma, sigma_min_, sigma_max_);

  // Write-before-read invariant: cheb_clenshaw3d's first (i-axis) collapse writes
  // every live cell tmp_jk[j*n_s_ + k] for j<n_T_, k<n_s_ before the later
  // collapses read them. Bounded-init ONLY that live n_T_*n_s_ prefix (a few
  // hundred doubles) — not the full kTmpSize (4096) capacity — so the hot path
  // stays cheap while the used span is defined even if the kernel is later edited.
  std::array<double, kTmpSize> tmp_jk;
  std::fill(tmp_jk.data(), tmp_jk.data() + static_cast<std::size_t>(n_T_) * n_s_, 0.0);
  const double v =
      detail::cheb_clenshaw3d(coefs_.data(), n_k_, n_T_, n_s_, xi, xj, xk, tmp_jk.data());
  return (v > 0.0) ? v : 0.0;
}

void CorrectionCache::collapse_T_plane(double T, double *plane_out) const noexcept {
  // Perf review F4: pre-collapse the T (j) axis for an equal-T ladder so every
  // strike thereafter is a 2-D (k_log, sigma) Clenshaw. plane_out[k*n_k + i] =
  // Σ_j coefs[i,j,k] T_j(xj), the Clenshaw collapse of the T column at fixed
  // (i, k). coefs[i,j,k] lives at j*n_s*n_k + k*n_k + i (cheb_idx), so the T
  // column for a fixed (i, k) strides by n_s*n_k; the plane keeps i innermost so
  // eval_plane's k_log collapse reads a contiguous row plane + k*n_k, exactly as
  // cheb_clenshaw3d's first collapse reads a fixed-j sub-block.
  const std::size_t live = static_cast<std::size_t>(n_k_) * static_cast<std::size_t>(n_s_);
  if (!populated_) {
    std::fill(plane_out, plane_out + live, 0.0);
    return;
  }
  ATX_VOL_COUNT(ClenshawSweeps); // one full-tensor T-axis collapse (reads every coef once)
  T = atx::core::clamp(T, T_min_, T_max_);
  const double xj = detail::cheb_to_unit(T, T_min_, T_max_);
  const double two_xj = 2.0 * xj;
  const std::size_t nk = n_k_;
  const std::size_t ns = n_s_;
  const double *coefs = coefs_.data();
  for (std::size_t k = 0; k < ns; ++k) {
    for (std::size_t i = 0; i < nk; ++i) {
      const double *col = coefs + k * nk + i;
      double bk1 = 0.0;
      double bk2 = 0.0;
      for (int j = static_cast<int>(n_T_) - 1; j >= 1; --j) {
        const double bk = col[static_cast<std::size_t>(j) * ns * nk] + two_xj * bk1 - bk2;
        bk2 = bk1;
        bk1 = bk;
      }
      plane_out[k * nk + i] = col[0] + xj * bk1 - bk2;
    }
  }
}

double CorrectionCache::eval_plane(const double *plane, double k_log, double sigma) const noexcept {
  if (!populated_) {
    return 0.0;
  }
  // Same box clamp / unit map as eval(); T was already clamped and collapsed away
  // in collapse_T_plane. 2-D Clenshaw over the plane: k_log (i) then sigma (k).
  k_log = atx::core::clamp(k_log, k_log_min_, k_log_max_);
  sigma = atx::core::clamp(sigma, sigma_min_, sigma_max_);
  const double xi = detail::cheb_to_unit(k_log, k_log_min_, k_log_max_);
  const double xk = detail::cheb_to_unit(sigma, sigma_min_, sigma_max_);
  const std::size_t nk = n_k_;
  const std::size_t ns = n_s_;
  const double two_xi = 2.0 * xi;
  // tmp_k[k] is written for every k < n_s_ by the k_log collapse before the sigma
  // collapse reads it; value-initialized to match the codebase's scratch style.
  std::array<double, detail::kChebMaxNodes> tmp_k{};
  for (std::size_t k = 0; k < ns; ++k) {
    const double *row = plane + k * nk;
    double bk1 = 0.0;
    double bk2 = 0.0;
    for (int i = static_cast<int>(n_k_) - 1; i >= 1; --i) {
      const double bk = row[i] + two_xi * bk1 - bk2;
      bk2 = bk1;
      bk1 = bk;
    }
    tmp_k[k] = row[0] + xi * bk1 - bk2;
  }
  double bk1 = 0.0;
  double bk2 = 0.0;
  const double two_xk = 2.0 * xk;
  for (int k = static_cast<int>(n_s_) - 1; k >= 1; --k) {
    const double bk = tmp_k[static_cast<std::size_t>(k)] + two_xk * bk1 - bk2;
    bk2 = bk1;
    bk1 = bk;
  }
  const double v = tmp_k[0] + xk * bk1 - bk2;
  return (v > 0.0) ? v : 0.0; // max(0, polynomial), as eval()
}

double CorrectionCache::eval_value_dk(double k_log, double T, double sigma,
                                      double *out_dk_log) const noexcept {
  if (!populated_) {
    if (out_dk_log != nullptr) {
      *out_dk_log = 0.0;
    }
    return 0.0;
  }
  const bool oob_k = (k_log < k_log_min_) || (k_log > k_log_max_);
  k_log = atx::core::clamp(k_log, k_log_min_, k_log_max_);
  T = atx::core::clamp(T, T_min_, T_max_);
  sigma = atx::core::clamp(sigma, sigma_min_, sigma_max_);

  const double xi = detail::cheb_to_unit(k_log, k_log_min_, k_log_max_);
  const double xj = detail::cheb_to_unit(T, T_min_, T_max_);
  const double xk = detail::cheb_to_unit(sigma, sigma_min_, sigma_max_);
  const std::size_t live_scratch = static_cast<std::size_t>(n_T_) * n_s_;
  std::array<double, kTmpSize> tmp_jk;
  std::fill(tmp_jk.data(), tmp_jk.data() + live_scratch, 0.0);
  const double raw_value =
      detail::cheb_clenshaw3d(coefs_.data(), n_k_, n_T_, n_s_, xi, xj, xk, tmp_jk.data());
  const bool correction_active = raw_value > 0.0;
  if (out_dk_log != nullptr) {
    if (oob_k || !correction_active) {
      *out_dk_log = 0.0;
    } else {
      std::fill(tmp_jk.data(), tmp_jk.data() + live_scratch, 0.0);
      *out_dk_log =
          detail::cheb_clenshaw3d(dk_coefs_.data(), n_k_, n_T_, n_s_, xi, xj, xk, tmp_jk.data());
    }
  }
  return correction_active ? raw_value : 0.0;
}

void CorrectionCache::eval_partials(double k_log, double T, double sigma, double *out_dk_log,
                                    double *out_dT, double *out_dsigma) const noexcept {
  if (!populated_) {
    if (out_dk_log) {
      *out_dk_log = 0.0;
    }
    if (out_dT) {
      *out_dT = 0.0;
    }
    if (out_dsigma) {
      *out_dsigma = 0.0;
    }
    return;
  }

  // Out-of-box on an axis nulls that partial (the value, via eval, still clamps).
  // Identical to eval_grad's partial path — this IS that path, minus the value
  // sweep that eval_grad's callers were discarding.
  const bool oob_k = (k_log < k_log_min_) || (k_log > k_log_max_);
  const bool oob_T = (T < T_min_) || (T > T_max_);
  const bool oob_s = (sigma < sigma_min_) || (sigma > sigma_max_);

  k_log = atx::core::clamp(k_log, k_log_min_, k_log_max_);
  T = atx::core::clamp(T, T_min_, T_max_);
  sigma = atx::core::clamp(sigma, sigma_min_, sigma_max_);

  const double xi = detail::cheb_to_unit(k_log, k_log_min_, k_log_max_);
  const double xj = detail::cheb_to_unit(T, T_min_, T_max_);
  const double xk = detail::cheb_to_unit(sigma, sigma_min_, sigma_max_);

  // T16b: the k_log axis scale is baked into dk_coefs_ at build time; only the T
  // and sigma partials still differentiate a query-dependent (Clenshaw-collapsed)
  // vector, so only their axis scales are needed here.
  // Bounded live-span init (see eval): the n_T_*n_s_ prefix is written before it
  // is read by cheb_clenshaw3d / cheb_clenshaw3d_partial; zero just that prefix.
  std::array<double, kTmpSize> tmp_jk;
  std::fill(tmp_jk.data(), tmp_jk.data() + static_cast<std::size_t>(n_T_) * n_s_, 0.0);
  if (out_dk_log) {
    // dC/dk_log: PLAIN value Clenshaw over the precomputed k_log-derivative tensor
    // (T16b). Bit-identical to the pre-change cheb_clenshaw3d_partial(diff_axis==0)
    // because the differentiated rows were produced by the same cheb_diff_coefs on
    // the same coefficients at build, and the innermost-first collapse order is
    // unchanged — no per-query differentiation.
    *out_dk_log = oob_k ? 0.0
                        : detail::cheb_clenshaw3d(dk_coefs_.data(), n_k_, n_T_, n_s_, xi, xj, xk,
                                                  tmp_jk.data());
  }
  if (out_dT) {
    *out_dT = oob_T ? 0.0
                    : detail::cheb_clenshaw3d_partial(coefs_.data(), n_k_, n_T_, n_s_, xi, xj, xk,
                                                      1, scale_T_, tmp_jk.data());
  }
  if (out_dsigma) {
    *out_dsigma = oob_s ? 0.0
                        : detail::cheb_clenshaw3d_partial(coefs_.data(), n_k_, n_T_, n_s_, xi, xj,
                                                          xk, 2, scale_s_, tmp_jk.data());
  }
}

double CorrectionCache::eval_value_and_dsigma(double k_log, double T, double sigma,
                                              double *out_dsigma) const noexcept {
  // Stage (b) [F1]: value + sigma-partial from ONE fused Clenshaw sweep (sigma is
  // the last collapse axis). The VALUE is bit-identical to eval() — the i/j collapses
  // and the sigma-value recurrence are byte-for-byte cheb_clenshaw3d — so the Newton
  // residual is unchanged. The dsigma is the SAME analytic derivative eval_partials()
  // returns, computed by the value+derivative recurrence instead of
  // differentiate-then-evaluate: ULP-different from eval_partials(), bit-identical to
  // eval_second_order()'s dsigma (economic-parity, gated by the P1 |dIV| < 1e-12
  // test). Un-gated by the value here (mirrors eval_partials/eval_grad); the caller
  // applies the served-correction max(0, .) derivative gate.
  if (!populated_) {
    if (out_dsigma != nullptr) {
      *out_dsigma = 0.0;
    }
    return 0.0;
  }
#if defined(ATX_VOL_COUNTERS)
  if ((k_log < k_log_min_) || (k_log > k_log_max_) || (T < T_min_) || (T > T_max_) ||
      (sigma < sigma_min_) || (sigma > sigma_max_)) {
    ATX_VOL_COUNT(CacheOutOfBoxClamps);
  }
#endif
  const bool oob_s = (sigma < sigma_min_) || (sigma > sigma_max_);
  k_log = atx::core::clamp(k_log, k_log_min_, k_log_max_);
  T = atx::core::clamp(T, T_min_, T_max_);
  sigma = atx::core::clamp(sigma, sigma_min_, sigma_max_);

  const double xi = detail::cheb_to_unit(k_log, k_log_min_, k_log_max_);
  const double xj = detail::cheb_to_unit(T, T_min_, T_max_);
  const double xk = detail::cheb_to_unit(sigma, sigma_min_, sigma_max_);
  std::array<double, kTmpSize> tmp_jk;
  std::fill(tmp_jk.data(), tmp_jk.data() + static_cast<std::size_t>(n_T_) * n_s_, 0.0);
  const ClenshawD1 jet =
      cheb_clenshaw3d_value_dsigma(coefs_.data(), n_k_, n_T_, n_s_, xi, xj, xk, tmp_jk.data());
  if (out_dsigma != nullptr) {
    // Out-of-box on sigma nulls the partial (matches eval_partials); the value still
    // clamps. The derivative is in unit (Chebyshev) space -> scale to the physical σ.
    *out_dsigma = oob_s ? 0.0 : jet.d1 * scale_s_;
  }
  // max(0, polynomial): bit-identical to eval().
  return (jet.value > 0.0) ? jet.value : 0.0;
}

CorrSecondOrder CorrectionCache::eval_second_order(double k_log, double T,
                                                   double sigma) const noexcept {
  if (!populated_) {
    return CorrSecondOrder{};
  }

  const bool oob_k = (k_log < k_log_min_) || (k_log > k_log_max_);
  const bool oob_T = (T < T_min_) || (T > T_max_);
  const bool oob_s = (sigma < sigma_min_) || (sigma > sigma_max_);
  k_log = atx::core::clamp(k_log, k_log_min_, k_log_max_);
  T = atx::core::clamp(T, T_min_, T_max_);
  sigma = atx::core::clamp(sigma, sigma_min_, sigma_max_);

  const double xi = detail::cheb_to_unit(k_log, k_log_min_, k_log_max_);
  const double xj = detail::cheb_to_unit(T, T_min_, T_max_);
  const double xk = detail::cheb_to_unit(sigma, sigma_min_, sigma_max_);
  CorrSecondOrder out = cheb_clenshaw3d_second_order(coefs_.data(), n_k_, n_T_, n_s_, xi, xj, xk);

  const bool correction_active = out.value > 0.0;
  out.value = (out.value > 0.0) ? out.value : 0.0;
  out.dk_log *= scale_k_;
  out.dT *= scale_T_;
  out.dsigma *= scale_s_;
  out.dkk *= scale_k_ * scale_k_;
  out.dk_dT *= scale_k_ * scale_T_;
  out.dk_dsigma *= scale_k_ * scale_s_;
  out.dsigma2 *= scale_s_ * scale_s_;

  if (!correction_active) {
    out.dk_log = 0.0;
    out.dT = 0.0;
    out.dsigma = 0.0;
    out.dkk = 0.0;
    out.dk_dT = 0.0;
    out.dk_dsigma = 0.0;
    out.dsigma2 = 0.0;
  }

  if (oob_k) {
    out.dk_log = 0.0;
    out.dkk = 0.0;
    out.dk_dT = 0.0;
    out.dk_dsigma = 0.0;
  }
  if (oob_T) {
    out.dT = 0.0;
    out.dk_dT = 0.0;
  }
  if (oob_s) {
    out.dsigma = 0.0;
    out.dk_dsigma = 0.0;
    out.dsigma2 = 0.0;
  }
  return out;
}

double CorrectionCache::eval_grad(double k_log, double T, double sigma, double *out_dk_log,
                                  double *out_dT, double *out_dsigma) const noexcept {
  // Public value+partials behavior is EXACTLY the composition of the value-only
  // eval() and the partials-only eval_partials(): the discarded value sweep never
  // fed the partial sweeps (each partial sweep rewrites its own scratch before
  // reading it), and eval() reproduces the value computation bit-for-bit. Callers
  // that discard the value should call eval_partials directly to skip the sweep.
  const double v = eval(k_log, T, sigma);
  eval_partials(k_log, T, sigma, out_dk_log, out_dT, out_dsigma);
  return v;
}

bool CorrectionBlend::valid() const noexcept {
  if (!std::isfinite(upper_weight) || upper_weight < 0.0 || upper_weight > 1.0) {
    return false;
  }
  const auto populated = [](const CorrectionCache *cache) noexcept {
    return cache != nullptr && cache->populated();
  };
  if (upper_weight == 0.0) {
    return populated(lower);
  }
  if (upper_weight == 1.0) {
    return populated(upper);
  }
  if (lower == upper) {
    return populated(lower);
  }
  return populated(lower) && populated(upper) && lower->side() == upper->side();
}

bool CorrectionBlend::usable(Side side) const noexcept {
  if (!valid()) {
    return false;
  }
  if (upper_weight == 1.0) {
    return upper->side() == side;
  }
  return lower->side() == side;
}

double CorrectionBlend::baked_r() const noexcept {
  if (!valid()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // eval() reads `upper` only at weight 1; every other case reads `lower`. The two
  // blended endpoints share the same baked rate (the carry bank blends only q), so
  // the active endpoint's baked_r is the blend's baked rate.
  const CorrectionCache *active = (upper_weight == 1.0) ? upper : lower;
  return active->baked_r();
}

bool CorrectionBlend::contains(double k_log, double T, double sigma) const noexcept {
  if (!valid()) {
    return false;
  }
  // Mirror eval()'s endpoint selection: a query is in-box iff every cache eval()
  // would actually read contains it (so nothing is clamped to a box edge).
  if (upper_weight == 0.0 || lower == upper) {
    return lower->contains(k_log, T, sigma);
  }
  if (upper_weight == 1.0) {
    return upper->contains(k_log, T, sigma);
  }
  return lower->contains(k_log, T, sigma) && upper->contains(k_log, T, sigma);
}

double CorrectionBlend::eval(double k_log, double T, double sigma) const noexcept {
  if (!valid()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (upper_weight == 0.0 || lower == upper) {
    return lower->eval(k_log, T, sigma);
  }
  if (upper_weight == 1.0) {
    return upper->eval(k_log, T, sigma);
  }
  const double lo = lower->eval(k_log, T, sigma);
  const double hi = upper->eval(k_log, T, sigma);
  return lo + upper_weight * (hi - lo);
}

double CorrectionBlend::eval_dsigma(double k_log, double T, double sigma) const noexcept {
  if (!valid()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double lo = 0.0;
  if (upper_weight == 0.0 || lower == upper) {
    const double value = lower->eval_grad(k_log, T, sigma, nullptr, nullptr, &lo);
    if (!(value > 0.0)) {
      lo = 0.0;
    }
    return lo;
  }
  double hi = 0.0;
  if (upper_weight == 1.0) {
    const double value = upper->eval_grad(k_log, T, sigma, nullptr, nullptr, &hi);
    if (!(value > 0.0)) {
      hi = 0.0;
    }
    return hi;
  }
  const double lo_value = lower->eval_grad(k_log, T, sigma, nullptr, nullptr, &lo);
  const double hi_value = upper->eval_grad(k_log, T, sigma, nullptr, nullptr, &hi);
  if (!(lo_value > 0.0)) {
    lo = 0.0;
  }
  if (!(hi_value > 0.0)) {
    hi = 0.0;
  }
  return lo + upper_weight * (hi - lo);
}

double CorrectionBlend::eval_value_and_dsigma(double k_log, double T, double sigma,
                                              double *out_dsigma) const noexcept {
  // Bit-identical to {eval(), eval_dsigma()} but sharing each endpoint's fused
  // value+dsigma kernel: the value blends as (1-w)*lower + w*upper and the sigma
  // partial applies the SAME per-endpoint max(0, .) gate as eval_dsigma() before
  // blending. Exact endpoints / identical pointers evaluate one cache only, so a
  // single-cache blend reproduces the CorrectionCache path byte-for-byte.
  if (!valid()) {
    if (out_dsigma != nullptr) {
      *out_dsigma = std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (upper_weight == 0.0 || lower == upper) {
    double lo = 0.0;
    const double value = lower->eval_value_and_dsigma(k_log, T, sigma, &lo);
    if (!(value > 0.0)) {
      lo = 0.0;
    }
    if (out_dsigma != nullptr) {
      *out_dsigma = lo;
    }
    return value;
  }
  if (upper_weight == 1.0) {
    double hi = 0.0;
    const double value = upper->eval_value_and_dsigma(k_log, T, sigma, &hi);
    if (!(value > 0.0)) {
      hi = 0.0;
    }
    if (out_dsigma != nullptr) {
      *out_dsigma = hi;
    }
    return value;
  }
  double lo = 0.0;
  double hi = 0.0;
  const double lo_value = lower->eval_value_and_dsigma(k_log, T, sigma, &lo);
  const double hi_value = upper->eval_value_and_dsigma(k_log, T, sigma, &hi);
  if (!(lo_value > 0.0)) {
    lo = 0.0;
  }
  if (!(hi_value > 0.0)) {
    hi = 0.0;
  }
  if (out_dsigma != nullptr) {
    *out_dsigma = lo + upper_weight * (hi - lo);
  }
  return lo_value + upper_weight * (hi_value - lo_value);
}

double CorrectionBlend::eval_value_dk(double k_log, double T, double sigma,
                                      double *out_dk_log) const noexcept {
  if (!valid()) {
    if (out_dk_log != nullptr) {
      *out_dk_log = std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (upper_weight == 0.0 || lower == upper) {
    return lower->eval_value_dk(k_log, T, sigma, out_dk_log);
  }
  if (upper_weight == 1.0) {
    return upper->eval_value_dk(k_log, T, sigma, out_dk_log);
  }
  double lo_dk = 0.0;
  double hi_dk = 0.0;
  const double lo_value = lower->eval_value_dk(k_log, T, sigma, &lo_dk);
  const double hi_value = upper->eval_value_dk(k_log, T, sigma, &hi_dk);
  if (out_dk_log != nullptr) {
    *out_dk_log = lo_dk + upper_weight * (hi_dk - lo_dk);
  }
  return lo_value + upper_weight * (hi_value - lo_value);
}

CorrSecondOrder CorrectionBlend::eval_second_order(double k_log, double T,
                                                   double sigma) const noexcept {
  if (!valid()) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return CorrSecondOrder{nan, nan, nan, nan, nan, nan, nan, nan};
  }
  if (upper_weight == 0.0 || lower == upper) {
    return lower->eval_second_order(k_log, T, sigma);
  }
  if (upper_weight == 1.0) {
    return upper->eval_second_order(k_log, T, sigma);
  }
  const CorrSecondOrder lo = lower->eval_second_order(k_log, T, sigma);
  const CorrSecondOrder hi = upper->eval_second_order(k_log, T, sigma);
  const auto blend = [weight = upper_weight](double a, double b) noexcept {
    return a + weight * (b - a);
  };
  return CorrSecondOrder{
      blend(lo.value, hi.value),         blend(lo.dk_log, hi.dk_log),  blend(lo.dT, hi.dT),
      blend(lo.dsigma, hi.dsigma),       blend(lo.dkk, hi.dkk),        blend(lo.dk_dT, hi.dk_dT),
      blend(lo.dk_dsigma, hi.dk_dsigma), blend(lo.dsigma2, hi.dsigma2)};
}

Result<CorrResult> CorrectionCache::query(double k_log, double T, double sigma,
                                          CorrPartials want) const {
  CorrResult out;

  const CorrPartials partials = want & (CorrPartials::Dk | CorrPartials::Dt | CorrPartials::Dsigma);

  // Non-default extrap policies short-circuit before the kernel on any-axis OOB.
  if (extrap_policy_ != ExtrapPolicy::Clamp) {
    const bool oob = (k_log < k_log_min_) || (k_log > k_log_max_) || (T < T_min_) || (T > T_max_) ||
                     (sigma < sigma_min_) || (sigma > sigma_max_);
    if (oob) {
      out.value = std::numeric_limits<double>::quiet_NaN();
      out.mask_filled = CorrPartials::Value | partials;
      if (extrap_policy_ == ExtrapPolicy::ErrorOutside) {
        return Err(ErrorCode::OutOfRange, "CorrectionCache::query: point outside box");
      }
      return Ok(out); // NanOutside: success with NaN value.
    }
  }

  if (!any(partials)) {
    out.value = eval(k_log, T, sigma);
  } else {
    double *p_dk = has(want, CorrPartials::Dk) ? &out.dk_log : nullptr;
    double *p_dT = has(want, CorrPartials::Dt) ? &out.dT : nullptr;
    double *p_ds = has(want, CorrPartials::Dsigma) ? &out.dsigma : nullptr;
    out.value = eval_grad(k_log, T, sigma, p_dk, p_dT, p_ds);
  }

  out.mask_filled = CorrPartials::Value | partials;
  return Ok(out);
}

Status CorrectionCache::set_extrap_policy(ExtrapPolicy policy) noexcept {
  if (policy != ExtrapPolicy::Clamp && policy != ExtrapPolicy::NanOutside &&
      policy != ExtrapPolicy::ErrorOutside) {
    return Err(ErrorCode::InvalidArgument, "CorrectionCache::set_extrap_policy: unknown policy");
  }
  extrap_policy_ = policy;
  return Ok();
}

// ── Equal-T cached ladder batch (perf review F4) ─────────────────────────────

namespace {

// Mirror american.cpp's file-local floor_cached_price (not exported there): the
// cached served mark is floored at max(price, intrinsic, euro, 0). Reproduced so
// the ladder batch's per-strike assembly is byte-identical to the scalar cached
// entry apart from the (economic-parity) correction reorder.
[[nodiscard]] inline double ladder_floor_price(double price, double euro,
                                               double intrinsic) noexcept {
  if (intrinsic > price) {
    price = intrinsic;
  }
  if (euro > price) {
    price = euro;
  }
  if (price < 0.0) {
    price = 0.0;
  }
  return price;
}

} // namespace

Status american_price_cached_ladder(double S, std::span<const double> strikes,
                                    std::span<const double> sigmas, double T, double r, double q,
                                    Side side, const CorrectionBlend &correction,
                                    std::span<double> price_out) {
  const std::size_t n = strikes.size();
  if (sigmas.size() != n || price_out.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "american_price_cached_ladder: strikes/sigmas/price_out length mismatch");
  }
  if (!std::isfinite(T) || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "american_price_cached_ladder: non-finite or non-positive T");
  }

  // Degenerate / rare inputs delegate to the scalar cached entry so the batch is a
  // pure fast-path specialization: an unusable blend (cold Andersen-Lake fallback)
  // or a double-continuation query regime (NaN) reproduce the scalar path exactly.
  const bool unsupported = detail::classify_regime(/*rate=*/(side == Side::Put) ? r : q,
                                                   /*yield=*/(side == Side::Put) ? q : r) ==
                           detail::ExerciseRegime::Unsupported;
  if (!correction.usable(side) || unsupported) {
    for (std::size_t i = 0; i < n; ++i) {
      const double K = strikes[i];
      price_out[i] = (std::isfinite(K) && K > 0.0)
                         ? american_price_cached(S, K, T, sigmas[i], r, q, side, correction)
                         : std::numeric_limits<double>::quiet_NaN();
    }
    return Ok();
  }

  // T-invariant hoists (S, r, q, T fixed across the ladder): F, the discount, and
  // √T are all strike-invariant, so they are computed ONCE for the whole ladder.
  const double df = std::exp(-r * T);
  const double F = S * std::exp((r - q) * T);
  const double sqrt_t = std::sqrt(T);

  // Resolve up to two active endpoints + the blend weight, mirroring
  // CorrectionBlend::eval's exact-endpoint / interior split, then collapse each
  // usable endpoint's T (j) axis into its (k_log, sigma) plane ONCE.
  const CorrectionCache *lo_cache = nullptr;
  const CorrectionCache *hi_cache = nullptr;
  double weight = 0.0;
  if (correction.upper_weight == 0.0 || correction.lower == correction.upper) {
    lo_cache = correction.lower;
  } else if (correction.upper_weight == 1.0) {
    lo_cache = correction.upper;
  } else {
    lo_cache = correction.lower;
    hi_cache = correction.upper;
    weight = correction.upper_weight;
  }

  std::array<double, kTmpSize> lo_plane;
  std::array<double, kTmpSize> hi_plane;
  lo_cache->collapse_T_plane(T, lo_plane.data());
  if (hi_cache != nullptr) {
    hi_cache->collapse_T_plane(T, hi_plane.data());
  }

  for (std::size_t i = 0; i < n; ++i) {
    const double K = strikes[i];
    if (!(std::isfinite(K) && K > 0.0)) {
      price_out[i] = std::numeric_limits<double>::quiet_NaN();
      continue;
    }
    const double sigma = sigmas[i];
    const double ln_fk = std::log(F / K);
    const double euro = black76_price_from_lnfk(F, K, T, sigma, df, ln_fk, sqrt_t, side);
    const double k_log = -ln_fk;
    // Correction: per-endpoint max(0, .) then (1-w)·lo + w·hi (matches
    // CorrectionBlend::eval), evaluated over the pre-collapsed T planes.
    double corr = lo_cache->eval_plane(lo_plane.data(), k_log, sigma);
    if (hi_cache != nullptr) {
      const double hi = hi_cache->eval_plane(hi_plane.data(), k_log, sigma);
      corr = corr + weight * (hi - corr);
    }
    const double intr = (side == Side::Put) ? (K - S) : (S - K);
    price_out[i] = ladder_floor_price(euro + F * corr, euro, intr);
  }
  return Ok();
}

} // namespace atx::vol
