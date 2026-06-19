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
// TODO(S11-3): implement build_cluster_panel(); the declarations below are the
// frozen seam so S11-4's store record can be written against a stable result type.

#include <vector>

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // i64, usize

#include "atx/engine/alpha/panel.hpp" // Panel (the read-only returns input)

namespace atx::engine::alpha {

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

  // Target cluster count handed to atx::core::cluster (1 ≤ k ≤ instruments).
  int k = 0;

  // Residualization mode (default: raw returns).
  Residualize residualize = Residualize::None;
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

// Build a rolling cluster panel from a returns Panel.
//
// `src` is the read-only returns Panel (one return field selected per cfg; the
// exact field-selection contract is fixed in S11-3). `cfg` sets the window,
// recluster cadence, target k, and residualization.
//
// Returns the per-date cluster snapshots, or Err on an invalid config (non-
// positive window / recluster_every / k, k beyond the instrument count) or a
// propagated atx-core failure (InvalidArgument / Internal).
//
// TODO(S11-3): define this function.
[[nodiscard]] Result<ClusterPanel> build_cluster_panel(const Panel &src, ClusterPanelConfig cfg);

} // namespace atx::engine::alpha
