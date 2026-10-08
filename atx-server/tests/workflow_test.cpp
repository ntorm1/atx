#include <atx/server/core.hpp>
#include "legacy_schema.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>

namespace atx::server {
namespace {
constexpr std::int64_t epoch = 1700000000000LL;
WorkflowInput recurring() {
    return {"  Daily study  ", "Frozen recipe", R"({"mode":"synthetic_demo"})",
        "", 0, 60000, 3, epoch, "workflow-key", "alice"};
}
struct WorkflowFile {
    std::filesystem::path path{std::filesystem::temp_directory_path() /
        ("atx-workflow-test-" + std::to_string(unix_ms()) + "-" +
         std::to_string(sequence.fetch_add(1)) + ".db")};
    ~WorkflowFile() {
        std::error_code ignored;
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path.string() + suffix, ignored);
        }
    }
    static inline std::atomic<unsigned> sequence{};
};
} // namespace
TEST(ServerWorkflow, FrozenConfigurationIdempotencyAndSummaryBudgets) {
    Store store(":memory:", {}, [] { return epoch; });
    auto input = recurring();
    const auto first = store.create_workflow(input);
    EXPECT_EQ(first.name, "Daily study");
    EXPECT_EQ(first.actor_id, "alice");
    EXPECT_EQ(first.revision, 1U);
    EXPECT_EQ(first.state, "active");
    EXPECT_EQ(first.request_sha256.size(), 64U);
    EXPECT_EQ(first.next_run_at_ms, epoch);
    EXPECT_EQ(store.create_workflow(input).id, first.id);
    input.actor_id = "bob";
    EXPECT_THROW(static_cast<void>(store.create_workflow(input)), Error);
    input.actor_id = "alice"; input.max_runs = 4;
    EXPECT_THROW(static_cast<void>(store.create_workflow(input)), Error);
    const auto page = store.list_workflows(100, 0);
    ASSERT_EQ(page.workflows.size(), 1U);
    EXPECT_TRUE(page.workflows[0].request_json.empty());
    EXPECT_EQ(store.list_audit(100, 0, "workflow", first.id).total, 1U);
}
TEST(ServerWorkflow, AdmissionCursorAndRunLinkCommitAtomicallyDespiteBackpressure) {
    Store store(":memory:", {}, [] { return epoch; });
    const auto flow = store.create_workflow(recurring());
    const auto occupied = store.create("{}", "", 0, 1);
    const auto blocked = store.dispatch_workflow(flow.id, 1, true, epoch + 180000, 1);
    EXPECT_FALSE(blocked.job);
    const auto pending = store.get_workflow(flow.id);
    EXPECT_EQ(pending.next_run_at_ms, epoch);
    EXPECT_EQ(pending.missed_slots, 0U);
    EXPECT_EQ(pending.skipped_slots, 0U);
    EXPECT_EQ(pending.admitted_runs, 0U);
    EXPECT_EQ(pending.last_dispatch_error, "backpressure");
    static_cast<void>(store.cancel(occupied.id));
    const auto admitted = store.dispatch_workflow(flow.id, 1, true, epoch + 180000, 1);
    ASSERT_TRUE(admitted.job);
    EXPECT_EQ(admitted.job->workflow_id, flow.id);
    EXPECT_EQ(admitted.job->workflow_slot_ms, epoch + 180000);
    EXPECT_EQ(admitted.job->actor_id, "alice");
    EXPECT_EQ(store.get(admitted.job->id).workflow_slot_ms, epoch + 180000);
    const auto current = store.get_workflow(flow.id);
    EXPECT_EQ(current.missed_slots, 3U);
    EXPECT_EQ(current.skipped_slots, 3U);
    EXPECT_EQ(current.admitted_runs, 1U);
    EXPECT_EQ(current.next_run_at_ms, epoch + 240000);
    EXPECT_TRUE(current.last_dispatch_error.empty());
    EXPECT_FALSE(store.dispatch_workflow(flow.id, 1, true, epoch + 180000, 1).job);
    const auto runs = store.list_workflow_runs(flow.id, 100, 0);
    ASSERT_EQ(runs.jobs.size(), 1U);
    EXPECT_TRUE(runs.jobs[0].request_json.empty());
}
TEST(ServerWorkflow, NonOverlapCoalescesRealSlotsAndRunBudgetCountsActualAdmissions) {
    Store store(":memory:", {}, [] { return epoch; });
    auto input = recurring(); input.max_runs = 2;
    const auto flow = store.create_workflow(input);
    const auto first = store.dispatch_workflow(flow.id, 1, true, epoch, 10);
    ASSERT_TRUE(first.job);
    EXPECT_FALSE(store.dispatch_workflow(flow.id, 1, true, epoch + 180000, 10).job);
    const auto skipped = store.get_workflow(flow.id);
    EXPECT_EQ(skipped.admitted_runs, 1U);
    EXPECT_EQ(skipped.missed_slots, 2U);
    EXPECT_EQ(skipped.skipped_slots, 3U);
    EXPECT_EQ(skipped.next_run_at_ms, epoch + 240000);
    static_cast<void>(store.cancel(first.job->id));
    const auto last = store.dispatch_workflow(flow.id, 1, true, epoch + 240000, 10);
    ASSERT_TRUE(last.job);
    EXPECT_EQ(store.get_workflow(flow.id).state, "draining");
    static_cast<void>(store.cancel(last.job->id));
    const auto complete = store.get_workflow(flow.id);
    EXPECT_EQ(complete.state, "completed");
    EXPECT_EQ(complete.cancelled, 2U);
    EXPECT_TRUE(store.due_workflows(epoch + 1000000).empty());
    EXPECT_THROW(static_cast<void>(store.set_workflow_paused(flow.id, 1, false)), Error);
}
TEST(ServerWorkflow, AutomaticPauseChangesLifecycleRevisionAndRejectsStaleDispatch) {
    Store store(":memory:", {}, [] { return epoch; });
    const auto flow = store.create_workflow(recurring());
    EXPECT_TRUE(store.dispatch_workflow(flow.id, 1, false, epoch, 10).changed);
    const auto paused = store.get_workflow(flow.id);
    EXPECT_FALSE(paused.enabled);
    EXPECT_EQ(paused.pause_reason, "creator_permission");
    EXPECT_EQ(paused.revision, 2U);
    EXPECT_THROW(static_cast<void>(store.set_workflow_paused(flow.id, 1, false)), Error);
    const auto resumed = store.set_workflow_paused(flow.id, 2, false, "admin");
    EXPECT_EQ(resumed.revision, 3U);
    EXPECT_FALSE(store.dispatch_workflow(flow.id, 2, true, epoch, 10).job);
    EXPECT_TRUE(store.dispatch_workflow(flow.id, 3, true, epoch, 10).job);
}
TEST(ServerWorkflow, ExactSavedAlphaAndArchiveGuardPreserveRetryIdentity) {
    Store store(":memory:", {}, [] { return epoch; });
    const auto alpha = store.create_alpha({"Signal", "", {}, "",
        R"({"mode":"synthetic_demo"})", "", "local", "alice"});
    auto input = recurring(); input.request_json.clear();
    input.alpha_id = alpha.id; input.alpha_revision = 1;
    const auto flow = store.create_workflow(input);
    EXPECT_EQ(flow.request_json, alpha.request_json);
    static_cast<void>(store.set_alpha_archived(alpha.id, 1, true));
    EXPECT_EQ(store.create_workflow(input).id, flow.id);
    EXPECT_FALSE(store.dispatch_workflow(flow.id, 1, true, epoch, 10).job);
    EXPECT_EQ(store.get_workflow(flow.id).pause_reason, "source_archived");
    input.idempotency_key = "new";
    EXPECT_THROW(static_cast<void>(store.create_workflow(input)), Error);
}
TEST(ServerWorkflow, ConcurrentConnectionsAndRestartCannotDuplicateOccurrence) {
    WorkflowFile file;
    std::string flow_id, job_id;
    {
        Store first(file.path.string(), {}, [] { return epoch; });
        Store second(file.path.string(), {}, [] { return epoch; });
        flow_id = first.create_workflow(recurring()).id;
        auto a = std::async(std::launch::async, [&] {
            return first.dispatch_workflow(flow_id, 1, true, epoch, 10);
        });
        auto b = std::async(std::launch::async, [&] {
            return second.dispatch_workflow(flow_id, 1, true, epoch, 10);
        });
        const auto left = a.get(), right = b.get();
        EXPECT_NE(left.job.has_value(), right.job.has_value());
        job_id = left.job ? left.job->id : right.job->id;
        EXPECT_EQ(first.list_workflow_runs(flow_id, 100, 0).total, 1U);
    }
    Store restored(file.path.string(), {}, [] { return epoch; });
    EXPECT_EQ(restored.get_workflow(flow_id).active_job_id, job_id);
    EXPECT_FALSE(restored.dispatch_workflow(flow_id, 1, true, epoch, 10).job);
    const auto claimed = restored.claim_due();
    ASSERT_TRUE(claimed);
    restored.recover();
    EXPECT_EQ(restored.get_workflow(flow_id).failed, 1U);
    EXPECT_TRUE(restored.dispatch_workflow(flow_id, 1, true, epoch + 60000, 10).job);
}
TEST(ServerWorkflow, InvalidConfigurationAndPagingCannotLeaveRows) {
    Store store(":memory:", {}, [] { return epoch; });
    for (const auto interval : {0ULL, 59999ULL, 2592000001ULL,
        std::numeric_limits<unsigned long long>::max()}) {
        auto input = recurring(); input.interval_ms = interval;
        EXPECT_THROW(static_cast<void>(store.create_workflow(input)), Error);
    }
    for (const auto count : {0U, 1001U}) {
        auto input = recurring(); input.max_runs = count;
        EXPECT_THROW(static_cast<void>(store.create_workflow(input)), Error);
    }
    for (const auto time : {0LL, -1LL, epoch + 365LL * 86400000 + 1}) {
        auto input = recurring(); input.first_run_at_ms = time;
        EXPECT_THROW(static_cast<void>(store.create_workflow(input)), Error);
    }
    auto input = recurring(); input.alpha_id = std::string(32, 'a'); input.alpha_revision = 1;
    EXPECT_THROW(static_cast<void>(store.create_workflow(input)), Error);
    EXPECT_EQ(store.list_workflows(100, 0).total, 0U);
    EXPECT_THROW(static_cast<void>(store.list_workflows(101, 0)), Error);
}
TEST(ServerWorkflow, SchedulerAuthorizesOutsideLocksAndPausesRevokedCreator) {
    Store store(":memory:", {}, [] { return epoch; });
    EventBus events;
    std::promise<void> checked;
    std::atomic<bool> signalled{};
    Scheduler* borrowed{};
    Scheduler scheduler(store, events,
        [](std::string_view, std::stop_token, const Progress&) { return "{}"; },
        1, 8, false, [&](std::string_view actor, std::int64_t now) {
            EXPECT_EQ(actor, "alice"); EXPECT_EQ(now, epoch);
            EXPECT_TRUE(borrowed->healthy());
            static_cast<void>(store.stats());
            if (!signalled.exchange(true)) checked.set_value();
            return false;
        });
    borrowed = &scheduler;
    scheduler.start();
    const auto flow = scheduler.create_workflow(recurring());
    EXPECT_EQ(checked.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    scheduler.shutdown();
    const auto current = store.get_workflow(flow.id);
    EXPECT_EQ(current.state, "paused");
    EXPECT_EQ(current.pause_reason, "creator_permission");
    EXPECT_EQ(current.admitted_runs, 0U);
    EXPECT_EQ(current.revision, 2U);
}
TEST(ServerWorkflow, SchedulerDispatchesLocalRecipeAndStopsJoinedRecurringTimer) {
    Store store(":memory:", {}, [] { return epoch; });
    EventBus events;
    std::promise<void> executed;
    Scheduler scheduler(store, events,
        [&](std::string_view recipe, std::stop_token, const Progress&) {
            EXPECT_EQ(recipe, R"({"mode":"synthetic_demo"})");
            executed.set_value(); return "{}";
        }, 1, 8);
    auto input = recurring(); input.actor_id = "local"; input.max_runs = 1;
    const auto flow = scheduler.create_workflow(input);
    EXPECT_EQ(executed.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    scheduler.shutdown();
    EXPECT_EQ(store.get_workflow(flow.id).admitted_runs, 1U);
    EXPECT_TRUE(store.due_workflows(epoch + 60000).empty());
    EXPECT_THROW(static_cast<void>(scheduler.create_workflow(input)), Error);
}
TEST(ServerWorkflow, OccurrenceAuditFailureRollsBackJobAndCursorAndImmutableLinksCannotChange) {
    WorkflowFile file;
    Store store(file.path.string(), {}, [] { return epoch; });
    const auto flow = store.create_workflow(recurring());
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, "CREATE TRIGGER fail_occurrence_audit BEFORE INSERT "
        "ON audit_records WHEN NEW.action='workflow.dispatched' "
        "BEGIN SELECT RAISE(ABORT,'fixture disk failure'); END;",
        nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_THROW(static_cast<void>(store.dispatch_workflow(flow.id, 1, true, epoch, 10)), Error);
    EXPECT_EQ(store.list(100, 0, "").total, 0U);
    EXPECT_EQ(store.get_workflow(flow.id).next_run_at_ms, epoch);
    EXPECT_EQ(store.list_workflow_runs(flow.id, 100, 0).total, 0U);
    ASSERT_EQ(sqlite3_exec(raw, "DROP TRIGGER fail_occurrence_audit;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    ASSERT_TRUE(store.dispatch_workflow(flow.id, 1, true, epoch, 10).job);
    for (const auto* sql : {"UPDATE workflow_definitions SET request_json='{}'",
        "DELETE FROM workflow_definitions", "UPDATE workflow_occurrences SET scheduled_slot_ms=1",
        "DELETE FROM workflow_occurrences", "UPDATE jobs SET workflow_id='forged'",
        "UPDATE jobs SET workflow_slot_ms=1"}) {
        EXPECT_EQ(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr), SQLITE_CONSTRAINT);
    }
}
TEST(ServerWorkflow, ConcurrentPauseCASAdmitsOneWriterWithoutCancellingActiveRun) {
    Store store(":memory:", {}, [] { return epoch; });
    const auto flow = store.create_workflow(recurring());
    const auto run = store.dispatch_workflow(flow.id, 1, true, epoch, 10);
    ASSERT_TRUE(run.job);
    std::atomic<unsigned> succeeded{};
    const auto pause = [&] {
        try {
            static_cast<void>(store.set_workflow_paused(flow.id, 1, true, "admin"));
            ++succeeded;
        } catch (const Error& error) { EXPECT_EQ(error.code(), ErrorCode::conflict); }
    };
    auto a = std::async(std::launch::async, pause), b = std::async(std::launch::async, pause);
    a.get(); b.get();
    EXPECT_EQ(succeeded.load(), 1U);
    EXPECT_EQ(store.get(run.job->id).status, "QUEUED");
    EXPECT_TRUE(store.due_workflows(epoch + 60000).empty());
    const auto resumed = store.set_workflow_paused(flow.id, 2, false, "admin");
    EXPECT_EQ(resumed.admitted_runs, 1U);
    EXPECT_EQ(resumed.revision, 3U);
}
TEST(ServerWorkflow, ArithmeticOverflowPausesInsteadOfWrappingAndClockRollbackNeverRewinds) {
    Store store(":memory:", {}, [] { return epoch; });
    const auto flow = store.create_workflow(recurring());
    EXPECT_FALSE(store.dispatch_workflow(flow.id, 1, true, epoch - 1, 10).job);
    EXPECT_EQ(store.get_workflow(flow.id).next_run_at_ms, epoch);
    EXPECT_FALSE(store.dispatch_workflow(flow.id, 1, true,
        std::numeric_limits<std::int64_t>::max(), 10).job);
    const auto paused = store.get_workflow(flow.id);
    EXPECT_EQ(paused.pause_reason, "schedule_overflow");
    EXPECT_EQ(paused.revision, 2U);
    EXPECT_EQ(paused.admitted_runs, 0U);
}
TEST(ServerWorkflow, CancelledOccurrenceCannotOverlapExecutorStillCooperativelyStopping) {
    std::atomic<std::int64_t> now{epoch};
    Store store(":memory:", {}, [&] { return now.load(); });
    EventBus events;
    std::promise<void> first_started, first_stopping, release_first, second_started;
    auto release = release_first.get_future().share();
    std::atomic<unsigned> calls{};
    Scheduler scheduler(store, events,
        [&](std::string_view, std::stop_token stop, const Progress&) {
            if (calls.fetch_add(1) == 0) {
                first_started.set_value();
                std::mutex mutex;
                std::condition_variable_any changed;
                std::unique_lock lock(mutex);
                changed.wait(lock, stop, [] { return false; });
                first_stopping.set_value();
                static_cast<void>(release.wait_for(std::chrono::seconds(3)));
            } else second_started.set_value();
            return "{}";
        }, 2, 8);
    auto input = recurring(); input.actor_id = "local"; input.max_runs = 2;
    const auto flow = scheduler.create_workflow(input);
    EXPECT_EQ(first_started.get_future().wait_for(std::chrono::seconds(3)),
        std::future_status::ready);
    const auto first = store.get_workflow(flow.id).active_job_id;
    static_cast<void>(scheduler.cancel(first));
    EXPECT_EQ(first_stopping.get_future().wait_for(std::chrono::seconds(3)),
        std::future_status::ready);
    now.store(epoch + 60000);
    auto second = second_started.get_future();
    EXPECT_EQ(second.wait_for(std::chrono::milliseconds(700)), std::future_status::timeout);
    EXPECT_EQ(store.get_workflow(flow.id).admitted_runs, 1U);
    release_first.set_value();
    EXPECT_EQ(second.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    scheduler.shutdown();
    EXPECT_EQ(store.get_workflow(flow.id).admitted_runs, 2U);
}
TEST(ServerWorkflow, VersionSixMigrationPreservesPriorEntitiesAndAuditHistory) {
    WorkflowFile file;
    std::string workspace_id, alpha_id, experiment_id, job_id;
    std::uint64_t audits{};
    {
        Store store(file.path.string(), {}, [] { return epoch; });
        workspace_id = store.create_workspace("Preserved", "",
            R"({"schema_version":1,"layout":{}})", "alice").id;
        alpha_id = store.create_alpha({"Preserved signal", "", {}, "",
            R"({"mode":"synthetic_demo"})", "", "local", "alice"}).id;
        const auto batch = store.create_experiment({"Preserved batch", "",
            {{"Run", "", alpha_id, 1}}, "", 0, "bob"}, 10);
        experiment_id = batch.id; job_id = batch.jobs[0].id;
        audits = store.list_audit(100, 0).total;
    }
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    {
        struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
        ASSERT_EQ(sqlite3_exec(raw, (std::string(test_detail::remove_workflows) +
            "PRAGMA user_version=6;").c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
    Store restored(file.path.string(), {}, [] { return epoch; });
    EXPECT_EQ(restored.get_workspace(workspace_id).actor_id, "alice");
    EXPECT_EQ(restored.get_alpha(alpha_id).actor_id, "alice");
    EXPECT_EQ(restored.get_experiment(experiment_id).jobs[0].id, job_id);
    EXPECT_EQ(restored.get(job_id).actor_id, "bob");
    EXPECT_TRUE(restored.get(job_id).workflow_id.empty());
    EXPECT_EQ(restored.list_audit(100, 0).total, audits);
    EXPECT_EQ(restored.list_workflows(100, 0).total, 0U);
    EXPECT_TRUE(restored.create_workflow(recurring()).enabled);
}
TEST(ServerWorkflow, CancellationDuringAuthorityLookupCannotUseStaleActiveRunSnapshot) {
    std::atomic<std::int64_t> now{epoch};
    Store store(":memory:", {}, [&] { return now.load(); });
    EventBus events;
    std::promise<void> first_started, first_stopping, release_first, second_started;
    std::promise<void> authority_entered, release_authority;
    auto release = release_first.get_future().share();
    auto authorization_release = release_authority.get_future().share();
    std::atomic<unsigned> calls{};
    std::atomic<bool> gated{};
    Scheduler scheduler(store, events,
        [&](std::string_view, std::stop_token stop, const Progress&) {
            if (calls.fetch_add(1) == 0) {
                first_started.set_value();
                std::mutex mutex;
                std::condition_variable_any changed;
                std::unique_lock lock(mutex);
                changed.wait(lock, stop, [] { return false; });
                first_stopping.set_value();
                static_cast<void>(release.wait_for(std::chrono::seconds(4)));
            } else second_started.set_value();
            return "{}";
        }, 2, 8, true, [&](std::string_view, std::int64_t timestamp) {
            if (timestamp == epoch + 60000 && !gated.exchange(true)) {
                authority_entered.set_value();
                static_cast<void>(authorization_release.wait_for(std::chrono::seconds(4)));
            }
            return true;
        });
    auto input = recurring(); input.actor_id = "local"; input.max_runs = 2;
    const auto flow = scheduler.create_workflow(input);
    EXPECT_EQ(first_started.get_future().wait_for(std::chrono::seconds(3)),
        std::future_status::ready);
    const auto first = store.get_workflow(flow.id).active_job_id;
    now.store(epoch + 60000);
    EXPECT_EQ(authority_entered.get_future().wait_for(std::chrono::seconds(3)),
        std::future_status::ready);
    // The timer holds a snapshot saying RUNNING; cancel changes only the authoritative store.
    static_cast<void>(scheduler.cancel(first));
    EXPECT_EQ(first_stopping.get_future().wait_for(std::chrono::seconds(3)),
        std::future_status::ready);
    release_authority.set_value();
    auto second = second_started.get_future();
    EXPECT_EQ(second.wait_for(std::chrono::milliseconds(700)), std::future_status::timeout);
    EXPECT_EQ(store.get_workflow(flow.id).admitted_runs, 1U);
    release_first.set_value();
    EXPECT_EQ(second.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    scheduler.shutdown();
    EXPECT_EQ(store.get_workflow(flow.id).admitted_runs, 2U);
}
} // namespace atx::server
