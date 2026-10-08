#include "legacy_schema.hpp"
#include <atx/server/core.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace atx::server {
namespace {
ExperimentInput batch(unsigned members = 2) {
    ExperimentInput input{"  Sensitivity study  ", "Trading cost scenarios", {}, "batch-key", 0};
    for (unsigned i = 0; i < members; ++i) {
        input.simulations.push_back({"Scenario " + std::to_string(i),
            "{\"mode\":\"synthetic_demo\",\"trade_bps\":" + std::to_string(i) + "}", "", 0});
    }
    return input;
}
AlphaInput alpha_recipe() {
    return {"Alpha", "", {}, "", R"({"mode":"synthetic_demo","sessions":32})", "", "local"};
}
struct ExperimentFile {
    std::filesystem::path path{std::filesystem::temp_directory_path() /
        ("atx-experiment-test-" + std::to_string(unix_ms()) + "-" +
         std::to_string(sequence.fetch_add(1)) + ".db")};
    ~ExperimentFile() {
        std::error_code ignored;
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path.string() + suffix, ignored);
        }
    }
    static inline std::atomic<unsigned> sequence{};
};
} // namespace
TEST(ServerExperiment, BatchAdmissionIsAtomicAndIdempotencyHasStableSemantics) {
    Store store(":memory:");
    auto input = batch(3);
    EXPECT_THROW(static_cast<void>(store.create_experiment(input, 2)), Error);
    EXPECT_EQ(store.list(100, 0, "").total, 0U);
    EXPECT_EQ(store.list_experiments(100, 0).total, 0U);
    const auto first = store.create_experiment(input, 3);
    ASSERT_EQ(first.jobs.size(), 3U);
    EXPECT_EQ(first.name, "Sensitivity study");
    EXPECT_EQ(first.status, "QUEUED");
    EXPECT_EQ(first.total, 3U);
    const auto repeated = store.create_experiment(input, 3);
    EXPECT_EQ(repeated.id, first.id);
    for (std::size_t i = 0; i < first.jobs.size(); ++i) {
        EXPECT_EQ(first.jobs[i].id, repeated.jobs[i].id);
        EXPECT_EQ(first.jobs[i].experiment_id, first.id);
        EXPECT_TRUE(first.jobs[i].request_json.empty());
    }
    input.simulations[0].request_json = R"({"trade_bps":0,"mode":"synthetic_demo"})";
    EXPECT_EQ(store.create_experiment(input, 3).id, first.id);
    input.description = "Conflicting semantics";
    EXPECT_THROW(static_cast<void>(store.create_experiment(input, 10)), Error);
    EXPECT_EQ(store.list(100, 0, "").total, 3U);
}
TEST(ServerExperiment, ExactAlphaRevisionLinksAreResolvedAndCannotForgeProvenance) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(alpha_recipe());
    const auto linked = store.create("", "linked", 0, 10, {alpha.id, 1, "Exact recipe"});
    EXPECT_EQ(linked.alpha_id, alpha.id);
    EXPECT_EQ(linked.alpha_revision, 1U);
    EXPECT_EQ(linked.request_json, alpha.request_json);
    EXPECT_TRUE(linked.experiment_id.empty());
    EXPECT_THROW(static_cast<void>(store.create("", "linked", 0, 10,
        {alpha.id, 1, ""})), Error);
    EXPECT_THROW(static_cast<void>(store.create(R"({"sessions":40})", "", 0, 10,
        {alpha.id, 1, "Wrong"})), Error);
    EXPECT_THROW(static_cast<void>(store.create("", "", 0, 10, {alpha.id, 0, ""})), Error);
    auto input = batch(1);
    input.simulations = {{"Pinned alpha", "", alpha.id, 1}};
    const auto saved = store.create_experiment(input, 10);
    static_cast<void>(store.set_alpha_archived(alpha.id, 1, true));
    EXPECT_EQ(store.create_experiment(input, 10).id, saved.id);
    EXPECT_EQ(store.create("", "linked", 0, 10, {alpha.id, 1, "Exact recipe"}).id, linked.id);
    input.idempotency_key = "new-archived";
    EXPECT_THROW(static_cast<void>(store.create_experiment(input, 10)), Error);
    EXPECT_THROW(static_cast<void>(store.create("", "new-linked", 0, 10,
        {alpha.id, 1, ""})), Error);
    EXPECT_EQ(store.list(100, 0, "").total, 2U);
}
TEST(ServerExperiment, ConcurrentIndependentConnectionsDeduplicateWholeBatch) {
    ExperimentFile file;
    Store first(file.path.string()), second(file.path.string());
    auto input = batch(4);
    auto a = std::async(std::launch::async, [&] { return first.create_experiment(input, 4); });
    auto b = std::async(std::launch::async, [&] { return second.create_experiment(input, 4); });
    const auto left = a.get(), right = b.get();
    EXPECT_EQ(left.id, right.id);
    EXPECT_EQ(first.list(100, 0, "").total, 4U);
    EXPECT_EQ(first.list_experiments(100, 0).total, 1U);
}
TEST(ServerExperiment, AggregatesRemainActiveUntilAllChildrenAreTerminalAndCancelIsTruthful) {
    Store store(":memory:");
    const auto experiment = store.create_experiment(batch(3), 10);
    const auto first = store.claim_due();
    ASSERT_TRUE(first);
    store.finish(first->id, "FAILED", "", "Model invalid");
    auto current = store.get_experiment(experiment.id);
    EXPECT_EQ(current.status, "RUNNING");
    EXPECT_EQ(current.failed, 1U);
    EXPECT_NEAR(current.progress, 1.0 / 3.0, 1e-12);
    const auto cancelled = store.cancel_experiment(experiment.id);
    EXPECT_EQ(cancelled.status, "FAILED");
    EXPECT_EQ(cancelled.cancelled, 2U);
    EXPECT_EQ(cancelled.progress, 1);
    EXPECT_EQ(store.cancel_experiment(experiment.id).failed, 1U);
    auto successful_input = batch(1); successful_input.idempotency_key = "success";
    const auto successful = store.create_experiment(successful_input, 10);
    const auto running = store.claim_due();
    ASSERT_TRUE(running);
    store.finish(running->id, "SUCCEEDED", "{}", "");
    EXPECT_EQ(store.cancel_experiment(successful.id).status, "SUCCEEDED");
    EXPECT_EQ(store.list_experiments(100, 0).experiments[0].jobs.size(), 0U);
}
TEST(ServerExperiment, ValidationAndCanonicalAggregateByteBudgetsHaveNoPartialRows) {
    Store store(":memory:");
    for (const auto count : {0U, 33U}) {
        EXPECT_THROW(static_cast<void>(store.create_experiment(batch(count), 100)), Error);
    }
    auto input = batch(32);
    const auto full = store.create_experiment(input, 100);
    EXPECT_EQ(full.jobs.size(), 32U);
    input = batch(); input.idempotency_key = "bad-both";
    input.simulations[0].alpha_id = std::string(32, 'a');
    input.simulations[0].alpha_revision = 1;
    EXPECT_THROW(static_cast<void>(store.create_experiment(input, 100)), Error);
    input = batch(); input.idempotency_key = "missing-alpha";
    input.simulations[1] = {"Missing", "", std::string(32, 'a'), 1};
    EXPECT_THROW(static_cast<void>(store.create_experiment(input, 100)), Error);
    input = batch(2); input.idempotency_key = "large";
    for (auto& simulation : input.simulations) {
        simulation.request_json = "{\"mode\":\"synthetic_demo\",\"strategy_dsl\":\"" +
            std::string(50000, 'x') + "\"}";
    }
    EXPECT_THROW(static_cast<void>(store.create_experiment(input, 100)), Error);
    input = batch(); input.idempotency_key = std::string(129, 'x');
    EXPECT_THROW(static_cast<void>(store.create_experiment(input, 100)), Error);
    EXPECT_THROW(static_cast<void>(store.list_experiments(101, 0)), Error);
    EXPECT_EQ(store.list(100, 0, "").total, 32U);
    EXPECT_EQ(store.list_experiments(100, 0).total, 1U);
}
TEST(ServerExperiment, RestartRetainsMemberLinksAndRecoversInterruptedChildrenHonestly) {
    ExperimentFile file;
    std::string id, running_id;
    {
        Store store(file.path.string());
        const auto created = store.create_experiment(batch(), 10);
        id = created.id;
        running_id = store.claim_due()->id;
    }
    Store reopened(file.path.string());
    reopened.recover();
    const auto restored = reopened.get_experiment(id);
    EXPECT_EQ(restored.failed, 1U);
    EXPECT_EQ(restored.queued, 1U);
    EXPECT_EQ(restored.status, "RUNNING");
    EXPECT_EQ(reopened.get(running_id).experiment_id, id);
    EXPECT_EQ(reopened.create_experiment(batch(), 10).id, id);
}
TEST(ServerExperiment, SchedulerCancellationStopsRunningAndScheduledMembersCooperatively) {
    Store store(":memory:");
    EventBus events(128);
    std::promise<void> started;
    Scheduler scheduler(store, events, [&](std::string_view, std::stop_token stop,
        const Progress&) {
        started.set_value();
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        changed.wait(lock, stop, [] { return false; });
        return std::string("{}");
    }, 1, 32);
    auto input = batch(2);
    const auto experiment = scheduler.submit_experiment(input);
    ASSERT_EQ(started.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto cancelled = scheduler.cancel_experiment(experiment.id);
    EXPECT_EQ(cancelled.status, "CANCELLED");
    EXPECT_EQ(cancelled.cancelled, 2U);
    scheduler.shutdown();
    EXPECT_EQ(store.get_experiment(experiment.id).cancelled, 2U);
    for (const auto& job : store.get_experiment(experiment.id).jobs) {
        EXPECT_EQ(store.get(job.id).status, "CANCELLED");
    }
}
TEST(ServerExperiment, VersionFourMigrationRetainsExistingWorkspaceAlphaAndJobEvidence) {
    ExperimentFile file;
    std::string job_id, workspace_id, alpha_id;
    {
        Store store(file.path.string());
        job_id = store.create("{}", "legacy", 0, 10).id;
        workspace_id = store.create_workspace("Desk", "", R"({"schema_version":1,"layout":{}})").id;
        alpha_id = store.create_alpha(alpha_recipe()).id;
        EXPECT_EQ(store.reserve_event_sequences(4096), 0U);
    }
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, (std::string(test_detail::remove_governance) +
        "DROP TABLE experiment_members; DROP TABLE experiments;"
        "DROP TRIGGER job_recipe_immutable; DROP INDEX jobs_alpha; DROP INDEX jobs_experiment;"
        "ALTER TABLE jobs DROP COLUMN alpha_id; ALTER TABLE jobs DROP COLUMN alpha_revision;"
        "ALTER TABLE jobs DROP COLUMN experiment_id; PRAGMA user_version=4;").c_str(),
        nullptr, nullptr, nullptr), SQLITE_OK);
    Store migrated(file.path.string());
    EXPECT_EQ(migrated.get(job_id).request_json, "{}");
    EXPECT_TRUE(migrated.get(job_id).alpha_id.empty());
    EXPECT_EQ(migrated.get(job_id).alpha_revision, 0U);
    EXPECT_TRUE(migrated.get(job_id).experiment_id.empty());
    EXPECT_EQ(migrated.get_workspace(workspace_id).name, "Desk");
    EXPECT_EQ(migrated.get_alpha(alpha_id).revision, 1U);
    EXPECT_EQ(migrated.reserve_event_sequences(1), 4096U);
    EXPECT_EQ(migrated.create_experiment(batch(), 10).total, 2U);
}
TEST(ServerExperiment, FutureMembersDoNotExecuteAndNotificationFailureCannotUndoSubmission) {
    ExperimentFile file;
    Store store(file.path.string());
    EventBus events(1, &store);
    for (unsigned i = 0; i < 4095; ++i) events.publish("system", "fixture", "", "{}");
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, "DROP TABLE metadata", nullptr, nullptr, nullptr), SQLITE_OK);
    std::atomic<unsigned> calls{};
    Scheduler scheduler(store, events, [&](std::string_view, std::stop_token, const Progress&) {
        ++calls; return std::string("{}");
    }, 1, 8);
    auto input = batch(); input.scheduled_at_ms = unix_ms() + 86400000;
    const auto submitted = scheduler.submit_experiment(input);
    EXPECT_EQ(submitted.queued, 2U);
    EXPECT_EQ(store.list(100, 0, "").total, 2U);
    EXPECT_TRUE(scheduler.healthy());
    EXPECT_EQ(scheduler.submit_experiment(input).id, submitted.id);
    EXPECT_EQ(scheduler.cancel_experiment(submitted.id).cancelled, 2U);
    scheduler.shutdown();
    EXPECT_EQ(calls.load(), 0U);
    EXPECT_EQ(store.get_experiment(submitted.id).status, "CANCELLED");
}
TEST(ServerExperiment, StopCallbacksRunOutsideSchedulerAndStoreLocksForAllRunningMembers) {
    Store store(":memory:");
    EventBus events(128);
    std::promise<void> started;
    std::atomic<unsigned> active{}, callbacks{};
    Scheduler* observer{}; // Borrowed; assigned before submitting any work.
    Scheduler scheduler(store, events, [&](std::string_view, std::stop_token stop,
        const Progress&) {
        std::stop_callback callback(stop, [&] {
            EXPECT_TRUE(observer->healthy());
            EXPECT_EQ(store.list(100, 0, "CANCELLED").total, 2U);
            ++callbacks;
        });
        if (active.fetch_add(1) == 1) started.set_value();
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        changed.wait(lock, stop, [] { return false; });
        return std::string("{}");
    }, 2, 8);
    observer = &scheduler;
    const auto submitted = scheduler.submit_experiment(batch());
    ASSERT_EQ(started.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(scheduler.cancel_experiment(submitted.id).cancelled, 2U);
    EXPECT_EQ(callbacks.load(), 2U);
    scheduler.shutdown();
}
TEST(ServerExperiment, CancelAndFinishRaceNeverRewritesACompletedResult) {
    Store store(":memory:");
    for (unsigned i = 0; i < 20; ++i) {
        auto input = batch(1); input.idempotency_key = "race-" + std::to_string(i);
        const auto submitted = store.create_experiment(input, 8);
        const auto job = store.claim_due();
        ASSERT_TRUE(job);
        auto finish = std::async(std::launch::async, [&] {
            store.finish(job->id, "SUCCEEDED", "{\"evidence\":1}", "");
        });
        auto cancel = std::async(std::launch::async, [&] {
            return store.cancel_experiment(submitted.id);
        });
        finish.get();
        const auto cancelled = cancel.get();
        const auto saved = store.get(job->id);
        EXPECT_TRUE(saved.status == "SUCCEEDED" || saved.status == "CANCELLED");
        EXPECT_EQ(cancelled.status, saved.status);
        if (saved.status == "SUCCEEDED") EXPECT_EQ(saved.result_json, "{\"evidence\":1}");
        else EXPECT_TRUE(saved.result_json.empty());
    }
}
TEST(ServerExperiment, DurableParentCapacityRemainsBoundedAfterCancellation) {
    Store store(":memory:");
    std::string first;
    for (unsigned i = 0; i < 2048; ++i) {
        auto input = batch(1); input.idempotency_key = "parent-" + std::to_string(i);
        const auto experiment = store.create_experiment(input, 1);
        if (i == 0) first = experiment.id;
        static_cast<void>(store.cancel_experiment(experiment.id));
    }
    auto overflow = batch(1); overflow.idempotency_key = "overflow";
    EXPECT_THROW(static_cast<void>(store.create_experiment(overflow, 1)), Error);
    EXPECT_EQ(store.list_experiments(100, 0).total, 2048U);
    EXPECT_EQ(store.list(100, 0, "").total, 2048U);
    auto repeated = batch(1); repeated.idempotency_key = "parent-0";
    EXPECT_EQ(store.create_experiment(repeated, 1).id, first);
}
} // namespace atx::server
