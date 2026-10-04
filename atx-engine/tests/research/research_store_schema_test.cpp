// The printed schema (atx.store-schema/v1) against the hand-written group fixtures, and the
// group DDL on the vendored SQLite.

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/db/sqlite.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/research_store_test_support.hpp"

namespace store = atx::engine::research::store;
namespace db = atx::core::db;

TEST(ResearchStoreSchema, GroupJsonEqualsFixture) {
  const std::string core = store::group_schema_json(store::core_group());
  const std::string cache = store::group_schema_json(store::cache_group());
  EXPECT_EQ(core, store::test::read_bytes(store::test::fixture("schema/catalog_core.json")));
  EXPECT_EQ(cache, store::test::read_bytes(store::test::fixture("schema/cache.json")));
  EXPECT_EQ(store::group_schema_json(store::test::kToyGroup),
            store::test::read_bytes(store::test::fixture("schema/toy.json")));
  EXPECT_EQ(core.find('\r'), std::string::npos);
  EXPECT_EQ(core.back(), '\n');
}

TEST(ResearchStoreSchema, DdlAppliesOnVendoredSqlite) {
  for (const store::GroupOps *group : {&store::core_group(), &store::cache_group()}) {
    // The full current-version DDL of every table...
    db::Database full = store::test::memory_db();
    std::vector<std::string> names;
    for (const store::TableSchema &table : group->tables()) {
      store::test::exec_all(full, table.ddl);
      names.push_back(table.name);
    }
    // ...and the migration steps from version 1 build the same tables.
    db::Database stepped = store::test::memory_db();
    for (atx::i32 v = 1; v <= group->version; ++v) {
      store::test::exec_all(stepped, group->steps(v));
    }
    for (const std::string &name : names) {
      const std::string count =
          "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = '" + name + "'";
      EXPECT_EQ(store::test::scalar_int(full, count), 1) << group->name << "." << name;
      EXPECT_EQ(store::test::scalar_int(stepped, count), 1) << group->name << "." << name;
      EXPECT_EQ(store::test::scalar_text(full, "SELECT sql FROM sqlite_schema WHERE name = '" +
                                                   name + "'"),
                store::test::scalar_text(stepped, "SELECT sql FROM sqlite_schema WHERE name = '" +
                                                      name + "'"));
    }
  }
}

TEST(ResearchStoreSchema, DbDocumentHasEveryGroupInOrder) {
  const store::GroupOps *const two[] = {&store::core_group(), &store::test::kPlainCatalogGroup};
  const std::string text = store::schema_json(store::DbKind::Catalog, two);
  const nlohmann::ordered_json doc = nlohmann::ordered_json::parse(text);
  std::vector<std::string> keys;
  for (const auto &item : doc.items()) {
    keys.push_back(item.key());
  }
  EXPECT_EQ(keys, (std::vector<std::string>{"schema", "db", "application_id", "user_version",
                                            "page_size", "groups"}));
  EXPECT_EQ(doc.at("schema"), "atx.store-schema/v1");
  EXPECT_EQ(doc.at("db"), "catalog");
  EXPECT_EQ(doc.at("application_id"), 0x41545843);
  EXPECT_EQ(doc.at("user_version"), 1);
  EXPECT_EQ(doc.at("page_size"), 8192);
  ASSERT_EQ(doc.at("groups").size(), 2U);
  EXPECT_EQ(doc.at("groups")[0], nlohmann::ordered_json::parse(
                                     store::group_schema_json(store::core_group())));
  EXPECT_EQ(doc.at("groups")[1].at("name"), "plain_group");
  EXPECT_EQ(text.back(), '\n');
  // The given order is kept.
  const store::GroupOps *const reversed[] = {&store::test::kPlainCatalogGroup,
                                             &store::core_group()};
  const nlohmann::ordered_json other =
      nlohmann::ordered_json::parse(store::schema_json(store::DbKind::Catalog, reversed));
  EXPECT_EQ(other.at("groups")[0].at("name"), "plain_group");
  EXPECT_EQ(other.at("groups")[1].at("name"), "catalog_core");
  // A cache document carries the cache application id.
  const store::GroupOps *const cache[] = {&store::cache_group()};
  EXPECT_EQ(nlohmann::ordered_json::parse(store::schema_json(store::DbKind::Cache, cache))
                .at("application_id"),
            0x4154584B);
}
