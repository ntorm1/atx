// The artifact catalog over the synthetic research tree (P9 SQL2; sql-design 3.7 / 3.9):
// row counts, digest reproducibility and order independence, pins (ok / declared / stale /
// missing / unresolved), declared vs verified payloads, unparsed JSON, line ends, idempotent
// ingest, the append-only trial ledger index and its chain-head seam, the records group schema
// fixture, the classifier, and the legacy record-store import of `cache init --import`.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/research/store/catalog/catalog.hpp"
#include "atx/engine/research/store/catalog/classify.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/store_cli.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/research_catalog_test_support.hpp"

namespace cat = atx::engine::research::store::catalog;
namespace store = atx::engine::research::store;
namespace t = atx::engine::research::store::catalog::test;
using atx::core::ErrorCode;

namespace {

constexpr std::string_view kSpec = "scripts/specs/v8/fx-base.json";
constexpr std::string_view kTemplate = "scripts/specs/v8/fx-tmpl.json";
constexpr std::string_view kManifest = "build-equity/fx-fields/manifest.json";
constexpr std::string_view kReceipt = "build-equity/fx-nav-run1/receipt.json";

struct Catalogued {
  std::filesystem::path tree;
  atx::core::db::Database db;
  cat::CatalogReport report;
};

Catalogued catalogued(bool payloads, char fill = '\0') {
  const auto tree = t::tree_copy();
  if (payloads) {
    t::make_payloads(tree, fill);
  }
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  const cat::CatalogReport report = t::run(db, t::options(tree));
  return Catalogued{tree, std::move(db), report};
}

std::string state_count(atx::core::db::Database &db, std::string_view state) {
  return std::to_string(t::scalar_int(
      db, "SELECT count(*) FROM pin_status WHERE state = '" + std::string{state} + "';"));
}

} // namespace

TEST(ResearchCatalog, RecordsSchemaJsonEqualsFixture) {
  EXPECT_EQ(store::group_schema_json(store::records_group()),
            t::read_bytes(t::fixture("schema/catalog_records.json")));
}

TEST(ResearchCatalog, IngestsSyntheticTree) {
  Catalogued c = catalogued(true);
  struct Count {
    std::string_view table;
    atx::i64 rows;
  };
  const Count expected[] = {
      {"artifact", 27},      {"artifact_seen", 27},  {"skipped_path", 4}, {"catalog_run", 1},
      {"producer", 2},       {"run", 1},             {"run_command", 4},  {"run_binding", 2},
      {"run_start", 1},      {"stage_receipt", 3},   {"cycle_binding", 1}, {"cycle_verdict", 2},
      {"wave_result", 1},    {"wave_timing", 1},     {"spec_doc", 5},     {"pin", 29},
      {"trial_line", 5},     {"ledger_state", 1},    {"field_manifest", 1}, {"field_entry", 2},
      {"candidate", 2},      {"candidate_event", 3}, {"build_receipt", 1}, {"build_exe", 1}};
  for (const Count &e : expected) {
    EXPECT_EQ(t::rows(c.db, e.table), e.rows) << e.table;
  }
  EXPECT_EQ(c.report.files_verified, 25);
  EXPECT_EQ(c.report.files_declared, 2);
  EXPECT_EQ(c.report.files_skipped, 4);
  EXPECT_EQ(c.report.files_seen, 31);
  EXPECT_EQ(c.report.catalog_digest, t::digest_of(c.db));
  // Typed columns hold identities, never statistics; documents keep key order and line ends.
  EXPECT_EQ(t::scalar_text(c.db, "SELECT outcome FROM run;"), "completed");
  EXPECT_EQ(t::scalar_text(c.db, "SELECT key_order FROM run;")
                .value_or("")
                .rfind(R"(["schema","source_sha","started_utc","command",)", 0),
            0U);
  EXPECT_EQ(t::scalar_text(c.db, "SELECT extra FROM run;"), std::nullopt);
  EXPECT_EQ(t::scalar_text(c.db, "SELECT arg FROM run_command WHERE ord = 1;"), "nav");
  EXPECT_EQ(t::scalar_text(c.db, "SELECT tag FROM build_exe;"), "fx-1");
  EXPECT_EQ(t::scalar_text(c.db, "SELECT sha256 FROM build_exe;"), std::string(64, 'a'));
  EXPECT_EQ(t::scalar_text(c.db, "SELECT kind FROM spec_doc WHERE path = 'scripts/specs/v8/"
                                 "waves/fx-w1.json';"),
            "wave-manifest");
  EXPECT_EQ(t::scalar_text(c.db, "SELECT extra FROM stage_receipt WHERE file_name = "
                                 "'03-record.failed-1.json';"),
            R"({"code":4,"error":"placeholder failure"})");
  EXPECT_EQ(t::scalar_int(c.db, "SELECT count(*) FROM ledger_state WHERE head_sha256 IS NULL;"),
            1);
}

TEST(ResearchCatalog, DigestReproducibleFromScratch) {
  Catalogued c = catalogued(true);
  // The same tree again into the same store, and into a fresh one segment by segment.
  const cat::CatalogReport again = t::run(c.db, t::options(c.tree));
  auto fresh = t::open_new(t::temp_dir("db2") / "catalog.sqlite");
  cat::CatalogOptions opt = t::options(c.tree);
  opt.segment_files = 1;
  const cat::CatalogReport scratch = t::run(fresh, opt);
  EXPECT_EQ(again.catalog_digest, c.report.catalog_digest);
  EXPECT_EQ(scratch.catalog_digest, c.report.catalog_digest);
  EXPECT_EQ(t::rows(c.db, "artifact"), t::rows(fresh, "artifact"));
  EXPECT_EQ(t::rows(c.db, "catalog_run"), 2); // volatile run bookkeeping: outside the digest
}

TEST(ResearchCatalog, DigestIndependentOfIngestOrder) {
  Catalogued c = catalogued(true);
  auto reversed = t::open_new(t::temp_dir("db2") / "catalog.sqlite");
  cat::CatalogOptions opt = t::options(c.tree);
  opt.reverse_walk = true;
  opt.segment_files = 3;
  const cat::CatalogReport report = t::run(reversed, opt);
  EXPECT_EQ(report.catalog_digest, c.report.catalog_digest);
}

TEST(ResearchCatalog, PinsResolvedAndStaleDetected) {
  Catalogued c = catalogued(true);
  EXPECT_EQ(state_count(c.db, "ok"), "14");
  EXPECT_EQ(state_count(c.db, "declared"), "4");
  EXPECT_EQ(state_count(c.db, "missing"), "1");
  EXPECT_EQ(state_count(c.db, "unresolved"), "10");
  EXPECT_EQ(state_count(c.db, "stale"), "0");
  EXPECT_EQ(t::pin_state(c.db, kSpec, "/inputs/library/sha256"), "ok");
  EXPECT_EQ(t::pin_state(c.db, kSpec, "/inputs/reference_daily/sha256"), "missing");
  EXPECT_EQ(t::pin_state(c.db, kTemplate, "/change/inputs/extra/sha256"), "unresolved");
  EXPECT_EQ(t::pin_state(c.db, "build-equity/waves/fx-w1/wave-result.json",
                         "/receipts/01-preflight"),
            "ok");
  EXPECT_EQ(t::pin_state(c.db, "build-equity/cycle-fx-base/cycle_verdict.json", "/ledger/head"),
            "unresolved");

  // One pinned target edited in a temp copy: its pin, and only its pin, reads stale.
  const auto edited = t::tree_copy("edited");
  t::make_payloads(edited);
  const auto lib = edited / "atx-impl" / "strategies" / "libraries" / "lib-fx.json";
  t::write_bytes(lib, t::read_bytes(lib) + "\n");
  auto db = t::open_new(t::temp_dir("db2") / "catalog.sqlite");
  (void)t::run(db, t::options(edited));
  EXPECT_EQ(t::pin_state(db, kSpec, "/inputs/library/sha256"), "stale");
  EXPECT_EQ(state_count(db, "stale"), "1");
}

TEST(ResearchCatalog, DeclaredPayloadNeverOpenedAndMarkedDeclared) {
  // Payload bytes that do NOT hash to the declared SHA-256: a catalog that opened them would
  // record their own digest; it records the manifest's.
  const auto tree = t::tree_copy();
  t::make_payloads(tree, 'x');
  t::write_payload(tree / "build-equity" / "fx-nav" / "big_undeclared.csv", t::kF1Size, 'y');
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  (void)t::run(db, t::options(tree));
  const std::string where = " FROM artifact WHERE path = 'build-equity/fx-fields/f1.f64';";
  EXPECT_EQ(t::scalar_text(db, "SELECT sha_source" + where), "declared");
  EXPECT_EQ(t::scalar_text(db, "SELECT declared_by" + where), std::string{kManifest});
  EXPECT_EQ(t::scalar_text(db, "SELECT sha256" + where),
            t::scalar_text(db, "SELECT target_sha256 FROM pin WHERE pointer = "
                               "'/files/f1.f64/sha256';"));
  auto actual = atx::core::sha256_file((tree / "build-equity/fx-fields/f1.f64").string());
  ASSERT_TRUE(actual);
  EXPECT_NE(t::scalar_text(db, "SELECT sha256" + where), *actual);
  EXPECT_EQ(t::pin_state(db, kManifest, "/files/f1.f64/sha256"), "declared");
  EXPECT_EQ(t::pin_state(db, kManifest, "/fields/1/sha256"), "declared");
  EXPECT_EQ(t::scalar_int(db, "SELECT count(*) FROM pin_status WHERE pin_kind = 'field-payload' "
                              "AND state = 'ok';"),
            0);
  EXPECT_EQ(t::scalar_text(db, "SELECT reason FROM skipped_path WHERE path = "
                               "'build-equity/fx-nav/big_undeclared.csv';"),
            "declared-only");
  EXPECT_EQ(t::scalar_int(db, "SELECT count(*) FROM artifact WHERE path LIKE '%big_undeclared%';"),
            0);
}

TEST(ResearchCatalog, VerifyPayloadsUpgradesToVerified) {
  Catalogued c = catalogued(true);
  const std::string where = " FROM artifact WHERE path = 'build-equity/fx-fields/f2.f64';";
  EXPECT_EQ(t::scalar_text(c.db, "SELECT sha_source" + where), "declared");
  cat::CatalogOptions opt = t::options(c.tree);
  opt.verify_payloads_dir = "build-equity/fx-fields";
  const cat::CatalogReport report = t::run(c.db, opt);
  EXPECT_EQ(report.files_declared, 0);
  EXPECT_EQ(report.files_verified, 27);
  EXPECT_EQ(t::scalar_text(c.db, "SELECT sha_source" + where), "verified");
  EXPECT_EQ(t::scalar_text(c.db, "SELECT declared_by" + where), std::nullopt);
  auto actual = atx::core::sha256_file((c.tree / "build-equity/fx-fields/f2.f64").string());
  ASSERT_TRUE(actual);
  EXPECT_EQ(t::scalar_text(c.db, "SELECT sha256" + where), *actual);
  EXPECT_EQ(t::pin_state(c.db, kManifest, "/files/f2.f64/sha256"), "ok");

  // Hashed bytes that differ from the declaration read stale, never declared.
  Catalogued wrong = catalogued(true, 'x');
  (void)t::run(wrong.db, [&] {
    cat::CatalogOptions o = t::options(wrong.tree);
    o.verify_payloads_dir = "build-equity/fx-fields";
    return o;
  }());
  EXPECT_EQ(t::pin_state(wrong.db, kManifest, "/files/f2.f64/sha256"), "stale");
}

TEST(ResearchCatalog, UnparsedJsonCataloguedAsArtifact) {
  Catalogued c = catalogued(false);
  const std::string path = "build-equity/fx-nav/extra_nan.json";
  EXPECT_EQ(t::scalar_text(c.db, "SELECT sha_source FROM artifact WHERE path = '" + path + "';"),
            "verified");
  EXPECT_EQ(t::scalar_text(c.db, "SELECT class FROM artifact WHERE path = '" + path + "';"),
            "other");
  EXPECT_EQ(t::scalar_text(c.db, "SELECT reason FROM skipped_path WHERE path = '" + path + "';"),
            "unparsed");
  cat::IngestOneOptions one;
  one.root = c.tree;
  one.class_id = "run-receipt";
  one.path = path;
  auto report = cat::ingest_one(c.db, t::registry(), one);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_TRUE(report->unparsed);
  EXPECT_EQ(t::rows(c.db, "run"), 1); // no typed row from an unparsed file
}

TEST(ResearchCatalog, CrlfAndLfDetected) {
  Catalogued c = catalogued(false);
  const std::pair<std::string_view, std::string_view> expected[] = {
      {"build-equity/fx-nav-run1/receipt.json", "crlf"},
      {"build-equity/fx-nav-run1/start.json", "crlf"},
      {"build-equity/fx-nav-run1/cycle_binding.json", "crlf"},
      {"build-equity/mega-fx-1-receipt.json", "crlf"},
      {"build-equity/waves/fx-w1/receipts/02-run.json", "lf"},
      {"build-equity/cycle-fx-base/cycle_verdict.json", "lf"},
      {"build-equity/trials.jsonl", "lf"},
      {"build-equity/fx-nav-run1/stderr.log", "none"}};
  for (const auto &[path, eol] : expected) {
    EXPECT_EQ(t::scalar_text(c.db, "SELECT eol FROM artifact WHERE path = '" +
                                       std::string{path} + "';"),
              std::string{eol})
        << path;
  }
  // A mixed file.
  const auto mixed = c.tree / "build-equity" / "fx-nav" / "summary.json";
  t::write_bytes(mixed, "{\r\n  \"schema\": \"atx.dsl-nav-replay-summary/v1\"\n}\n");
  (void)t::run(c.db, t::options(c.tree));
  EXPECT_EQ(t::scalar_text(c.db, "SELECT eol FROM artifact WHERE path = "
                                 "'build-equity/fx-nav/summary.json';"),
            "mixed");
}

TEST(ResearchCatalog, IngestIsIdempotent) {
  const auto tree = t::tree_copy();
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  cat::IngestOneOptions one;
  one.root = tree;
  one.class_id = "run-receipt";
  one.path = std::string{kReceipt};
  auto first = cat::ingest_one(db, t::registry(), one);
  ASSERT_TRUE(first) << first.error().to_string();
  const std::string digest = t::digest_of(db);
  auto second = cat::ingest_one(db, t::registry(), one);
  ASSERT_TRUE(second) << second.error().to_string();
  EXPECT_EQ(t::digest_of(db), digest);
  EXPECT_EQ(first->pins, 5U);
  EXPECT_EQ(t::rows(db, "run"), 1);
  EXPECT_EQ(t::rows(db, "run_command"), 4);
  EXPECT_EQ(t::rows(db, "run_binding"), 2);
  EXPECT_EQ(t::rows(db, "pin"), 5);
  EXPECT_EQ(t::rows(db, "artifact"), 1);
}

TEST(ResearchCatalog, TrialLinesAppendOnly) {
  Catalogued c = catalogued(false);
  ASSERT_EQ(t::rows(c.db, "trial_line"), 5);
  const auto first = t::scalar_text(c.db, "SELECT line_sha256 FROM trial_line WHERE seq = 1;");
  EXPECT_FALSE(c.db.exec("UPDATE trial_line SET line = 'x' WHERE seq = 1;").has_value());
  EXPECT_FALSE(c.db.exec("DELETE FROM trial_line WHERE seq = 1;").has_value());

  // An appended line is indexed; the lines before it are untouched.
  const auto ledger = c.tree / "build-equity" / "trials.jsonl";
  const std::string before = t::read_bytes(ledger);
  t::write_bytes(ledger, before + R"({"cell":"fx-cell-6","schema":"atx.trial-ledger/v1",)"
                                  R"("trial_id":"fx-t6"})" "\n");
  (void)t::run(c.db, t::options(c.tree));
  EXPECT_EQ(t::rows(c.db, "trial_line"), 6);
  EXPECT_EQ(t::scalar_text(c.db, "SELECT line_sha256 FROM trial_line WHERE seq = 1;"), first);
  EXPECT_EQ(t::scalar_int(c.db, "SELECT lines FROM ledger_state;"), 6);

  // An edited line is a refusal: the JSONL ledger is the authority (SQL-9).
  std::string edited = before;
  edited.replace(edited.find("fx-cell-2"), 9, "fx-cell-X");
  t::write_bytes(ledger, edited);
  auto refused = cat::run_catalog(c.db, t::registry(), t::options(c.tree));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
  EXPECT_EQ(t::scalar_text(c.db, "SELECT line_sha256 FROM trial_line WHERE seq = 1;"), first);
}

TEST(ResearchCatalog, LedgerHeadSeamReceivesLinesInFileOrder) {
  EXPECT_FALSE(static_cast<bool>(cat::default_ledger_head())); // B2 not bound at this base
  const auto tree = t::tree_copy();
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  std::vector<std::string> seen;
  cat::CatalogOptions opt = t::options(tree);
  opt.ledger_head = [&](std::span<const std::string> lines) -> atx::core::Result<cat::LedgerHead> {
    seen.assign(lines.begin(), lines.end());
    return cat::LedgerHead{std::string(64, 'b'), "test-chain-rule"};
  };
  (void)t::run(db, opt);
  ASSERT_EQ(seen.size(), 5U);
  EXPECT_EQ(seen.front().rfind(R"({"cell":"fx-cell-1")", 0), 0U);
  EXPECT_EQ(seen.back().rfind(R"({"cell":"fx-cell-5")", 0), 0U);
  EXPECT_EQ(t::scalar_text(db, "SELECT head_sha256 FROM ledger_state;"), std::string(64, 'b'));
  EXPECT_EQ(t::scalar_text(db, "SELECT head_rule FROM ledger_state;"), "test-chain-rule");
}

TEST(ResearchCatalog, ClassifierUsesGlobsAndSchemas) {
  const cat::ClassRegistry &r = t::registry();
  const std::optional<std::string> run_schema{"atx.bounded-research-run/v1"};
  EXPECT_EQ(cat::classify(r, "build-equity/x/receipt.json", run_schema, true)->id, "run-receipt");
  EXPECT_EQ(cat::classify(r, "build-equity/x/start.json", run_schema, true)->id, "run-start");
  EXPECT_EQ(cat::classify(r, "build-equity/x/receipt.json", std::string{"atx.nope/v1"}, true)->id,
            "other");
  EXPECT_EQ(cat::classify(r, "build-equity/x/receipt.json", run_schema, false)->id, "other");
  EXPECT_EQ(cat::classify(r, "scripts/specs/v8/a.json",
                          std::string{"atx.research-cycle-template/v1"}, true)->id,
            "cycle-template");
  EXPECT_EQ(cat::classify(r, "build-equity/f/x.f64", std::nullopt, true)->id, "payload");
  EXPECT_EQ(cat::classify(r, "build-equity/trials.jsonl", std::nullopt, true)->id,
            "trial-ledger");
  EXPECT_TRUE(cat::glob_match("**/receipts/[0-9][0-9]-*.json", "a/b/receipts/01-x.json"));
  EXPECT_FALSE(cat::glob_match("**/receipts/[0-9][0-9]-*.json", "a/receipts/1-x.json"));
  EXPECT_TRUE(cat::glob_match("**/*.json", "x.json"));
  EXPECT_TRUE(cat::glob_match("build-equity/mega-*-receipt.json",
                              "build-equity/mega-p9-1-receipt.json"));
  EXPECT_FALSE(cat::glob_match("build-equity/mega-*-receipt.json",
                               "build-equity/sub/mega-p9-1-receipt.json"));
}

TEST(ResearchCatalog, CacheImportMatchesRecordStore) {
  const auto dir = t::temp_dir("cache");
  std::filesystem::copy(t::fixture("legacy_records"), dir,
                        std::filesystem::copy_options::recursive);
  // A record whose body is over 1 MiB, in record_store._file_put's exact bytes.
  const std::string key_text = R"({"i":1})";
  const std::string body_text = R"({"s":")" + std::string(1024 * 1024 + 10, 'a') + R"("})";
  auto key_sha = atx::core::sha256_hex(R"({"key":)" + key_text + R"(,"kind":"big"})");
  auto content = atx::core::sha256_hex(R"({"body":)" + body_text + R"(,"key":)" + key_text +
                                       R"(,"kind":"big","schema":"atx.record-store/v1"})");
  ASSERT_TRUE(key_sha && content);
  std::filesystem::create_directories(dir / "big");
  t::write_bytes(dir / "big" / (*key_sha + ".json"),
                 R"({"schema":"atx.record-store/v1","kind":"big","key":)" + key_text +
                     R"(,"body":)" + body_text + R"(,"content_sha256":")" + *content + "\"}");

  auto db = store::create_cache(dir);
  ASSERT_TRUE(db) << db.error().to_string();
  auto report = cat::import_legacy_records(*db, dir);
  ASSERT_TRUE(report) << report.error().to_string();
  EXPECT_EQ(report->imported, 3);
  EXPECT_EQ(report->rejected, 0);
  auto rows = store::select_all_record(*db);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 3U);
  for (const store::RecordRow &row : *rows) {
    if (row.kind == "factor") {
      // record_store.canonical(key): sorted, compact.
      EXPECT_EQ(row.key, R"({"id":"x1","n":3,"window":"w"})");
      EXPECT_EQ(row.root, "");
      ASSERT_TRUE(row.body.has_value());
      // Python's json.dumps of the body: insertion order, repr floats, ASCII escapes.
      EXPECT_EQ(row.body->rfind(R"({"zeta":0.1,"alpha":[1000000000000000.0,1e+16,1e-05,)", 0),
                0U);
    } else if (row.kind == "aim") {
      EXPECT_EQ(row.root, "part-a");
      EXPECT_EQ(row.key, R"({"a":[2,1],"z":1})");
      EXPECT_EQ(row.body, R"({"weights":[0.25,0.75]})");
    } else {
      EXPECT_EQ(row.kind, "big");
      EXPECT_FALSE(row.body.has_value());
      EXPECT_EQ(row.content_sha256, *content);
      ASSERT_TRUE(row.body_file.has_value());
      EXPECT_EQ(t::read_bytes(dir / *row.body_file), body_text);
    }
  }
  auto again = cat::import_legacy_records(*db, dir);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->imported, 0);
  EXPECT_EQ(again->present, 3);

  std::filesystem::remove_all(dir / "part-a");
  auto pruned = cat::prune_cache(*db, dir);
  ASSERT_TRUE(pruned) << pruned.error().to_string();
  EXPECT_EQ(*pruned, 1);
  EXPECT_EQ(t::rows(*db, "record"), 2);
}
