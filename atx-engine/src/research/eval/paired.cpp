#include "atx/engine/research/eval/paired.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "atx/engine/research/eval/house_moments.hpp"
#include "atx/engine/research/eval/numpy_rng.hpp"
#include "atx/engine/research/numpy_reduce.hpp"

namespace atx::engine::research::eval {
namespace {

constexpr usize kM = 4U; // the moment columns a, b, a^2, b^2
using Vec4 = std::array<f64, kM>;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

struct PopDsr {
  f64 d{};
  Vec4 grad{};
};

// nav_summ._dsr_population: dSR per session (population SD) and its gradient in
// (mu_a, mu_b, E a^2, E b^2).
[[nodiscard]] PopDsr dsr_population(const Vec4 &m) {
  const f64 mu_a = m[0];
  const f64 mu_b = m[1];
  const f64 g_a = m[2];
  const f64 g_b = m[3];
  const f64 va = g_a - mu_a * mu_a;
  const f64 vb = g_b - mu_b * mu_b;
  PopDsr out;
  out.d = mu_a / std::sqrt(va) - mu_b / std::sqrt(vb);
  out.grad = {g_a / std::pow(va, 1.5), -g_b / std::pow(vb, 1.5), -mu_a / (2.0 * std::pow(va, 1.5)),
              mu_b / (2.0 * std::pow(vb, 1.5))};
  return out;
}

// One row's influence term g'(y_k - m) of the dSR: the cell's Sharpe terms plus the reference's,
// summed last, so the one cancellation (the two Sharpe influence series, nearly equal as rho -> 1)
// happens on two numbers of the series' own scale.
[[nodiscard]] f64 influence(const Vec4 &g, const Vec4 &y, const Vec4 &m) {
  const f64 cell = g[0] * (y[0] - m[0]) + g[2] * (y[2] - m[2]);
  const f64 ref = g[1] * (y[1] - m[1]) + g[3] * (y[3] - m[3]);
  return cell + ref;
}

// g' Psi g of nav_summ.bartlett_psi (Bartlett-kernel HAC long-run covariance of the rows of y,
// T x 4, autocovariances / T, lag = block - 1), formed as the long-run variance of the scalar
// influence series z_k = g'(y_k - mean): g' Gamma_l g = sum_k z_{k+l} z_k / T term by term, so it
// is the same statistic (ruling B2-R1 (a)) without the 4 x 4 quadratic form whose terms cancel to
// 1 - rho as rho -> 1. Column means sequential (numpy's axis-0 order).
[[nodiscard]] f64 bartlett_quadratic(const std::vector<Vec4> &y, const Vec4 &g, usize lag) {
  const usize t = y.size();
  const f64 tf = static_cast<f64>(t);
  Vec4 mean{};
  for (const Vec4 &r : y) {
    for (usize c = 0; c < kM; ++c) {
      mean[c] += r[c];
    }
  }
  for (usize c = 0; c < kM; ++c) {
    mean[c] /= tf;
  }
  std::vector<f64> z(t);
  for (usize k = 0; k < t; ++k) {
    z[k] = influence(g, y[k], mean);
  }
  f64 acc = 0.0;
  for (usize k = 0; k < t; ++k) {
    acc += z[k] * z[k];
  }
  f64 q = acc / tf;
  const usize top = std::min(lag, t - 1U);
  for (usize ell = 1; ell <= top; ++ell) {
    f64 cross = 0.0;
    for (usize k = 0; k + ell < t; ++k) {
      cross += z[k + ell] * z[k];
    }
    const f64 w = 1.0 - static_cast<f64>(ell) / (static_cast<f64>(lag) + 1.0);
    q += w * (2.0 * cross / tf);
  }
  return q;
}

// np.corrcoef(a, b)[0, 1]: row means pairwise, cross products times 1 / (T - 1), two divisions
// by the standard deviations, clipped to [-1, 1].
[[nodiscard]] f64 pearson(std::span<const f64> a, std::span<const f64> b) {
  const usize t = a.size();
  const f64 ma = numpy::mean(a);
  const f64 mb = numpy::mean(b);
  f64 s00 = 0.0;
  f64 s11 = 0.0;
  f64 s01 = 0.0;
  for (usize k = 0; k < t; ++k) {
    const f64 da = a[k] - ma;
    const f64 db = b[k] - mb;
    s00 += da * da;
    s11 += db * db;
    s01 += da * db;
  }
  const f64 inv = 1.0 / static_cast<f64>(t - 1U);
  const f64 rho = s01 * inv / std::sqrt(s00 * inv) / std::sqrt(s11 * inv);
  return std::isnan(rho) ? rho : std::clamp(rho, -1.0, 1.0);
}

// 1 - rho from the standardized difference series: sum((za - zb)^2) = 2 (T - 1)(1 - rho) exactly,
// za = (a - mean) / sd (ddof 1). A sum of squares of small differences keeps its relative
// precision where 1 - corrcoef loses it (ruling B2-R1 (a)). NaN when either sd is not positive.
[[nodiscard]] f64 one_minus_rho(std::span<const f64> a, std::span<const f64> b, f64 sa, f64 sb) {
  if (!(sa > 0.0 && sb > 0.0)) {
    return kNaN;
  }
  const usize t = a.size();
  const f64 ma = numpy::mean(a);
  const f64 mb = numpy::mean(b);
  f64 acc = 0.0;
  for (usize k = 0; k < t; ++k) {
    const f64 dz = (a[k] - ma) / sa - (b[k] - mb) / sb;
    acc += dz * dz;
  }
  return acc / (2.0 * static_cast<f64>(t - 1U));
}

// nav_summ.memmel_se (annualized SRs in, annualized SE out) in the stable form of ruling B2-R1 (a):
// 2 - 2 rho = 2 omr and a^2 + b^2 - 2 a b rho^2 = (a - b)^2 + 2 a b omr (2 - omr), omr = 1 - rho.
[[nodiscard]] f64 memmel_se(f64 sr_a, f64 sr_b, f64 omr, usize t) {
  const f64 root = std::sqrt(kAnnualization);
  const f64 a = sr_a / root;
  const f64 b = sr_b / root;
  const f64 diff = a - b;
  const f64 one_minus_rho2 = omr * (2.0 - omr);
  const f64 var =
      (2.0 * omr + 0.5 * (diff * diff + 2.0 * a * b * one_minus_rho2)) / static_cast<f64>(t);
  return std::sqrt(std::max(var, 0.0) * kAnnualization);
}

// The finite values of `v`, sorted ascending (np.quantile's partition input).
[[nodiscard]] std::vector<f64> sorted_finite(const std::vector<f64> &v) {
  std::vector<f64> out;
  for (const f64 x : v) {
    if (std::isfinite(x)) {
      out.push_back(x);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

core::Result<PairedStats> paired_stats(std::span<const f64> a, std::span<const f64> b,
                                       const PairedConfig &config) {
  const usize t = a.size();
  if (t != b.size() || t < 3U) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "paired_stats: need two aligned series of >= 3 sessions");
  }
  if (config.draws == 0U || config.block == 0U || t > 0xFFFFFFFFULL) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "paired_stats: draws and block must be >= 1 and T below 2^32");
  }
  const f64 root = std::sqrt(kAnnualization);
  const f64 tf = static_cast<f64>(t);
  PairedStats out;
  out.sessions = t;
  out.config = config;
  out.sr = house_sharpe(a);
  out.sr_reference = house_sharpe(b);
  const f64 sa = numpy::std_dev(a, 1U);
  const f64 sb = numpy::std_dev(b, 1U);
  out.rho = (sa > 0.0 && sb > 0.0) ? pearson(a, b) : kNaN;
  out.one_minus_rho = one_minus_rho(a, b, sa, sb);
  out.degenerate = out.one_minus_rho < kDegenerateOneMinusRho;
  out.dsr = out.sr - out.sr_reference;
  out.memmel_se = out.degenerate ? kNaN : memmel_se(out.sr, out.sr_reference, out.one_minus_rho, t);
  if (out.memmel_se > 0.0) {
    out.t = out.dsr / out.memmel_se;
  }

  std::vector<Vec4> y(t);
  for (usize k = 0; k < t; ++k) {
    y[k] = {a[k], b[k], a[k] * a[k], b[k] * b[k]};
  }
  Vec4 y_mean{};
  for (const Vec4 &r : y) {
    for (usize c = 0; c < kM; ++c) {
      y_mean[c] += r[c];
    }
  }
  for (usize c = 0; c < kM; ++c) {
    y_mean[c] /= tf;
  }
  const PopDsr pop0 = dsr_population(y_mean);
  const f64 d0 = pop0.d;
  const bool grad_finite = std::all_of(pop0.grad.begin(), pop0.grad.end(),
                                       [](f64 g) { return std::isfinite(g); });
  const f64 s0 =
      grad_finite
          ? std::sqrt(std::max(bartlett_quadratic(y, pop0.grad, config.block - 1U), 0.0) / tf)
          : 0.0;

  const usize blocks = (t + config.block - 1U) / config.block;
  NumpyGenerator gen(config.seed);
  std::vector<usize> starts(blocks);
  std::vector<f64> xa(t);
  std::vector<f64> xb(t);
  std::vector<Vec4> ys(t);
  std::vector<f64> boot(config.draws);
  std::vector<f64> tstar(config.draws);
  std::vector<f64> ds(config.draws);
  std::vector<f64> ss(config.draws);
  for (usize d = 0; d < config.draws; ++d) {
    for (usize j = 0; j < blocks; ++j) {
      starts[j] = static_cast<usize>(gen.integers(t));
    }
    for (usize i = 0; i < t; ++i) {
      const usize idx = (starts[i / config.block] + i % config.block) % t;
      xa[i] = a[idx];
      xb[i] = b[idx];
      ys[i] = y[idx];
    }
    boot[d] =
        (numpy::mean(xa) / numpy::std_dev(xa, 1U) - numpy::mean(xb) / numpy::std_dev(xb, 1U)) *
        root;
    Vec4 ms{};
    for (const Vec4 &r : ys) {
      for (usize c = 0; c < kM; ++c) {
        ms[c] += r[c];
      }
    }
    for (usize c = 0; c < kM; ++c) {
      ms[c] /= tf;
    }
    const PopDsr pop = dsr_population(ms);
    // g' Psi* g with Psi* = sum_j S_j S_j' / T: the squares of the block sums of the influence
    // series, the same statistic as the 4 x 4 form (ruling B2-R1 (a)).
    f64 q = 0.0;
    for (usize j = 0; j < blocks; ++j) {
      f64 acc = 0.0;
      const usize end = std::min((j + 1U) * config.block, t);
      for (usize r = j * config.block; r < end; ++r) {
        acc += influence(pop.grad, ys[r], ms);
      }
      q += acc * acc;
    }
    ds[d] = pop.d;
    ss[d] = std::sqrt(q / tf / tf);
    tstar[d] = std::abs(pop.d - d0) / ss[d];
  }

  const std::vector<f64> ok = sorted_finite(boot);
  out.cbb_valid_draws = ok.size();
  out.cbb_lo = ok.empty() ? kNaN : numpy::quantile_linear(ok, 0.025);
  out.cbb_hi = ok.empty() ? kNaN : numpy::quantile_linear(ok, 0.975);

  std::vector<usize> good;
  for (usize d = 0; d < config.draws; ++d) {
    if (std::isfinite(tstar[d]) && ss[d] > 0.0) {
      good.push_back(d);
    }
  }
  out.lw.dsr_population = d0 * root;
  if (out.degenerate) { // ruling B2-R1 (c): no studentized inference on a degenerate pair
    out.lw.se = kNaN;
    out.lw.valid_draws = 0U;
    return core::Ok(std::move(out));
  }
  out.lw.se = s0 * root;
  out.lw.valid_draws = good.size();
  if (!good.empty() && s0 > 0.0 && std::isfinite(d0)) {
    std::vector<f64> good_t;
    good_t.reserve(good.size());
    for (const usize d : good) {
      good_t.push_back(tstar[d]);
    }
    std::sort(good_t.begin(), good_t.end());
    const f64 c = numpy::quantile_linear(good_t, 0.95);
    out.lw.ci_lo = (d0 - c * s0) * root;
    out.lw.ci_hi = (d0 + c * s0) * root;
    const f64 two_bar = std::abs(d0) / s0;
    const f64 one_bar = d0 / s0;
    usize two = 0;
    usize one = 0;
    for (const usize d : good) {
      two += tstar[d] >= two_bar ? 1U : 0U;
      one += (ds[d] - d0) / ss[d] >= one_bar ? 1U : 0U;
    }
    const f64 denom = static_cast<f64>(good.size()) + 1.0;
    out.lw.p_value = (static_cast<f64>(two) + 1.0) / denom;
    out.lw.p_one_sided = (static_cast<f64>(one) + 1.0) / denom;
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::eval
