// Store open / create / migrate, the schema document and the catalog digest (store.hpp).
// No template machinery here: every group arrives as a GroupOps of function pointers.

#include "atx/engine/research/store/store.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/db/connection.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/digest.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/rows_core.hpp"

namespace atx::engine::research::store {
namespace {

using OJson = nlohmann::ordered_json;
using core::db::Database;

[[nodiscard]] u32 application_id_of(DbKind kind) noexcept {
  return kind == DbKind::Catalog ? kCatalogApplicationId : kCacheApplicationId;
}

constexpr std::string_view kStoreInfoTableName = "store_info";

// Validate the group list and return the store's schema version (the largest group version):
// complete groups of this kind with distinct names, table names distinct across groups, and
// the shared store_info table carried by exactly one group (open_store writes it).
[[nodiscard]] core::Result<i32> store_version(DbKind kind,
                                              std::span<const GroupOps *const> groups) {
  if (groups.empty()) {
    return core::Err(core::ErrorCode::InvalidArgument, "research store: no groups");
  }
  i32 version = 0;
  std::vector<std::string> tables;
  for (usize i = 0; i < groups.size(); ++i) {
    const GroupOps *const g = groups[i];
    if (g == nullptr || g->tables == nullptr || g->steps == nullptr || g->digest == nullptr ||
        g->views == nullptr || g->version < 1) {
      return core::Err(core::ErrorCode::InvalidArgument, "research store: incomplete group");
    }
    if (g->db != kind) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       "research store: group " + std::string{g->name} + " belongs to a " +
                           std::string{db_kind_name(g->db)} + " store");
    }
    for (usize j = 0; j < i; ++j) {
      if (groups[j]->name == g->name) {
        return core::Err(core::ErrorCode::InvalidArgument,
                         "research store: group " + std::string{g->name} + " given twice");
      }
    }
    for (const TableSchema &table : g->tables()) {
      if (std::find(tables.begin(), tables.end(), table.name) != tables.end()) {
        return core::Err(core::ErrorCode::InvalidArgument,
                         "research store: table " + table.name + " appears in two groups (" +
                             std::string{g->name} + " is the second; exactly one group of a " +
                             "store folds in the shared store_info descriptor)");
      }
      tables.push_back(table.name);
    }
    version = std::max(version, g->version);
  }
  if (std::find(tables.begin(), tables.end(), kStoreInfoTableName) == tables.end()) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research store: no group carries the store_info table (exactly one group "
                     "of a store must fold in the shared store_info descriptor)");
  }
  return core::Ok(version);
}

// A UTF-8 path as a filesystem path; invalid UTF-8 is Err(InvalidArgument), not a throw.
[[nodiscard]] core::Result<std::filesystem::path> path_from_utf8(std::string_view text) {
  try {
    return core::Ok(std::filesystem::path{std::u8string{text.begin(), text.end()}});
  } catch (const std::exception &) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research store: path is not valid UTF-8: " + std::string{text});
  }
}

// StoreOpen::Existing: the file must exist and hold at least the database header.
[[nodiscard]] core::Status require_existing(std::string_view path) {
  ATX_TRY(const std::filesystem::path p, path_from_utf8(path));
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  if (ec || size == 0) {
    return core::Err(core::ErrorCode::NotFound,
                     "research store: no store at " + std::string{path} +
                         " (stores are created only on request: StoreOpen::CreateIfMissing)");
  }
  return core::Ok();
}

[[nodiscard]] OJson column_json(const ColumnSchema &column) {
  OJson out = OJson::object();
  out["name"] = column.name;
  out["type"] = column.type;
  out["nullable"] = column.nullable;
  out["indexed"] = column.indexed;
  out["volatile"] = column.is_volatile;
  out["since"] = column.since;
  out["allowed"] = OJson::array();
  for (const std::string &value : column.allowed) {
    out["allowed"].push_back(value);
  }
  return out;
}

[[nodiscard]] OJson table_json(const TableSchema &table) {
  OJson out = OJson::object();
  out["name"] = table.name;
  out["version"] = table.version;
  out["since"] = table.since;
  out["append_only"] = table.append_only;
  out["volatile"] = table.is_volatile;
  out["key"] = OJson::array();
  for (const std::string &key : table.key) {
    out["key"].push_back(key);
  }
  out["columns"] = OJson::array();
  for (const ColumnSchema &column : table.columns) {
    out["columns"].push_back(column_json(column));
  }
  out["ddl"] = OJson::array();
  for (const std::string &statement : table.ddl) {
    out["ddl"].push_back(statement);
  }
  return out;
}

[[nodiscard]] OJson group_json(const GroupOps &group) {
  OJson out = OJson::object();
  out["name"] = std::string{group.name};
  out["db"] = std::string{db_kind_name(group.db)};
  out["version"] = group.version;
  out["tables"] = OJson::array();
  for (const TableSchema &table : group.tables()) {
    out["tables"].push_back(table_json(table));
  }
  out["views"] = OJson::array();
  for (const ViewSchema &view : group.views()) {
    OJson v = OJson::object();
    v["name"] = view.name;
    v["sql"] = view.sql;
    out["views"].push_back(std::move(v));
  }
  return out;
}

[[nodiscard]] std::string print(const OJson &doc) { return doc.dump(2) + "\n"; }

[[nodiscard]] std::string utf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  std::string out;
  out.reserve(text.size());
  for (const char8_t c : text) {
    out.push_back(static_cast<char>(c));
  }
  return out;
}

[[nodiscard]] core::Status apply_steps(Database &db, std::span<const GroupOps *const> groups,
                                       i32 from_version, i32 to_version) {
  for (i32 v = from_version; v <= to_version; ++v) {
    for (const GroupOps *const group : groups) {
      for (const std::string &statement : group->steps(v)) {
        ATX_TRY_VOID(db.exec(statement));
      }
    }
  }
  return core::Ok();
}

// Views follow the tables: dropped (a migration may change one) and created again.
[[nodiscard]] core::Status create_views(Database &db, std::span<const GroupOps *const> groups) {
  for (const GroupOps *const group : groups) {
    for (const ViewSchema &view : group->views()) {
      ATX_TRY_VOID(db.exec("DROP VIEW IF EXISTS " + view.name + ";"));
      ATX_TRY_VOID(db.exec(view.sql));
    }
  }
  return core::Ok();
}

[[nodiscard]] core::Result<i64> schema_objects(Database &db) {
  ATX_TRY(core::db::Statement stmt, db.prepare("SELECT count(*) FROM sqlite_schema"));
  ATX_TRY(const core::db::Statement::Step step, stmt.step());
  (void)step;
  return stmt.checked_int(0);
}

// A store already at this build's version must hold exactly this build's schema: equal versions
// with different groups or descriptors is drift (store.hpp, "Same version").
[[nodiscard]] core::Status check_no_drift(Database &db, DbKind kind,
                                          std::span<const GroupOps *const> groups, i32 version) {
  auto rows = select_all_store_info(db);
  if (!rows) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research store: schema drift check cannot read store_info: " +
                         rows.error().to_string());
  }
  const std::string expected = schema_json(kind, groups);
  for (const StoreInfoRow &row : *rows) {
    if (row.key == kSchemaJsonKey && row.value == expected) {
      return core::Ok();
    }
  }
  return core::Err(core::ErrorCode::InvalidArgument,
                   "research store: schema drift: store_info('schema_json') differs from this "
                   "build's groups at the same user_version " +
                       std::to_string(version) +
                       " (a group added or edited without a version past every version the "
                       "store has reached); rebuild the derived store or migrate with a new "
                       "version");
}

// Runs inside BEGIN IMMEDIATE: the pragmas are read again under the write lock, so a store
// another process created or migrated meanwhile is not created or migrated twice.
[[nodiscard]] core::Status create_or_migrate(Database &db, DbKind kind,
                                             std::span<const GroupOps *const> groups,
                                             const core::db::StorePolicy &policy) {
  ATX_TRY(const u32 application_id, core::db::read_application_id(db));
  ATX_TRY(const i32 found, core::db::read_user_version(db));
  const i32 version = policy.user_version;
  if (application_id == 0 && found == 0) {
    ATX_TRY(const i64 objects, schema_objects(db));
    if (objects != 0) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       "research store: the file holds a foreign schema");
    }
    ATX_TRY_VOID(apply_steps(db, groups, 1, version));
    ATX_TRY_VOID(create_views(db, groups));
    ATX_TRY_VOID(core::db::stamp_identity(db, policy));
    ATX_TRY_VOID(upsert(db, StoreInfoRow{std::string{kDbKindKey},
                                         std::string{db_kind_name(kind)}}));
    return upsert(db, StoreInfoRow{std::string{kSchemaJsonKey}, schema_json(kind, groups)});
  }
  if (application_id != policy.application_id) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research store: foreign application_id under the write lock");
  }
  if (found > version) {
    return core::Err(core::ErrorCode::NotImplemented,
                     "this build cannot read schema " + std::to_string(found));
  }
  if (found == version) {
    return check_no_drift(db, kind, groups, version); // a concurrent opener got here first
  }
  ATX_TRY_VOID(apply_steps(db, groups, found + 1, version));
  ATX_TRY_VOID(create_views(db, groups));
  ATX_TRY_VOID(core::db::stamp_identity(db, policy));
  return upsert(db, StoreInfoRow{std::string{kSchemaJsonKey}, schema_json(kind, groups)});
}

} // namespace

std::string_view db_kind_name(DbKind kind) noexcept {
  switch (kind) {
  case DbKind::Catalog:
    return "catalog";
  case DbKind::Cache:
    return "cache";
  }
  return "unknown"; // unreachable for valid enumerators
}

core::db::StorePolicy store_policy(DbKind kind, i32 user_version) noexcept {
  core::db::StorePolicy policy;
  policy.application_id = application_id_of(kind);
  policy.user_version = user_version;
  using Sync = core::db::StorePolicy::Sync;
  policy.sync = kind == DbKind::Catalog ? Sync::Full : Sync::Normal;
  policy.busy_timeout_ms = kStoreBusyTimeoutMs;
  policy.page_size = kStorePageSize;
  return policy;
}

core::Result<Database> open_store(std::string_view path, DbKind kind,
                                  std::span<const GroupOps *const> groups, StoreOpen how) {
  ATX_TRY(const i32 version, store_version(kind, groups));
  const bool may_create = how == StoreOpen::CreateIfMissing;
  if (!may_create) {
    ATX_TRY_VOID(require_existing(path));
  }
  const core::db::StorePolicy policy = store_policy(kind, version);
  const core::db::OpenMode mode =
      may_create ? core::db::OpenMode::ReadWriteCreate : core::db::OpenMode::ReadWrite;
  ATX_TRY(core::db::OpenedStore opened, core::db::open_with_policy(path, mode, policy));
  Database db = std::move(opened.db);
  if (opened.created && !may_create) {
    return core::Err(core::ErrorCode::NotFound,
                     "research store: " + std::string{path} +
                         " holds no store (created only on request: CreateIfMissing)");
  }
  if (!opened.created && opened.user_version == version) {
    ATX_TRY_VOID(check_no_drift(db, kind, groups, version));
    return core::Ok(std::move(db));
  }
  ATX_TRY_VOID(core::db::with_immediate(db, [&](Database &d) -> core::Status {
    return create_or_migrate(d, kind, groups, policy);
  }));
  return core::Ok(std::move(db));
}

core::Result<Database> open_cache(const std::filesystem::path &dir) {
  const GroupOps *const groups[] = {&cache_group()};
  return open_store(utf8(dir / std::string{kCacheIndexName}), DbKind::Cache, groups,
                    StoreOpen::Existing);
}

core::Result<Database> create_cache(const std::filesystem::path &dir) {
  const GroupOps *const groups[] = {&cache_group()};
  return open_store(utf8(dir / std::string{kCacheIndexName}), DbKind::Cache, groups,
                    StoreOpen::CreateIfMissing);
}

std::string schema_json(DbKind kind, std::span<const GroupOps *const> groups) {
  i32 version = 0;
  for (const GroupOps *const group : groups) {
    version = std::max(version, group->version);
  }
  OJson doc = OJson::object();
  doc["schema"] = std::string{kStoreSchemaId};
  doc["db"] = std::string{db_kind_name(kind)};
  doc["application_id"] = application_id_of(kind);
  doc["user_version"] = version;
  doc["page_size"] = kStorePageSize;
  doc["groups"] = OJson::array();
  for (const GroupOps *const group : groups) {
    doc["groups"].push_back(group_json(*group));
  }
  return print(doc);
}

std::string group_schema_json(const GroupOps &group) { return print(group_json(group)); }

core::Result<std::string> catalog_digest(Database &db, std::span<const GroupOps *const> groups) {
  for (const GroupOps *const group : groups) {
    if (group == nullptr || group->digest == nullptr) {
      return core::Err(core::ErrorCode::InvalidArgument, "catalog digest: incomplete group");
    }
  }
  DigestStream stream{DigestStream::Kind::Catalog};
  // One read transaction: every table is read from the same snapshot.
  ATX_TRY(core::db::Transaction txn, core::db::Transaction::begin(db));
  for (const GroupOps *const group : groups) {
    ATX_TRY_VOID(group->digest(db, stream));
  }
  ATX_TRY_VOID(txn.commit());
  return core::Ok(stream.finish());
}

} // namespace atx::engine::research::store
