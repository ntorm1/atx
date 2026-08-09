#pragma once

// atx::agent::AgentDatabase is the durable control plane for cooperating
// agents. It complements atx-kb's evidence store with workspace isolation,
// dependency-aware tasks, expiring leases, append-only events, evidence-linked
// episodes, and bitemporal facts.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"

namespace atx::kb {
class KnowledgeBase;
}

namespace atx::agent {

struct RunRecord {
  std::string id;
  std::string objective;
  std::string status;
  std::int64_t revision{};
  std::string created_at;
  std::string updated_at;
};

struct AgentRecord {
  std::string id;
  std::string run_id;
  std::string role;
  std::string capabilities;
  std::string status;
  std::int64_t revision{};
  std::string last_heartbeat_at;
};

struct TaskSpec {
  std::string run_id;
  std::string title;
  std::string description;
  std::string idempotency_key;
  std::vector<std::string> dependencies;
  std::int64_t priority{};
  std::int64_t max_attempts{3};
};

struct TaskRecord {
  std::string id;
  std::string run_id;
  std::string title;
  std::string description;
  std::string status;
  std::int64_t priority{};
  std::int64_t max_attempts{};
  std::int64_t attempts{};
  std::int64_t revision{};
  std::string lease_owner;
  std::string lease_token;
  std::string lease_expires_at;
  std::string result_source_id;
  std::int64_t result_observation_id{};
  std::string result_content_hash;
  std::string result_evidence_status;
  std::string last_error;
  std::string created_at;
  std::string updated_at;
};

struct TaskEvent {
  std::int64_t sequence{};
  std::int64_t root_sequence{};
  std::int64_t redrive_count{};
  std::string run_id;
  std::string task_id;
  std::string agent_id;
  std::string type;
  std::string subject;
  std::string payload;
  std::string created_at;
};

struct EventConsumerRecord {
  std::string name;
  std::string subject_filter;
  std::int64_t max_delivery_attempts{};
  std::int64_t retry_backoff_seconds{};
  std::int64_t retry_backoff_max_seconds{};
  std::string retry_jitter;
  std::int64_t redrive_rate_per_second{};
  std::int64_t redrive_burst_events{};
  std::int64_t redrive_token_millis{};
  std::string redrive_refilled_at;
  std::int64_t max_redrive_count{};
  std::int64_t self_control_event_cutoff_sequence{};
  std::int64_t start_sequence{};
  std::int64_t cursor_sequence{};
  std::int64_t revision{};
  std::string created_at;
  std::string updated_at;
};

struct EventConsumerBatch {
  EventConsumerRecord consumer;
  std::vector<TaskEvent> events;
};

struct EventConsumerStatus {
  EventConsumerRecord consumer;
  std::string observed_at;
  std::int64_t event_high_watermark{};
  std::int64_t consumer_state_revision{};
  std::string next_dynamic_transition_at;
  std::int64_t pending_visible_event_count{};
  std::int64_t first_pending_visible_sequence{};
  std::int64_t last_pending_visible_sequence{};
  std::string oldest_pending_visible_event_at;
  std::string delivery_head_state;
  std::string delivery_head_owner;
  std::int64_t delivery_head_attempt{};
  std::int64_t delivery_head_through_sequence{};
  std::int64_t delivery_head_event_count{};
  std::string delivery_head_expires_at;
  std::string delivery_head_retry_at;
  std::int64_t queued_visible_event_count{};
  std::int64_t available_visible_event_count{};
  std::int64_t retained_dead_letter_count{};
  std::int64_t open_dead_letter_count{};
  std::int64_t open_dead_letter_event_count{};
  std::string oldest_open_dead_letter_at;
  std::int64_t redriven_dead_letter_count{};
  std::int64_t quarantined_dead_letter_count{};
};

struct EventConsumerFleetStatus {
  std::string workspace;
  std::string observed_at;
  std::int64_t event_high_watermark{};
  std::int64_t consumer_state_revision{};
  std::string next_dynamic_transition_at;
  std::vector<EventConsumerStatus> consumers;
};

struct EventConsumerFleetValidator {
  std::string workspace;
  std::int64_t event_high_watermark{};
  std::int64_t consumer_state_revision{};
  std::string next_dynamic_transition_at;
};

struct EventConsumerFleetCacheValidation {
  bool cache_valid{};
  std::string validated_at;
  EventConsumerFleetValidator current;
  std::optional<EventConsumerFleetStatus> snapshot;
};

struct EventConsumerDelivery {
  EventConsumerRecord consumer;
  std::string delivery_token;
  std::string owner;
  std::string request_token;
  std::int64_t previous_sequence{};
  std::int64_t through_sequence{};
  std::int64_t attempt{};
  std::int64_t retry_delay_seconds{};
  std::string acquired_at;
  std::string expires_at;
  std::string retry_not_before;
  std::int64_t dead_lettered_batches{};
  std::int64_t dead_lettered_events{};
  std::vector<TaskEvent> events;
};

enum class EventConsumerRetryJitter { None, Full };
enum class EventConsumerRejectionDisposition { Retry, DeadLetter };

struct EventConsumerRejection {
  EventConsumerRecord consumer;
  std::string delivery_token;
  std::string owner;
  std::string rejection_token;
  std::string disposition;
  std::string reason;
  std::int64_t attempt{};
  std::int64_t retry_delay_seconds{};
  std::string rejected_at;
  std::string retry_not_before;
  bool dead_lettered{};
  std::int64_t dead_letter_id{};
};

struct EventConsumerDeadLetter {
  std::int64_t id{};
  std::string consumer_name;
  std::string delivery_token;
  std::int64_t previous_sequence{};
  std::int64_t through_sequence{};
  std::int64_t delivery_attempts{};
  std::string reason;
  std::string rejection_disposition;
  std::string rejection_reason;
  std::string status;
  std::string redrive_token;
  std::int64_t redrive_budget_event_count{};
  std::int64_t redrive_budget_before_millis{};
  std::int64_t redrive_budget_after_millis{};
  std::string redrive_budget_refilled_at;
  std::string quarantine_token;
  std::string quarantined_by;
  std::string quarantine_reason;
  std::string quarantined_at;
  std::int64_t dead_lettered_event_sequence{};
  std::int64_t redriven_event_sequence{};
  std::int64_t quarantined_event_sequence{};
  std::string created_at;
  std::string redriven_at;
  std::vector<TaskEvent> events;
  std::vector<TaskEvent> redriven_events;
};

struct EpisodeInput {
  std::string idempotency_key;
  std::string run_id;
  std::string task_id;
  std::string agent_id;
  std::string source_id;
  std::int64_t observation_id{};
  std::string type{"work"};
};

struct EpisodeRecord {
  std::int64_t id{};
  std::string run_id;
  std::string task_id;
  std::string agent_id;
  std::string source_id;
  std::int64_t observation_id{};
  std::string type;
  std::string evidence_status;
  std::string evidence_content_hash;
  std::string evidence_verified_at;
  std::string created_at;
};

struct FactInput {
  std::string subject;
  std::string predicate;
  std::string object;
  std::string valid_from;
  std::string valid_to;
  std::string evidence_source_id;
  std::string idempotency_key;
  double confidence{1.0};
};

struct FactRecord {
  std::int64_t id{};
  std::string subject;
  std::string predicate;
  std::string object;
  std::string valid_from;
  std::string valid_to;
  std::string transaction_from;
  std::string transaction_to;
  std::int64_t transaction_from_sequence{};
  std::int64_t transaction_to_sequence{};
  std::string evidence_source_id;
  std::int64_t evidence_observation_id{};
  std::string evidence_content_hash;
  std::string evidence_status;
  std::string idempotency_key;
  std::string request_valid_from;
  double confidence{};
  std::int64_t supersedes_fact_id{};
};

struct BackupPairReport {
  std::string coordination_path;
  std::string knowledge_path;
  std::string manifest_path;
  std::string coordination_sha256;
  std::string knowledge_sha256;
  std::string manifest_sha256;
  std::int64_t event_high_watermark{};
  std::int64_t episode_count{};
  std::int64_t knowledge_observation_high_watermark{};
  atx::core::db::BackupReport coordination_backup;
  atx::core::db::BackupReport knowledge_backup;
};

class AgentDatabase {
public:
  [[nodiscard]] static atx::core::Result<AgentDatabase> open(std::string_view path,
                                                             std::string_view workspace);
  [[nodiscard]] static atx::core::Result<AgentDatabase>
  open_memory(std::string_view workspace = "default");

  AgentDatabase(AgentDatabase &&) noexcept = default;
  AgentDatabase &operator=(AgentDatabase &&) noexcept = default;
  AgentDatabase(const AgentDatabase &) = delete;
  AgentDatabase &operator=(const AgentDatabase &) = delete;

  [[nodiscard]] atx::core::Result<RunRecord> create_run(std::string_view objective,
                                                        std::string_view idempotency_key = {});
  [[nodiscard]] atx::core::Status finish_run(std::string_view run_id,
                                             std::string_view status = "completed");
  [[nodiscard]] atx::core::Result<AgentRecord> register_agent(std::string_view run_id,
                                                              std::string_view agent_id,
                                                              std::string_view role,
                                                              std::string_view capabilities = {});
  [[nodiscard]] atx::core::Status heartbeat(std::string_view agent_id,
                                            std::int64_t expected_revision = -1);

  [[nodiscard]] atx::core::Result<TaskRecord> add_task(const TaskSpec &task);
  [[nodiscard]] atx::core::Result<TaskRecord> claim_next(std::string_view agent_id,
                                                         std::int64_t lease_seconds = 300);
  [[nodiscard]] atx::core::Result<TaskRecord> renew_lease(std::string_view task_id,
                                                          std::string_view agent_id,
                                                          std::string_view lease_token,
                                                          std::int64_t lease_seconds = 300);
  [[nodiscard]] atx::core::Status complete_task(std::string_view task_id, std::string_view agent_id,
                                                std::string_view lease_token,
                                                std::string_view result_source_id = {});
  [[nodiscard]] atx::core::Status complete_task_verified(std::string_view task_id,
                                                         std::string_view agent_id,
                                                         std::string_view lease_token,
                                                         std::string_view result_source_id,
                                                         std::int64_t result_observation_id,
                                                         atx::kb::KnowledgeBase &knowledge_base);
  [[nodiscard]] atx::core::Status fail_task(std::string_view task_id, std::string_view agent_id,
                                            std::string_view lease_token, std::string_view error);
  [[nodiscard]] atx::core::Result<TaskRecord> get_task(std::string_view task_id);
  [[nodiscard]] atx::core::Result<std::vector<TaskRecord>> list_tasks(std::string_view run_id = {},
                                                                      std::size_t limit = 100);

  [[nodiscard]] atx::core::Result<std::int64_t>
  append_event(std::string_view type, std::string_view payload, std::string_view run_id = {},
               std::string_view task_id = {}, std::string_view agent_id = {},
               std::string_view idempotency_key = {}, std::string_view subject = {});
  [[nodiscard]] atx::core::Result<std::vector<TaskEvent>>
  events_after(std::int64_t sequence, std::size_t limit = 100, std::string_view subject = {});
  [[nodiscard]] atx::core::Result<EventConsumerRecord> register_event_consumer(
      std::string_view name, std::string_view subject_filter = {}, std::int64_t start_sequence = 0,
      std::int64_t max_delivery_attempts = 0, std::int64_t retry_backoff_seconds = 0,
      std::int64_t retry_backoff_max_seconds = 0,
      EventConsumerRetryJitter retry_jitter = EventConsumerRetryJitter::None,
      std::int64_t redrive_rate_per_second = 0, std::int64_t redrive_burst_events = 0,
      std::int64_t max_redrive_count = 0);
  [[nodiscard]] atx::core::Result<EventConsumerRecord> get_event_consumer(std::string_view name);
  [[nodiscard]] atx::core::Result<EventConsumerStatus>
  get_event_consumer_status(std::string_view name);
  [[nodiscard]] atx::core::Result<EventConsumerFleetStatus> list_event_consumer_statuses();
  [[nodiscard]] atx::core::Result<EventConsumerFleetCacheValidation>
  list_event_consumer_statuses_if_current(const EventConsumerFleetValidator &cached);
  [[nodiscard]] atx::core::Result<EventConsumerBatch> poll_event_consumer(std::string_view name,
                                                                          std::size_t limit = 100);
  [[nodiscard]] atx::core::Result<EventConsumerDelivery>
  receive_event_consumer(std::string_view name, std::string_view owner,
                         std::string_view request_token, std::int64_t lease_seconds = 30,
                         std::size_t limit = 100);
  [[nodiscard]] atx::core::Result<EventConsumerDelivery>
  renew_event_consumer_delivery(std::string_view name, std::string_view owner,
                                std::string_view delivery_token, std::int64_t lease_seconds = 30);
  [[nodiscard]] atx::core::Result<EventConsumerRejection> reject_event_consumer_delivery(
      std::string_view name, std::string_view owner, std::string_view delivery_token,
      std::string_view rejection_token, std::string_view reason,
      EventConsumerRejectionDisposition disposition = EventConsumerRejectionDisposition::Retry);
  [[nodiscard]] atx::core::Result<EventConsumerRecord>
  settle_event_consumer_delivery(std::string_view name, std::string_view owner,
                                 std::string_view delivery_token,
                                 std::string_view checkpoint_token);
  [[nodiscard]] atx::core::Result<std::vector<EventConsumerDeadLetter>>
  list_event_consumer_dead_letters(std::string_view name, std::size_t limit = 100);
  [[nodiscard]] atx::core::Result<EventConsumerDeadLetter>
  redrive_event_consumer_dead_letter(std::string_view name, std::int64_t dead_letter_id,
                                     std::string_view redrive_token);
  [[nodiscard]] atx::core::Result<EventConsumerDeadLetter>
  quarantine_event_consumer_dead_letter(std::string_view name, std::int64_t dead_letter_id,
                                        std::string_view quarantined_by,
                                        std::string_view quarantine_token, std::string_view reason);
  [[nodiscard]] atx::core::Result<EventConsumerRecord>
  checkpoint_event_consumer(std::string_view name, std::int64_t expected_revision,
                            std::int64_t through_sequence, std::string_view checkpoint_token);

  [[nodiscard]] atx::core::Result<EpisodeRecord> record_episode(const EpisodeInput &episode);
  [[nodiscard]] atx::core::Result<EpisodeRecord>
  record_verified_episode(const EpisodeInput &episode, atx::kb::KnowledgeBase &knowledge_base);
  [[nodiscard]] atx::core::Result<FactRecord> put_fact(const FactInput &fact);
  [[nodiscard]] atx::core::Result<FactRecord>
  put_verified_fact(const FactInput &fact, std::int64_t evidence_observation_id,
                    atx::kb::KnowledgeBase &knowledge_base);
  [[nodiscard]] atx::core::Result<std::vector<FactRecord>>
  facts_as_of(std::string_view valid_time, std::string_view transaction_time,
              std::string_view subject = {}, std::string_view predicate = {},
              std::size_t limit = 100);
  [[nodiscard]] atx::core::Result<std::vector<FactRecord>>
  facts_as_of_sequence(std::string_view valid_time, std::int64_t transaction_sequence,
                       std::string_view subject = {}, std::string_view predicate = {},
                       std::size_t limit = 100);

  [[nodiscard]] atx::core::Status verify_integrity();
  [[nodiscard]] atx::core::Status verify_evidence_links(atx::kb::KnowledgeBase &knowledge_base);
  // Copies the complete multi-workspace file through SQLite's online backup
  // API and verifies this object's workspace before publishing a new path.
  [[nodiscard]] atx::core::Result<atx::core::db::BackupReport>
  backup_to(std::string_view destination_path, const atx::core::db::BackupOptions &options = {});
  // Coordination is snapshotted before the append-only knowledge store. The
  // manifest is published only after restored evidence links and file digests
  // verify, making the manifest the pair's recovery commit point.
  [[nodiscard]] atx::core::Result<BackupPairReport>
  backup_pair(atx::kb::KnowledgeBase &knowledge_base, std::string_view destination_prefix,
              const atx::core::db::BackupOptions &options = {});
  [[nodiscard]] static atx::core::Result<BackupPairReport>
  verify_backup_pair(std::string_view manifest_path,
                     std::string_view expected_manifest_sha256 = {});
  [[nodiscard]] const std::string &workspace() const noexcept { return workspace_; }

private:
  AgentDatabase(atx::core::db::Database database, std::string workspace)
      : database_{std::move(database)}, workspace_{std::move(workspace)} {}

  [[nodiscard]] atx::core::Status initialize();
  [[nodiscard]] atx::core::Result<EpisodeRecord>
  record_episode_internal(const EpisodeInput &episode, std::string_view evidence_content_hash);
  [[nodiscard]] atx::core::Status
  complete_task_internal(std::string_view task_id, std::string_view agent_id,
                         std::string_view lease_token, std::string_view result_source_id,
                         std::int64_t result_observation_id, std::string_view result_content_hash);
  [[nodiscard]] atx::core::Result<FactRecord>
  put_fact_internal(const FactInput &fact, std::int64_t evidence_observation_id,
                    std::string_view evidence_content_hash);

  atx::core::db::Database database_;
  std::string workspace_;
};

} // namespace atx::agent
