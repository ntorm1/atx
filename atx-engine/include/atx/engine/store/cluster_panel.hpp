#pragma once
// atx::engine::store::cluster_panel — the DB index over the binary cluster-panel
// artifacts S11 produces. Mirrors segment_index: the DB never stores the heavy
// per-date labeling arrays, only a row of metadata that maps a panel_id (and its
// params/content hashes) to the binary file on disk, so a reader can locate and
// load the artifact.
//
// A cluster-panel artifact is the output of atx::engine::alpha::build_cluster_panel
// over one universe and parameter set: the rolling instrument×instrument cluster
// labeling described in atx/engine/alpha/cluster_panel.hpp. register_panel records
// it; lookup / locate resolve it back; compute_params_hash derives the canonical
// parameter fingerprint that makes two identical builds collide (a replay).
//
// Determinism contract (S11; implemented in S11-4): compute_params_hash is an
// FNV-1a-64 fold over a canonical, length-prefixed byte stream of the build
// parameters (same construction as store::fingerprint) — no wall-clock, no seed,
// no platform-dependent bytes — so identical parameters hash identically on every
// run and platform. content_hash is the FNV-1a-64 of the exact artifact bytes.
//
// ===========================================================================
//  Binary artifact layout (version 1) — consistent with how segment/segment_index
//  keep heavy arrays in an external file rather than a DB BLOB. All integers are
//  little-endian, fixed width; the layout is fully self-describing and lossless
//  for a ClusterPanel, so save_binary -> load_binary is an exact round-trip and
//  save -> load -> save reproduces byte-identical bytes (and content_hash).
//
//    Header (24 bytes):
//      u32  magic        = kMagic ('A','C','P','1' as 0x31504341 little-endian)
//      u32  version      = kFormatVersion (1)
//      u64  instruments  = ClusterPanel::instruments
//      u64  n_snapshots  = ClusterPanel::snapshots.size()
//    Then, for each snapshot in stored (ascending-date) order:
//      u64  date         = Snapshot::date           (DateIdx == usize)
//      i64  n_labels     = Snapshot::n_labels
//      i32  cluster_id[instruments]                 (each label, kUnclustered==-1
//                                                    encoded as 0xFFFFFFFF)
//
//  The per-snapshot label count is exactly `instruments` (the header field), so
//  the layout needs no per-snapshot length prefix. content_hash is FNV-1a-64 over
//  the whole byte image, using the same offset/prime as store::fingerprint.
// ===========================================================================

#include <cstddef>     // std::byte, std::size_t
#include <cstdint>     // fixed-width ints
#include <fstream>     // binary file I/O for the artifact
#include <ios>         // std::ios::binary
#include <optional>    // std::optional (lookup result)
#include <string>
#include <string_view>
#include <utility>     // std::move
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/cluster_panel.hpp" // ClusterPanel (the built result)
#include "atx/engine/store/fingerprint.hpp"   // FNV-1a-64 fold helpers (offset/prime)

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

// Binary artifact format constants (see the layout block at the top of the file).
// kMagic spells "ACP1" in little-endian byte order on disk.
inline constexpr atx::u32 kMagic = 0x31504341u;  // 'A','C','P','1'
inline constexpr atx::u32 kFormatVersion = 1u;
// kUnclustered (-1) serialized as a u32; load decodes it back to the sentinel.
inline constexpr atx::u32 kUnclusteredEncoded = 0xFFFFFFFFu;

// The canonical, replay-stable parameter set that determines a panel's contents.
// compute_params_hash folds these in a FROZEN order (see below). Any field change
// yields a different hash; no wall-clock/RNG is read. Mirrors store::RunInputs.
struct ParamsKey {
  std::string universe_id;          // the universe the panel was built over
  atx::u64 universe_content_hash{0};// content hash of the resolved universe
  atx::u64 source_content_hash{0};  // content hash of the source snapshot/panel
  atx::i64 window{0};               // ClusterPanelConfig::window
  atx::i64 recluster_every{0};      // ClusterPanelConfig::recluster_every
  atx::i64 k{0};                    // ClusterPanelConfig::k
  std::string residualize;          // ClusterPanelConfig::residualize tag ("None"/"CAPM")
  std::string return_field;         // ClusterPanelConfig::return_field
  std::string algo;                 // ClusterPanelConfig::algo tag
};

// ---------------------------------------------------------------------------
//  compute_params_hash — FROZEN field order. Folded via store::fingerprint with
//  the same FNV-1a-64 offset/prime and length-prefixed strings, so the key is
//  replay-stable and identical cross-platform. The order below MUST NOT change
//  once artifacts are keyed by it.
//
//    1. universe_id            (length-prefixed string)
//    2. universe_content_hash  (u64)
//    3. source_content_hash    (u64)
//    4. window                 (u64; i64 reinterpreted)
//    5. recluster_every        (u64)
//    6. k                      (u64)
//    7. residualize            (length-prefixed string)
//    8. return_field           (length-prefixed string)
//    9. algo                   (length-prefixed string)
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::u64 compute_params_hash(const ParamsKey& key) noexcept {
  atx::u64 h = fingerprint::kFnvOffset;
  h = fingerprint::fold_string(h, key.universe_id);
  h = fingerprint::fold_u64(h, key.universe_content_hash);
  h = fingerprint::fold_u64(h, key.source_content_hash);
  h = fingerprint::fold_u64(h, static_cast<atx::u64>(key.window));
  h = fingerprint::fold_u64(h, static_cast<atx::u64>(key.recluster_every));
  h = fingerprint::fold_u64(h, static_cast<atx::u64>(key.k));
  h = fingerprint::fold_string(h, key.residualize);
  h = fingerprint::fold_string(h, key.return_field);
  h = fingerprint::fold_string(h, key.algo);
  return h;
}

namespace detail {

// Append `v` as 8 little-endian bytes to `out`.
inline void put_u64(std::string& out, atx::u64 v) {
  for (int i = 0; i < 8; ++i) { out.push_back(static_cast<char>(v & 0xFF)); v >>= 8; }
}
// Append `v` as 4 little-endian bytes to `out`.
inline void put_u32(std::string& out, atx::u32 v) {
  for (int i = 0; i < 4; ++i) { out.push_back(static_cast<char>(v & 0xFF)); v >>= 8; }
}
// Read 8 little-endian bytes from `in` at `pos`, advancing `pos`.
[[nodiscard]] inline atx::u64 get_u64(std::string_view in, std::size_t& pos) noexcept {
  atx::u64 v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<atx::u64>(static_cast<atx::u8>(in[pos + static_cast<std::size_t>(i)]))
         << (8 * i);
  }
  pos += 8;
  return v;
}
// Read 4 little-endian bytes from `in` at `pos`, advancing `pos`.
[[nodiscard]] inline atx::u32 get_u32(std::string_view in, std::size_t& pos) noexcept {
  atx::u32 v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<atx::u32>(static_cast<atx::u8>(in[pos + static_cast<std::size_t>(i)]))
         << (8 * i);
  }
  pos += 4;
  return v;
}

// Serialize a ClusterPanel into the version-1 byte image (see the layout block).
[[nodiscard]] inline std::string serialize(const atx::engine::alpha::ClusterPanel& panel) {
  std::string bytes;
  put_u32(bytes, kMagic);
  put_u32(bytes, kFormatVersion);
  put_u64(bytes, static_cast<atx::u64>(panel.instruments));
  put_u64(bytes, static_cast<atx::u64>(panel.snapshots.size()));
  for (const auto& snap : panel.snapshots) {
    put_u64(bytes, static_cast<atx::u64>(snap.date));
    put_u64(bytes, static_cast<atx::u64>(snap.n_labels));
    // cluster_id is exactly `instruments` long for a well-formed snapshot; if a
    // caller hands a short/long vector we still write exactly `instruments`
    // entries so the layout stays self-consistent with the header.
    for (atx::usize i = 0; i < panel.instruments; ++i) {
      const int label = (i < snap.cluster_id.size()) ? snap.cluster_id[i]
                                                      : atx::engine::alpha::ClusterPanel::kUnclustered;
      const atx::u32 enc = (label == atx::engine::alpha::ClusterPanel::kUnclustered)
                               ? kUnclusteredEncoded
                               : static_cast<atx::u32>(label);
      put_u32(bytes, enc);
    }
  }
  return bytes;
}

}  // namespace detail

// FNV-1a-64 over a byte image, using the store fingerprint offset/prime so the
// content_hash convention matches the rest of the store.
[[nodiscard]] inline atx::u64 content_hash_of_bytes(std::string_view bytes) noexcept {
  return fingerprint::fold_bytes(fingerprint::kFnvOffset, bytes);
}

// Serialize `panel` to `path` as a version-1 binary artifact and return the
// content_hash of the exact bytes written. Err(Internal) on an I/O failure.
[[nodiscard]] inline atx::core::Result<atx::u64>
save_binary(const std::string& path, const atx::engine::alpha::ClusterPanel& panel) {
  const std::string bytes = detail::serialize(panel);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "cluster_panel::save_binary: cannot open artifact for write");
  }
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f.good()) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "cluster_panel::save_binary: write failed");
  }
  return atx::core::Ok(content_hash_of_bytes(bytes));
}

// Read the entire artifact file into a string. Err(NotFound) if it cannot open.
[[nodiscard]] inline atx::core::Result<std::string> read_file_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "cluster_panel::read_file_bytes: artifact not found");
  }
  std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (f.bad()) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "cluster_panel::read_file_bytes: read failed");
  }
  return atx::core::Ok(std::move(bytes));
}

// content_hash of the bytes currently on disk at `path` — used to verify an
// artifact against its registered content_hash (corruption / drift detection).
[[nodiscard]] inline atx::core::Result<atx::u64> content_hash_of_file(const std::string& path) {
  ATX_TRY(const auto bytes, read_file_bytes(path));
  return atx::core::Ok(content_hash_of_bytes(bytes));
}

// Deserialize a version-1 artifact at `path` back into a ClusterPanel. Validates
// magic/version and the declared size; Err(InvalidArgument) on a malformed image.
[[nodiscard]] inline atx::core::Result<atx::engine::alpha::ClusterPanel>
load_binary(const std::string& path) {
  ATX_TRY(const auto bytes, read_file_bytes(path));
  std::string_view in = bytes;
  // Header is 24 bytes: magic(4) + version(4) + instruments(8) + n_snapshots(8).
  if (in.size() < 24) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "cluster_panel::load_binary: truncated header");
  }
  std::size_t pos = 0;
  const atx::u32 magic = detail::get_u32(in, pos);
  const atx::u32 version = detail::get_u32(in, pos);
  if (magic != kMagic) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "cluster_panel::load_binary: bad magic");
  }
  if (version != kFormatVersion) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "cluster_panel::load_binary: unsupported format version");
  }
  const atx::u64 instruments = detail::get_u64(in, pos);
  const atx::u64 n_snapshots = detail::get_u64(in, pos);
  // Each snapshot is date(8) + n_labels(8) + instruments * label(4).
  const atx::u64 per_snap = 16u + instruments * 4u;
  const atx::u64 need = 24u + n_snapshots * per_snap;
  if (static_cast<atx::u64>(in.size()) != need) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "cluster_panel::load_binary: size does not match header");
  }
  atx::engine::alpha::ClusterPanel panel;
  panel.instruments = static_cast<atx::usize>(instruments);
  panel.snapshots.reserve(static_cast<std::size_t>(n_snapshots));
  for (atx::u64 s = 0; s < n_snapshots; ++s) {
    atx::engine::alpha::ClusterPanel::Snapshot snap;
    snap.date = static_cast<atx::engine::alpha::DateIdx>(detail::get_u64(in, pos));
    snap.n_labels = static_cast<atx::i64>(detail::get_u64(in, pos));
    snap.cluster_id.resize(static_cast<std::size_t>(instruments));
    for (atx::u64 i = 0; i < instruments; ++i) {
      const atx::u32 enc = detail::get_u32(in, pos);
      snap.cluster_id[static_cast<std::size_t>(i)] =
          (enc == kUnclusteredEncoded) ? atx::engine::alpha::ClusterPanel::kUnclustered
                                       : static_cast<int>(enc);
    }
    panel.snapshots.push_back(std::move(snap));
  }
  return atx::core::Ok(std::move(panel));
}

// Insert (or replace) a cluster-panel index row, idempotent on panel_id. Fail-
// closed: an incomplete write returns Err(Internal), matching segment_index.
[[nodiscard]] inline atx::core::Status
register_panel(atx::core::db::Database& db, const ClusterPanelRecord& rec) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO cluster_panel(panel_id, universe_id, window_start, window_end,"
      " recluster_every, params_hash, asof_date, binary_path, content_hash, algo, k,"
      " created_at, created_by_run_id) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)"));
  ATX_TRY_VOID(stmt->bind(1, rec.panel_id));
  ATX_TRY_VOID(stmt->bind(2, rec.universe_id));
  ATX_TRY_VOID(stmt->bind(3, rec.window_start));
  ATX_TRY_VOID(stmt->bind(4, rec.window_end));
  ATX_TRY_VOID(stmt->bind(5, rec.recluster_every));
  ATX_TRY_VOID(stmt->bind(6, static_cast<atx::i64>(rec.params_hash)));
  ATX_TRY_VOID(stmt->bind(7, rec.asof_date));
  ATX_TRY_VOID(stmt->bind(8, rec.binary_path));
  ATX_TRY_VOID(stmt->bind(9, static_cast<atx::i64>(rec.content_hash)));
  ATX_TRY_VOID(stmt->bind(10, rec.algo));
  ATX_TRY_VOID(stmt->bind(11, rec.k));
  ATX_TRY_VOID(stmt->bind(12, rec.created_at));
  ATX_TRY_VOID(stmt->bind(13, rec.created_by_run_id));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "cluster_panel::register_panel: incomplete");
  }
  return atx::core::Ok();
}

namespace detail {

// Materialize a ClusterPanelRecord from a SELECT * row (column order matches the
// register_panel INSERT / the table DDL).
[[nodiscard]] inline ClusterPanelRecord read_row(const atx::core::db::Statement& stmt) {
  ClusterPanelRecord rec;
  rec.panel_id = std::string(stmt.column_text(0));
  rec.universe_id = std::string(stmt.column_text(1));
  rec.window_start = stmt.column_int(2);
  rec.window_end = stmt.column_int(3);
  rec.recluster_every = stmt.column_int(4);
  rec.params_hash = static_cast<atx::u64>(stmt.column_int(5));
  rec.asof_date = std::string(stmt.column_text(6));
  rec.binary_path = std::string(stmt.column_text(7));
  rec.content_hash = static_cast<atx::u64>(stmt.column_int(8));
  rec.algo = std::string(stmt.column_text(9));
  rec.k = stmt.column_int(10);
  rec.created_at = std::string(stmt.column_text(11));
  rec.created_by_run_id = std::string(stmt.column_text(12));
  return rec;
}

}  // namespace detail

// Resolve a registered panel by its (universe_id, asof_date, params_hash) key,
// returning Ok(nullopt) when absent (a miss is not an error). This is the
// replay/reuse query: an identical params_hash collides with a prior build.
[[nodiscard]] inline atx::core::Result<std::optional<ClusterPanelRecord>>
lookup(atx::core::db::Database& db, std::string_view universe_id, std::string_view asof_date,
       atx::u64 params_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT panel_id, universe_id, window_start, window_end, recluster_every, params_hash,"
      " asof_date, binary_path, content_hash, algo, k, created_at, created_by_run_id"
      " FROM cluster_panel WHERE universe_id = ?1 AND asof_date = ?2 AND params_hash = ?3 LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, universe_id));
  ATX_TRY_VOID(stmt->bind(2, asof_date));
  ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(params_hash)));
  ATX_TRY(const auto step, stmt->step());
  if (step == atx::core::db::Statement::Step::Done) {
    return atx::core::Ok(std::optional<ClusterPanelRecord>{});
  }
  return atx::core::Ok(std::optional<ClusterPanelRecord>{detail::read_row(*stmt)});
}

// Locate the binary_path of the panel for a (universe_id, asof_date, params_hash)
// key so the caller can load the artifact. Err(NotFound) when no matching panel
// was registered.
[[nodiscard]] inline atx::core::Result<std::string>
locate(atx::core::db::Database& db, std::string_view universe_id, std::string_view asof_date,
       atx::u64 params_hash) {
  ATX_TRY(const auto rec, lookup(db, universe_id, asof_date, params_hash));
  if (!rec.has_value()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "cluster_panel::locate: panel not registered");
  }
  return atx::core::Ok(rec->binary_path);
}

}  // namespace cluster_panel
}  // namespace atx::engine::store
