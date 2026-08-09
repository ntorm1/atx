#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <latch>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/agent/database.hpp"
#include "atx/core/datetime.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/kb/knowledge_base.hpp"

namespace atxtest_agent_db {
namespace {

[[nodiscard]] std::filesystem::path database_path() {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto directory = std::filesystem::temp_directory_path() / "atx_agent_db_tests";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const auto path =
      directory / (std::string{info->test_suite_name()} + "_" + info->name() + ".sqlite");
  std::filesystem::remove(path, error);
  std::filesystem::remove(path.string() + "-wal", error);
  std::filesystem::remove(path.string() + "-shm", error);
  return path;
}

[[nodiscard]] atx::agent::EventConsumerFleetValidator
fleet_validator(const atx::agent::EventConsumerFleetStatus &fleet) {
  return atx::agent::EventConsumerFleetValidator{fleet.workspace, fleet.event_high_watermark,
                                                 fleet.consumer_state_revision,
                                                 fleet.next_dynamic_transition_at};
}

struct RunAndAgent {
  std::string run_id;
  std::string agent_id;
};

[[nodiscard]] atx::kb::Submission research(std::string title, std::string text) {
  atx::kb::Submission input;
  input.title = std::move(title);
  input.raw_text = std::move(text);
  input.submitted_by = "test-agent";
  return input;
}

[[nodiscard]] RunAndAgent initialize_run(atx::agent::AgentDatabase &database,
                                         std::string agent_id = "agent-one") {
  auto run = database.create_run("Improve the knowledge system");
  EXPECT_TRUE(run) << run.error().to_string();
  auto agent = database.register_agent(run->id, agent_id, "implementation", "cpp,sqlite");
  EXPECT_TRUE(agent) << agent.error().to_string();
  return {run->id, agent->id};
}

} // namespace

TEST(AgentDatabase, DependenciesGateAtomicClaimsAndCompletion) {
  auto opened = atx::agent::AgentDatabase::open_memory("workspace-a");
  ASSERT_TRUE(opened) << opened.error().to_string();
  const auto [run_id, agent_id] = initialize_run(*opened);
  atx::agent::TaskSpec research;
  research.run_id = run_id;
  research.title = "Research competitors";
  research.priority = 10;
  auto first = opened->add_task(research);
  ASSERT_TRUE(first) << first.error().to_string();
  atx::agent::TaskSpec implementation;
  implementation.run_id = run_id;
  implementation.title = "Implement evidence changes";
  implementation.dependencies = {first->id};
  auto second = opened->add_task(implementation);
  ASSERT_TRUE(second) << second.error().to_string();

  auto claimed_first = opened->claim_next(agent_id);
  ASSERT_TRUE(claimed_first) << claimed_first.error().to_string();
  EXPECT_EQ(claimed_first->id, first->id);
  EXPECT_EQ(claimed_first->status, "leased");
  EXPECT_EQ(claimed_first->attempts, 1);
  auto recovered_claim = opened->claim_next(agent_id);
  ASSERT_TRUE(recovered_claim);
  EXPECT_EQ(recovered_claim->id, claimed_first->id);
  EXPECT_EQ(recovered_claim->lease_token, claimed_first->lease_token);
  EXPECT_EQ(recovered_claim->attempts, 1);
  auto stale = opened->complete_task(claimed_first->id, agent_id, "wrong-token");
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code(), atx::core::ErrorCode::Unavailable);
  ASSERT_TRUE(opened->complete_task(claimed_first->id, agent_id, claimed_first->lease_token,
                                    "src_research"));
  EXPECT_TRUE(opened->complete_task(claimed_first->id, agent_id, claimed_first->lease_token,
                                    "src_research"));

  auto claimed_second = opened->claim_next(agent_id);
  ASSERT_TRUE(claimed_second) << claimed_second.error().to_string();
  EXPECT_EQ(claimed_second->id, second->id);
  ASSERT_TRUE(opened->complete_task(claimed_second->id, agent_id, claimed_second->lease_token));
  EXPECT_TRUE(opened->finish_run(run_id));
  auto events = opened->events_after(0);
  ASSERT_TRUE(events);
  for (const auto &event : *events) {
    if (event.type.starts_with("task.")) {
      EXPECT_EQ(event.subject, "tasks/" + event.task_id);
    }
  }
  EXPECT_TRUE(opened->verify_integrity());
}

TEST(AgentDatabase, VerifiedTaskCompletionCertifiesExactKnowledgeArtifact) {
  auto coordination = atx::agent::AgentDatabase::open_memory();
  auto knowledge = atx::kb::KnowledgeBase::open_memory();
  ASSERT_TRUE(coordination);
  ASSERT_TRUE(knowledge);
  const auto [run_id, agent_id] = initialize_run(*coordination);
  atx::agent::TaskSpec spec;
  spec.run_id = run_id;
  spec.title = "Produce certified result";
  auto task = coordination->add_task(spec);
  auto lease = coordination->claim_next(agent_id);
  auto submitted = knowledge->submit(
      research("Certified task result",
               "This exact observation is the immutable artifact produced by the completed task."));
  ASSERT_TRUE(task);
  ASSERT_TRUE(lease);
  ASSERT_TRUE(submitted);
  auto wrong = coordination->complete_task_verified(task->id, agent_id, lease->lease_token,
                                                    submitted->source_id,
                                                    submitted->observation_id + 1, *knowledge);
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().code(), atx::core::ErrorCode::InvalidArgument);
  ASSERT_TRUE(coordination->complete_task_verified(task->id, agent_id, lease->lease_token,
                                                   submitted->source_id, submitted->observation_id,
                                                   *knowledge));
  ASSERT_TRUE(coordination->complete_task_verified(task->id, agent_id, lease->lease_token,
                                                   submitted->source_id, submitted->observation_id,
                                                   *knowledge));
  auto completed = coordination->get_task(task->id);
  ASSERT_TRUE(completed);
  EXPECT_EQ(completed->result_source_id, submitted->source_id);
  EXPECT_EQ(completed->result_observation_id, submitted->observation_id);
  EXPECT_EQ(completed->result_content_hash, submitted->content_hash);
  EXPECT_EQ(completed->result_evidence_status, "verified");
  EXPECT_TRUE(coordination->verify_evidence_links(*knowledge));
  EXPECT_TRUE(coordination->verify_integrity());
}

TEST(AgentDatabase, ConcurrentWorkersCannotDoubleClaimATask) {
  const auto path = database_path();
  std::string run_id;
  std::string task_id;
  constexpr std::size_t worker_count = 8;
  {
    auto coordinator = atx::agent::AgentDatabase::open(path.string(), "shared");
    ASSERT_TRUE(coordinator) << coordinator.error().to_string();
    auto run = coordinator->create_run("Concurrent scheduling");
    ASSERT_TRUE(run);
    run_id = run->id;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      auto registered =
          coordinator->register_agent(run_id, "worker-" + std::to_string(worker), "worker");
      ASSERT_TRUE(registered) << registered.error().to_string();
    }
    atx::agent::TaskSpec spec;
    spec.run_id = run_id;
    spec.title = "Exactly once lease";
    auto task = coordinator->add_task(spec);
    ASSERT_TRUE(task);
    task_id = task->id;
  }

  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::string> claimed;
  std::vector<std::string> unexpected_errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "shared");
      if (!connection) {
        std::lock_guard lock{mutex};
        unexpected_errors.push_back(connection.error().to_string());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto result = connection->claim_next("worker-" + std::to_string(worker));
      std::lock_guard lock{mutex};
      if (result) {
        claimed.push_back(result->id);
      } else if (result.error().code() != atx::core::ErrorCode::NotFound) {
        unexpected_errors.push_back(result.error().to_string());
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();
  EXPECT_TRUE(unexpected_errors.empty());
  ASSERT_EQ(claimed.size(), 1U);
  EXPECT_EQ(claimed.front(), task_id);
  auto reopened = atx::agent::AgentDatabase::open(path.string(), "shared");
  ASSERT_TRUE(reopened);
  EXPECT_TRUE(reopened->verify_integrity());
}

TEST(AgentDatabase, FailureRetriesThenTerminatesAtAttemptLimit) {
  auto opened = atx::agent::AgentDatabase::open_memory();
  ASSERT_TRUE(opened);
  const auto [run_id, agent_id] = initialize_run(*opened);
  atx::agent::TaskSpec spec;
  spec.run_id = run_id;
  spec.title = "Flaky task";
  spec.max_attempts = 2;
  auto created = opened->add_task(spec);
  ASSERT_TRUE(created);
  auto first = opened->claim_next(agent_id);
  ASSERT_TRUE(first);
  EXPECT_TRUE(opened->fail_task(first->id, agent_id, first->lease_token, "retryable"));
  EXPECT_TRUE(opened->fail_task(first->id, agent_id, first->lease_token, "retryable"));
  auto queued = opened->get_task(first->id);
  ASSERT_TRUE(queued);
  EXPECT_EQ(queued->status, "queued");
  auto second = opened->claim_next(agent_id);
  ASSERT_TRUE(second);
  EXPECT_TRUE(opened->fail_task(second->id, agent_id, second->lease_token, "terminal"));
  auto failed = opened->get_task(second->id);
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->status, "failed");
  auto none = opened->claim_next(agent_id);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().code(), atx::core::ErrorCode::NotFound);
}

TEST(AgentDatabase, TerminalDependencyFailureCancelsEveryUnreachableDescendantExactlyOnce) {
  auto opened = atx::agent::AgentDatabase::open_memory();
  ASSERT_TRUE(opened);
  const auto [run_id, agent_id] = initialize_run(*opened);
  atx::agent::TaskSpec root_spec;
  root_spec.run_id = run_id;
  root_spec.title = "Terminal prerequisite";
  root_spec.priority = 100;
  root_spec.max_attempts = 1;
  auto root = opened->add_task(root_spec);
  ASSERT_TRUE(root);

  atx::agent::TaskSpec child_spec;
  child_spec.run_id = run_id;
  child_spec.title = "Direct descendant";
  child_spec.dependencies = {root->id};
  auto child = opened->add_task(child_spec);
  ASSERT_TRUE(child);
  auto sibling_spec = child_spec;
  sibling_spec.title = "Second direct descendant";
  auto sibling = opened->add_task(sibling_spec);
  ASSERT_TRUE(sibling);
  atx::agent::TaskSpec grandchild_spec;
  grandchild_spec.run_id = run_id;
  grandchild_spec.title = "Transitive descendant";
  grandchild_spec.dependencies = {child->id};
  auto grandchild = opened->add_task(grandchild_spec);
  ASSERT_TRUE(grandchild);
  atx::agent::TaskSpec independent_spec;
  independent_spec.run_id = run_id;
  independent_spec.title = "Independent work";
  auto independent = opened->add_task(independent_spec);
  ASSERT_TRUE(independent);

  auto lease = opened->claim_next(agent_id);
  ASSERT_TRUE(lease);
  ASSERT_EQ(lease->id, root->id);
  ASSERT_TRUE(opened->fail_task(root->id, agent_id, lease->lease_token, "terminal cause"));
  ASSERT_TRUE(opened->fail_task(root->id, agent_id, lease->lease_token, "terminal cause"));
  for (const auto *task : {&*child, &*sibling, &*grandchild}) {
    auto cancelled = opened->get_task(task->id);
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled->status, "cancelled");
    EXPECT_NE(cancelled->last_error.find(root->id), std::string::npos);
  }
  auto next = opened->claim_next(agent_id);
  ASSERT_TRUE(next) << next.error().to_string();
  EXPECT_EQ(next->id, independent->id);

  auto events = opened->events_after(0);
  ASSERT_TRUE(events);
  const auto cancellations = std::count_if(events->begin(), events->end(), [](const auto &event) {
    return event.type == "task.cancelled";
  });
  EXPECT_EQ(cancellations, 3);
  auto invalid_spec = child_spec;
  invalid_spec.title = "Added after terminal failure";
  auto invalid = opened->add_task(invalid_spec);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_TRUE(opened->verify_integrity());
}

TEST(AgentDatabase, ConcurrentTerminalFailuresCancelSharedDescendantsExactlyOnce) {
  const auto path = database_path();
  std::string run_id;
  std::string first_root_id;
  std::string second_root_id;
  std::string shared_child_id;
  std::string grandchild_id;
  std::string first_lease_token;
  std::string second_lease_token;
  {
    auto coordinator = atx::agent::AgentDatabase::open(path.string(), "failure-race");
    ASSERT_TRUE(coordinator) << coordinator.error().to_string();
    const auto initialized = initialize_run(*coordinator, "failure-agent-a");
    run_id = initialized.run_id;
    auto second_agent = coordinator->register_agent(run_id, "failure-agent-b", "implementation");
    ASSERT_TRUE(second_agent);

    atx::agent::TaskSpec first_root_spec;
    first_root_spec.run_id = run_id;
    first_root_spec.title = "First terminal root";
    first_root_spec.priority = 100;
    first_root_spec.max_attempts = 1;
    auto first_root = coordinator->add_task(first_root_spec);
    ASSERT_TRUE(first_root);
    first_root_id = first_root->id;

    auto second_root_spec = first_root_spec;
    second_root_spec.title = "Second terminal root";
    second_root_spec.priority = 90;
    auto second_root = coordinator->add_task(second_root_spec);
    ASSERT_TRUE(second_root);
    second_root_id = second_root->id;

    atx::agent::TaskSpec shared_child_spec;
    shared_child_spec.run_id = run_id;
    shared_child_spec.title = "Shared descendant";
    shared_child_spec.dependencies = {first_root_id, second_root_id};
    auto shared_child = coordinator->add_task(shared_child_spec);
    ASSERT_TRUE(shared_child);
    shared_child_id = shared_child->id;

    atx::agent::TaskSpec grandchild_spec;
    grandchild_spec.run_id = run_id;
    grandchild_spec.title = "Shared transitive descendant";
    grandchild_spec.dependencies = {shared_child_id};
    auto grandchild = coordinator->add_task(grandchild_spec);
    ASSERT_TRUE(grandchild);
    grandchild_id = grandchild->id;

    auto first_lease = coordinator->claim_next(initialized.agent_id);
    ASSERT_TRUE(first_lease);
    ASSERT_EQ(first_lease->id, first_root_id);
    first_lease_token = first_lease->lease_token;
    auto second_lease = coordinator->claim_next(second_agent->id);
    ASSERT_TRUE(second_lease);
    ASSERT_EQ(second_lease->id, second_root_id);
    second_lease_token = second_lease->lease_token;
  }

  std::latch ready{2};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::string> errors;
  auto fail_root = [&](std::string_view task_id, std::string_view agent_id,
                       std::string_view lease_token) {
    auto connection = atx::agent::AgentDatabase::open(path.string(), "failure-race");
    if (!connection) {
      std::lock_guard lock{mutex};
      errors.push_back(connection.error().to_string());
      ready.count_down();
      return;
    }
    ready.count_down();
    start.wait();
    auto failed = connection->fail_task(task_id, agent_id, lease_token, "terminal race");
    if (!failed) {
      std::lock_guard lock{mutex};
      errors.push_back(failed.error().to_string());
    }
  };
  std::jthread first_worker{fail_root, first_root_id, "failure-agent-a", first_lease_token};
  std::jthread second_worker{fail_root, second_root_id, "failure-agent-b", second_lease_token};
  ready.wait();
  start.count_down();
  first_worker.join();
  second_worker.join();
  EXPECT_TRUE(errors.empty());

  auto reopened = atx::agent::AgentDatabase::open(path.string(), "failure-race");
  ASSERT_TRUE(reopened);
  for (const auto &task_id : {shared_child_id, grandchild_id}) {
    auto task = reopened->get_task(task_id);
    ASSERT_TRUE(task);
    EXPECT_EQ(task->status, "cancelled");
  }
  auto events = reopened->events_after(0);
  ASSERT_TRUE(events);
  const auto cancellations = std::count_if(events->begin(), events->end(), [](const auto &event) {
    return event.type == "task.cancelled";
  });
  EXPECT_EQ(cancellations, 2);
  EXPECT_TRUE(reopened->verify_integrity());
}

TEST(AgentDatabase, TerminalRunFencesAnOutstandingWorkerLease) {
  auto opened = atx::agent::AgentDatabase::open_memory();
  ASSERT_TRUE(opened);
  const auto [run_id, agent_id] = initialize_run(*opened);
  atx::agent::TaskSpec spec;
  spec.run_id = run_id;
  spec.title = "Must be cancelled";
  auto task = opened->add_task(spec);
  ASSERT_TRUE(task);
  auto lease = opened->claim_next(agent_id);
  ASSERT_TRUE(lease);
  EXPECT_TRUE(opened->finish_run(run_id, "cancelled"));
  auto cancelled = opened->get_task(task->id);
  ASSERT_TRUE(cancelled);
  EXPECT_EQ(cancelled->status, "cancelled");
  EXPECT_TRUE(cancelled->lease_token.empty());
  auto stale = opened->complete_task(task->id, agent_id, lease->lease_token);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_TRUE(opened->verify_integrity());
}

TEST(AgentDatabase, ExpiredLeaseIsReclaimedWithANewFencingToken) {
  auto opened = atx::agent::AgentDatabase::open_memory();
  ASSERT_TRUE(opened);
  const auto [run_id, agent_id] = initialize_run(*opened);
  atx::agent::TaskSpec spec;
  spec.run_id = run_id;
  spec.title = "Recover after worker loss";
  spec.max_attempts = 2;
  auto task = opened->add_task(spec);
  ASSERT_TRUE(task);
  auto first = opened->claim_next(agent_id, 1);
  ASSERT_TRUE(first);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto second = opened->claim_next(agent_id, 30);
  ASSERT_TRUE(second) << second.error().to_string();
  EXPECT_EQ(second->id, first->id);
  EXPECT_NE(second->lease_token, first->lease_token);
  EXPECT_EQ(second->attempts, 2);
  auto stale = opened->complete_task(first->id, agent_id, first->lease_token);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_TRUE(opened->complete_task(second->id, agent_id, second->lease_token));
  auto events = opened->events_after(0);
  ASSERT_TRUE(events);
  EXPECT_TRUE(std::any_of(events->begin(), events->end(),
                          [](const auto &event) { return event.type == "task.lease_expired"; }));
}

TEST(AgentDatabase, TerminalLeaseExpiryCancelsDependencyDescendantsBeforeScheduling) {
  auto opened = atx::agent::AgentDatabase::open_memory();
  ASSERT_TRUE(opened);
  const auto [run_id, agent_id] = initialize_run(*opened);
  atx::agent::TaskSpec root_spec;
  root_spec.run_id = run_id;
  root_spec.title = "Expiring prerequisite";
  root_spec.priority = 100;
  root_spec.max_attempts = 1;
  auto root = opened->add_task(root_spec);
  ASSERT_TRUE(root);
  atx::agent::TaskSpec child_spec;
  child_spec.run_id = run_id;
  child_spec.title = "Blocked by expired prerequisite";
  child_spec.dependencies = {root->id};
  auto child = opened->add_task(child_spec);
  ASSERT_TRUE(child);
  atx::agent::TaskSpec independent_spec;
  independent_spec.run_id = run_id;
  independent_spec.title = "Still schedulable";
  auto independent = opened->add_task(independent_spec);
  ASSERT_TRUE(independent);

  auto expired = opened->claim_next(agent_id, 1);
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->id, root->id);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto next = opened->claim_next(agent_id, 30);
  ASSERT_TRUE(next) << next.error().to_string();
  EXPECT_EQ(next->id, independent->id);
  auto failed = opened->get_task(root->id);
  auto cancelled = opened->get_task(child->id);
  ASSERT_TRUE(failed);
  ASSERT_TRUE(cancelled);
  EXPECT_EQ(failed->status, "failed");
  EXPECT_EQ(cancelled->status, "cancelled");
  EXPECT_NE(cancelled->last_error.find(root->id), std::string::npos);
  EXPECT_TRUE(opened->verify_integrity());
}

TEST(AgentDatabase, HeartbeatRevisionProvidesCompareAndSwap) {
  auto opened = atx::agent::AgentDatabase::open_memory();
  ASSERT_TRUE(opened);
  auto run = opened->create_run("Revision test");
  ASSERT_TRUE(run);
  auto agent = opened->register_agent(run->id, "cas-agent", "worker");
  ASSERT_TRUE(agent);
  EXPECT_TRUE(opened->heartbeat(agent->id, agent->revision));
  auto stale = opened->heartbeat(agent->id, agent->revision);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code(), atx::core::ErrorCode::Unavailable);
}

TEST(AgentDatabase, EventsEpisodesAndIdempotencyRemainDurable) {
  auto opened = atx::agent::AgentDatabase::open_memory();
  ASSERT_TRUE(opened);
  auto first_run = opened->create_run("Idempotent run", "run-request-1");
  auto conflicting_run =
      opened->create_run("This payload conflicts with the first write", "run-request-1");
  auto same_run = opened->create_run("Idempotent run", "run-request-1");
  ASSERT_TRUE(first_run);
  ASSERT_FALSE(conflicting_run);
  EXPECT_EQ(conflicting_run.error().code(), atx::core::ErrorCode::InvalidArgument);
  ASSERT_TRUE(same_run);
  EXPECT_EQ(same_run->id, first_run->id);
  auto agent = opened->register_agent(first_run->id, "recorder", "research");
  ASSERT_TRUE(agent);
  auto first_event = opened->append_event("review.completed", "pass", first_run->id, {}, agent->id,
                                          "review-event-1", "reviews/quality-gate");
  auto conflicting_event =
      opened->append_event("review.completed", "different", first_run->id, {}, agent->id,
                           "review-event-1", "reviews/quality-gate");
  auto conflicting_subject =
      opened->append_event("review.completed", "pass", first_run->id, {}, agent->id,
                           "review-event-1", "reviews/different-gate");
  auto same_event = opened->append_event("review.completed", "pass", first_run->id, {}, agent->id,
                                         "review-event-1", "reviews/quality-gate");
  ASSERT_TRUE(first_event);
  ASSERT_FALSE(conflicting_event);
  ASSERT_FALSE(conflicting_subject);
  EXPECT_EQ(conflicting_event.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(conflicting_subject.error().code(), atx::core::ErrorCode::InvalidArgument);
  ASSERT_TRUE(same_event);
  EXPECT_EQ(*same_event, *first_event);
  atx::agent::EpisodeInput episode;
  episode.idempotency_key = "episode-1";
  episode.run_id = first_run->id;
  episode.agent_id = agent->id;
  episode.source_id = "src_evidence";
  episode.observation_id = 42;
  episode.type = "research";
  auto recorded = opened->record_episode(episode);
  auto repeated = opened->record_episode(episode);
  ASSERT_TRUE(recorded);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(recorded->id, repeated->id);
  auto events = opened->events_after(0);
  ASSERT_TRUE(events);
  EXPECT_TRUE(
      std::is_sorted(events->begin(), events->end(), [](const auto &left, const auto &right) {
        return left.sequence < right.sequence;
      }));
  const auto review_event = std::find_if(events->begin(), events->end(), [&](const auto &event) {
    return event.sequence == *first_event;
  });
  ASSERT_NE(review_event, events->end());
  EXPECT_EQ(review_event->subject, "reviews/quality-gate");
  auto review_events = opened->events_after(0, 100, "reviews/quality-gate");
  ASSERT_TRUE(review_events);
  ASSERT_EQ(review_events->size(), 1U);
  EXPECT_EQ(review_events->front().sequence, *first_event);
  const std::string episode_subject = "episodes/" + std::to_string(recorded->id);
  EXPECT_EQ(std::count_if(events->begin(), events->end(),
                          [&](const auto &event) {
                            return event.type == "episode.recorded" &&
                                   event.subject == episode_subject;
                          }),
            1);
  EXPECT_TRUE(std::any_of(events->begin(), events->end(), [&](const auto &event) {
    return event.type == "run.created" && event.subject == "runs/" + first_run->id;
  }));
  EXPECT_TRUE(std::any_of(events->begin(), events->end(), [&](const auto &event) {
    return event.type == "agent.registered" && event.subject == "agents/" + agent->id;
  }));
  EXPECT_TRUE(opened->verify_integrity());
}

TEST(AgentDatabase, DurableEventConsumerResumesFilteredBatchesAfterCheckpoint) {
  const auto path = database_path();
  std::int64_t first_sequence{};
  std::int64_t second_sequence{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "durable-consumers");
    ASSERT_TRUE(database);
    auto first = database->append_event("item.changed", "first", {}, {}, {}, {}, "items/a");
    auto ignored = database->append_event("item.changed", "other", {}, {}, {}, {}, "items/b");
    auto second = database->append_event("item.changed", "second", {}, {}, {}, {}, "items/a");
    ASSERT_TRUE(first);
    ASSERT_TRUE(ignored);
    ASSERT_TRUE(second);
    first_sequence = *first;
    second_sequence = *second;
    auto consumer = database->register_event_consumer("indexer", "items/a");
    ASSERT_TRUE(consumer) << consumer.error().to_string();
    EXPECT_EQ(consumer->revision, 1);
    EXPECT_EQ(consumer->cursor_sequence, 0);
    auto repeated = database->register_event_consumer("indexer", "items/a");
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->revision, 1);
    auto conflicting = database->register_event_consumer("indexer", "items/b");
    ASSERT_FALSE(conflicting);
    EXPECT_EQ(conflicting.error().code(), atx::core::ErrorCode::InvalidArgument);
    auto beyond = database->register_event_consumer("future", {}, second_sequence + 10'000);
    ASSERT_FALSE(beyond);
    EXPECT_EQ(beyond.error().code(), atx::core::ErrorCode::InvalidArgument);

    auto batch = database->poll_event_consumer("indexer", 1);
    ASSERT_TRUE(batch);
    ASSERT_EQ(batch->events.size(), 1U);
    EXPECT_EQ(batch->events.front().sequence, first_sequence);
    auto checkpoint = database->checkpoint_event_consumer(
        "indexer", batch->consumer.revision, batch->events.back().sequence, "checkpoint-first");
    ASSERT_TRUE(checkpoint) << checkpoint.error().to_string();
    EXPECT_EQ(checkpoint->revision, 2);
    EXPECT_EQ(checkpoint->cursor_sequence, first_sequence);
    auto repeated_checkpoint = database->checkpoint_event_consumer(
        "indexer", batch->consumer.revision, batch->events.back().sequence, "checkpoint-first");
    ASSERT_TRUE(repeated_checkpoint);
    EXPECT_EQ(repeated_checkpoint->revision, 2);
    auto conflicting_checkpoint = database->checkpoint_event_consumer(
        "indexer", batch->consumer.revision, second_sequence, "checkpoint-first");
    ASSERT_FALSE(conflicting_checkpoint);
    EXPECT_EQ(conflicting_checkpoint.error().code(), atx::core::ErrorCode::InvalidArgument);
  }

  auto reopened = atx::agent::AgentDatabase::open(path.string(), "durable-consumers");
  ASSERT_TRUE(reopened);
  auto resumed = reopened->poll_event_consumer("indexer", 10);
  ASSERT_TRUE(resumed);
  ASSERT_EQ(resumed->events.size(), 1U);
  EXPECT_EQ(resumed->events.front().sequence, second_sequence);
  EXPECT_EQ(resumed->consumer.revision, 2);
  auto stale = reopened->checkpoint_event_consumer("indexer", 1, second_sequence, "stale-writer");
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code(), atx::core::ErrorCode::Unavailable);
  auto final =
      reopened->checkpoint_event_consumer("indexer", 2, second_sequence, "checkpoint-second");
  ASSERT_TRUE(final);
  EXPECT_EQ(final->revision, 3);
  EXPECT_EQ(final->cursor_sequence, second_sequence);
  auto rollback =
      reopened->checkpoint_event_consumer("indexer", 3, first_sequence, "rollback-attempt");
  ASSERT_FALSE(rollback);
  EXPECT_EQ(rollback.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto empty = reopened->poll_event_consumer("indexer");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->events.empty());
  EXPECT_TRUE(reopened->verify_integrity());
}

TEST(AgentDatabase, CompetingConsumerCheckpointsHaveOneTokenWinner) {
  const auto path = database_path();
  std::int64_t event_sequence{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-race");
    ASSERT_TRUE(database);
    auto event = database->append_event("job.ready", "one", {}, {}, {}, {}, "jobs/a");
    ASSERT_TRUE(event);
    event_sequence = *event;
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
  }

  constexpr std::size_t worker_count = 2;
  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::string> successful_tokens;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "consumer-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      const std::string token = "worker-checkpoint-" + std::to_string(worker);
      auto result = connection->checkpoint_event_consumer("workers", 1, event_sequence, token);
      std::lock_guard lock{mutex};
      if (result) {
        successful_tokens.push_back(token);
      } else {
        errors.push_back(result.error().code());
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();
  ASSERT_EQ(successful_tokens.size(), 1U);
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_EQ(errors.front(), atx::core::ErrorCode::Unavailable);

  auto reopened = atx::agent::AgentDatabase::open(path.string(), "consumer-race");
  ASSERT_TRUE(reopened);
  auto consumer = reopened->get_event_consumer("workers");
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->revision, 2);
  EXPECT_EQ(consumer->cursor_sequence, event_sequence);
  auto winner_retry =
      reopened->checkpoint_event_consumer("workers", 1, event_sequence, successful_tokens.front());
  ASSERT_TRUE(winner_retry);
  EXPECT_EQ(winner_retry->revision, 2);
  EXPECT_TRUE(reopened->verify_integrity());
}

TEST(AgentDatabase, LeasedConsumerDeliveryIsExclusiveRetrySafeAndFenced) {
  auto database = atx::agent::AgentDatabase::open_memory("leased-consumer");
  ASSERT_TRUE(database);
  auto event = database->append_event("item.changed", "first", {}, {}, {}, {}, "items/a");
  ASSERT_TRUE(event);
  ASSERT_TRUE(database->register_event_consumer("indexer", "items/a"));

  auto delivery = database->receive_event_consumer("indexer", "worker-a", "receive-a", 30, 1);
  ASSERT_TRUE(delivery) << delivery.error().to_string();
  ASSERT_EQ(delivery->events.size(), 1U);
  EXPECT_EQ(delivery->events.front().sequence, *event);
  EXPECT_EQ(delivery->previous_sequence, 0);
  EXPECT_EQ(delivery->through_sequence, *event);
  EXPECT_EQ(delivery->attempt, 1);
  EXPECT_FALSE(delivery->delivery_token.empty());

  auto retry = database->receive_event_consumer("indexer", "worker-a", "receive-a", 30, 1);
  ASSERT_TRUE(retry);
  EXPECT_EQ(retry->delivery_token, delivery->delivery_token);
  EXPECT_EQ(retry->acquired_at, delivery->acquired_at);
  EXPECT_EQ(retry->expires_at, delivery->expires_at);
  auto changed_retry = database->receive_event_consumer("indexer", "worker-a", "receive-a", 30, 2);
  ASSERT_FALSE(changed_retry);
  EXPECT_EQ(changed_retry.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto competitor = database->receive_event_consumer("indexer", "worker-b", "receive-b", 30, 1);
  ASSERT_FALSE(competitor);
  EXPECT_EQ(competitor.error().code(), atx::core::ErrorCode::Unavailable);
  auto bypass = database->checkpoint_event_consumer("indexer", 1, *event, "manual-bypass");
  ASSERT_FALSE(bypass);
  EXPECT_EQ(bypass.error().code(), atx::core::ErrorCode::Unavailable);
  auto wrong_renew =
      database->renew_event_consumer_delivery("indexer", "worker-b", delivery->delivery_token, 30);
  ASSERT_FALSE(wrong_renew);
  EXPECT_EQ(wrong_renew.error().code(), atx::core::ErrorCode::Unavailable);
  auto renewed =
      database->renew_event_consumer_delivery("indexer", "worker-a", delivery->delivery_token, 60);
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed->delivery_token, delivery->delivery_token);

  auto wrong_settle = database->settle_event_consumer_delivery(
      "indexer", "worker-b", delivery->delivery_token, "settle-a");
  ASSERT_FALSE(wrong_settle);
  EXPECT_EQ(wrong_settle.error().code(), atx::core::ErrorCode::Unavailable);
  auto settled = database->settle_event_consumer_delivery("indexer", "worker-a",
                                                          delivery->delivery_token, "settle-a");
  ASSERT_TRUE(settled) << settled.error().to_string();
  EXPECT_EQ(settled->cursor_sequence, *event);
  EXPECT_EQ(settled->revision, 2);
  auto settle_retry = database->settle_event_consumer_delivery(
      "indexer", "worker-a", delivery->delivery_token, "settle-a");
  ASSERT_TRUE(settle_retry);
  EXPECT_EQ(settle_retry->cursor_sequence, *event);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, ExpiredConsumerDeliveryRedeliversSameBatchWithFreshFence) {
  auto database = atx::agent::AgentDatabase::open_memory("consumer-redelivery");
  ASSERT_TRUE(database);
  auto first = database->append_event("item.changed", "first", {}, {}, {}, {}, "items/a");
  auto second = database->append_event("item.changed", "second", {}, {}, {}, {}, "items/a");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(database->register_event_consumer("indexer", "items/a"));
  auto initial = database->receive_event_consumer("indexer", "worker-a", "receive-initial", 1, 2);
  ASSERT_TRUE(initial);
  ASSERT_EQ(initial->events.size(), 2U);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});

  auto undersized = database->receive_event_consumer("indexer", "worker-b", "receive-small", 30, 1);
  ASSERT_FALSE(undersized);
  EXPECT_EQ(undersized.error().code(), atx::core::ErrorCode::Unavailable);
  auto redelivery =
      database->receive_event_consumer("indexer", "worker-b", "receive-redelivery", 30, 2);
  ASSERT_TRUE(redelivery) << redelivery.error().to_string();
  EXPECT_NE(redelivery->delivery_token, initial->delivery_token);
  EXPECT_EQ(redelivery->attempt, 2);
  EXPECT_EQ(redelivery->previous_sequence, initial->previous_sequence);
  EXPECT_EQ(redelivery->through_sequence, initial->through_sequence);
  ASSERT_EQ(redelivery->events.size(), initial->events.size());
  EXPECT_EQ(redelivery->events.front().sequence, initial->events.front().sequence);
  EXPECT_EQ(redelivery->events.back().sequence, initial->events.back().sequence);

  auto stale = database->settle_event_consumer_delivery("indexer", "worker-a",
                                                        initial->delivery_token, "stale-settle");
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code(), atx::core::ErrorCode::Unavailable);
  auto settled = database->settle_event_consumer_delivery(
      "indexer", "worker-b", redelivery->delivery_token, "redelivery-settle");
  ASSERT_TRUE(settled);
  EXPECT_EQ(settled->cursor_sequence, *second);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, DurableRetryBackoffSurvivesBackupAndCapsBeforeTerminalDeadLetter) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".retry-backoff";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-retry-backoff");
  ASSERT_TRUE(database);
  auto sequence = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(sequence);
  auto consumer = database->register_event_consumer("workers", "jobs/a", 0, 3, 1, 2);
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->retry_backoff_seconds, 1);
  EXPECT_EQ(consumer->retry_backoff_max_seconds, 2);
  EXPECT_EQ(consumer->retry_jitter, "none");
  auto invalid_policy = database->register_event_consumer("invalid", "jobs/a", 0, 3, 0, 2);
  ASSERT_FALSE(invalid_policy);
  EXPECT_EQ(invalid_policy.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto conflicting_policy = database->register_event_consumer("workers", "jobs/a", 0, 3, 1, 1);
  ASSERT_FALSE(conflicting_policy);
  EXPECT_EQ(conflicting_policy.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto conflicting_jitter = database->register_event_consumer(
      "workers", "jobs/a", 0, 3, 1, 2, atx::agent::EventConsumerRetryJitter::Full);
  ASSERT_FALSE(conflicting_jitter);
  EXPECT_EQ(conflicting_jitter.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto first = database->receive_event_consumer("workers", "worker-a", "first", 1, 1);
  ASSERT_TRUE(first);
  ASSERT_EQ(first->events.size(), 1U);
  EXPECT_EQ(first->attempt, 1);
  EXPECT_EQ(first->retry_delay_seconds, 1);
  auto first_expiry = atx::core::time::from_iso8601(first->expires_at);
  auto first_retry = atx::core::time::from_iso8601(first->retry_not_before);
  ASSERT_TRUE(first_expiry);
  ASSERT_TRUE(first_retry);
  EXPECT_EQ((*first_retry - *first_expiry).count_seconds(), 1);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto cooling = database->receive_event_consumer("workers", "worker-b", "cooling", 1, 1);
  ASSERT_FALSE(cooling);
  EXPECT_EQ(cooling.error().code(), atx::core::ErrorCode::Unavailable);
  auto backup = database->backup_to(backup_path.string());
  ASSERT_TRUE(backup);
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-retry-backoff");
  ASSERT_TRUE(restored);
  auto restored_consumer = restored->get_event_consumer("workers");
  ASSERT_TRUE(restored_consumer);
  EXPECT_EQ(restored_consumer->retry_backoff_seconds, 1);
  EXPECT_EQ(restored_consumer->retry_backoff_max_seconds, 2);
  auto restored_cooling =
      restored->receive_event_consumer("workers", "worker-b", "restored-cooling", 1, 1);
  ASSERT_FALSE(restored_cooling);
  EXPECT_EQ(restored_cooling.error().code(), atx::core::ErrorCode::Unavailable);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto second = restored->receive_event_consumer("workers", "worker-b", "second", 1, 1);
  ASSERT_TRUE(second) << second.error().to_string();
  ASSERT_EQ(second->events.size(), 1U);
  EXPECT_EQ(second->events.front().sequence, *sequence);
  EXPECT_EQ(second->attempt, 2);
  EXPECT_EQ(second->retry_delay_seconds, 2);
  auto second_expiry = atx::core::time::from_iso8601(second->expires_at);
  auto second_retry = atx::core::time::from_iso8601(second->retry_not_before);
  ASSERT_TRUE(second_expiry);
  ASSERT_TRUE(second_retry);
  EXPECT_EQ((*second_retry - *second_expiry).count_seconds(), 2);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto second_cooling =
      restored->receive_event_consumer("workers", "worker-c", "second-cooling", 1, 1);
  ASSERT_FALSE(second_cooling);
  EXPECT_EQ(second_cooling.error().code(), atx::core::ErrorCode::Unavailable);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto still_cooling =
      restored->receive_event_consumer("workers", "worker-c", "still-cooling", 1, 1);
  ASSERT_FALSE(still_cooling);
  EXPECT_EQ(still_cooling.error().code(), atx::core::ErrorCode::Unavailable);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto third = restored->receive_event_consumer("workers", "worker-c", "third", 1, 1);
  ASSERT_TRUE(third) << third.error().to_string();
  EXPECT_EQ(third->attempt, 3);
  EXPECT_EQ(third->retry_delay_seconds, 2);
  auto third_expiry = atx::core::time::from_iso8601(third->expires_at);
  auto third_retry = atx::core::time::from_iso8601(third->retry_not_before);
  ASSERT_TRUE(third_expiry);
  ASSERT_TRUE(third_retry);
  EXPECT_EQ((*third_retry - *third_expiry).count_seconds(), 2);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto terminal = restored->receive_event_consumer("workers", "worker-d", "terminal", 1, 1);
  ASSERT_TRUE(terminal) << terminal.error().to_string();
  EXPECT_TRUE(terminal->events.empty());
  EXPECT_EQ(terminal->dead_lettered_batches, 1);
  EXPECT_EQ(terminal->consumer.cursor_sequence, *sequence);
  auto dead_letters = restored->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  EXPECT_EQ(dead_letters->front().delivery_attempts, 3);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, DeliveryRenewalMovesRetryScheduleWithTheLease) {
  auto database = atx::agent::AgentDatabase::open(":memory:", "consumer-renew-backoff");
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
  ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 1, 4));
  auto delivery = database->receive_event_consumer("workers", "worker-a", "receive", 10, 1);
  ASSERT_TRUE(delivery);
  auto renewed =
      database->renew_event_consumer_delivery("workers", "worker-a", delivery->delivery_token, 20);
  ASSERT_TRUE(renewed);
  EXPECT_GT(renewed->expires_at, delivery->expires_at);
  EXPECT_GT(renewed->retry_not_before, delivery->retry_not_before);
  auto expiry = atx::core::time::from_iso8601(renewed->expires_at);
  auto retry = atx::core::time::from_iso8601(renewed->retry_not_before);
  ASSERT_TRUE(expiry);
  ASSERT_TRUE(retry);
  EXPECT_EQ((*retry - *expiry).count_seconds(), 1);
  EXPECT_EQ(renewed->retry_delay_seconds, delivery->retry_delay_seconds);
  auto settled = database->settle_event_consumer_delivery("workers", "worker-a",
                                                          renewed->delivery_token, "settle");
  ASSERT_TRUE(settled);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, FullJitterIsSampledOnceAndPersistsAcrossDeliveryLifecycleAndBackup) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".full-jitter";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-full-jitter");
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));

  std::vector<std::int64_t> sampled_delays;
  std::string retained_delivery_token;
  std::int64_t retained_retry_delay{};
  for (std::size_t index = 0; index < 24; ++index) {
    const auto suffix = std::to_string(index);
    auto consumer = database->register_event_consumer("worker-" + suffix, "jobs/a", 0, 0, 60, 60,
                                                      atx::agent::EventConsumerRetryJitter::Full);
    ASSERT_TRUE(consumer) << consumer.error().to_string();
    EXPECT_EQ(consumer->retry_jitter, "full");
    auto delivery = database->receive_event_consumer("worker-" + suffix, "owner-" + suffix,
                                                     "request-" + suffix, 30, 1);
    ASSERT_TRUE(delivery) << delivery.error().to_string();
    ASSERT_EQ(delivery->events.size(), 1U);
    EXPECT_GE(delivery->retry_delay_seconds, 0);
    EXPECT_LE(delivery->retry_delay_seconds, 60);
    auto expiry = atx::core::time::from_iso8601(delivery->expires_at);
    auto retry = atx::core::time::from_iso8601(delivery->retry_not_before);
    ASSERT_TRUE(expiry);
    ASSERT_TRUE(retry);
    EXPECT_EQ((*retry - *expiry).count_seconds(), delivery->retry_delay_seconds);
    auto exact = database->receive_event_consumer("worker-" + suffix, "owner-" + suffix,
                                                  "request-" + suffix, 30, 1);
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact->delivery_token, delivery->delivery_token);
    EXPECT_EQ(exact->retry_delay_seconds, delivery->retry_delay_seconds);
    EXPECT_EQ(exact->retry_not_before, delivery->retry_not_before);
    if (index == 0) {
      retained_delivery_token = delivery->delivery_token;
      retained_retry_delay = delivery->retry_delay_seconds;
    }
    sampled_delays.push_back(delivery->retry_delay_seconds);
  }
  std::sort(sampled_delays.begin(), sampled_delays.end());
  EXPECT_NE(sampled_delays.front(), sampled_delays.back());

  auto renewed =
      database->renew_event_consumer_delivery("worker-0", "owner-0", retained_delivery_token, 45);
  ASSERT_TRUE(renewed) << renewed.error().to_string();
  EXPECT_EQ(renewed->retry_delay_seconds, retained_retry_delay);
  auto rejected = database->reject_event_consumer_delivery(
      "worker-0", "owner-0", retained_delivery_token, "reject-once", "transient overload");
  ASSERT_TRUE(rejected) << rejected.error().to_string();
  EXPECT_EQ(rejected->retry_delay_seconds, renewed->retry_delay_seconds);
  auto rejected_at = atx::core::time::from_iso8601(rejected->rejected_at);
  auto retry_at = atx::core::time::from_iso8601(rejected->retry_not_before);
  ASSERT_TRUE(rejected_at);
  ASSERT_TRUE(retry_at);
  EXPECT_EQ((*retry_at - *rejected_at).count_seconds(), rejected->retry_delay_seconds);
  auto exact_rejection = database->reject_event_consumer_delivery(
      "worker-0", "owner-0", retained_delivery_token, "reject-once", "transient overload");
  ASSERT_TRUE(exact_rejection);
  EXPECT_EQ(exact_rejection->retry_delay_seconds, rejected->retry_delay_seconds);
  EXPECT_EQ(exact_rejection->retry_not_before, rejected->retry_not_before);

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-full-jitter");
  ASSERT_TRUE(restored);
  auto restored_rejection = restored->reject_event_consumer_delivery(
      "worker-0", "owner-0", retained_delivery_token, "reject-once", "transient overload");
  ASSERT_TRUE(restored_rejection);
  EXPECT_EQ(restored_rejection->retry_delay_seconds, rejected->retry_delay_seconds);
  EXPECT_EQ(restored_rejection->retry_not_before, rejected->retry_not_before);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, SchemaSixteenMigrationBackfillsDeterministicRetryDelayAudits) {
  const auto path = database_path();
  std::string active_delivery_token;
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "retry-jitter-migration");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "first", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 4, 8));
    auto first = database->receive_event_consumer("workers", "worker-a", "first", 30, 1);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->retry_delay_seconds, 4);
    ASSERT_TRUE(database->settle_event_consumer_delivery("workers", "worker-a",
                                                         first->delivery_token, "settle-first"));
    ASSERT_TRUE(database->append_event("job.ready", "second", {}, {}, {}, {}, "jobs/a"));
    auto active = database->receive_event_consumer("workers", "worker-b", "second", 30, 1);
    ASSERT_TRUE(active);
    EXPECT_EQ(active->retry_delay_seconds, 4);
    active_delivery_token = active->delivery_token;
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE event_consumer_deliveries SET retry_delay_seconds=0;"
                          "UPDATE event_consumers SET retry_jitter='full',"
                          "active_delivery_retry_delay_seconds=0;"
                          "UPDATE agent_db_meta SET value='16' WHERE key='schema_version'"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "retry-jitter-migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto consumer = migrated->get_event_consumer("workers");
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->retry_jitter, "none");
  auto active = migrated->receive_event_consumer("workers", "worker-b", "second", 30, 1);
  ASSERT_TRUE(active) << active.error().to_string();
  EXPECT_EQ(active->delivery_token, active_delivery_token);
  EXPECT_EQ(active->retry_delay_seconds, 4);
  EXPECT_TRUE(migrated->verify_integrity());
}

TEST(AgentDatabase, IntegrityRejectsFullJitterDelayBeyondTheCappedWindow) {
  const auto path = database_path();
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "retry-jitter-integrity");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 8, 8,
                                                  atx::agent::EventConsumerRetryJitter::Full));
    ASSERT_TRUE(database->receive_event_consumer("workers", "worker-a", "receive", 30, 1));
    EXPECT_TRUE(database->verify_integrity());
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE event_consumers SET active_delivery_retry_delay_seconds=9 WHERE "
                          "workspace='retry-jitter-integrity' AND name='workers';"
                          "UPDATE event_consumer_deliveries SET retry_delay_seconds=9 WHERE "
                          "workspace='retry-jitter-integrity' AND consumer_name='workers'"));
  }
  auto corrupted = atx::agent::AgentDatabase::open(path.string(), "retry-jitter-integrity");
  ASSERT_TRUE(corrupted);
  auto verified = corrupted->verify_integrity();
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, ExplicitRejectionRetriesThenDeadLettersFinalAttemptIdempotently) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".rejection-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-rejection");
  ASSERT_TRUE(database);
  auto sequence = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(sequence);
  ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 2, 1, 2));
  auto first = database->receive_event_consumer("workers", "worker-a", "first", 30, 1);
  ASSERT_TRUE(first);

  auto rejected = database->reject_event_consumer_delivery(
      "workers", "worker-a", first->delivery_token, "reject-first", "dependency unavailable");
  ASSERT_TRUE(rejected) << rejected.error().to_string();
  EXPECT_FALSE(rejected->dead_lettered);
  EXPECT_EQ(rejected->attempt, 1);
  EXPECT_EQ(rejected->consumer.cursor_sequence, 0);
  auto rejected_at = atx::core::time::from_iso8601(rejected->rejected_at);
  auto retry_at = atx::core::time::from_iso8601(rejected->retry_not_before);
  ASSERT_TRUE(rejected_at);
  ASSERT_TRUE(retry_at);
  EXPECT_EQ((*retry_at - *rejected_at).count_seconds(), 1);
  auto exact_retry = database->reject_event_consumer_delivery(
      "workers", "worker-a", first->delivery_token, "reject-first", "dependency unavailable");
  ASSERT_TRUE(exact_retry);
  EXPECT_EQ(exact_retry->rejected_at, rejected->rejected_at);
  EXPECT_EQ(exact_retry->retry_not_before, rejected->retry_not_before);
  auto changed_intent = database->reject_event_consumer_delivery(
      "workers", "worker-a", first->delivery_token, "reject-first", "different reason");
  ASSERT_FALSE(changed_intent);
  EXPECT_EQ(changed_intent.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto changed_token = database->reject_event_consumer_delivery(
      "workers", "worker-a", first->delivery_token, "reject-other", "dependency unavailable");
  ASSERT_FALSE(changed_token);
  EXPECT_EQ(changed_token.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto stale_renew =
      database->renew_event_consumer_delivery("workers", "worker-a", first->delivery_token, 30);
  ASSERT_FALSE(stale_renew);
  EXPECT_EQ(stale_renew.error().code(), atx::core::ErrorCode::Unavailable);
  auto stale_settle = database->settle_event_consumer_delivery(
      "workers", "worker-a", first->delivery_token, "stale-settle");
  ASSERT_FALSE(stale_settle);
  EXPECT_EQ(stale_settle.error().code(), atx::core::ErrorCode::Unavailable);
  auto cooling = database->receive_event_consumer("workers", "worker-b", "cooling", 30, 1);
  ASSERT_FALSE(cooling);
  EXPECT_EQ(cooling.error().code(), atx::core::ErrorCode::Unavailable);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto second = database->receive_event_consumer("workers", "worker-b", "second", 30, 1);
  ASSERT_TRUE(second) << second.error().to_string();
  EXPECT_EQ(second->attempt, 2);
  EXPECT_EQ(second->events.front().sequence, *sequence);
  auto terminal = database->reject_event_consumer_delivery(
      "workers", "worker-b", second->delivery_token, "reject-final", "malformed payload");
  ASSERT_TRUE(terminal) << terminal.error().to_string();
  EXPECT_TRUE(terminal->dead_lettered);
  EXPECT_GT(terminal->dead_letter_id, 0);
  EXPECT_EQ(terminal->consumer.cursor_sequence, *sequence);
  EXPECT_EQ(terminal->consumer.revision, 2);
  auto exact_terminal = database->reject_event_consumer_delivery(
      "workers", "worker-b", second->delivery_token, "reject-final", "malformed payload");
  ASSERT_TRUE(exact_terminal);
  EXPECT_EQ(exact_terminal->dead_letter_id, terminal->dead_letter_id);
  EXPECT_EQ(exact_terminal->rejected_at, terminal->rejected_at);
  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  EXPECT_EQ(dead_letters->front().reason, "max_delivery_attempts_rejected");
  EXPECT_EQ(dead_letters->front().delivery_attempts, 2);
  EXPECT_EQ(dead_letters->front().events.front().sequence, *sequence);
  auto empty = database->receive_event_consumer("workers", "worker-c", "after-terminal", 30, 1);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->events.empty());
  EXPECT_TRUE(database->verify_integrity());

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-rejection");
  ASSERT_TRUE(restored);
  auto restored_retry = restored->reject_event_consumer_delivery(
      "workers", "worker-b", second->delivery_token, "reject-final", "malformed payload");
  ASSERT_TRUE(restored_retry);
  EXPECT_TRUE(restored_retry->dead_lettered);
  EXPECT_EQ(restored_retry->dead_letter_id, terminal->dead_letter_id);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ExplicitDeadLetterDispositionBypassesUnlimitedRetryPolicy) {
  auto database = atx::agent::AgentDatabase::open(":memory:", "consumer-explicit-dead-letter");
  ASSERT_TRUE(database);
  auto sequence = database->append_event("job.ready", "invalid", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(sequence);
  ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 1, 8));
  auto delivery = database->receive_event_consumer("workers", "worker-a", "receive", 30, 1);
  ASSERT_TRUE(delivery);
  auto dead_lettered = database->reject_event_consumer_delivery(
      "workers", "worker-a", delivery->delivery_token, "explicit-dead-letter",
      "authentication failed", atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(dead_lettered) << dead_lettered.error().to_string();
  EXPECT_TRUE(dead_lettered->dead_lettered);
  EXPECT_EQ(dead_lettered->disposition, "dead_letter");
  EXPECT_EQ(dead_lettered->attempt, 1);
  EXPECT_EQ(dead_lettered->consumer.cursor_sequence, *sequence);
  auto exact_retry = database->reject_event_consumer_delivery(
      "workers", "worker-a", delivery->delivery_token, "explicit-dead-letter",
      "authentication failed", atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(exact_retry);
  EXPECT_EQ(exact_retry->dead_letter_id, dead_lettered->dead_letter_id);
  auto disposition_conflict = database->reject_event_consumer_delivery(
      "workers", "worker-a", delivery->delivery_token, "explicit-dead-letter",
      "authentication failed", atx::agent::EventConsumerRejectionDisposition::Retry);
  ASSERT_FALSE(disposition_conflict);
  EXPECT_EQ(disposition_conflict.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  EXPECT_EQ(dead_letters->front().reason, "explicit_rejection");
  EXPECT_EQ(dead_letters->front().rejection_disposition, "dead_letter");
  EXPECT_EQ(dead_letters->front().rejection_reason, "authentication failed");
  EXPECT_EQ(dead_letters->front().delivery_attempts, 1);
  auto redriven = database->redrive_event_consumer_dead_letter("workers", dead_letters->front().id,
                                                               "explicit-redrive");
  ASSERT_TRUE(redriven);
  EXPECT_EQ(redriven->rejection_disposition, "dead_letter");
  EXPECT_EQ(redriven->rejection_reason, "authentication failed");
  ASSERT_EQ(redriven->redriven_events.size(), 1U);
  auto replay = database->receive_event_consumer("workers", "worker-b", "replay", 30, 1);
  ASSERT_TRUE(replay);
  EXPECT_EQ(replay->events.front().sequence, redriven->redriven_events.front().sequence);
  ASSERT_TRUE(database->settle_event_consumer_delivery("workers", "worker-b",
                                                       replay->delivery_token, "settle-replay"));
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, ConcurrentExactDeliveryRejectionsConvergeOnOneAuditOutcome) {
  const auto path = database_path();
  std::string delivery_token;
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-rejection-race");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 1, 2));
    auto delivery = database->receive_event_consumer("workers", "worker-a", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    delivery_token = delivery->delivery_token;
  }

  constexpr std::size_t worker_count = 8;
  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::pair<std::string, std::string>> outcomes;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "consumer-rejection-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto rejection = connection->reject_event_consumer_delivery(
          "workers", "worker-a", delivery_token, "shared-rejection", "transient dependency");
      std::lock_guard lock{mutex};
      if (rejection) {
        outcomes.emplace_back(rejection->rejected_at, rejection->retry_not_before);
      } else {
        errors.push_back(rejection.error().code());
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();
  EXPECT_TRUE(errors.empty());
  ASSERT_EQ(outcomes.size(), worker_count);
  EXPECT_TRUE(std::all_of(outcomes.begin(), outcomes.end(),
                          [&](const auto &outcome) { return outcome == outcomes.front(); }));

  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-rejection-race");
  ASSERT_TRUE(database);
  auto conflict = database->reject_event_consumer_delivery(
      "workers", "worker-a", delivery_token, "other-rejection", "transient dependency");
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code(), atx::core::ErrorCode::InvalidArgument);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto successor = database->receive_event_consumer("workers", "worker-b", "successor", 30, 1);
  ASSERT_TRUE(successor);
  EXPECT_EQ(successor->attempt, 2);
  auto settled = database->settle_event_consumer_delivery(
      "workers", "worker-b", successor->delivery_token, "settle-successor");
  ASSERT_TRUE(settled);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, CompetingReceiversPublishOneReceiptAfterRetryCooldown) {
  const auto path = database_path();
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-backoff-race");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 1, 2));
    ASSERT_TRUE(database->receive_event_consumer("workers", "initial", "initial", 1, 1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{2'100});

  constexpr std::size_t worker_count = 6;
  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  struct Winner {
    std::string owner;
    std::string delivery_token;
    std::int64_t attempt{};
  };
  std::vector<Winner> winners;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "consumer-backoff-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      const std::string owner = "worker-" + std::to_string(worker);
      auto delivery = connection->receive_event_consumer("workers", owner,
                                                         "retry-" + std::to_string(worker), 30, 1);
      std::lock_guard lock{mutex};
      if (delivery) {
        winners.push_back({owner, delivery->delivery_token, delivery->attempt});
      } else {
        errors.push_back(delivery.error().code());
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();

  ASSERT_EQ(winners.size(), 1U);
  EXPECT_EQ(winners.front().attempt, 2);
  ASSERT_EQ(errors.size(), worker_count - 1);
  EXPECT_TRUE(std::all_of(errors.begin(), errors.end(), [](const auto error) {
    return error == atx::core::ErrorCode::Unavailable;
  }));
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-backoff-race");
  ASSERT_TRUE(database);
  auto settled = database->settle_event_consumer_delivery(
      "workers", winners.front().owner, winners.front().delivery_token, "settle-winner");
  ASSERT_TRUE(settled);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, BoundedDeliveryDeadLettersPoisonBatchAndAdvances) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".dead-letter-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-dead-letters");
  ASSERT_TRUE(database);
  auto poison = database->append_event("item.changed", "poison", {}, {}, {}, {}, "items/a");
  auto next = database->append_event("item.changed", "healthy", {}, {}, {}, {}, "items/a");
  ASSERT_TRUE(poison);
  ASSERT_TRUE(next);
  auto consumer = database->register_event_consumer("indexer", "items/a", 0, 2);
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->max_delivery_attempts, 2);
  auto conflicting = database->register_event_consumer("indexer", "items/a", 0, 3);
  ASSERT_FALSE(conflicting);
  EXPECT_EQ(conflicting.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto first = database->receive_event_consumer("indexer", "worker-a", "poison-first", 1, 1);
  ASSERT_TRUE(first);
  ASSERT_EQ(first->events.size(), 1U);
  EXPECT_EQ(first->events.front().sequence, *poison);
  EXPECT_EQ(first->attempt, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto second = database->receive_event_consumer("indexer", "worker-b", "poison-second", 1, 1);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->through_sequence, *poison);
  EXPECT_EQ(second->attempt, 2);
  EXPECT_NE(second->delivery_token, first->delivery_token);
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});

  auto healthy = database->receive_event_consumer("indexer", "worker-c", "after-poison", 30, 1);
  ASSERT_TRUE(healthy) << healthy.error().to_string();
  ASSERT_EQ(healthy->events.size(), 1U);
  EXPECT_EQ(healthy->events.front().sequence, *next);
  EXPECT_EQ(healthy->attempt, 1);
  EXPECT_EQ(healthy->dead_lettered_batches, 1);
  EXPECT_EQ(healthy->dead_lettered_events, 1);
  EXPECT_EQ(healthy->consumer.cursor_sequence, *poison);
  EXPECT_EQ(healthy->consumer.revision, 2);
  auto healthy_retry =
      database->receive_event_consumer("indexer", "worker-c", "after-poison", 30, 1);
  ASSERT_TRUE(healthy_retry);
  EXPECT_EQ(healthy_retry->delivery_token, healthy->delivery_token);
  EXPECT_EQ(healthy_retry->dead_lettered_batches, 1);
  EXPECT_EQ(healthy_retry->dead_lettered_events, 1);

  auto dead_letters = database->list_event_consumer_dead_letters("indexer");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  EXPECT_EQ(dead_letters->front().delivery_token, second->delivery_token);
  EXPECT_EQ(dead_letters->front().delivery_attempts, 2);
  EXPECT_EQ(dead_letters->front().reason, "max_delivery_attempts_exceeded");
  ASSERT_EQ(dead_letters->front().events.size(), 1U);
  EXPECT_EQ(dead_letters->front().events.front().sequence, *poison);
  auto stale = database->settle_event_consumer_delivery(
      "indexer", "worker-b", second->delivery_token, "poison-stale-settle");
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code(), atx::core::ErrorCode::Unavailable);
  auto settled = database->settle_event_consumer_delivery(
      "indexer", "worker-c", healthy->delivery_token, "healthy-settle");
  ASSERT_TRUE(settled);
  EXPECT_EQ(settled->cursor_sequence, *next);
  EXPECT_EQ(settled->revision, 3);

  const auto dead_letter_id = dead_letters->front().id;
  auto redriven =
      database->redrive_event_consumer_dead_letter("indexer", dead_letter_id, "redrive-poison");
  ASSERT_TRUE(redriven) << redriven.error().to_string();
  EXPECT_EQ(redriven->status, "redriven");
  EXPECT_EQ(redriven->redrive_token, "redrive-poison");
  EXPECT_FALSE(redriven->redriven_at.empty());
  ASSERT_EQ(redriven->events.size(), 1U);
  ASSERT_EQ(redriven->redriven_events.size(), 1U);
  const auto redriven_sequence = redriven->redriven_events.front().sequence;
  EXPECT_GT(redriven_sequence, *next);
  EXPECT_EQ(redriven->events.front().sequence, *poison);
  EXPECT_EQ(redriven->redriven_events.front().run_id, redriven->events.front().run_id);
  EXPECT_EQ(redriven->redriven_events.front().task_id, redriven->events.front().task_id);
  EXPECT_EQ(redriven->redriven_events.front().agent_id, redriven->events.front().agent_id);
  EXPECT_EQ(redriven->redriven_events.front().type, redriven->events.front().type);
  EXPECT_EQ(redriven->redriven_events.front().subject, redriven->events.front().subject);
  EXPECT_EQ(redriven->redriven_events.front().payload, redriven->events.front().payload);

  auto exact_redrive_retry =
      database->redrive_event_consumer_dead_letter("indexer", dead_letter_id, "redrive-poison");
  ASSERT_TRUE(exact_redrive_retry);
  ASSERT_EQ(exact_redrive_retry->redriven_events.size(), 1U);
  EXPECT_EQ(exact_redrive_retry->redriven_events.front().sequence, redriven_sequence);
  auto conflicting_redrive =
      database->redrive_event_consumer_dead_letter("indexer", dead_letter_id, "different-redrive");
  ASSERT_FALSE(conflicting_redrive);
  EXPECT_EQ(conflicting_redrive.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto redrive_delivery =
      database->receive_event_consumer("indexer", "worker-d", "redrive-receive", 30, 1);
  ASSERT_TRUE(redrive_delivery);
  ASSERT_EQ(redrive_delivery->events.size(), 1U);
  EXPECT_EQ(redrive_delivery->events.front().sequence, redriven_sequence);
  EXPECT_EQ(redrive_delivery->events.front().payload, "poison");
  auto redrive_settled = database->settle_event_consumer_delivery(
      "indexer", "worker-d", redrive_delivery->delivery_token, "redrive-settle");
  ASSERT_TRUE(redrive_settled);
  EXPECT_EQ(redrive_settled->cursor_sequence, redriven_sequence);

  auto redriven_dead_letters = database->list_event_consumer_dead_letters("indexer");
  ASSERT_TRUE(redriven_dead_letters);
  ASSERT_EQ(redriven_dead_letters->size(), 1U);
  EXPECT_EQ(redriven_dead_letters->front().status, "redriven");
  EXPECT_EQ(redriven_dead_letters->front().redrive_token, "redrive-poison");
  ASSERT_EQ(redriven_dead_letters->front().redriven_events.size(), 1U);
  EXPECT_EQ(redriven_dead_letters->front().redriven_events.front().sequence, redriven_sequence);
  EXPECT_TRUE(database->verify_integrity());

  auto backup = database->backup_to(backup_path.string());
  ASSERT_TRUE(backup);
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-dead-letters");
  ASSERT_TRUE(restored);
  auto restored_dead_letters = restored->list_event_consumer_dead_letters("indexer");
  ASSERT_TRUE(restored_dead_letters);
  ASSERT_EQ(restored_dead_letters->size(), 1U);
  EXPECT_EQ(restored_dead_letters->front().events.front().sequence, *poison);
  EXPECT_EQ(restored_dead_letters->front().status, "redriven");
  EXPECT_EQ(restored_dead_letters->front().redrive_token, "redrive-poison");
  ASSERT_EQ(restored_dead_letters->front().redriven_events.size(), 1U);
  EXPECT_EQ(restored_dead_letters->front().redriven_events.front().sequence, redriven_sequence);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ConcurrentDeadLetterRedrivePublishesExactlyOneOccurrence) {
  const auto path = database_path();
  std::int64_t original_sequence{};
  std::int64_t dead_letter_id{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-race");
    ASSERT_TRUE(database);
    auto poison = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
    ASSERT_TRUE(poison);
    original_sequence = *poison;
    ASSERT_TRUE(database->register_event_consumer(
        "workers", "jobs/a", 0, 1, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 1));
    ASSERT_TRUE(database->receive_event_consumer("workers", "initial", "initial-receive", 1, 1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-race");
    ASSERT_TRUE(database);
    auto after_expiry =
        database->receive_event_consumer("workers", "terminal", "terminal-receive", 30, 1);
    ASSERT_TRUE(after_expiry);
    EXPECT_TRUE(after_expiry->events.empty());
    EXPECT_EQ(after_expiry->dead_lettered_batches, 1);
    auto dead_letters = database->list_event_consumer_dead_letters("workers");
    ASSERT_TRUE(dead_letters);
    ASSERT_EQ(dead_letters->size(), 1U);
    dead_letter_id = dead_letters->front().id;
  }

  constexpr std::size_t worker_count = 8;
  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::int64_t> redriven_sequences;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto redriven = connection->redrive_event_consumer_dead_letter("workers", dead_letter_id,
                                                                     "same-redrive-token");
      std::lock_guard lock{mutex};
      if (!redriven) {
        errors.push_back(redriven.error().code());
        return;
      }
      if (redriven->redriven_events.size() != 1U) {
        errors.push_back(atx::core::ErrorCode::Internal);
        return;
      }
      redriven_sequences.push_back(redriven->redriven_events.front().sequence);
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();

  EXPECT_TRUE(errors.empty());
  ASSERT_EQ(redriven_sequences.size(), worker_count);
  EXPECT_TRUE(
      std::all_of(redriven_sequences.begin(), redriven_sequences.end(),
                  [&](const auto sequence) { return sequence == redriven_sequences.front(); }));
  EXPECT_GT(redriven_sequences.front(), original_sequence);

  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-race");
  ASSERT_TRUE(database);
  auto conflicting = database->redrive_event_consumer_dead_letter("workers", dead_letter_id,
                                                                  "different-redrive-token");
  ASSERT_FALSE(conflicting);
  EXPECT_EQ(conflicting.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto occurrences = database->events_after(original_sequence, 100, "jobs/a");
  ASSERT_TRUE(occurrences);
  ASSERT_EQ(occurrences->size(), 1U);
  EXPECT_EQ(occurrences->front().sequence, redriven_sequences.front());
  EXPECT_EQ(occurrences->front().root_sequence, original_sequence);
  EXPECT_EQ(occurrences->front().redrive_count, 1);
  EXPECT_EQ(occurrences->front().type, "job.ready");
  EXPECT_EQ(occurrences->front().payload, "poison");
  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  EXPECT_EQ(dead_letters->front().status, "redriven");
  EXPECT_EQ(dead_letters->front().redrive_budget_event_count, 1);
  EXPECT_EQ(dead_letters->front().redrive_budget_before_millis, 1'000);
  EXPECT_EQ(dead_letters->front().redrive_budget_after_millis, 0);
  ASSERT_EQ(dead_letters->front().redriven_events.size(), 1U);
  EXPECT_EQ(dead_letters->front().redriven_events.front().sequence, redriven_sequences.front());
  auto status = database->get_event_consumer_status("workers");
  ASSERT_TRUE(status);
  EXPECT_EQ(status->retained_dead_letter_count, 1);
  EXPECT_EQ(status->open_dead_letter_count, 0);
  EXPECT_EQ(status->open_dead_letter_event_count, 0);
  EXPECT_TRUE(status->oldest_open_dead_letter_at.empty());
  EXPECT_EQ(status->redriven_dead_letter_count, 1);
  EXPECT_EQ(status->quarantined_dead_letter_count, 0);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, DurableRedriveBudgetThrottlesAndRefillsAcrossBackup) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".redrive-budget";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-budget");
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->append_event("job.ready", "first", {}, {}, {}, {}, "jobs/a"));
  ASSERT_TRUE(database->append_event("job.ready", "second", {}, {}, {}, {}, "jobs/a"));
  auto consumer = database->register_event_consumer(
      "workers", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 1);
  ASSERT_TRUE(consumer) << consumer.error().to_string();
  EXPECT_EQ(consumer->redrive_rate_per_second, 1);
  EXPECT_EQ(consumer->redrive_burst_events, 1);
  EXPECT_EQ(consumer->redrive_token_millis, 1'000);
  EXPECT_FALSE(consumer->redrive_refilled_at.empty());
  auto invalid_pair = database->register_event_consumer(
      "invalid", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 0);
  ASSERT_FALSE(invalid_pair);
  EXPECT_EQ(invalid_pair.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto conflicting_policy = database->register_event_consumer(
      "workers", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 2, 1);
  ASSERT_FALSE(conflicting_policy);
  EXPECT_EQ(conflicting_policy.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto first = database->receive_event_consumer("workers", "worker-a", "first", 30, 1);
  ASSERT_TRUE(first);
  auto first_dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker-a", first->delivery_token, "dead-letter-first", "invalid first",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(first_dead_letter);
  auto second = database->receive_event_consumer("workers", "worker-b", "second", 30, 1);
  ASSERT_TRUE(second);
  auto second_dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker-b", second->delivery_token, "dead-letter-second", "invalid second",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(second_dead_letter);

  auto first_redrive = database->redrive_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "redrive-first");
  ASSERT_TRUE(first_redrive) << first_redrive.error().to_string();
  EXPECT_EQ(first_redrive->redrive_budget_event_count, 1);
  EXPECT_EQ(first_redrive->redrive_budget_before_millis, 1'000);
  EXPECT_EQ(first_redrive->redrive_budget_after_millis, 0);
  EXPECT_FALSE(first_redrive->redrive_budget_refilled_at.empty());
  auto exact_first = database->redrive_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "redrive-first");
  ASSERT_TRUE(exact_first);
  EXPECT_EQ(exact_first->redriven_events.front().sequence,
            first_redrive->redriven_events.front().sequence);
  EXPECT_EQ(exact_first->redrive_budget_after_millis, 0);

  auto throttled = database->redrive_event_consumer_dead_letter(
      "workers", second_dead_letter->dead_letter_id, "redrive-second");
  ASSERT_FALSE(throttled);
  EXPECT_EQ(throttled.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_NE(throttled.error().message().find("retry after"), std::string::npos);
  auto after_throttle = database->get_event_consumer("workers");
  ASSERT_TRUE(after_throttle);
  EXPECT_EQ(after_throttle->redrive_token_millis, 0);
  EXPECT_EQ(after_throttle->redrive_refilled_at, first_redrive->redrive_budget_refilled_at);

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-redrive-budget");
  ASSERT_TRUE(restored);
  auto restored_consumer = restored->get_event_consumer("workers");
  ASSERT_TRUE(restored_consumer);
  EXPECT_EQ(restored_consumer->redrive_token_millis, after_throttle->redrive_token_millis);
  EXPECT_EQ(restored_consumer->redrive_refilled_at, after_throttle->redrive_refilled_at);
  auto restored_exact = restored->redrive_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "redrive-first");
  ASSERT_TRUE(restored_exact);
  EXPECT_EQ(restored_exact->redrive_budget_after_millis, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto second_redrive = restored->redrive_event_consumer_dead_letter(
      "workers", second_dead_letter->dead_letter_id, "redrive-second");
  ASSERT_TRUE(second_redrive) << second_redrive.error().to_string();
  EXPECT_EQ(second_redrive->redrive_budget_event_count, 1);
  EXPECT_EQ(second_redrive->redrive_budget_before_millis, 1'000);
  EXPECT_EQ(second_redrive->redrive_budget_after_millis, 0);
  EXPECT_GT(second_redrive->redrive_budget_refilled_at, first_redrive->redrive_budget_refilled_at);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, RedriveBudgetRejectsABatchLargerThanItsAtomicBurst) {
  auto database = atx::agent::AgentDatabase::open_memory("consumer-redrive-burst");
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->append_event("job.ready", "one", {}, {}, {}, {}, "jobs/a"));
  ASSERT_TRUE(database->append_event("job.ready", "two", {}, {}, {}, {}, "jobs/a"));
  ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 0, 0,
                                                atx::agent::EventConsumerRetryJitter::None, 1, 1));
  auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 2);
  ASSERT_TRUE(delivery);
  ASSERT_EQ(delivery->events.size(), 2U);
  auto dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker", delivery->delivery_token, "dead-letter", "invalid batch",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(dead_letter);
  auto redrive = database->redrive_event_consumer_dead_letter(
      "workers", dead_letter->dead_letter_id, "oversized-redrive");
  ASSERT_FALSE(redrive);
  EXPECT_EQ(redrive.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto listed = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 1U);
  EXPECT_EQ(listed->front().status, "open");
  EXPECT_EQ(listed->front().redrive_budget_event_count, 0);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, ConcurrentDifferentRedrivesCannotOverspendOneConsumerBudget) {
  const auto path = database_path();
  std::array<std::int64_t, 2> dead_letter_ids{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-budget-race");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "first", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->append_event("job.ready", "second", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer(
        "workers", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 1));
    for (std::size_t index = 0; index < dead_letter_ids.size(); ++index) {
      const auto suffix = std::to_string(index);
      auto delivery = database->receive_event_consumer("workers", "worker-" + suffix,
                                                       "receive-" + suffix, 30, 1);
      ASSERT_TRUE(delivery);
      auto dead_letter = database->reject_event_consumer_delivery(
          "workers", "worker-" + suffix, delivery->delivery_token, "reject-" + suffix, "invalid",
          atx::agent::EventConsumerRejectionDisposition::DeadLetter);
      ASSERT_TRUE(dead_letter);
      dead_letter_ids[index] = dead_letter->dead_letter_id;
    }
  }

  std::latch ready{2};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::int64_t> published_sequences;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t index = 0; index < dead_letter_ids.size(); ++index) {
    workers.emplace_back([&, index] {
      auto connection =
          atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-budget-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto redrive = connection->redrive_event_consumer_dead_letter(
          "workers", dead_letter_ids[index], "redrive-" + std::to_string(index));
      std::lock_guard lock{mutex};
      if (!redrive) {
        errors.push_back(redrive.error().code());
      } else {
        published_sequences.push_back(redrive->redriven_events.front().sequence);
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();

  ASSERT_EQ(published_sequences.size(), 1U);
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_EQ(errors.front(), atx::core::ErrorCode::Unavailable);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-budget-race");
  ASSERT_TRUE(database);
  auto consumer = database->get_event_consumer("workers");
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->redrive_token_millis, 0);
  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  EXPECT_EQ(std::count_if(dead_letters->begin(), dead_letters->end(),
                          [](const auto &dead_letter) { return dead_letter.status == "redriven"; }),
            1);
  EXPECT_EQ(std::count_if(dead_letters->begin(), dead_letters->end(),
                          [](const auto &dead_letter) {
                            return dead_letter.redrive_budget_event_count == 1;
                          }),
            1);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, IntegrityRejectsRedriveBudgetHeadDrift) {
  const auto path = database_path();
  {
    auto database =
        atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-budget-integrity");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer(
        "workers", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 1));
    auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    auto dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker", delivery->delivery_token, "reject", "invalid",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(dead_letter);
    ASSERT_TRUE(database->redrive_event_consumer_dead_letter("workers", dead_letter->dead_letter_id,
                                                             "redrive"));
    EXPECT_TRUE(database->verify_integrity());
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE event_consumers SET redrive_token_millis=1 WHERE "
                          "workspace='consumer-redrive-budget-integrity' AND name='workers'"));
  }
  auto corrupted =
      atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-budget-integrity");
  ASSERT_TRUE(corrupted);
  auto verified = corrupted->verify_integrity();
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, RedriveLineagePersistsGenerationsAndBoundsPoisonReplay) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".lineage-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-redrive-lineage");
  ASSERT_TRUE(database);
  auto original = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(original);
  auto initial_events = database->events_after(0, 100, "jobs/a");
  ASSERT_TRUE(initial_events);
  ASSERT_EQ(initial_events->size(), 1U);
  EXPECT_EQ(initial_events->front().root_sequence, *original);
  EXPECT_EQ(initial_events->front().redrive_count, 0);
  auto consumer = database->register_event_consumer(
      "workers", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 2, 2);
  ASSERT_TRUE(consumer) << consumer.error().to_string();
  EXPECT_EQ(consumer->max_redrive_count, 2);

  auto first_delivery = database->receive_event_consumer("workers", "worker-0", "receive-0", 30, 1);
  ASSERT_TRUE(first_delivery);
  auto first_dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker-0", first_delivery->delivery_token, "reject-0", "poison generation 0",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(first_dead_letter);
  auto first_redrive = database->redrive_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "redrive-0");
  ASSERT_TRUE(first_redrive) << first_redrive.error().to_string();
  ASSERT_EQ(first_redrive->redriven_events.size(), 1U);
  const auto first_replay_sequence = first_redrive->redriven_events.front().sequence;
  EXPECT_EQ(first_redrive->redriven_events.front().root_sequence, *original);
  EXPECT_EQ(first_redrive->redriven_events.front().redrive_count, 1);
  auto exact_first = database->redrive_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "redrive-0");
  ASSERT_TRUE(exact_first);
  EXPECT_EQ(exact_first->redriven_events.front().sequence, first_replay_sequence);

  auto second_delivery =
      database->receive_event_consumer("workers", "worker-1", "receive-1", 30, 1);
  ASSERT_TRUE(second_delivery);
  ASSERT_EQ(second_delivery->events.size(), 1U);
  EXPECT_EQ(second_delivery->events.front().redrive_count, 1);
  auto second_dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker-1", second_delivery->delivery_token, "reject-1", "poison generation 1",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(second_dead_letter);
  auto second_redrive = database->redrive_event_consumer_dead_letter(
      "workers", second_dead_letter->dead_letter_id, "redrive-1");
  ASSERT_TRUE(second_redrive) << second_redrive.error().to_string();
  ASSERT_EQ(second_redrive->redriven_events.size(), 1U);
  const auto second_replay_sequence = second_redrive->redriven_events.front().sequence;
  EXPECT_EQ(second_redrive->redriven_events.front().root_sequence, *original);
  EXPECT_EQ(second_redrive->redriven_events.front().redrive_count, 2);
  auto budget_before_bound = database->get_event_consumer("workers");
  ASSERT_TRUE(budget_before_bound);

  auto third_delivery = database->receive_event_consumer("workers", "worker-2", "receive-2", 30, 1);
  ASSERT_TRUE(third_delivery);
  ASSERT_EQ(third_delivery->events.size(), 1U);
  EXPECT_EQ(third_delivery->events.front().sequence, second_replay_sequence);
  auto third_dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker-2", third_delivery->delivery_token, "reject-2", "poison generation 2",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(third_dead_letter);
  auto bounded = database->redrive_event_consumer_dead_letter(
      "workers", third_dead_letter->dead_letter_id, "redrive-2");
  ASSERT_FALSE(bounded);
  EXPECT_EQ(bounded.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_NE(bounded.error().message().find("generation limit"), std::string::npos);
  auto budget_after_bound = database->get_event_consumer("workers");
  ASSERT_TRUE(budget_after_bound);
  EXPECT_EQ(budget_after_bound->redrive_token_millis, budget_before_bound->redrive_token_millis);
  EXPECT_EQ(budget_after_bound->redrive_refilled_at, budget_before_bound->redrive_refilled_at);
  auto occurrences = database->events_after(0, 100, "jobs/a");
  ASSERT_TRUE(occurrences);
  ASSERT_EQ(occurrences->size(), 3U);
  EXPECT_EQ(occurrences->at(0).redrive_count, 0);
  EXPECT_EQ(occurrences->at(1).redrive_count, 1);
  EXPECT_EQ(occurrences->at(2).redrive_count, 2);
  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 3U);
  EXPECT_EQ(dead_letters->front().id, third_dead_letter->dead_letter_id);
  EXPECT_EQ(dead_letters->front().status, "open");
  EXPECT_TRUE(dead_letters->front().redrive_token.empty());
  EXPECT_TRUE(dead_letters->front().redriven_events.empty());
  EXPECT_TRUE(database->verify_integrity());

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-redrive-lineage");
  ASSERT_TRUE(restored);
  auto restored_consumer = restored->get_event_consumer("workers");
  ASSERT_TRUE(restored_consumer);
  EXPECT_EQ(restored_consumer->max_redrive_count, 2);
  auto restored_exact = restored->redrive_event_consumer_dead_letter(
      "workers", second_dead_letter->dead_letter_id, "redrive-1");
  ASSERT_TRUE(restored_exact);
  EXPECT_EQ(restored_exact->redriven_events.front().sequence, second_replay_sequence);
  auto restored_bounded = restored->redrive_event_consumer_dead_letter(
      "workers", third_dead_letter->dead_letter_id, "redrive-2");
  ASSERT_FALSE(restored_bounded);
  EXPECT_EQ(restored_bounded.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, SchemaEighteenMigrationReconstructsHistoricalRedriveLineage) {
  const auto path = database_path();
  std::int64_t original_sequence{};
  std::int64_t first_replay_sequence{};
  std::int64_t second_replay_sequence{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "redrive-lineage-migration");
    ASSERT_TRUE(database);
    auto original = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
    ASSERT_TRUE(original);
    original_sequence = *original;
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
    auto first = database->receive_event_consumer("workers", "worker-0", "receive-0", 30, 1);
    ASSERT_TRUE(first);
    auto first_dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker-0", first->delivery_token, "reject-0", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(first_dead_letter);
    auto first_redrive = database->redrive_event_consumer_dead_letter(
        "workers", first_dead_letter->dead_letter_id, "redrive-0");
    ASSERT_TRUE(first_redrive);
    first_replay_sequence = first_redrive->redriven_events.front().sequence;
    auto second = database->receive_event_consumer("workers", "worker-1", "receive-1", 30, 1);
    ASSERT_TRUE(second);
    auto second_dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker-1", second->delivery_token, "reject-1", "poison again",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(second_dead_letter);
    auto second_redrive = database->redrive_event_consumer_dead_letter(
        "workers", second_dead_letter->dead_letter_id, "redrive-1");
    ASSERT_TRUE(second_redrive);
    second_replay_sequence = second_redrive->redriven_events.front().sequence;
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE agent_events SET root_sequence=sequence,redrive_count=0;"
                          "UPDATE event_consumers SET max_redrive_count=7;"
                          "UPDATE agent_db_meta SET value='18' WHERE key='schema_version'"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "redrive-lineage-migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto events = migrated->events_after(0, 100, "jobs/a");
  ASSERT_TRUE(events);
  ASSERT_EQ(events->size(), 3U);
  EXPECT_EQ(events->at(0).sequence, original_sequence);
  EXPECT_EQ(events->at(0).root_sequence, original_sequence);
  EXPECT_EQ(events->at(0).redrive_count, 0);
  EXPECT_EQ(events->at(1).sequence, first_replay_sequence);
  EXPECT_EQ(events->at(1).root_sequence, original_sequence);
  EXPECT_EQ(events->at(1).redrive_count, 1);
  EXPECT_EQ(events->at(2).sequence, second_replay_sequence);
  EXPECT_EQ(events->at(2).root_sequence, original_sequence);
  EXPECT_EQ(events->at(2).redrive_count, 2);
  auto consumer = migrated->get_event_consumer("workers");
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->max_redrive_count, 0);
  EXPECT_TRUE(migrated->verify_integrity());
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE event_consumers SET max_redrive_count=1 WHERE "
                          "workspace='redrive-lineage-migration' AND name='workers'"));
  }
  auto policy_drift = migrated->verify_integrity();
  ASSERT_FALSE(policy_drift);
  EXPECT_EQ(policy_drift.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, IntegrityRejectsRedriveLineageGenerationDrift) {
  const auto path = database_path();
  std::int64_t redriven_sequence{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "redrive-lineage-integrity");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
    auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    auto dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker", delivery->delivery_token, "reject", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(dead_letter);
    auto redriven = database->redrive_event_consumer_dead_letter(
        "workers", dead_letter->dead_letter_id, "redrive");
    ASSERT_TRUE(redriven);
    redriven_sequence = redriven->redriven_events.front().sequence;
    EXPECT_TRUE(database->verify_integrity());
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE agent_events SET redrive_count=2 WHERE sequence=" +
                          std::to_string(redriven_sequence)));
  }
  auto corrupted = atx::agent::AgentDatabase::open(path.string(), "redrive-lineage-integrity");
  ASSERT_TRUE(corrupted);
  auto verified = corrupted->verify_integrity();
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, DeadLetterQuarantineIsIdempotentNonDestructiveAndSurvivesBackup) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".quarantine-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-quarantine");
  ASSERT_TRUE(database);
  auto first_sequence = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
  auto second_sequence =
      database->append_event("job.ready", "recoverable", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(first_sequence);
  ASSERT_TRUE(second_sequence);
  auto consumer = database->register_event_consumer(
      "workers", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 1);
  ASSERT_TRUE(consumer);
  auto first = database->receive_event_consumer("workers", "worker-a", "receive-a", 30, 1);
  ASSERT_TRUE(first);
  auto first_dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker-a", first->delivery_token, "reject-a", "permanently invalid",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(first_dead_letter);
  auto open_dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(open_dead_letters);
  ASSERT_EQ(open_dead_letters->size(), 1U);
  const auto first_dead_lettered_event_sequence =
      open_dead_letters->front().dead_lettered_event_sequence;
  EXPECT_GT(first_dead_lettered_event_sequence, 0);
  EXPECT_EQ(open_dead_letters->front().redriven_event_sequence, 0);
  EXPECT_EQ(open_dead_letters->front().quarantined_event_sequence, 0);
  auto before = database->get_event_consumer("workers");
  ASSERT_TRUE(before);
  auto quarantined = database->quarantine_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "operator-a", "quarantine-a",
      "reviewed invalid payload");
  ASSERT_TRUE(quarantined) << quarantined.error().to_string();
  EXPECT_EQ(quarantined->status, "quarantined");
  EXPECT_EQ(quarantined->quarantine_token, "quarantine-a");
  EXPECT_EQ(quarantined->quarantined_by, "operator-a");
  EXPECT_EQ(quarantined->quarantine_reason, "reviewed invalid payload");
  EXPECT_FALSE(quarantined->quarantined_at.empty());
  EXPECT_EQ(quarantined->dead_lettered_event_sequence, first_dead_lettered_event_sequence);
  EXPECT_EQ(quarantined->redriven_event_sequence, 0);
  EXPECT_GT(quarantined->quarantined_event_sequence, first_dead_lettered_event_sequence);
  EXPECT_TRUE(quarantined->redrive_token.empty());
  EXPECT_TRUE(quarantined->redriven_events.empty());
  ASSERT_EQ(quarantined->events.size(), 1U);
  EXPECT_EQ(quarantined->events.front().sequence, *first_sequence);
  auto exact = database->quarantine_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "operator-a", "quarantine-a",
      "reviewed invalid payload");
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->quarantined_at, quarantined->quarantined_at);
  EXPECT_EQ(exact->dead_lettered_event_sequence, quarantined->dead_lettered_event_sequence);
  EXPECT_EQ(exact->quarantined_event_sequence, quarantined->quarantined_event_sequence);
  auto changed_intent = database->quarantine_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "operator-a", "quarantine-a", "changed reason");
  ASSERT_FALSE(changed_intent);
  EXPECT_EQ(changed_intent.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto changed_token = database->quarantine_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "operator-a", "quarantine-b",
      "reviewed invalid payload");
  ASSERT_FALSE(changed_token);
  EXPECT_EQ(changed_token.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto forbidden_redrive = database->redrive_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "redrive-quarantined");
  ASSERT_FALSE(forbidden_redrive);
  EXPECT_EQ(forbidden_redrive.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto after = database->get_event_consumer("workers");
  ASSERT_TRUE(after);
  EXPECT_EQ(after->redrive_token_millis, before->redrive_token_millis);
  EXPECT_EQ(after->redrive_refilled_at, before->redrive_refilled_at);
  auto occurrences = database->events_after(0, 100, "jobs/a");
  ASSERT_TRUE(occurrences);
  ASSERT_EQ(occurrences->size(), 2U);

  auto second = database->receive_event_consumer("workers", "worker-b", "receive-b", 30, 1);
  ASSERT_TRUE(second);
  auto second_dead_letter = database->reject_event_consumer_delivery(
      "workers", "worker-b", second->delivery_token, "reject-b", "repair available",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(second_dead_letter);
  auto redriven = database->redrive_event_consumer_dead_letter(
      "workers", second_dead_letter->dead_letter_id, "redrive-b");
  ASSERT_TRUE(redriven) << redriven.error().to_string();
  EXPECT_GT(redriven->dead_lettered_event_sequence, quarantined->quarantined_event_sequence);
  EXPECT_GT(redriven->redriven_event_sequence, redriven->dead_lettered_event_sequence);
  EXPECT_EQ(redriven->quarantined_event_sequence, 0);
  auto quarantine_redriven = database->quarantine_event_consumer_dead_letter(
      "workers", second_dead_letter->dead_letter_id, "operator-b", "quarantine-redriven",
      "too late");
  ASSERT_FALSE(quarantine_redriven);
  EXPECT_EQ(quarantine_redriven.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto reused_token = database->quarantine_event_consumer_dead_letter(
      "workers", second_dead_letter->dead_letter_id, "operator-a", "quarantine-a",
      "reviewed invalid payload");
  ASSERT_FALSE(reused_token);
  EXPECT_EQ(reused_token.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto listed = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 2U);
  EXPECT_EQ(listed->at(0).status, "redriven");
  EXPECT_EQ(listed->at(1).status, "quarantined");
  auto lifecycle = database->events_after(0, 100, "consumers/workers");
  ASSERT_TRUE(lifecycle);
  ASSERT_EQ(lifecycle->size(), 5U);
  EXPECT_EQ(lifecycle->at(0).type, "consumer.registered");
  EXPECT_EQ(lifecycle->at(1).type, "consumer.dead_lettered");
  EXPECT_EQ(lifecycle->at(1).sequence, quarantined->dead_lettered_event_sequence);
  EXPECT_EQ(lifecycle->at(1).payload, std::to_string(first_dead_letter->dead_letter_id));
  EXPECT_EQ(lifecycle->at(2).type, "consumer.dead_letter_quarantined");
  EXPECT_EQ(lifecycle->at(2).sequence, quarantined->quarantined_event_sequence);
  EXPECT_EQ(lifecycle->at(3).type, "consumer.dead_lettered");
  EXPECT_EQ(lifecycle->at(3).sequence, redriven->dead_lettered_event_sequence);
  EXPECT_EQ(lifecycle->at(4).type, "consumer.dead_letter_redriven");
  EXPECT_EQ(lifecycle->at(4).sequence, redriven->redriven_event_sequence);
  for (const auto &event : *lifecycle) {
    EXPECT_EQ(event.root_sequence, event.sequence);
    EXPECT_EQ(event.redrive_count, 0);
  }
  EXPECT_TRUE(database->verify_integrity());

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-quarantine");
  ASSERT_TRUE(restored);
  auto restored_exact = restored->quarantine_event_consumer_dead_letter(
      "workers", first_dead_letter->dead_letter_id, "operator-a", "quarantine-a",
      "reviewed invalid payload");
  ASSERT_TRUE(restored_exact);
  EXPECT_EQ(restored_exact->quarantined_at, quarantined->quarantined_at);
  EXPECT_EQ(restored_exact->status, "quarantined");
  EXPECT_EQ(restored_exact->dead_lettered_event_sequence,
            quarantined->dead_lettered_event_sequence);
  EXPECT_EQ(restored_exact->quarantined_event_sequence, quarantined->quarantined_event_sequence);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ConcurrentQuarantineAndRedriveCommitOneTerminalOutcome) {
  const auto path = database_path();
  std::int64_t dead_letter_id{};
  std::int64_t original_sequence{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "quarantine-redrive-race");
    ASSERT_TRUE(database);
    auto event = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
    ASSERT_TRUE(event);
    original_sequence = *event;
    ASSERT_TRUE(database->register_event_consumer(
        "workers", "jobs/a", 0, 0, 0, 0, atx::agent::EventConsumerRetryJitter::None, 1, 1));
    auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    auto dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker", delivery->delivery_token, "reject", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(dead_letter);
    dead_letter_id = dead_letter->dead_letter_id;
  }

  constexpr std::size_t operation_count = 8;
  std::latch ready{operation_count};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::string> successes;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t index = 0; index < operation_count; ++index) {
    workers.emplace_back([&, index] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "quarantine-redrive-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      if (index % 2 == 0) {
        auto result = connection->quarantine_event_consumer_dead_letter(
            "workers", dead_letter_id, "operator", "quarantine", "reviewed poison");
        std::lock_guard lock{mutex};
        if (result) {
          successes.emplace_back("quarantined");
        } else {
          errors.push_back(result.error().code());
        }
      } else {
        auto result =
            connection->redrive_event_consumer_dead_letter("workers", dead_letter_id, "redrive");
        std::lock_guard lock{mutex};
        if (result) {
          successes.emplace_back("redriven");
        } else {
          errors.push_back(result.error().code());
        }
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();

  ASSERT_EQ(successes.size(), operation_count / 2);
  ASSERT_EQ(errors.size(), operation_count / 2);
  EXPECT_TRUE(std::all_of(errors.begin(), errors.end(), [](const auto error) {
    return error == atx::core::ErrorCode::InvalidArgument;
  }));
  EXPECT_TRUE(std::all_of(successes.begin(), successes.end(),
                          [&](const auto &outcome) { return outcome == successes.front(); }));
  auto database = atx::agent::AgentDatabase::open(path.string(), "quarantine-redrive-race");
  ASSERT_TRUE(database);
  auto listed = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 1U);
  EXPECT_EQ(listed->front().status, successes.front());
  auto occurrences = database->events_after(0, 100, "jobs/a");
  ASSERT_TRUE(occurrences);
  EXPECT_EQ(occurrences->size(), successes.front() == "redriven" ? 2U : 1U);
  EXPECT_EQ(occurrences->front().sequence, original_sequence);
  auto lifecycle = database->events_after(0, 100, "consumers/workers");
  ASSERT_TRUE(lifecycle);
  ASSERT_EQ(lifecycle->size(), 3U);
  EXPECT_EQ(lifecycle->at(0).type, "consumer.registered");
  EXPECT_EQ(lifecycle->at(1).type, "consumer.dead_lettered");
  EXPECT_EQ(lifecycle->at(2).type, successes.front() == "redriven"
                                       ? "consumer.dead_letter_redriven"
                                       : "consumer.dead_letter_quarantined");
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, NewConsumerSuppressesOwnControlEventsWithoutHidingExternalWork) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".self-control-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "self-control");
  ASSERT_TRUE(database);
  auto first = database->append_event("job.ready", "first", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(first);
  auto consumer = database->register_event_consumer("workers");
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->self_control_event_cutoff_sequence, 0);
  auto public_control = database->events_after(0, 100, "consumers/workers");
  ASSERT_TRUE(public_control);
  ASSERT_EQ(public_control->size(), 1U);
  EXPECT_EQ(public_control->front().type, "consumer.registered");
  auto initial_poll = database->poll_event_consumer("workers", 100);
  ASSERT_TRUE(initial_poll);
  ASSERT_EQ(initial_poll->events.size(), 1U);
  EXPECT_EQ(initial_poll->events.front().sequence, *first);
  auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 1);
  ASSERT_TRUE(delivery);
  ASSERT_EQ(delivery->events.size(), 1U);
  EXPECT_EQ(delivery->events.front().sequence, *first);
  auto rejected = database->reject_event_consumer_delivery(
      "workers", "worker", delivery->delivery_token, "reject", "poison",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(rejected);
  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  const auto dead_lettered_sequence = dead_letters->front().dead_lettered_event_sequence;
  EXPECT_GT(dead_lettered_sequence, *first);
  auto before_empty = database->get_event_consumer("workers");
  ASSERT_TRUE(before_empty);
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto empty = database->receive_event_consumer("workers", "worker",
                                                  "empty-" + std::to_string(attempt), 30, 10);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->delivery_token.empty());
    EXPECT_TRUE(empty->events.empty());
  }
  auto after_empty = database->get_event_consumer("workers");
  ASSERT_TRUE(after_empty);
  EXPECT_EQ(after_empty->cursor_sequence, before_empty->cursor_sequence);
  EXPECT_EQ(after_empty->revision, before_empty->revision);
  auto forbidden_checkpoint = database->checkpoint_event_consumer(
      "workers", after_empty->revision, dead_lettered_sequence, "self-control-checkpoint");
  ASSERT_FALSE(forbidden_checkpoint);
  EXPECT_EQ(forbidden_checkpoint.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto second = database->append_event("job.ready", "second", {}, {}, {}, {}, "jobs/b");
  ASSERT_TRUE(second);
  auto next = database->receive_event_consumer("workers", "worker", "next", 30, 10);
  ASSERT_TRUE(next);
  ASSERT_EQ(next->events.size(), 1U);
  EXPECT_EQ(next->events.front().sequence, *second);
  EXPECT_GT(next->events.front().sequence, dead_lettered_sequence);
  ASSERT_TRUE(database->settle_event_consumer_delivery("workers", "worker", next->delivery_token,
                                                       "settle-next"));
  auto monitor = database->register_event_consumer("monitor", "consumers/workers");
  ASSERT_TRUE(monitor);
  auto monitored = database->poll_event_consumer("monitor", 100);
  ASSERT_TRUE(monitored);
  ASSERT_EQ(monitored->events.size(), 2U);
  EXPECT_EQ(monitored->events.at(0).type, "consumer.registered");
  EXPECT_EQ(monitored->events.at(1).type, "consumer.dead_lettered");
  EXPECT_EQ(monitored->events.at(1).sequence, dead_lettered_sequence);
  EXPECT_TRUE(database->verify_integrity());
  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "self-control");
  ASSERT_TRUE(restored);
  auto restored_consumer = restored->get_event_consumer("workers");
  ASSERT_TRUE(restored_consumer);
  EXPECT_EQ(restored_consumer->self_control_event_cutoff_sequence, 0);
  auto restored_poll = restored->poll_event_consumer("workers", 100);
  ASSERT_TRUE(restored_poll);
  ASSERT_EQ(restored_poll->events.size(), 1U);
  EXPECT_EQ(restored_poll->events.front().type, "consumer.registered");
  EXPECT_EQ(restored_poll->events.front().subject, "consumers/monitor");
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ConsumerStatusReportsOnlyActuallyVisibleBacklogWithoutMutation) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".consumer-status-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-status");
  ASSERT_TRUE(database);

  auto broad = database->register_event_consumer("broad");
  ASSERT_TRUE(broad);
  auto broad_before = database->get_event_consumer("broad");
  ASSERT_TRUE(broad_before);
  auto only_local_control = database->events_after(0, 100, "consumers/broad");
  ASSERT_TRUE(only_local_control);
  ASSERT_EQ(only_local_control->size(), 1U);

  auto broad_status = database->get_event_consumer_status("broad");
  ASSERT_TRUE(broad_status);
  EXPECT_EQ(broad_status->event_high_watermark, only_local_control->front().sequence);
  EXPECT_EQ(broad_status->pending_visible_event_count, 0);
  EXPECT_EQ(broad_status->first_pending_visible_sequence, 0);
  EXPECT_EQ(broad_status->last_pending_visible_sequence, 0);
  EXPECT_TRUE(broad_status->oldest_pending_visible_event_at.empty());
  EXPECT_EQ(broad_status->delivery_head_state, "idle");
  EXPECT_TRUE(broad_status->delivery_head_owner.empty());
  EXPECT_EQ(broad_status->delivery_head_event_count, 0);
  EXPECT_EQ(broad_status->queued_visible_event_count, 0);
  EXPECT_EQ(broad_status->available_visible_event_count, 0);
  auto broad_fleet = database->list_event_consumer_statuses();
  ASSERT_TRUE(broad_fleet);
  ASSERT_EQ(broad_fleet->consumers.size(), 1U);
  EXPECT_EQ(broad_fleet->consumers.front().consumer.name, "broad");
  EXPECT_EQ(broad_fleet->consumers.front().event_high_watermark,
            broad_status->event_high_watermark);
  EXPECT_EQ(broad_fleet->consumers.front().pending_visible_event_count, 0);
  EXPECT_EQ(broad_fleet->consumers.front().delivery_head_state, "idle");
  auto broad_status_retry = database->get_event_consumer_status("broad");
  ASSERT_TRUE(broad_status_retry);
  EXPECT_EQ(broad_status_retry->event_high_watermark, broad_status->event_high_watermark);
  EXPECT_EQ(broad_status_retry->pending_visible_event_count,
            broad_status->pending_visible_event_count);
  auto broad_after = database->get_event_consumer("broad");
  ASSERT_TRUE(broad_after);
  EXPECT_EQ(broad_after->cursor_sequence, broad_before->cursor_sequence);
  EXPECT_EQ(broad_after->revision, broad_before->revision);

  auto filtered = database->register_event_consumer("filtered", "jobs/a");
  ASSERT_TRUE(filtered);
  auto unrelated = database->append_event("job.ready", "other", {}, {}, {}, {}, "jobs/b");
  ASSERT_TRUE(unrelated);
  auto filtered_empty = database->get_event_consumer_status("filtered");
  ASSERT_TRUE(filtered_empty);
  EXPECT_EQ(filtered_empty->event_high_watermark, *unrelated);
  EXPECT_EQ(filtered_empty->pending_visible_event_count, 0);

  auto first = database->append_event("job.ready", "first", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(first);
  auto second = database->append_event("job.ready", "second", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(second);
  auto visible_events = database->events_after(0, 100, "jobs/a");
  ASSERT_TRUE(visible_events);
  ASSERT_EQ(visible_events->size(), 2U);
  auto filtered_status = database->get_event_consumer_status("filtered");
  ASSERT_TRUE(filtered_status);
  EXPECT_EQ(filtered_status->event_high_watermark, *second);
  EXPECT_EQ(filtered_status->pending_visible_event_count, 2);
  EXPECT_EQ(filtered_status->first_pending_visible_sequence, *first);
  EXPECT_EQ(filtered_status->last_pending_visible_sequence, *second);
  EXPECT_EQ(filtered_status->oldest_pending_visible_event_at, visible_events->front().created_at);
  EXPECT_EQ(filtered_status->delivery_head_state, "idle");
  EXPECT_EQ(filtered_status->delivery_head_event_count, 0);
  EXPECT_EQ(filtered_status->queued_visible_event_count, 2);
  EXPECT_EQ(filtered_status->available_visible_event_count, 2);
  EXPECT_EQ(filtered_status->consumer.cursor_sequence, filtered->cursor_sequence);
  EXPECT_EQ(filtered_status->consumer.revision, filtered->revision);

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-status");
  ASSERT_TRUE(restored);
  auto restored_status = restored->get_event_consumer_status("filtered");
  ASSERT_TRUE(restored_status);
  EXPECT_EQ(restored_status->event_high_watermark, filtered_status->event_high_watermark);
  EXPECT_EQ(restored_status->pending_visible_event_count,
            filtered_status->pending_visible_event_count);
  EXPECT_EQ(restored_status->first_pending_visible_sequence,
            filtered_status->first_pending_visible_sequence);
  EXPECT_EQ(restored_status->last_pending_visible_sequence,
            filtered_status->last_pending_visible_sequence);
  EXPECT_EQ(restored_status->oldest_pending_visible_event_at,
            filtered_status->oldest_pending_visible_event_at);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ConsumerStatusPartitionsInFlightBackoffAndTerminalDeliveryHead) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".delivery-head-status-backup";
  auto terminal_backup_path = path;
  terminal_backup_path += ".terminal-status-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  std::filesystem::remove(terminal_backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "delivery-head-status");
  ASSERT_TRUE(database);
  auto consumer = database->register_event_consumer("workers", "jobs/a", 0, 2, 1, 2,
                                                    atx::agent::EventConsumerRetryJitter::None);
  ASSERT_TRUE(consumer);
  auto first = database->append_event("job.ready", "first", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(first);
  auto second = database->append_event("job.ready", "second", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(second);

  auto idle = database->get_event_consumer_status("workers");
  ASSERT_TRUE(idle);
  EXPECT_EQ(idle->delivery_head_state, "idle");
  EXPECT_EQ(idle->pending_visible_event_count, 2);
  EXPECT_EQ(idle->delivery_head_event_count, 0);
  EXPECT_EQ(idle->queued_visible_event_count, 2);
  EXPECT_EQ(idle->available_visible_event_count, 2);
  EXPECT_EQ(idle->retained_dead_letter_count, 0);
  EXPECT_EQ(idle->open_dead_letter_count, 0);
  EXPECT_TRUE(idle->oldest_open_dead_letter_at.empty());

  auto first_delivery = database->receive_event_consumer("workers", "worker-a", "receive-1", 30, 1);
  ASSERT_TRUE(first_delivery);
  ASSERT_EQ(first_delivery->events.size(), 1U);
  EXPECT_EQ(first_delivery->events.front().sequence, *first);
  auto in_flight = database->get_event_consumer_status("workers");
  ASSERT_TRUE(in_flight);
  EXPECT_EQ(in_flight->delivery_head_state, "in_flight");
  EXPECT_EQ(in_flight->delivery_head_owner, "worker-a");
  EXPECT_EQ(in_flight->delivery_head_attempt, 1);
  EXPECT_EQ(in_flight->delivery_head_through_sequence, *first);
  EXPECT_EQ(in_flight->delivery_head_event_count, 1);
  EXPECT_FALSE(in_flight->delivery_head_expires_at.empty());
  EXPECT_FALSE(in_flight->delivery_head_retry_at.empty());
  EXPECT_EQ(in_flight->pending_visible_event_count, 2);
  EXPECT_EQ(in_flight->queued_visible_event_count, 1);
  EXPECT_EQ(in_flight->available_visible_event_count, 0);
  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "delivery-head-status");
  ASSERT_TRUE(restored);
  auto restored_in_flight = restored->get_event_consumer_status("workers");
  ASSERT_TRUE(restored_in_flight);
  EXPECT_EQ(restored_in_flight->delivery_head_state, "in_flight");
  EXPECT_EQ(restored_in_flight->delivery_head_owner, in_flight->delivery_head_owner);
  EXPECT_EQ(restored_in_flight->delivery_head_event_count, in_flight->delivery_head_event_count);
  EXPECT_EQ(restored_in_flight->queued_visible_event_count, in_flight->queued_visible_event_count);

  auto rejected = database->reject_event_consumer_delivery(
      "workers", "worker-a", first_delivery->delivery_token, "reject-1", "retry me",
      atx::agent::EventConsumerRejectionDisposition::Retry);
  ASSERT_TRUE(rejected);
  auto cooling = database->get_event_consumer_status("workers");
  ASSERT_TRUE(cooling);
  EXPECT_EQ(cooling->delivery_head_state, "retry_backoff");
  EXPECT_EQ(cooling->delivery_head_event_count, 1);
  EXPECT_EQ(cooling->queued_visible_event_count, 1);
  EXPECT_EQ(cooling->available_visible_event_count, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto ready = database->get_event_consumer_status("workers");
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready->delivery_head_state, "redelivery_ready");
  EXPECT_EQ(ready->delivery_head_event_count, 1);
  EXPECT_EQ(ready->queued_visible_event_count, 1);
  EXPECT_EQ(ready->available_visible_event_count, 1);
  auto second_attempt = database->receive_event_consumer("workers", "worker-b", "receive-2", 1, 1);
  ASSERT_TRUE(second_attempt);
  ASSERT_EQ(second_attempt->events.size(), 1U);
  EXPECT_EQ(second_attempt->events.front().sequence, *first);
  EXPECT_EQ(second_attempt->attempt, 2);
  auto second_in_flight = database->get_event_consumer_status("workers");
  ASSERT_TRUE(second_in_flight);
  EXPECT_EQ(second_in_flight->delivery_head_state, "in_flight");
  EXPECT_EQ(second_in_flight->delivery_head_attempt, 2);

  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
  auto terminal = database->get_event_consumer_status("workers");
  ASSERT_TRUE(terminal);
  EXPECT_EQ(terminal->delivery_head_state, "dead_letter_ready");
  EXPECT_EQ(terminal->delivery_head_event_count, 1);
  EXPECT_EQ(terminal->queued_visible_event_count, 1);
  EXPECT_EQ(terminal->available_visible_event_count, 1);
  auto next = database->receive_event_consumer("workers", "worker-c", "receive-3", 30, 1);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->dead_lettered_batches, 1);
  EXPECT_EQ(next->dead_lettered_events, 1);
  ASSERT_EQ(next->events.size(), 1U);
  EXPECT_EQ(next->events.front().sequence, *second);
  auto next_in_flight = database->get_event_consumer_status("workers");
  ASSERT_TRUE(next_in_flight);
  EXPECT_EQ(next_in_flight->delivery_head_state, "in_flight");
  EXPECT_EQ(next_in_flight->pending_visible_event_count, 1);
  EXPECT_EQ(next_in_flight->delivery_head_event_count, 1);
  EXPECT_EQ(next_in_flight->queued_visible_event_count, 0);
  EXPECT_EQ(next_in_flight->available_visible_event_count, 0);
  EXPECT_EQ(next_in_flight->retained_dead_letter_count, 1);
  EXPECT_EQ(next_in_flight->open_dead_letter_count, 1);
  EXPECT_EQ(next_in_flight->open_dead_letter_event_count, 1);
  EXPECT_FALSE(next_in_flight->oldest_open_dead_letter_at.empty());
  EXPECT_EQ(next_in_flight->redriven_dead_letter_count, 0);
  EXPECT_EQ(next_in_flight->quarantined_dead_letter_count, 0);
  ASSERT_TRUE(database->settle_event_consumer_delivery("workers", "worker-c", next->delivery_token,
                                                       "settle-next"));
  auto empty = database->get_event_consumer_status("workers");
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty->delivery_head_state, "idle");
  EXPECT_EQ(empty->pending_visible_event_count, 0);
  EXPECT_EQ(empty->queued_visible_event_count, 0);
  EXPECT_EQ(empty->available_visible_event_count, 0);
  EXPECT_EQ(empty->open_dead_letter_count, 1);
  EXPECT_EQ(empty->open_dead_letter_event_count, 1);
  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  auto quarantined = database->quarantine_event_consumer_dead_letter(
      "workers", dead_letters->front().id, "operator", "quarantine-head",
      "reviewed terminal poison");
  ASSERT_TRUE(quarantined);
  auto resolved = database->get_event_consumer_status("workers");
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->pending_visible_event_count, 0);
  EXPECT_EQ(resolved->retained_dead_letter_count, 1);
  EXPECT_EQ(resolved->open_dead_letter_count, 0);
  EXPECT_EQ(resolved->open_dead_letter_event_count, 0);
  EXPECT_TRUE(resolved->oldest_open_dead_letter_at.empty());
  EXPECT_EQ(resolved->redriven_dead_letter_count, 0);
  EXPECT_EQ(resolved->quarantined_dead_letter_count, 1);
  ASSERT_TRUE(database->backup_to(terminal_backup_path.string()));
  auto restored_terminal =
      atx::agent::AgentDatabase::open(terminal_backup_path.string(), "delivery-head-status");
  ASSERT_TRUE(restored_terminal);
  auto restored_resolved = restored_terminal->get_event_consumer_status("workers");
  ASSERT_TRUE(restored_resolved);
  EXPECT_EQ(restored_resolved->retained_dead_letter_count, resolved->retained_dead_letter_count);
  EXPECT_EQ(restored_resolved->open_dead_letter_count, resolved->open_dead_letter_count);
  EXPECT_EQ(restored_resolved->quarantined_dead_letter_count,
            resolved->quarantined_dead_letter_count);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, ConsumerFleetStatusReportsCompleteOrderedMixedStateSnapshot) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".fleet-status-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "fleet-status");
  ASSERT_TRUE(database);

  ASSERT_TRUE(database->register_event_consumer("z-quarantined", "jobs/quarantined"));
  ASSERT_TRUE(database->register_event_consumer("a-idle", "jobs/idle"));
  ASSERT_TRUE(database->register_event_consumer("f-redriven", "jobs/redriven"));
  ASSERT_TRUE(database->register_event_consumer("b-in-flight", "jobs/in-flight"));
  ASSERT_TRUE(database->register_event_consumer("e-open-dlq", "jobs/open"));
  ASSERT_TRUE(database->register_event_consumer("c-retry-backoff", "jobs/backoff", 0, 3, 3'600,
                                                3'600, atx::agent::EventConsumerRetryJitter::None));

  ASSERT_TRUE(database->append_event("job.ready", "idle", {}, {}, {}, {}, "jobs/idle"));
  ASSERT_TRUE(database->append_event("job.ready", "in-flight", {}, {}, {}, {}, "jobs/in-flight"));
  auto in_flight =
      database->receive_event_consumer("b-in-flight", "owner-b", "receive-b", 3'600, 1);
  ASSERT_TRUE(in_flight);

  ASSERT_TRUE(database->append_event("job.ready", "backoff", {}, {}, {}, {}, "jobs/backoff"));
  auto cooling =
      database->receive_event_consumer("c-retry-backoff", "owner-c", "receive-c", 3'600, 1);
  ASSERT_TRUE(cooling);
  ASSERT_TRUE(database->reject_event_consumer_delivery(
      "c-retry-backoff", "owner-c", cooling->delivery_token, "reject-c", "transient",
      atx::agent::EventConsumerRejectionDisposition::Retry));

  const auto make_dead_letter = [&](std::string_view name, std::string_view subject,
                                    std::string_view token) -> std::int64_t {
    auto event = database->append_event("job.ready", "poison", {}, {}, {}, {}, subject);
    EXPECT_TRUE(event);
    auto delivery = database->receive_event_consumer(name, "poison-owner", token, 3'600, 1);
    EXPECT_TRUE(delivery);
    if (!delivery) {
      return 0;
    }
    auto rejected = database->reject_event_consumer_delivery(
        name, "poison-owner", delivery->delivery_token, std::string{token} + "-reject", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    EXPECT_TRUE(rejected);
    return rejected ? rejected->dead_letter_id : 0;
  };
  const auto open_id = make_dead_letter("e-open-dlq", "jobs/open", "open");
  const auto redriven_id = make_dead_letter("f-redriven", "jobs/redriven", "redriven");
  const auto quarantined_id = make_dead_letter("z-quarantined", "jobs/quarantined", "quarantined");
  ASSERT_GT(open_id, 0);
  ASSERT_GT(redriven_id, 0);
  ASSERT_GT(quarantined_id, 0);
  ASSERT_TRUE(database->redrive_event_consumer_dead_letter("f-redriven", redriven_id, "redrive-f"));
  ASSERT_TRUE(database->quarantine_event_consumer_dead_letter(
      "z-quarantined", quarantined_id, "operator", "quarantine-z", "reviewed"));

  auto before = database->get_event_consumer("b-in-flight");
  ASSERT_TRUE(before);
  auto other = atx::agent::AgentDatabase::open(path.string(), "other-workspace");
  ASSERT_TRUE(other);
  auto other_sequence =
      other->append_event("other.ready", "later global event", {}, {}, {}, {}, "other");
  ASSERT_TRUE(other_sequence);

  auto fleet = database->list_event_consumer_statuses();
  ASSERT_TRUE(fleet) << fleet.error().to_string();
  EXPECT_EQ(fleet->workspace, "fleet-status");
  EXPECT_FALSE(fleet->observed_at.empty());
  EXPECT_LT(fleet->event_high_watermark, *other_sequence);
  ASSERT_EQ(fleet->consumers.size(), 6U);
  const std::array<std::string_view, 6> expected_names{
      "a-idle", "b-in-flight", "c-retry-backoff", "e-open-dlq", "f-redriven", "z-quarantined"};
  for (std::size_t index = 0; index < expected_names.size(); ++index) {
    EXPECT_EQ(fleet->consumers[index].consumer.name, expected_names[index]);
    EXPECT_EQ(fleet->consumers[index].observed_at, fleet->observed_at);
    EXPECT_EQ(fleet->consumers[index].event_high_watermark, fleet->event_high_watermark);
    EXPECT_EQ(fleet->consumers[index].consumer_state_revision, fleet->consumer_state_revision);
    auto point = database->get_event_consumer_status(expected_names[index]);
    ASSERT_TRUE(point);
    const auto &listed = fleet->consumers[index];
    EXPECT_EQ(listed.consumer.subject_filter, point->consumer.subject_filter);
    EXPECT_EQ(listed.consumer.cursor_sequence, point->consumer.cursor_sequence);
    EXPECT_EQ(listed.consumer.revision, point->consumer.revision);
    EXPECT_EQ(listed.event_high_watermark, point->event_high_watermark);
    EXPECT_EQ(listed.consumer_state_revision, point->consumer_state_revision);
    EXPECT_EQ(listed.next_dynamic_transition_at, point->next_dynamic_transition_at);
    EXPECT_EQ(listed.pending_visible_event_count, point->pending_visible_event_count);
    EXPECT_EQ(listed.first_pending_visible_sequence, point->first_pending_visible_sequence);
    EXPECT_EQ(listed.last_pending_visible_sequence, point->last_pending_visible_sequence);
    EXPECT_EQ(listed.oldest_pending_visible_event_at, point->oldest_pending_visible_event_at);
    EXPECT_EQ(listed.delivery_head_state, point->delivery_head_state);
    EXPECT_EQ(listed.delivery_head_owner, point->delivery_head_owner);
    EXPECT_EQ(listed.delivery_head_attempt, point->delivery_head_attempt);
    EXPECT_EQ(listed.delivery_head_through_sequence, point->delivery_head_through_sequence);
    EXPECT_EQ(listed.delivery_head_event_count, point->delivery_head_event_count);
    EXPECT_EQ(listed.delivery_head_expires_at, point->delivery_head_expires_at);
    EXPECT_EQ(listed.delivery_head_retry_at, point->delivery_head_retry_at);
    EXPECT_EQ(listed.queued_visible_event_count, point->queued_visible_event_count);
    EXPECT_EQ(listed.available_visible_event_count, point->available_visible_event_count);
    EXPECT_EQ(listed.retained_dead_letter_count, point->retained_dead_letter_count);
    EXPECT_EQ(listed.open_dead_letter_count, point->open_dead_letter_count);
    EXPECT_EQ(listed.open_dead_letter_event_count, point->open_dead_letter_event_count);
    EXPECT_EQ(listed.oldest_open_dead_letter_at, point->oldest_open_dead_letter_at);
    EXPECT_EQ(listed.redriven_dead_letter_count, point->redriven_dead_letter_count);
    EXPECT_EQ(listed.quarantined_dead_letter_count, point->quarantined_dead_letter_count);
  }
  EXPECT_EQ(fleet->consumers[0].delivery_head_state, "idle");
  EXPECT_EQ(fleet->consumers[0].pending_visible_event_count, 1);
  EXPECT_EQ(fleet->consumers[0].available_visible_event_count, 1);
  EXPECT_EQ(fleet->consumers[1].delivery_head_state, "in_flight");
  EXPECT_EQ(fleet->consumers[1].delivery_head_owner, "owner-b");
  EXPECT_EQ(fleet->consumers[1].delivery_head_event_count, 1);
  EXPECT_EQ(fleet->consumers[2].delivery_head_state, "retry_backoff");
  EXPECT_EQ(fleet->consumers[2].available_visible_event_count, 0);
  EXPECT_EQ(fleet->consumers[3].open_dead_letter_count, 1);
  EXPECT_EQ(fleet->consumers[3].open_dead_letter_event_count, 1);
  EXPECT_EQ(fleet->consumers[4].redriven_dead_letter_count, 1);
  EXPECT_EQ(fleet->consumers[4].quarantined_dead_letter_count, 0);
  EXPECT_EQ(fleet->consumers[5].open_dead_letter_count, 0);
  EXPECT_EQ(fleet->consumers[5].quarantined_dead_letter_count, 1);

  auto repeated = database->list_event_consumer_statuses();
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->event_high_watermark, fleet->event_high_watermark);
  auto after = database->get_event_consumer("b-in-flight");
  ASSERT_TRUE(after);
  EXPECT_EQ(after->cursor_sequence, before->cursor_sequence);
  EXPECT_EQ(after->revision, before->revision);
  EXPECT_EQ(after->updated_at, before->updated_at);

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "fleet-status");
  ASSERT_TRUE(restored);
  auto restored_fleet = restored->list_event_consumer_statuses();
  ASSERT_TRUE(restored_fleet);
  EXPECT_EQ(restored_fleet->workspace, fleet->workspace);
  EXPECT_EQ(restored_fleet->event_high_watermark, fleet->event_high_watermark);
  ASSERT_EQ(restored_fleet->consumers.size(), fleet->consumers.size());
  for (std::size_t index = 0; index < fleet->consumers.size(); ++index) {
    EXPECT_EQ(restored_fleet->consumers[index].consumer.name,
              fleet->consumers[index].consumer.name);
    EXPECT_EQ(restored_fleet->consumers[index].delivery_head_state,
              fleet->consumers[index].delivery_head_state);
    EXPECT_EQ(restored_fleet->consumers[index].retained_dead_letter_count,
              fleet->consumers[index].retained_dead_letter_count);
  }
  ASSERT_TRUE(database->settle_event_consumer_delivery(
      "b-in-flight", "owner-b", in_flight->delivery_token, "settle-after-observation"));
  EXPECT_TRUE(database->verify_integrity());
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ConsumerFleetStatusIsAtomicAcrossConcurrentRegistration) {
  const auto path = database_path();
  {
    auto initialized = atx::agent::AgentDatabase::open(path.string(), "fleet-race");
    ASSERT_TRUE(initialized);
  }

  for (std::size_t iteration = 0; iteration < 12; ++iteration) {
    const std::string name = "consumer-" + std::to_string(iteration);
    std::latch ready{2};
    std::latch start{1};
    std::mutex mutex;
    bool registered = false;
    bool listed = false;
    atx::agent::EventConsumerFleetStatus fleet;
    std::vector<atx::core::ErrorCode> errors;
    std::vector<std::jthread> workers;
    workers.emplace_back([&] {
      auto writer = atx::agent::AgentDatabase::open(path.string(), "fleet-race");
      if (!writer) {
        std::lock_guard lock{mutex};
        errors.push_back(writer.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto result = writer->register_event_consumer(name, "jobs/" + name);
      std::lock_guard lock{mutex};
      if (result) {
        registered = true;
      } else {
        errors.push_back(result.error().code());
      }
    });
    workers.emplace_back([&] {
      auto reader = atx::agent::AgentDatabase::open(path.string(), "fleet-race");
      if (!reader) {
        std::lock_guard lock{mutex};
        errors.push_back(reader.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto result = reader->list_event_consumer_statuses();
      std::lock_guard lock{mutex};
      if (result) {
        fleet = std::move(*result);
        listed = true;
      } else {
        errors.push_back(result.error().code());
      }
    });
    ready.wait();
    start.count_down();
    workers.clear();
    ASSERT_TRUE(errors.empty());
    ASSERT_TRUE(registered);
    ASSERT_TRUE(listed);

    const bool included = std::ranges::any_of(
        fleet.consumers, [&](const auto &status) { return status.consumer.name == name; });
    EXPECT_EQ(fleet.consumers.size(), iteration + (included ? 1U : 0U));
    EXPECT_TRUE(std::ranges::is_sorted(fleet.consumers, {},
                                       [](const auto &status) { return status.consumer.name; }));
    for (const auto &status : fleet.consumers) {
      EXPECT_EQ(status.observed_at, fleet.observed_at);
      EXPECT_EQ(status.event_high_watermark, fleet.event_high_watermark);
      EXPECT_EQ(status.consumer_state_revision, fleet.consumer_state_revision);
    }
    auto verifier = atx::agent::AgentDatabase::open(path.string(), "fleet-race");
    ASSERT_TRUE(verifier);
    auto registration = verifier->events_after(0, 10, "consumers/" + name);
    ASSERT_TRUE(registration);
    ASSERT_EQ(registration->size(), 1U);
    if (included) {
      EXPECT_LE(registration->front().sequence, fleet.event_high_watermark);
    } else {
      EXPECT_GT(registration->front().sequence, fleet.event_high_watermark);
    }
    EXPECT_EQ(fleet.consumer_state_revision,
              static_cast<std::int64_t>(iteration + (included ? 1U : 0U)));
  }
}

TEST(AgentDatabase, ConditionalConsumerFleetCacheValidationIsAtomicAcrossRegistration) {
  const auto path = database_path();
  {
    auto initialized = atx::agent::AgentDatabase::open(path.string(), "conditional-fleet-race");
    ASSERT_TRUE(initialized);
  }

  for (std::size_t iteration = 0; iteration < 8; ++iteration) {
    auto coordinator = atx::agent::AgentDatabase::open(path.string(), "conditional-fleet-race");
    ASSERT_TRUE(coordinator);
    auto before = coordinator->list_event_consumer_statuses();
    ASSERT_TRUE(before);
    const auto cached = fleet_validator(*before);
    const std::string name = "conditional-consumer-" + std::to_string(iteration);

    std::latch ready{2};
    std::latch start{1};
    std::mutex mutex;
    bool registered = false;
    bool validated = false;
    atx::agent::EventConsumerFleetCacheValidation result;
    std::vector<atx::core::ErrorCode> errors;
    std::vector<std::jthread> workers;
    workers.emplace_back([&] {
      auto writer = atx::agent::AgentDatabase::open(path.string(), "conditional-fleet-race");
      if (!writer) {
        std::lock_guard lock{mutex};
        errors.push_back(writer.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto registration = writer->register_event_consumer(name, "jobs/" + name);
      std::lock_guard lock{mutex};
      if (registration) {
        registered = true;
      } else {
        errors.push_back(registration.error().code());
      }
    });
    workers.emplace_back([&] {
      auto reader = atx::agent::AgentDatabase::open(path.string(), "conditional-fleet-race");
      if (!reader) {
        std::lock_guard lock{mutex};
        errors.push_back(reader.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto conditional = reader->list_event_consumer_statuses_if_current(cached);
      std::lock_guard lock{mutex};
      if (conditional) {
        result = std::move(*conditional);
        validated = true;
      } else {
        errors.push_back(conditional.error().code());
      }
    });
    ready.wait();
    start.count_down();
    workers.clear();
    ASSERT_TRUE(errors.empty());
    ASSERT_TRUE(registered);
    ASSERT_TRUE(validated);

    if (result.cache_valid) {
      EXPECT_FALSE(result.snapshot.has_value());
      EXPECT_EQ(result.current.event_high_watermark, cached.event_high_watermark);
      EXPECT_EQ(result.current.consumer_state_revision, cached.consumer_state_revision);
      EXPECT_EQ(result.current.next_dynamic_transition_at, cached.next_dynamic_transition_at);
    } else {
      ASSERT_TRUE(result.snapshot.has_value());
      const auto &snapshot = *result.snapshot;
      EXPECT_EQ(snapshot.consumers.size(), before->consumers.size() + 1U);
      EXPECT_GT(snapshot.event_high_watermark, cached.event_high_watermark);
      EXPECT_GT(snapshot.consumer_state_revision, cached.consumer_state_revision);
      EXPECT_EQ(result.current.event_high_watermark, snapshot.event_high_watermark);
      EXPECT_EQ(result.current.consumer_state_revision, snapshot.consumer_state_revision);
      EXPECT_EQ(result.current.next_dynamic_transition_at, snapshot.next_dynamic_transition_at);
      ASSERT_FALSE(snapshot.consumers.empty());
      EXPECT_EQ(snapshot.consumers.back().consumer.name, name);
      for (const auto &status : snapshot.consumers) {
        EXPECT_EQ(status.observed_at, snapshot.observed_at);
        EXPECT_EQ(status.event_high_watermark, snapshot.event_high_watermark);
        EXPECT_EQ(status.consumer_state_revision, snapshot.consumer_state_revision);
      }
    }
  }
}

TEST(AgentDatabase, ConsumerFleetStatusEnforcesCompleteSnapshotBoundAndEmptyCase) {
  auto empty = atx::agent::AgentDatabase::open_memory("empty-fleet");
  ASSERT_TRUE(empty);
  auto empty_fleet = empty->list_event_consumer_statuses();
  ASSERT_TRUE(empty_fleet);
  EXPECT_EQ(empty_fleet->workspace, "empty-fleet");
  EXPECT_FALSE(empty_fleet->observed_at.empty());
  EXPECT_EQ(empty_fleet->event_high_watermark, 0);
  EXPECT_TRUE(empty_fleet->consumers.empty());
  auto empty_hit = empty->list_event_consumer_statuses_if_current(fleet_validator(*empty_fleet));
  ASSERT_TRUE(empty_hit);
  EXPECT_TRUE(empty_hit->cache_valid);
  EXPECT_FALSE(empty_hit->snapshot.has_value());

  const auto path = database_path();
  {
    auto initialized = atx::agent::AgentDatabase::open(path.string(), "fleet-bound");
    ASSERT_TRUE(initialized);
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec(
        "WITH RECURSIVE names(value) AS (SELECT 0 UNION ALL SELECT value+1 FROM names WHERE "
        "value<999) INSERT INTO event_consumers(workspace,name,subject_filter) SELECT "
        "'fleet-bound',printf('consumer-%04d',value),'unmatched' FROM names"));
  }
  {
    auto bounded = atx::agent::AgentDatabase::open(path.string(), "fleet-bound");
    ASSERT_TRUE(bounded);
    auto fleet = bounded->list_event_consumer_statuses();
    ASSERT_TRUE(fleet) << fleet.error().to_string();
    ASSERT_EQ(fleet->consumers.size(), 1'000U);
    EXPECT_EQ(fleet->consumers.front().consumer.name, "consumer-0000");
    EXPECT_EQ(fleet->consumers.back().consumer.name, "consumer-0999");
    auto hit = bounded->list_event_consumer_statuses_if_current(fleet_validator(*fleet));
    ASSERT_TRUE(hit);
    EXPECT_TRUE(hit->cache_valid);
  }
  std::int64_t oversized_revision{};
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("INSERT INTO event_consumers(workspace,name,subject_filter) VALUES("
                          "'fleet-bound','consumer-1000','unmatched')"));
    auto revision = raw->prepare("SELECT revision FROM event_consumer_state_revisions WHERE "
                                 "workspace='fleet-bound'");
    ASSERT_TRUE(revision);
    auto revision_step = revision->step();
    ASSERT_TRUE(revision_step);
    ASSERT_EQ(*revision_step, atx::core::db::Statement::Step::Row);
    oversized_revision = revision->column_int(0);
  }
  auto oversized = atx::agent::AgentDatabase::open(path.string(), "fleet-bound");
  ASSERT_TRUE(oversized);
  auto rejected = oversized->list_event_consumer_statuses();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::OutOfRange);
  atx::agent::EventConsumerFleetValidator oversized_cached{
      "fleet-bound", 0, oversized_revision, {}};
  auto conditional_rejected = oversized->list_event_consumer_statuses_if_current(oversized_cached);
  ASSERT_FALSE(conditional_rejected);
  EXPECT_EQ(conditional_rejected.error().code(), atx::core::ErrorCode::OutOfRange);
  EXPECT_TRUE(oversized->get_event_consumer_status("consumer-0000"));
}

TEST(AgentDatabase, ConsumerFleetStatusAvoidsWorkspaceTailScansForSelectiveFilters) {
  const auto path = database_path();
  {
    auto initialized = atx::agent::AgentDatabase::open(path.string(), "fleet-selective-scale");
    ASSERT_TRUE(initialized);
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec(
        "BEGIN;WITH RECURSIVE events(value) AS (SELECT 0 UNION ALL SELECT value+1 FROM events "
        "WHERE value<49999) INSERT INTO agent_events(workspace,event_type,subject,payload) SELECT "
        "'fleet-selective-scale','job.ready',printf('jobs/%04d',value%1000),'payload' FROM events;"
        "WITH RECURSIVE consumers(value) AS (SELECT 0 UNION ALL SELECT value+1 FROM consumers "
        "WHERE value<49) INSERT INTO event_consumers(workspace,name,subject_filter) SELECT "
        "'fleet-selective-scale',printf('consumer-%03d',value),printf('missing/%03d',value) FROM "
        "consumers;COMMIT;"));
  }
  auto database = atx::agent::AgentDatabase::open(path.string(), "fleet-selective-scale");
  ASSERT_TRUE(database);
  const auto started = std::chrono::steady_clock::now();
  auto fleet = database->list_event_consumer_statuses();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(fleet) << fleet.error().to_string();
  ASSERT_EQ(fleet->consumers.size(), 50U);
  EXPECT_EQ(fleet->event_high_watermark, 50'000);
  for (const auto &status : fleet->consumers) {
    EXPECT_EQ(status.pending_visible_event_count, 0);
  }
  // INDEXED BY makes the subject-index access path a preparation-time
  // requirement; this generous ceiling guards the historical 50x tail-scan
  // regression without treating wall time as the semantic oracle.
  EXPECT_LT(elapsed, std::chrono::seconds{2});
}

TEST(AgentDatabase, ConditionalConsumerFleetCacheValidationUsesAllThreeAuthoritativeMarkers) {
  const auto path = database_path();
  auto database = atx::agent::AgentDatabase::open(path.string(), "conditional-fleet");
  ASSERT_TRUE(database);

  auto empty = database->list_event_consumer_statuses();
  ASSERT_TRUE(empty);
  auto empty_hit = database->list_event_consumer_statuses_if_current(fleet_validator(*empty));
  ASSERT_TRUE(empty_hit);
  EXPECT_TRUE(empty_hit->cache_valid);
  EXPECT_FALSE(empty_hit->snapshot.has_value());
  EXPECT_EQ(empty_hit->current.workspace, empty->workspace);
  EXPECT_EQ(empty_hit->current.event_high_watermark, 0);
  EXPECT_EQ(empty_hit->current.consumer_state_revision, 0);
  EXPECT_TRUE(empty_hit->current.next_dynamic_transition_at.empty());
  EXPECT_FALSE(empty_hit->validated_at.empty());

  auto invalid = fleet_validator(*empty);
  invalid.workspace = "wrong-workspace";
  auto wrong_workspace = database->list_event_consumer_statuses_if_current(invalid);
  ASSERT_FALSE(wrong_workspace);
  EXPECT_EQ(wrong_workspace.error().code(), atx::core::ErrorCode::InvalidArgument);
  invalid = fleet_validator(*empty);
  invalid.event_high_watermark = -1;
  EXPECT_FALSE(database->list_event_consumer_statuses_if_current(invalid));
  invalid = fleet_validator(*empty);
  invalid.consumer_state_revision = -1;
  EXPECT_FALSE(database->list_event_consumer_statuses_if_current(invalid));
  invalid = fleet_validator(*empty);
  invalid.next_dynamic_transition_at = "2026-02-30T12:00:00.000Z";
  auto invalid_calendar = database->list_event_consumer_statuses_if_current(invalid);
  ASSERT_FALSE(invalid_calendar);
  EXPECT_EQ(invalid_calendar.error().code(), atx::core::ErrorCode::InvalidArgument);

  ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 0, 2, 4));
  ASSERT_TRUE(database->append_event("job.ready", "one", {}, {}, {}, {}, "jobs/a"));
  auto ready = database->list_event_consumer_statuses();
  ASSERT_TRUE(ready);
  ASSERT_EQ(ready->consumers.size(), 1U);
  EXPECT_EQ(ready->consumers.front().delivery_head_state, "idle");
  auto ready_hit = database->list_event_consumer_statuses_if_current(fleet_validator(*ready));
  ASSERT_TRUE(ready_hit);
  EXPECT_TRUE(ready_hit->cache_valid);

  auto future = fleet_validator(*ready);
  ++future.event_high_watermark;
  ++future.consumer_state_revision;
  auto future_miss = database->list_event_consumer_statuses_if_current(future);
  ASSERT_TRUE(future_miss);
  EXPECT_FALSE(future_miss->cache_valid);
  ASSERT_TRUE(future_miss->snapshot.has_value());
  EXPECT_EQ(future_miss->snapshot->event_high_watermark, ready->event_high_watermark);
  EXPECT_EQ(future_miss->snapshot->consumer_state_revision, ready->consumer_state_revision);

  ASSERT_TRUE(database->append_event("job.ready", "two", {}, {}, {}, {}, "jobs/a"));
  auto event_miss = database->list_event_consumer_statuses_if_current(fleet_validator(*ready));
  ASSERT_TRUE(event_miss);
  EXPECT_FALSE(event_miss->cache_valid);
  ASSERT_TRUE(event_miss->snapshot.has_value());
  EXPECT_GT(event_miss->snapshot->event_high_watermark, ready->event_high_watermark);
  EXPECT_EQ(event_miss->snapshot->consumer_state_revision, ready->consumer_state_revision);
  const auto before_lease = fleet_validator(*event_miss->snapshot);

  auto delivery = database->receive_event_consumer("workers", "owner", "conditional-receive", 2, 2);
  ASSERT_TRUE(delivery);
  auto control_miss = database->list_event_consumer_statuses_if_current(before_lease);
  ASSERT_TRUE(control_miss);
  EXPECT_FALSE(control_miss->cache_valid);
  ASSERT_TRUE(control_miss->snapshot.has_value());
  EXPECT_EQ(control_miss->snapshot->event_high_watermark, before_lease.event_high_watermark);
  EXPECT_GT(control_miss->snapshot->consumer_state_revision, before_lease.consumer_state_revision);
  ASSERT_EQ(control_miss->snapshot->consumers.size(), 1U);
  EXPECT_EQ(control_miss->snapshot->consumers.front().delivery_head_state, "in_flight");
  EXPECT_FALSE(control_miss->snapshot->next_dynamic_transition_at.empty());
  const auto leased = fleet_validator(*control_miss->snapshot);

  auto lease_hit = database->list_event_consumer_statuses_if_current(leased);
  ASSERT_TRUE(lease_hit);
  EXPECT_TRUE(lease_hit->cache_valid);
  EXPECT_FALSE(lease_hit->snapshot.has_value());
  EXPECT_EQ(lease_hit->current.next_dynamic_transition_at, leased.next_dynamic_transition_at);
  EXPECT_LT(lease_hit->validated_at, leased.next_dynamic_transition_at);

  auto fabricated_empty = leased;
  fabricated_empty.next_dynamic_transition_at.clear();
  auto fabricated_empty_miss = database->list_event_consumer_statuses_if_current(fabricated_empty);
  ASSERT_TRUE(fabricated_empty_miss);
  EXPECT_FALSE(fabricated_empty_miss->cache_valid);
  ASSERT_TRUE(fabricated_empty_miss->snapshot.has_value());
  EXPECT_EQ(fabricated_empty_miss->snapshot->next_dynamic_transition_at,
            leased.next_dynamic_transition_at);

  auto fabricated_future = leased;
  fabricated_future.next_dynamic_transition_at = "2099-12-31T23:59:59.999Z";
  auto fabricated_future_miss =
      database->list_event_consumer_statuses_if_current(fabricated_future);
  ASSERT_TRUE(fabricated_future_miss);
  EXPECT_FALSE(fabricated_future_miss->cache_valid);
  ASSERT_TRUE(fabricated_future_miss->snapshot.has_value());

  auto fabricated_past = leased;
  fabricated_past.next_dynamic_transition_at = "2000-01-01T00:00:00.000Z";
  auto fabricated_past_miss = database->list_event_consumer_statuses_if_current(fabricated_past);
  ASSERT_TRUE(fabricated_past_miss);
  EXPECT_FALSE(fabricated_past_miss->cache_valid);

  std::this_thread::sleep_for(std::chrono::milliseconds{2'100});
  auto time_miss = database->list_event_consumer_statuses_if_current(leased);
  ASSERT_TRUE(time_miss);
  EXPECT_FALSE(time_miss->cache_valid);
  ASSERT_TRUE(time_miss->snapshot.has_value());
  EXPECT_EQ(time_miss->snapshot->event_high_watermark, leased.event_high_watermark);
  EXPECT_EQ(time_miss->snapshot->consumer_state_revision, leased.consumer_state_revision);
  ASSERT_EQ(time_miss->snapshot->consumers.size(), 1U);
  EXPECT_EQ(time_miss->snapshot->consumers.front().delivery_head_state, "retry_backoff");
  EXPECT_NE(time_miss->snapshot->next_dynamic_transition_at, leased.next_dynamic_transition_at);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, ConditionalConsumerFleetCacheHitBypassesDetailedIndexPreparation) {
  const auto path = database_path();
  auto database = atx::agent::AgentDatabase::open(path.string(), "conditional-fleet-bypass");
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->register_event_consumer("workers"));
  ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
  auto fleet = database->list_event_consumer_statuses();
  ASSERT_TRUE(fleet);
  const auto cached = fleet_validator(*fleet);
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("DROP INDEX agent_events_poll_idx"));
  }
  auto hit = database->list_event_consumer_statuses_if_current(cached);
  ASSERT_TRUE(hit) << hit.error().to_string();
  EXPECT_TRUE(hit->cache_valid);
  auto stale = cached;
  --stale.event_high_watermark;
  auto forced_full = database->list_event_consumer_statuses_if_current(stale);
  EXPECT_FALSE(forced_full);
}

TEST(AgentDatabase, ConditionalConsumerFleetCacheHitAvoidsLargeBacklogAggregationWork) {
  const auto path = database_path();
  {
    auto initialized =
        atx::agent::AgentDatabase::open(path.string(), "conditional-fleet-performance");
    ASSERT_TRUE(initialized);
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec(
        "BEGIN;WITH RECURSIVE events(value) AS (SELECT 0 UNION ALL SELECT value+1 FROM events "
        "WHERE value<99999) INSERT INTO agent_events(workspace,event_type,subject,payload) SELECT "
        "'conditional-fleet-performance','job.ready','jobs/a','payload' FROM events;"
        "INSERT INTO event_consumers(workspace,name,subject_filter) VALUES("
        "'conditional-fleet-performance','workers','');COMMIT;"));
  }
  auto database = atx::agent::AgentDatabase::open(path.string(), "conditional-fleet-performance");
  ASSERT_TRUE(database);
  auto fleet = database->list_event_consumer_statuses();
  ASSERT_TRUE(fleet);
  ASSERT_EQ(fleet->consumers.size(), 1U);
  EXPECT_EQ(fleet->consumers.front().pending_visible_event_count, 100'000);
  const auto cached = fleet_validator(*fleet);
  auto stale = cached;
  --stale.event_high_watermark;

  constexpr std::size_t hit_iterations = 20;
  const auto hit_started = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < hit_iterations; ++iteration) {
    auto hit = database->list_event_consumer_statuses_if_current(cached);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(hit->cache_valid);
    ASSERT_FALSE(hit->snapshot.has_value());
  }
  const auto hit_elapsed = std::chrono::steady_clock::now() - hit_started;

  constexpr std::size_t miss_iterations = 3;
  const auto miss_started = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < miss_iterations; ++iteration) {
    auto miss = database->list_event_consumer_statuses_if_current(stale);
    ASSERT_TRUE(miss);
    ASSERT_FALSE(miss->cache_valid);
    ASSERT_TRUE(miss->snapshot.has_value());
    ASSERT_EQ(miss->snapshot->consumers.front().pending_visible_event_count, 100'000);
  }
  const auto miss_elapsed = std::chrono::steady_clock::now() - miss_started;

  // The index-drop test proves branch selection deterministically. This broad
  // fixed fixture additionally blocks regressions that perform equivalent
  // event aggregation work before claiming a cache hit.
  const double average_hit_us =
      std::chrono::duration<double, std::micro>{hit_elapsed}.count() / hit_iterations;
  const double average_miss_us =
      std::chrono::duration<double, std::micro>{miss_elapsed}.count() / miss_iterations;
  const double normalized_speedup = average_miss_us / average_hit_us;
  RecordProperty("average_cache_hit_us", average_hit_us);
  RecordProperty("average_forced_full_us", average_miss_us);
  RecordProperty("normalized_speedup", normalized_speedup);
  EXPECT_GT(normalized_speedup, 5.0);
}

TEST(AgentDatabase, ConsumerStateRevisionSeparatesEventsControlsAndDynamicTime) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".consumer-state-revision-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-state-revision");
  ASSERT_TRUE(database);

  auto empty = database->list_event_consumer_statuses();
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty->event_high_watermark, 0);
  EXPECT_EQ(empty->consumer_state_revision, 0);
  EXPECT_TRUE(empty->next_dynamic_transition_at.empty());

  auto registered = database->register_event_consumer("workers", "jobs/a");
  ASSERT_TRUE(registered);
  auto after_register = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_register);
  EXPECT_EQ(after_register->consumer_state_revision, 1);
  EXPECT_GT(after_register->event_high_watermark, 0);
  const auto registration_hwm = after_register->event_high_watermark;
  ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
  auto registration_retry = database->list_event_consumer_statuses();
  ASSERT_TRUE(registration_retry);
  EXPECT_EQ(registration_retry->consumer_state_revision, after_register->consumer_state_revision);
  EXPECT_EQ(registration_retry->event_high_watermark, registration_hwm);

  auto first = database->append_event("job.ready", "first", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(first);
  auto after_event = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_event);
  EXPECT_GT(after_event->event_high_watermark, registration_hwm);
  EXPECT_EQ(after_event->consumer_state_revision, after_register->consumer_state_revision);
  ASSERT_EQ(after_event->consumers.size(), 1U);
  EXPECT_EQ(after_event->consumers.front().pending_visible_event_count, 1);

  auto delivery =
      database->receive_event_consumer("workers", "owner", "revision-receive", 3'600, 1);
  ASSERT_TRUE(delivery);
  auto after_receive = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_receive);
  EXPECT_EQ(after_receive->event_high_watermark, after_event->event_high_watermark);
  EXPECT_GT(after_receive->consumer_state_revision, after_event->consumer_state_revision);
  ASSERT_EQ(after_receive->consumers.size(), 1U);
  EXPECT_EQ(after_receive->consumers.front().delivery_head_state, "in_flight");
  EXPECT_EQ(after_receive->next_dynamic_transition_at,
            after_receive->consumers.front().delivery_head_expires_at);
  EXPECT_EQ(after_receive->consumers.front().next_dynamic_transition_at,
            after_receive->next_dynamic_transition_at);
  auto point = database->get_event_consumer_status("workers");
  ASSERT_TRUE(point);
  EXPECT_EQ(point->consumer_state_revision, after_receive->consumer_state_revision);
  EXPECT_EQ(point->next_dynamic_transition_at, point->delivery_head_expires_at);

  ASSERT_TRUE(database->receive_event_consumer("workers", "owner", "revision-receive", 3'600, 1));
  auto receive_retry = database->list_event_consumer_statuses();
  ASSERT_TRUE(receive_retry);
  EXPECT_EQ(receive_retry->consumer_state_revision, after_receive->consumer_state_revision);
  auto renewed =
      database->renew_event_consumer_delivery("workers", "owner", delivery->delivery_token, 3'600);
  ASSERT_TRUE(renewed);
  auto after_renew = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_renew);
  EXPECT_EQ(after_renew->event_high_watermark, after_receive->event_high_watermark);
  EXPECT_GT(after_renew->consumer_state_revision, after_receive->consumer_state_revision);
  EXPECT_EQ(after_renew->next_dynamic_transition_at, renewed->expires_at);
  auto stale_renew = database->renew_event_consumer_delivery("workers", "other-owner",
                                                             delivery->delivery_token, 3'600);
  ASSERT_FALSE(stale_renew);
  auto after_stale_renew = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_stale_renew);
  EXPECT_EQ(after_stale_renew->consumer_state_revision, after_renew->consumer_state_revision);

  auto settled = database->settle_event_consumer_delivery(
      "workers", "owner", delivery->delivery_token, "revision-settle");
  ASSERT_TRUE(settled);
  auto after_settle = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_settle);
  EXPECT_EQ(after_settle->event_high_watermark, after_renew->event_high_watermark);
  EXPECT_GT(after_settle->consumer_state_revision, after_renew->consumer_state_revision);
  EXPECT_TRUE(after_settle->next_dynamic_transition_at.empty());
  ASSERT_TRUE(database->settle_event_consumer_delivery("workers", "owner", delivery->delivery_token,
                                                       "revision-settle"));
  auto settle_retry = database->list_event_consumer_statuses();
  ASSERT_TRUE(settle_retry);
  EXPECT_EQ(settle_retry->consumer_state_revision, after_settle->consumer_state_revision);

  auto second = database->append_event("job.ready", "second", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(second);
  auto before_checkpoint = database->list_event_consumer_statuses();
  ASSERT_TRUE(before_checkpoint);
  EXPECT_GT(before_checkpoint->event_high_watermark, after_settle->event_high_watermark);
  EXPECT_EQ(before_checkpoint->consumer_state_revision, after_settle->consumer_state_revision);
  auto checkpoint = database->checkpoint_event_consumer("workers", settled->revision, *second,
                                                        "revision-checkpoint");
  ASSERT_TRUE(checkpoint);
  auto after_checkpoint = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_checkpoint);
  EXPECT_EQ(after_checkpoint->event_high_watermark, before_checkpoint->event_high_watermark);
  EXPECT_GT(after_checkpoint->consumer_state_revision, before_checkpoint->consumer_state_revision);
  ASSERT_TRUE(database->checkpoint_event_consumer("workers", settled->revision, *second,
                                                  "revision-checkpoint"));
  auto checkpoint_retry = database->list_event_consumer_statuses();
  ASSERT_TRUE(checkpoint_retry);
  EXPECT_EQ(checkpoint_retry->consumer_state_revision, after_checkpoint->consumer_state_revision);

  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "consumer-state-revision");
  ASSERT_TRUE(restored);
  auto restored_fleet = restored->list_event_consumer_statuses();
  ASSERT_TRUE(restored_fleet);
  EXPECT_EQ(restored_fleet->event_high_watermark, checkpoint_retry->event_high_watermark);
  EXPECT_EQ(restored_fleet->consumer_state_revision, checkpoint_retry->consumer_state_revision);
  ASSERT_TRUE(restored->register_event_consumer("second-worker", "jobs/b"));
  auto restored_changed = restored->list_event_consumer_statuses();
  ASSERT_TRUE(restored_changed);
  EXPECT_GT(restored_changed->consumer_state_revision, restored_fleet->consumer_state_revision);

  auto other = atx::agent::AgentDatabase::open(path.string(), "other-revision-workspace");
  ASSERT_TRUE(other);
  ASSERT_TRUE(other->register_event_consumer("other", "jobs/other"));
  auto other_tail = other->append_event("other.ready", "other", {}, {}, {}, {}, "jobs/other");
  ASSERT_TRUE(other_tail);
  EXPECT_GT(*other_tail, checkpoint_retry->event_high_watermark);
  auto cross_workspace_start =
      database->register_event_consumer("invalid-cross-workspace-start", {}, *other_tail);
  ASSERT_FALSE(cross_workspace_start);
  EXPECT_EQ(cross_workspace_start.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto original_after_other = database->list_event_consumer_statuses();
  ASSERT_TRUE(original_after_other);
  EXPECT_EQ(original_after_other->consumer_state_revision,
            checkpoint_retry->consumer_state_revision);
  EXPECT_TRUE(database->verify_integrity());
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ConsumerStateRevisionAtomicallyFencesLeaseOnlyFleetSnapshots) {
  const auto path = database_path();
  {
    auto initialized = atx::agent::AgentDatabase::open(path.string(), "fleet-revision-race");
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(initialized->register_event_consumer("workers", "jobs/a"));
  }

  for (std::size_t iteration = 0; iteration < 8; ++iteration) {
    auto coordinator = atx::agent::AgentDatabase::open(path.string(), "fleet-revision-race");
    ASSERT_TRUE(coordinator);
    ASSERT_TRUE(coordinator->append_event("job.ready", std::to_string(iteration), {}, {}, {}, {},
                                          "jobs/a"));
    auto before = coordinator->list_event_consumer_statuses();
    ASSERT_TRUE(before);
    ASSERT_EQ(before->consumers.size(), 1U);
    ASSERT_EQ(before->consumers.front().delivery_head_state, "idle");

    std::latch ready{2};
    std::latch start{1};
    std::mutex mutex;
    std::vector<atx::core::ErrorCode> errors;
    std::string delivery_token;
    atx::agent::EventConsumerFleetStatus raced;
    std::vector<std::jthread> workers;
    workers.emplace_back([&] {
      auto writer = atx::agent::AgentDatabase::open(path.string(), "fleet-revision-race");
      if (!writer) {
        std::lock_guard lock{mutex};
        errors.push_back(writer.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto delivery = writer->receive_event_consumer(
          "workers", "owner", "revision-race-" + std::to_string(iteration), 3'600, 1);
      std::lock_guard lock{mutex};
      if (delivery) {
        delivery_token = delivery->delivery_token;
      } else {
        errors.push_back(delivery.error().code());
      }
    });
    workers.emplace_back([&] {
      auto reader = atx::agent::AgentDatabase::open(path.string(), "fleet-revision-race");
      if (!reader) {
        std::lock_guard lock{mutex};
        errors.push_back(reader.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto fleet = reader->list_event_consumer_statuses();
      std::lock_guard lock{mutex};
      if (fleet) {
        raced = std::move(*fleet);
      } else {
        errors.push_back(fleet.error().code());
      }
    });
    ready.wait();
    start.count_down();
    workers.clear();
    ASSERT_TRUE(errors.empty());
    ASSERT_FALSE(delivery_token.empty());
    ASSERT_EQ(raced.consumers.size(), 1U);
    EXPECT_EQ(raced.event_high_watermark, before->event_high_watermark);
    const auto &status = raced.consumers.front();
    if (status.delivery_head_state == "idle") {
      EXPECT_EQ(raced.consumer_state_revision, before->consumer_state_revision);
    } else {
      EXPECT_EQ(status.delivery_head_state, "in_flight");
      EXPECT_GT(raced.consumer_state_revision, before->consumer_state_revision);
    }
    EXPECT_EQ(status.consumer_state_revision, raced.consumer_state_revision);

    auto after = coordinator->list_event_consumer_statuses();
    ASSERT_TRUE(after);
    EXPECT_EQ(after->event_high_watermark, before->event_high_watermark);
    EXPECT_GT(after->consumer_state_revision, before->consumer_state_revision);
    ASSERT_EQ(after->consumers.front().delivery_head_state, "in_flight");
    ASSERT_TRUE(coordinator->settle_event_consumer_delivery(
        "workers", "owner", delivery_token, "revision-race-settle-" + std::to_string(iteration)));
  }
}

TEST(AgentDatabase, ConsumerStateRevisionCoversDlqRedriveAndQuarantinePartitions) {
  auto database = atx::agent::AgentDatabase::open_memory("consumer-state-dlq");
  ASSERT_TRUE(database);
  ASSERT_TRUE(database->register_event_consumer("redrive", "jobs/redrive"));
  ASSERT_TRUE(database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/redrive"));
  auto delivery = database->receive_event_consumer("redrive", "owner", "dlq-receive", 3'600, 1);
  ASSERT_TRUE(delivery);
  auto before_reject = database->list_event_consumer_statuses();
  ASSERT_TRUE(before_reject);
  auto rejected = database->reject_event_consumer_delivery(
      "redrive", "owner", delivery->delivery_token, "dlq-reject", "poison",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(rejected);
  auto after_reject = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_reject);
  EXPECT_GT(after_reject->consumer_state_revision, before_reject->consumer_state_revision);
  EXPECT_GT(after_reject->event_high_watermark, before_reject->event_high_watermark);
  ASSERT_TRUE(database->reject_event_consumer_delivery(
      "redrive", "owner", delivery->delivery_token, "dlq-reject", "poison",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter));
  auto reject_retry = database->list_event_consumer_statuses();
  ASSERT_TRUE(reject_retry);
  EXPECT_EQ(reject_retry->consumer_state_revision, after_reject->consumer_state_revision);
  EXPECT_EQ(reject_retry->event_high_watermark, after_reject->event_high_watermark);

  auto redriven = database->redrive_event_consumer_dead_letter("redrive", rejected->dead_letter_id,
                                                               "dlq-redrive");
  ASSERT_TRUE(redriven);
  auto after_redrive = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_redrive);
  EXPECT_GT(after_redrive->consumer_state_revision, after_reject->consumer_state_revision);
  EXPECT_GT(after_redrive->event_high_watermark, after_reject->event_high_watermark);
  ASSERT_TRUE(database->redrive_event_consumer_dead_letter("redrive", rejected->dead_letter_id,
                                                           "dlq-redrive"));
  auto redrive_retry = database->list_event_consumer_statuses();
  ASSERT_TRUE(redrive_retry);
  EXPECT_EQ(redrive_retry->consumer_state_revision, after_redrive->consumer_state_revision);
  EXPECT_EQ(redrive_retry->event_high_watermark, after_redrive->event_high_watermark);

  ASSERT_TRUE(database->register_event_consumer("quarantine", "jobs/quarantine"));
  ASSERT_TRUE(database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/quarantine"));
  auto quarantine_delivery =
      database->receive_event_consumer("quarantine", "operator", "quarantine-receive", 3'600, 1);
  ASSERT_TRUE(quarantine_delivery);
  auto quarantine_rejection = database->reject_event_consumer_delivery(
      "quarantine", "operator", quarantine_delivery->delivery_token, "quarantine-reject", "poison",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(quarantine_rejection);
  auto before_quarantine = database->list_event_consumer_statuses();
  ASSERT_TRUE(before_quarantine);
  auto quarantined = database->quarantine_event_consumer_dead_letter(
      "quarantine", quarantine_rejection->dead_letter_id, "operator", "quarantine-token",
      "reviewed");
  ASSERT_TRUE(quarantined);
  auto after_quarantine = database->list_event_consumer_statuses();
  ASSERT_TRUE(after_quarantine);
  EXPECT_GT(after_quarantine->consumer_state_revision, before_quarantine->consumer_state_revision);
  EXPECT_GT(after_quarantine->event_high_watermark, before_quarantine->event_high_watermark);
  ASSERT_TRUE(database->quarantine_event_consumer_dead_letter(
      "quarantine", quarantine_rejection->dead_letter_id, "operator", "quarantine-token",
      "reviewed"));
  auto quarantine_retry = database->list_event_consumer_statuses();
  ASSERT_TRUE(quarantine_retry);
  EXPECT_EQ(quarantine_retry->consumer_state_revision, after_quarantine->consumer_state_revision);
  EXPECT_EQ(quarantine_retry->event_high_watermark, after_quarantine->event_high_watermark);
  const auto quarantine_status =
      std::ranges::find_if(quarantine_retry->consumers,
                           [](const auto &status) { return status.consumer.name == "quarantine"; });
  ASSERT_NE(quarantine_status, quarantine_retry->consumers.end());
  EXPECT_EQ(quarantine_status->open_dead_letter_count, 0);
  EXPECT_EQ(quarantine_status->quarantined_dead_letter_count, 1);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, SchemaTwentyTwoMigrationBaselinesAndRestoresConsumerStateRevision) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".state-revision-migration-backup";
  std::error_code cleanup_error;
  std::filesystem::remove(backup_path, cleanup_error);
  std::filesystem::remove(backup_path.string() + "-wal", cleanup_error);
  std::filesystem::remove(backup_path.string() + "-shm", cleanup_error);
  std::string delivery_token;
  std::int64_t historical_hwm{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "state-revision-migration");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
    ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
    auto delivery =
        database->receive_event_consumer("workers", "owner", "migration-receive", 3'600, 1);
    ASSERT_TRUE(delivery);
    delivery_token = delivery->delivery_token;
    auto fleet = database->list_event_consumer_statuses();
    ASSERT_TRUE(fleet);
    historical_hwm = fleet->event_high_watermark;
    EXPECT_GT(fleet->consumer_state_revision, 1);
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("DROP TRIGGER event_consumers_state_revision_insert;"
                          "DROP TRIGGER event_consumers_state_revision_update;"
                          "DROP TRIGGER event_consumers_state_revision_delete;"
                          "DROP TRIGGER event_consumer_dead_letters_state_revision_insert;"
                          "DROP TRIGGER event_consumer_dead_letters_state_revision_update;"
                          "DROP TRIGGER event_consumer_dead_letters_state_revision_delete;"
                          "DROP TRIGGER event_consumer_quarantines_state_revision_insert;"
                          "DROP TRIGGER event_consumer_quarantines_state_revision_update;"
                          "DROP TRIGGER event_consumer_quarantines_state_revision_delete;"
                          "DROP TABLE event_consumer_state_revisions;"
                          "UPDATE agent_db_meta SET value='22' WHERE key='schema_version'"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "state-revision-migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto fleet = migrated->list_event_consumer_statuses();
  ASSERT_TRUE(fleet);
  EXPECT_EQ(fleet->event_high_watermark, historical_hwm);
  EXPECT_EQ(fleet->consumer_state_revision, 1);
  ASSERT_EQ(fleet->consumers.size(), 1U);
  EXPECT_EQ(fleet->consumers.front().delivery_head_state, "in_flight");
  EXPECT_FALSE(fleet->next_dynamic_transition_at.empty());
  const auto migrated_validator = fleet_validator(*fleet);
  auto migrated_hit = migrated->list_event_consumer_statuses_if_current(migrated_validator);
  ASSERT_TRUE(migrated_hit);
  EXPECT_TRUE(migrated_hit->cache_valid);
  ASSERT_TRUE(migrated->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "state-revision-migration");
  ASSERT_TRUE(restored);
  auto restored_fleet = restored->list_event_consumer_statuses();
  ASSERT_TRUE(restored_fleet);
  EXPECT_EQ(restored_fleet->consumer_state_revision, fleet->consumer_state_revision);
  EXPECT_EQ(restored_fleet->event_high_watermark, fleet->event_high_watermark);
  auto restored_hit = restored->list_event_consumer_statuses_if_current(migrated_validator);
  ASSERT_TRUE(restored_hit);
  EXPECT_TRUE(restored_hit->cache_valid);
  ASSERT_TRUE(restored->settle_event_consumer_delivery("workers", "owner", delivery_token,
                                                       "migration-restored-settle"));
  auto restored_changed = restored->list_event_consumer_statuses();
  ASSERT_TRUE(restored_changed);
  EXPECT_EQ(restored_changed->event_high_watermark, restored_fleet->event_high_watermark);
  EXPECT_GT(restored_changed->consumer_state_revision, restored_fleet->consumer_state_revision);
  auto restored_miss = restored->list_event_consumer_statuses_if_current(migrated_validator);
  ASSERT_TRUE(restored_miss);
  EXPECT_FALSE(restored_miss->cache_valid);
  ASSERT_TRUE(restored_miss->snapshot.has_value());
  EXPECT_TRUE(migrated->verify_integrity());
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, ConsumerStateRevisionOverflowRollsBackTheSourceMutation) {
  const auto path = database_path();
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "state-revision-overflow");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
    ASSERT_TRUE(database->append_event("job.ready", "work", {}, {}, {}, {}, "jobs/a"));
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(
        raw->exec("UPDATE event_consumer_state_revisions SET revision=9223372036854775807 WHERE "
                  "workspace='state-revision-overflow'"));
  }
  auto database = atx::agent::AgentDatabase::open(path.string(), "state-revision-overflow");
  ASSERT_TRUE(database);
  auto before = database->list_event_consumer_statuses();
  ASSERT_TRUE(before);
  ASSERT_EQ(before->consumer_state_revision, std::numeric_limits<std::int64_t>::max());
  auto failed = database->receive_event_consumer("workers", "owner", "overflow-receive", 3'600, 1);
  ASSERT_FALSE(failed);
  auto after = database->list_event_consumer_statuses();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->consumer_state_revision, before->consumer_state_revision);
  EXPECT_EQ(after->event_high_watermark, before->event_high_watermark);
  ASSERT_EQ(after->consumers.size(), 1U);
  EXPECT_EQ(after->consumers.front().delivery_head_state, "idle");
  EXPECT_EQ(after->consumers.front().pending_visible_event_count, 1);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, ConsumerStatusRejectsAMissingDurableStateRevision) {
  const auto path = database_path();
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "missing-state-revision");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("DELETE FROM event_consumer_state_revisions WHERE "
                          "workspace='missing-state-revision'"));
  }
  auto database = atx::agent::AgentDatabase::open(path.string(), "missing-state-revision");
  ASSERT_TRUE(database);
  auto point = database->get_event_consumer_status("workers");
  ASSERT_FALSE(point);
  EXPECT_EQ(point.error().code(), atx::core::ErrorCode::Internal);
  auto fleet = database->list_event_consumer_statuses();
  ASSERT_FALSE(fleet);
  EXPECT_EQ(fleet.error().code(), atx::core::ErrorCode::Internal);
  atx::agent::EventConsumerFleetValidator cached{"missing-state-revision", 0, 0, {}};
  auto conditional = database->list_event_consumer_statuses_if_current(cached);
  ASSERT_FALSE(conditional);
  EXPECT_EQ(conditional.error().code(), atx::core::ErrorCode::Internal);
  EXPECT_FALSE(database->verify_integrity());
}

TEST(AgentDatabase, SchemaTwentyOneMigrationPreservesHistoricalSelfControlVisibility) {
  const auto path = database_path();
  std::int64_t historical_high_watermark{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "self-control-migration");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->register_event_consumer("workers"));
    auto events = database->events_after(0, 100, "consumers/workers");
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 1U);
    historical_high_watermark = events->front().sequence;
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE event_consumers SET self_control_event_cutoff_sequence=" +
                          std::to_string(historical_high_watermark) +
                          " WHERE workspace='self-control-migration' AND name='workers'"));
  }
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "self-control-migration");
    ASSERT_TRUE(database);
    auto checkpoint = database->checkpoint_event_consumer("workers", 1, historical_high_watermark,
                                                          "historical-self-control");
    ASSERT_TRUE(checkpoint) << checkpoint.error().to_string();
    EXPECT_EQ(checkpoint->cursor_sequence, historical_high_watermark);
    EXPECT_TRUE(database->verify_integrity());
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("UPDATE event_consumers SET self_control_event_cutoff_sequence=0;"
                          "UPDATE agent_db_meta SET value='21' WHERE key='schema_version'"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "self-control-migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto consumer = migrated->get_event_consumer("workers");
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->self_control_event_cutoff_sequence, historical_high_watermark);
  EXPECT_EQ(consumer->cursor_sequence, historical_high_watermark);
  auto events = migrated->events_after(0, 100);
  ASSERT_TRUE(events);
  ASSERT_FALSE(events->empty());
  EXPECT_EQ(events->back().sequence, historical_high_watermark);
  auto work = migrated->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(work);
  auto delivery = migrated->receive_event_consumer("workers", "worker", "receive", 30, 1);
  ASSERT_TRUE(delivery);
  ASSERT_EQ(delivery->events.size(), 1U);
  EXPECT_EQ(delivery->events.front().sequence, *work);
  auto rejected = migrated->reject_event_consumer_delivery(
      "workers", "worker", delivery->delivery_token, "reject", "poison",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(rejected);
  auto empty = migrated->receive_event_consumer("workers", "worker", "empty", 30, 10);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->events.empty());
  auto integrity = migrated->verify_integrity();
  EXPECT_TRUE(integrity) << (integrity ? "" : integrity.error().to_string());
}

TEST(AgentDatabase, SchemaTwentyMigrationBackfillsWithoutPublishingThenEmitsNewTransition) {
  const auto path = database_path();
  std::int64_t dead_letter_id{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "lifecycle-migration");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
    auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    auto dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker", delivery->delivery_token, "reject", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(dead_letter);
    dead_letter_id = dead_letter->dead_letter_id;
  }
  std::int64_t historical_high_watermark{};
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("DROP TABLE event_consumer_dead_letter_lifecycle_events;"
                          "DROP TABLE event_consumer_lifecycle_epochs;"
                          "DELETE FROM agent_events WHERE event_type IN ('consumer.dead_lettered',"
                          "'consumer.dead_letter_redriven','consumer.dead_letter_quarantined');"
                          "UPDATE agent_db_meta SET value='20' WHERE key='schema_version'"));
    auto high_watermark = raw->prepare("SELECT COALESCE(max(sequence),0) FROM agent_events");
    ASSERT_TRUE(high_watermark);
    auto high_watermark_step = high_watermark->step();
    ASSERT_TRUE(high_watermark_step);
    ASSERT_EQ(*high_watermark_step, atx::core::db::Statement::Step::Row);
    historical_high_watermark = high_watermark->column_int(0);
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "lifecycle-migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto events = migrated->events_after(0, 100);
  ASSERT_TRUE(events);
  ASSERT_FALSE(events->empty());
  EXPECT_EQ(events->back().sequence, historical_high_watermark);
  auto listed = migrated->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 1U);
  EXPECT_EQ(listed->front().dead_lettered_event_sequence, 0);
  EXPECT_EQ(listed->front().redriven_event_sequence, 0);
  EXPECT_EQ(listed->front().quarantined_event_sequence, 0);
  auto quarantined = migrated->quarantine_event_consumer_dead_letter(
      "workers", dead_letter_id, "operator", "quarantine", "reviewed historical poison");
  ASSERT_TRUE(quarantined) << quarantined.error().to_string();
  EXPECT_EQ(quarantined->dead_lettered_event_sequence, 0);
  EXPECT_GT(quarantined->quarantined_event_sequence, historical_high_watermark);
  auto exact = migrated->quarantine_event_consumer_dead_letter(
      "workers", dead_letter_id, "operator", "quarantine", "reviewed historical poison");
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->quarantined_event_sequence, quarantined->quarantined_event_sequence);
  auto lifecycle = migrated->events_after(historical_high_watermark, 100, "consumers/workers");
  ASSERT_TRUE(lifecycle);
  ASSERT_EQ(lifecycle->size(), 1U);
  EXPECT_EQ(lifecycle->front().type, "consumer.dead_letter_quarantined");
  EXPECT_EQ(lifecycle->front().payload, std::to_string(dead_letter_id));
  EXPECT_TRUE(migrated->verify_integrity());
}

TEST(AgentDatabase, SchemaNineteenMigrationAddsEmptyQuarantineAudit) {
  const auto path = database_path();
  std::int64_t dead_letter_id{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "quarantine-migration");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
    auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    auto dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker", delivery->delivery_token, "reject", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(dead_letter);
    dead_letter_id = dead_letter->dead_letter_id;
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("DROP TABLE event_consumer_dead_letter_quarantines;"
                          "UPDATE agent_db_meta SET value='19' WHERE key='schema_version'"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "quarantine-migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto listed = migrated->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 1U);
  EXPECT_EQ(listed->front().status, "open");
  auto quarantined = migrated->quarantine_event_consumer_dead_letter(
      "workers", dead_letter_id, "operator", "quarantine", "reviewed");
  ASSERT_TRUE(quarantined);
  EXPECT_EQ(quarantined->status, "quarantined");
  EXPECT_TRUE(migrated->verify_integrity());
}

TEST(AgentDatabase, IntegrityRejectsCrossConsumerQuarantineAttachment) {
  const auto path = database_path();
  std::int64_t dead_letter_id{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "quarantine-integrity");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("worker-a", "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("worker-b", "jobs/b"));
    auto delivery = database->receive_event_consumer("worker-a", "worker", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    auto dead_letter = database->reject_event_consumer_delivery(
        "worker-a", "worker", delivery->delivery_token, "reject", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(dead_letter);
    dead_letter_id = dead_letter->dead_letter_id;
    EXPECT_TRUE(database->verify_integrity());
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec(
        "INSERT INTO event_consumer_dead_letter_quarantines(workspace,consumer_name,"
        "dead_letter_id,quarantine_token,quarantined_by,reason) VALUES("
        "'quarantine-integrity','worker-b'," +
        std::to_string(dead_letter_id) + ",'cross-attach','operator','invalid attachment')"));
  }
  auto corrupted = atx::agent::AgentDatabase::open(path.string(), "quarantine-integrity");
  ASSERT_TRUE(corrupted);
  auto verified = corrupted->verify_integrity();
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, IntegrityRejectsMissingDeadLetterLifecycleOccurrence) {
  const auto path = database_path();
  std::int64_t dead_letter_id{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "lifecycle-integrity");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
    auto delivery = database->receive_event_consumer("workers", "worker", "receive", 30, 1);
    ASSERT_TRUE(delivery);
    auto dead_letter = database->reject_event_consumer_delivery(
        "workers", "worker", delivery->delivery_token, "reject", "poison",
        atx::agent::EventConsumerRejectionDisposition::DeadLetter);
    ASSERT_TRUE(dead_letter);
    dead_letter_id = dead_letter->dead_letter_id;
    EXPECT_TRUE(database->verify_integrity());
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec("DELETE FROM event_consumer_dead_letter_lifecycle_events WHERE workspace="
                          "'lifecycle-integrity' AND consumer_name='workers' AND dead_letter_id=" +
                          std::to_string(dead_letter_id) + " AND transition='dead_lettered'"));
  }
  auto corrupted = atx::agent::AgentDatabase::open(path.string(), "lifecycle-integrity");
  ASSERT_TRUE(corrupted);
  auto verified = corrupted->verify_integrity();
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, IntegrityRejectsCheckpointOfPostCutoffSelfControlEvent) {
  const auto path = database_path();
  std::int64_t registration_sequence{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "self-control-integrity");
    ASSERT_TRUE(database);
    auto consumer = database->register_event_consumer("workers");
    ASSERT_TRUE(consumer);
    EXPECT_EQ(consumer->self_control_event_cutoff_sequence, 0);
    auto events = database->events_after(0, 100, "consumers/workers");
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 1U);
    registration_sequence = events->front().sequence;
    EXPECT_TRUE(database->verify_integrity());
  }
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec(
        "UPDATE event_consumers SET cursor_sequence=" + std::to_string(registration_sequence) +
        ",revision=2 WHERE workspace='self-control-integrity' AND name='workers';"
        "INSERT INTO event_consumer_checkpoints(workspace,consumer_name,checkpoint_token,"
        "request_revision,previous_sequence,through_sequence,result_revision) VALUES("
        "'self-control-integrity','workers','forged-self-control',1,0," +
        std::to_string(registration_sequence) + ",2)"));
  }
  auto corrupted = atx::agent::AgentDatabase::open(path.string(), "self-control-integrity");
  ASSERT_TRUE(corrupted);
  auto verified = corrupted->verify_integrity();
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, LifecycleControlRedriveIsLineageReplayNotSecondLifecycleOrigin) {
  const auto path = database_path();
  auto backup_path = path;
  backup_path += ".lifecycle-replay-backup";
  std::error_code ignored;
  std::filesystem::remove(backup_path, ignored);
  auto database = atx::agent::AgentDatabase::open(path.string(), "lifecycle-replay");
  ASSERT_TRUE(database);
  auto work = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
  ASSERT_TRUE(work);
  ASSERT_TRUE(database->register_event_consumer("target", "jobs/a"));
  auto target_registration = database->events_after(0, 100, "consumers/target");
  ASSERT_TRUE(target_registration);
  ASSERT_EQ(target_registration->size(), 1U);
  ASSERT_TRUE(database->register_event_consumer("monitor", "consumers/target",
                                                target_registration->front().sequence));
  auto target_delivery =
      database->receive_event_consumer("target", "worker", "target-receive", 30, 1);
  ASSERT_TRUE(target_delivery);
  auto target_rejection = database->reject_event_consumer_delivery(
      "target", "worker", target_delivery->delivery_token, "target-reject", "poison",
      atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(target_rejection);
  auto target_dead_letters = database->list_event_consumer_dead_letters("target");
  ASSERT_TRUE(target_dead_letters);
  ASSERT_EQ(target_dead_letters->size(), 1U);
  const auto lifecycle_origin = target_dead_letters->front().dead_lettered_event_sequence;
  EXPECT_GT(lifecycle_origin, *work);
  auto control_delivery =
      database->receive_event_consumer("monitor", "monitor-worker", "control-receive", 30, 1);
  ASSERT_TRUE(control_delivery);
  ASSERT_EQ(control_delivery->events.size(), 1U);
  const auto &original_control = control_delivery->events.front();
  EXPECT_EQ(original_control.sequence, lifecycle_origin);
  EXPECT_EQ(original_control.type, "consumer.dead_lettered");
  EXPECT_EQ(original_control.subject, "consumers/target");
  EXPECT_EQ(original_control.redrive_count, 0);
  auto monitor_rejection = database->reject_event_consumer_delivery(
      "monitor", "monitor-worker", control_delivery->delivery_token, "monitor-reject",
      "control handler unavailable", atx::agent::EventConsumerRejectionDisposition::DeadLetter);
  ASSERT_TRUE(monitor_rejection);
  auto redriven = database->redrive_event_consumer_dead_letter(
      "monitor", monitor_rejection->dead_letter_id, "monitor-redrive");
  ASSERT_TRUE(redriven) << redriven.error().to_string();
  ASSERT_EQ(redriven->redriven_events.size(), 1U);
  const auto &replay = redriven->redriven_events.front();
  EXPECT_EQ(replay.type, original_control.type);
  EXPECT_EQ(replay.subject, original_control.subject);
  EXPECT_EQ(replay.payload, original_control.payload);
  EXPECT_EQ(replay.root_sequence, lifecycle_origin);
  EXPECT_EQ(replay.redrive_count, 1);
  EXPECT_GT(replay.sequence, lifecycle_origin);
  auto exact = database->redrive_event_consumer_dead_letter(
      "monitor", monitor_rejection->dead_letter_id, "monitor-redrive");
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->redriven_events.front().sequence, replay.sequence);
  auto replay_delivery =
      database->receive_event_consumer("monitor", "monitor-worker", "replay-receive", 30, 1);
  ASSERT_TRUE(replay_delivery);
  ASSERT_EQ(replay_delivery->events.size(), 1U);
  EXPECT_EQ(replay_delivery->events.front().sequence, replay.sequence);
  auto still_one_origin = database->list_event_consumer_dead_letters("target");
  ASSERT_TRUE(still_one_origin);
  ASSERT_EQ(still_one_origin->size(), 1U);
  EXPECT_EQ(still_one_origin->front().dead_lettered_event_sequence, lifecycle_origin);
  EXPECT_TRUE(database->verify_integrity());
  ASSERT_TRUE(database->backup_to(backup_path.string()));
  auto restored = atx::agent::AgentDatabase::open(backup_path.string(), "lifecycle-replay");
  ASSERT_TRUE(restored);
  auto restored_exact = restored->redrive_event_consumer_dead_letter(
      "monitor", monitor_rejection->dead_letter_id, "monitor-redrive");
  ASSERT_TRUE(restored_exact);
  EXPECT_EQ(restored_exact->redriven_events.front().sequence, replay.sequence);
  EXPECT_TRUE(restored->verify_integrity());
}

TEST(AgentDatabase, IntegrityRejectsForgedGenerationZeroLifecycleOrigin) {
  auto database = atx::agent::AgentDatabase::open_memory("forged-lifecycle-origin");
  ASSERT_TRUE(database);
  auto forged =
      database->append_event("consumer.dead_lettered", "1", {}, {}, {}, {}, "consumers/forged");
  ASSERT_TRUE(forged);
  auto verified = database->verify_integrity();
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, CompetingReceiversDeadLetterFinalAttemptExactlyOnce) {
  const auto path = database_path();
  std::int64_t poison_sequence{};
  std::int64_t next_sequence{};
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-dead-letter-race");
    ASSERT_TRUE(database);
    auto poison = database->append_event("job.ready", "poison", {}, {}, {}, {}, "jobs/a");
    auto next = database->append_event("job.ready", "healthy", {}, {}, {}, {}, "jobs/a");
    ASSERT_TRUE(poison);
    ASSERT_TRUE(next);
    poison_sequence = *poison;
    next_sequence = *next;
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a", 0, 1));
    ASSERT_TRUE(database->receive_event_consumer("workers", "initial", "initial-receive", 1, 1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{1'100});

  constexpr std::size_t worker_count = 6;
  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  struct Winner {
    std::string owner;
    std::string delivery_token;
    std::int64_t dead_lettered_batches{};
    std::int64_t sequence{};
  };
  std::vector<Winner> winners;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "consumer-dead-letter-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      const std::string owner = "worker-" + std::to_string(worker);
      auto delivery = connection->receive_event_consumer(
          "workers", owner, "terminal-receive-" + std::to_string(worker), 30, 1);
      std::lock_guard lock{mutex};
      if (delivery) {
        winners.push_back({owner, delivery->delivery_token, delivery->dead_lettered_batches,
                           delivery->events.empty() ? 0 : delivery->events.front().sequence});
      } else {
        errors.push_back(delivery.error().code());
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();
  ASSERT_EQ(winners.size(), 1U);
  ASSERT_EQ(errors.size(), worker_count - 1);
  EXPECT_TRUE(std::all_of(errors.begin(), errors.end(), [](const auto error) {
    return error == atx::core::ErrorCode::Unavailable;
  }));
  EXPECT_EQ(winners.front().dead_lettered_batches, 1);
  EXPECT_EQ(winners.front().sequence, next_sequence);

  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-dead-letter-race");
  ASSERT_TRUE(database);
  auto dead_letters = database->list_event_consumer_dead_letters("workers");
  ASSERT_TRUE(dead_letters);
  ASSERT_EQ(dead_letters->size(), 1U);
  EXPECT_EQ(dead_letters->front().events.front().sequence, poison_sequence);
  auto consumer = database->get_event_consumer("workers");
  ASSERT_TRUE(consumer);
  EXPECT_EQ(consumer->cursor_sequence, poison_sequence);
  EXPECT_EQ(consumer->revision, 2);
  auto settled = database->settle_event_consumer_delivery(
      "workers", winners.front().owner, winners.front().delivery_token, "race-healthy-settle");
  ASSERT_TRUE(settled);
  EXPECT_EQ(settled->cursor_sequence, next_sequence);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, CompetingConsumerReceivesHaveOneLeaseWinner) {
  const auto path = database_path();
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-receive-race");
    ASSERT_TRUE(database);
    ASSERT_TRUE(database->append_event("job.ready", "one", {}, {}, {}, {}, "jobs/a"));
    ASSERT_TRUE(database->register_event_consumer("workers", "jobs/a"));
  }

  constexpr std::size_t worker_count = 8;
  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  std::vector<std::pair<std::string, std::string>> winners;
  std::vector<atx::core::ErrorCode> errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "consumer-receive-race");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().code());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      const std::string owner = "worker-" + std::to_string(worker);
      const std::string request = "receive-" + std::to_string(worker);
      auto delivery = connection->receive_event_consumer("workers", owner, request, 30, 1);
      std::lock_guard lock{mutex};
      if (delivery) {
        winners.emplace_back(owner, delivery->delivery_token);
      } else {
        errors.push_back(delivery.error().code());
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();
  ASSERT_EQ(winners.size(), 1U);
  ASSERT_EQ(errors.size(), worker_count - 1);
  EXPECT_TRUE(std::all_of(errors.begin(), errors.end(), [](const auto error) {
    return error == atx::core::ErrorCode::Unavailable;
  }));

  auto database = atx::agent::AgentDatabase::open(path.string(), "consumer-receive-race");
  ASSERT_TRUE(database);
  auto settled = database->settle_event_consumer_delivery("workers", winners.front().first,
                                                          winners.front().second, "race-settle");
  ASSERT_TRUE(settled);
  EXPECT_EQ(settled->revision, 2);
  EXPECT_TRUE(database->verify_integrity());
}

TEST(AgentDatabase, EpisodeEvidenceLinksVerifyAgainstExactKnowledgeObservation) {
  auto coordination = atx::agent::AgentDatabase::open_memory();
  auto knowledge = atx::kb::KnowledgeBase::open_memory();
  ASSERT_TRUE(coordination);
  ASSERT_TRUE(knowledge);
  const auto [run_id, agent_id] = initialize_run(*coordination);
  atx::kb::Submission source;
  source.title = "Verified episode evidence";
  source.raw_text = "The agent produced this exact evidence during its task.";
  source.submitted_by = agent_id;
  auto submitted = knowledge->submit(source);
  ASSERT_TRUE(submitted);
  atx::agent::EpisodeInput episode;
  episode.idempotency_key = "verified-episode";
  episode.run_id = run_id;
  episode.agent_id = agent_id;
  episode.source_id = submitted->source_id;
  episode.observation_id = submitted->observation_id;
  ASSERT_TRUE(coordination->record_episode(episode));
  EXPECT_TRUE(coordination->verify_evidence_links(*knowledge));

  auto wrong = episode;
  wrong.idempotency_key = "missing-observation";
  wrong.observation_id += 99;
  ASSERT_TRUE(coordination->record_episode(wrong));
  auto invalid = coordination->verify_evidence_links(*knowledge);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code(), atx::core::ErrorCode::Internal);
}

TEST(AgentDatabase, VerifiedEpisodesCertifyExactObservationAndUpgradeUnverifiedRetries) {
  auto coordination = atx::agent::AgentDatabase::open_memory();
  auto knowledge = atx::kb::KnowledgeBase::open_memory();
  ASSERT_TRUE(coordination);
  ASSERT_TRUE(knowledge);
  const auto [run_id, agent_id] = initialize_run(*coordination);
  auto submitted = knowledge->submit(
      research("Certified episode evidence",
               "This immutable content hash and exact observation certify an agent episode."));
  ASSERT_TRUE(submitted);
  atx::agent::EpisodeInput episode;
  episode.idempotency_key = "certified-episode";
  episode.run_id = run_id;
  episode.agent_id = agent_id;
  episode.source_id = submitted->source_id;
  episode.observation_id = submitted->observation_id;
  auto certified = coordination->record_verified_episode(episode, *knowledge);
  ASSERT_TRUE(certified) << certified.error().to_string();
  EXPECT_EQ(certified->evidence_status, "verified");
  EXPECT_EQ(certified->evidence_content_hash, submitted->content_hash);
  EXPECT_FALSE(certified->evidence_verified_at.empty());
  auto repeated = coordination->record_verified_episode(episode, *knowledge);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->id, certified->id);

  auto upgrade = episode;
  upgrade.idempotency_key = "upgrade-unverified-episode";
  auto unverified = coordination->record_episode(upgrade);
  ASSERT_TRUE(unverified);
  EXPECT_EQ(unverified->evidence_status, "unverified");
  auto upgraded = coordination->record_verified_episode(upgrade, *knowledge);
  ASSERT_TRUE(upgraded);
  EXPECT_EQ(upgraded->id, unverified->id);
  EXPECT_EQ(upgraded->evidence_status, "verified");
  EXPECT_EQ(upgraded->evidence_content_hash, submitted->content_hash);

  auto wrong = episode;
  wrong.idempotency_key = "wrong-certified-observation";
  wrong.observation_id += 1;
  auto rejected = coordination->record_verified_episode(wrong, *knowledge);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_TRUE(coordination->verify_evidence_links(*knowledge));
  EXPECT_TRUE(coordination->verify_integrity());
}

TEST(AgentDatabase, BitemporalFactsPreserveWhatWasKnownWhen) {
  auto opened = atx::agent::AgentDatabase::open_memory("temporal");
  ASSERT_TRUE(opened);
  atx::agent::FactInput first;
  first.subject = "atx-kb";
  first.predicate = "release_state";
  first.object = "experimental";
  first.valid_from = "2026-07-01T00:00:00.000Z";
  first.evidence_source_id = "src_old";
  auto old_fact = opened->put_fact(first);
  ASSERT_TRUE(old_fact) << old_fact.error().to_string();
  auto replacement = first;
  replacement.object = "production-candidate";
  replacement.valid_from = "2026-07-18T00:00:00.000Z";
  replacement.evidence_source_id = "src_new";
  auto new_fact = opened->put_fact(replacement);
  ASSERT_TRUE(new_fact) << new_fact.error().to_string();
  EXPECT_EQ(new_fact->supersedes_fact_id, old_fact->id);
  EXPECT_LT(old_fact->transaction_from_sequence, new_fact->transaction_from_sequence);

  auto historically_known = opened->facts_as_of_sequence(
      "2026-07-10T00:00:00.000Z", old_fact->transaction_from_sequence, "atx-kb", "release_state");
  ASSERT_TRUE(historically_known);
  ASSERT_EQ(historically_known->size(), 1U);
  EXPECT_EQ(historically_known->front().object, "experimental");
  auto currently_known = opened->facts_as_of("2026-07-19T00:00:00.000Z", new_fact->transaction_from,
                                             "atx-kb", "release_state");
  ASSERT_TRUE(currently_known);
  ASSERT_EQ(currently_known->size(), 1U);
  EXPECT_EQ(currently_known->front().object, "production-candidate");
  auto current_history = opened->facts_as_of_sequence(
      "2026-07-10T00:00:00.000Z", new_fact->transaction_from_sequence, "atx-kb", "release_state");
  ASSERT_TRUE(current_history);
  ASSERT_EQ(current_history->size(), 1U);
  EXPECT_EQ(current_history->front().object, "experimental");
  EXPECT_TRUE(opened->verify_integrity());
}

TEST(AgentDatabase, VerifiedFactsCertifyExactObservationThroughIntervalSplits) {
  auto coordination = atx::agent::AgentDatabase::open_memory("certified-temporal");
  auto knowledge = atx::kb::KnowledgeBase::open_memory();
  ASSERT_TRUE(coordination);
  ASSERT_TRUE(knowledge);
  auto original_evidence = knowledge->submit(
      research("Original temporal fact", "The system was experimental beginning July 1."));
  auto replacement_evidence = knowledge->submit(
      research("Replacement temporal fact", "The system became a production candidate July 18."));
  ASSERT_TRUE(original_evidence);
  ASSERT_TRUE(replacement_evidence);

  atx::agent::FactInput original;
  original.subject = "atx-kb";
  original.predicate = "release_state";
  original.object = "experimental";
  original.valid_from = "2026-07-01T00:00:00.000Z";
  original.evidence_source_id = original_evidence->source_id;
  original.idempotency_key = "certified-original-fact";
  auto certified_original =
      coordination->put_verified_fact(original, original_evidence->observation_id, *knowledge);
  ASSERT_TRUE(certified_original) << certified_original.error().to_string();
  EXPECT_EQ(certified_original->evidence_status, "verified");
  EXPECT_EQ(certified_original->evidence_observation_id, original_evidence->observation_id);
  EXPECT_EQ(certified_original->evidence_content_hash, original_evidence->content_hash);
  EXPECT_EQ(certified_original->idempotency_key, original.idempotency_key);
  EXPECT_EQ(certified_original->request_valid_from, original.valid_from);
  auto repeated_original =
      coordination->put_verified_fact(original, original_evidence->observation_id, *knowledge);
  ASSERT_TRUE(repeated_original);
  EXPECT_EQ(repeated_original->id, certified_original->id);
  EXPECT_EQ(repeated_original->transaction_from_sequence,
            certified_original->transaction_from_sequence);
  auto conflicting_original = original;
  conflicting_original.object = "conflicting-retry";
  auto conflict = coordination->put_verified_fact(conflicting_original,
                                                  original_evidence->observation_id, *knowledge);
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto replacement = original;
  replacement.object = "production-candidate";
  replacement.valid_from = "2026-07-18T00:00:00.000Z";
  replacement.evidence_source_id = replacement_evidence->source_id;
  replacement.idempotency_key = "certified-replacement-fact";
  auto wrong = coordination->put_verified_fact(
      replacement, replacement_evidence->observation_id + 1, *knowledge);
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto certified_replacement = coordination->put_verified_fact(
      replacement, replacement_evidence->observation_id, *knowledge);
  ASSERT_TRUE(certified_replacement) << certified_replacement.error().to_string();
  EXPECT_EQ(certified_replacement->evidence_status, "verified");
  EXPECT_EQ(certified_replacement->evidence_content_hash, replacement_evidence->content_hash);
  auto repeated_replacement = coordination->put_verified_fact(
      replacement, replacement_evidence->observation_id, *knowledge);
  ASSERT_TRUE(repeated_replacement);
  EXPECT_EQ(repeated_replacement->id, certified_replacement->id);
  auto replayed_original_after_split =
      coordination->put_verified_fact(original, original_evidence->observation_id, *knowledge);
  ASSERT_TRUE(replayed_original_after_split);
  EXPECT_EQ(replayed_original_after_split->id, certified_original->id);
  EXPECT_FALSE(replayed_original_after_split->transaction_to.empty());

  auto preserved = coordination->facts_as_of_sequence(
      "2026-07-10T00:00:00.000Z", certified_replacement->transaction_from_sequence, "atx-kb",
      "release_state");
  ASSERT_TRUE(preserved);
  ASSERT_EQ(preserved->size(), 1U);
  EXPECT_EQ(preserved->front().object, "experimental");
  EXPECT_EQ(preserved->front().evidence_status, "verified");
  EXPECT_EQ(preserved->front().evidence_source_id, original_evidence->source_id);
  EXPECT_EQ(preserved->front().evidence_observation_id, original_evidence->observation_id);
  EXPECT_EQ(preserved->front().evidence_content_hash, original_evidence->content_hash);
  EXPECT_TRUE(preserved->front().idempotency_key.empty());
  EXPECT_TRUE(preserved->front().request_valid_from.empty());
  auto events = coordination->events_after(0);
  ASSERT_TRUE(events);
  EXPECT_EQ(std::count_if(events->begin(), events->end(),
                          [](const auto &event) { return event.type == "fact.put"; }),
            2);
  EXPECT_TRUE(std::any_of(events->begin(), events->end(), [&](const auto &event) {
    return event.type == "fact.put" &&
           event.subject == "facts/" + std::to_string(certified_original->id);
  }));
  EXPECT_TRUE(std::any_of(events->begin(), events->end(), [&](const auto &event) {
    return event.type == "fact.verified" &&
           event.subject == "facts/" + std::to_string(certified_replacement->id);
  }));
  auto original_events =
      coordination->events_after(0, 100, "facts/" + std::to_string(certified_original->id));
  ASSERT_TRUE(original_events);
  ASSERT_EQ(original_events->size(), 2U);
  EXPECT_TRUE(std::all_of(original_events->begin(), original_events->end(), [&](const auto &event) {
    return event.subject == "facts/" + std::to_string(certified_original->id);
  }));
  EXPECT_TRUE(coordination->verify_evidence_links(*knowledge));
  EXPECT_TRUE(coordination->verify_integrity());
}

TEST(AgentDatabase, ConcurrentFactRetriesCommitOneTemporalTransition) {
  const auto path = database_path();
  constexpr std::size_t worker_count = 8;
  {
    auto initialized = atx::agent::AgentDatabase::open(path.string(), "retry-safe-facts");
    ASSERT_TRUE(initialized);
  }
  atx::agent::FactInput fact;
  fact.subject = "atx-db";
  fact.predicate = "release_state";
  fact.object = "production";
  fact.idempotency_key = "one-logical-fact-request";

  std::latch ready{worker_count};
  std::latch start{1};
  std::mutex mutex;
  std::vector<atx::agent::FactRecord> successful;
  std::vector<std::string> errors;
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&] {
      auto connection = atx::agent::AgentDatabase::open(path.string(), "retry-safe-facts");
      if (!connection) {
        std::lock_guard lock{mutex};
        errors.push_back(connection.error().to_string());
        ready.count_down();
        return;
      }
      ready.count_down();
      start.wait();
      auto result = connection->put_fact(fact);
      std::lock_guard lock{mutex};
      if (result) {
        successful.push_back(*result);
      } else {
        errors.push_back(result.error().to_string());
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();
  EXPECT_TRUE(errors.empty());
  ASSERT_EQ(successful.size(), worker_count);
  for (const auto &result : successful) {
    EXPECT_EQ(result.id, successful.front().id);
    EXPECT_EQ(result.transaction_from_sequence, successful.front().transaction_from_sequence);
    EXPECT_EQ(result.transaction_from, successful.front().transaction_from);
    EXPECT_TRUE(result.request_valid_from.empty());
  }

  auto reopened = atx::agent::AgentDatabase::open(path.string(), "retry-safe-facts");
  ASSERT_TRUE(reopened);
  auto conflict_request = fact;
  conflict_request.object = "different-intent";
  auto conflict = reopened->put_fact(conflict_request);
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code(), atx::core::ErrorCode::InvalidArgument);
  auto events = reopened->events_after(0);
  ASSERT_TRUE(events);
  EXPECT_EQ(std::count_if(events->begin(), events->end(),
                          [](const auto &event) { return event.type == "fact.put"; }),
            1);
  const auto fact_event = std::find_if(events->begin(), events->end(),
                                       [](const auto &event) { return event.type == "fact.put"; });
  ASSERT_NE(fact_event, events->end());
  EXPECT_EQ(fact_event->subject, "facts/" + std::to_string(successful.front().id));
  auto current = reopened->facts_as_of_sequence(successful.front().valid_from,
                                                successful.front().transaction_from_sequence,
                                                "atx-db", "release_state");
  ASSERT_TRUE(current);
  ASSERT_EQ(current->size(), 1U);
  EXPECT_EQ(current->front().id, successful.front().id);
  EXPECT_TRUE(reopened->verify_integrity());
}

TEST(AgentDatabase, WorkspacesAreIsolatedInsideOneDatabase) {
  const auto path = database_path();
  auto alpha = atx::agent::AgentDatabase::open(path.string(), "alpha");
  auto beta = atx::agent::AgentDatabase::open(path.string(), "beta");
  ASSERT_TRUE(alpha);
  ASSERT_TRUE(beta);
  auto alpha_run = alpha->create_run("Alpha only");
  auto beta_run = beta->create_run("Beta only");
  ASSERT_TRUE(alpha_run);
  ASSERT_TRUE(beta_run);
  atx::agent::TaskSpec spec;
  spec.run_id = alpha_run->id;
  spec.title = "Private task";
  auto task = alpha->add_task(spec);
  ASSERT_TRUE(task);
  auto invisible = beta->get_task(task->id);
  ASSERT_FALSE(invisible);
  EXPECT_EQ(invisible.error().code(), atx::core::ErrorCode::NotFound);
  EXPECT_TRUE(alpha->verify_integrity());
  EXPECT_TRUE(beta->verify_integrity());
}

TEST(AgentDatabase, VerifiedOnlineBackupRestoresEveryWorkspaceAndRefusesOverwrite) {
  const auto source_path = database_path();
  auto backup_path = source_path;
  backup_path += ".backup";
  std::error_code cleanup_error;
  std::filesystem::remove(backup_path, cleanup_error);
  std::filesystem::remove(backup_path.string() + ".partial", cleanup_error);
  auto alpha = atx::agent::AgentDatabase::open(source_path.string(), "backup-alpha");
  auto beta = atx::agent::AgentDatabase::open(source_path.string(), "backup-beta");
  ASSERT_TRUE(alpha);
  ASSERT_TRUE(beta);
  auto alpha_run = alpha->create_run("Back up alpha");
  auto beta_run = beta->create_run("Back up beta");
  ASSERT_TRUE(alpha_run);
  ASSERT_TRUE(beta_run);
  atx::agent::TaskSpec alpha_spec;
  alpha_spec.run_id = alpha_run->id;
  alpha_spec.title = "Alpha snapshot task";
  auto alpha_task = alpha->add_task(alpha_spec);
  atx::agent::TaskSpec beta_spec;
  beta_spec.run_id = beta_run->id;
  beta_spec.title = "Beta snapshot task";
  auto beta_task = beta->add_task(beta_spec);
  ASSERT_TRUE(alpha_task);
  ASSERT_TRUE(beta_task);
  const std::string alpha_subject = "tasks/" + alpha_task->id;
  auto alpha_consumer = alpha->register_event_consumer("alpha-indexer", alpha_subject);
  ASSERT_TRUE(alpha_consumer);
  auto alpha_batch = alpha->poll_event_consumer("alpha-indexer");
  ASSERT_TRUE(alpha_batch);
  ASSERT_EQ(alpha_batch->events.size(), 1U);
  auto alpha_checkpoint = alpha->checkpoint_event_consumer(
      "alpha-indexer", alpha_batch->consumer.revision, alpha_batch->events.back().sequence,
      "alpha-backup-checkpoint");
  ASSERT_TRUE(alpha_checkpoint);
  auto pending_event =
      alpha->append_event("projection.refresh", "pending", {}, {}, {}, {}, alpha_subject);
  ASSERT_TRUE(pending_event);
  auto pending_delivery =
      alpha->receive_event_consumer("alpha-indexer", "backup-worker", "backup-receive", 60, 1);
  ASSERT_TRUE(pending_delivery);
  ASSERT_EQ(pending_delivery->events.size(), 1U);
  EXPECT_EQ(pending_delivery->events.front().sequence, *pending_event);

  auto backed_up = alpha->backup_to(backup_path.string());
  ASSERT_TRUE(backed_up) << backed_up.error().to_string();
  EXPECT_GT(backed_up->page_count, 0);
  EXPECT_EQ(backed_up->remaining_pages, 0);
  EXPECT_TRUE(std::filesystem::exists(backup_path));
  EXPECT_FALSE(std::filesystem::exists(backup_path.string() + ".partial"));
  EXPECT_FALSE(std::filesystem::exists(backup_path.string() + ".partial-wal"));
  EXPECT_FALSE(std::filesystem::exists(backup_path.string() + ".partial-shm"));

  auto restored_alpha = atx::agent::AgentDatabase::open(backup_path.string(), "backup-alpha");
  auto restored_beta = atx::agent::AgentDatabase::open(backup_path.string(), "backup-beta");
  ASSERT_TRUE(restored_alpha);
  ASSERT_TRUE(restored_beta);
  auto alpha_copy = restored_alpha->get_task(alpha_task->id);
  auto beta_copy = restored_beta->get_task(beta_task->id);
  ASSERT_TRUE(alpha_copy);
  ASSERT_TRUE(beta_copy);
  EXPECT_EQ(alpha_copy->title, alpha_spec.title);
  EXPECT_EQ(beta_copy->title, beta_spec.title);
  auto restored_consumer = restored_alpha->get_event_consumer("alpha-indexer");
  ASSERT_TRUE(restored_consumer);
  EXPECT_EQ(restored_consumer->cursor_sequence, alpha_checkpoint->cursor_sequence);
  EXPECT_EQ(restored_consumer->revision, alpha_checkpoint->revision);
  auto restored_batch = restored_alpha->poll_event_consumer("alpha-indexer");
  ASSERT_TRUE(restored_batch);
  ASSERT_EQ(restored_batch->events.size(), 1U);
  EXPECT_EQ(restored_batch->events.front().sequence, *pending_event);
  auto restored_delivery = restored_alpha->receive_event_consumer("alpha-indexer", "backup-worker",
                                                                  "backup-receive", 60, 1);
  ASSERT_TRUE(restored_delivery);
  EXPECT_EQ(restored_delivery->delivery_token, pending_delivery->delivery_token);
  auto restored_settle = restored_alpha->settle_event_consumer_delivery(
      "alpha-indexer", "backup-worker", restored_delivery->delivery_token,
      "backup-delivery-settle");
  ASSERT_TRUE(restored_settle);
  auto restored_empty = restored_alpha->poll_event_consumer("alpha-indexer");
  ASSERT_TRUE(restored_empty);
  EXPECT_TRUE(restored_empty->events.empty());
  EXPECT_TRUE(restored_alpha->verify_integrity());
  EXPECT_TRUE(restored_beta->verify_integrity());

  auto overwrite = alpha->backup_to(backup_path.string());
  ASSERT_FALSE(overwrite);
  EXPECT_EQ(overwrite.error().code(), atx::core::ErrorCode::AlreadyExists);
}

TEST(AgentDatabase, EvidenceConsistentBackupPairPublishesVerifiableDigestManifest) {
  const auto coordination_path = database_path();
  auto knowledge_path = coordination_path;
  knowledge_path += ".knowledge";
  const std::string prefix = coordination_path.string() + ".paired";
  std::error_code ignored;
  for (const auto &path : {knowledge_path, std::filesystem::path{prefix + ".atx-db.sqlite"},
                           std::filesystem::path{prefix + ".atx-kb.sqlite"},
                           std::filesystem::path{prefix + ".manifest.sqlite"}}) {
    std::filesystem::remove(path, ignored);
  }
  auto coordination =
      atx::agent::AgentDatabase::open(coordination_path.string(), "paired-workspace");
  auto knowledge = atx::kb::KnowledgeBase::open(knowledge_path.string());
  ASSERT_TRUE(coordination);
  ASSERT_TRUE(knowledge);
  const auto [run_id, agent_id] = initialize_run(*coordination, "paired-agent");
  auto submitted = knowledge->submit(
      research("Paired backup evidence",
               "The coordination snapshot references this exact immutable knowledge observation."));
  ASSERT_TRUE(submitted);
  atx::agent::EpisodeInput episode;
  episode.idempotency_key = "paired-episode";
  episode.run_id = run_id;
  episode.agent_id = agent_id;
  episode.source_id = submitted->source_id;
  episode.observation_id = submitted->observation_id;
  ASSERT_TRUE(coordination->record_episode(episode));

  auto paired = coordination->backup_pair(*knowledge, prefix);
  ASSERT_TRUE(paired) << paired.error().to_string();
  EXPECT_EQ(paired->episode_count, 1);
  EXPECT_GE(paired->event_high_watermark, 1);
  EXPECT_EQ(paired->knowledge_observation_high_watermark, submitted->observation_id);
  EXPECT_EQ(paired->coordination_sha256.size(), 64U);
  EXPECT_EQ(paired->knowledge_sha256.size(), 64U);
  EXPECT_EQ(paired->manifest_sha256.size(), 64U);
  EXPECT_TRUE(std::filesystem::exists(paired->manifest_path));

  auto verified =
      atx::agent::AgentDatabase::verify_backup_pair(paired->manifest_path, paired->manifest_sha256);
  ASSERT_TRUE(verified) << verified.error().to_string();
  EXPECT_EQ(verified->coordination_sha256, paired->coordination_sha256);
  EXPECT_EQ(verified->knowledge_sha256, paired->knowledge_sha256);
  EXPECT_EQ(verified->manifest_sha256, paired->manifest_sha256);
  auto wrong_anchor =
      atx::agent::AgentDatabase::verify_backup_pair(paired->manifest_path, std::string(64, '0'));
  ASSERT_FALSE(wrong_anchor);
  EXPECT_EQ(wrong_anchor.error().code(), atx::core::ErrorCode::IoError);
  auto overwrite = coordination->backup_pair(*knowledge, prefix);
  ASSERT_FALSE(overwrite);
  EXPECT_EQ(overwrite.error().code(), atx::core::ErrorCode::AlreadyExists);
}

TEST(AgentDatabase, BackupPairRefusesOrphanEvidenceAndLeavesNoPublishedFiles) {
  const auto coordination_path = database_path();
  auto knowledge_path = coordination_path;
  knowledge_path += ".knowledge";
  const std::string prefix = coordination_path.string() + ".orphan-pair";
  std::error_code ignored;
  std::filesystem::remove(knowledge_path, ignored);
  auto coordination =
      atx::agent::AgentDatabase::open(coordination_path.string(), "pair-primary-workspace");
  auto hidden_workspace =
      atx::agent::AgentDatabase::open(coordination_path.string(), "hidden-orphan-workspace");
  auto knowledge = atx::kb::KnowledgeBase::open(knowledge_path.string());
  ASSERT_TRUE(coordination);
  ASSERT_TRUE(hidden_workspace);
  ASSERT_TRUE(knowledge);
  const auto [run_id, agent_id] = initialize_run(*hidden_workspace, "orphan-agent");
  atx::agent::EpisodeInput episode;
  episode.idempotency_key = "orphan-episode";
  episode.run_id = run_id;
  episode.agent_id = agent_id;
  episode.source_id = "src_missing_from_knowledge_snapshot";
  episode.observation_id = 999;
  ASSERT_TRUE(hidden_workspace->record_episode(episode));

  auto paired = coordination->backup_pair(*knowledge, prefix);
  ASSERT_FALSE(paired);
  EXPECT_EQ(paired.error().code(), atx::core::ErrorCode::Internal);
  EXPECT_FALSE(std::filesystem::exists(prefix + ".atx-db.sqlite"));
  EXPECT_FALSE(std::filesystem::exists(prefix + ".atx-kb.sqlite"));
  EXPECT_FALSE(std::filesystem::exists(prefix + ".manifest.sqlite"));
}

TEST(AgentDatabase, BackupPairVerificationDetectsSnapshotTampering) {
  const auto coordination_path = database_path();
  auto knowledge_path = coordination_path;
  knowledge_path += ".knowledge";
  const std::string prefix = coordination_path.string() + ".tampered-pair";
  std::error_code ignored;
  std::filesystem::remove(knowledge_path, ignored);
  std::filesystem::remove(prefix + ".atx-db.sqlite", ignored);
  std::filesystem::remove(prefix + ".atx-kb.sqlite", ignored);
  std::filesystem::remove(prefix + ".manifest.sqlite", ignored);
  auto coordination =
      atx::agent::AgentDatabase::open(coordination_path.string(), "tamper-workspace");
  auto knowledge = atx::kb::KnowledgeBase::open(knowledge_path.string());
  ASSERT_TRUE(coordination);
  ASSERT_TRUE(knowledge);
  ASSERT_TRUE(coordination->create_run("Manifest tamper detection"));
  ASSERT_TRUE(knowledge->submit(research(
      "Tamper detection evidence",
      "A byte-level digest must reject a modified database even if SQLite can still open it.")));
  auto paired = coordination->backup_pair(*knowledge, prefix);
  ASSERT_TRUE(paired) << paired.error().to_string();
  {
    std::ofstream tamper{paired->coordination_path, std::ios::binary | std::ios::app};
    ASSERT_TRUE(tamper);
    tamper.put('\0');
  }
  auto verified = atx::agent::AgentDatabase::verify_backup_pair(paired->manifest_path);
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code(), atx::core::ErrorCode::IoError);
}

TEST(AgentDatabase, SchemaEightMigrationBackfillsOnlyUnambiguousEventSubjects) {
  const auto path = database_path();
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw);
    ASSERT_TRUE(raw->exec(R"sql(
      CREATE TABLE agent_db_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL) STRICT;
      INSERT INTO agent_db_meta VALUES('schema_version','8');
      CREATE TABLE agent_events(
        sequence INTEGER PRIMARY KEY AUTOINCREMENT,
        workspace TEXT NOT NULL,
        run_id TEXT NOT NULL DEFAULT '',
        task_id TEXT NOT NULL DEFAULT '',
        agent_id TEXT NOT NULL DEFAULT '',
        event_type TEXT NOT NULL,
        payload TEXT NOT NULL DEFAULT '',
        idempotency_key TEXT NOT NULL DEFAULT '',
        created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
      ) STRICT;
      INSERT INTO agent_events(workspace,run_id,task_id,event_type)
        VALUES('event-migration','run-legacy','task-legacy','task.created');
      INSERT INTO agent_events(workspace,run_id,agent_id,event_type)
        VALUES('event-migration','run-legacy','agent-legacy','episode.recorded');
    )sql"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "event-migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto events = migrated->events_after(0);
  ASSERT_TRUE(events);
  ASSERT_EQ(events->size(), 2U);
  EXPECT_EQ(events->at(0).subject, "tasks/task-legacy");
  EXPECT_TRUE(events->at(1).subject.empty());
  auto consumer = migrated->register_event_consumer("migration-consumer", "tasks/task-legacy");
  ASSERT_TRUE(consumer);
  auto batch = migrated->poll_event_consumer("migration-consumer");
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->events.size(), 1U);
  EXPECT_EQ(batch->events.front().sequence, events->front().sequence);
  auto delivery = migrated->receive_event_consumer("migration-consumer", "migration-worker",
                                                   "migration-receive", 30, 1);
  ASSERT_TRUE(delivery);
  ASSERT_EQ(delivery->events.size(), 1U);
  auto settled = migrated->settle_event_consumer_delivery(
      "migration-consumer", "migration-worker", delivery->delivery_token, "migration-settle");
  ASSERT_TRUE(settled);
  EXPECT_EQ(settled->cursor_sequence, events->front().sequence);
  EXPECT_TRUE(migrated->verify_integrity());
}

TEST(AgentDatabase, SchemaTwoDatabaseMigratesToMonotonicSegmentedFacts) {
  const auto path = database_path();
  {
    auto raw = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(raw) << raw.error().to_string();
    ASSERT_TRUE(raw->exec(R"sql(
      CREATE TABLE agent_db_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL) STRICT;
      INSERT INTO agent_db_meta VALUES('schema_version','2');
      CREATE TABLE runs(
        workspace TEXT NOT NULL,id TEXT NOT NULL,objective TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'active',idempotency_key TEXT NOT NULL DEFAULT '',
        revision INTEGER NOT NULL DEFAULT 1,created_at TEXT NOT NULL DEFAULT '',
        updated_at TEXT NOT NULL DEFAULT '',PRIMARY KEY(workspace,id)
      ) WITHOUT ROWID, STRICT;
      CREATE TABLE tasks(
        workspace TEXT NOT NULL,id TEXT NOT NULL,run_id TEXT NOT NULL,title TEXT NOT NULL,
        description TEXT NOT NULL DEFAULT '',status TEXT NOT NULL DEFAULT 'queued',
        priority INTEGER NOT NULL DEFAULT 0,max_attempts INTEGER NOT NULL DEFAULT 3,
        attempts INTEGER NOT NULL DEFAULT 0,revision INTEGER NOT NULL DEFAULT 1,
        lease_owner TEXT NOT NULL DEFAULT '',lease_token TEXT NOT NULL DEFAULT '',
        lease_expires_at TEXT NOT NULL DEFAULT '',result_source_id TEXT NOT NULL DEFAULT '',
        last_error TEXT NOT NULL DEFAULT '',idempotency_key TEXT NOT NULL DEFAULT '',
        created_at TEXT NOT NULL DEFAULT '',updated_at TEXT NOT NULL DEFAULT '',
        PRIMARY KEY(workspace,id),
        FOREIGN KEY(workspace,run_id) REFERENCES runs(workspace,id)
      ) WITHOUT ROWID, STRICT;
      CREATE TABLE facts(
        id INTEGER PRIMARY KEY,workspace TEXT NOT NULL,subject TEXT NOT NULL,
        predicate TEXT NOT NULL,object TEXT NOT NULL,valid_from TEXT NOT NULL,valid_to TEXT,
        transaction_from TEXT NOT NULL,transaction_to TEXT,evidence_source_id TEXT NOT NULL DEFAULT '',
        confidence REAL NOT NULL,supersedes_fact_id INTEGER REFERENCES facts(id)
      ) STRICT;
      CREATE UNIQUE INDEX facts_current_interval_unique
        ON facts(workspace,subject,predicate,valid_from) WHERE transaction_to IS NULL;
      INSERT INTO facts(workspace,subject,predicate,object,valid_from,transaction_from,
                        evidence_source_id,confidence)
        VALUES('migration','legacy-engine','provenance','imported',
               '2025-01-01T00:00:00.000Z','2025-01-02T00:00:00.000Z','src_legacy',1.0);
    )sql"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "migration");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  atx::agent::FactInput old;
  old.subject = "engine";
  old.predicate = "state";
  old.object = "old";
  old.valid_from = "2026-01-01T00:00:00.000Z";
  ASSERT_TRUE(migrated->put_fact(old));
  auto current = old;
  current.object = "new";
  current.valid_from = "2026-06-01T00:00:00.000Z";
  ASSERT_TRUE(migrated->put_fact(current));
  auto legacy =
      migrated->facts_as_of_sequence("2026-01-01T00:00:00.000Z", 1, "legacy-engine", "provenance");
  ASSERT_TRUE(legacy);
  ASSERT_EQ(legacy->size(), 1U);
  EXPECT_EQ(legacy->front().evidence_source_id, "src_legacy");
  EXPECT_EQ(legacy->front().evidence_status, "unverified");
  EXPECT_EQ(legacy->front().evidence_observation_id, 0);
  EXPECT_TRUE(legacy->front().evidence_content_hash.empty());
  EXPECT_TRUE(legacy->front().idempotency_key.empty());
  EXPECT_TRUE(legacy->front().request_valid_from.empty());
  auto integrity = migrated->verify_integrity();
  EXPECT_TRUE(integrity) << (integrity ? "" : integrity.error().to_string());
}

TEST(AgentDatabase, SchemaThreeMigrationRepairsTasksBlockedByTerminalDependencies) {
  const auto path = database_path();
  std::string parent_id;
  std::string child_id;
  std::string beta_parent_id;
  std::string beta_child_id;
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "migration-v4");
    ASSERT_TRUE(database) << database.error().to_string();
    const auto [run_id, ignored_agent] = initialize_run(*database);
    (void)ignored_agent;
    atx::agent::TaskSpec parent_spec;
    parent_spec.run_id = run_id;
    parent_spec.title = "Legacy failed dependency";
    auto parent = database->add_task(parent_spec);
    ASSERT_TRUE(parent);
    parent_id = parent->id;
    atx::agent::TaskSpec child_spec;
    child_spec.run_id = run_id;
    child_spec.title = "Legacy blocked child";
    child_spec.dependencies = {parent_id};
    auto child = database->add_task(child_spec);
    ASSERT_TRUE(child);
    child_id = child->id;
  }
  {
    auto database = atx::agent::AgentDatabase::open(path.string(), "migration-v4-beta");
    ASSERT_TRUE(database) << database.error().to_string();
    const auto [run_id, ignored_agent] = initialize_run(*database, "beta-agent");
    (void)ignored_agent;
    atx::agent::TaskSpec parent_spec;
    parent_spec.run_id = run_id;
    parent_spec.title = "Beta legacy failed dependency";
    auto parent = database->add_task(parent_spec);
    ASSERT_TRUE(parent);
    beta_parent_id = parent->id;
    atx::agent::TaskSpec child_spec;
    child_spec.run_id = run_id;
    child_spec.title = "Beta legacy blocked child";
    child_spec.dependencies = {beta_parent_id};
    auto child = database->add_task(child_spec);
    ASSERT_TRUE(child);
    beta_child_id = child->id;
  }
  {
    auto database = atx::core::db::Database::open(path.string());
    ASSERT_TRUE(database) << database.error().to_string();
    auto fail = database->prepare("UPDATE tasks SET status='failed' WHERE workspace=?1 AND id=?2");
    ASSERT_TRUE(fail);
    ASSERT_TRUE(fail->bind(1, "migration-v4"));
    ASSERT_TRUE(fail->bind(2, parent_id));
    ASSERT_TRUE(fail->step());
    ASSERT_TRUE(fail->reset());
    ASSERT_TRUE(fail->clear_bindings());
    ASSERT_TRUE(fail->bind(1, "migration-v4-beta"));
    ASSERT_TRUE(fail->bind(2, beta_parent_id));
    ASSERT_TRUE(fail->step());
    ASSERT_TRUE(database->exec("UPDATE agent_db_meta SET value='3' WHERE key='schema_version'"));
  }
  auto migrated = atx::agent::AgentDatabase::open(path.string(), "migration-v4");
  ASSERT_TRUE(migrated) << migrated.error().to_string();
  auto child = migrated->get_task(child_id);
  ASSERT_TRUE(child);
  EXPECT_EQ(child->status, "cancelled");
  EXPECT_NE(child->last_error.find(parent_id), std::string::npos);
  EXPECT_TRUE(migrated->verify_integrity());
  auto beta = atx::agent::AgentDatabase::open(path.string(), "migration-v4-beta");
  ASSERT_TRUE(beta) << beta.error().to_string();
  auto beta_child = beta->get_task(beta_child_id);
  ASSERT_TRUE(beta_child);
  EXPECT_EQ(beta_child->status, "cancelled");
  EXPECT_NE(beta_child->last_error.find(beta_parent_id), std::string::npos);
  EXPECT_TRUE(beta->verify_integrity());
}

} // namespace atxtest_agent_db
