#pragma once

// atx::engine::research::store -- the operation templates over a table descriptor
// (ruling SQL-5, sql-design section 3.6). PRIVATE header, same includers as table.hpp.
//
// Every operation is a fold over the descriptor's columns (std::index_sequence, no
// recursion). SQL text is built at run time with std::string; nothing about strings is
// constexpr. The exact DDL grammar (the committed schema fixtures are written from it):
//
//   CREATE TABLE <t>(<col>, <col>, PRIMARY KEY(<k1>, <k2>)[, CHECK(<table check>)]) STRICT,
//     WITHOUT ROWID;                                          (one line, columns joined ", ")
//   <col>  = <name> <INTEGER|REAL|TEXT|BLOB>[ NOT NULL][ CHECK(<expr>)]
//   <expr> = the type check, then " AND <name> IN ('a', 'b')" for an `allowed` list:
//     Bool     <c> IN (0, 1)
//     Sha256   length(<c>) = 64 AND <c> NOT GLOB '*[^0-9a-f]*'
//     RelPath  instr(<c>, '\') = 0 AND substr(<c>, 1, 1) <> '/' AND ('/' || <c> || '/') NOT
//              GLOB '*/../*'                  (no backslash, no leading '/', no '..' segment)
//     Json     json_valid(<c>)
//   CREATE INDEX ix_<t>_<c> ON <t>(<c>);                      per kIndexed column, declared order
//   CREATE TRIGGER <t>_no_update BEFORE UPDATE ON <t> BEGIN SELECT RAISE(ABORT, '<t> is
//     append-only'); END;                                     append-only tables, then
//   CREATE TRIGGER <t>_no_delete BEFORE DELETE ON <t> BEGIN SELECT RAISE(ABORT, '<t> is
//     append-only'); END;
//   ALTER TABLE <t> ADD COLUMN <col>;                         migration step of a later column
//
// Real values: SQLite stores NaN as NULL and a REAL -0.0 as integer 0 (read back +0.0), so
// bind() refuses both (Err(InvalidArgument)) instead of storing a different value.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/digest.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/store/detail/table.hpp"

namespace atx::engine::research::store {
namespace detail {

[[nodiscard]] constexpr std::string_view sql_name(Sql type) noexcept {
  switch (type) {
  case Sql::Int:
    return "int";
  case Sql::Bool:
    return "bool";
  case Sql::Real:
    return "real";
  case Sql::U64:
    return "u64";
  case Sql::Text:
    return "text";
  case Sql::Sha256:
    return "sha256";
  case Sql::RelPath:
    return "relpath";
  case Sql::Json:
    return "json";
  case Sql::Blob:
    return "blob";
  }
  return "invalid"; // unreachable for valid enumerators
}

[[nodiscard]] constexpr std::string_view storage_name(Sql type) noexcept {
  switch (type) {
  case Sql::Int:
  case Sql::Bool:
  case Sql::U64:
    return "INTEGER";
  case Sql::Real:
    return "REAL";
  case Sql::Text:
  case Sql::Sha256:
  case Sql::RelPath:
  case Sql::Json:
    return "TEXT";
  case Sql::Blob:
    return "BLOB";
  }
  return "BLOB"; // unreachable for valid enumerators
}

// The type-derived CHECK expression of column `c` (empty when the type has none).
[[nodiscard]] inline std::string type_check(Sql type, std::string_view c) {
  const std::string n{c};
  switch (type) {
  case Sql::Bool:
    return n + " IN (0, 1)";
  case Sql::Sha256:
    return "length(" + n + ") = 64 AND " + n + " NOT GLOB '*[^0-9a-f]*'";
  case Sql::RelPath:
    return "instr(" + n + ", '\\') = 0 AND substr(" + n + ", 1, 1) <> '/' AND ('/' || " + n +
           " || '/') NOT GLOB '*/../*'";
  case Sql::Json:
    return "json_valid(" + n + ")";
  case Sql::Int:
  case Sql::Real:
  case Sql::U64:
  case Sql::Text:
  case Sql::Blob:
    return {};
  }
  return {};
}

// "<name> <TYPE>[ NOT NULL][ CHECK(<expr>)]".
[[nodiscard]] inline std::string column_definition(std::string_view name, Sql type,
                                                   bool nullable,
                                                   std::span<const std::string_view> allowed) {
  std::string out{name};
  out += ' ';
  out += storage_name(type);
  if (!nullable) {
    out += " NOT NULL";
  }
  std::string check = type_check(type, name);
  if (!allowed.empty()) {
    if (!check.empty()) {
      check += " AND ";
    }
    check += name;
    check += " IN (";
    for (usize i = 0; i < allowed.size(); ++i) {
      if (i > 0) {
        check += ", ";
      }
      check += '\'';
      check += allowed[i];
      check += '\'';
    }
    check += ')';
  }
  if (!check.empty()) {
    out += " CHECK(";
    out += check;
    out += ')';
  }
  return out;
}

template <TableDescriptor Tbl, class F> constexpr void for_each_column(const Tbl &t, F &&f) {
  [&]<usize... I>(std::index_sequence<I...>) {
    (f(std::get<I>(t.cols)), ...);
  }(std::make_index_sequence<std::remove_cvref_t<Tbl>::column_count>{});
}

// A column's schema version: its own `since`, never earlier than its table's.
template <class Col, TableDescriptor Tbl>
[[nodiscard]] constexpr i32 effective_since(const Col &c, const Tbl &t) noexcept {
  return std::max(c.opts.since, t.opts.since);
}

template <TableDescriptor Tbl> [[nodiscard]] std::string key_list(const Tbl &t) {
  std::string out;
  for_each_column(t, [&](const auto &c) {
    if (c.has(kKey)) {
      out += out.empty() ? "" : ", ";
      out += c.name;
    }
  });
  return out;
}

template <TableDescriptor Tbl> [[nodiscard]] std::string column_list(const Tbl &t) {
  std::string out;
  for_each_column(t, [&](const auto &c) {
    out += out.empty() ? "" : ", ";
    out += c.name;
  });
  return out;
}

// CREATE TABLE (columns with schema version <= up_to), its indexes and its triggers.
template <TableDescriptor Tbl>
[[nodiscard]] std::vector<std::string> create_statements(const Tbl &t, i32 up_to) {
  const std::string name{t.name};
  std::string create = "CREATE TABLE " + name + "(";
  bool first = true;
  for_each_column(t, [&](const auto &c) {
    using Col = std::remove_cvref_t<decltype(c)>;
    if (effective_since(c, t) > up_to) {
      return;
    }
    create += first ? "" : ", ";
    first = false;
    create += column_definition(c.name, Col::sql, Col::nullable, c.opts.allowed);
  });
  create += ", PRIMARY KEY(" + key_list(t) + ")";
  if (!t.opts.check.empty()) {
    create += ", CHECK(" + std::string{t.opts.check} + ")";
  }
  create += ") STRICT, WITHOUT ROWID;";
  std::vector<std::string> out{std::move(create)};
  for_each_column(t, [&](const auto &c) {
    if (c.has(kIndexed) && effective_since(c, t) <= up_to) {
      const std::string column{c.name};
      out.push_back("CREATE INDEX ix_" + name + "_" + column + " ON " + name + "(" + column +
                    ");");
    }
  });
  if (t.opts.append_only) {
    const std::string raise = " BEGIN SELECT RAISE(ABORT, '" + name + " is append-only'); END;";
    out.push_back("CREATE TRIGGER " + name + "_no_update BEFORE UPDATE ON " + name + raise);
    out.push_back("CREATE TRIGGER " + name + "_no_delete BEFORE DELETE ON " + name + raise);
  }
  return out;
}

// Bind one value of kind T at 1-based `index`.
template <Sql T, class V>
[[nodiscard]] core::Status bind_value(core::db::Statement &stmt, i32 index, const V &value,
                                      std::string_view table, std::string_view column) {
  if constexpr (T == Sql::Int) {
    return stmt.bind(index, value);
  } else if constexpr (T == Sql::Bool) {
    return stmt.bind(index, static_cast<i64>(value ? 1 : 0));
  } else if constexpr (T == Sql::Real) {
    if (std::isnan(value) || (value == 0.0 && std::signbit(value))) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       std::string{table} + "." + std::string{column} +
                           ": a REAL column cannot hold NaN or -0.0 losslessly");
    }
    return stmt.bind(index, value);
  } else if constexpr (T == Sql::U64) {
    return stmt.bind(index, std::bit_cast<i64>(value));
  } else if constexpr (T == Sql::Blob) {
    return stmt.bind(index, std::span<const std::byte>{value});
  } else {
    return stmt.bind(index, std::string_view{value});
  }
}

template <class Col>
[[nodiscard]] core::Status bind_column(core::db::Statement &stmt, i32 index,
                                       std::string_view table, const Col &c,
                                       const typename Col::row_type &row) {
  const auto &member = row.*(c.member);
  if constexpr (Col::nullable) {
    if (!member.has_value()) {
      return stmt.bind_null(index);
    }
    return bind_value<Col::sql>(stmt, index, *member, table, c.name);
  } else {
    return bind_value<Col::sql>(stmt, index, member, table, c.name);
  }
}

// Read one value of kind T at 0-based `index` through the checked readers.
template <Sql T>
[[nodiscard]] core::Result<typename SqlCpp<T>::type> read_value(const core::db::Statement &stmt,
                                                                i32 index) {
  if constexpr (T == Sql::Int) {
    return stmt.checked_int(index);
  } else if constexpr (T == Sql::Bool) {
    ATX_TRY(const i64 raw, stmt.checked_int(index));
    if (raw != 0 && raw != 1) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       "bool column holds " + std::to_string(raw));
    }
    return core::Ok(raw == 1);
  } else if constexpr (T == Sql::Real) {
    return stmt.checked_double(index);
  } else if constexpr (T == Sql::U64) {
    ATX_TRY(const i64 raw, stmt.checked_int(index));
    return core::Ok(std::bit_cast<u64>(raw));
  } else if constexpr (T == Sql::Blob) {
    return stmt.checked_blob(index);
  } else {
    return stmt.checked_text(index);
  }
}

template <class Col>
[[nodiscard]] core::Status read_column(const core::db::Statement &stmt, i32 index,
                                       std::string_view table, const Col &c,
                                       typename Col::row_type &row) {
  if constexpr (Col::nullable) {
    if (stmt.column_type(index) == core::db::ColumnType::Null) {
      (row.*(c.member)).reset();
      return core::Ok();
    }
  }
  auto value = read_value<Col::sql>(stmt, index);
  if (!value) {
    return core::Err(value.error().code(), std::string{table} + "." + std::string{c.name} +
                                               ": " + value.error().message());
  }
  row.*(c.member) = std::move(*value);
  return core::Ok();
}

template <Sql T, class V>
void encode_value(DigestStream &stream, std::string_view column, const V &value) {
  if constexpr (T == Sql::Int) {
    stream.add_int(column, value);
  } else if constexpr (T == Sql::Bool) {
    stream.add_bool(column, value);
  } else if constexpr (T == Sql::Real) {
    stream.add_real(column, value);
  } else if constexpr (T == Sql::U64) {
    stream.add_u64(column, value);
  } else if constexpr (T == Sql::Blob) {
    stream.add_blob(column, std::span<const std::byte>{value});
  } else {
    stream.add_text(column, value);
  }
}

template <class Col>
void encode_column(DigestStream &stream, const Col &c, const typename Col::row_type &row) {
  const auto &member = row.*(c.member);
  if constexpr (Col::nullable) {
    if (!member.has_value()) {
      stream.add_null(c.name);
      return;
    }
    encode_value<Col::sql>(stream, c.name, *member);
  } else {
    encode_value<Col::sql>(stream, c.name, member);
  }
}

} // namespace detail

// ---------------------------------------------------------------------------------------
//  Operations over one descriptor
// ---------------------------------------------------------------------------------------

// The DDL creating the table at its current version (every column inline).
template <TableDescriptor Tbl> [[nodiscard]] std::vector<std::string> ddl(const Tbl &t) {
  return detail::create_statements(t, std::numeric_limits<i32>::max());
}

// The migration step to schema version `v`: the table's DDL (columns of version <= v) when
// the table is created at v; ALTER TABLE ADD COLUMN (+ its index) per column added at v;
// nothing otherwise.
template <TableDescriptor Tbl>
[[nodiscard]] std::vector<std::string> ddl_since(const Tbl &t, i32 v) {
  if (t.opts.since == v) {
    return detail::create_statements(t, v);
  }
  std::vector<std::string> out;
  if (t.opts.since > v) {
    return out;
  }
  const std::string name{t.name};
  detail::for_each_column(t, [&](const auto &c) {
    using Col = std::remove_cvref_t<decltype(c)>;
    if (detail::effective_since(c, t) != v) {
      return;
    }
    out.push_back("ALTER TABLE " + name + " ADD COLUMN " +
                  detail::column_definition(c.name, Col::sql, Col::nullable, c.opts.allowed) +
                  ";");
    if (c.has(kIndexed)) {
      const std::string column{c.name};
      out.push_back("CREATE INDEX ix_" + name + "_" + column + " ON " + name + "(" + column +
                    ");");
    }
  });
  return out;
}

// INSERT INTO <t>(<c1>, <c2>) VALUES (?1, ?2);
template <TableDescriptor Tbl> [[nodiscard]] std::string insert_sql(const Tbl &t) {
  std::string params;
  for (usize i = 1; i <= std::remove_cvref_t<Tbl>::column_count; ++i) {
    params += (i == 1 ? "?" : ", ?") + std::to_string(i);
  }
  return "INSERT INTO " + std::string{t.name} + "(" + detail::column_list(t) + ") VALUES (" +
         params + ");";
}

// INSERT ... ON CONFLICT(<key>) DO UPDATE SET <c> = excluded.<c>, ... (every non-key
// column); DO NOTHING when every column is a key column.
template <TableDescriptor Tbl> [[nodiscard]] std::string upsert_sql(const Tbl &t) {
  std::string set;
  detail::for_each_column(t, [&](const auto &c) {
    if (!c.has(kKey)) {
      set += set.empty() ? "" : ", ";
      set += std::string{c.name} + " = excluded." + std::string{c.name};
    }
  });
  std::string sql = insert_sql(t);
  sql.pop_back(); // the ';'
  sql += " ON CONFLICT(" + detail::key_list(t) + ") DO ";
  sql += set.empty() ? std::string{"NOTHING;"} : "UPDATE SET " + set + ";";
  return sql;
}

// SELECT <c1>, <c2> FROM <t> ORDER BY <key>;
template <TableDescriptor Tbl> [[nodiscard]] std::string select_all_sql(const Tbl &t) {
  return "SELECT " + detail::column_list(t) + " FROM " + std::string{t.name} + " ORDER BY " +
         detail::key_list(t) + ";";
}

// Bind every column of `row` to parameters 1..N in declared order.
template <TableDescriptor Tbl>
[[nodiscard]] core::Status bind(core::db::Statement &stmt, const Tbl &t,
                                const typename std::remove_cvref_t<Tbl>::row_type &row) {
  core::Status status = core::Ok();
  [&]<usize... I>(std::index_sequence<I...>) {
    (void)((status = detail::bind_column(stmt, static_cast<i32>(I + 1), t.name,
                                         std::get<I>(t.cols), row))
               .has_value() &&
           ...);
  }(std::make_index_sequence<std::remove_cvref_t<Tbl>::column_count>{});
  return status;
}

// Read the current row (result columns 0..N-1 in declared order). Err(InvalidArgument)
// naming table and column for a NULL in a non-optional member or a wrong storage class, or
// when the statement's column count is not the table's.
template <TableDescriptor Tbl>
[[nodiscard]] core::Result<typename std::remove_cvref_t<Tbl>::row_type>
read(const core::db::Statement &stmt, const Tbl &t) {
  using Row = typename std::remove_cvref_t<Tbl>::row_type;
  constexpr usize n = std::remove_cvref_t<Tbl>::column_count;
  if (stmt.column_count() != static_cast<i32>(n)) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     std::string{t.name} + ": statement has " +
                         std::to_string(stmt.column_count()) + " columns, table has " +
                         std::to_string(n));
  }
  Row row{};
  core::Status status = core::Ok();
  [&]<usize... I>(std::index_sequence<I...>) {
    (void)((status = detail::read_column(stmt, static_cast<i32>(I), t.name, std::get<I>(t.cols),
                                         row))
               .has_value() &&
           ...);
  }(std::make_index_sequence<n>{});
  if (!status) {
    return core::Err(std::move(status).error());
  }
  return core::Ok(std::move(row));
}

// The record-digest lines of `row`: "row <t>@<version>" and every non-volatile column.
template <TableDescriptor Tbl>
void encode(DigestStream &stream, const Tbl &t,
            const typename std::remove_cvref_t<Tbl>::row_type &row) {
  stream.begin_row(t.name, t.opts.version);
  detail::for_each_column(t, [&](const auto &c) {
    if (!c.has(kVolatile)) {
      detail::encode_column(stream, c, row);
    }
  });
}

// atx.record-digest/v1 of one row (lower-case hex).
template <TableDescriptor Tbl>
[[nodiscard]] std::string row_digest(const Tbl &t,
                                     const typename std::remove_cvref_t<Tbl>::row_type &row) {
  DigestStream stream;
  encode(stream, t, row);
  return stream.finish();
}

// The schema object of the table (no JSON type: store.cpp prints it).
template <TableDescriptor Tbl> [[nodiscard]] TableSchema table_schema(const Tbl &t) {
  TableSchema out;
  out.name = std::string{t.name};
  out.version = t.opts.version;
  out.since = t.opts.since;
  out.append_only = t.opts.append_only;
  out.is_volatile = t.opts.volatile_table;
  detail::for_each_column(t, [&](const auto &c) {
    using Col = std::remove_cvref_t<decltype(c)>;
    if (c.has(kKey)) {
      out.key.emplace_back(c.name);
    }
    ColumnSchema column;
    column.name = std::string{c.name};
    column.type = std::string{detail::sql_name(Col::sql)};
    column.nullable = Col::nullable;
    column.indexed = c.has(kIndexed);
    column.is_volatile = c.has(kVolatile);
    column.since = detail::effective_since(c, t);
    column.allowed.assign(c.opts.allowed.begin(), c.opts.allowed.end());
    out.columns.push_back(std::move(column));
  });
  out.ddl = ddl(t);
  return out;
}

// Insert / upsert one row through the connection's statement cache.
template <TableDescriptor Tbl>
[[nodiscard]] core::Status execute_row(core::db::Database &db, const std::string &sql,
                                       const Tbl &t,
                                       const typename std::remove_cvref_t<Tbl>::row_type &row) {
  ATX_TRY(core::db::Statement * stmt, db.prepare_cached(sql));
  ATX_TRY_VOID(bind(*stmt, t, row));
  ATX_TRY(const core::db::Statement::Step step, stmt->step());
  (void)step; // an INSERT returns no rows: Done
  return core::Ok();
}

// Every row in key order.
template <TableDescriptor Tbl>
[[nodiscard]] core::Result<std::vector<typename std::remove_cvref_t<Tbl>::row_type>>
select_all(core::db::Database &db, const Tbl &t) {
  using Row = typename std::remove_cvref_t<Tbl>::row_type;
  ATX_TRY(core::db::Statement stmt, db.prepare(select_all_sql(t)));
  std::vector<Row> rows;
  // Bounded by the table's row count: step() reaches Done after the last row.
  for (;;) {
    ATX_TRY(const core::db::Statement::Step step, stmt.step());
    if (step == core::db::Statement::Step::Done) {
      return core::Ok(std::move(rows));
    }
    ATX_TRY(Row row, read(stmt, t));
    rows.push_back(std::move(row));
  }
}

// Append "<t> <row digest>" for every row in key order (nothing for a volatile table).
template <TableDescriptor Tbl>
[[nodiscard]] core::Status digest_table(core::db::Database &db, const Tbl &t,
                                        DigestStream &stream) {
  if (t.opts.volatile_table) {
    return core::Ok();
  }
  ATX_TRY(core::db::Statement stmt, db.prepare(select_all_sql(t)));
  // Bounded by the table's row count: step() reaches Done after the last row.
  for (;;) {
    ATX_TRY(const core::db::Statement::Step step, stmt.step());
    if (step == core::db::Statement::Step::Done) {
      return core::Ok();
    }
    ATX_TRY(const auto row, read(stmt, t));
    stream.add_entry(t.name, row_digest(t, row));
  }
}

// ---------------------------------------------------------------------------------------
//  Groups: the descriptors folded into a GroupOps of plain function pointers
// ---------------------------------------------------------------------------------------

namespace detail {

template <const auto &...Tables> std::vector<TableSchema> tables_thunk() {
  return std::vector<TableSchema>{table_schema(Tables)...};
}

template <const auto &...Tables> std::vector<std::string> steps_thunk(i32 to_version) {
  std::vector<std::string> out;
  (
      [&] {
        for (std::string &statement : ddl_since(Tables, to_version)) {
          out.push_back(std::move(statement));
        }
      }(),
      ...);
  return out;
}

template <const auto &...Tables>
core::Status digest_thunk(core::db::Database &db, DigestStream &stream) {
  core::Status status = core::Ok();
  (void)((status = digest_table(db, Tables, stream)).has_value() && ...);
  return status;
}

inline void descriptor_error_group_version_below_one() noexcept {}
inline void descriptor_error_duplicate_table_name() noexcept {}
inline void descriptor_error_since_above_group_version() noexcept {}

template <TableDescriptor Tbl> consteval bool columns_within(const Tbl &t, i32 version) {
  bool ok = true;
  for_each_column(t, [&](const auto &c) { ok = ok && effective_since(c, t) <= version; });
  return ok;
}

} // namespace detail

// A group without views.
inline std::vector<ViewSchema> no_views() { return {}; }

// Fold the descriptors `Tables` (namespace-scope constexpr objects, in table order) into a
// GroupOps. Checked at compile time: a lower-case identifier name, version >= 1, distinct
// table names, every table and column `since` <= version. The thunks are instantiated in
// the calling TU (the group's one instantiating TU).
template <const auto &...Tables>
  requires(sizeof...(Tables) >= 1 && (TableDescriptor<decltype(Tables)> && ...))
[[nodiscard]] consteval GroupOps group_ops(std::string_view name, DbKind db, i32 version,
                                           std::vector<ViewSchema> (*views)() = &no_views) {
  constexpr usize n = sizeof...(Tables);
  const std::array<std::string_view, n> names{Tables.name...};
  const std::array<i32, n> since{Tables.opts.since...};
  if (!detail::is_identifier(name)) {
    detail::descriptor_error_name_not_lowercase_identifier();
  }
  if (version < 1) {
    detail::descriptor_error_group_version_below_one();
  }
  for (usize i = 0; i < n; ++i) {
    for (usize j = i + 1; j < n; ++j) {
      if (names[i] == names[j]) {
        detail::descriptor_error_duplicate_table_name();
      }
    }
    if (since[i] > version) {
      detail::descriptor_error_since_above_group_version();
    }
  }
  if (!(detail::columns_within(Tables, version) && ...)) {
    detail::descriptor_error_since_above_group_version();
  }
  return GroupOps{name,
                  db,
                  version,
                  &detail::tables_thunk<Tables...>,
                  &detail::steps_thunk<Tables...>,
                  &detail::digest_thunk<Tables...>,
                  views};
}

} // namespace atx::engine::research::store
