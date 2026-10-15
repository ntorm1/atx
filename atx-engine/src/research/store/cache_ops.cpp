// The group `cache`: the only TU that instantiates the descriptor templates for its tables
// (ops_cache.hpp declares what it defines). The shared store_info table is part of the group
// (its descriptor is folded into the GroupOps here) but its per-table operations live in
// core_ops.cpp.

#include "atx/engine/research/store/ops_cache.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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

// ---- Keyed reads and conflict-free inserts (P9 lane SQL4; ops_cache.hpp) ---------------------
namespace {

// INSERT INTO <t>(...) VALUES (...) ON CONFLICT DO NOTHING;
template <TableDescriptor Tbl> [[nodiscard]] std::string insert_or_ignore_sql(const Tbl &t) {
  std::string sql = insert_sql(t);
  sql.pop_back(); // the ';'
  return sql + " ON CONFLICT DO NOTHING;";
}

// SELECT <every column> FROM <t> WHERE <k1> = ?1 AND ... [AND (<k1'>, <k2'>) > (?, ?)]
// ORDER BY <key> [LIMIT ?];  `equal` = how many leading key columns are bound for equality;
// `after` = how many key columns that follow them bound a keyset page; `limit` adds a LIMIT.
template <TableDescriptor Tbl>
[[nodiscard]] std::string select_by_key_sql(const Tbl &t, usize equal, usize after, bool limit) {
  std::string where;
  std::string page_columns;
  std::string page_params;
  usize keys = 0;
  i32 param = 0;
  detail::for_each_column(t, [&](const auto &c) {
    if (!c.has(kKey)) {
      return;
    }
    if (keys < equal) {
      where += where.empty() ? "" : " AND ";
      where += std::string{c.name} + " = ?" + std::to_string(++param);
    } else if (keys < equal + after) {
      page_columns += page_columns.empty() ? "" : ", ";
      page_columns += c.name;
      page_params += page_params.empty() ? "?" : ", ?";
      page_params += std::to_string(++param);
    }
    ++keys;
  });
  if (!page_columns.empty()) {
    where += " AND (" + page_columns + ") > (" + page_params + ")";
  }
  std::string sql = "SELECT " + detail::column_list(t) + " FROM " + std::string{t.name} +
                    " WHERE " + where + " ORDER BY " + detail::key_list(t);
  if (limit) {
    sql += " LIMIT ?" + std::to_string(++param);
  }
  return sql + ";";
}

template <TableDescriptor Tbl>
[[nodiscard]] core::Result<bool>
insert_or_ignore_row(core::db::Database &db, const std::string &sql, const Tbl &t,
                     const typename std::remove_cvref_t<Tbl>::row_type &row) {
  ATX_TRY_VOID(execute_row(db, sql, t, row));
  return core::Ok(db.changes() == 1);
}

// Resets a cached statement when the read leaves scope (an early return included), so its
// cursor never holds a read transaction past the call.
class ResetOnExit {
public:
  explicit ResetOnExit(core::db::Statement &stmt) noexcept : stmt_{stmt} {}
  ResetOnExit(const ResetOnExit &) = delete;
  ResetOnExit &operator=(const ResetOnExit &) = delete;
  ResetOnExit(ResetOnExit &&) = delete;
  ResetOnExit &operator=(ResetOnExit &&) = delete;
  ~ResetOnExit() { (void)stmt_.reset(); } // a reset reports the last step's code: not an error
private:
  core::db::Statement &stmt_;
};

// The rows `sql` (select_by_key_sql) selects with `texts` bound to ?1.. in order and, when
// `limit` is set, the limit bound last. At most `max_rows` rows are read: Err(OutOfRange) when
// another row follows them (callers pass the row bound they admit).
template <TableDescriptor Tbl>
[[nodiscard]] core::Result<std::vector<typename std::remove_cvref_t<Tbl>::row_type>>
select_rows(core::db::Database &db, const std::string &sql, const Tbl &t,
            std::span<const std::string_view> texts, std::optional<usize> limit, usize max_rows) {
  using Row = typename std::remove_cvref_t<Tbl>::row_type;
  ATX_TRY(core::db::Statement * stmt, db.prepare_cached(sql));
  const ResetOnExit reset{*stmt};
  i32 param = 0;
  for (const std::string_view text : texts) {
    ATX_TRY_VOID(stmt->bind(++param, text));
  }
  if (limit) {
    ATX_TRY_VOID(stmt->bind(++param, static_cast<i64>(*limit)));
  }
  std::vector<Row> rows;
  // Bounded: at most max_rows + 1 steps.
  for (;;) {
    ATX_TRY(const core::db::Statement::Step step, stmt->step());
    if (step == core::db::Statement::Step::Done) {
      return core::Ok(std::move(rows));
    }
    if (rows.size() >= max_rows) {
      return core::Err(core::ErrorCode::OutOfRange, std::string{t.name} + ": more than " +
                                                        std::to_string(max_rows) +
                                                        " rows for one key");
    }
    ATX_TRY(Row row, read(*stmt, t));
    rows.push_back(std::move(row));
  }
}

template <TableDescriptor Tbl>
[[nodiscard]] core::Result<std::optional<typename std::remove_cvref_t<Tbl>::row_type>>
select_one(core::db::Database &db, const std::string &sql, const Tbl &t,
           std::span<const std::string_view> texts) {
  using Row = typename std::remove_cvref_t<Tbl>::row_type;
  ATX_TRY(std::vector<Row> rows, select_rows(db, sql, t, texts, std::nullopt, 1));
  if (rows.empty()) {
    return core::Ok(std::optional<Row>{});
  }
  return core::Ok(std::optional<Row>{std::move(rows.front())});
}

} // namespace

core::Result<bool> insert_or_ignore(core::db::Database &db, const IcSignalRow &row) {
  static const std::string sql = insert_or_ignore_sql(kIcSignalTable);
  return insert_or_ignore_row(db, sql, kIcSignalTable, row);
}
core::Result<bool> insert_or_ignore(core::db::Database &db, const IcResultRow &row) {
  static const std::string sql = insert_or_ignore_sql(kIcResultTable);
  return insert_or_ignore_row(db, sql, kIcResultTable, row);
}
core::Result<bool> insert_or_ignore(core::db::Database &db, const PairKeyRow &row) {
  static const std::string sql = insert_or_ignore_sql(kPairKeyTable);
  return insert_or_ignore_row(db, sql, kPairKeyTable, row);
}
core::Result<bool> insert_or_ignore(core::db::Database &db, const PairStatRow &row) {
  static const std::string sql = insert_or_ignore_sql(kPairStatTable);
  return insert_or_ignore_row(db, sql, kPairStatTable, row);
}

core::Result<std::optional<IcSignalRow>> select_ic_signal(core::db::Database &db,
                                                          std::string_view key_sha256) {
  static const std::string sql = select_by_key_sql(kIcSignalTable, 1, 0, false);
  const std::array<std::string_view, 1> texts{key_sha256};
  return select_one(db, sql, kIcSignalTable, texts);
}
core::Result<std::optional<IcResultRow>>
select_ic_result(core::db::Database &db, std::string_view entry_dir, std::string_view id) {
  static const std::string sql = select_by_key_sql(kIcResultTable, 2, 0, false);
  const std::array<std::string_view, 2> texts{entry_dir, id};
  return select_one(db, sql, kIcResultTable, texts);
}
core::Result<std::optional<PairKeyRow>> select_pair_key(core::db::Database &db,
                                                        std::string_view key_sha256) {
  static const std::string sql = select_by_key_sql(kPairKeyTable, 1, 0, false);
  const std::array<std::string_view, 1> texts{key_sha256};
  return select_one(db, sql, kPairKeyTable, texts);
}
core::Result<std::optional<PairStatRow>>
select_pair_stat(core::db::Database &db, std::string_view key_sha256, std::string_view payload_lo,
                 std::string_view payload_hi) {
  static const std::string sql = select_by_key_sql(kPairStatTable, 3, 0, false);
  const std::array<std::string_view, 3> texts{key_sha256, payload_lo, payload_hi};
  return select_one(db, sql, kPairStatTable, texts);
}
core::Result<std::vector<PairStatRow>>
select_pair_stats_page(core::db::Database &db, std::string_view key_sha256,
                       std::string_view after_lo, std::string_view after_hi, usize limit) {
  static const std::string sql = select_by_key_sql(kPairStatTable, 1, 2, true);
  const std::array<std::string_view, 3> texts{key_sha256, after_lo, after_hi};
  return select_rows(db, sql, kPairStatTable, texts, limit, limit);
}

} // namespace atx::engine::research::store
