// open_store / open_cache: create, reopen, migrate, refusals, the stored schema document,
// a store made by Python's SQLite, and the catalog digest.

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/db/connection.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/rows_cache.hpp"
#include "atx/engine/research/store/rows_core.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/research_store_test_support.hpp"

namespace store = atx::engine::research::store;
namespace db = atx::core::db;
using atx::core::ErrorCode;
using store::test::sha;

namespace {

store::RecordRow record(std::string kind, char key) {
  store::RecordRow row;
  row.root = "";
  row.kind = std::move(kind);
  row.key_sha256 = sha(key);
  row.key = R"({"a":1})";
  row.body = R"({"z":[1.0,null]})";
  row.body_bytes = 16;
  row.content_sha256 = sha('c');
  return row;
}

std::optional<std::string> store_info(db::Database &d, const std::string &key) {
  auto rows = store::select_all_store_info(d);
  EXPECT_TRUE(rows) << rows.error().to_string();
  if (rows) {
    for (const store::StoreInfoRow &row : *rows) {
      if (row.key == key) {
        return row.value;
      }
    }
  }
  return std::nullopt;
}

store::ArtifactRow artifact(std::string path_key, char digest, std::optional<atx::i64> bytes) {
  store::ArtifactRow row;
  row.path_key = path_key;
  row.path = std::move(path_key);
  row.sha256 = sha(digest);
  row.bytes = bytes;
  row.artifact_class = "spec";
  row.sha_source = "verified";
  row.eol = "lf";
  return row;
}

store::CatalogRunRow catalog_run(std::string id, atx::f64 seconds) {
  store::CatalogRunRow row;
  row.catalog_run_id = std::move(id);
  row.roots = "[]";
  row.started_utc = "2023-01-02T00:00:00Z";
  row.seconds = seconds;
  return row;
}

} // namespace

TEST(ResearchStoreOpen, CreateThenReopen) {
  const auto dir = store::test::temp_path("cache_dir");
  std::filesystem::create_directories(dir);
  {
    auto opened = store::open_cache(dir);
    ASSERT_TRUE(opened) << opened.error().to_string();
    ASSERT_TRUE(store::insert(*opened, record("factor", '1')));
    ASSERT_TRUE(db::checkpoint_truncate(*opened));
  }
  ASSERT_TRUE(std::filesystem::exists(dir / "index.sqlite"));
  auto reopened = store::open_cache(dir);
  ASSERT_TRUE(reopened) << reopened.error().to_string();
  auto rows = store::select_all_record(*reopened);
  ASSERT_TRUE(rows) << rows.error().to_string();
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(rows->front().kind, "factor");
  EXPECT_EQ(rows->front().body, std::optional<std::string>{R"({"z":[1.0,null]})"});
  EXPECT_EQ(store::test::scalar_int(*reopened, "PRAGMA application_id"), 0x4154584B);
  EXPECT_EQ(store::test::scalar_int(*reopened, "PRAGMA user_version"), 1);
  EXPECT_EQ(store::test::scalar_int(*reopened, "PRAGMA page_size"), 8192);
  EXPECT_EQ(store::test::scalar_text(*reopened, "PRAGMA journal_mode"), "wal");
  EXPECT_EQ(store::test::scalar_int(*reopened, "PRAGMA foreign_keys"), 1);
  // The record table CHECK: exactly one of body / body_file.
  store::RecordRow both = record("factor", '2');
  both.body_file = "objects/cc/x.json";
  auto refused = store::insert(*reopened, both);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  store::RecordRow neither = record("factor", '3');
  neither.body = std::nullopt;
  EXPECT_FALSE(store::insert(*reopened, neither));
}

TEST(ResearchStoreOpen, MigratesToyV1ToV2) {
  const auto path = store::test::temp_path("mig.sqlite").string();
  {
    const store::GroupOps *const v1[] = {&store::test::kMigGroupV1};
    auto opened = store::open_store(path, store::DbKind::Cache, v1);
    ASSERT_TRUE(opened) << opened.error().to_string();
    for (const char *key : {"b", "a"}) {
      ASSERT_TRUE(store::execute_row(*opened, store::insert_sql(store::test::kMigV1Table),
                                     store::test::kMigV1Table, store::test::MigV1Row{key, 7}));
    }
  }
  const store::GroupOps *const v2[] = {&store::test::kMigGroupV2};
  auto migrated = store::open_store(path, store::DbKind::Cache, v2);
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  EXPECT_EQ(store::test::scalar_int(*migrated, "PRAGMA user_version"), 2);
  EXPECT_EQ(store_info(*migrated, "schema_json"), store::schema_json(store::DbKind::Cache, v2));
  EXPECT_EQ(store::test::scalar_int(
                *migrated, "SELECT count(*) FROM sqlite_schema WHERE name = 'ix_mig_b'"),
            1);
  auto rows = store::select_all(*migrated, store::test::kMigV2Table);
  ASSERT_TRUE(rows) << rows.error().to_string();
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ((*rows)[0], (store::test::MigV2Row{"a", 7, std::nullopt}));
  EXPECT_EQ((*rows)[1], (store::test::MigV2Row{"b", 7, std::nullopt}));
  ASSERT_TRUE(store::execute_row(*migrated, store::insert_sql(store::test::kMigV2Table),
                                 store::test::kMigV2Table, store::test::MigV2Row{"c", 1, 0.25}));
  // A store created at v2 directly has the same columns.
  const auto fresh = store::test::temp_path("fresh.sqlite").string();
  auto created = store::open_store(fresh, store::DbKind::Cache, v2);
  ASSERT_TRUE(created) << created.error().to_string();
  const char *columns = "SELECT group_concat(name, ',') FROM pragma_table_info('mig')";
  EXPECT_EQ(store::test::scalar_text(*created, columns), "k,a,b");
  EXPECT_EQ(store::test::scalar_text(*migrated, columns), "k,a,b");
}

TEST(ResearchStoreOpen, RefusesForeignApplicationId) {
  const auto path = store::test::temp_path("catalog.sqlite").string();
  const store::GroupOps *const core[] = {&store::core_group()};
  { ASSERT_TRUE(store::open_store(path, store::DbKind::Catalog, core)); }
  const store::GroupOps *const cache[] = {&store::cache_group()};
  auto opened = store::open_store(path, store::DbKind::Cache, cache);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
  // A group of the other kind is refused before any file is opened.
  auto mixed = store::open_store(store::test::temp_path("x.sqlite").string(),
                                 store::DbKind::Catalog, cache);
  ASSERT_FALSE(mixed);
  EXPECT_EQ(mixed.error().code(), ErrorCode::InvalidArgument);
}

TEST(ResearchStoreOpen, RefusesNewerVersion) {
  const auto path = store::test::temp_path("mig.sqlite").string();
  const store::GroupOps *const v2[] = {&store::test::kMigGroupV2};
  { ASSERT_TRUE(store::open_store(path, store::DbKind::Cache, v2)); }
  const store::GroupOps *const v1[] = {&store::test::kMigGroupV1};
  auto opened = store::open_store(path, store::DbKind::Cache, v1);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code(), ErrorCode::NotImplemented);
}

TEST(ResearchStoreOpen, StoreInfoHoldsSchemaJson) {
  const auto dir = store::test::temp_path("cache_dir");
  std::filesystem::create_directories(dir);
  auto opened = store::open_cache(dir);
  ASSERT_TRUE(opened) << opened.error().to_string();
  const store::GroupOps *const cache[] = {&store::cache_group()};
  EXPECT_EQ(store_info(*opened, "schema_json"), store::schema_json(store::DbKind::Cache, cache));
  EXPECT_EQ(store_info(*opened, "db_kind"), std::optional<std::string>{"cache"});
  const auto catalog_path = store::test::temp_path("catalog.sqlite").string();
  const store::GroupOps *const core[] = {&store::core_group()};
  auto catalog = store::open_store(catalog_path, store::DbKind::Catalog, core);
  ASSERT_TRUE(catalog) << catalog.error().to_string();
  EXPECT_EQ(store_info(*catalog, "schema_json"), store::schema_json(store::DbKind::Catalog, core));
  EXPECT_EQ(store_info(*catalog, "db_kind"), std::optional<std::string>{"catalog"});
  EXPECT_EQ(store::test::scalar_int(*catalog, "PRAGMA synchronous"), 2); // FULL
}

TEST(ResearchStoreOpen, ReadsPythonCreatedFixture) {
  const auto path = store::test::temp_path("py_created.sqlite");
  std::filesystem::copy_file(store::test::fixture("py_created.sqlite"), path);
  const store::GroupOps *const core[] = {&store::core_group()};
  auto opened = store::open_store(path.string(), store::DbKind::Catalog, core);
  ASSERT_TRUE(opened) << opened.error().to_string();
  // Python assembled the same document from the committed fixture.
  EXPECT_EQ(store_info(*opened, "schema_json"), store::schema_json(store::DbKind::Catalog, core));
  auto artifacts = store::select_all_artifact(*opened);
  ASSERT_TRUE(artifacts) << artifacts.error().to_string();
  ASSERT_EQ(artifacts->size(), 2U);
  const store::ArtifactRow &payload = (*artifacts)[0]; // key order: build-equity/... first
  EXPECT_EQ(payload.path_key, "build-equity/runs/r1/payload.f64");
  EXPECT_FALSE(payload.bytes.has_value());
  EXPECT_EQ(payload.sha_source, "declared");
  EXPECT_EQ(payload.declared_by, std::optional<std::string>{"build-equity/runs/r1/manifest.json"});
  EXPECT_EQ(payload.producer_key, std::optional<std::string>{sha('c')});
  const store::ArtifactRow &spec = (*artifacts)[1];
  EXPECT_EQ(spec.path, "scripts/specs/p9/A.json");
  EXPECT_EQ(spec.bytes, std::optional<atx::i64>{120});
  EXPECT_EQ(spec.json_schema, std::optional<std::string>{"atx.spec/v1"});
  EXPECT_EQ(spec.eol, std::optional<std::string>{"lf"});
  auto producers = store::select_all_producer(*opened);
  ASSERT_TRUE(producers) << producers.error().to_string();
  ASSERT_EQ(producers->size(), 1U);
  EXPECT_EQ(producers->front().kind, "engine");
  EXPECT_FALSE(producers->front().module_name.has_value());
  auto runs = store::select_all_catalog_run(*opened);
  ASSERT_TRUE(runs) << runs.error().to_string();
  ASSERT_EQ(runs->size(), 1U);
  EXPECT_EQ(runs->front().seconds, 1.5);
  EXPECT_EQ(runs->front().files_seen, 3);
  auto seen = store::select_all_artifact_seen(*opened);
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->size(), 1U);
  auto skipped = store::select_all_skipped_path(*opened);
  ASSERT_TRUE(skipped);
  ASSERT_EQ(skipped->size(), 1U);
  EXPECT_EQ(skipped->front().reason, "outside-roots");
  auto checked = db::quick_check(*opened);
  ASSERT_TRUE(checked);
  EXPECT_EQ(*checked, "ok");
}

TEST(ResearchStoreOpen, CatalogDigestIndependentOfInsertOrder) {
  const store::GroupOps *const core[] = {&store::core_group()};
  const std::vector<store::ArtifactRow> artifacts{artifact("b/one.json", 'a', 10),
                                                  artifact("a/two.json", 'b', std::nullopt),
                                                  artifact("c/three.json", 'c', 30)};
  store::ProducerRow producer;
  producer.producer_key = sha('d');
  producer.kind = "python";
  producer.module_name = "fit_composition_weights";

  auto first = store::open_store(store::test::temp_path("first.sqlite").string(),
                                 store::DbKind::Catalog, core);
  auto second = store::open_store(store::test::temp_path("second.sqlite").string(),
                                  store::DbKind::Catalog, core);
  ASSERT_TRUE(first) << first.error().to_string();
  ASSERT_TRUE(second) << second.error().to_string();
  ASSERT_TRUE(store::insert(*first, producer));
  for (const store::ArtifactRow &row : artifacts) {
    ASSERT_TRUE(store::insert(*first, row));
  }
  for (auto it = artifacts.rbegin(); it != artifacts.rend(); ++it) {
    ASSERT_TRUE(store::insert(*second, *it));
  }
  ASSERT_TRUE(store::insert(*second, producer));
  // Volatile tables differ and do not count.
  ASSERT_TRUE(store::insert(*first, catalog_run("run-a", 1.0)));
  ASSERT_TRUE(store::insert(*second, catalog_run("run-b", 2.0)));
  ASSERT_TRUE(store::insert(*second, store::SkippedPathRow{"run-b", "x", "unreadable"}));

  auto a = store::catalog_digest(*first, core);
  auto b = store::catalog_digest(*second, core);
  ASSERT_TRUE(a) << a.error().to_string();
  ASSERT_TRUE(b) << b.error().to_string();
  EXPECT_EQ(*a, *b);
  // The digest is the documented line sequence: artifact rows in key order, then producer.
  std::string expected = "atx.catalog-digest/v1\n";
  for (const store::ArtifactRow &row : {artifacts[1], artifacts[0], artifacts[2]}) {
    expected += "artifact " + store::digest(row) + "\n";
  }
  expected += "producer " + store::digest(producer) + "\n";
  auto hex = atx::core::sha256_hex(expected);
  ASSERT_TRUE(hex);
  EXPECT_EQ(*a, *hex);
  // Any content change moves it.
  store::ArtifactRow changed = artifacts[0];
  changed.bytes = 11;
  ASSERT_TRUE(store::upsert(*second, changed));
  auto c = store::catalog_digest(*second, core);
  ASSERT_TRUE(c);
  EXPECT_NE(*a, *c);
}
