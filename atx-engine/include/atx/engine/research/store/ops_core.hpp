#pragma once

// atx::engine::research::store -- non-template operations of the group `catalog_core`
// (and of the shared store_info table). Defined in src/research/store/core_ops.cpp, the
// group's only instantiating TU.
//
// Per table X:
//   insert(db, row)       INSERT; Err(AlreadyExists) for a duplicate key, Err(InvalidArgument)
//                         for a value a CHECK / trigger refuses (or a NaN / -0.0 REAL)
//   upsert(db, row)       INSERT ... ON CONFLICT(key) DO UPDATE SET every non-key column
//   read_X(stmt)          the current row of a statement whose result columns are the
//                         table's columns in row-member order
//   select_all_X(db)      every row, ORDER BY the key
//   digest(row)           atx.record-digest/v1 of the row (digest.hpp), 64 lower-case hex
// Statements go through the connection's prepare_cached; the caller owns transactions.
// Thread-safety: as the Database (one thread per connection).

#include <string>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/rows_core.hpp"
#include "atx/engine/research/store/store.hpp"

namespace atx::engine::research::store {

// The catalog DB's core group: store_info, catalog_run, artifact, artifact_seen,
// skipped_path, producer (version 1).
[[nodiscard]] const GroupOps &core_group() noexcept;

// store_info
[[nodiscard]] core::Status insert(core::db::Database &db, const StoreInfoRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const StoreInfoRow &row);
[[nodiscard]] core::Result<StoreInfoRow> read_store_info(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<StoreInfoRow>> select_all_store_info(core::db::Database &db);
[[nodiscard]] std::string digest(const StoreInfoRow &row);

// catalog_run
[[nodiscard]] core::Status insert(core::db::Database &db, const CatalogRunRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const CatalogRunRow &row);
[[nodiscard]] core::Result<CatalogRunRow> read_catalog_run(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<CatalogRunRow>>
select_all_catalog_run(core::db::Database &db);
[[nodiscard]] std::string digest(const CatalogRunRow &row);

// artifact
[[nodiscard]] core::Status insert(core::db::Database &db, const ArtifactRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const ArtifactRow &row);
[[nodiscard]] core::Result<ArtifactRow> read_artifact(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<ArtifactRow>> select_all_artifact(core::db::Database &db);
[[nodiscard]] std::string digest(const ArtifactRow &row);

// artifact_seen (append-only: an upsert of an existing key is a no-op, never an update)
[[nodiscard]] core::Status insert(core::db::Database &db, const ArtifactSeenRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const ArtifactSeenRow &row);
[[nodiscard]] core::Result<ArtifactSeenRow> read_artifact_seen(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<ArtifactSeenRow>>
select_all_artifact_seen(core::db::Database &db);
[[nodiscard]] std::string digest(const ArtifactSeenRow &row);

// skipped_path
[[nodiscard]] core::Status insert(core::db::Database &db, const SkippedPathRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const SkippedPathRow &row);
[[nodiscard]] core::Result<SkippedPathRow> read_skipped_path(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<SkippedPathRow>>
select_all_skipped_path(core::db::Database &db);
[[nodiscard]] std::string digest(const SkippedPathRow &row);

// producer
[[nodiscard]] core::Status insert(core::db::Database &db, const ProducerRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const ProducerRow &row);
[[nodiscard]] core::Result<ProducerRow> read_producer(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<ProducerRow>> select_all_producer(core::db::Database &db);
[[nodiscard]] std::string digest(const ProducerRow &row);

} // namespace atx::engine::research::store
