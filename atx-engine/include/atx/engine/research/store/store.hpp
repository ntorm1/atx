#pragma once

// atx::engine::research::store -- the research-artifact SQLite store (P9 SQL1, contract
// K-P9-13; sql-design sections 3.3-3.7).
//
// A store is one SQLite file holding one or more table GROUPS. A group is a set of
// compile-time table descriptors (src/research/store/detail/table.hpp) folded into a
// GroupOps: plain function pointers, so this non-template layer creates, migrates, prints
// and digests any group without knowing its tables. Two kinds of store exist:
//
//   kind     application_id        synchronous   groups
//   catalog  0x41545843 ("ATXC")   FULL          catalog_core (SQL1) + catalog_records (SQL2)
//   cache    0x4154584B ("ATXK")   NORMAL        cache (SQL1); every cache index is
//                                                <cache dir>/index.sqlite
//
// Both: WAL, page_size 8192 (new files), busy_timeout 30 s, foreign_keys ON, defensive
// mode, trusted_schema OFF (core/db/connection.hpp). `user_version` is the store's schema
// version: the largest version of its groups. Every store carries the schema document it
// was created or last migrated with in store_info('schema_json'), the exact text of
// schema_json() (Python's research_store.py reads it; it never runs DDL).
//
// Header tiers: consumers include this header, rows_<group>.hpp and ops_<group>.hpp only;
// the templates and descriptors live under src/ and are instantiated in one TU per group.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/connection.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/digest.hpp"

namespace atx::engine::research::store {

enum class DbKind : u8 { Catalog, Cache };

inline constexpr std::string_view kStoreSchemaId = "atx.store-schema/v1";
inline constexpr u32 kCatalogApplicationId = 0x41545843U; // "ATXC"
inline constexpr u32 kCacheApplicationId = 0x4154584BU;   // "ATXK"
inline constexpr i32 kStorePageSize = 8192;
inline constexpr i32 kStoreBusyTimeoutMs = 30000;
inline constexpr std::string_view kCacheIndexName = "index.sqlite";
// store_info keys written by open_store.
inline constexpr std::string_view kDbKindKey = "db_kind";
inline constexpr std::string_view kSchemaJsonKey = "schema_json";

// "catalog" / "cache": the `db` value of the schema document and store_info('db_kind').
[[nodiscard]] std::string_view db_kind_name(DbKind kind) noexcept;

// One column as the schema document prints it. `type` is the descriptor's Sql name
// ("int", "bool", "real", "u64", "text", "sha256", "relpath", "json", "blob").
struct ColumnSchema {
  std::string name;
  std::string type;
  bool nullable{};
  bool indexed{};
  bool is_volatile{};
  i32 since{1};
  std::vector<std::string> allowed;
};

// One table: its descriptor's options, key columns in declared order, columns, and the
// DDL that creates it at its current version (CREATE TABLE, indexes, triggers).
struct TableSchema {
  std::string name;
  i32 version{1};
  i32 since{1};
  bool append_only{};
  bool is_volatile{};
  std::vector<std::string> key;
  std::vector<ColumnSchema> columns;
  std::vector<std::string> ddl;
};

// A view: `sql` is the complete CREATE VIEW statement.
struct ViewSchema {
  std::string name;
  std::string sql;
};

// A table group folded into plain function pointers (no templates past this point).
// `name` points at static storage. `steps(v)` is the DDL moving the group from schema
// version v - 1 to v (empty when the group has nothing at v). `digest` appends one
// "<table> <row digest>" entry per row of every non-volatile table of the group, tables
// in descriptor order, rows in key order.
struct GroupOps {
  std::string_view name;
  DbKind db{DbKind::Catalog};
  i32 version{1};
  std::vector<TableSchema> (*tables)(){nullptr};
  std::vector<std::string> (*steps)(i32 to_version){nullptr};
  core::Status (*digest)(core::db::Database &, DigestStream &){nullptr};
  std::vector<ViewSchema> (*views)(){nullptr};
};

// The section 3.3 policy of `kind` for a store of schema version `user_version`.
[[nodiscard]] core::db::StorePolicy store_policy(DbKind kind, i32 user_version) noexcept;

// Open (create or migrate) the store at `path` holding `groups`.
// @param path    UTF-8 path of the .sqlite file (UNC / network paths are refused).
// @param kind    catalog or cache; every group must be of this kind.
// @param groups  the store's groups in order (non-null, distinct names, version >= 1).
// Create (empty file): every group's steps(1..V) in version order, then every view, then
// application_id / user_version and store_info('db_kind', 'schema_json'), all in one
// BEGIN IMMEDIATE. Migrate (user_version u < V): the steps u+1..V, views dropped and
// re-created, user_version, store_info('schema_json') rewritten, in one BEGIN IMMEDIATE.
// The pragmas are re-read under the write lock, so a concurrent creator is not repeated.
// @return the open connection; Err(InvalidArgument) for bad groups, a foreign
//         application_id or a foreign non-empty file; Err(NotImplemented) for a store
//         newer than V; SQLite errors as the wrapper maps them.
[[nodiscard]] core::Result<core::db::Database> open_store(std::string_view path, DbKind kind,
                                                         std::span<const GroupOps *const> groups);

// open_store(dir / "index.sqlite", Cache, {&cache_group()}). `dir` must exist.
[[nodiscard]] core::Result<core::db::Database> open_cache(const std::filesystem::path &dir);

// The `atx.store-schema/v1` document of a store of `kind` holding `groups`:
//   {schema, db, application_id, user_version, page_size, groups: [<group object>...]}
// nlohmann ordered_json, keys in that order, dump(2) + "\n" (LF only, ASCII).
[[nodiscard]] std::string schema_json(DbKind kind, std::span<const GroupOps *const> groups);

// One group object alone: {name, db, version, tables: [{name, version, since, append_only,
// volatile, key, columns: [{name, type, nullable, indexed, volatile, since, allowed}],
// ddl}], views: [{name, sql}]}, same printing rules. The committed fixture
// tests/fixtures/research_store/schema/<group>.json holds exactly these bytes.
[[nodiscard]] std::string group_schema_json(const GroupOps &group);

// `atx.catalog-digest/v1` of the store (digest.hpp), read in one read transaction.
[[nodiscard]] core::Result<std::string> catalog_digest(core::db::Database &db,
                                                       std::span<const GroupOps *const> groups);

} // namespace atx::engine::research::store
