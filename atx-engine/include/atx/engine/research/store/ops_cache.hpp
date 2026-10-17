#pragma once

// atx::engine::research::store -- non-template operations of the group `cache`. Defined
// in src/research/store/cache_ops.cpp, the group's only instantiating TU. Same contract per
// table as ops_core.hpp (insert / upsert / read_X / select_all_X / digest). The group also
// holds the shared store_info table, whose operations are declared in ops_core.hpp.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
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

// ---- Keyed reads and conflict-free inserts of the IC and pair tables (P9 lane SQL4) ----------
// The IC exe's cache indexes (atx-impl strategy_ic_cache_index.hpp) look rows up by key and
// insert without replacing. Every statement is the connection's cached prepared statement and is
// reset before the call returns, so no read transaction outlives the call (sql-design 3.3). Reads
// are ORDER BY the primary key (3.5).
//
// insert_or_ignore: INSERT ... ON CONFLICT DO NOTHING (a primary-key conflict only; every other
// constraint still fails the statement). true when the row was inserted, false when a row with
// its key exists: the caller reads that row and compares it.
[[nodiscard]] core::Result<bool> insert_or_ignore(core::db::Database &db, const IcSignalRow &row);
[[nodiscard]] core::Result<bool> insert_or_ignore(core::db::Database &db, const IcResultRow &row);
[[nodiscard]] core::Result<bool> insert_or_ignore(core::db::Database &db, const PairKeyRow &row);
[[nodiscard]] core::Result<bool> insert_or_ignore(core::db::Database &db, const PairStatRow &row);

// The row with this primary key (nullopt: none).
[[nodiscard]] core::Result<std::optional<IcSignalRow>>
select_ic_signal(core::db::Database &db, std::string_view key_sha256);
[[nodiscard]] core::Result<std::optional<IcResultRow>>
select_ic_result(core::db::Database &db, std::string_view entry_dir, std::string_view id);
[[nodiscard]] core::Result<std::optional<PairKeyRow>>
select_pair_key(core::db::Database &db, std::string_view key_sha256);
[[nodiscard]] core::Result<std::optional<PairStatRow>>
select_pair_stat(core::db::Database &db, std::string_view key_sha256, std::string_view payload_lo,
                 std::string_view payload_hi);
// One page of the pair_stat rows of `key_sha256` whose (payload_lo, payload_hi) is above
// (after_lo, after_hi) ("" and "" for the first page), in key order, at most `limit` rows (a
// keyset page: memory stays bounded by `limit` whatever the key holds).
[[nodiscard]] core::Result<std::vector<PairStatRow>>
select_pair_stats_page(core::db::Database &db, std::string_view key_sha256,
                       std::string_view after_lo, std::string_view after_hi, usize limit);

} // namespace atx::engine::research::store
