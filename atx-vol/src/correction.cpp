#include "atx/vol/correction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/math.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/counters.hpp"  // ATX_VOL_COUNT (opt-in P0.2; no-op when OFF)

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
inline constexpr std::size_t kTmpSize =
    static_cast<std::size_t>(detail::kChebMaxNodes) *
    static_cast<std::size_t>(detail::kChebMaxNodes);

// Correction sample at one (k_log, T, sigma) node, normalized to F = 1:
//   S = e^{-(r-q)T}, K = e^{k_log},  c = P_amer(S,K,...) - P_euro(S,K,...).
// Reuses the Andersen-Lake cold pricer and the Black-76 European kernel (with
// F = 1 the two agree with the C `euro_at`). Non-convergence / domain failures
// fall back to a zero correction, matching the C populator.
[[nodiscard]] double sample_correction(double k_log, double T, double sigma,
                                       double r, double q, Side side,
                                       const std::optional<AlOpts>& opts) {
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

}  // namespace

namespace detail {

double cheb_node(std::uint16_t j, std::uint16_t n) noexcept {
  if (n == 0) {
    return 0.0;
  }
  return std::cos(kPi * (2.0 * static_cast<double>(j) + 1.0) /
                  (2.0 * static_cast<double>(n)));
}

void cheb_dct2(const double* vals, double* coefs, std::uint16_t n) noexcept {
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

double cheb_clenshaw1d(const double* coefs, std::uint16_t n, double x) noexcept {
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

void cheb_diff_coefs(const double* c, double* d, std::uint16_t n,
                     double scale) noexcept {
  ATX_VOL_COUNT(ChebDiffCoefs);  // one derivative-coef transform (opt-in P0.2; no-op when OFF)
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
  d[0] *= 0.5;  // Numerical Recipes halving to match the full-c0 Clenshaw form.
  if (scale != 1.0) {
    for (std::uint16_t k = 0u; k < n; ++k) {
      d[k] *= scale;
    }
  }
}

double cheb_clenshaw3d(const double* coefs, std::uint16_t n_k, std::uint16_t n_T,
                       std::uint16_t n_s, double xi, double xj, double xk,
                       double* tmp_jk) noexcept {
  if (n_k == 0u || n_T == 0u || n_s == 0u) {
    return 0.0;
  }
  ATX_VOL_COUNT(ClenshawSweeps);  // one value sweep (opt-in P0.2; no-op when OFF)
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
      const double* row = coefs + j * ns * nk + k * nk;
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
    const double a0 = tmp_jk[k];  // j = 0 row, before overwrite
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

double cheb_clenshaw3d_partial(const double* coefs, std::uint16_t n_k,
                               std::uint16_t n_T, std::uint16_t n_s, double xi,
                               double xj, double xk, int diff_axis,
                               double axis_scale, double* tmp_jk) noexcept {
  if (n_k == 0u || n_T == 0u || n_s == 0u) {
    return 0.0;
  }
  ATX_VOL_COUNT(ClenshawSweeps);  // one partial sweep (opt-in P0.2; no-op when OFF)
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
      const double* row = coefs + j * ns * nk + k * nk;
      const double* eval_row = row;
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
        const double bk =
            tmp_jk[static_cast<std::size_t>(j) * ns + k] + two_xj * bk1 - bk2;
        bk2 = bk1;
        bk1 = bk;
      }
      const double a0 = tmp_jk[k];  // j = 0 row, before overwrite
      tmp_jk[k] = a0 + xj * bk1 - bk2;
    }
  }

  // 3rd collapse: k-axis (sigma). diff_axis == 2 differentiates first.
  const double* eval_vec = tmp_jk;
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

}  // namespace detail

// ── CorrectionCache ─────────────────────────────────────────────────────

Result<CorrectionCache> CorrectionCache::build(
    std::uint16_t n_log_moneyness, std::uint16_t n_T_nodes,
    std::uint16_t n_sigma_nodes, double r, double q, double k_log_min,
    double k_log_max, double T_min, double T_max, double sigma_min,
    double sigma_max, Side side, const std::optional<AlOpts>& opts) {
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
  cache.r_ = r;
  cache.q_ = q;
  cache.side_ = side;

  const std::uint16_t n_k = cache.n_k_;
  const std::uint16_t n_T = cache.n_T_;
  const std::uint16_t n_s = cache.n_s_;
  cache.coefs_.assign(static_cast<std::size_t>(n_k) * static_cast<std::size_t>(n_T) *
                          static_cast<std::size_t>(n_s),
                      0.0);
  double* coefs = cache.coefs_.data();

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
      double* row = coefs + detail::cheb_idx(0, j, k, n_k, n_s);  // i-contiguous
      bool used_slice = false;
      if (side == Side::Call || side == Side::Put) {
        for (std::uint16_t i = 0; i < n_k; ++i) {
          strike_buf[i] = std::exp(k_log_grid[i]);
        }
        const Status st =
            (side == Side::Call)
                ? andersen_lake_call_slice(
                      S_j, std::span<const double>(strike_buf.data(), n_k), Tj, sig, r, q,
                      std::span<double>(px_buf.data(), n_k), opts)
                : andersen_lake_put_slice(
                      S_j, std::span<const double>(strike_buf.data(), n_k), Tj, sig, r, q,
                      std::span<double>(px_buf.data(), n_k), opts);
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
      double* row = coefs + detail::cheb_idx(0, j, k, n_k, n_s);
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
    const double scale_k = 2.0 / (k_log_max - k_log_min);
    std::array<double, kChebMaxNodes> drow{};
    for (std::uint16_t j = 0; j < n_T; ++j) {
      for (std::uint16_t k = 0; k < n_s; ++k) {
        const double* row = coefs + detail::cheb_idx(0, j, k, n_k, n_s);
        detail::cheb_diff_coefs(row, drow.data(), n_k, scale_k);
        double* drow_dst = cache.dk_coefs_.data() + detail::cheb_idx(0, j, k, n_k, n_s);
        for (std::uint16_t i = 0; i < n_k; ++i) {
          drow_dst[i] = drow[i];
        }
      }
    }
  }

  cache.populated_ = true;
  return Ok(std::move(cache));
}

double CorrectionCache::eval(double k_log, double T, double sigma) const noexcept {
  if (!populated_) {
    return 0.0;
  }
#if defined(ATX_VOL_COUNTERS)
  // The box test itself (not just the counter increment) only exists in the ON
  // build: with ATX_VOL_COUNTERS undefined this whole block is gone at the
  // preprocessor, not merely dead code left for the optimizer to remove.
  if ((k_log < k_log_min_) || (k_log > k_log_max_) || (T < T_min_) ||
      (T > T_max_) || (sigma < sigma_min_) || (sigma > sigma_max_)) {
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
  std::fill(tmp_jk.data(),
            tmp_jk.data() + static_cast<std::size_t>(n_T_) * n_s_, 0.0);
  const double v = detail::cheb_clenshaw3d(coefs_.data(), n_k_, n_T_, n_s_, xi, xj,
                                           xk, tmp_jk.data());
  return (v > 0.0) ? v : 0.0;
}

void CorrectionCache::eval_partials(double k_log, double T, double sigma,
                                    double* out_dk_log, double* out_dT,
                                    double* out_dsigma) const noexcept {
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
  const double scale_T = 2.0 / (T_max_ - T_min_);
  const double scale_s = 2.0 / (sigma_max_ - sigma_min_);

  // Bounded live-span init (see eval): the n_T_*n_s_ prefix is written before it
  // is read by cheb_clenshaw3d / cheb_clenshaw3d_partial; zero just that prefix.
  std::array<double, kTmpSize> tmp_jk;
  std::fill(tmp_jk.data(),
            tmp_jk.data() + static_cast<std::size_t>(n_T_) * n_s_, 0.0);
  if (out_dk_log) {
    // dC/dk_log: PLAIN value Clenshaw over the precomputed k_log-derivative tensor
    // (T16b). Bit-identical to the pre-change cheb_clenshaw3d_partial(diff_axis==0)
    // because the differentiated rows were produced by the same cheb_diff_coefs on
    // the same coefficients at build, and the innermost-first collapse order is
    // unchanged — no per-query differentiation.
    *out_dk_log = oob_k ? 0.0
                        : detail::cheb_clenshaw3d(dk_coefs_.data(), n_k_, n_T_,
                                                  n_s_, xi, xj, xk, tmp_jk.data());
  }
  if (out_dT) {
    *out_dT = oob_T ? 0.0
                    : detail::cheb_clenshaw3d_partial(coefs_.data(), n_k_, n_T_, n_s_,
                                                      xi, xj, xk, 1, scale_T,
                                                      tmp_jk.data());
  }
  if (out_dsigma) {
    *out_dsigma = oob_s ? 0.0
                        : detail::cheb_clenshaw3d_partial(coefs_.data(), n_k_, n_T_,
                                                          n_s_, xi, xj, xk, 2,
                                                          scale_s, tmp_jk.data());
  }
}

double CorrectionCache::eval_grad(double k_log, double T, double sigma,
                                  double* out_dk_log, double* out_dT,
                                  double* out_dsigma) const noexcept {
  // Public value+partials behavior is EXACTLY the composition of the value-only
  // eval() and the partials-only eval_partials(): the discarded value sweep never
  // fed the partial sweeps (each partial sweep rewrites its own scratch before
  // reading it), and eval() reproduces the value computation bit-for-bit. Callers
  // that discard the value should call eval_partials directly to skip the sweep.
  const double v = eval(k_log, T, sigma);
  eval_partials(k_log, T, sigma, out_dk_log, out_dT, out_dsigma);
  return v;
}

Result<CorrResult> CorrectionCache::query(double k_log, double T, double sigma,
                                          CorrPartials want) const {
  CorrResult out;

  const CorrPartials partials = want & (CorrPartials::Dk | CorrPartials::Dt | CorrPartials::Dsigma);

  // Non-default extrap policies short-circuit before the kernel on any-axis OOB.
  if (extrap_policy_ != ExtrapPolicy::Clamp) {
    const bool oob = (k_log < k_log_min_) || (k_log > k_log_max_) ||
                     (T < T_min_) || (T > T_max_) || (sigma < sigma_min_) ||
                     (sigma > sigma_max_);
    if (oob) {
      out.value = std::numeric_limits<double>::quiet_NaN();
      out.mask_filled = CorrPartials::Value | partials;
      if (extrap_policy_ == ExtrapPolicy::ErrorOutside) {
        return Err(ErrorCode::OutOfRange, "CorrectionCache::query: point outside box");
      }
      return Ok(out);  // NanOutside: success with NaN value.
    }
  }

  if (!any(partials)) {
    out.value = eval(k_log, T, sigma);
  } else {
    double* p_dk = has(want, CorrPartials::Dk) ? &out.dk_log : nullptr;
    double* p_dT = has(want, CorrPartials::Dt) ? &out.dT : nullptr;
    double* p_ds = has(want, CorrPartials::Dsigma) ? &out.dsigma : nullptr;
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

}  // namespace atx::vol
