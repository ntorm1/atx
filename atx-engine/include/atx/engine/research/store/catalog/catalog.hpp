#pragma once

// atx::engine::research::store::catalog -- the artifact catalog (P9 SQL2; sql-design section
// 3.9, rulings SQL-2 / SQL-7). It indexes the research tree without moving a byte: JSON stays
// the authority for every pinned or chained artifact (SQL-2).
//
// A catalog run walks from the pins outward (seal-safe). Seeds: the spec globs (default
// scripts/specs/*.json, scripts/specs/v8/**, scripts/specs/p9/**), every registry class's
// `seed_globs` (repository inputs such as the alpha registry, the research window, the field
// registry, and the research-build receipts), the --ledger files and explicit --include paths.
// From each opened holder it follows the pins (except field-source), the files it names, and
// the run / cycle / wave dirs it names (walked whole). The walk takes the smallest pending
// path_key first, in segments of `segment_files` files; each segment's rows are written in one
// BEGIN IMMEDIATE. Never opened: a path with a standalone year token 2024-2099 (seal-name), a
// file over 16 MiB (its SHA-256 is the one the smallest holder path declares: sha_source
// `declared`, else skipped `declared-only`; --verify-payloads DIR hashes those under DIR and
// records `verified`). After the walk every other file under build-equity/ is listed by name
// only (skipped_path `outside-roots`; a seal-named dir as one `seal-name` row, not descended).
// The run ends with its catalog_run row (files_verified / files_declared apart), the catalog
// digest and PRAGMA wal_checkpoint(TRUNCATE).
//
// A run upserts: rows of files no longer on disk stay until `catalog --rebuild` (a fresh
// store). The catalog digest covers the non-volatile tables, so a run over the same tree from
// scratch, in any walk order, reproduces it.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/classify.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/store.hpp"

namespace atx::engine::research::store::catalog {

// The catalog DB's groups, in order: {core_group(), records_group()}.
[[nodiscard]] std::span<const GroupOps *const> catalog_groups() noexcept;

// open_store(path, Catalog, catalog_groups()).
[[nodiscard]] core::Result<core::db::Database> open_catalog(std::string_view path);

// The default --specs globs (root-relative).
[[nodiscard]] std::vector<std::string> default_spec_globs();

struct CatalogOptions {
  std::filesystem::path root;                     // the tree; every catalogued path is under it
  std::vector<std::string> spec_globs;            // empty: default_spec_globs()
  std::vector<std::string> ledgers;               // root-relative or absolute ledger files
  std::vector<std::string> includes;              // extra root-relative files or dirs
  std::optional<std::string> verify_payloads_dir; // root-relative dir whose big files are hashed
  std::optional<std::string> store_exe_sha256;    // catalog_run.store_exe_sha256
  LedgerHeadFn ledger_head;                       // ingest.hpp seam (empty: heads stay NULL)
  usize segment_files{500};                       // files per BEGIN IMMEDIATE
  bool reverse_walk{false}; // tests only: take the largest pending path first (order proof)
};

struct CatalogReport {
  std::string catalog_run_id;
  std::string catalog_digest;
  i64 files_seen{};
  i64 files_verified{};
  i64 files_declared{};
  i64 files_skipped{};
  f64 seconds{};
  bool checkpointed{}; // wal_checkpoint(TRUNCATE) completed (a concurrent reader blocks it)
};

// One catalog run over `opt.root` into `db` (a catalog store). Err as the store reports it;
// Err(PermissionDenied) when a trial ledger's catalogued lines changed (append-only).
[[nodiscard]] core::Result<CatalogReport> run_catalog(core::db::Database &db,
                                                      const ClassRegistry &registry,
                                                      const CatalogOptions &opt);

struct IngestOneOptions {
  std::filesystem::path root;
  std::string class_id;
  std::string path; // root-relative or absolute, inside root
  LedgerHeadFn ledger_head;
};

struct IngestOneReport {
  std::string path;
  std::string class_id;
  bool unparsed{};
  usize pins{};
};

// `ingest --class C --path P`: one file, by the named class, in one BEGIN IMMEDIATE;
// idempotent (the same file gives the same rows). Err(InvalidArgument) for an unknown class;
// Err(PermissionDenied) for a path outside the root, a sealed path or a file over 16 MiB
// (never opened); Err(NotFound) for a missing file.
[[nodiscard]] core::Result<IngestOneReport> ingest_one(core::db::Database &db,
                                                       const ClassRegistry &registry,
                                                       const IngestOneOptions &opt);

} // namespace atx::engine::research::store::catalog
