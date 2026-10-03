#include "atx/engine/research/eval/onc_house.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "atx/engine/eval/deflated_sharpe_house.hpp"
#include "atx/engine/research/numpy_reduce.hpp"

namespace atx::engine::research::eval {
namespace {

constexpr usize kUnset = std::numeric_limits<usize>::max();
constexpr f64 kInf = std::numeric_limits<f64>::infinity();

using Members = std::vector<usize>;
using Clusters = std::vector<std::vector<usize>>;

[[nodiscard]] core::Error invalid(const std::string &message) {
  return core::Error(core::ErrorCode::InvalidArgument, "house onc: " + message);
}

// x_ij = sqrt(max((1 - rho_ij) / 2, 0)), NaN read as 0 first (backtest_integrity.corr_distance).
[[nodiscard]] std::vector<f64> corr_distance(std::span<const f64> c) {
  std::vector<f64> x(c.size());
  for (usize i = 0; i < c.size(); ++i) {
    const f64 rho = std::isnan(c[i]) ? 0.0 : c[i];
    x[i] = std::sqrt(std::max((1.0 - rho) / 2.0, 0.0));
  }
  return x;
}

[[nodiscard]] std::span<const f64> row(const std::vector<f64> &m, usize n, usize i) {
  return std::span<const f64>(m).subspan(i * n, n);
}

// np.sum((a - b) ** 2): the squares summed in numpy's pairwise order.
[[nodiscard]] f64 sq_dist(std::span<const f64> a, std::span<const f64> b,
                          std::vector<f64> &scratch) {
  scratch.resize(a.size());
  for (usize k = 0; k < a.size(); ++k) {
    const f64 d = a[k] - b[k];
    scratch[k] = d * d;
  }
  return numpy::sum(scratch);
}

[[nodiscard]] usize index_of(const Members &members, usize id) {
  return static_cast<usize>(std::find(members.begin(), members.end(), id) - members.begin());
}

// backtest_integrity.kmeans on the n rows of x (each of dimension n).
[[nodiscard]] core::Result<std::vector<usize>> kmeans(const std::vector<f64> &x, usize n, usize k,
                                                      KmeansDraws &draws) {
  std::vector<f64> scratch;
  std::vector<std::vector<f64>> centers(k);
  ATX_TRY(const usize first, draws.integers(n));
  if (first >= n) {
    return core::Err(invalid("a k-means++ draw is out of range"));
  }
  const auto first_row = row(x, n, first);
  centers[0].assign(first_row.begin(), first_row.end());
  std::vector<f64> d2(n);
  for (usize i = 0; i < n; ++i) {
    d2[i] = sq_dist(row(x, n, i), centers[0], scratch);
  }
  std::vector<f64> p(n);
  for (usize j = 1; j < k; ++j) {
    const f64 total = numpy::sum(d2);
    usize pick = 0;
    if (total <= 0.0) {
      ATX_TRY(pick, draws.integers(n));
    } else {
      for (usize i = 0; i < n; ++i) {
        p[i] = d2[i] / total;
      }
      ATX_TRY(pick, draws.choice(p));
    }
    if (pick >= n) {
      return core::Err(invalid("a k-means++ draw is out of range"));
    }
    const auto picked = row(x, n, pick);
    centers[j].assign(picked.begin(), picked.end());
    for (usize i = 0; i < n; ++i) {
      d2[i] = std::min(d2[i], sq_dist(row(x, n, i), centers[j], scratch));
    }
  }
  std::vector<usize> labels(n, kUnset);
  std::vector<usize> fresh(n);
  std::vector<f64> dist(n * k);
  for (usize iter = 0; iter < kKmeansMaxIter; ++iter) {
    for (usize i = 0; i < n; ++i) {
      usize best = 0;
      for (usize j = 0; j < k; ++j) {
        dist[i * k + j] = sq_dist(row(x, n, i), centers[j], scratch);
        if (dist[i * k + j] < dist[i * k + best]) {
          best = j;
        }
      }
      fresh[i] = best;
    }
    for (usize j = 0; j < k; ++j) {
      if (std::find(fresh.begin(), fresh.end(), j) != fresh.end()) {
        continue;
      }
      usize far = 0;
      for (usize i = 1; i < n; ++i) {
        if (dist[i * k + fresh[i]] > dist[far * k + fresh[far]]) {
          far = i;
        }
      }
      fresh[far] = j;
    }
    if (fresh == labels) {
      break;
    }
    labels = fresh;
    for (usize j = 0; j < k; ++j) {
      std::vector<f64> sums(n, 0.0);
      usize count = 0;
      for (usize i = 0; i < n; ++i) {
        if (labels[i] != j) {
          continue;
        }
        ++count;
        for (usize c = 0; c < n; ++c) {
          sums[c] += x[i * n + c];
        }
      }
      if (count == 0U) {
        continue;
      }
      for (usize c = 0; c < n; ++c) {
        sums[c] /= static_cast<f64>(count);
      }
      centers[j] = std::move(sums);
    }
  }
  return core::Ok(std::move(labels));
}

// backtest_integrity.silhouette_samples (Gram-form distances).
[[nodiscard]] std::vector<f64> silhouettes(const std::vector<f64> &x, usize n,
                                           const std::vector<usize> &labels) {
  std::vector<f64> scratch(n);
  std::vector<f64> g(n);
  for (usize i = 0; i < n; ++i) {
    const auto r = row(x, n, i);
    for (usize c = 0; c < n; ++c) {
      scratch[c] = r[c] * r[c];
    }
    g[i] = numpy::sum(scratch);
  }
  std::vector<f64> d(n * n, 0.0);
  for (usize i = 0; i < n; ++i) {
    for (usize j = 0; j < n; ++j) {
      if (i == j) {
        continue;
      }
      const auto a = row(x, n, i);
      const auto b = row(x, n, j);
      for (usize c = 0; c < n; ++c) {
        scratch[c] = a[c] * b[c];
      }
      const f64 d2 = std::max((g[i] + g[j]) - 2.0 * numpy::sum(scratch), 0.0);
      d[i * n + j] = std::sqrt(d2);
    }
  }
  const std::set<usize> uniq(labels.begin(), labels.end());
  std::vector<f64> out(n, 0.0);
  std::vector<f64> picked;
  for (usize i = 0; i < n; ++i) {
    picked.clear();
    for (usize j = 0; j < n; ++j) {
      if (labels[j] == labels[i]) {
        picked.push_back(d[i * n + j]);
      }
    }
    if (picked.size() <= 1U || uniq.size() < 2U) {
      continue;
    }
    const f64 a = numpy::sum(picked) / static_cast<f64>(picked.size() - 1U);
    f64 b = kInf;
    for (const usize u : uniq) {
      if (u == labels[i]) {
        continue;
      }
      picked.clear();
      for (usize j = 0; j < n; ++j) {
        if (labels[j] == u) {
          picked.push_back(d[i * n + j]);
        }
      }
      b = std::min(b, numpy::mean(picked));
    }
    const f64 m = std::max(a, b);
    out[i] = m > 0.0 ? (b - a) / m : 0.0;
  }
  return out;
}

// backtest_integrity._quality: mean / std (population) of the silhouettes.
[[nodiscard]] f64 quality(std::span<const f64> s) {
  const f64 sd = std::sqrt(numpy::var(s, 0U));
  const f64 mu = numpy::mean(s);
  if (sd > 0.0) {
    return mu / sd;
  }
  return mu > 0.0 ? kInf : (mu < 0.0 ? -kInf : 0.0);
}

// _groups: members per label, labels ascending.
[[nodiscard]] Clusters groups(const std::vector<usize> &labels, const Members &members) {
  const std::set<usize> uniq(labels.begin(), labels.end());
  Clusters out;
  for (const usize u : uniq) {
    std::vector<usize> cluster;
    for (usize i = 0; i < labels.size(); ++i) {
      if (labels[i] == u) {
        cluster.push_back(members[i]);
      }
    }
    out.push_back(std::move(cluster));
  }
  return out;
}

// _cluster_tstats: mean / std of each cluster's silhouettes (NaN for zero dispersion).
[[nodiscard]] std::vector<f64> cluster_tstats(const std::vector<f64> &silh, const Members &members,
                                              const Clusters &clusters) {
  std::vector<f64> out;
  std::vector<f64> v;
  for (const auto &cluster : clusters) {
    v.clear();
    for (const usize m : cluster) {
      v.push_back(silh[index_of(members, m)]);
    }
    const f64 sd = std::sqrt(numpy::var(v, 0U));
    out.push_back(sd > 0.0 ? numpy::mean(v) / sd : std::numeric_limits<f64>::quiet_NaN());
  }
  return out;
}

[[nodiscard]] std::vector<f64> finite_values(const std::vector<f64> &values) {
  std::vector<f64> out;
  for (const f64 v : values) {
    if (std::isfinite(v)) {
      out.push_back(v);
    }
  }
  return out;
}

// One level of backtest_integrity.onc (members: the trial ids of this level's rows). The
// recursion re-clusters a strict subset of the members, so its depth is below n.
[[nodiscard]] core::Result<OncPartition> onc_level(const std::vector<f64> &c, const Members &members,
                                                   std::optional<usize> max_k, usize n_init,
                                                   KmeansDraws &draws) {
  const usize n = members.size();
  OncPartition out;
  if (n < 3U) {
    for (const usize m : members) {
      out.clusters.push_back({m});
    }
    out.silhouettes.assign(n, 0.0);
    return core::Ok(std::move(out));
  }
  const usize mk = max_k ? std::min(*max_k, n - 1U) : n - 1U;
  if (mk < 2U || n_init == 0U) {
    return core::Err(invalid("needs max_k >= 2 and n_init >= 1"));
  }
  const std::vector<f64> x = corr_distance(c);
  std::vector<usize> best_labels;
  std::vector<f64> best_s;
  f64 best_q = 0.0;
  bool have_best = false;
  for (usize init = 0; init < n_init; ++init) {
    for (usize k = 2; k <= mk; ++k) {
      ATX_TRY(std::vector<usize> labels, kmeans(x, n, k, draws));
      std::vector<f64> s = silhouettes(x, n, labels);
      const f64 q = quality(s);
      if (!have_best || q > best_q) {
        best_labels = std::move(labels);
        best_s = std::move(s);
        best_q = q;
        have_best = true;
      }
    }
  }
  out.clusters = groups(best_labels, members);
  out.silhouettes = best_s;
  const std::vector<f64> tstats = cluster_tstats(best_s, members, out.clusters);
  const std::vector<f64> finite = finite_values(tstats);
  if (finite.empty()) {
    return core::Ok(std::move(out));
  }
  f64 finite_sum = 0.0;
  for (const f64 t : finite) {
    finite_sum += t;
  }
  const f64 mean_t = finite_sum / static_cast<f64>(finite.size());
  std::vector<usize> redo;
  Members keys;
  for (usize i = 0; i < tstats.size(); ++i) {
    if (std::isfinite(tstats[i]) && tstats[i] < mean_t) {
      redo.push_back(i);
      keys.insert(keys.end(), out.clusters[i].begin(), out.clusters[i].end());
    }
  }
  if (redo.size() <= 1U || keys.size() < 3U) {
    return core::Ok(std::move(out));
  }
  const usize nk = keys.size();
  std::vector<usize> pos(nk);
  for (usize i = 0; i < nk; ++i) {
    pos[i] = index_of(members, keys[i]);
  }
  std::vector<f64> sub_c(nk * nk);
  for (usize i = 0; i < nk; ++i) {
    for (usize j = 0; j < nk; ++j) {
      sub_c[i * nk + j] = c[pos[i] * n + pos[j]];
    }
  }
  ATX_TRY(OncPartition sub, onc_level(sub_c, keys, std::min(mk, nk - 1U), n_init, draws));
  Clusters fresh;
  for (usize i = 0; i < out.clusters.size(); ++i) {
    if (std::find(redo.begin(), redo.end(), i) == redo.end()) {
      fresh.push_back(out.clusters[i]);
    }
  }
  fresh.insert(fresh.end(), sub.clusters.begin(), sub.clusters.end());
  std::vector<usize> fresh_labels(n, 0U);
  for (usize j = 0; j < fresh.size(); ++j) {
    for (const usize m : fresh[j]) {
      fresh_labels[index_of(members, m)] = j;
    }
  }
  std::vector<f64> fresh_s = silhouettes(x, n, fresh_labels);
  const std::vector<f64> fresh_t = finite_values(cluster_tstats(fresh_s, members, fresh));
  std::vector<f64> redo_t;
  for (const usize i : redo) {
    redo_t.push_back(tstats[i]);
  }
  const f64 old_mean = numpy::mean(redo_t);
  if (fresh_t.empty() || numpy::mean(fresh_t) <= old_mean) {
    return core::Ok(std::move(out));
  }
  out.clusters = std::move(fresh);
  out.silhouettes = std::move(fresh_s);
  return core::Ok(std::move(out));
}

// ivp_series: the inverse-variance (ddof 1) mix of the given columns, equal weights when every
// variance is 0; column reductions sequential (numpy axis 0), the weights' sum pairwise.
[[nodiscard]] std::vector<f64> ivp_series(std::span<const f64> matrix, usize sessions,
                                          usize n_trials, const std::vector<usize> &columns) {
  const usize m = columns.size();
  std::vector<f64> v(m, 0.0);
  if (sessions > 1U) {
    for (usize c = 0; c < m; ++c) {
      f64 total = 0.0;
      for (usize t = 0; t < sessions; ++t) {
        total += matrix[t * n_trials + columns[c]];
      }
      const f64 mean = total / static_cast<f64>(sessions);
      f64 sq = 0.0;
      for (usize t = 0; t < sessions; ++t) {
        const f64 d = matrix[t * n_trials + columns[c]] - mean;
        sq += d * d;
      }
      v[c] = sq / static_cast<f64>(sessions - 1U);
    }
  }
  std::vector<f64> w(m);
  for (usize c = 0; c < m; ++c) {
    w[c] = v[c] > 0.0 ? 1.0 / v[c] : 0.0;
  }
  const f64 wsum = numpy::sum(w);
  for (usize c = 0; c < m; ++c) {
    w[c] = wsum > 0.0 ? w[c] / wsum : 1.0 / static_cast<f64>(m);
  }
  std::vector<f64> out(sessions);
  for (usize t = 0; t < sessions; ++t) {
    f64 acc = 0.0;
    for (usize c = 0; c < m; ++c) {
      acc += matrix[t * n_trials + columns[c]] * w[c];
    }
    out[t] = acc;
  }
  return out;
}

} // namespace

core::Result<usize> NumpyKmeansDraws::integers(usize n) {
  if (n == 0U || n > 0xFFFFFFFFULL) {
    return core::Err(invalid("integers(n) needs 1 <= n < 2^32"));
  }
  return core::Ok(static_cast<usize>(generator_.integers(n)));
}

core::Result<usize> NumpyKmeansDraws::choice(std::span<const f64> p) {
  if (p.empty()) {
    return core::Err(invalid("choice needs a probability"));
  }
  return core::Ok(generator_.choice(p));
}

core::Result<OncPartition> onc_house_v1(std::span<const f64> corr, usize n, usize n_init,
                                        KmeansDraws &draws) {
  if (corr.size() != n * n) {
    return core::Err(invalid("the correlation is not n x n"));
  }
  Members members(n);
  for (usize i = 0; i < n; ++i) {
    members[i] = i;
  }
  return onc_level(std::vector<f64>(corr.begin(), corr.end()), members, std::nullopt, n_init,
                   draws);
}

std::vector<f64> trial_correlation(std::span<const f64> matrix, usize sessions, usize n_trials) {
  if (n_trials == 1U) {
    return {1.0};
  }
  std::vector<f64> means(n_trials);
  std::vector<f64> column(sessions);
  for (usize k = 0; k < n_trials; ++k) {
    for (usize t = 0; t < sessions; ++t) {
      column[t] = matrix[t * n_trials + k];
    }
    means[k] = numpy::mean(column);
  }
  const f64 inv_fact = 1.0 / static_cast<f64>(sessions - 1U);
  std::vector<f64> cov(n_trials * n_trials);
  for (usize i = 0; i < n_trials; ++i) {
    for (usize j = 0; j < n_trials; ++j) {
      f64 acc = 0.0;
      for (usize t = 0; t < sessions; ++t) {
        acc += (matrix[t * n_trials + i] - means[i]) * (matrix[t * n_trials + j] - means[j]);
      }
      cov[i * n_trials + j] = acc * inv_fact;
    }
  }
  std::vector<f64> out(n_trials * n_trials);
  for (usize i = 0; i < n_trials; ++i) {
    for (usize j = 0; j < n_trials; ++j) {
      f64 r = cov[i * n_trials + j] / std::sqrt(cov[i * n_trials + i]) /
              std::sqrt(cov[j * n_trials + j]);
      if (!std::isnan(r)) {
        r = std::clamp(r, -1.0, 1.0);
      }
      out[i * n_trials + j] = i == j ? 1.0 : (std::isnan(r) ? 0.0 : r);
    }
  }
  return out;
}

core::Result<EffectiveTrials> effective_trials_with(std::span<const f64> matrix, usize sessions,
                                                    usize n_trials, usize n_init, u64 seed,
                                                    KmeansDraws &draws) {
  if (sessions < 3U || n_trials == 0U || matrix.size() != sessions * n_trials) {
    return core::Err(invalid("effective_trials needs a (T >= 3, N) matrix and N names"));
  }
  const std::vector<f64> corr = trial_correlation(matrix, sessions, n_trials);
  ATX_TRY(OncPartition part, onc_house_v1(corr, n_trials, n_init, draws));
  EffectiveTrials out;
  out.n_trials = n_trials;
  out.n_eff = part.clusters.size();
  out.sessions = sessions;
  out.n_init = n_init;
  out.seed = seed;
  std::vector<f64> defined;
  for (const auto &cluster : part.clusters) {
    const HouseMoments m = house_moments(ivp_series(matrix, sessions, n_trials, cluster));
    out.cluster_sr_daily.push_back(m.sr_daily);
    if (m.sr_daily) {
      defined.push_back(*m.sr_daily);
    }
  }
  if (defined.size() >= 2U) {
    out.variance_sr = numpy::var(defined, 1U);
  }
  const f64 pairs = static_cast<f64>(std::max<usize>(n_trials * (n_trials - 1U), 1U));
  out.mean_offdiag_corr = (numpy::sum(corr) - static_cast<f64>(n_trials)) / pairs;
  out.clusters = std::move(part.clusters);
  return core::Ok(std::move(out));
}

core::Result<EffectiveTrials> effective_trials(std::span<const f64> matrix, usize sessions,
                                               usize n_trials, usize n_init, u64 seed) {
  NumpyKmeansDraws draws(seed);
  return effective_trials_with(matrix, sessions, n_trials, n_init, seed, draws);
}

EffectiveNDsr effective_n_dsr(const HouseMoments &cell, const EffectiveTrials &eff) {
  EffectiveNDsr row;
  row.n_eff = eff.n_eff;
  row.n_trials = eff.n_trials;
  row.variance_sr = eff.variance_sr;
  row.variance_source = "sample variance of " + std::to_string(eff.n_eff) +
                        " ONC cluster IVP-series per-session SRs (" +
                        std::to_string(eff.n_trials) + " trial series, " +
                        std::to_string(eff.sessions) + " common sessions)";
  if (!cell.defined() || !eff.variance_sr || eff.n_eff < 2U || cell.sessions < 2U) {
    row.variance_source += "; undefined (N_eff < 2 or no defined SR)";
    return row;
  }
  const atx::engine::eval::HouseDsr d = atx::engine::eval::house_dsr(
      *cell.sr_daily, cell.sessions, *cell.skew, *cell.kurtosis, eff.n_eff, *eff.variance_sr);
  row.sr0_daily = d.sr0_daily;
  row.sr0_annual = d.sr0_daily * std::sqrt(kAnnualization);
  row.dsr = d.dsr;
  return row;
}

} // namespace atx::engine::research::eval
