#pragma once

// atx::engine::alpha — wire a rolling ClusterPanel into the DSL's group operators
// as an `IndClass.cluster` field (S11-5).
//
// ===========================================================================
//  What this header does (no new DSL operators)
// ===========================================================================
//  The DSL's cross-sectional group family (group_mean / group_neutralize ==
//  indneutralize / group_zscore / group_rank / group_scale / group_count /
//  cs_residualize) already keys its group-id argument PURELY BY FIELD NAME:
//  typecheck.hpp's `is_group_field()` treats a field as a `Group` dtype iff it is
//  named `sector` OR carries the `IndClass.` prefix. So a field literally named
//  `IndClass.cluster` is ALREADY group-typed with zero typecheck/registry/VM
//  change. This unit's only job is to PRODUCE that f64 column from a built (or
//  store-loaded) ClusterPanel and feed it to the existing consumers:
//
//    * broadcast_cluster_field  — ClusterPanel -> dense date-major f64 column.
//    * append_cluster_field     — source Panel + ClusterPanel -> new Panel with
//                                  the `IndClass.cluster` column appended (the
//                                  headline offline path; reuses Panel::create).
//    * cluster_group_map        — one date's cluster cross-section as the u32
//                                  group_map WeightPolicy::to_target_weights
//                                  consumes (the online / backtest-loop path).
//
// ===========================================================================
//  Encoding — label L -> f64(L); kUnclustered (-1) -> NaN
// ===========================================================================
//  cs_ops.hpp partitions a group field by the cell's f64 value and treats a NaN
//  cell as OUT-OF-GROUP (no group -> its output stays NaN). The ClusterPanel uses
//  the sentinel kUnclustered (-1) for an instrument outside the point-in-time
//  valid set. To match the cs_ops out-of-set semantics EXACTLY, a real label `L`
//  is encoded as `f64(L)` and `kUnclustered` is encoded as NaN — so an
//  unclustered instrument is excluded from its group's reduction with no special
//  case in the kernels (it reads back exactly like an out-of-universe cell).
//
// ===========================================================================
//  Step-function hold (point-in-time, the S11-3 contract)
// ===========================================================================
//  build_cluster_panel emits exactly ONE snapshot per rebalance date `t`, in
//  ascending-date order. A snapshot's labels are the partition that holds AS OF
//  `t` and remain valid until the NEXT rebalance. broadcast_cluster_field fills
//  date range `[snapshots[i].date, snapshots[i+1].date)` with `snapshots[i]`'s
//  labels; the last snapshot holds to the panel end. Dates BEFORE the first
//  snapshot (the warm-up window, `< window-1`) have no labeling and read NaN
//  (kUnclustered). No look-ahead: date `d` reads only the most recent snapshot
//  whose `date <= d`.
//
// Header-only; every free function is `inline`. The broadcast is a COLD path (run
// once when a Panel is materialized for a backtest window), so std::vector
// allocation is fine.

#include <algorithm> // std::min
#include <cstdint>   // std::uint8_t (universe mask element)
#include <limits>    // std::numeric_limits
#include <span>        // std::span (Panel field views)
#include <string>      // std::string
#include <string_view> // std::string_view (rename source field)
#include <utility>     // std::move
#include <vector>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/alpha/cluster_panel.hpp" // ClusterPanel, DateIdx, kUnclustered
#include "atx/engine/alpha/panel.hpp"         // Panel (the source + augmented input)

namespace atx::engine::alpha {

// The canonical field name the group operators recognize for a return-structure
// cluster classifier. `IndClass.cluster` is already group-typed by name via
// typecheck.hpp's `is_group_field()` (the `IndClass.` prefix) — no typecheck
// change is needed to use it.
inline constexpr const char *kClusterFieldName = "IndClass.cluster";

// Encode one cluster label as the group field's f64 value: a real label `L` (>= 0)
// becomes `f64(L)`; the kUnclustered sentinel (-1) becomes NaN so the group ops
// treat the cell as out-of-set (matching cs_ops). Defined as a free function so
// the offline broadcast and the per-date views share one encoding rule.
[[nodiscard]] inline atx::f64 encode_cluster_label(int label) noexcept {
  return (label == ClusterPanel::kUnclustered) ? std::numeric_limits<atx::f64>::quiet_NaN()
                                               : static_cast<atx::f64>(label);
}

// Broadcast a rolling ClusterPanel into a dense date-major f64 column of length
// `dates * panel.instruments`, applying the step-function hold described above.
//
// `dates` is the date count of the target Panel (must cover the snapshot dates).
// The column is laid out date-major: cell (date, inst) at flat index
// `date * instruments + inst`, matching Panel's storage. Snapshots are assumed in
// ascending-date order (the build_cluster_panel contract); a snapshot whose `date`
// is >= `dates` contributes no cells (it lies beyond the target window).
//
// Returns the column. An empty panel (no snapshots) -> every cell NaN (no labeling
// anywhere). Cells before the first snapshot's date are NaN (warm-up, kUnclustered).
[[nodiscard]] inline std::vector<atx::f64>
broadcast_cluster_field(const ClusterPanel &panel, atx::usize dates) {
  const atx::usize instruments = panel.instruments;
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  std::vector<atx::f64> col(dates * instruments, nan);

  // For each snapshot, fill [snapshots[i].date, next_date) with its labels. The
  // last snapshot holds to `dates`. Pre-first-snapshot dates keep their NaN init.
  for (atx::usize i = 0; i < panel.snapshots.size(); ++i) {
    const ClusterPanel::Snapshot &snap = panel.snapshots[i];
    const atx::usize d0 = snap.date;
    if (d0 >= dates) {
      continue; // snapshot lies beyond the target window — nothing to fill
    }
    const atx::usize d1 = (i + 1 < panel.snapshots.size())
                              ? std::min<atx::usize>(panel.snapshots[i + 1].date, dates)
                              : dates;
    for (atx::usize d = d0; d < d1; ++d) {
      const atx::usize base = d * instruments;
      for (atx::usize j = 0; j < instruments; ++j) {
        const int label = (j < snap.cluster_id.size()) ? snap.cluster_id[j]
                                                        : ClusterPanel::kUnclustered;
        col[base + j] = encode_cluster_label(label);
      }
    }
  }
  return col;
}

// Append the broadcast `IndClass.cluster` column to a copy of `src`, returning a
// new Panel the DSL group operators can read by name. The headline OFFLINE path:
// the source Panel carries the return/price fields; the cluster panel (built fresh
// or rehydrated via store::cluster_panel::load_binary) carries the labels.
//
// Reuses Panel::create with the augmented field list (every source field, in the
// same order, then `IndClass.cluster`), so the result is an ordinary owned Panel —
// no new Panel machinery. The cluster panel's instrument count MUST match the
// source Panel's, or the column would mis-index instruments; mismatch ->
// Err(InvalidArgument). The source universe mask is carried forward verbatim
// (the cluster column is a classifier, not a universe gate).
[[nodiscard]] inline atx::core::Result<Panel>
append_cluster_field(const Panel &src, const ClusterPanel &clusters) {
  if (clusters.instruments != src.instruments()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "append_cluster_field: cluster panel instrument count does not match "
                          "the source Panel");
  }
  const atx::usize dates = src.dates();
  const atx::usize instruments = src.instruments();

  // Rebuild the source field dictionary + columns (Panel exposes field_name and
  // field_all so a const Panel re-enumerates itself without a separate name list),
  // then append the cluster classifier as the last field.
  std::vector<std::string> names;
  names.reserve(src.num_fields() + 1);
  std::vector<std::vector<atx::f64>> data;
  data.reserve(src.num_fields() + 1);
  for (atx::usize f = 0; f < src.num_fields(); ++f) {
    names.emplace_back(src.field_name(f));
    const std::span<const atx::f64> col = src.field_all(static_cast<FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }
  names.emplace_back(kClusterFieldName);
  data.push_back(broadcast_cluster_field(clusters, dates));

  // Reconstruct the universe mask (1 == in-universe). Panel offers in_universe but
  // not a whole-mask span, so materialize it cell-by-cell to feed Panel::create.
  std::vector<std::uint8_t> universe(dates * instruments, std::uint8_t{0});
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      universe[d * instruments + j] = src.in_universe(d, j) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }

  return Panel::create(dates, instruments, std::move(names), std::move(data), std::move(universe));
}

// The on-disk segment field-name capacity (atx-tsdb FieldEntry) is 15 usable
// bytes, so the full 16-char `IndClass.cluster` name cannot survive a segment
// round-trip verbatim — it would be truncated to `IndClass.cluste`. The SEGMENT
// path therefore bakes the cluster column under this short tag (well within the
// cap) and `rename_field_to_cluster` re-labels it to `IndClass.cluster` after the
// segment is attached, so the DSL group operators recognize it by name.
inline constexpr const char *kClusterSegmentField = "cluster";

// Return a copy of `attached` with the field named `from` renamed to
// `IndClass.cluster` (every other field, its order, the universe mask, and all
// values are preserved). This bridges the segment field-name cap (see
// kClusterSegmentField): a segment bakes the labels under the short `cluster` tag,
// attach_multi_segment_panel/attach_segment_panel reads it back, and this rename
// makes the column group-typed by name for the existing group_* operators. The
// resulting Panel is OWNED (it copies the column data), so it does not alias the
// attached segment mapping. Err(NotFound) if `from` is not a field of `attached`.
[[nodiscard]] inline atx::core::Result<Panel>
rename_field_to_cluster(const Panel &attached, std::string_view from = kClusterSegmentField) {
  const atx::usize dates = attached.dates();
  const atx::usize instruments = attached.instruments();

  bool found = false;
  std::vector<std::string> names;
  names.reserve(attached.num_fields());
  std::vector<std::vector<atx::f64>> data;
  data.reserve(attached.num_fields());
  for (atx::usize f = 0; f < attached.num_fields(); ++f) {
    const std::string_view fname = attached.field_name(static_cast<FieldId>(f));
    if (fname == from) {
      names.emplace_back(kClusterFieldName);
      found = true;
    } else {
      names.emplace_back(fname);
    }
    const std::span<const atx::f64> col = attached.field_all(static_cast<FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }
  if (!found) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "rename_field_to_cluster: source field not present in the attached Panel");
  }

  std::vector<std::uint8_t> universe(dates * instruments, std::uint8_t{0});
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      universe[d * instruments + j] = attached.in_universe(d, j) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return Panel::create(dates, instruments, std::move(names), std::move(data), std::move(universe));
}

// Build the per-date cluster group_map for the ONLINE backtest path: one u32 group
// id per universe instrument at date `date`, as consumed by
// WeightPolicy::to_target_weights(signal, universe, group_map) when
// industry_neutral is on.
//
// WeightPolicy's group_map is a DENSE u32 keyed per universe index; it has no NaN /
// "no group" sentinel (every live instrument lands in exactly one group, demeaned
// within it). An unclustered instrument is therefore mapped to a SINGLETON group of
// its own (id `base + inst_index`, disjoint from every real cluster id) so its
// per-group demean drives it to ~0 rather than contaminating a real cluster's mean.
// This matches the cs_ops "out-of-group" intent (an unclustered name carries no
// cluster exposure) within WeightPolicy's no-sentinel group model.
//
// `date` selects the most recent snapshot whose `date <= date` (the step-function
// hold). Before the first snapshot every instrument is unclustered -> each its own
// singleton group. The returned vector has length `panel.instruments`, universe-
// aligned exactly as build_cluster_panel's cluster_id is.
[[nodiscard]] inline std::vector<atx::u32>
cluster_group_map(const ClusterPanel &panel, DateIdx date) {
  const atx::usize instruments = panel.instruments;
  // Singleton ids for unclustered instruments start ABOVE any real cluster id. A
  // snapshot's n_labels bounds the real ids in [0, n_labels); using `instruments`
  // as the base is always safe (a partition over M<=instruments names has at most
  // `instruments` labels), keeping singleton ids disjoint from real ones.
  const atx::u32 singleton_base = static_cast<atx::u32>(instruments);

  // Locate the held snapshot: the last one with date <= `date` (ascending order).
  const ClusterPanel::Snapshot *held = nullptr;
  for (const ClusterPanel::Snapshot &snap : panel.snapshots) {
    if (snap.date <= date) {
      held = &snap;
    } else {
      break;
    }
  }

  std::vector<atx::u32> group_map(instruments, 0U);
  for (atx::usize j = 0; j < instruments; ++j) {
    const int label =
        (held != nullptr && j < held->cluster_id.size()) ? held->cluster_id[j]
                                                          : ClusterPanel::kUnclustered;
    group_map[j] = (label == ClusterPanel::kUnclustered) ? (singleton_base + static_cast<atx::u32>(j))
                                                         : static_cast<atx::u32>(label);
  }
  return group_map;
}

} // namespace atx::engine::alpha
