#include "legacy_schema.hpp"
#include <atx/server/core.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>
#include <vector>

namespace atx::server {
namespace {
constexpr auto document = R"({"schema_version":1,"layout":{"panels":["database"]},)"
                          R"("panel_state":{"source_id":"fixture"}})";
struct DatabaseFile {
    std::filesystem::path path{std::filesystem::temp_directory_path() /
        ("atx-workspace-test-" + std::to_string(unix_ms()) + "-" +
         std::to_string(sequence.fetch_add(1)) + ".db")};
    ~DatabaseFile() {
        std::error_code ignored;
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path.string() + suffix, ignored);
        }
    }
    static inline std::atomic<unsigned> sequence{};
};
} // namespace
TEST(ServerWorkspace, DocumentsPersistAndSummaryListsOmitLayout) {
    DatabaseFile file;
    std::string id;
    {
        Store store(file.path.string());
        const auto created = store.create_workspace("  Database Desk  ", "A database workspace",
                                                     document);
        id = created.id;
        EXPECT_EQ(id.size(), 32U);
        EXPECT_EQ(created.name, "Database Desk");
        EXPECT_EQ(created.revision, 1U);
        EXPECT_FALSE(created.document_json.empty());
        const auto summaries = store.list_workspaces(100, 0);
        ASSERT_EQ(summaries.total, 1U);
        ASSERT_EQ(summaries.workspaces.size(), 1U);
        EXPECT_TRUE(summaries.workspaces[0].document_json.empty());
    }
    Store reopened(file.path.string());
    const auto loaded = reopened.get_workspace(id);
    EXPECT_EQ(loaded.name, "Database Desk");
    EXPECT_EQ(loaded.description, "A database workspace");
    EXPECT_NE(loaded.document_json.find("source_id"), std::string::npos);
    EXPECT_EQ(loaded.revision, 1U);
}
TEST(ServerWorkspace, OptimisticUpdatesAndDeletesRejectStaleRevisions) {
    Store store(":memory:");
    const auto created = store.create_workspace("Desk", "", document);
    const auto updated = store.update_workspace(created.id, 1, "Updated", "New description",
                                                  document);
    EXPECT_EQ(updated.revision, 2U);
    EXPECT_EQ(updated.created_at_ms, created.created_at_ms);
    EXPECT_GE(updated.updated_at_ms, created.updated_at_ms);
    EXPECT_THROW(static_cast<void>(store.update_workspace(created.id, 1, "Lost", "", document)),
                 Error);
    EXPECT_THROW(store.delete_workspace(created.id, 1), Error);
    EXPECT_EQ(store.get_workspace(created.id).name, "Updated");
    store.delete_workspace(created.id, 2);
    EXPECT_THROW(static_cast<void>(store.get_workspace(created.id)), Error);
    EXPECT_EQ(store.list_workspaces(100, 0).total, 0U);
}
TEST(ServerWorkspace, SimultaneousCompareAndSwapAdmitsExactlyOneWriter) {
    Store store(":memory:");
    const auto created = store.create_workspace("Desk", "", document);
    std::vector<std::future<bool>> writers;
    for (unsigned i = 0; i < 12; ++i) {
        writers.push_back(std::async(std::launch::async, [&, i] {
            try {
                static_cast<void>(store.update_workspace(created.id, 1,
                    "Writer " + std::to_string(i), "", document));
                return true;
            } catch (const Error& error) {
                EXPECT_EQ(error.code(), ErrorCode::conflict);
                return false;
            }
        }));
    }
    auto successful = 0U;
    for (auto& writer : writers) if (writer.get()) ++successful;
    EXPECT_EQ(successful, 1U);
    EXPECT_EQ(store.get_workspace(created.id).revision, 2U);
}
TEST(ServerWorkspace, NamesDescriptionsPaginationAndEnvelopeAreBounded) {
    Store store(":memory:");
    for (const auto& bad_name : {std::string{}, std::string("   "), std::string("bad\nname"),
            std::string(81, 'x'), std::string("nul\0name", 8)}) {
        EXPECT_THROW(static_cast<void>(store.create_workspace(bad_name, "", document)), Error);
    }
    EXPECT_THROW(static_cast<void>(store.create_workspace("Desk", std::string(513, 'x'), document)),
                 Error);
    for (const auto* bad : {"{}", "[]", "{\"schema_version\":2,\"layout\":{}}",
            "{\"schema_version\":1.0,\"layout\":{}}", "{\"schema_version\":true,\"layout\":{}}",
            "{\"schema_version\":1,\"layout\":[]}",
            "{\"schema_version\":1,\"layout\":{},\"panel_state\":[]}",
            "{\"schema_version\":1,\"layout\":{},\"layout\":{}}",
            "{\"schema_version\":1,\"layout\":{\"x\":1,\"x\":2}}"}) {
        EXPECT_THROW(static_cast<void>(store.create_workspace("Desk", "", bad)), Error) << bad;
    }
    const auto deep = std::string("{\"schema_version\":1,\"layout\":{\"nested\":") +
        std::string(33, '[') + "0" + std::string(33, ']') + "}}";
    EXPECT_THROW(static_cast<void>(store.create_workspace("Desk", "", deep)), Error);
    EXPECT_THROW(static_cast<void>(store.create_workspace("Desk", "", std::string(98305, ' '))),
                 Error);
    EXPECT_THROW(static_cast<void>(store.list_workspaces(101, 0)), Error);
    EXPECT_THROW(static_cast<void>(store.list_workspaces(0, 0)), Error);
    EXPECT_THROW(static_cast<void>(store.list_workspaces(1, 1000001)), Error);
    const auto created = store.create_workspace("Desk", "", document);
    EXPECT_THROW(store.delete_workspace(created.id, 0), Error);
    EXPECT_THROW(static_cast<void>(store.get_workspace("not-an-id")), Error);
    EXPECT_EQ(store.list_workspaces(100, 0).total, 1U);
}
TEST(ServerWorkspace, VersionTwoMigrationPreservesExistingJobsAndEventWatermark) {
    DatabaseFile file;
    std::string job_id;
    {
        Store store(file.path.string());
        job_id = store.create("{}", "before-migration", 0, 10).id;
        EXPECT_EQ(store.reserve_event_sequences(4096), 0U);
    }
    // Construct the predecessor schema from a current fixture without changing job evidence.
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close {
        sqlite3*& value;
        ~Close() { if (value != nullptr) sqlite3_close(value); }
    } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, (std::string(test_detail::remove_governance) +
        "DROP TABLE IF EXISTS experiment_members; DROP TABLE IF EXISTS experiments;"
        "DROP TRIGGER IF EXISTS job_recipe_immutable; DROP INDEX IF EXISTS jobs_alpha;"
        "DROP INDEX IF EXISTS jobs_experiment; ALTER TABLE jobs DROP COLUMN alpha_id;"
        "ALTER TABLE jobs DROP COLUMN alpha_revision; ALTER TABLE jobs DROP COLUMN experiment_id;"
        "DROP TABLE IF EXISTS alpha_revisions;"
        "DROP TABLE IF EXISTS alpha_heads; DROP TABLE IF EXISTS workspaces; PRAGMA user_version=2;").c_str(),
        nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    raw = nullptr;
    Store migrated(file.path.string());
    EXPECT_EQ(migrated.get(job_id).request_json, "{}");
    EXPECT_EQ(migrated.reserve_event_sequences(1), 4096U);
    EXPECT_EQ(migrated.create_workspace("Migrated", "", document).revision, 1U);
}
TEST(ServerWorkspace, CapacityIsFiniteAndDeletionReleasesAdmission) {
    Store store(":memory:");
    std::string first;
    for (unsigned i = 0; i < 256; ++i) {
        const auto created = store.create_workspace("Desk " + std::to_string(i), "", document);
        if (i == 0) first = created.id;
    }
    EXPECT_THROW(static_cast<void>(store.create_workspace("Overflow", "", document)), Error);
    EXPECT_EQ(store.list_workspaces(100, 0).total, 256U);
    store.delete_workspace(first, 1);
    EXPECT_EQ(store.create_workspace("Replacement", "", document).revision, 1U);
    EXPECT_EQ(store.list_workspaces(100, 0).total, 256U);
}
TEST(ServerWorkspace, UnicodeNamesHaveCharacterBoundsAndTextMustBeValidUtf8) {
    Store store(":memory:");
    std::string unicode;
    for (unsigned i = 0; i < 80; ++i) unicode += "\xE2\x82\xAC";
    const auto created = store.create_workspace("\xC2\xA0" + unicode + "\xC2\xA0", "", document);
    EXPECT_EQ(created.name, unicode);
    EXPECT_THROW(static_cast<void>(store.create_workspace(unicode + "x", "", document)), Error);
    EXPECT_THROW(static_cast<void>(store.create_workspace(
        std::string("\xC0\x80", 2), "", document)),
                 Error);
    EXPECT_THROW(static_cast<void>(store.create_workspace("Desk", std::string("\xED\xA0\x80", 3),
                                                          document)), Error);
    EXPECT_THROW(static_cast<void>(store.create_workspace("Desk", "",
        R"({"schema_version":1,"layout":{},"authorization":"Bearer secret"})")), Error);
}
TEST(ServerWorkspace, ListOrderUsesUpdatedTimeThenStableIdAndClockCannotMoveUpdatesBackward) {
    std::atomic<std::int64_t> clock{100};
    Store store(":memory:", {}, [&] { return clock.load(); });
    const auto first = store.create_workspace("First", "", document);
    clock = 200;
    const auto second = store.create_workspace("Second", "", document);
    EXPECT_EQ(store.list_workspaces(1, 0).workspaces[0].id, second.id);
    clock = 300;
    const auto updated = store.update_workspace(first.id, 1, "First updated", "", document);
    EXPECT_EQ(store.list_workspaces(1, 0).workspaces[0].id, first.id);
    clock = 50;
    EXPECT_EQ(store.update_workspace(first.id, 2, "First again", "", document).updated_at_ms,
              updated.updated_at_ms);
}
TEST(ServerWorkspace, IndependentConnectionsSerializeOptimisticWrites) {
    DatabaseFile file;
    Store first(file.path.string()), second(file.path.string());
    const auto created = first.create_workspace("Shared", "", document);
    auto write = [&](Store& store, std::string_view name) {
        try {
            static_cast<void>(store.update_workspace(created.id, 1, name, "", document));
            return true;
        } catch (const Error& error) { EXPECT_EQ(error.code(), ErrorCode::conflict); return false; }
    };
    auto a = std::async(std::launch::async, [&] { return write(first, "First writer"); });
    auto b = std::async(std::launch::async, [&] { return write(second, "Second writer"); });
    const auto successful = static_cast<unsigned>(a.get()) + static_cast<unsigned>(b.get());
    EXPECT_EQ(successful, 1U);
    EXPECT_EQ(first.get_workspace(created.id).revision, 2U);
    EXPECT_EQ(second.get_workspace(created.id).revision, 2U);
}
} // namespace atx::server
