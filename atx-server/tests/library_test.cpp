#include "legacy_schema.hpp"
#include <atx/server/core.hpp>
#include <atx/core/sha256.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>
#include <vector>

namespace atx::server {
namespace {
AlphaInput recipe(std::string name = "Momentum") {
    return {std::move(name), "Cross-sectional effect", {"momentum", "equity"},
        "Recent relative strength persists", R"({"mode":"synthetic_demo","sessions":32})",
        "", "local"};
}
struct LibraryFile {
    std::filesystem::path path{std::filesystem::temp_directory_path() /
        ("atx-library-test-" + std::to_string(unix_ms()) + "-" +
         std::to_string(sequence.fetch_add(1)) + ".db")};
    ~LibraryFile() {
        std::error_code ignored;
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path.string() + suffix, ignored);
        }
    }
    static inline std::atomic<unsigned> sequence{};
};
} // namespace
TEST(ServerLibrary, ImmutableSnapshotsPersistAndCanonicalRecipesHaveServerHashes) {
    LibraryFile file;
    std::string id;
    {
        Store store(file.path.string());
        auto input = recipe("  Momentum  ");
        const auto created = store.create_alpha(input);
        id = created.id;
        EXPECT_EQ(created.name, "Momentum");
        EXPECT_EQ(created.revision, 1U);
        EXPECT_EQ(created.stage, "research");
        const auto hash = atx::core::sha256_hex(created.request_json);
        ASSERT_TRUE(hash);
        EXPECT_EQ(created.request_sha256, *hash);
        input.name = "Revised hypothesis";
        input.request_json = R"({"sessions":40,"mode":"synthetic_demo"})";
        const auto updated = store.update_alpha(id, 1, input);
        EXPECT_EQ(updated.revision, 2U);
        EXPECT_EQ(updated.created_at_ms, created.created_at_ms);
        EXPECT_EQ(store.get_alpha(id, 1).request_sha256, created.request_sha256);
        EXPECT_EQ(store.get_alpha(id).name, input.name);
        const auto history = store.list_alpha_revisions(id, 100, 0);
        ASSERT_EQ(history.total, 2U);
        EXPECT_EQ(history.alphas[0].revision, 2U);
        EXPECT_EQ(history.alphas[1].revision, 1U);
    }
    Store reopened(file.path.string());
    EXPECT_EQ(reopened.get_alpha(id).revision, 2U);
    EXPECT_EQ(reopened.get_alpha(id, 1).name, "Momentum");
}
TEST(ServerLibrary, CompareAndSwapAndArchivePreserveFullHistory) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(recipe());
    const auto archived = store.set_alpha_archived(alpha.id, 1, true);
    EXPECT_EQ(archived.revision, 2U);
    EXPECT_EQ(archived.stage, "archived");
    EXPECT_EQ(store.list_alphas(100, 0).total, 0U);
    EXPECT_EQ(store.list_alphas(100, 0, "", "", "local", true).total, 1U);
    EXPECT_THROW(static_cast<void>(store.update_alpha(alpha.id, 1, recipe())), Error);
    EXPECT_THROW(static_cast<void>(store.set_alpha_archived(alpha.id, 1, false)), Error);
    const auto edited = store.update_alpha(alpha.id, 2, recipe("Archived edit"));
    EXPECT_EQ(edited.stage, "archived");
    const auto restored = store.set_alpha_archived(alpha.id, 3, false);
    EXPECT_EQ(restored.revision, 4U);
    EXPECT_EQ(restored.stage, "research");
    EXPECT_EQ(store.get_alpha(alpha.id, 2).stage, "archived");
    EXPECT_EQ(store.get_alpha(alpha.id, 1).stage, "research");
}
TEST(ServerLibrary, SummariesSearchTagsAndPaginationAreBoundedAndLiteral) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(recipe("Literal %_ edge"));
    static_cast<void>(store.create_alpha(recipe("Other")));
    const auto page = store.list_alphas(1, 0, "%_", "momentum");
    ASSERT_EQ(page.total, 1U);
    ASSERT_EQ(page.alphas.size(), 1U);
    EXPECT_EQ(page.alphas[0].id, alpha.id);
    EXPECT_TRUE(page.alphas[0].request_json.empty());
    EXPECT_TRUE(page.alphas[0].hypothesis.empty());
    EXPECT_EQ(store.list_alphas(100, 0, "", "absent").total, 0U);
    EXPECT_THROW(static_cast<void>(store.list_alphas(101, 0)), Error);
    EXPECT_THROW(static_cast<void>(store.list_alphas(1, 1000001)), Error);
    EXPECT_THROW(static_cast<void>(store.list_alphas(1, 0, std::string(129, 'a'))), Error);
    EXPECT_THROW(static_cast<void>(store.list_alphas(1, 0, "", "", "team")), Error);
    EXPECT_THROW(static_cast<void>(store.list_alpha_revisions(alpha.id, 0, 0)), Error);
    EXPECT_THROW(static_cast<void>(store.get_alpha(alpha.id, 99)), Error);
}
TEST(ServerLibrary, SourceJobsMustMatchCanonicalRecipeWithoutImplyingValidation) {
    Store store(":memory:");
    const auto job = store.create(R"({ "sessions":32, "mode":"synthetic_demo" })", "", 0, 10);
    auto input = recipe();
    input.source_job_id = job.id;
    const auto alpha = store.create_alpha(input);
    EXPECT_EQ(alpha.source_job_id, job.id);
    EXPECT_EQ(alpha.stage, "research");
    input.request_json = R"({"mode":"synthetic_demo","sessions":40})";
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input.source_job_id = std::string(32, 'a');
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    EXPECT_EQ(store.list_alphas(100, 0).total, 1U);
}
TEST(ServerLibrary, ValidationRejectsMalformedRecipesAndUnboundedMetadata) {
    Store store(":memory:");
    for (const auto* json : {"[]", "null", R"({"mode":"other"})",
            R"({"mode":"synthetic_demo","mode":"synthetic_demo"})",
            R"({"mode":"synthetic_demo","sessions":1.5})",
            R"({"mode":"synthetic_demo","sessions":1e100})",
            R"({"mode":"import_nav"})", R"({"mode":"real_nav"})",
            R"({"mode":"synthetic_demo","authorization":"secret"})"}) {
        auto input = recipe(); input.request_json = json;
        EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error) << json;
    }
    auto input = recipe(); input.tags.push_back("momentum");
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input = recipe(); input.hypothesis.assign(4097, 'x');
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input = recipe(); input.project_id = "team";
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input = recipe(); input.name.assign(81, 'x');
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input = recipe(); input.request_json = std::string(65537, ' ');
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    EXPECT_EQ(store.list_alphas(100, 0).total, 0U);
}
TEST(ServerLibrary, IndependentConnectionsAdmitOnlyOneRevisionWriter) {
    LibraryFile file;
    Store first(file.path.string()), second(file.path.string());
    const auto alpha = first.create_alpha(recipe());
    auto write = [&](Store& store, const char* name) {
        try { static_cast<void>(store.update_alpha(alpha.id, 1, recipe(name))); return true; }
        catch (const Error& error) { EXPECT_EQ(error.code(), ErrorCode::conflict); return false; }
    };
    auto a = std::async(std::launch::async, [&] { return write(first, "First"); });
    auto b = std::async(std::launch::async, [&] { return write(second, "Second"); });
    EXPECT_EQ(static_cast<unsigned>(a.get()) + static_cast<unsigned>(b.get()), 1U);
    EXPECT_EQ(first.list_alpha_revisions(alpha.id, 100, 0).total, 2U);
}
TEST(ServerLibrary, VersionThreeMigrationPreservesWorkspaceJobAndEventEvidence) {
    LibraryFile file;
    std::string job, workspace;
    {
        Store store(file.path.string());
        job = store.create("{}", "migration", 0, 10).id;
        workspace = store.create_workspace("Desk", "", R"({"schema_version":1,"layout":{}})").id;
        EXPECT_EQ(store.reserve_event_sequences(4096), 0U);
    }
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, (std::string(test_detail::remove_governance) +
        "DROP TABLE IF EXISTS experiment_members; DROP TABLE IF EXISTS experiments;"
        "DROP TRIGGER IF EXISTS job_recipe_immutable; DROP INDEX IF EXISTS jobs_alpha;"
        "DROP INDEX IF EXISTS jobs_experiment; ALTER TABLE jobs DROP COLUMN alpha_id;"
        "ALTER TABLE jobs DROP COLUMN alpha_revision; ALTER TABLE jobs DROP COLUMN experiment_id;"
        "DROP TABLE IF EXISTS alpha_revisions;"
        "DROP TABLE IF EXISTS alpha_heads; PRAGMA user_version=3;").c_str(), nullptr, nullptr, nullptr),
        SQLITE_OK);
    Store migrated(file.path.string());
    EXPECT_EQ(migrated.get(job).request_json, "{}");
    EXPECT_EQ(migrated.get_workspace(workspace).name, "Desk");
    EXPECT_EQ(migrated.reserve_event_sequences(1), 4096U);
    EXPECT_EQ(migrated.create_alpha(recipe()).revision, 1U);
}
TEST(ServerLibrary, HeadAdmissionIsFiniteAndArchivedHeadsRetainCapacityAndEvidence) {
    Store store(":memory:");
    std::string first;
    for (unsigned i = 0; i < 2048; ++i) {
        const auto alpha = store.create_alpha(recipe("Alpha " + std::to_string(i)));
        if (i == 0) first = alpha.id;
    }
    EXPECT_THROW(static_cast<void>(store.create_alpha(recipe("Overflow"))), Error);
    static_cast<void>(store.set_alpha_archived(first, 1, true));
    EXPECT_THROW(static_cast<void>(store.create_alpha(recipe("Still full"))), Error);
    EXPECT_EQ(store.list_alphas(100, 0, "", "", "", true).total, 2048U);
    EXPECT_EQ(store.get_alpha(first, 1).stage, "research");
}
TEST(ServerLibrary, GlobalRevisionLimitRollsBackHeadAndSqlRejectsHistoryMutation) {
    LibraryFile file;
    Store store(file.path.string());
    const auto alpha = store.create_alpha(recipe());
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, "WITH RECURSIVE n(v) AS (VALUES(2) UNION ALL "
        "SELECT v+1 FROM n WHERE v<16384) INSERT INTO alpha_revisions "
        "SELECT alpha_id,project_id,name,description,tags_json,stage,n.v,hypothesis,request_json,"
        "request_sha256,source_job_id,created_at_ms,updated_at_ms,actor_id FROM alpha_revisions,n "
        "WHERE revision=1; UPDATE alpha_heads SET revision=16384;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    EXPECT_THROW(static_cast<void>(store.update_alpha(alpha.id, 16384, recipe("Overflow"))),
                 Error);
    EXPECT_THROW(static_cast<void>(store.set_alpha_archived(alpha.id, 16384, true)), Error);
    EXPECT_THROW(static_cast<void>(store.create_alpha(recipe("Overflow"))), Error);
    EXPECT_EQ(store.get_alpha(alpha.id).revision, 16384U);
    EXPECT_EQ(store.list_alpha_revisions(alpha.id, 1, 0).total, 16384U);
    EXPECT_EQ(sqlite3_exec(raw, "UPDATE alpha_revisions SET name='tampered' WHERE revision=1",
        nullptr, nullptr, nullptr), SQLITE_CONSTRAINT);
    EXPECT_EQ(sqlite3_exec(raw, "DELETE FROM alpha_revisions WHERE revision=1",
        nullptr, nullptr, nullptr), SQLITE_CONSTRAINT);
    EXPECT_EQ(store.get_alpha(alpha.id, 1).name, "Momentum");
}
TEST(ServerLibrary, RecipeShapesDoNotPerformIoAndMetadataCannotBypassUtf8OrDepthLimits) {
    Store store(":memory:");
    auto input = recipe();
    input.request_json = R"({"mode":"import_nav","artifact_path":"missing.csv"})";
    EXPECT_EQ(store.create_alpha(input).revision, 1U);
    input.request_json = R"({"mode":"real_nav","combined_path":"missing.json",)"
        R"("role_path":"role.json","combined_sha256":")" + std::string(64, 'a') +
        R"(","role_sha256":")" + std::string(64, 'b') + R"(","cadence":5})";
    EXPECT_EQ(store.create_alpha(input).revision, 1U);
    for (const auto* invalid : {R"({"mode":"synthetic_demo","initial_capital":1e999})",
            R"({"mode":"synthetic_demo","strategy_dsl":null})",
            R"({"mode":"import_nav","artifact_path":"a","start_date":"2026-02-30"})",
            R"({"mode":"import_nav","artifact_path":"a","start_date":"2026-10-02",)"
            R"("end_date":"2026-10-01"})"}) {
        input = recipe(); input.request_json = invalid;
        EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    }
    input = recipe(); input.hypothesis = std::string("\xED\xA0\x80", 3);
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input = recipe(); input.tags = {" tag "};
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input = recipe(); input.tags.assign(17, "tag");
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    input = recipe(); input.request_json = "{\"mode\":\"synthetic_demo\",\"strategy_dsl\":" +
        std::string(33, '[') + "0" + std::string(33, ']') + "}";
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
}
TEST(ServerLibrary, PinnedRealDslRecipesRoundTripWithoutLoadingOperatorDatasets) {
    Store store(":memory:");
    auto input = recipe();
    input.request_json = R"({"mode":"real_dsl","strategy_id":"real-data-long-short-v1",)"
        R"("dataset_id":"equity_train","dataset_sha256":")" + std::string(64, 'c') +
        R"json(","strategy_dsl":"rank(delta(close,5))","start_date":"2020-01-01",)json"
        R"("end_date":"2022-12-31","instrument_limit":256,"cadence":5,)"
        R"("execution_delay_sessions":1,"neutralization":"dollar"})";
    const auto saved = store.create_alpha(input);
    EXPECT_EQ(saved.request_sha256.size(), 64U);
    EXPECT_NE(store.get_alpha(saved.id).request_json.find("equity_train"), std::string::npos);
    input.request_json = R"({"mode":"real_dsl","dataset_id":"equity_train",)"
        R"("dataset_sha256":")" + std::string(64, 'c') + R"(","start_date":"2023-01-01"})";
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
}
TEST(ServerLibrary, FrozenPortfolioRecipeRoundTripsAndRejectsAlteredExpressionPins) {
    using Json = nlohmann::json;
    Store store(":memory:");
    const auto first = atx::core::sha256_hex("rank(close)"); ASSERT_TRUE(first);
    const auto second = atx::core::sha256_hex("rank(volume)"); ASSERT_TRUE(second);
    Json request{{"mode", "real_portfolio"}, {"strategy_id", "real-data-portfolio-v1"},
        {"dataset_id", "equity_train"}, {"dataset_sha256", std::string(64, 'c')},
        {"components", Json::array({{{"strategy_dsl", "rank(close)"},
            {"expression_sha256", *first}, {"weight", 0.6}},
            {{"strategy_dsl", "rank(volume)"}, {"expression_sha256", *second},
             {"weight", -0.4}}})}, {"gross_limit", 1}, {"name_cap", 0.1},
        {"max_turnover", 1}, {"signal_decay_sessions", 5}};
    auto input = recipe(); input.request_json = request.dump();
    const auto saved = store.create_alpha(input);
    EXPECT_EQ(Json::parse(saved.request_json), request);
    EXPECT_EQ(store.get_alpha(saved.id, 1).request_sha256, saved.request_sha256);
    auto invalid = request;
    invalid["components"][0]["expression_sha256"] = std::string(64, 'a');
    input.request_json = invalid.dump();
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    invalid = request; invalid["components"][1]["weight"] = 0;
    invalid["components"][0]["weight"] = 0; input.request_json = invalid.dump();
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    invalid = request; invalid["components"][0]["alpha_id"] = std::string(32, 'a');
    input.request_json = invalid.dump();
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
    invalid = request; invalid["signal_decay_sessions"] = 1.5;
    input.request_json = invalid.dump();
    EXPECT_THROW(static_cast<void>(store.create_alpha(input)), Error);
}
} // namespace atx::server
