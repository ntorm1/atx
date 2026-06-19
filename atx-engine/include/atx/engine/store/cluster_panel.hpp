#pragma once
// atx::engine::store::cluster_panel — the DB index over the binary cluster-panel
// artifacts S11 produces (S11 scaffold). Mirrors segment_index: the DB never
// stores the heavy per-date labeling arrays, only a row of metadata that maps a
// panel_id (and its params/content hashes) to the binary file on disk, so a
// reader can locate and mmap the artifact.
//
// A cluster-panel artifact is the output of atx::engine::alpha::build_cluster_panel
// over one universe and parameter set: the rolling instrument×instrument cluster
// labeling described in atx/engine/alpha/cluster_panel.hpp. register_panel records
// it; lookup / locate resolve it back; compute_params_hash derives the canonical
// parameter fingerprint that makes two identical builds collide (a replay).
//
// Determinism contract (S11; inherited by S11-4): compute_params_hash is an
// FNV-1a-64 fold over a canonical, length-prefixed byte stream of the build
// parameters (same construction as store::fingerprint) — no wall-clock, no seed,
// no platform-dependent bytes — so identical parameters hash identically on every
// run and platform. content_hash is the FNV-1a-64 of the artifact bytes.
//
// TODO(S11-4): define register_panel / lookup / locate / compute_params_hash; the
// declarations below are the frozen seam.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store {

// One row of the cluster_panel index. Columns match the S11 plan. The DB holds
// this metadata only; `binary_path` points at the on-disk artifact, identified
// by `content_hash` and reproducible from `params_hash`.
struct ClusterPanelRecord {
  std::string panel_id;       // stable artifact id (primary key)
  std::string universe_id;    // the universe the panel was built over
  atx::i64 window_start{0};   // first date index covered by the panel (inclusive)
  atx::i64 window_end{0};     // last date index covered (inclusive)
  atx::i64 recluster_every{0};// recluster cadence in dates (1 == every date)
  atx::u64 params_hash{0};    // FNV-1a-64 of the canonical build parameters
  std::string asof_date;      // point-in-time as-of date the build was run for
  std::string binary_path;    // path to the on-disk labeling artifact
  atx::u64 content_hash{0};   // FNV-1a-64 of the artifact bytes
  std::string algo;           // clustering algorithm tag ("hierarchical"/"sponge")
  atx::i64 k{0};              // target cluster count
  std::string created_at;     // ISO-8601 creation timestamp (provenance only)
  std::string created_by_run_id; // run that produced the artifact
};

namespace cluster_panel {

// Insert (or replace) a cluster-panel index row. Fail-closed: an incomplete write
// returns Err(Internal), matching segment_index::register_segment.
//
// TODO(S11-4): define.
[[nodiscard]] atx::core::Status
register_panel(atx::core::db::Database& db, const ClusterPanelRecord& rec);

// Resolve a panel_id to its full record, or Err(NotFound) when absent.
//
// TODO(S11-4): define.
[[nodiscard]] atx::core::Result<ClusterPanelRecord>
lookup(atx::core::db::Database& db, std::string_view panel_id);

// Locate the most recent panel for a (universe_id, params_hash) as of a date,
// returning its record so the caller can mmap `binary_path`. Err(NotFound) when
// no matching panel was registered. This is the replay/reuse query: an identical
// params_hash collides with a prior build instead of recomputing it.
//
// TODO(S11-4): define.
[[nodiscard]] atx::core::Result<ClusterPanelRecord>
locate(atx::core::db::Database& db, std::string_view universe_id, atx::u64 params_hash,
       std::string_view asof_date);

// Canonical FNV-1a-64 fingerprint of the build parameters that determine a
// panel's contents (universe, window, recluster cadence, k, algorithm,
// residualization). Two builds with the same parameters MUST produce the same
// hash — see the determinism contract above. The exact field order is frozen in
// S11-4 and must not change once artifacts are keyed by it.
//
// TODO(S11-4): define against the final ClusterPanelConfig field set.
[[nodiscard]] atx::u64 compute_params_hash(const ClusterPanelRecord& rec) noexcept;

}  // namespace cluster_panel
}  // namespace atx::engine::store
