#pragma once

// Descriptors of the group `catalog_core` (K-P9-13, sql-design section 3.7), in table
// order. PRIVATE header: included only by core_ops.cpp (and the schema gtests). Editing a
// descriptor changes the printed schema: the fixture
// atx-engine/tests/fixtures/research_store/schema/catalog_core.json must change with it.

#include <array>
#include <string_view>

#include "atx/engine/research/store/rows_core.hpp"
#include "research/store/detail/table.hpp"
#include "research/store/tables_common.hpp"

namespace atx::engine::research::store {

inline constexpr std::array<std::string_view, 2> kShaSources{"verified", "declared"};
inline constexpr std::array<std::string_view, 4> kEols{"lf", "crlf", "mixed", "none"};
inline constexpr std::array<std::string_view, 5> kSkipReasons{
    "seal-name", "outside-roots", "declared-only", "unreadable", "unparsed"};
inline constexpr std::array<std::string_view, 5> kProducerKinds{"engine", "python", "powershell",
                                                                "human", "unknown"};

inline constexpr auto kCatalogRunTable = table<CatalogRunRow>(
    "catalog_run",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = true, .check = {}},
    col<Sql::Text>("catalog_run_id", &CatalogRunRow::catalog_run_id, kKey),
    col<Sql::Text>("git_sha", &CatalogRunRow::git_sha),
    col<Sql::Sha256>("store_exe_sha256", &CatalogRunRow::store_exe_sha256),
    col<Sql::Json>("roots", &CatalogRunRow::roots),
    col<Sql::Text>("started_utc", &CatalogRunRow::started_utc),
    col<Sql::Real>("seconds", &CatalogRunRow::seconds),
    col<Sql::Int>("files_seen", &CatalogRunRow::files_seen),
    col<Sql::Int>("files_verified", &CatalogRunRow::files_verified),
    col<Sql::Int>("files_declared", &CatalogRunRow::files_declared),
    col<Sql::Int>("files_skipped", &CatalogRunRow::files_skipped),
    col<Sql::Sha256>("catalog_digest", &CatalogRunRow::catalog_digest));

inline constexpr auto kArtifactTable = table<ArtifactRow>(
    "artifact",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Text>("path_key", &ArtifactRow::path_key, kKey),
    col<Sql::RelPath>("path", &ArtifactRow::path),
    col<Sql::Sha256>("sha256", &ArtifactRow::sha256, kIndexed),
    col<Sql::Int>("bytes", &ArtifactRow::bytes),
    col<Sql::Text>("class", &ArtifactRow::artifact_class, kIndexed),
    col<Sql::Text>("json_schema", &ArtifactRow::json_schema),
    col<Sql::Text>("sha_source", &ArtifactRow::sha_source, 0, kShaSources),
    col<Sql::RelPath>("declared_by", &ArtifactRow::declared_by),
    col<Sql::Text>("eol", &ArtifactRow::eol, 0, kEols),
    col<Sql::Sha256>("producer_key", &ArtifactRow::producer_key));

inline constexpr auto kArtifactSeenTable = table<ArtifactSeenRow>(
    "artifact_seen",
    TableOpts{.version = 1, .since = 1, .append_only = true, .volatile_table = true, .check = {}},
    col<Sql::Text>("path_key", &ArtifactSeenRow::path_key, kKey),
    col<Sql::Sha256>("sha256", &ArtifactSeenRow::sha256, kKey),
    col<Sql::Text>("catalog_run_id", &ArtifactSeenRow::catalog_run_id, kKey));

inline constexpr auto kSkippedPathTable = table<SkippedPathRow>(
    "skipped_path",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = true, .check = {}},
    col<Sql::Text>("catalog_run_id", &SkippedPathRow::catalog_run_id, kKey),
    col<Sql::Text>("path", &SkippedPathRow::path, kKey),
    col<Sql::Text>("reason", &SkippedPathRow::reason, 0, kSkipReasons));

inline constexpr auto kProducerTable = table<ProducerRow>(
    "producer",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Sha256>("producer_key", &ProducerRow::producer_key, kKey),
    col<Sql::Text>("kind", &ProducerRow::kind, 0, kProducerKinds),
    col<Sql::Sha256>("exe_sha256", &ProducerRow::exe_sha256),
    col<Sql::Text>("git_sha", &ProducerRow::git_sha),
    col<Sql::Text>("build_type", &ProducerRow::build_type),
    col<Sql::Text>("module", &ProducerRow::module_name),
    col<Sql::Sha256>("code_sha256", &ProducerRow::code_sha256),
    col<Sql::Sha256>("receipt_sha256", &ProducerRow::receipt_sha256));

} // namespace atx::engine::research::store
