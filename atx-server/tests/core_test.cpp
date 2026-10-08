#include <atx/server/core.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace atx::server {
namespace {
class TemporaryDatabase final {
public:
    TemporaryDatabase() {
        std::random_device random;
        root_ = std::filesystem::temp_directory_path() /
            ("atx-server-test-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!std::filesystem::create_directory(root_)) {
            throw std::runtime_error("cannot create unique temporary database directory");
        }
    }
    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    [[nodiscard]] std::string path() const { return (root_ / "jobs.db").string(); }
private:
    std::filesystem::path root_;
};
} // namespace

TEST(ServerStore, IdempotencyIsDurableAndRejectsDifferentRequests) {
    Store store(":memory:");
    const auto first = store.create("{\"mode\":\"synthetic_demo\"}", "same", 0, 2);
    const auto again = store.create("{\"mode\":\"synthetic_demo\"}", "same", 0, 2);
    EXPECT_EQ(first.id, again.id);
    EXPECT_THROW(static_cast<void>(store.create("{}", "same", 0, 2)), Error);
    EXPECT_EQ(store.list(10, 0, "").total, 1U);
}

TEST(ServerStore, ReadCacheInvalidatesOnTransitionAndListOmitsResults) {
    Store store(":memory:");
    auto job = store.create("{}", "", 0, 2);
    EXPECT_EQ(store.get(job.id).status, "QUEUED");
    EXPECT_EQ(store.get(job.id).status, "QUEUED");
    const auto claimed = store.claim_due();
    ASSERT_TRUE(claimed);
    store.finish(job.id, "SUCCEEDED", "{\"large\":1}", "");
    EXPECT_EQ(store.get(job.id).status, "SUCCEEDED");
    EXPECT_FALSE(store.get(job.id).result_json.empty());
    EXPECT_TRUE(store.list(10, 0, "").jobs.front().result_json.empty());
    EXPECT_GT(store.stats().cache_hits, 0U);
}

TEST(ServerStore, CapacityAndCancellationAreAtomic) {
    Store store(":memory:");
    const auto job = store.create("{}", "", 0, 1);
    EXPECT_THROW(static_cast<void>(store.create("{}", "", 0, 1)), Error);
    EXPECT_EQ(store.cancel(job.id).status, "CANCELLED");
    EXPECT_NO_THROW(static_cast<void>(store.create("{}", "", 0, 1)));
    EXPECT_FALSE(store.claim_due()->id.empty());
    store.finish(job.id, "SUCCEEDED", "{}", "");
    EXPECT_EQ(store.get(job.id).status, "CANCELLED");
}

TEST(ServerStore, OverdueScheduledWorkPrecedesNewImmediateSubmissions) {
    std::int64_t now = 100;
    Store store(":memory:", {}, [&] { return now; });
    const auto scheduled = store.create("{}", "scheduled", 150, 3);
    now = 200;
    const auto immediate = store.create("{}", "immediate", 0, 3);
    const auto first = store.claim_due();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->id, scheduled.id);
    const auto second = store.claim_due();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->id, immediate.id);
}

TEST(ServerStore, PastScheduledTimestampCannotJumpOlderAdmittedWork) {
    std::int64_t now = 100;
    Store store(":memory:", {}, [&] { return now; });
    const auto older = store.create("{}", "older", 0, 3);
    now = 200;
    const auto newest = store.create("{}", "newest", 1, 3);
    EXPECT_EQ(store.get(older.id).created_at_ms, 100);
    EXPECT_EQ(store.get(newest.id).created_at_ms, 200);
    const auto first = store.claim_due();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->id, older.id);
    const auto second = store.claim_due();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->id, newest.id);
}

TEST(ServerStore, DiskRestartPreservesQueuedJobsAndFailsInterruptedWork) {
    TemporaryDatabase database;
    std::string running;
    std::string queued;
    {
        Store store(database.path());
        running = store.create("{}", "first", 0, 3).id;
        ASSERT_TRUE(store.claim_due());
        queued = store.create("{}", "later", unix_ms() + 86400000, 3).id;
    }
    Store reopened(database.path());
    reopened.recover();
    EXPECT_EQ(reopened.get(running).status, "FAILED");
    EXPECT_FALSE(reopened.get(running).error.empty());
    EXPECT_EQ(reopened.get(queued).status, "QUEUED");
    EXPECT_FALSE(reopened.claim_due());
}

TEST(ServerStore, CacheEvictionAndDisabledTtlRespectByteAndEntryCaps) {
    Store store(":memory:", {.entries = 1, .bytes = 1024, .ttl = std::chrono::seconds(10)});
    const auto first = store.create("{}", "", 0, 3);
    const auto second = store.create("{}", "", 0, 3);
    static_cast<void>(store.get(first.id));
    static_cast<void>(store.get(second.id));
    static_cast<void>(store.get(first.id));
    EXPECT_EQ(store.stats().cache_entries, 1U);
    EXPECT_LE(store.stats().cache_bytes, 1024U);
    EXPECT_EQ(store.stats().cache_misses, 3U);
    Store disabled(":memory:", {.entries = 1, .bytes = 1024,
                                 .ttl = std::chrono::milliseconds(0)});
    const auto job = disabled.create("{}", "", 0, 3);
    static_cast<void>(disabled.get(job.id));
    static_cast<void>(disabled.get(job.id));
    EXPECT_EQ(disabled.stats().cache_entries, 0U);
    EXPECT_EQ(disabled.stats().cache_hits, 0U);
}

TEST(ServerStore, CacheResourceConfigurationHasFiniteBounds) {
    EXPECT_THROW(Store(":memory:", {.entries = 100001}), Error);
    EXPECT_THROW(Store(":memory:", {.entries = 1, .bytes = 1024ULL * 1024 * 1024 + 1}), Error);
    EXPECT_THROW(Store(":memory:", {.ttl = std::chrono::milliseconds(-1)}), Error);
}

TEST(ServerStore, ConcurrentIdempotentCreatesAdmitExactlyOneJob) {
    Store store(":memory:");
    std::vector<std::future<Job>> pending;
    for (int i = 0; i < 16; ++i) {
        pending.push_back(std::async(std::launch::async, [&] {
            return store.create("{}", "shared", 0, 1);
        }));
    }
    const auto expected = pending.front().get().id;
    for (std::size_t i = 1; i < pending.size(); ++i) EXPECT_EQ(pending[i].get().id, expected);
    EXPECT_EQ(store.list(10, 0, "").total, 1U);
}

TEST(ServerStore, ProgressAndPaginationRejectInvalidBounds) {
    Store store(":memory:");
    EXPECT_THROW(static_cast<void>(store.list(101, 0, "")), Error);
    EXPECT_THROW(static_cast<void>(store.list(10, 0, "UNKNOWN")), Error);
    EXPECT_THROW(static_cast<void>(store.list(10, 1000001, "")), Error);
    const auto job = store.create("{}", "", 0, 3);
    ASSERT_TRUE(store.claim_due());
    EXPECT_THROW(store.progress(job.id, -0.1), Error);
    EXPECT_THROW(store.progress(job.id, 1.1), Error);
    store.progress(job.id, 0.8);
    store.progress(job.id, 0.2);
    EXPECT_DOUBLE_EQ(store.get(job.id).progress, 0.8);
}

TEST(ServerStore, EmptyStringViewsRemainEmptyTextRatherThanSqlNull) {
    Store store(":memory:");
    const auto job = store.create("{}", std::string_view{}, 0, 2);
    ASSERT_TRUE(store.claim_due());
    store.finish(job.id, "FAILED", std::string_view{}, std::string_view{});
    const auto result = store.get(job.id);
    EXPECT_EQ(result.status, "FAILED");
    EXPECT_TRUE(result.result_json.empty());
    EXPECT_TRUE(result.error.empty());
}

TEST(ServerEvents, ReplayReportsGapsAndFiltersChannels) {
    EventBus events(2);
    events.publish("jobs", "created", "a", "{}");
    events.publish("system", "ready", "", "{}");
    events.publish("jobs", "finished", "a", "{}");
    const auto replay = events.read(0, "jobs", "a");
    ASSERT_EQ(replay.size(), 2U);
    EXPECT_TRUE(replay.front().gap);
    EXPECT_EQ(replay.back().type, "finished");
    const auto restart = events.read(500, "all", "");
    ASSERT_EQ(restart.size(), 1U);
    EXPECT_TRUE(restart.front().gap);
}

TEST(ServerEvents, DurableCursorCannotOverlapAfterRestart) {
    TemporaryDatabase database;
    std::uint64_t cursor{};
    {
        Store store(database.path());
        EventBus events(16, &store);
        events.publish("jobs", "first", "a", "{}");
        cursor = events.sequence();
    }
    Store reopened(database.path());
    EventBus events(16, &reopened);
    auto replay = events.read(cursor, "all", "");
    ASSERT_EQ(replay.size(), 1U);
    EXPECT_TRUE(replay.front().gap);
    events.publish("jobs", "later", "a", "{}");
    replay = events.read(cursor, "all", "");
    ASSERT_EQ(replay.size(), 2U);
    EXPECT_TRUE(replay.front().gap);
    EXPECT_GT(replay.back().sequence, cursor);
}

TEST(ServerEvents, DurableAllocatorExtendsBlocksWithoutReusingSequenceIds) {
    Store store(":memory:");
    EventBus events(16, &store);
    auto previous = events.sequence();
    for (int i = 0; i < 4100; ++i) {
        events.publish("jobs", "progress", "a", "{}");
        EXPECT_GT(events.sequence(), previous);
        previous = events.sequence();
    }
    EXPECT_GT(store.reserve_event_sequences(1), previous);
    EXPECT_THROW(static_cast<void>(store.reserve_event_sequences(0)), Error);
}

TEST(ServerScheduler, CancellationStopsExecutorAndNeverBecomesSuccess) {
    Store store(":memory:");
    EventBus events(16);
    std::promise<void> started;
    Scheduler scheduler(store, events, [&started](std::string_view,
        std::stop_token stop, const Progress&) {
        started.set_value();
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        wake.wait(lock, stop, [] { return false; });
        return std::string("{}");
    }, 1, 8);
    const auto job = scheduler.submit("{}", "", 0);
    ASSERT_EQ(started.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(scheduler.cancel(job.id).status, "CANCELLED");
    scheduler.shutdown();
    EXPECT_EQ(store.get(job.id).status, "CANCELLED");
}

TEST(ServerScheduler, ShutdownStopsExecutorAndPersistsFailure) {
    Store store(":memory:");
    EventBus events(16);
    std::promise<void> started;
    Scheduler scheduler(store, events, [&started](std::string_view,
        std::stop_token stop, const Progress&) {
        started.set_value();
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        wake.wait(lock, stop, [] { return false; });
        return std::string("{}");
    }, 1, 8);
    const auto job = scheduler.submit("{}", "", 0);
    ASSERT_EQ(started.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    scheduler.shutdown();
    EXPECT_EQ(store.get(job.id).status, "FAILED");
    EXPECT_FALSE(store.get(job.id).error.empty());
    EXPECT_THROW(static_cast<void>(scheduler.submit("{}", "", 0)), Error);
}

TEST(ServerScheduler, FutureJobRemainsQueuedAcrossShutdownAndBadJsonIsRejected) {
    Store store(":memory:");
    EventBus events(16);
    std::atomic<unsigned> calls{};
    Scheduler scheduler(store, events,
        [&calls](std::string_view, std::stop_token, const Progress&) {
        ++calls;
        return std::string("{}");
    }, 1, 8);
    EXPECT_THROW(static_cast<void>(scheduler.submit("[]", "", 0)), Error);
    EXPECT_THROW(static_cast<void>(scheduler.submit("invalid", "", 0)), Error);
    EXPECT_THROW(static_cast<void>(scheduler.submit("{\"a\":1,\"a\":2}", "", 0)), Error);
    EXPECT_THROW(static_cast<void>(
        scheduler.submit("{\"a\":[{\"x\":1,\"x\":2}]}", "", 0)), Error);
    EXPECT_THROW(static_cast<void>(
        scheduler.submit(std::string(33, '[') + std::string(33, ']'), "", 0)), Error);
    const auto job = scheduler.submit("{}", "", unix_ms() + 86400000);
    scheduler.shutdown();
    EXPECT_EQ(store.get(job.id).status, "QUEUED");
    EXPECT_EQ(calls.load(), 0U);
}

TEST(ServerScheduler, DeferredWorkersDoNotClaimDurableJobsBeforeListenerStartup) {
    Store store(":memory:");
    EventBus events(16);
    const auto queued = store.create("{}", "", 0, 8);
    std::atomic<unsigned> calls{};
    Scheduler scheduler(store, events,
        [&calls](std::string_view, std::stop_token, const Progress&) {
            ++calls;
            return std::string("{}");
        }, 1, 8, false);
    EXPECT_FALSE(scheduler.healthy());
    EXPECT_EQ(store.get(queued.id).status, "QUEUED");
    EXPECT_EQ(calls.load(), 0U);
    scheduler.shutdown();
    EXPECT_EQ(store.get(queued.id).status, "QUEUED");
    EXPECT_THROW(scheduler.start(), Error);
}

TEST(ServerScheduler, ExplicitStartExecutesDurableQueueAndIsIdempotent) {
    Store store(":memory:");
    EventBus events(16);
    std::promise<void> started;
    static_cast<void>(store.create("{}", "", 0, 8));
    Scheduler scheduler(store, events,
        [&started](std::string_view, std::stop_token, const Progress&) {
            started.set_value();
            return std::string("{}");
        }, 1, 8, false);
    scheduler.start();
    scheduler.start();
    ASSERT_EQ(started.get_future().wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_TRUE(scheduler.healthy());
    scheduler.shutdown();
}
} // namespace atx::server
