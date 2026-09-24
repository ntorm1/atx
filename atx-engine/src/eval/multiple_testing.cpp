// atx::engine::eval — multiple-testing control plane (see multiple_testing.hpp).
#include "atx/engine/eval/multiple_testing.hpp"

#include <algorithm> // std::sort, std::min, std::max
#include <cmath>     // std::sqrt, std::log, std::log1p, std::isfinite, std::floor
#include <limits>    // std::numeric_limits
#include <numeric>   // std::iota
#include <thread>    // std::thread
#include <utility>   // std::move

namespace atx::engine::eval {

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;

// ---------------------------------------------------------------------------
//  p.adjust helpers.
// ---------------------------------------------------------------------------

// Indices ordering p descending (stable), as R's order(p, decreasing = TRUE).
std::vector<usize> order_desc(std::span<const f64> p) {
  std::vector<usize> o(p.size());
  std::iota(o.begin(), o.end(), usize{0});
  std::stable_sort(o.begin(), o.end(), [&](usize a, usize b) { return p[a] > p[b]; });
  return o;
}

// Shared step-up form: adj_(i) = min(1, cummin_{j>=i}(scale * n / j * p_(j))).
std::vector<f64> step_up(std::span<const f64> p, f64 scale) {
  const usize n = p.size();
  std::vector<f64> out(n);
  if (n == 0U) {
    return out;
  }
  const std::vector<usize> o = order_desc(p);
  const f64 nf = static_cast<f64>(n);
  f64 running = std::numeric_limits<f64>::infinity();
  for (usize r = 0; r < n; ++r) {
    const f64 rank = static_cast<f64>(n - r); // i = n, n-1, ..., 1
    const f64 v = scale * nf / rank * p[o[r]];
    running = std::min(running, v);
    out[o[r]] = std::min(1.0, running);
  }
  return out;
}

std::vector<bool> mask_le(const std::vector<f64> &adj, f64 level) {
  std::vector<bool> out(adj.size());
  for (usize i = 0; i < adj.size(); ++i) {
    out[i] = adj[i] <= level;
  }
  return out;
}

// ---------------------------------------------------------------------------
//  Counter-based RNG.
// ---------------------------------------------------------------------------
constexpr u64 kGolden = 0x9e3779b97f4a7c15ULL;

[[nodiscard]] constexpr u64 splitmix64(u64 x) noexcept {
  u64 z = x + kGolden;
  z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31U);
}

// Uniform in [0, 1) with 53 random bits.
[[nodiscard]] constexpr f64 to_unit(u64 x) noexcept {
  return static_cast<f64>(x >> 11U) * 0x1.0p-53;
}

// ---------------------------------------------------------------------------
//  Input validation shared by romano_wolf and hansen_spa.
// ---------------------------------------------------------------------------
atx::core::Status validate(const PnlMatrix &m, const BootstrapCfg &cfg) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  if (m.n_series == 0U || m.n_periods < 2U) {
    return Err(ErrorCode::InvalidArgument, "multiple_testing: need K >= 1 and T >= 2");
  }
  if (m.n_periods >= std::numeric_limits<u32>::max()) {
    return Err(ErrorCode::InvalidArgument, "multiple_testing: T too large");
  }
  if (m.data.size() / m.n_series != m.n_periods || m.data.size() % m.n_series != 0U) {
    return Err(ErrorCode::InvalidArgument, "multiple_testing: data.size() != K * T");
  }
  if (cfg.n_boot == 0U || !(cfg.mean_block >= 1.0) || !std::isfinite(cfg.mean_block)) {
    return Err(ErrorCode::InvalidArgument,
               "multiple_testing: need n_boot >= 1 and finite mean_block >= 1");
  }
  for (const f64 v : m.data) {
    if (!std::isfinite(v)) {
      return Err(ErrorCode::InvalidArgument, "multiple_testing: non-finite pnl");
    }
  }
  return atx::core::Ok();
}

// ---------------------------------------------------------------------------
//  Prefix-sum panel: pre[k*(T+1) + t] = Σ_{s<t} x_k,s. A circular block sum is
//  then one or two prefix differences.
// ---------------------------------------------------------------------------
struct PrefixPanel {
  usize k{};
  usize t{};
  std::vector<f64> pre;

  [[nodiscard]] const f64 *row(usize i) const noexcept { return pre.data() + i * (t + 1U); }
};

// Build prefixes of x_k − bench (bench empty == no benchmark).
PrefixPanel build_prefix(const PnlMatrix &m, std::span<const f64> bench) {
  PrefixPanel p;
  p.k = m.n_series;
  p.t = m.n_periods;
  p.pre.assign(p.k * (p.t + 1U), 0.0);
  for (usize i = 0; i < p.k; ++i) {
    const std::span<const f64> x = m.row(i);
    f64 *dst = p.pre.data() + i * (p.t + 1U);
    f64 acc = 0.0;
    for (usize s = 0; s < p.t; ++s) {
      acc += bench.empty() ? x[s] : x[s] - bench[s];
      dst[s + 1U] = acc;
    }
  }
  return p;
}

[[nodiscard]] inline f64 block_sum(const f64 *pre, usize t, const BootstrapBlock &blk) noexcept {
  const usize s = blk.start;
  const usize e = s + blk.len;
  if (e <= t) {
    return pre[e] - pre[s];
  }
  // Wraps: [s, T) then [0, e - T).
  return (pre[t] - pre[s]) + pre[e - t];
}

// Bootstrap means of every series for every replicate of one chunk.
// means[r*K + k] = mean of series k under replicate b0 + r. Loop order is
// series-outer / replicate-inner so a series' prefix row (8·(T+1) bytes) stays
// in L1/L2 across the chunk's replicates instead of streaming the whole K×T
// prefix panel from memory once per replicate. Each (replicate, series) sum
// runs over that replicate's blocks in order, so the value does not depend on
// chunking.
struct ChunkScratch {
  std::vector<std::vector<BootstrapBlock>> blocks;
  std::vector<f64> means;
};

void chunk_means(const PrefixPanel &p, const BootstrapCfg &cfg, usize b0, usize b1,
                 ChunkScratch &sc) {
  const usize nb = b1 - b0;
  sc.blocks.resize(nb);
  for (usize r = 0; r < nb; ++r) {
    stationary_bootstrap_blocks(p.t, cfg, b0 + r, sc.blocks[r]);
  }
  sc.means.assign(nb * p.k, 0.0);
  const f64 inv_t = 1.0 / static_cast<f64>(p.t);
  for (usize i = 0; i < p.k; ++i) {
    const f64 *pre = p.row(i);
    for (usize r = 0; r < nb; ++r) {
      f64 acc = 0.0;
      for (const BootstrapBlock &blk : sc.blocks[r]) {
        acc += block_sum(pre, p.t, blk);
      }
      sc.means[r * p.k + i] = acc * inv_t;
    }
  }
}

// ---------------------------------------------------------------------------
//  Deterministic chunked parallel-for over replicates. Chunk c covers
//  replicates [c*kChunk, min(B, (c+1)*kChunk)); `fn(c, b0, b1)` writes only
//  chunk-c state, so the caller reduces chunk results in chunk order and the
//  outcome never depends on the thread count.
// ---------------------------------------------------------------------------
constexpr usize kChunk = 16U;

template <class Fn> void for_each_chunk(usize n_boot, usize threads, Fn &&fn) {
  const usize n_chunks = (n_boot + kChunk - 1U) / kChunk;
  const usize workers = std::max<usize>(1U, std::min(threads, n_chunks));
  auto run = [&](usize w) {
    for (usize c = w; c < n_chunks; c += workers) {
      const usize b0 = c * kChunk;
      const usize b1 = std::min(n_boot, b0 + kChunk);
      fn(c, b0, b1);
    }
  };
  if (workers == 1U) {
    run(0U);
    return;
  }
  // Each worker touches a disjoint chunk set; joins happen before return, so
  // no state outlives the threads (lifetime argument for the by-ref capture).
  std::vector<std::thread> pool;
  pool.reserve(workers - 1U);
  for (usize w = 1U; w < workers; ++w) {
    pool.emplace_back(run, w);
  }
  run(0U);
  for (std::thread &th : pool) {
    th.join();
  }
}

[[nodiscard]] usize n_chunks_of(usize n_boot) noexcept { return (n_boot + kChunk - 1U) / kChunk; }

struct MeanSd {
  f64 mean{};
  f64 sd{};
};

MeanSd mean_sd_sample(std::span<const f64> x) {
  f64 mean = 0.0;
  for (const f64 v : x) {
    mean += v;
  }
  mean /= static_cast<f64>(x.size());
  f64 ss = 0.0;
  for (const f64 v : x) {
    ss += (v - mean) * (v - mean);
  }
  return MeanSd{mean, std::sqrt(ss / static_cast<f64>(x.size() - 1U))};
}

} // namespace

// ===========================================================================
//  p.adjust family.
// ===========================================================================
std::vector<f64> p_adjust_bh(std::span<const f64> p) { return step_up(p, 1.0); }

std::vector<f64> p_adjust_by(std::span<const f64> p) {
  f64 q = 0.0;
  for (usize i = 1; i <= p.size(); ++i) {
    q += 1.0 / static_cast<f64>(i);
  }
  return step_up(p, q);
}

std::vector<f64> p_adjust_holm(std::span<const f64> p) {
  const usize n = p.size();
  std::vector<f64> out(n);
  std::vector<usize> o(n);
  std::iota(o.begin(), o.end(), usize{0});
  std::stable_sort(o.begin(), o.end(), [&](usize a, usize b) { return p[a] < p[b]; });
  f64 running = 0.0;
  for (usize r = 0; r < n; ++r) {
    const f64 v = static_cast<f64>(n - r) * p[o[r]]; // (n - i + 1) * p_(i)
    running = std::max(running, v);
    out[o[r]] = std::min(1.0, running);
  }
  return out;
}

std::vector<f64> p_adjust_bonferroni(std::span<const f64> p) {
  std::vector<f64> out(p.size());
  const f64 n = static_cast<f64>(p.size());
  for (usize i = 0; i < p.size(); ++i) {
    out[i] = std::min(1.0, n * p[i]);
  }
  return out;
}

std::vector<bool> benjamini_hochberg(std::span<const f64> p, f64 q) {
  return mask_le(p_adjust_bh(p), q);
}

std::vector<bool> benjamini_yekutieli(std::span<const f64> p, f64 q) {
  return mask_le(p_adjust_by(p), q);
}

std::vector<bool> holm(std::span<const f64> p, f64 alpha) {
  return mask_le(p_adjust_holm(p), alpha);
}

// ===========================================================================
//  Stationary bootstrap block generator.
// ===========================================================================
void stationary_bootstrap_blocks(usize T, const BootstrapCfg &cfg, usize b,
                                 std::vector<BootstrapBlock> &out) {
  out.clear();
  if (T == 0U) {
    return;
  }
  const u64 key = splitmix64(cfg.seed ^ splitmix64(static_cast<u64>(b) + 0x632be59bd9b4e019ULL));
  const f64 p_new = cfg.mean_block > 1.0 ? 1.0 / cfg.mean_block : 1.0;
  // log(1 - p) < 0 for p in (0, 1); unused when p_new == 1 (iid).
  const f64 log_q = p_new < 1.0 ? std::log1p(-p_new) : -1.0;
  const f64 tf = static_cast<f64>(T);
  usize pos = 0;
  u64 j = 0;
  // Bounded: every iteration advances pos by >= 1, so at most T iterations.
  while (pos < T) {
    const f64 u_start = to_unit(splitmix64(key + j));
    ++j;
    usize start = static_cast<usize>(u_start * tf);
    start = std::min(start, T - 1U);
    const usize remaining = T - pos;
    usize len = 1U;
    if (p_new < 1.0) {
      const f64 u_len = to_unit(splitmix64(key + j));
      ++j;
      // Geometric(p) on {1, 2, ...}: 1 + floor(log(1-u) / log(1-p)).
      const f64 extra = std::floor(std::log1p(-u_len) / log_q);
      len = extra >= static_cast<f64>(remaining - 1U) ? remaining
                                                        : 1U + static_cast<usize>(extra);
    }
    out.push_back(BootstrapBlock{static_cast<u32>(start), static_cast<u32>(len)});
    pos += len;
  }
}

// ===========================================================================
//  Romano-Wolf stepdown.
// ===========================================================================
atx::core::Result<RomanoWolfResult> romano_wolf(const PnlMatrix &pnl, const BootstrapCfg &cfg,
                                                f64 alpha) {
  ATX_TRY_VOID(validate(pnl, cfg));
  if (!(alpha > 0.0) || !(alpha < 1.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "romano_wolf: alpha must lie in (0, 1)");
  }
  const usize K = pnl.n_series;
  const usize T = pnl.n_periods;
  const f64 sqrt_t = std::sqrt(static_cast<f64>(T));

  std::vector<f64> mean(K);
  std::vector<f64> inv_sd(K); // 0 for a flat series: its statistic is pinned at 0
  RomanoWolfResult res;
  res.alpha = alpha;
  res.t_stat.resize(K);
  for (usize k = 0; k < K; ++k) {
    const MeanSd ms = mean_sd_sample(pnl.row(k));
    mean[k] = ms.mean;
    inv_sd[k] = ms.sd > 0.0 ? 1.0 / ms.sd : 0.0;
    res.t_stat[k] = sqrt_t * ms.mean * inv_sd[k];
  }

  // Descending-t order (stable on index for ties).
  std::vector<usize> order(K);
  std::iota(order.begin(), order.end(), usize{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](usize a, usize b) { return res.t_stat[a] > res.t_stat[b]; });

  const PrefixPanel pre = build_prefix(pnl, {});
  const usize n_chunks = n_chunks_of(cfg.n_boot);
  std::vector<u64> chunk_counts(n_chunks * K, 0U);

  for_each_chunk(cfg.n_boot, cfg.threads, [&](usize c, usize b0, usize b1) {
    ChunkScratch scr;
    chunk_means(pre, cfg, b0, b1, scr);
    std::vector<f64> t_star(K);
    u64 *counts = chunk_counts.data() + c * K;
    for (usize b = b0; b < b1; ++b) {
      const f64 *m_star = scr.means.data() + (b - b0) * K;
      for (usize k = 0; k < K; ++k) {
        t_star[k] = sqrt_t * (m_star[k] - mean[k]) * inv_sd[k];
      }
      // Suffix max over the descending-t order: position j tests hypothesis
      // order[j] against the max over the still-active set {order[j..K)}.
      f64 running = -std::numeric_limits<f64>::infinity();
      for (usize jj = K; jj-- > 0U;) {
        running = std::max(running, t_star[order[jj]]);
        if (running >= res.t_stat[order[jj]]) {
          ++counts[jj];
        }
      }
    }
  });

  std::vector<u64> counts(K, 0U);
  for (usize c = 0; c < n_chunks; ++c) {
    for (usize j = 0; j < K; ++j) {
      counts[j] += chunk_counts[c * K + j];
    }
  }
  res.p_adjusted.assign(K, 1.0);
  res.reject.assign(K, false);
  const f64 denom = 1.0 + static_cast<f64>(cfg.n_boot);
  f64 running = 0.0;
  for (usize j = 0; j < K; ++j) {
    const f64 p_raw = (1.0 + static_cast<f64>(counts[j])) / denom;
    running = std::max(running, p_raw); // stepdown monotonicity
    res.p_adjusted[order[j]] = running;
    res.reject[order[j]] = running <= alpha;
  }
  return atx::core::Ok(std::move(res));
}

// ===========================================================================
//  Hansen SPA + White Reality Check.
// ===========================================================================
atx::core::Result<SpaResult> hansen_spa(const PnlMatrix &candidates,
                                        std::span<const f64> benchmark, const BootstrapCfg &cfg) {
  ATX_TRY_VOID(validate(candidates, cfg));
  const usize K = candidates.n_series;
  const usize T = candidates.n_periods;
  if (benchmark.size() != T) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "hansen_spa: benchmark length != T");
  }
  for (const f64 v : benchmark) {
    if (!std::isfinite(v)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "hansen_spa: non-finite benchmark");
    }
  }
  const f64 tf = static_cast<f64>(T);
  const f64 sqrt_t = std::sqrt(tf);
  const PrefixPanel pre = build_prefix(candidates, benchmark);
  std::vector<f64> dbar(K);
  for (usize k = 0; k < K; ++k) {
    dbar[k] = pre.row(k)[T] / tf;
  }

  // ---- pass 1: ω̂_k = bootstrap std of √T·(d̄*_k − d̄_k) ----------------------
  const usize n_chunks = n_chunks_of(cfg.n_boot);
  std::vector<f64> chunk_s1(n_chunks * K, 0.0);
  std::vector<f64> chunk_s2(n_chunks * K, 0.0);
  for_each_chunk(cfg.n_boot, cfg.threads, [&](usize c, usize b0, usize b1) {
    ChunkScratch scr;
    chunk_means(pre, cfg, b0, b1, scr);
    f64 *s1 = chunk_s1.data() + c * K;
    f64 *s2 = chunk_s2.data() + c * K;
    for (usize b = b0; b < b1; ++b) {
      const f64 *m_star = scr.means.data() + (b - b0) * K;
      for (usize k = 0; k < K; ++k) {
        const f64 z = sqrt_t * (m_star[k] - dbar[k]);
        s1[k] += z;
        s2[k] += z * z;
      }
    }
  });
  const f64 bf = static_cast<f64>(cfg.n_boot);
  std::vector<f64> inv_omega(K, 0.0);
  for (usize k = 0; k < K; ++k) {
    f64 s1 = 0.0;
    f64 s2 = 0.0;
    for (usize c = 0; c < n_chunks; ++c) {
      s1 += chunk_s1[c * K + k];
      s2 += chunk_s2[c * K + k];
    }
    const f64 mu = s1 / bf;
    const f64 var = s2 / bf - mu * mu;
    // A constant differential has zero bootstrap variance: exclude it.
    const f64 omega = var > 0.0 ? std::sqrt(var) : 0.0;
    inv_omega[k] = omega > 1e-300 ? 1.0 / omega : 0.0;
  }

  // ---- observed statistics + recentring ------------------------------------
  SpaResult res;
  res.statistic = 0.0;
  res.rc_statistic = -std::numeric_limits<f64>::infinity();
  f64 best = -std::numeric_limits<f64>::infinity();
  const f64 threshold = -std::sqrt(2.0 * std::log(std::log(std::max(tf, 3.0))));
  std::vector<f64> g_l(K);
  std::vector<f64> g_c(K);
  for (usize k = 0; k < K; ++k) {
    const f64 tk = sqrt_t * dbar[k] * inv_omega[k];
    if (inv_omega[k] > 0.0 && tk > best) {
      best = tk;
      res.best_index = k;
    }
    res.statistic = std::max(res.statistic, inv_omega[k] > 0.0 ? tk : 0.0);
    res.rc_statistic = std::max(res.rc_statistic, sqrt_t * dbar[k]);
    g_l[k] = std::max(dbar[k], 0.0);
    g_c[k] = tk >= threshold ? dbar[k] : 0.0;
  }

  // ---- pass 2: bootstrap null distributions --------------------------------
  // Per chunk: exceedance counts for {lower, consistent, upper, rc}.
  std::vector<u64> chunk_cnt(n_chunks * 4U, 0U);
  for_each_chunk(cfg.n_boot, cfg.threads, [&](usize c, usize b0, usize b1) {
    ChunkScratch scr;
    chunk_means(pre, cfg, b0, b1, scr);
    u64 *cnt = chunk_cnt.data() + c * 4U;
    for (usize b = b0; b < b1; ++b) {
      const f64 *m_star = scr.means.data() + (b - b0) * K;
      f64 tl = 0.0;
      f64 tc = 0.0;
      f64 tu = 0.0;
      f64 v = -std::numeric_limits<f64>::infinity();
      for (usize k = 0; k < K; ++k) {
        v = std::max(v, sqrt_t * (m_star[k] - dbar[k]));
        if (inv_omega[k] > 0.0) {
          const f64 s = sqrt_t * inv_omega[k];
          tl = std::max(tl, s * (m_star[k] - g_l[k]));
          tc = std::max(tc, s * (m_star[k] - g_c[k]));
          tu = std::max(tu, s * (m_star[k] - dbar[k]));
        }
      }
      cnt[0] += tl > res.statistic ? 1U : 0U;
      cnt[1] += tc > res.statistic ? 1U : 0U;
      cnt[2] += tu > res.statistic ? 1U : 0U;
      cnt[3] += v >= res.rc_statistic ? 1U : 0U;
    }
  });
  u64 tot[4] = {0U, 0U, 0U, 0U};
  for (usize c = 0; c < n_chunks; ++c) {
    for (usize i = 0; i < 4U; ++i) {
      tot[i] += chunk_cnt[c * 4U + i];
    }
  }
  res.p_lower = static_cast<f64>(tot[0]) / bf;
  res.p_consistent = static_cast<f64>(tot[1]) / bf;
  res.p_upper = static_cast<f64>(tot[2]) / bf;
  // The SPA statistic is max(0, ·): a statistic of 0 (no candidate beats the
  // benchmark, or every candidate has zero bootstrap variance and is excluded)
  // can never be evidence against the no-superiority null. The strict '>'
  // count would give p = 0 there (e.g. all-constant differentials) — i.e.
  // reject at any level — so pin p = 1, which is P(T* >= 0).
  if (!(res.statistic > 0.0)) {
    res.p_lower = 1.0;
    res.p_consistent = 1.0;
    res.p_upper = 1.0;
  }
  res.rc_pvalue = static_cast<f64>(tot[3]) / bf;
  return atx::core::Ok(res);
}

} // namespace atx::engine::eval
