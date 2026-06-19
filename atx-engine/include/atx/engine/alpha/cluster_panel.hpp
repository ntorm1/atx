#pragma once

// atx::engine::alpha — rolling return-structure clustering over a Panel (S11
// scaffold).
//
// build_cluster_panel() turns a returns Panel into a time series of cluster
// assignments: over a rolling window it estimates the instrument×instrument
// return correlation, cleans it with random-matrix theory
// (atx::core::linalg::rmt_clean), partitions the cleaned correlation into groups
// (atx::core::cluster::cluster), and records one labeling per recluster date. The
// result is the data plane S11's downstream alpha / risk units read to express
// "trade within / across return clusters" signals.
//
// Pipeline (Pattern B consumer): this engine header composes the two atx-core
// numerics edges — rmt_clean (S11-1) and cluster (S11-2) — over the engine's
// Panel input. The heavy lifting is in atx-core; this unit owns only the windowing,
// optional residualization, and the date→labeling bookkeeping.
//
// Determinism contract (S11; inherited by S11-3): no RNG on the result path; the
// correlation reduction is order-fixed (ascending date within the window,
// ascending instrument pair); eigen-based steps inside rmt_clean / cluster use
// the fixed ascending-eigenvalue order and first-nonzero-component-positive sign
// convention; and cluster labels arrive already canonicalized by ascending
// smallest-member index from atx::core::cluster. Point-in-time: each window ends
// at its recluster date and reads only dates ≤ that date — no look-ahead.
//
#include <algorithm> // std::min, std::clamp
#include <cmath>     // std::isnan, std::sqrt
#include <string>    // std::string
#include <utility>   // std::move
#include <vector>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/types.hpp" // f64, i64, usize

#include "atx/core/cluster/cluster.hpp" // cluster, ClusterConfig, Clustering
#include "atx/core/linalg/linalg.hpp"   // MatX
#include "atx/core/linalg/rmt_clean.hpp" // rmt_clean, CleanedCorr

#include "atx/engine/alpha/panel.hpp" // Panel (the read-only returns input)

namespace atx::engine::alpha {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// Configuration for a rolling cluster panel. Aggregate so callers brace-init only
// the fields they override.
struct ClusterPanelConfig {
  // Residualization applied to each instrument's return series before the
  // correlation is estimated, so clusters reflect IDIOSYNCRATIC co-movement
  // rather than shared market beta.
  enum class Residualize {
    None, // cluster on raw returns
    CAPM, // regress out a single market factor first (residual returns)
  };

  // Number of trailing dates in each correlation-estimation window. Must be > 0
  // and ≥ the instrument count for a non-degenerate sample correlation.
  int window = 0;

  // Recluster cadence in dates: a new labeling is computed every
  // `recluster_every` dates (1 == every date). Must be > 0.
  int recluster_every = 0;

  // Target cluster count handed to atx::core::cluster (1 ≤ k ≤ instruments). For a
  // window whose valid instrument count M is below k, the realized target is
  // min(k, M) so a thin window still partitions; n_labels records what landed.
  int k = 0;

  // Residualization mode (default: raw returns).
  Residualize residualize = Residualize::None;

  // Partitioning algorithm handed to atx::core::cluster for each window. Defaults
  // to Hierarchical (Ward linkage) — the interpretable, RNG-free, O(N²·log) edge
  // baseline. SpongeSym is exposed for callers that want signed-graph clustering
  // (negative correlations as repulsion), but it is deliberately NOT the default:
  // its deterministic k-means does N restarts (≈O(N³)) per window, which is costly
  // for a rolling panel. Both algorithms are deterministic, so either keeps the
  // truncation-invariance / two-runs-equal / pinned-digest guarantees.
  atx::core::cluster::Algo algo = atx::core::cluster::Algo::Hierarchical;

  // FIELD-SELECTION CONTRACT (S11-3): the name of the Panel field carrying each
  // instrument's per-date RETURN. build_cluster_panel resolves it once via
  // Panel::field_id and reads it with field_cross_section over the window; an
  // unknown name propagates Panel's Err(NotFound). The correlation (and the CAPM
  // market factor) are computed on this single field. Default "ret".
  std::string return_field = "ret";
};

// Result of building a rolling cluster panel: one cluster labeling per recluster
// date, in ascending-date order.
struct ClusterPanel {
  // One snapshot of the partition at a recluster date.
  struct Snapshot {
    // Index into the source Panel's date axis at which this labeling holds
    // (the last date of the window that produced it).
    DateIdx date{};

    // cluster_id[i] is the canonical cluster label of instrument i at `date`,
    // in [0, n_labels). Length == source Panel instruments(); an instrument
    // outside the point-in-time universe for the whole window carries the
    // sentinel kUnclustered.
    std::vector<int> cluster_id;

    // Distinct label count realized for this snapshot.
    i64 n_labels = 0;
  };

  // Sentinel cluster_id for an instrument that was not clusterable at a snapshot
  // (e.g. out-of-universe / all-NaN across the window).
  static constexpr int kUnclustered = -1;

  // Snapshots in ascending-date order, one per recluster date.
  std::vector<Snapshot> snapshots;

  // Instrument count the labels index into (== source Panel instruments()).
  atx::usize instruments = 0;
};

namespace detail {

// Neumaier (improved Kahan) compensated sum. The per-window correlation and the
// CAPM beta regression fold many products of mixed magnitude across the window; an
// order-fixed compensated sum keeps the low bits stable so the partition (and its
// pinned digest) is byte-identical across runs — the same determinism primitive
// rmt_clean.hpp / cluster.hpp use. atx-core exposes no shared helper, so this small
// local copy is reused here.
class NeumaierSum {
public:
  void add(atx::f64 x) noexcept {
    const atx::f64 t = sum_ + x;
    if (std::abs(sum_) >= std::abs(x)) {
      compensation_ += (sum_ - t) + x;
    } else {
      compensation_ += (x - t) + sum_;
    }
    sum_ = t;
  }
  [[nodiscard]] atx::f64 value() const noexcept { return sum_ + compensation_; }

private:
  atx::f64 sum_ = 0.0;
  atx::f64 compensation_ = 0.0;
};

} // namespace detail

// Build a rolling cluster panel from a returns Panel.
//
// `src` is the read-only returns Panel. The per-instrument RETURN series is read
// from the single field named cfg.return_field (FIELD-SELECTION CONTRACT, see
// ClusterPanelConfig); an unknown name returns Panel's Err(NotFound). `cfg` sets
// the window length, recluster cadence, target k, and residualization mode.
//
// Per rebalance date t — the last date of each window, stepping every
// cfg.recluster_every dates with the first valid t == cfg.window-1 — the builder
// uses ONLY the strict window [t-window+1, t] (no future bars):
//   1. Valid set = instruments in-universe for the WHOLE window AND non-NaN at
//      every date of it (mirrors the cs_ops valid-set semantics). M == |valid set|.
//   2. If cfg.residualize==CAPM, form an equal-weight cross-sectional market factor
//      over the valid set per date, estimate each instrument's window beta by least
//      squares, and replace its series with the residual r - beta*mkt.
//   3. Compute the MxM Pearson correlation over the window (order-fixed: ascending
//      date, ascending instrument pair, compensated summation) into a MatX.
//   4. rmt_clean(corr, q=M/window).
//   5. cluster(cleaned, { .algo = cfg.algo, .k = min(cfg.k, M) }).
//   6. Scatter the M canonical labels back into a full-length cluster_id, with
//      kUnclustered for every instrument outside the valid set.
//
// Point-in-time: because every window is strictly <= t and the valid set / reduction
// read only those dates, the snapshot at t is byte-identical with or without later
// bars (truncation-invariance — asserted in the ClusterPanel suite).
//
// Returns the per-date cluster snapshots, or Err on an invalid config (non-positive
// window / recluster_every / k, or k beyond the instrument count), an unknown return
// field, or a propagated atx-core failure (InvalidArgument / Internal).
[[nodiscard]] inline Result<ClusterPanel> build_cluster_panel(const Panel &src,
                                                              ClusterPanelConfig cfg) {
  const atx::usize instruments = src.instruments();
  const atx::usize dates = src.dates();

  if (cfg.window <= 0 || cfg.recluster_every <= 0 || cfg.k <= 0) {
    return Err(ErrorCode::InvalidArgument,
               "build_cluster_panel: window, recluster_every, and k must be positive");
  }
  if (static_cast<atx::usize>(cfg.k) > instruments) {
    return Err(ErrorCode::InvalidArgument,
               "build_cluster_panel: k must not exceed the instrument count");
  }

  // Resolve the return field once; an unknown name propagates Panel's Err(NotFound).
  const auto field = src.field_id(cfg.return_field);
  if (!field.has_value()) {
    return Err(field.error());
  }
  const FieldId ret_field = field.value();

  const atx::usize window = static_cast<atx::usize>(cfg.window);
  const atx::usize step = static_cast<atx::usize>(cfg.recluster_every);

  ClusterPanel panel;
  panel.instruments = instruments;

  // No reachable rebalance date (window longer than the available history) -> an
  // empty-snapshot panel, which is a valid (degenerate) result.
  if (window == 0 || dates < window) {
    return Ok(std::move(panel));
  }

  // Rebalance dates: t = window-1, window-1+step, ... while t < dates.
  for (atx::usize t = window - 1; t < dates; t += step) {
    const atx::usize w0 = t + 1 - window; // first date of the strict window [w0, t]

    // ---- 1. Valid set: in-universe for the WHOLE window AND finite throughout. --
    std::vector<atx::usize> valid; // ascending instrument indices
    valid.reserve(instruments);
    for (atx::usize i = 0; i < instruments; ++i) {
      bool ok = true;
      for (atx::usize d = w0; d <= t && ok; ++d) {
        if (!src.in_universe(d, i)) {
          ok = false;
          break;
        }
        const atx::f64 v = src.field_cross_section(ret_field, d)[i];
        if (std::isnan(v)) {
          ok = false;
        }
      }
      if (ok) {
        valid.push_back(i);
      }
    }
    const atx::usize m = valid.size();

    ClusterPanel::Snapshot snap;
    snap.date = t;
    snap.cluster_id.assign(instruments, ClusterPanel::kUnclustered);
    snap.n_labels = 0;

    // A window with fewer than two clusterable instruments cannot yield a
    // correlation partition; leave every cell kUnclustered.
    if (m < 2) {
      panel.snapshots.push_back(std::move(snap));
      continue;
    }

    // ---- 2. Materialize the window return matrix in valid-set order (rows == --
    //         window dates ascending, cols == valid instruments ascending), then
    //         optionally CAPM-residualize each column against the equal-weight
    //         market factor.
    // series[col] holds the column's window values (length == window), date-ordered.
    std::vector<std::vector<atx::f64>> series(m, std::vector<atx::f64>(window, 0.0));
    for (atx::usize d = 0; d < window; ++d) {
      const auto xs = src.field_cross_section(ret_field, w0 + d);
      for (atx::usize c = 0; c < m; ++c) {
        series[c][d] = xs[valid[c]];
      }
    }

    if (cfg.residualize == ClusterPanelConfig::Residualize::CAPM) {
      // Equal-weight cross-sectional market factor over the valid set, per date.
      std::vector<atx::f64> mkt(window, 0.0);
      for (atx::usize d = 0; d < window; ++d) {
        detail::NeumaierSum s;
        for (atx::usize c = 0; c < m; ++c) {
          s.add(series[c][d]);
        }
        mkt[d] = s.value() / static_cast<atx::f64>(m);
      }
      // Window means of the market factor (order-fixed).
      detail::NeumaierSum mkt_mean_s;
      for (atx::usize d = 0; d < window; ++d) {
        mkt_mean_s.add(mkt[d]);
      }
      const atx::f64 mkt_mean = mkt_mean_s.value() / static_cast<atx::f64>(window);
      // Var(mkt) over the window (compensated).
      detail::NeumaierSum mkt_var_s;
      for (atx::usize d = 0; d < window; ++d) {
        const atx::f64 dm = mkt[d] - mkt_mean;
        mkt_var_s.add(dm * dm);
      }
      const atx::f64 mkt_var = mkt_var_s.value();
      // Per-instrument OLS beta = Cov(r, mkt)/Var(mkt); residual = r - beta*mkt. A
      // degenerate (flat) market factor -> beta 0 (the residual is the raw series).
      for (atx::usize c = 0; c < m; ++c) {
        detail::NeumaierSum mean_s;
        for (atx::usize d = 0; d < window; ++d) {
          mean_s.add(series[c][d]);
        }
        const atx::f64 r_mean = mean_s.value() / static_cast<atx::f64>(window);
        detail::NeumaierSum cov_s;
        for (atx::usize d = 0; d < window; ++d) {
          cov_s.add((series[c][d] - r_mean) * (mkt[d] - mkt_mean));
        }
        const atx::f64 beta = mkt_var > 0.0 ? cov_s.value() / mkt_var : 0.0;
        for (atx::usize d = 0; d < window; ++d) {
          series[c][d] = series[c][d] - beta * mkt[d];
        }
      }
    }

    // ---- 3. Order-fixed Pearson correlation over the window. ------------------
    // Precompute each column's window mean and centered L2 norm (compensated).
    std::vector<atx::f64> mean(m, 0.0);
    std::vector<atx::f64> norm(m, 0.0); // sqrt(Σ (x-mean)²)
    for (atx::usize c = 0; c < m; ++c) {
      detail::NeumaierSum mean_s;
      for (atx::usize d = 0; d < window; ++d) {
        mean_s.add(series[c][d]);
      }
      mean[c] = mean_s.value() / static_cast<atx::f64>(window);
      detail::NeumaierSum ss;
      for (atx::usize d = 0; d < window; ++d) {
        const atx::f64 dm = series[c][d] - mean[c];
        ss.add(dm * dm);
      }
      norm[c] = std::sqrt(ss.value());
    }
    atx::core::linalg::MatX corr(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
    for (atx::usize a = 0; a < m; ++a) {
      corr(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(a)) = 1.0;
      for (atx::usize b = a + 1; b < m; ++b) {
        atx::f64 rho;
        if (norm[a] <= 0.0 || norm[b] <= 0.0) {
          // A flat (zero-variance) residual carries no co-movement signal; treat it
          // as uncorrelated rather than dividing by zero.
          rho = 0.0;
        } else {
          detail::NeumaierSum cov_s;
          for (atx::usize d = 0; d < window; ++d) {
            cov_s.add((series[a][d] - mean[a]) * (series[b][d] - mean[b]));
          }
          rho = std::clamp(cov_s.value() / (norm[a] * norm[b]), -1.0, 1.0);
        }
        corr(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) = rho;
        corr(static_cast<Eigen::Index>(b), static_cast<Eigen::Index>(a)) = rho;
      }
    }

    // ---- 4. RMT-clean the correlation (q = M/window). -------------------------
    const atx::f64 q = static_cast<atx::f64>(m) / static_cast<atx::f64>(window);
    auto cleaned = atx::core::linalg::rmt_clean(corr, q, {});
    if (!cleaned.has_value()) {
      return Err(cleaned.error());
    }

    // ---- 5. Partition the cleaned correlation. --------------------------------
    atx::core::cluster::ClusterConfig ccfg;
    ccfg.algo = cfg.algo;
    ccfg.k = static_cast<int>(std::min<atx::usize>(static_cast<atx::usize>(cfg.k), m));
    auto clustering = atx::core::cluster::cluster(cleaned.value().corr, ccfg);
    if (!clustering.has_value()) {
      return Err(clustering.error());
    }

    // ---- 6. Scatter canonical labels back to a full-length cluster_id. --------
    const std::vector<int> &labels = clustering.value().cluster_id;
    for (atx::usize c = 0; c < m; ++c) {
      snap.cluster_id[valid[c]] = labels[c];
    }
    snap.n_labels = clustering.value().n_labels;
    panel.snapshots.push_back(std::move(snap));
  }

  return Ok(std::move(panel));
}

} // namespace atx::engine::alpha
