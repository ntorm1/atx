// The group `cache`: the only TU that instantiates the descriptor templates for its tables
// (ops_cache.hpp declares what it defines). The shared store_info table is part of the group
// (its descriptor is folded into the GroupOps here) but its per-table operations live in
// core_ops.cpp.

#include "atx/engine/research/store/ops_cache.hpp"

#include <string>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/rows_cache.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/store/detail/table_ops.hpp"
#include "research/store/tables_cache.hpp"

namespace atx::engine::research::store {

namespace {

constexpr GroupOps kCacheGroup =
    group_ops<kStoreInfoTable, kRecordTable, kIcSignalTable, kIcResultTable, kPairKeyTable,
              kPairStatTable>("cache", DbKind::Cache, 1);

} // namespace

const GroupOps &cache_group() noexcept { return kCacheGroup; }

// One block of the five per-table operations (ops_cache.hpp); see core_ops.cpp.
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

ATX_RESEARCH_STORE_TABLE_OPS(RecordRow, kRecordTable, record)
ATX_RESEARCH_STORE_TABLE_OPS(IcSignalRow, kIcSignalTable, ic_signal)
ATX_RESEARCH_STORE_TABLE_OPS(IcResultRow, kIcResultTable, ic_result)
ATX_RESEARCH_STORE_TABLE_OPS(PairKeyRow, kPairKeyTable, pair_key)
ATX_RESEARCH_STORE_TABLE_OPS(PairStatRow, kPairStatTable, pair_stat)

#undef ATX_RESEARCH_STORE_TABLE_OPS

} // namespace atx::engine::research::store
