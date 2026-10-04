// Read-only catalog queries (query.hpp).

#include "atx/engine/research/store/catalog/query.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/store.hpp"

namespace atx::engine::research::store::catalog {
namespace {

using OJson = nlohmann::ordered_json;
using core::db::ColumnType;
using core::db::Database;
using core::db::Statement;

// SELECT <columns> FROM <from> ORDER BY <order>: the column list is also the printed header.
struct NamedQuery {
  std::string_view name;
  std::string_view columns;
  std::string_view from;
  std::string_view order;
};

constexpr std::string_view kPinColumns =
    "holder_path, pointer, pin_kind, target_path, target_sha256, artifact_sha256, sha_source, "
    "state";

constexpr std::array<NamedQuery, 6> kQueries{{
    {"artifacts",
     "path_key, path, sha256, bytes, class, json_schema, sha_source, declared_by, eol, "
     "producer_key",
     "artifact", "path_key"},
    {"pins", kPinColumns, "pin_status", "holder_path, pointer"},
    {"stale-pins", kPinColumns, "pin_status WHERE state IN ('stale', 'missing')",
     "holder_path, pointer"},
    {"runs",
     "run_dir, receipt_schema, source_sha, executable_sha256, argv_sha256, attempt, build_type, "
     "role_id, outcome, exit_code, git_state, wall_seconds, sampled_peak_tree_rss_bytes, "
     "file_sha256",
     "run", "run_dir"},
    {"timings",
     "path, ord, phase, run_dir, outcome, exit_code, seconds, peak_mib, executable_sha256",
     "wave_timing", "path, ord"},
    {"producers",
     "producer_key, kind, exe_sha256, git_sha, build_type, module, code_sha256, receipt_sha256",
     "producer", "producer_key"},
}};

[[nodiscard]] std::vector<std::string> split_columns(std::string_view list) {
  std::vector<std::string> out;
  usize start = 0;
  // Bounded by the list length: each pass consumes one name.
  for (;;) {
    const usize comma = list.find(',', start);
    std::string_view name = list.substr(start, comma == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : comma - start);
    while (!name.empty() && name.front() == ' ') {
      name.remove_prefix(1);
    }
    out.emplace_back(name);
    if (comma == std::string_view::npos) {
      return out;
    }
    start = comma + 1;
  }
}

// One result value as JSON by its storage class; `type` (a schema column type) refines it:
// bool -> true / false, u64 -> its unsigned bit pattern, blob -> lower-case hex.
[[nodiscard]] OJson cell_json(const Statement &stmt, i32 col, std::string_view type) {
  switch (stmt.column_type(col)) {
  case ColumnType::Integer: {
    const i64 v = stmt.column_int(col);
    if (type == "bool") {
      return OJson(v != 0);
    }
    if (type == "u64") {
      return OJson(std::bit_cast<u64>(v));
    }
    return OJson(v);
  }
  case ColumnType::Float:
    return OJson(stmt.column_double(col));
  case ColumnType::Text:
    return OJson(std::string{stmt.column_text(col)});
  case ColumnType::Blob: {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string hex;
    for (const std::byte b : stmt.column_blob(col)) {
      const auto v = std::to_integer<unsigned>(b);
      hex += kHex[(v >> 4U) & 0xFU];
      hex += kHex[v & 0xFU];
    }
    return OJson(std::move(hex));
  }
  case ColumnType::Null:
    break;
  }
  return OJson(nullptr);
}

[[nodiscard]] std::string cell_text(const Statement &stmt, i32 col) {
  const OJson v = cell_json(stmt, col, {});
  if (v.is_null()) {
    return "NULL";
  }
  return v.is_string() ? v.get<std::string>() : v.dump();
}

// Run `sql` and print each row (see run_query); `types` (optional, one per column) refines JSON.
[[nodiscard]] core::Result<std::vector<std::string>>
print_rows(Database &db, const std::string &sql, const std::vector<std::string> &names,
           const std::vector<std::string> &types, QueryFormat format) {
  ATX_TRY(Statement stmt, db.prepare(sql));
  const i32 n = stmt.column_count();
  if (n != static_cast<i32>(names.size())) {
    return core::Err(core::ErrorCode::Internal, "query column count mismatch: " + sql);
  }
  std::vector<std::string> out;
  if (format == QueryFormat::Text) {
    std::string header;
    for (usize c = 0; c < names.size(); ++c) {
      header += (c == 0 ? "" : "\t");
      header += names[c];
    }
    out.push_back(std::move(header));
  }
  // Bounded by the result's row count.
  for (;;) {
    ATX_TRY(const Statement::Step step, stmt.step());
    if (step == Statement::Step::Done) {
      return out;
    }
    if (format == QueryFormat::Json) {
      OJson row = OJson::object();
      for (i32 c = 0; c < n; ++c) {
        const auto at = static_cast<usize>(c);
        row[names[at]] = cell_json(stmt, c, at < types.size() ? types[at] : std::string{});
      }
      out.push_back(row.dump());
    } else {
      std::string line;
      for (i32 c = 0; c < n; ++c) {
        line += (c == 0 ? "" : "\t");
        line += cell_text(stmt, c);
      }
      out.push_back(std::move(line));
    }
  }
}

[[nodiscard]] core::Result<i64> count_query(Database &db, std::string_view sql,
                                            const std::optional<std::string> &holder) {
  ATX_TRY(Statement stmt, db.prepare(sql));
  if (holder) {
    ATX_TRY_VOID(stmt.bind(1, std::string_view{*holder}));
  }
  ATX_TRY(const Statement::Step step, stmt.step());
  (void)step;
  return stmt.checked_int(0);
}

[[nodiscard]] std::string holder_clause(const std::optional<std::string> &holder,
                                        std::string_view joiner) {
  return holder ? std::string{joiner} + " holder_path = ?1" : std::string{};
}

} // namespace

std::vector<std::string> query_names() {
  std::vector<std::string> out;
  for (const NamedQuery &q : kQueries) {
    out.emplace_back(q.name);
  }
  return out;
}

core::Result<std::vector<std::string>> run_query(Database &db, std::string_view name,
                                                 QueryFormat format) {
  for (const NamedQuery &q : kQueries) {
    if (q.name != name) {
      continue;
    }
    const std::string sql = "SELECT " + std::string{q.columns} + " FROM " + std::string{q.from} +
                            " ORDER BY " + std::string{q.order} + ";";
    return print_rows(db, sql, split_columns(q.columns), {}, format);
  }
  return core::Err(core::ErrorCode::InvalidArgument, "unknown query " + std::string{name});
}

core::Result<std::vector<std::string>> dump_table(Database &db, std::string_view table) {
  for (const GroupOps *group : {&core_group(), &records_group(), &cache_group()}) {
    for (const TableSchema &t : group->tables()) {
      if (t.name != table) {
        continue;
      }
      std::vector<std::string> names;
      std::vector<std::string> types;
      std::string columns;
      for (const ColumnSchema &c : t.columns) {
        columns += columns.empty() ? "" : ", ";
        columns += c.name;
        names.push_back(c.name);
        types.push_back(c.type);
      }
      std::string order;
      for (const std::string &k : t.key) {
        order += order.empty() ? "" : ", ";
        order += k;
      }
      const std::string sql =
          "SELECT " + columns + " FROM " + t.name + " ORDER BY " + order + ";";
      return print_rows(db, sql, names, types, QueryFormat::Json);
    }
  }
  return core::Err(core::ErrorCode::InvalidArgument, "no table " + std::string{table});
}

core::Result<std::vector<PinCount>> pin_counts(Database &db,
                                               const std::optional<std::string> &holder) {
  const std::string sql = "SELECT pin_kind, state, count(*) FROM pin_status" +
                          holder_clause(holder, " WHERE") +
                          " GROUP BY pin_kind, state ORDER BY pin_kind, state;";
  ATX_TRY(Statement stmt, db.prepare(sql));
  if (holder) {
    ATX_TRY_VOID(stmt.bind(1, std::string_view{*holder}));
  }
  std::vector<PinCount> out;
  // Bounded by the number of (kind, state) groups.
  for (;;) {
    ATX_TRY(const Statement::Step step, stmt.step());
    if (step == Statement::Step::Done) {
      return out;
    }
    PinCount c;
    ATX_TRY(c.pin_kind, stmt.checked_text(0));
    ATX_TRY(c.state, stmt.checked_text(1));
    ATX_TRY(c.count, stmt.checked_int(2));
    out.push_back(std::move(c));
  }
}

core::Result<std::vector<PinProblem>> pin_problems(Database &db,
                                                   const std::optional<std::string> &holder) {
  const std::string sql =
      "SELECT holder_path, pointer, pin_kind, state, target_path, target_sha256, "
      "artifact_sha256 FROM pin_status WHERE state IN ('stale', 'missing')" +
      holder_clause(holder, " AND") + " ORDER BY holder_path, pointer;";
  ATX_TRY(Statement stmt, db.prepare(sql));
  if (holder) {
    ATX_TRY_VOID(stmt.bind(1, std::string_view{*holder}));
  }
  std::vector<PinProblem> out;
  // Bounded by the number of stale and missing pins.
  for (;;) {
    ATX_TRY(const Statement::Step step, stmt.step());
    if (step == Statement::Step::Done) {
      return out;
    }
    PinProblem p;
    ATX_TRY(p.holder_path, stmt.checked_text(0));
    ATX_TRY(p.pointer, stmt.checked_text(1));
    ATX_TRY(p.pin_kind, stmt.checked_text(2));
    ATX_TRY(p.state, stmt.checked_text(3));
    if (stmt.column_type(4) != ColumnType::Null) {
      ATX_TRY(p.target_path, stmt.checked_text(4));
    }
    ATX_TRY(p.target_sha256, stmt.checked_text(5));
    if (stmt.column_type(6) != ColumnType::Null) {
      ATX_TRY(p.artifact_sha256, stmt.checked_text(6));
    }
    out.push_back(std::move(p));
  }
}

core::Result<std::vector<ShaSourceCount>> artifact_counts(Database &db) {
  static constexpr std::array<std::string_view, 2> kSources{"verified", "declared"};
  std::vector<ShaSourceCount> out;
  for (const std::string_view source : kSources) {
    const std::string sql =
        "SELECT count(*) FROM artifact WHERE sha_source = '" + std::string{source} + "';";
    ATX_TRY(const i64 n, count_query(db, sql, std::nullopt));
    out.push_back(ShaSourceCount{std::string{source}, n});
  }
  return out;
}

} // namespace atx::engine::research::store::catalog
