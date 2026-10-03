// The catalog's seal and blindness guards (P9 SQL2; sql-design 3.9, ruling SQL-7): a path with a
// standalone year token 2024-2099 is never opened, files no holder names are listed by name only,
// and field-source pins are recorded but never followed.

#include <filesystem>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/research/store/catalog/catalog.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "research/research_catalog_test_support.hpp"

namespace cat = atx::engine::research::store::catalog;
namespace t = atx::engine::research::store::catalog::test;
using atx::core::ErrorCode;

TEST(SealGuard, HasSealedYearMatchesTheBackstopRegex) {
  EXPECT_TRUE(cat::has_sealed_year("build-equity/role-2023-2024/x.json"));
  EXPECT_TRUE(cat::has_sealed_year("2024"));
  EXPECT_TRUE(cat::has_sealed_year("a/v2030b/c"));
  EXPECT_TRUE(cat::has_sealed_year("holdout_2099.json"));
  EXPECT_FALSE(cat::has_sealed_year("train-2020-2023-lo3-fields-v15/f.f64"));
  EXPECT_FALSE(cat::has_sealed_year("x12024/y")); // a digit before: not standalone
  EXPECT_FALSE(cat::has_sealed_year("x/20245"));  // a digit after: not standalone
  EXPECT_FALSE(cat::has_sealed_year("x/2100"));
  EXPECT_FALSE(cat::has_sealed_year("x/2019"));
}

TEST(SealGuard, YearTokenPathNeverOpened) {
  const auto tree = t::tree_copy();
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  // The decoy dir holds invalid JSON inside a walked wave dir: opening it would leave an
  // `unparsed` row (and fail a forced ingest). The run succeeds and lists the dir by name only.
  const cat::CatalogReport report = t::run(db, t::options(tree));
  EXPECT_FALSE(report.catalog_digest.empty());
  EXPECT_EQ(t::scalar_text(db, "SELECT reason FROM skipped_path WHERE path = "
                               "'build-equity/waves/fx-w1/role-2023-2024';"),
            "seal-name");
  EXPECT_EQ(t::scalar_int(db, "SELECT count(*) FROM artifact WHERE path LIKE '%2023-2024%';"), 0);
  EXPECT_EQ(t::scalar_int(db, "SELECT count(*) FROM skipped_path WHERE path LIKE "
                              "'%2023-2024/%';"),
            0);
  cat::IngestOneOptions one;
  one.root = tree;
  one.class_id = "run-receipt";
  one.path = "build-equity/waves/fx-w1/role-2023-2024/bad.json";
  auto refused = cat::ingest_one(db, t::registry(), one);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
}

TEST(SealGuard, OutsideRootsListedNotOpened) {
  const auto tree = t::tree_copy();
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  (void)t::run(db, t::options(tree));
  EXPECT_EQ(t::scalar_text(db, "SELECT reason FROM skipped_path WHERE path = "
                               "'build-equity/unpinned/notes.json';"),
            "outside-roots");
  EXPECT_EQ(t::scalar_int(db, "SELECT count(*) FROM artifact WHERE path LIKE '%unpinned%';"), 0);
  cat::IngestOneOptions one;
  one.root = tree / "scripts";
  one.class_id = "other";
  one.path = "../build-equity/unpinned/notes.json";
  auto refused = cat::ingest_one(db, t::registry(), one);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
}

TEST(SealGuard, FieldSourcePinsNotFollowed) {
  const auto tree = t::tree_copy();
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  (void)t::run(db, t::options(tree));
  const std::string holder = "build-equity/fx-fields/manifest.json";
  EXPECT_EQ(t::scalar_text(db, "SELECT pin_kind FROM pin WHERE holder_path = '" + holder +
                                   "' AND pointer = '/fields/0/sources/1/sha256';"),
            "field-source");
  EXPECT_EQ(t::scalar_text(db, "SELECT target_path FROM pin WHERE holder_path = '" + holder +
                                   "' AND pointer = '/fields/0/sources/1/sha256';"),
            "build-equity/fx-sources/src.json");
  EXPECT_EQ(t::pin_state(db, holder, "/fields/0/sources/1/sha256"), "unresolved");
  EXPECT_EQ(t::scalar_int(db, "SELECT count(*) FROM artifact WHERE path LIKE '%fx-sources%';"), 0);
  EXPECT_EQ(t::scalar_text(db, "SELECT reason FROM skipped_path WHERE path = "
                               "'build-equity/fx-sources/src.json';"),
            "outside-roots");
}
