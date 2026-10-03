// The atx-research-store command line, in process (P9 SQL2; sql-design 3.9): exit codes, ordered
// query output, and `schema --json` as the exact text a created store keeps in
// store_info('schema_json') (what Python's generic accessor reads, ruling SQL-10).

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/research/store/catalog/catalog.hpp"
#include "atx/engine/research/store/catalog/store_cli.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/research_catalog_test_support.hpp"

namespace cat = atx::engine::research::store::catalog;
namespace store = atx::engine::research::store;
namespace t = atx::engine::research::store::catalog::test;

namespace {

struct CliRun {
  int code{};
  std::string out;
  std::string err;
};

CliRun cli(std::initializer_list<std::string> args) {
  const std::vector<std::string> argv(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  CliRun r;
  r.code = cat::run_store_cli(argv, out, err);
  r.out = out.str();
  r.err = err.str();
  return r;
}

std::vector<std::string> lines(const std::string &text) {
  std::vector<std::string> out;
  std::istringstream in{text};
  for (std::string line; std::getline(in, line);) {
    out.push_back(line);
  }
  return out;
}

std::string first_field(const std::string &line) { return line.substr(0, line.find('\t')); }

std::string store_info_value(const std::filesystem::path &db_path, store::DbKind kind,
                             const std::string &key) {
  auto db = kind == store::DbKind::Catalog
                ? cat::open_catalog(db_path.string(), store::StoreOpen::Existing)
                : store::open_cache(db_path.parent_path());
  EXPECT_TRUE(db) << db.error().to_string();
  if (!db) {
    return {};
  }
  auto rows = store::select_all_store_info(*db);
  EXPECT_TRUE(rows);
  for (const store::StoreInfoRow &row : rows ? *rows : std::vector<store::StoreInfoRow>{}) {
    if (row.key == key) {
      return row.value;
    }
  }
  return {};
}

} // namespace

TEST(StoreCli, ExitCodes) {
  const auto tree = t::tree_copy();
  const std::string db = (t::temp_dir("db") / "catalog.sqlite").string();
  const std::string classes = ATX_RESEARCH_STORE_CLASSES_FILE;
  EXPECT_EQ(cli({}).code, cat::kExitUsage);
  EXPECT_EQ(cli({"frobnicate"}).code, cat::kExitUsage);
  EXPECT_EQ(cli({"schema", "--json", "--db", "nope"}).code, cat::kExitUsage);
  EXPECT_EQ(cli({"verify", "--catalog", db}).code, cat::kExitUsage); // --pins missing
  EXPECT_EQ(cli({"--help"}).code, cat::kExitOk);
  EXPECT_NE(cli({"--help"}).out.find("SQL1-MON"), std::string::npos);
  // Readers never create a store.
  EXPECT_EQ(cli({"digest", "--catalog", db}).code, cat::kExitRefusal);
  EXPECT_FALSE(std::filesystem::exists(db));

  EXPECT_EQ(cli({"init", "--catalog", db}).code, cat::kExitOk);
  const CliRun catalog = cli({"catalog", "--catalog", db, "--root", tree.string(), "--classes",
                           classes});
  ASSERT_EQ(catalog.code, cat::kExitOk) << catalog.err;
  EXPECT_NE(catalog.out.find("catalog_digest "), std::string::npos);
  // The fixture has one missing pin (reference_daily): verify reports it, --strict refuses.
  const CliRun verify = cli({"verify", "--catalog", db, "--pins"});
  EXPECT_EQ(verify.code, cat::kExitOk) << verify.err;
  EXPECT_NE(verify.out.find("pins declared 0\n"), std::string::npos) << verify.out;
  EXPECT_NE(verify.out.find("pins missing 5\n"), std::string::npos) << verify.out;
  EXPECT_EQ(cli({"verify", "--catalog", db, "--pins", "--strict"}).code, cat::kExitRefusal);
  EXPECT_EQ(cli({"verify", "--catalog", db, "--pins", "--strict", "--spec",
                 "scripts/specs/v8/fx-tmpl.json"})
                .code,
            cat::kExitOk);
  EXPECT_EQ(cli({"quick-check", "--catalog", db}).code, cat::kExitOk);
  EXPECT_EQ(cli({"dump", "--catalog", db, "--table", "no_such_table"}).code, cat::kExitUsage);
  EXPECT_EQ(cli({"query", "nonsense", "--catalog", db}).code, cat::kExitUsage);
  // A sealed path is a refusal; an unknown class a usage error.
  EXPECT_EQ(cli({"ingest", "--catalog", db, "--root", tree.string(), "--classes", classes,
                 "--class", "run-receipt", "--path",
                 "build-equity/waves/fx-w1/role-2023-2024/bad.json"})
                .code,
            cat::kExitRefusal);
  EXPECT_EQ(cli({"ingest", "--catalog", db, "--root", tree.string(), "--classes", classes,
                 "--class", "no-such-class", "--path", "build-equity/trials.jsonl"})
                .code,
            cat::kExitUsage);
  // A cache index is not a catalog (foreign application_id): refusal.
  const auto cache_dir = t::temp_dir("cache");
  ASSERT_EQ(cli({"cache", "init", cache_dir.string()}).code, cat::kExitOk);
  EXPECT_EQ(cli({"digest", "--catalog", (cache_dir / "index.sqlite").string()}).code,
            cat::kExitRefusal);
  EXPECT_EQ(cli({"cache", "init", (cache_dir / "missing").string()}).code, cat::kExitRefusal);
  EXPECT_EQ(cli({"cache", "prune", "--base", cache_dir.string()}).code, cat::kExitOk);
  // An unreadable class registry is an error.
  EXPECT_EQ(cli({"catalog", "--catalog", db, "--root", tree.string(), "--classes",
                 (cache_dir / "absent.json").string()})
                .code,
            cat::kExitError);
}

TEST(StoreCli, QueryRowsOrderedByKey) {
  const auto tree = t::tree_copy();
  const std::string db = (t::temp_dir("db") / "catalog.sqlite").string();
  ASSERT_EQ(cli({"catalog", "--catalog", db, "--root", tree.string(), "--classes",
                 ATX_RESEARCH_STORE_CLASSES_FILE})
                .code,
            cat::kExitOk);
  for (const char *name : {"artifacts", "pins", "runs", "timings", "producers"}) {
    const CliRun q = cli({"query", name, "--catalog", db});
    ASSERT_EQ(q.code, cat::kExitOk) << name << q.err;
    std::vector<std::string> rows = lines(q.out);
    ASSERT_GE(rows.size(), 2U) << name;
    rows.erase(rows.begin()); // header
    std::vector<std::string> keys;
    for (const std::string &row : rows) {
      keys.push_back(first_field(row));
    }
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end())) << name;
  }
  const CliRun artifacts = cli({"query", "artifacts", "--catalog", db});
  EXPECT_EQ(lines(artifacts.out).front().rfind("path_key\tpath\tsha256\t", 0), 0U);
  const CliRun json = cli({"query", "runs", "--catalog", db, "--json"});
  ASSERT_EQ(json.code, cat::kExitOk);
  EXPECT_EQ(json.out.rfind(R"({"run_dir":"build-equity/fx-nav-run1",)", 0), 0U) << json.out;
  const CliRun dump = cli({"dump", "--catalog", db, "--table", "trial_line"});
  ASSERT_EQ(dump.code, cat::kExitOk);
  const std::vector<std::string> dumped = lines(dump.out);
  ASSERT_EQ(dumped.size(), 5U);
  EXPECT_NE(dumped[0].find(R"("seq":1,)"), std::string::npos);
  EXPECT_NE(dumped[4].find(R"("seq":5,)"), std::string::npos);
}

TEST(StoreCli, SchemaJsonEqualsStoreInfo) {
  const auto dir = t::temp_dir("db");
  const auto db = dir / "catalog.sqlite";
  ASSERT_EQ(cli({"init", "--catalog", db.string()}).code, cat::kExitOk);
  const CliRun schema = cli({"schema", "--json", "--db", "catalog"});
  ASSERT_EQ(schema.code, cat::kExitOk);
  EXPECT_EQ(schema.out, store_info_value(db, store::DbKind::Catalog, "schema_json"));
  EXPECT_EQ(store_info_value(db, store::DbKind::Catalog, "db_kind"), "catalog");

  const auto cache_dir = t::temp_dir("cache");
  ASSERT_EQ(cli({"cache", "init", cache_dir.string()}).code, cat::kExitOk);
  const CliRun cache_schema = cli({"schema", "--json", "--db", "cache"});
  ASSERT_EQ(cache_schema.code, cat::kExitOk);
  EXPECT_EQ(cache_schema.out,
            store_info_value(cache_dir / "index.sqlite", store::DbKind::Cache, "schema_json"));
}
