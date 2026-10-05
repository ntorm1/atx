#pragma once

// atx::engine::research::store -- non-template operations of the group `cache`. Defined
// in src/research/store/cache_ops.cpp, the group's only instantiating TU. Same contract per
// table as ops_core.hpp (insert / upsert / read_X / select_all_X / digest). The group also
// holds the shared store_info table, whose operations are declared in ops_core.hpp.

#include <string>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/rows_cache.hpp"
#include "atx/engine/research/store/store.hpp"

namespace atx::engine::research::store {

// Every cache index's group: store_info, record, ic_signal, ic_result, pair_key, pair_stat
// (version 1).
[[nodiscard]] const GroupOps &cache_group() noexcept;

// record
[[nodiscard]] core::Status insert(core::db::Database &db, const RecordRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const RecordRow &row);
[[nodiscard]] core::Result<RecordRow> read_record(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<RecordRow>> select_all_record(core::db::Database &db);
[[nodiscard]] std::string digest(const RecordRow &row);

// ic_signal
[[nodiscard]] core::Status insert(core::db::Database &db, const IcSignalRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const IcSignalRow &row);
[[nodiscard]] core::Result<IcSignalRow> read_ic_signal(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<IcSignalRow>> select_all_ic_signal(core::db::Database &db);
[[nodiscard]] std::string digest(const IcSignalRow &row);

// ic_result
[[nodiscard]] core::Status insert(core::db::Database &db, const IcResultRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const IcResultRow &row);
[[nodiscard]] core::Result<IcResultRow> read_ic_result(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<IcResultRow>> select_all_ic_result(core::db::Database &db);
[[nodiscard]] std::string digest(const IcResultRow &row);

// pair_key
[[nodiscard]] core::Status insert(core::db::Database &db, const PairKeyRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const PairKeyRow &row);
[[nodiscard]] core::Result<PairKeyRow> read_pair_key(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<PairKeyRow>> select_all_pair_key(core::db::Database &db);
[[nodiscard]] std::string digest(const PairKeyRow &row);

// pair_stat
[[nodiscard]] core::Status insert(core::db::Database &db, const PairStatRow &row);
[[nodiscard]] core::Status upsert(core::db::Database &db, const PairStatRow &row);
[[nodiscard]] core::Result<PairStatRow> read_pair_stat(const core::db::Statement &stmt);
[[nodiscard]] core::Result<std::vector<PairStatRow>> select_all_pair_stat(core::db::Database &db);
[[nodiscard]] std::string digest(const PairStatRow &row);

} // namespace atx::engine::research::store
