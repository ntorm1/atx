// The group `catalog_core` and the shared store_info table: the only TU that instantiates
// the descriptor templates for them (ops_core.hpp declares what it defines). Nothing else
// lives here, so editing a core descriptor recompiles this file alone.

#include "atx/engine/research/store/ops_core.hpp"

#include <string>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/rows_core.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/store/detail/table_ops.hpp"
#include "research/store/tables_core.hpp"

namespace atx::engine::research::store {

namespace {

constexpr GroupOps kCoreGroup =
    group_ops<kStoreInfoTable, kCatalogRunTable, kArtifactTable, kArtifactSeenTable,
              kSkippedPathTable, kProducerTable>("catalog_core", DbKind::Catalog, 1);

} // namespace

const GroupOps &core_group() noexcept { return kCoreGroup; }

// One block of the five per-table operations (ops_core.hpp). The SQL text of insert /
// upsert is built once per process (thread-safe static) and keys the statement cache.
#define ATX_RESEARCH_STORE_TABLE_OPS(Row, kTable, suffix)                                      \
  core::Status insert(core::db::Database &db, const Row &row) {                                \
    static const std::string sql = insert_sql(kTable);                                         \
    return execute_row(db, sql, kTable, row);                                                  \
  }                                                                                            \
  core::Status upsert(core::db::Database &db, const Row &row) {                                \
    static const std::string sql = upsert_sql(kTable);                                         \
    return execute_row(db, sql, kTable, row);                                                  \
  }                                                                                            \
  core::Result<Row> read_##suffix(const core::db::Statement &stmt) { return read(stmt, kTable); } \
  core::Result<std::vector<Row>> select_all_##suffix(core::db::Database &db) {                 \
    return select_all(db, kTable);                                                             \
  }                                                                                            \
  std::string digest(const Row &row) { return row_digest(kTable, row); }

ATX_RESEARCH_STORE_TABLE_OPS(StoreInfoRow, kStoreInfoTable, store_info)
ATX_RESEARCH_STORE_TABLE_OPS(CatalogRunRow, kCatalogRunTable, catalog_run)
ATX_RESEARCH_STORE_TABLE_OPS(ArtifactRow, kArtifactTable, artifact)
ATX_RESEARCH_STORE_TABLE_OPS(ArtifactSeenRow, kArtifactSeenTable, artifact_seen)
ATX_RESEARCH_STORE_TABLE_OPS(SkippedPathRow, kSkippedPathTable, skipped_path)
ATX_RESEARCH_STORE_TABLE_OPS(ProducerRow, kProducerTable, producer)

#undef ATX_RESEARCH_STORE_TABLE_OPS

} // namespace atx::engine::research::store
