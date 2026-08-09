#include "atx/agent/database.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/kb/knowledge_base.hpp"

namespace atx::agent {
namespace {

using atx::i64;
using atx::usize;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
using atx::core::db::Database;
using atx::core::db::Statement;
using atx::core::db::Transaction;

constexpr i64 kSchemaVersion = 23;
constexpr usize kMaximumWorkspaceBytes = 256;
constexpr usize kMaximumIdBytes = 512;
constexpr usize kMaximumTitleBytes = 16U * 1024U;
constexpr usize kMaximumPayloadBytes = 1U * 1024U * 1024U;
constexpr i64 kMaximumLeaseSeconds = 86'400;
constexpr i64 kRedriveTokenUnitsPerEvent = 1'000;
constexpr i64 kMaximumRedriveRatePerSecond = 1'000;
constexpr i64 kMaximumRedriveBurstEvents = 1'000;

[[nodiscard]] constexpr i64 delivery_retry_backoff(i64 base_seconds, i64 max_seconds,
                                                   i64 attempt) noexcept {
  if (base_seconds == 0 || max_seconds == 0 || attempt < 1) {
    return 0;
  }
  i64 delay = base_seconds;
  for (i64 exponent = 1; exponent < attempt && delay < max_seconds; ++exponent) {
    delay = delay > max_seconds / 2 ? max_seconds : std::min(max_seconds, delay * 2);
  }
  return delay;
}

[[nodiscard]] Result<i64> sample_delivery_retry_delay(Database &database, std::string_view jitter,
                                                      i64 maximum_delay) {
  if (jitter == "none") {
    return Ok(maximum_delay);
  }
  if (jitter != "full" || maximum_delay < 0 || maximum_delay > kMaximumLeaseSeconds) {
    return Err(ErrorCode::Internal, "event consumer retry jitter policy is invalid");
  }
  if (maximum_delay == 0) {
    return Ok(maximum_delay);
  }
  const auto window = static_cast<std::uint64_t>(maximum_delay) + 1U;
  const auto rejection_threshold = (std::uint64_t{0} - window) % window;
  while (true) {
    ATX_TRY(auto random, database.prepare("SELECT randomblob(8)"));
    ATX_TRY(const auto step, random.step());
    if (step != Statement::Step::Row) {
      return Err(ErrorCode::Internal, "could not sample event consumer retry jitter");
    }
    const auto bytes = random.column_blob(0);
    if (bytes.size() != sizeof(std::uint64_t)) {
      return Err(ErrorCode::Internal, "event consumer retry jitter sample is invalid");
    }
    std::uint64_t sample{};
    for (const auto byte : bytes) {
      sample = (sample << 8U) | std::to_integer<unsigned char>(byte);
    }
    if (sample >= rejection_threshold) {
      return Ok(static_cast<i64>(sample % window));
    }
  }
}

[[nodiscard]] Result<i64> elapsed_milliseconds(std::string_view earlier, std::string_view later) {
  ATX_TRY(const auto earlier_time, atx::core::time::from_iso8601(earlier));
  ATX_TRY(const auto later_time, atx::core::time::from_iso8601(later));
  return Ok((later_time - earlier_time).count_ms());
}

constexpr std::string_view kSchema = R"sql(
CREATE TABLE IF NOT EXISTS agent_db_meta(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
) STRICT;
INSERT OR IGNORE INTO agent_db_meta(key,value) VALUES('schema_version','23');

CREATE TABLE IF NOT EXISTS runs(
  workspace TEXT NOT NULL,
  id TEXT NOT NULL,
  objective TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'active'
    CHECK(status IN ('active','completed','failed','cancelled')),
  idempotency_key TEXT NOT NULL DEFAULT '',
  revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  PRIMARY KEY(workspace,id)
) WITHOUT ROWID, STRICT;
CREATE UNIQUE INDEX IF NOT EXISTS runs_idempotency_idx
  ON runs(workspace,idempotency_key) WHERE idempotency_key<>'';

CREATE TABLE IF NOT EXISTS agents(
  workspace TEXT NOT NULL,
  id TEXT NOT NULL,
  run_id TEXT NOT NULL,
  role TEXT NOT NULL,
  capabilities TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','offline','stopped')),
  revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
  registered_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  last_heartbeat_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  PRIMARY KEY(workspace,id),
  FOREIGN KEY(workspace,run_id) REFERENCES runs(workspace,id)
) WITHOUT ROWID, STRICT;
CREATE INDEX IF NOT EXISTS agents_run_idx ON agents(workspace,run_id,status);

CREATE TABLE IF NOT EXISTS tasks(
  workspace TEXT NOT NULL,
  id TEXT NOT NULL,
  run_id TEXT NOT NULL,
  title TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'queued'
    CHECK(status IN ('queued','leased','completed','failed','cancelled')),
  priority INTEGER NOT NULL DEFAULT 0,
  max_attempts INTEGER NOT NULL DEFAULT 3 CHECK(max_attempts BETWEEN 1 AND 1000),
  attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts >= 0),
  revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
  lease_owner TEXT NOT NULL DEFAULT '',
  lease_token TEXT NOT NULL DEFAULT '',
  lease_expires_at TEXT NOT NULL DEFAULT '',
  result_source_id TEXT NOT NULL DEFAULT '',
  result_observation_id INTEGER NOT NULL DEFAULT 0 CHECK(result_observation_id>=0),
  result_content_hash TEXT NOT NULL DEFAULT '',
  result_evidence_status TEXT NOT NULL DEFAULT 'none'
    CHECK(result_evidence_status IN ('none','unverified','verified')),
  last_error TEXT NOT NULL DEFAULT '',
  last_transition_token TEXT NOT NULL DEFAULT '',
  last_transition_kind TEXT NOT NULL DEFAULT '',
  idempotency_key TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  PRIMARY KEY(workspace,id),
  FOREIGN KEY(workspace,run_id) REFERENCES runs(workspace,id),
  CHECK((result_evidence_status='none' AND result_source_id='' AND result_observation_id=0 AND
         result_content_hash='') OR
        (result_evidence_status='unverified' AND result_source_id<>'' AND
         result_observation_id=0 AND result_content_hash='') OR
        (result_evidence_status='verified' AND result_source_id<>'' AND
         result_observation_id>0 AND length(result_content_hash)=64))
) WITHOUT ROWID, STRICT;
CREATE UNIQUE INDEX IF NOT EXISTS tasks_idempotency_idx
  ON tasks(workspace,idempotency_key) WHERE idempotency_key<>'';
CREATE INDEX IF NOT EXISTS tasks_schedule_idx
  ON tasks(workspace,run_id,status,priority DESC,created_at,id);
CREATE INDEX IF NOT EXISTS tasks_lease_idx
  ON tasks(workspace,status,lease_expires_at) WHERE status='leased';

CREATE TABLE IF NOT EXISTS task_dependencies(
  workspace TEXT NOT NULL,
  run_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  depends_on_task_id TEXT NOT NULL,
  PRIMARY KEY(workspace,task_id,depends_on_task_id),
  FOREIGN KEY(workspace,task_id) REFERENCES tasks(workspace,id) ON DELETE CASCADE,
  FOREIGN KEY(workspace,depends_on_task_id) REFERENCES tasks(workspace,id),
  FOREIGN KEY(workspace,run_id) REFERENCES runs(workspace,id),
  CHECK(task_id <> depends_on_task_id)
) WITHOUT ROWID, STRICT;
CREATE INDEX IF NOT EXISTS task_dependencies_parent_idx
  ON task_dependencies(workspace,depends_on_task_id,task_id);

CREATE TABLE IF NOT EXISTS agent_events(
  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  root_sequence INTEGER NOT NULL DEFAULT 0 CHECK(root_sequence>=0),
  redrive_count INTEGER NOT NULL DEFAULT 0 CHECK(redrive_count>=0),
  workspace TEXT NOT NULL,
  run_id TEXT NOT NULL DEFAULT '',
  task_id TEXT NOT NULL DEFAULT '',
  agent_id TEXT NOT NULL DEFAULT '',
  event_type TEXT NOT NULL,
  subject TEXT NOT NULL DEFAULT '',
  payload TEXT NOT NULL DEFAULT '',
  idempotency_key TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
) STRICT;
CREATE UNIQUE INDEX IF NOT EXISTS agent_events_idempotency_idx
  ON agent_events(workspace,idempotency_key) WHERE idempotency_key<>'';
CREATE INDEX IF NOT EXISTS agent_events_poll_idx ON agent_events(workspace,sequence);
CREATE INDEX IF NOT EXISTS agent_events_task_idx ON agent_events(workspace,task_id,sequence);
CREATE INDEX IF NOT EXISTS agent_events_subject_idx ON agent_events(workspace,subject,sequence)
  WHERE subject<>'';
CREATE INDEX IF NOT EXISTS agent_events_lineage_idx
  ON agent_events(workspace,root_sequence,redrive_count,sequence);

CREATE TABLE IF NOT EXISTS event_consumers(
  workspace TEXT NOT NULL,
  name TEXT NOT NULL,
  subject_filter TEXT NOT NULL DEFAULT '',
  max_delivery_attempts INTEGER NOT NULL DEFAULT 0 CHECK(max_delivery_attempts BETWEEN 0 AND 1000),
  retry_backoff_seconds INTEGER NOT NULL DEFAULT 0
    CHECK(retry_backoff_seconds BETWEEN 0 AND 86400),
  retry_backoff_max_seconds INTEGER NOT NULL DEFAULT 0
    CHECK(retry_backoff_max_seconds BETWEEN 0 AND 86400),
  retry_jitter TEXT NOT NULL DEFAULT 'none' CHECK(retry_jitter IN ('none','full')),
  redrive_rate_per_second INTEGER NOT NULL DEFAULT 0
    CHECK(redrive_rate_per_second BETWEEN 0 AND 1000),
  redrive_burst_events INTEGER NOT NULL DEFAULT 0
    CHECK(redrive_burst_events BETWEEN 0 AND 1000),
  redrive_token_millis INTEGER NOT NULL DEFAULT 0
    CHECK(redrive_token_millis BETWEEN 0 AND 1000000),
  redrive_refilled_at TEXT NOT NULL DEFAULT '',
  max_redrive_count INTEGER NOT NULL DEFAULT 0 CHECK(max_redrive_count BETWEEN 0 AND 1000),
  self_control_event_cutoff_sequence INTEGER NOT NULL DEFAULT 0
    CHECK(self_control_event_cutoff_sequence>=0),
  start_sequence INTEGER NOT NULL DEFAULT 0 CHECK(start_sequence>=0),
  cursor_sequence INTEGER NOT NULL DEFAULT 0 CHECK(cursor_sequence>=start_sequence),
  revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1),
  active_delivery_token TEXT NOT NULL DEFAULT '',
  active_delivery_owner TEXT NOT NULL DEFAULT '',
  active_delivery_previous_sequence INTEGER NOT NULL DEFAULT 0
    CHECK(active_delivery_previous_sequence>=0),
  active_delivery_through_sequence INTEGER NOT NULL DEFAULT 0
    CHECK(active_delivery_through_sequence>=0),
  active_delivery_attempt INTEGER NOT NULL DEFAULT 0 CHECK(active_delivery_attempt>=0),
  active_delivery_retry_delay_seconds INTEGER NOT NULL DEFAULT 0
    CHECK(active_delivery_retry_delay_seconds BETWEEN 0 AND 86400),
  active_delivery_expires_at TEXT NOT NULL DEFAULT '',
  active_delivery_retry_at TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  CHECK((active_delivery_token='' AND active_delivery_owner='' AND
         active_delivery_previous_sequence=0 AND active_delivery_through_sequence=0 AND
         active_delivery_attempt=0 AND active_delivery_retry_delay_seconds=0 AND
         active_delivery_expires_at='' AND
         active_delivery_retry_at='') OR
        (active_delivery_token<>'' AND active_delivery_owner<>'' AND
         active_delivery_previous_sequence=cursor_sequence AND
         active_delivery_through_sequence>active_delivery_previous_sequence AND
         active_delivery_attempt>=1 AND active_delivery_retry_delay_seconds>=0 AND
         active_delivery_expires_at<>'' AND
         active_delivery_retry_at>=active_delivery_expires_at)),
  CHECK((retry_backoff_seconds=0 AND retry_backoff_max_seconds=0) OR
        (retry_backoff_seconds>=1 AND retry_backoff_max_seconds>=retry_backoff_seconds)),
  CHECK((redrive_rate_per_second=0 AND redrive_burst_events=0 AND
         redrive_token_millis=0 AND redrive_refilled_at='') OR
        (redrive_rate_per_second>=1 AND redrive_burst_events>=1 AND
         redrive_token_millis<=redrive_burst_events*1000 AND redrive_refilled_at<>'')),
  PRIMARY KEY(workspace,name)
) WITHOUT ROWID, STRICT;
CREATE INDEX IF NOT EXISTS event_consumers_cursor_idx
  ON event_consumers(workspace,cursor_sequence,name);
CREATE UNIQUE INDEX IF NOT EXISTS event_consumers_active_delivery_idx
  ON event_consumers(workspace,active_delivery_token) WHERE active_delivery_token<>'';

CREATE TABLE IF NOT EXISTS event_consumer_checkpoints(
  id INTEGER PRIMARY KEY,
  workspace TEXT NOT NULL,
  consumer_name TEXT NOT NULL,
  checkpoint_token TEXT NOT NULL,
  delivery_token TEXT NOT NULL DEFAULT '',
  outcome TEXT NOT NULL DEFAULT 'processed' CHECK(outcome IN ('processed','dead_lettered')),
  request_revision INTEGER NOT NULL CHECK(request_revision>=1),
  previous_sequence INTEGER NOT NULL CHECK(previous_sequence>=0),
  through_sequence INTEGER NOT NULL CHECK(through_sequence>previous_sequence),
  result_revision INTEGER NOT NULL CHECK(result_revision=request_revision+1),
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  UNIQUE(workspace,consumer_name,checkpoint_token),
  UNIQUE(workspace,consumer_name,result_revision),
  FOREIGN KEY(workspace,consumer_name) REFERENCES event_consumers(workspace,name)
) STRICT;
CREATE INDEX IF NOT EXISTS event_consumer_checkpoints_sequence_idx
  ON event_consumer_checkpoints(workspace,consumer_name,through_sequence);

CREATE TABLE IF NOT EXISTS event_consumer_deliveries(
  id INTEGER PRIMARY KEY,
  workspace TEXT NOT NULL,
  consumer_name TEXT NOT NULL,
  delivery_token TEXT NOT NULL,
  owner TEXT NOT NULL,
  request_token TEXT NOT NULL,
  request_revision INTEGER NOT NULL CHECK(request_revision>=1),
  previous_sequence INTEGER NOT NULL CHECK(previous_sequence>=0),
  through_sequence INTEGER NOT NULL CHECK(through_sequence>previous_sequence),
  attempt INTEGER NOT NULL CHECK(attempt>=1),
  requested_limit INTEGER NOT NULL CHECK(requested_limit BETWEEN 1 AND 1000),
  lease_seconds INTEGER NOT NULL CHECK(lease_seconds BETWEEN 1 AND 86400),
  preceding_dead_lettered_batches INTEGER NOT NULL DEFAULT 0
    CHECK(preceding_dead_lettered_batches>=0),
  preceding_dead_lettered_events INTEGER NOT NULL DEFAULT 0
    CHECK(preceding_dead_lettered_events>=0),
  state TEXT NOT NULL DEFAULT 'active' CHECK(state IN ('active','settled','expired')),
  acquired_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  expires_at TEXT NOT NULL,
  retry_delay_seconds INTEGER NOT NULL DEFAULT 0
    CHECK(retry_delay_seconds BETWEEN 0 AND 86400),
  retry_not_before TEXT NOT NULL CHECK(retry_not_before>=expires_at),
  rejection_token TEXT NOT NULL DEFAULT '',
  rejection_disposition TEXT NOT NULL DEFAULT ''
    CHECK(rejection_disposition IN ('','retry','dead_letter')),
  rejection_reason TEXT NOT NULL DEFAULT '',
  rejected_at TEXT NOT NULL DEFAULT '',
  finished_at TEXT NOT NULL DEFAULT '',
  CHECK((rejection_token='' AND rejection_disposition='' AND rejection_reason='' AND
         rejected_at='') OR
        (rejection_token<>'' AND rejection_disposition<>'' AND rejection_reason<>'' AND
         rejected_at<>'')),
  UNIQUE(workspace,consumer_name,delivery_token),
  UNIQUE(workspace,consumer_name,request_token),
  FOREIGN KEY(workspace,consumer_name) REFERENCES event_consumers(workspace,name)
) STRICT;
CREATE INDEX IF NOT EXISTS event_consumer_deliveries_state_idx
  ON event_consumer_deliveries(workspace,consumer_name,state,id);
CREATE UNIQUE INDEX IF NOT EXISTS event_consumer_deliveries_rejection_token_idx
  ON event_consumer_deliveries(workspace,consumer_name,rejection_token)
  WHERE rejection_token<>'';

CREATE TABLE IF NOT EXISTS event_consumer_dead_letters(
  id INTEGER PRIMARY KEY,
  workspace TEXT NOT NULL,
  consumer_name TEXT NOT NULL,
  delivery_token TEXT NOT NULL,
  previous_sequence INTEGER NOT NULL CHECK(previous_sequence>=0),
  through_sequence INTEGER NOT NULL CHECK(through_sequence>previous_sequence),
  delivery_attempts INTEGER NOT NULL CHECK(delivery_attempts>=1),
  event_count INTEGER NOT NULL CHECK(event_count BETWEEN 1 AND 1000),
  reason TEXT NOT NULL CHECK(reason<>''),
  status TEXT NOT NULL DEFAULT 'open' CHECK(status IN ('open','redriven')),
  redrive_token TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  redriven_at TEXT NOT NULL DEFAULT '',
  CHECK((status='open' AND redrive_token='' AND redriven_at='') OR
        (status='redriven' AND redrive_token<>'' AND redriven_at<>'')),
  UNIQUE(workspace,consumer_name,delivery_token),
  FOREIGN KEY(workspace,consumer_name) REFERENCES event_consumers(workspace,name),
  FOREIGN KEY(workspace,consumer_name,delivery_token)
    REFERENCES event_consumer_deliveries(workspace,consumer_name,delivery_token)
) STRICT;
CREATE INDEX IF NOT EXISTS event_consumer_dead_letters_consumer_idx
  ON event_consumer_dead_letters(workspace,consumer_name,id);
CREATE UNIQUE INDEX IF NOT EXISTS event_consumer_dead_letters_redrive_token_idx
  ON event_consumer_dead_letters(workspace,consumer_name,redrive_token) WHERE redrive_token<>'';

CREATE TABLE IF NOT EXISTS event_consumer_dead_letter_quarantines(
  id INTEGER PRIMARY KEY,
  workspace TEXT NOT NULL,
  consumer_name TEXT NOT NULL,
  dead_letter_id INTEGER NOT NULL,
  quarantine_token TEXT NOT NULL CHECK(quarantine_token<>''),
  quarantined_by TEXT NOT NULL CHECK(quarantined_by<>''),
  reason TEXT NOT NULL CHECK(reason<>''),
  quarantined_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  UNIQUE(workspace,consumer_name,dead_letter_id),
  UNIQUE(workspace,consumer_name,quarantine_token),
  FOREIGN KEY(workspace,consumer_name) REFERENCES event_consumers(workspace,name),
  FOREIGN KEY(dead_letter_id) REFERENCES event_consumer_dead_letters(id)
) STRICT;
CREATE INDEX IF NOT EXISTS event_consumer_dead_letter_quarantines_consumer_idx
  ON event_consumer_dead_letter_quarantines(workspace,consumer_name,id);

CREATE TABLE IF NOT EXISTS event_consumer_lifecycle_epochs(
  workspace TEXT PRIMARY KEY,
  activated_at TEXT NOT NULL,
  event_high_watermark INTEGER NOT NULL CHECK(event_high_watermark>=0)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS event_consumer_dead_letter_lifecycle_events(
  workspace TEXT NOT NULL,
  consumer_name TEXT NOT NULL,
  dead_letter_id INTEGER NOT NULL,
  transition TEXT NOT NULL
    CHECK(transition IN ('dead_lettered','redriven','quarantined')),
  event_sequence INTEGER,
  transition_at TEXT NOT NULL,
  legacy INTEGER NOT NULL DEFAULT 0 CHECK(legacy IN (0,1)),
  PRIMARY KEY(workspace,consumer_name,dead_letter_id,transition),
  UNIQUE(event_sequence),
  FOREIGN KEY(workspace,consumer_name) REFERENCES event_consumers(workspace,name),
  FOREIGN KEY(dead_letter_id) REFERENCES event_consumer_dead_letters(id),
  FOREIGN KEY(event_sequence) REFERENCES agent_events(sequence),
  CHECK((legacy=1 AND event_sequence IS NULL) OR
        (legacy=0 AND event_sequence IS NOT NULL AND event_sequence>0))
) WITHOUT ROWID, STRICT;
CREATE INDEX IF NOT EXISTS event_consumer_dead_letter_lifecycle_consumer_idx
  ON event_consumer_dead_letter_lifecycle_events(workspace,consumer_name,dead_letter_id);

CREATE TABLE IF NOT EXISTS event_consumer_redrive_events(
  workspace TEXT NOT NULL,
  consumer_name TEXT NOT NULL,
  dead_letter_id INTEGER NOT NULL,
  original_sequence INTEGER NOT NULL,
  redriven_sequence INTEGER NOT NULL,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  PRIMARY KEY(workspace,consumer_name,dead_letter_id,original_sequence),
  UNIQUE(workspace,consumer_name,dead_letter_id,redriven_sequence),
  FOREIGN KEY(dead_letter_id) REFERENCES event_consumer_dead_letters(id),
  FOREIGN KEY(original_sequence) REFERENCES agent_events(sequence),
  FOREIGN KEY(redriven_sequence) REFERENCES agent_events(sequence)
) WITHOUT ROWID, STRICT;
CREATE UNIQUE INDEX IF NOT EXISTS event_consumer_redrive_events_target_idx
  ON event_consumer_redrive_events(redriven_sequence);

CREATE TABLE IF NOT EXISTS event_consumer_redrive_budget_charges(
  id INTEGER PRIMARY KEY,
  workspace TEXT NOT NULL,
  consumer_name TEXT NOT NULL,
  dead_letter_id INTEGER NOT NULL,
  redrive_token TEXT NOT NULL,
  event_count INTEGER NOT NULL CHECK(event_count BETWEEN 1 AND 1000),
  refilled_token_millis INTEGER NOT NULL CHECK(refilled_token_millis>=event_count*1000),
  result_token_millis INTEGER NOT NULL
    CHECK(result_token_millis=refilled_token_millis-event_count*1000),
  refilled_at TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  UNIQUE(workspace,consumer_name,dead_letter_id),
  UNIQUE(workspace,consumer_name,redrive_token),
  FOREIGN KEY(workspace,consumer_name) REFERENCES event_consumers(workspace,name),
  FOREIGN KEY(dead_letter_id) REFERENCES event_consumer_dead_letters(id)
) STRICT;
CREATE INDEX IF NOT EXISTS event_consumer_redrive_budget_charges_consumer_idx
  ON event_consumer_redrive_budget_charges(workspace,consumer_name,id);

CREATE TABLE IF NOT EXISTS episodes(
  id INTEGER PRIMARY KEY,
  workspace TEXT NOT NULL,
  idempotency_key TEXT NOT NULL,
  run_id TEXT NOT NULL,
  task_id TEXT NOT NULL DEFAULT '',
  agent_id TEXT NOT NULL,
  source_id TEXT NOT NULL,
  observation_id INTEGER NOT NULL CHECK(observation_id > 0),
  episode_type TEXT NOT NULL DEFAULT 'work',
  evidence_status TEXT NOT NULL DEFAULT 'unverified'
    CHECK(evidence_status IN ('unverified','verified')),
  evidence_content_hash TEXT NOT NULL DEFAULT '',
  evidence_verified_at TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  UNIQUE(workspace,idempotency_key),
  FOREIGN KEY(workspace,run_id) REFERENCES runs(workspace,id),
  FOREIGN KEY(workspace,agent_id) REFERENCES agents(workspace,id),
  CHECK((evidence_status='unverified' AND evidence_content_hash='' AND evidence_verified_at='') OR
        (evidence_status='verified' AND length(evidence_content_hash)=64 AND
         evidence_verified_at<>''))
) STRICT;
CREATE INDEX IF NOT EXISTS episodes_task_idx ON episodes(workspace,task_id,id);
CREATE INDEX IF NOT EXISTS episodes_source_idx ON episodes(workspace,source_id,observation_id);

CREATE TABLE IF NOT EXISTS temporal_clocks(
  workspace TEXT PRIMARY KEY,
  last_sequence INTEGER NOT NULL CHECK(last_sequence >= 0)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS facts(
  id INTEGER PRIMARY KEY,
  workspace TEXT NOT NULL,
  subject TEXT NOT NULL,
  predicate TEXT NOT NULL,
  object TEXT NOT NULL,
  valid_from TEXT NOT NULL,
  valid_to TEXT,
  transaction_from TEXT NOT NULL,
  transaction_to TEXT,
  transaction_from_sequence INTEGER NOT NULL CHECK(transaction_from_sequence > 0),
  transaction_to_sequence INTEGER,
  evidence_source_id TEXT NOT NULL DEFAULT '',
  evidence_observation_id INTEGER NOT NULL DEFAULT 0 CHECK(evidence_observation_id>=0),
  evidence_content_hash TEXT NOT NULL DEFAULT '',
  evidence_status TEXT NOT NULL DEFAULT 'none'
    CHECK(evidence_status IN ('none','unverified','verified')),
  idempotency_key TEXT NOT NULL DEFAULT '',
  request_valid_from TEXT NOT NULL DEFAULT '',
  confidence REAL NOT NULL CHECK(confidence BETWEEN 0.0 AND 1.0),
  supersedes_fact_id INTEGER REFERENCES facts(id),
  CHECK(valid_to IS NULL OR valid_from < valid_to),
  CHECK(transaction_to IS NULL OR transaction_from <= transaction_to),
  CHECK(transaction_to_sequence IS NULL OR
        transaction_from_sequence < transaction_to_sequence),
  CHECK((evidence_status='none' AND evidence_source_id='' AND evidence_observation_id=0 AND
         evidence_content_hash='') OR
        (evidence_status='unverified' AND evidence_source_id<>'' AND
         evidence_observation_id=0 AND evidence_content_hash='') OR
        (evidence_status='verified' AND evidence_source_id<>'' AND
         evidence_observation_id>0 AND length(evidence_content_hash)=64)),
  CHECK((idempotency_key='' AND request_valid_from='') OR
        (idempotency_key<>'' AND (request_valid_from='' OR request_valid_from=valid_from)))
) STRICT;
CREATE INDEX IF NOT EXISTS facts_lookup_idx
  ON facts(workspace,subject,predicate,valid_from,valid_to,transaction_from,transaction_to);
CREATE UNIQUE INDEX IF NOT EXISTS facts_current_interval_unique
  ON facts(workspace,subject,predicate,valid_from) WHERE transaction_to IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS facts_idempotency_idx
  ON facts(workspace,idempotency_key) WHERE idempotency_key<>'';
)sql";

constexpr std::string_view kConsumerStateRevisionSchema = R"sql(
CREATE TABLE IF NOT EXISTS event_consumer_state_revisions(
  workspace TEXT PRIMARY KEY,
  revision INTEGER NOT NULL CHECK(revision>=1),
  updated_at TEXT NOT NULL
) WITHOUT ROWID, STRICT;

CREATE TRIGGER IF NOT EXISTS event_consumers_state_revision_insert
AFTER INSERT ON event_consumers BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(NEW.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;
CREATE TRIGGER IF NOT EXISTS event_consumers_state_revision_update
AFTER UPDATE ON event_consumers BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(OLD.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  SELECT NEW.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now')
  WHERE NEW.workspace<>OLD.workspace
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;
CREATE TRIGGER IF NOT EXISTS event_consumers_state_revision_delete
AFTER DELETE ON event_consumers BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(OLD.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;

CREATE TRIGGER IF NOT EXISTS event_consumer_dead_letters_state_revision_insert
AFTER INSERT ON event_consumer_dead_letters BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(NEW.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;
CREATE TRIGGER IF NOT EXISTS event_consumer_dead_letters_state_revision_update
AFTER UPDATE ON event_consumer_dead_letters BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(OLD.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  SELECT NEW.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now')
  WHERE NEW.workspace<>OLD.workspace
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;
CREATE TRIGGER IF NOT EXISTS event_consumer_dead_letters_state_revision_delete
AFTER DELETE ON event_consumer_dead_letters BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(OLD.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;

CREATE TRIGGER IF NOT EXISTS event_consumer_quarantines_state_revision_insert
AFTER INSERT ON event_consumer_dead_letter_quarantines BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(NEW.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;
CREATE TRIGGER IF NOT EXISTS event_consumer_quarantines_state_revision_update
AFTER UPDATE ON event_consumer_dead_letter_quarantines BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(OLD.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  SELECT NEW.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now')
  WHERE NEW.workspace<>OLD.workspace
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;
CREATE TRIGGER IF NOT EXISTS event_consumer_quarantines_state_revision_delete
AFTER DELETE ON event_consumer_dead_letter_quarantines BEGIN
  INSERT INTO event_consumer_state_revisions(workspace,revision,updated_at)
  VALUES(OLD.workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now'))
  ON CONFLICT(workspace) DO UPDATE SET
    revision=event_consumer_state_revisions.revision+1,
    updated_at=max(event_consumer_state_revisions.updated_at,excluded.updated_at);
END;
)sql";

[[nodiscard]] Status step_done(Statement &statement) {
  ATX_TRY(const auto step, statement.step());
  if (step != Statement::Step::Done) {
    return Err(ErrorCode::Internal, "statement unexpectedly returned a row");
  }
  return Ok();
}

[[nodiscard]] Status ensure_wal(Database &database) {
  for (usize attempt = 0; attempt < 32; ++attempt) {
    {
      auto query = database.prepare("PRAGMA journal_mode");
      if (query) {
        auto step = query->step();
        if (step && *step == Statement::Step::Row) {
          const std::string_view mode = query->column_text(0);
          if (mode == "wal" || mode == "memory") {
            return Ok();
          }
        } else if (!step && step.error().code() != ErrorCode::Unavailable) {
          return Err(std::move(step).error());
        }
      } else if (query.error().code() != ErrorCode::Unavailable) {
        return Err(std::move(query).error());
      }
    }
    auto changed = database.pragma("journal_mode", "WAL");
    if (changed) {
      return Ok();
    }
    if (changed.error().code() != ErrorCode::Unavailable) {
      return Err(std::move(changed).error());
    }
    std::this_thread::yield();
  }
  return Err(ErrorCode::Unavailable, "could not establish WAL journal mode after contention");
}

[[nodiscard]] bool valid_utf8(std::string_view text) {
  usize index{};
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }
    usize continuation_count{};
    unsigned int codepoint{};
    if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation_count = 1;
      codepoint = lead & 0x1FU;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation_count = 2;
      codepoint = lead & 0x0FU;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation_count = 3;
      codepoint = lead & 0x07U;
    } else {
      return false;
    }
    if (index + continuation_count >= text.size()) {
      return false;
    }
    for (usize offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }
    if ((continuation_count == 2 && codepoint < 0x800U) ||
        (continuation_count == 3 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

[[nodiscard]] bool valid_field(std::string_view value, usize maximum, bool allow_empty = true) {
  return value.size() <= maximum && (allow_empty || !value.empty()) &&
         value.find('\0') == std::string_view::npos && valid_utf8(value);
}

[[nodiscard]] bool valid_sha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool canonical_utc_timestamp(std::string_view value) {
  if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z') {
    return false;
  }
  constexpr std::array<usize, 7> separators{4, 7, 10, 13, 16, 19, 23};
  for (usize index = 0; index < value.size(); ++index) {
    if (std::find(separators.begin(), separators.end(), index) != separators.end()) {
      continue;
    }
    if (value[index] < '0' || value[index] > '9') {
      return false;
    }
  }
  const auto number = [&](usize offset, usize length) {
    i64 result{};
    for (usize index = 0; index < length; ++index) {
      result = result * 10 + (value[offset + index] - '0');
    }
    return result;
  };
  const i64 month = number(5, 2);
  const i64 day = number(8, 2);
  const i64 hour = number(11, 2);
  const i64 minute = number(14, 2);
  const i64 second = number(17, 2);
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  return static_cast<bool>(atx::core::time::from_iso8601(value));
}

[[nodiscard]] Result<std::string> scalar_text(Database &database, std::string_view sql) {
  ATX_TRY(auto statement, database.prepare(sql));
  ATX_TRY(const auto step, statement.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::NotFound, "query returned no row");
  }
  return Ok(std::string{statement.column_text(0)});
}

[[nodiscard]] Result<std::string> new_id(Database &database, std::string_view prefix) {
  ATX_TRY(auto statement, database.prepare("SELECT ?1 || lower(hex(randomblob(16)))"));
  ATX_TRY_VOID(statement.bind(1, prefix));
  ATX_TRY(const auto step, statement.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "could not generate an identifier");
  }
  return Ok(std::string{statement.column_text(0)});
}

[[nodiscard]] Result<i64> next_temporal_sequence(Database &database, std::string_view workspace) {
  ATX_TRY(auto statement,
          database.prepare("INSERT INTO temporal_clocks(workspace,last_sequence) VALUES(?1,1) "
                           "ON CONFLICT(workspace) DO UPDATE SET "
                           "last_sequence=last_sequence+1 RETURNING last_sequence"));
  ATX_TRY_VOID(statement.bind(1, workspace));
  ATX_TRY(const auto step, statement.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "could not advance the temporal sequence");
  }
  return Ok(statement.column_int(0));
}

[[nodiscard]] Result<std::string> monotonic_wall_time(Database &database,
                                                      std::string_view workspace) {
  ATX_TRY(auto statement,
          database.prepare("SELECT max(strftime('%Y-%m-%dT%H:%M:%fZ','now'),COALESCE(("
                           "SELECT max(transaction_from) FROM facts WHERE workspace=?1),''))"));
  ATX_TRY_VOID(statement.bind(1, workspace));
  ATX_TRY(const auto step, statement.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "could not read the temporal wall clock");
  }
  return Ok(std::string{statement.column_text(0)});
}

[[nodiscard]] Result<TaskRecord> read_task(Statement &statement) {
  TaskRecord task;
  task.id = statement.column_text(0);
  task.run_id = statement.column_text(1);
  task.title = statement.column_text(2);
  task.description = statement.column_text(3);
  task.status = statement.column_text(4);
  task.priority = statement.column_int(5);
  task.max_attempts = statement.column_int(6);
  task.attempts = statement.column_int(7);
  task.revision = statement.column_int(8);
  task.lease_owner = statement.column_text(9);
  task.lease_token = statement.column_text(10);
  task.lease_expires_at = statement.column_text(11);
  task.result_source_id = statement.column_text(12);
  task.result_observation_id = statement.column_int(13);
  task.result_content_hash = statement.column_text(14);
  task.result_evidence_status = statement.column_text(15);
  task.last_error = statement.column_text(16);
  task.created_at = statement.column_text(17);
  task.updated_at = statement.column_text(18);
  return Ok(std::move(task));
}

constexpr std::string_view kTaskColumns =
    "id,run_id,title,description,status,priority,max_attempts,attempts,revision,"
    "lease_owner,lease_token,lease_expires_at,result_source_id,result_observation_id,"
    "result_content_hash,result_evidence_status,last_error,created_at,updated_at";

[[nodiscard]] Result<TaskRecord> find_task(Database &database, std::string_view workspace,
                                           std::string_view task_id) {
  const std::string sql =
      "SELECT " + std::string{kTaskColumns} + " FROM tasks WHERE workspace=?1 AND id=?2";
  ATX_TRY(auto statement, database.prepare(sql));
  ATX_TRY_VOID(statement.bind(1, workspace));
  ATX_TRY_VOID(statement.bind(2, task_id));
  ATX_TRY(const auto step, statement.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::NotFound, "task was not found");
  }
  return read_task(statement);
}

[[nodiscard]] Result<RunRecord> read_run(Statement &statement) {
  RunRecord run;
  run.id = statement.column_text(0);
  run.objective = statement.column_text(1);
  run.status = statement.column_text(2);
  run.revision = statement.column_int(3);
  run.created_at = statement.column_text(4);
  run.updated_at = statement.column_text(5);
  return Ok(std::move(run));
}

[[nodiscard]] Result<AgentRecord> read_agent(Statement &statement) {
  AgentRecord agent;
  agent.id = statement.column_text(0);
  agent.run_id = statement.column_text(1);
  agent.role = statement.column_text(2);
  agent.capabilities = statement.column_text(3);
  agent.status = statement.column_text(4);
  agent.revision = statement.column_int(5);
  agent.last_heartbeat_at = statement.column_text(6);
  return Ok(std::move(agent));
}

constexpr std::string_view kEventConsumerColumns =
    "name,subject_filter,max_delivery_attempts,retry_backoff_seconds,retry_backoff_max_seconds,"
    "retry_jitter,redrive_rate_per_second,redrive_burst_events,redrive_token_millis,"
    "redrive_refilled_at,max_redrive_count,self_control_event_cutoff_sequence,start_sequence,"
    "cursor_sequence,revision,created_at,updated_at";

constexpr std::string_view kQualifiedEventConsumerColumns =
    "c.name,c.subject_filter,c.max_delivery_attempts,c.retry_backoff_seconds,"
    "c.retry_backoff_max_seconds,c.retry_jitter,c.redrive_rate_per_second,"
    "c.redrive_burst_events,c.redrive_token_millis,c.redrive_refilled_at,c.max_redrive_count,"
    "c.self_control_event_cutoff_sequence,c.start_sequence,c.cursor_sequence,c.revision,"
    "c.created_at,c.updated_at";

[[nodiscard]] Result<EventConsumerRecord> read_event_consumer(Statement &statement) {
  EventConsumerRecord consumer;
  consumer.name = statement.column_text(0);
  consumer.subject_filter = statement.column_text(1);
  consumer.max_delivery_attempts = statement.column_int(2);
  consumer.retry_backoff_seconds = statement.column_int(3);
  consumer.retry_backoff_max_seconds = statement.column_int(4);
  consumer.retry_jitter = statement.column_text(5);
  consumer.redrive_rate_per_second = statement.column_int(6);
  consumer.redrive_burst_events = statement.column_int(7);
  consumer.redrive_token_millis = statement.column_int(8);
  consumer.redrive_refilled_at = statement.column_text(9);
  consumer.max_redrive_count = statement.column_int(10);
  consumer.self_control_event_cutoff_sequence = statement.column_int(11);
  consumer.start_sequence = statement.column_int(12);
  consumer.cursor_sequence = statement.column_int(13);
  consumer.revision = statement.column_int(14);
  consumer.created_at = statement.column_text(15);
  consumer.updated_at = statement.column_text(16);
  return Ok(std::move(consumer));
}

[[nodiscard]] Result<EventConsumerRecord>
find_event_consumer(Database &database, std::string_view workspace, std::string_view name) {
  ATX_TRY(auto query, database.prepare("SELECT " + std::string{kEventConsumerColumns} +
                                       " FROM event_consumers WHERE workspace=?1 AND name=?2"));
  ATX_TRY_VOID(query.bind(1, workspace));
  ATX_TRY_VOID(query.bind(2, name));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::NotFound, "event consumer was not found");
  }
  return read_event_consumer(query);
}

[[nodiscard]] Result<EventConsumerStatus> read_event_consumer_status(Statement &query) {
  EventConsumerStatus status;
  ATX_TRY(status.consumer, read_event_consumer(query));
  status.event_high_watermark = query.column_int(17);
  status.pending_visible_event_count = query.column_int(18);
  status.first_pending_visible_sequence = query.column_int(19);
  status.last_pending_visible_sequence = query.column_int(20);
  status.oldest_pending_visible_event_at = query.column_text(21);
  status.delivery_head_state = query.column_text(22);
  status.delivery_head_owner = query.column_text(23);
  status.delivery_head_attempt = query.column_int(24);
  status.delivery_head_through_sequence = query.column_int(25);
  status.delivery_head_expires_at = query.column_text(26);
  status.delivery_head_retry_at = query.column_text(27);
  status.delivery_head_event_count = query.column_int(28);
  if (status.delivery_head_event_count < 0 ||
      status.delivery_head_event_count > status.pending_visible_event_count ||
      (status.delivery_head_state == "idle" && status.delivery_head_event_count != 0) ||
      (status.delivery_head_state != "idle" && status.delivery_head_event_count == 0)) {
    return Err(ErrorCode::Internal, "event consumer status head partition is invalid");
  }
  status.queued_visible_event_count =
      status.pending_visible_event_count - status.delivery_head_event_count;
  if (status.delivery_head_state == "idle") {
    status.available_visible_event_count = status.pending_visible_event_count;
  } else if (status.delivery_head_state == "in_flight" ||
             status.delivery_head_state == "retry_backoff") {
    status.available_visible_event_count = 0;
  } else if (status.delivery_head_state == "redelivery_ready") {
    status.available_visible_event_count = status.delivery_head_event_count;
  } else if (status.delivery_head_state == "dead_letter_ready") {
    status.available_visible_event_count = status.queued_visible_event_count;
  } else {
    return Err(ErrorCode::Internal, "event consumer status head state is invalid");
  }
  status.retained_dead_letter_count = query.column_int(29);
  status.open_dead_letter_count = query.column_int(30);
  status.open_dead_letter_event_count = query.column_int(31);
  status.oldest_open_dead_letter_at = query.column_text(32);
  status.redriven_dead_letter_count = query.column_int(33);
  status.quarantined_dead_letter_count = query.column_int(34);
  status.observed_at = query.column_text(35);
  status.consumer_state_revision = query.column_int(36);
  if (status.consumer_state_revision <= 0) {
    return Err(ErrorCode::Internal, "event consumer status state revision is invalid");
  }
  if (status.delivery_head_state == "in_flight") {
    status.next_dynamic_transition_at = status.delivery_head_expires_at;
  } else if (status.delivery_head_state == "retry_backoff") {
    status.next_dynamic_transition_at = status.delivery_head_retry_at;
  }
  if (status.retained_dead_letter_count < 0 || status.open_dead_letter_count < 0 ||
      status.open_dead_letter_event_count < 0 || status.redriven_dead_letter_count < 0 ||
      status.quarantined_dead_letter_count < 0 ||
      status.open_dead_letter_count + status.redriven_dead_letter_count +
              status.quarantined_dead_letter_count !=
          status.retained_dead_letter_count ||
      (status.open_dead_letter_count == 0) != status.oldest_open_dead_letter_at.empty()) {
    return Err(ErrorCode::Internal, "event consumer status dead-letter partition is invalid");
  }
  return Ok(std::move(status));
}

struct EventDeliveryState {
  std::string delivery_token;
  std::string owner;
  std::string request_token;
  i64 request_revision{};
  i64 previous_sequence{};
  i64 through_sequence{};
  i64 attempt{};
  i64 requested_limit{};
  i64 lease_seconds{};
  i64 preceding_dead_lettered_batches{};
  i64 preceding_dead_lettered_events{};
  std::string state;
  std::string acquired_at;
  std::string expires_at;
  i64 retry_delay_seconds{};
  std::string retry_not_before;
  std::string rejection_token;
  std::string rejection_disposition;
  std::string rejection_reason;
  std::string rejected_at;
};

[[nodiscard]] EventDeliveryState read_event_delivery(Statement &statement) {
  EventDeliveryState delivery;
  delivery.delivery_token = statement.column_text(0);
  delivery.owner = statement.column_text(1);
  delivery.request_token = statement.column_text(2);
  delivery.request_revision = statement.column_int(3);
  delivery.previous_sequence = statement.column_int(4);
  delivery.through_sequence = statement.column_int(5);
  delivery.attempt = statement.column_int(6);
  delivery.requested_limit = statement.column_int(7);
  delivery.lease_seconds = statement.column_int(8);
  delivery.preceding_dead_lettered_batches = statement.column_int(9);
  delivery.preceding_dead_lettered_events = statement.column_int(10);
  delivery.state = statement.column_text(11);
  delivery.acquired_at = statement.column_text(12);
  delivery.expires_at = statement.column_text(13);
  delivery.retry_delay_seconds = statement.column_int(14);
  delivery.retry_not_before = statement.column_text(15);
  delivery.rejection_token = statement.column_text(16);
  delivery.rejection_disposition = statement.column_text(17);
  delivery.rejection_reason = statement.column_text(18);
  delivery.rejected_at = statement.column_text(19);
  return delivery;
}

constexpr std::string_view kEventDeliveryColumns =
    "delivery_token,owner,request_token,request_revision,previous_sequence,through_sequence,"
    "attempt,requested_limit,lease_seconds,preceding_dead_lettered_batches,"
    "preceding_dead_lettered_events,state,acquired_at,expires_at,retry_delay_seconds,"
    "retry_not_before,rejection_token,rejection_disposition,rejection_reason,rejected_at";

[[nodiscard]] Result<EventDeliveryState> find_event_delivery(Database &database,
                                                             std::string_view workspace,
                                                             std::string_view name,
                                                             std::string_view delivery_token) {
  ATX_TRY(auto query, database.prepare("SELECT " + std::string{kEventDeliveryColumns} +
                                       " FROM event_consumer_deliveries WHERE workspace=?1 AND "
                                       "consumer_name=?2 AND delivery_token=?3"));
  ATX_TRY_VOID(query.bind(1, workspace));
  ATX_TRY_VOID(query.bind(2, name));
  ATX_TRY_VOID(query.bind(3, delivery_token));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::NotFound, "event consumer delivery was not found");
  }
  return Ok(read_event_delivery(query));
}

[[nodiscard]] Result<std::vector<TaskEvent>>
events_between(Database &database, std::string_view workspace, i64 previous_sequence,
               i64 through_sequence, std::string_view subject, std::string_view consumer_name = {},
               i64 self_control_cutoff = 0) {
  const std::string sql =
      "SELECT sequence,root_sequence,redrive_count,run_id,task_id,agent_id,event_type,subject,"
      "payload,created_at FROM "
      "agent_events WHERE workspace=?1 AND sequence>?2 AND sequence<=?3 AND (?4='' OR "
      "subject=?4) AND (?5='' OR sequence<=?6 OR substr(event_type,1,9)<>'consumer.' OR "
      "subject<>?7) ORDER BY sequence LIMIT 1001";
  ATX_TRY(auto query, database.prepare(sql));
  ATX_TRY_VOID(query.bind(1, workspace));
  ATX_TRY_VOID(query.bind(2, previous_sequence));
  ATX_TRY_VOID(query.bind(3, through_sequence));
  ATX_TRY_VOID(query.bind(4, subject));
  ATX_TRY_VOID(query.bind(5, consumer_name));
  ATX_TRY_VOID(query.bind(6, self_control_cutoff));
  ATX_TRY_VOID(query.bind(7, "consumers/" + std::string{consumer_name}));
  std::vector<TaskEvent> events;
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    TaskEvent event;
    event.sequence = query.column_int(0);
    event.root_sequence = query.column_int(1);
    event.redrive_count = query.column_int(2);
    event.run_id = query.column_text(3);
    event.task_id = query.column_text(4);
    event.agent_id = query.column_text(5);
    event.type = query.column_text(6);
    event.subject = query.column_text(7);
    event.payload = query.column_text(8);
    event.created_at = query.column_text(9);
    events.push_back(std::move(event));
  }
  if (events.empty() || events.size() > 1'000 || events.back().sequence != through_sequence) {
    return Err(ErrorCode::Internal, "event delivery range is invalid");
  }
  return Ok(std::move(events));
}

[[nodiscard]] Result<std::vector<TaskEvent>>
events_after_filtered(Database &database, std::string_view workspace, i64 sequence, usize limit,
                      std::string_view subject, std::string_view consumer_name = {},
                      i64 self_control_cutoff = 0) {
  ATX_TRY(auto query,
          database.prepare("SELECT sequence,root_sequence,redrive_count,run_id,task_id,agent_id,"
                           "event_type,subject,payload,created_at FROM agent_events WHERE "
                           "workspace=?1 AND sequence>?2 AND (?3='' OR subject=?3) AND (?4='' OR "
                           "sequence<=?5 OR substr(event_type,1,9)<>'consumer.' OR subject<>?6) "
                           "ORDER BY sequence LIMIT ?7"));
  ATX_TRY_VOID(query.bind(1, workspace));
  ATX_TRY_VOID(query.bind(2, sequence));
  ATX_TRY_VOID(query.bind(3, subject));
  ATX_TRY_VOID(query.bind(4, consumer_name));
  ATX_TRY_VOID(query.bind(5, self_control_cutoff));
  ATX_TRY_VOID(query.bind(6, "consumers/" + std::string{consumer_name}));
  ATX_TRY_VOID(query.bind(7, static_cast<i64>(limit)));
  std::vector<TaskEvent> result;
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    TaskEvent event;
    event.sequence = query.column_int(0);
    event.root_sequence = query.column_int(1);
    event.redrive_count = query.column_int(2);
    event.run_id = query.column_text(3);
    event.task_id = query.column_text(4);
    event.agent_id = query.column_text(5);
    event.type = query.column_text(6);
    event.subject = query.column_text(7);
    event.payload = query.column_text(8);
    event.created_at = query.column_text(9);
    result.push_back(std::move(event));
  }
  return Ok(std::move(result));
}

[[nodiscard]] Result<std::vector<TaskEvent>> redriven_events_for(Database &database,
                                                                 std::string_view workspace,
                                                                 std::string_view consumer_name,
                                                                 i64 dead_letter_id) {
  ATX_TRY(auto query,
          database.prepare("SELECT e.sequence,e.root_sequence,e.redrive_count,e.run_id,e.task_id,"
                           "e.agent_id,e.event_type,e.subject,e.payload,e.created_at FROM "
                           "event_consumer_redrive_events r JOIN "
                           "agent_events e ON e.sequence=r.redriven_sequence WHERE r.workspace=?1 "
                           "AND r.consumer_name=?2 AND r.dead_letter_id=?3 ORDER BY "
                           "r.original_sequence"));
  ATX_TRY_VOID(query.bind(1, workspace));
  ATX_TRY_VOID(query.bind(2, consumer_name));
  ATX_TRY_VOID(query.bind(3, dead_letter_id));
  std::vector<TaskEvent> events;
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    TaskEvent event;
    event.sequence = query.column_int(0);
    event.root_sequence = query.column_int(1);
    event.redrive_count = query.column_int(2);
    event.run_id = query.column_text(3);
    event.task_id = query.column_text(4);
    event.agent_id = query.column_text(5);
    event.type = query.column_text(6);
    event.subject = query.column_text(7);
    event.payload = query.column_text(8);
    event.created_at = query.column_text(9);
    events.push_back(std::move(event));
  }
  return Ok(std::move(events));
}

[[nodiscard]] Result<EventConsumerDeadLetter>
find_event_consumer_dead_letter(Database &database, std::string_view workspace,
                                const EventConsumerRecord &consumer, std::string_view name,
                                i64 dead_letter_id) {
  ATX_TRY(auto query,
          database.prepare(
              "SELECT delivery_token,previous_sequence,through_sequence,delivery_attempts,reason,"
              "status,redrive_token,created_at,redriven_at,(SELECT rejection_disposition FROM "
              "event_consumer_deliveries d WHERE d.workspace=l.workspace AND "
              "d.consumer_name=l.consumer_name AND d.delivery_token=l.delivery_token),(SELECT "
              "rejection_reason FROM event_consumer_deliveries d WHERE d.workspace=l.workspace "
              "AND d.consumer_name=l.consumer_name AND d.delivery_token=l.delivery_token),"
              "COALESCE((SELECT event_count FROM event_consumer_redrive_budget_charges b WHERE "
              "b.workspace=l.workspace AND b.consumer_name=l.consumer_name AND "
              "b.dead_letter_id=l.id),0),COALESCE((SELECT refilled_token_millis FROM "
              "event_consumer_redrive_budget_charges b WHERE b.workspace=l.workspace AND "
              "b.consumer_name=l.consumer_name AND b.dead_letter_id=l.id),0),COALESCE((SELECT "
              "result_token_millis FROM event_consumer_redrive_budget_charges b WHERE "
              "b.workspace=l.workspace AND b.consumer_name=l.consumer_name AND "
              "b.dead_letter_id=l.id),0),COALESCE((SELECT refilled_at FROM "
              "event_consumer_redrive_budget_charges b WHERE b.workspace=l.workspace AND "
              "b.consumer_name=l.consumer_name AND b.dead_letter_id=l.id),''),COALESCE((SELECT "
              "quarantine_token FROM event_consumer_dead_letter_quarantines q WHERE "
              "q.workspace=l.workspace AND q.consumer_name=l.consumer_name AND "
              "q.dead_letter_id=l.id),''),COALESCE((SELECT quarantined_by FROM "
              "event_consumer_dead_letter_quarantines q WHERE q.workspace=l.workspace AND "
              "q.consumer_name=l.consumer_name AND q.dead_letter_id=l.id),''),COALESCE((SELECT "
              "reason FROM event_consumer_dead_letter_quarantines q WHERE q.workspace=l.workspace "
              "AND q.consumer_name=l.consumer_name AND q.dead_letter_id=l.id),''),"
              "COALESCE((SELECT quarantined_at FROM event_consumer_dead_letter_quarantines q "
              "WHERE q.workspace=l.workspace AND q.consumer_name=l.consumer_name AND "
              "q.dead_letter_id=l.id),''),COALESCE((SELECT event_sequence FROM "
              "event_consumer_dead_letter_lifecycle_events x WHERE x.workspace=l.workspace AND "
              "x.consumer_name=l.consumer_name AND x.dead_letter_id=l.id AND "
              "x.transition='dead_lettered'),0),COALESCE((SELECT event_sequence FROM "
              "event_consumer_dead_letter_lifecycle_events x WHERE x.workspace=l.workspace AND "
              "x.consumer_name=l.consumer_name AND x.dead_letter_id=l.id AND "
              "x.transition='redriven'),0),COALESCE((SELECT event_sequence FROM "
              "event_consumer_dead_letter_lifecycle_events x WHERE x.workspace=l.workspace AND "
              "x.consumer_name=l.consumer_name AND x.dead_letter_id=l.id AND "
              "x.transition='quarantined'),0) FROM event_consumer_dead_letters l WHERE id=?1 AND "
              "workspace=?2 AND consumer_name=?3"));
  ATX_TRY_VOID(query.bind(1, dead_letter_id));
  ATX_TRY_VOID(query.bind(2, workspace));
  ATX_TRY_VOID(query.bind(3, name));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::NotFound, "event consumer dead letter was not found");
  }
  EventConsumerDeadLetter dead_letter;
  dead_letter.id = dead_letter_id;
  dead_letter.consumer_name = std::string{name};
  dead_letter.delivery_token = query.column_text(0);
  dead_letter.previous_sequence = query.column_int(1);
  dead_letter.through_sequence = query.column_int(2);
  dead_letter.delivery_attempts = query.column_int(3);
  dead_letter.reason = query.column_text(4);
  dead_letter.status = query.column_text(5);
  dead_letter.redrive_token = query.column_text(6);
  dead_letter.created_at = query.column_text(7);
  dead_letter.redriven_at = query.column_text(8);
  dead_letter.rejection_disposition = query.column_text(9);
  dead_letter.rejection_reason = query.column_text(10);
  dead_letter.redrive_budget_event_count = query.column_int(11);
  dead_letter.redrive_budget_before_millis = query.column_int(12);
  dead_letter.redrive_budget_after_millis = query.column_int(13);
  dead_letter.redrive_budget_refilled_at = query.column_text(14);
  dead_letter.quarantine_token = query.column_text(15);
  dead_letter.quarantined_by = query.column_text(16);
  dead_letter.quarantine_reason = query.column_text(17);
  dead_letter.quarantined_at = query.column_text(18);
  dead_letter.dead_lettered_event_sequence = query.column_int(19);
  dead_letter.redriven_event_sequence = query.column_int(20);
  dead_letter.quarantined_event_sequence = query.column_int(21);
  ATX_TRY_VOID(query.reset());
  const bool quarantined = !dead_letter.quarantine_token.empty();
  if ((!quarantined &&
       (!dead_letter.quarantined_by.empty() || !dead_letter.quarantine_reason.empty() ||
        !dead_letter.quarantined_at.empty())) ||
      (quarantined && (dead_letter.status != "open" || !dead_letter.redrive_token.empty() ||
                       !dead_letter.redriven_at.empty() ||
                       !valid_field(dead_letter.quarantine_token, kMaximumIdBytes, false) ||
                       !valid_field(dead_letter.quarantined_by, kMaximumIdBytes, false) ||
                       !valid_field(dead_letter.quarantine_reason, kMaximumTitleBytes, false) ||
                       !canonical_utc_timestamp(dead_letter.quarantined_at)))) {
    return Err(ErrorCode::Internal, "event consumer dead-letter quarantine audit is invalid");
  }
  if (quarantined) {
    dead_letter.status = "quarantined";
  }
  ATX_TRY(dead_letter.events,
          events_between(database, workspace, dead_letter.previous_sequence,
                         dead_letter.through_sequence, consumer.subject_filter, consumer.name,
                         consumer.self_control_event_cutoff_sequence));
  ATX_TRY(dead_letter.redriven_events,
          redriven_events_for(database, workspace, name, dead_letter.id));
  return Ok(std::move(dead_letter));
}

[[nodiscard]] Result<EventConsumerDelivery> make_event_delivery(Database &database,
                                                                std::string_view workspace,
                                                                EventConsumerRecord consumer,
                                                                EventDeliveryState state) {
  ATX_TRY(auto events, events_between(database, workspace, state.previous_sequence,
                                      state.through_sequence, consumer.subject_filter,
                                      consumer.name, consumer.self_control_event_cutoff_sequence));
  if (events.size() > static_cast<usize>(state.requested_limit)) {
    return Err(ErrorCode::Internal, "event delivery exceeds its requested batch limit");
  }
  EventConsumerDelivery delivery;
  delivery.consumer = std::move(consumer);
  delivery.delivery_token = std::move(state.delivery_token);
  delivery.owner = std::move(state.owner);
  delivery.request_token = std::move(state.request_token);
  delivery.previous_sequence = state.previous_sequence;
  delivery.through_sequence = state.through_sequence;
  delivery.attempt = state.attempt;
  delivery.retry_delay_seconds = state.retry_delay_seconds;
  delivery.acquired_at = std::move(state.acquired_at);
  delivery.expires_at = std::move(state.expires_at);
  delivery.retry_not_before = std::move(state.retry_not_before);
  delivery.dead_lettered_batches = state.preceding_dead_lettered_batches;
  delivery.dead_lettered_events = state.preceding_dead_lettered_events;
  delivery.events = std::move(events);
  return Ok(std::move(delivery));
}

[[nodiscard]] Result<EventConsumerRejection> make_event_rejection(Database &database,
                                                                  std::string_view workspace,
                                                                  std::string_view name,
                                                                  EventDeliveryState state) {
  if (state.rejection_token.empty() ||
      (state.rejection_disposition != "retry" && state.rejection_disposition != "dead_letter") ||
      state.rejection_reason.empty() || state.rejected_at.empty()) {
    return Err(ErrorCode::Internal, "event consumer rejection audit is incomplete");
  }
  ATX_TRY(auto consumer, find_event_consumer(database, workspace, name));
  ATX_TRY(auto dead_letter,
          database.prepare("SELECT id FROM event_consumer_dead_letters WHERE workspace=?1 AND "
                           "consumer_name=?2 AND delivery_token=?3"));
  ATX_TRY_VOID(dead_letter.bind(1, workspace));
  ATX_TRY_VOID(dead_letter.bind(2, name));
  ATX_TRY_VOID(dead_letter.bind(3, state.delivery_token));
  ATX_TRY(const auto dead_letter_step, dead_letter.step());
  EventConsumerRejection rejection;
  rejection.consumer = std::move(consumer);
  rejection.delivery_token = std::move(state.delivery_token);
  rejection.owner = std::move(state.owner);
  rejection.rejection_token = std::move(state.rejection_token);
  rejection.disposition = std::move(state.rejection_disposition);
  rejection.reason = std::move(state.rejection_reason);
  rejection.attempt = state.attempt;
  rejection.retry_delay_seconds = state.retry_delay_seconds;
  rejection.rejected_at = std::move(state.rejected_at);
  rejection.retry_not_before = std::move(state.retry_not_before);
  rejection.dead_lettered = dead_letter_step == Statement::Step::Row;
  rejection.dead_letter_id = rejection.dead_lettered ? dead_letter.column_int(0) : 0;
  return Ok(std::move(rejection));
}

[[nodiscard]] std::string event_subject(std::string_view type, std::string_view run_id,
                                        std::string_view task_id, std::string_view agent_id,
                                        std::string_view explicit_subject) {
  if (!explicit_subject.empty()) {
    return std::string{explicit_subject};
  }
  if (type.starts_with("task.") && !task_id.empty()) {
    return "tasks/" + std::string{task_id};
  }
  if (type.starts_with("agent.") && !agent_id.empty()) {
    return "agents/" + std::string{agent_id};
  }
  if (type.starts_with("run.") && !run_id.empty()) {
    return "runs/" + std::string{run_id};
  }
  return {};
}

[[nodiscard]] Result<i64>
insert_event(Database &database, std::string_view workspace, std::string_view type,
             std::string_view payload, std::string_view run_id, std::string_view task_id,
             std::string_view agent_id, std::string_view idempotency_key = {},
             std::string_view explicit_subject = {}, i64 root_sequence = 0, i64 redrive_count = 0) {
  if ((root_sequence == 0 && redrive_count != 0) || root_sequence < 0 || redrive_count < 0 ||
      (root_sequence > 0 && redrive_count == 0)) {
    return Err(ErrorCode::Internal, "event lineage insertion is invalid");
  }
  const std::string subject = event_subject(type, run_id, task_id, agent_id, explicit_subject);
  ATX_TRY(auto insert,
          database.prepare("INSERT OR IGNORE INTO agent_events("
                           "workspace,run_id,task_id,agent_id,event_type,subject,payload,"
                           "idempotency_key,root_sequence,redrive_count) "
                           "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)"));
  ATX_TRY_VOID(insert.bind(1, workspace));
  ATX_TRY_VOID(insert.bind(2, run_id));
  ATX_TRY_VOID(insert.bind(3, task_id));
  ATX_TRY_VOID(insert.bind(4, agent_id));
  ATX_TRY_VOID(insert.bind(5, type));
  ATX_TRY_VOID(insert.bind(6, subject));
  ATX_TRY_VOID(insert.bind(7, payload));
  ATX_TRY_VOID(insert.bind(8, idempotency_key));
  ATX_TRY_VOID(insert.bind(9, root_sequence));
  ATX_TRY_VOID(insert.bind(10, redrive_count));
  ATX_TRY_VOID(step_done(insert));
  if (database.changes() != 0) {
    const i64 sequence = database.last_insert_rowid();
    if (root_sequence == 0) {
      ATX_TRY(auto root,
              database.prepare("UPDATE agent_events SET root_sequence=sequence WHERE sequence=?1 "
                               "AND root_sequence=0 AND redrive_count=0"));
      ATX_TRY_VOID(root.bind(1, sequence));
      ATX_TRY_VOID(step_done(root));
      if (database.changes() != 1) {
        return Err(ErrorCode::Internal, "event root lineage initialization failed");
      }
    }
    return Ok(sequence);
  }
  if (idempotency_key.empty()) {
    return Err(ErrorCode::AlreadyExists, "event uniqueness conflict");
  }
  ATX_TRY(auto existing,
          database.prepare("SELECT sequence,event_type,subject,payload,run_id,task_id,agent_id,"
                           "root_sequence,redrive_count FROM "
                           "agent_events WHERE workspace=?1 AND idempotency_key=?2"));
  ATX_TRY_VOID(existing.bind(1, workspace));
  ATX_TRY_VOID(existing.bind(2, idempotency_key));
  ATX_TRY(const auto step, existing.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "idempotent event disappeared");
  }
  if (existing.column_text(1) != type || existing.column_text(2) != subject ||
      existing.column_text(3) != payload || existing.column_text(4) != run_id ||
      existing.column_text(5) != task_id || existing.column_text(6) != agent_id ||
      (root_sequence == 0 &&
       (existing.column_int(7) != existing.column_int(0) || existing.column_int(8) != 0)) ||
      (root_sequence > 0 &&
       (existing.column_int(7) != root_sequence || existing.column_int(8) != redrive_count))) {
    return Err(ErrorCode::InvalidArgument,
               "event idempotency key was reused with a different request");
  }
  return Ok(existing.column_int(0));
}

[[nodiscard]] Status ensure_event_consumer_lifecycle_epoch(Database &database,
                                                           std::string_view workspace) {
  ATX_TRY(auto insert,
          database.prepare("INSERT OR IGNORE INTO event_consumer_lifecycle_epochs("
                           "workspace,activated_at,event_high_watermark) SELECT ?1,max("
                           "strftime('%Y-%m-%dT%H:%M:%fZ','now'),COALESCE((SELECT "
                           "max(created_at) FROM event_consumer_dead_letters WHERE "
                           "workspace=?1),''),COALESCE((SELECT max(redriven_at) FROM "
                           "event_consumer_dead_letters WHERE workspace=?1),''),COALESCE((SELECT "
                           "max(quarantined_at) FROM event_consumer_dead_letter_quarantines "
                           "WHERE workspace=?1),'')),COALESCE((SELECT max(sequence) FROM "
                           "agent_events WHERE workspace=?1),0)"));
  ATX_TRY_VOID(insert.bind(1, workspace));
  return step_done(insert);
}

[[nodiscard]] Result<i64>
append_event_consumer_dead_letter_lifecycle(Database &database, std::string_view workspace,
                                            std::string_view consumer_name, i64 dead_letter_id,
                                            std::string_view transition) {
  std::string_view event_type;
  if (transition == "dead_lettered") {
    event_type = "consumer.dead_lettered";
  } else if (transition == "redriven") {
    event_type = "consumer.dead_letter_redriven";
  } else if (transition == "quarantined") {
    event_type = "consumer.dead_letter_quarantined";
  } else {
    return Err(ErrorCode::Internal, "event consumer lifecycle transition is invalid");
  }
  ATX_TRY_VOID(ensure_event_consumer_lifecycle_epoch(database, workspace));
  ATX_TRY(auto occurrence,
          database.prepare("SELECT CASE ?4 WHEN 'dead_lettered' THEN l.created_at WHEN "
                           "'redriven' THEN l.redriven_at ELSE q.quarantined_at END FROM "
                           "event_consumer_dead_letters l LEFT JOIN "
                           "event_consumer_dead_letter_quarantines q ON q.workspace=l.workspace "
                           "AND q.consumer_name=l.consumer_name AND q.dead_letter_id=l.id WHERE "
                           "l.id=?1 AND l.workspace=?2 AND l.consumer_name=?3 AND ((?4="
                           "'dead_lettered') OR (?4='redriven' AND l.status='redriven') OR "
                           "(?4='quarantined' AND l.status='open' AND q.id IS NOT NULL))"));
  ATX_TRY_VOID(occurrence.bind(1, dead_letter_id));
  ATX_TRY_VOID(occurrence.bind(2, workspace));
  ATX_TRY_VOID(occurrence.bind(3, consumer_name));
  ATX_TRY_VOID(occurrence.bind(4, transition));
  ATX_TRY(const auto occurrence_step, occurrence.step());
  if (occurrence_step != Statement::Step::Row ||
      !canonical_utc_timestamp(occurrence.column_text(0))) {
    return Err(ErrorCode::Internal, "event consumer lifecycle state is invalid");
  }
  const std::string transition_at{occurrence.column_text(0)};
  ATX_TRY_VOID(occurrence.reset());
  ATX_TRY(const auto event_sequence,
          insert_event(database, workspace, event_type, std::to_string(dead_letter_id), {}, {}, {},
                       {}, "consumers/" + std::string{consumer_name}));
  ATX_TRY(auto mapping,
          database.prepare("INSERT INTO event_consumer_dead_letter_lifecycle_events("
                           "workspace,consumer_name,dead_letter_id,transition,event_sequence,"
                           "transition_at,legacy) VALUES(?1,?2,?3,?4,?5,?6,0)"));
  ATX_TRY_VOID(mapping.bind(1, workspace));
  ATX_TRY_VOID(mapping.bind(2, consumer_name));
  ATX_TRY_VOID(mapping.bind(3, dead_letter_id));
  ATX_TRY_VOID(mapping.bind(4, transition));
  ATX_TRY_VOID(mapping.bind(5, event_sequence));
  ATX_TRY_VOID(mapping.bind(6, transition_at));
  ATX_TRY_VOID(step_done(mapping));
  return Ok(event_sequence);
}

[[nodiscard]] Result<i64> cancel_dependency_descendants(Database &database,
                                                        std::string_view workspace,
                                                        std::string_view run_id,
                                                        std::string_view terminal_task_id,
                                                        std::string_view terminal_status) {
  struct Descendant {
    std::string id;
  };
  std::vector<Descendant> descendants;
  ATX_TRY(auto query,
          database.prepare("WITH RECURSIVE descendants(id) AS ("
                           "SELECT task_id FROM task_dependencies WHERE workspace=?1 AND "
                           "depends_on_task_id=?2 UNION SELECT d.task_id FROM task_dependencies d "
                           "JOIN descendants x ON x.id=d.depends_on_task_id WHERE d.workspace=?1) "
                           "SELECT t.id FROM descendants x JOIN tasks t ON t.workspace=?1 AND "
                           "t.id=x.id WHERE t.run_id=?3 AND t.status IN ('queued','leased') "
                           "ORDER BY t.id"));
  ATX_TRY_VOID(query.bind(1, workspace));
  ATX_TRY_VOID(query.bind(2, terminal_task_id));
  ATX_TRY_VOID(query.bind(3, run_id));
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    descendants.push_back({std::string{query.column_text(0)}});
  }
  const std::string cause =
      "dependency=" + std::string{terminal_task_id} + ";status=" + std::string{terminal_status};
  i64 cancelled = 0;
  for (const auto &descendant : descendants) {
    ATX_TRY(auto update,
            database.prepare("UPDATE tasks SET status='cancelled',lease_owner='',lease_token='',"
                             "lease_expires_at='',last_error=?3,last_transition_token=?4,"
                             "last_transition_kind='dependency_cancelled',revision=revision+1,"
                             "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                             "AND id=?2 AND run_id=?5 AND status IN ('queued','leased')"));
    ATX_TRY_VOID(update.bind(1, workspace));
    ATX_TRY_VOID(update.bind(2, descendant.id));
    ATX_TRY_VOID(update.bind(3, cause));
    ATX_TRY_VOID(update.bind(4, terminal_task_id));
    ATX_TRY_VOID(update.bind(5, run_id));
    ATX_TRY_VOID(step_done(update));
    if (database.changes() != 1) {
      continue;
    }
    ++cancelled;
    ATX_TRY_VOID(
        insert_event(database, workspace, "task.cancelled", cause, run_id, descendant.id, {}));
  }
  return Ok(cancelled);
}

[[nodiscard]] Result<FactRecord> read_fact(Statement &statement) {
  FactRecord fact;
  fact.id = statement.column_int(0);
  fact.subject = statement.column_text(1);
  fact.predicate = statement.column_text(2);
  fact.object = statement.column_text(3);
  fact.valid_from = statement.column_text(4);
  if (!statement.column_is_null(5)) {
    fact.valid_to = statement.column_text(5);
  }
  fact.transaction_from = statement.column_text(6);
  if (!statement.column_is_null(7)) {
    fact.transaction_to = statement.column_text(7);
  }
  fact.evidence_source_id = statement.column_text(8);
  fact.evidence_observation_id = statement.column_int(9);
  fact.evidence_content_hash = statement.column_text(10);
  fact.evidence_status = statement.column_text(11);
  fact.confidence = statement.column_double(12);
  if (!statement.column_is_null(13)) {
    fact.supersedes_fact_id = statement.column_int(13);
  }
  fact.transaction_from_sequence = statement.column_int(14);
  if (!statement.column_is_null(15)) {
    fact.transaction_to_sequence = statement.column_int(15);
  }
  fact.idempotency_key = statement.column_text(16);
  fact.request_valid_from = statement.column_text(17);
  return Ok(std::move(fact));
}

[[nodiscard]] Status verify_backup_pair_domains(std::string_view coordination_path,
                                                std::string_view knowledge_path,
                                                std::string_view required_workspace) {
  std::vector<std::string> workspaces{std::string{required_workspace}};
  ATX_TRY(auto raw_coordination,
          Database::open(coordination_path, atx::core::db::OpenMode::ReadOnly));
  ATX_TRY(auto query,
          raw_coordination.prepare(
              "SELECT workspace FROM runs UNION SELECT workspace FROM agents UNION "
              "SELECT workspace FROM tasks UNION SELECT workspace FROM task_dependencies UNION "
              "SELECT workspace FROM agent_events UNION SELECT workspace FROM event_consumers "
              "UNION SELECT workspace FROM event_consumer_checkpoints UNION "
              "SELECT workspace FROM event_consumer_dead_letters UNION "
              "SELECT workspace FROM event_consumer_redrive_events UNION "
              "SELECT workspace FROM episodes UNION SELECT workspace FROM temporal_clocks UNION "
              "SELECT workspace FROM facts "
              "ORDER BY workspace"));
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    workspaces.emplace_back(query.column_text(0));
  }
  std::sort(workspaces.begin(), workspaces.end());
  workspaces.erase(std::unique(workspaces.begin(), workspaces.end()), workspaces.end());
  ATX_TRY(auto knowledge, atx::kb::KnowledgeBase::open(knowledge_path));
  ATX_TRY_VOID(knowledge.verify_integrity());
  for (const auto &workspace : workspaces) {
    ATX_TRY(auto coordination, AgentDatabase::open(coordination_path, workspace));
    ATX_TRY_VOID(coordination.verify_integrity());
    ATX_TRY_VOID(coordination.verify_evidence_links(knowledge));
  }
  return Ok();
}

constexpr std::string_view kFactColumns =
    "id,subject,predicate,object,valid_from,valid_to,transaction_from,transaction_to,"
    "evidence_source_id,evidence_observation_id,evidence_content_hash,evidence_status,confidence,"
    "supersedes_fact_id,transaction_from_sequence,transaction_to_sequence,idempotency_key,"
    "request_valid_from";

} // namespace

Result<AgentDatabase> AgentDatabase::open(std::string_view path, std::string_view workspace) {
  if (!valid_field(path, kMaximumPayloadBytes, false) ||
      !valid_field(workspace, kMaximumWorkspaceBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "database path or workspace is invalid");
  }
  ATX_TRY(auto database, Database::open(path));
  AgentDatabase result{std::move(database), std::string{workspace}};
  ATX_TRY_VOID(result.initialize());
  return Ok(std::move(result));
}

Result<AgentDatabase> AgentDatabase::open_memory(std::string_view workspace) {
  if (!valid_field(workspace, kMaximumWorkspaceBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "workspace is invalid");
  }
  ATX_TRY(auto database, Database::open_memory());
  AgentDatabase result{std::move(database), std::string{workspace}};
  ATX_TRY_VOID(result.initialize());
  return Ok(std::move(result));
}

Status AgentDatabase::initialize() {
  ATX_TRY_VOID(database_.set_busy_timeout(5'000));
  ATX_TRY_VOID(database_.pragma("foreign_keys", "ON"));
  ATX_TRY_VOID(ensure_wal(database_));
  ATX_TRY_VOID(database_.pragma("synchronous", "FULL"));
  ATX_TRY_VOID(database_.exec("CREATE TABLE IF NOT EXISTS agent_db_meta("
                              "key TEXT PRIMARY KEY,value TEXT NOT NULL) STRICT"));
  auto version =
      scalar_text(database_, "SELECT value FROM agent_db_meta WHERE key='schema_version'");
  if (!version) {
    if (version.error().code() != ErrorCode::NotFound) {
      return Err(std::move(version).error());
    }
    ATX_TRY_VOID(database_.exec(kSchema));
    ATX_TRY_VOID(database_.exec(kConsumerStateRevisionSchema));
    return Ok();
  }
  std::string current_version = *version;
  bool migrated = false;
  if (current_version == "1") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    ATX_TRY_VOID(database_.exec(
        "DROP INDEX IF EXISTS facts_one_active_version;"
        "CREATE UNIQUE INDEX IF NOT EXISTS facts_current_interval_unique "
        "ON facts(workspace,subject,predicate,valid_from) WHERE transaction_to IS NULL;"
        "UPDATE agent_db_meta SET value='2' WHERE key='schema_version';"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "2";
    migrated = true;
  }
  if (current_version == "2") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    ATX_TRY_VOID(database_.exec(
        "ALTER TABLE tasks ADD COLUMN last_transition_token TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE tasks ADD COLUMN last_transition_kind TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE facts ADD COLUMN transaction_from_sequence INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE facts ADD COLUMN transaction_to_sequence INTEGER;"
        "CREATE TABLE IF NOT EXISTS temporal_clocks("
        "workspace TEXT PRIMARY KEY,last_sequence INTEGER NOT NULL CHECK(last_sequence>=0)) "
        "WITHOUT ROWID, STRICT;"
        "UPDATE facts SET transaction_from_sequence=id WHERE transaction_from_sequence=0;"
        "UPDATE facts AS f SET transaction_to_sequence=COALESCE((SELECT min(b.id) FROM facts b "
        "WHERE b.workspace=f.workspace AND b.id>f.id AND "
        "b.transaction_from>=f.transaction_to),f.id+1) WHERE transaction_to IS NOT NULL;"
        "INSERT INTO temporal_clocks(workspace,last_sequence) SELECT workspace,"
        "max(CASE WHEN transaction_to_sequence>transaction_from_sequence THEN "
        "transaction_to_sequence ELSE transaction_from_sequence END) FROM facts GROUP BY workspace "
        "ON CONFLICT(workspace) DO UPDATE SET last_sequence="
        "max(last_sequence,excluded.last_sequence);"
        "UPDATE agent_db_meta SET value='3' WHERE key='schema_version';"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "3";
    migrated = true;
  }
  if (current_version == "3") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    struct TerminalTask {
      std::string workspace;
      std::string id;
      std::string run_id;
      std::string status;
    };
    std::vector<TerminalTask> terminal_tasks;
    ATX_TRY(auto query,
            database_.prepare("SELECT workspace,id,run_id,status FROM tasks WHERE "
                              "status IN ('failed','cancelled') ORDER BY workspace,id"));
    while (true) {
      ATX_TRY(const auto step, query.step());
      if (step == Statement::Step::Done) {
        break;
      }
      terminal_tasks.push_back(
          {std::string{query.column_text(0)}, std::string{query.column_text(1)},
           std::string{query.column_text(2)}, std::string{query.column_text(3)}});
    }
    for (const auto &task : terminal_tasks) {
      ATX_TRY(auto ignored, cancel_dependency_descendants(database_, task.workspace, task.run_id,
                                                          task.id, task.status));
      (void)ignored;
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='4' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "4";
    migrated = true;
  }
  if (current_version == "4") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool episodes_exists = false;
    bool has_status = false;
    bool has_hash = false;
    bool has_verified_at = false;
    ATX_TRY(auto columns, database_.prepare("PRAGMA table_info(episodes)"));
    while (true) {
      ATX_TRY(const auto step, columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      episodes_exists = true;
      const std::string_view name = columns.column_text(1);
      has_status = has_status || name == "evidence_status";
      has_hash = has_hash || name == "evidence_content_hash";
      has_verified_at = has_verified_at || name == "evidence_verified_at";
    }
    if (episodes_exists && !has_status) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE episodes ADD COLUMN evidence_status TEXT NOT NULL DEFAULT 'unverified' "
          "CHECK(evidence_status IN ('unverified','verified'))"));
    }
    if (episodes_exists && !has_hash) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE episodes ADD COLUMN evidence_content_hash TEXT NOT NULL DEFAULT ''"));
    }
    if (episodes_exists && !has_verified_at) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE episodes ADD COLUMN evidence_verified_at TEXT NOT NULL DEFAULT ''"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='5' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "5";
    migrated = true;
  }
  if (current_version == "5") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool tasks_exist = false;
    bool has_observation = false;
    bool has_hash = false;
    bool has_status = false;
    ATX_TRY(auto columns, database_.prepare("PRAGMA table_info(tasks)"));
    while (true) {
      ATX_TRY(const auto step, columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      tasks_exist = true;
      const std::string_view name = columns.column_text(1);
      has_observation = has_observation || name == "result_observation_id";
      has_hash = has_hash || name == "result_content_hash";
      has_status = has_status || name == "result_evidence_status";
    }
    if (tasks_exist && !has_observation) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE tasks ADD COLUMN result_observation_id INTEGER NOT NULL DEFAULT 0 "
          "CHECK(result_observation_id>=0)"));
    }
    if (tasks_exist && !has_hash) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE tasks ADD COLUMN result_content_hash TEXT NOT NULL DEFAULT ''"));
    }
    if (tasks_exist && !has_status) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE tasks ADD COLUMN result_evidence_status TEXT NOT NULL DEFAULT 'none' "
          "CHECK(result_evidence_status IN ('none','unverified','verified'))"));
      ATX_TRY_VOID(database_.exec(
          "UPDATE tasks SET result_evidence_status='unverified' WHERE result_source_id<>''"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='6' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "6";
    migrated = true;
  }
  if (current_version == "6") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool facts_exist = false;
    bool has_observation = false;
    bool has_hash = false;
    bool has_status = false;
    ATX_TRY(auto columns, database_.prepare("PRAGMA table_info(facts)"));
    while (true) {
      ATX_TRY(const auto step, columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      facts_exist = true;
      const std::string_view name = columns.column_text(1);
      has_observation = has_observation || name == "evidence_observation_id";
      has_hash = has_hash || name == "evidence_content_hash";
      has_status = has_status || name == "evidence_status";
    }
    if (facts_exist && !has_observation) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE facts ADD COLUMN evidence_observation_id INTEGER NOT NULL DEFAULT 0 "
          "CHECK(evidence_observation_id>=0)"));
    }
    if (facts_exist && !has_hash) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE facts ADD COLUMN evidence_content_hash TEXT NOT NULL DEFAULT ''"));
    }
    if (facts_exist && !has_status) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE facts ADD COLUMN evidence_status TEXT NOT NULL DEFAULT 'none' "
          "CHECK(evidence_status IN ('none','unverified','verified'))"));
    }
    if (facts_exist) {
      ATX_TRY_VOID(database_.exec(
          "UPDATE facts SET evidence_status='unverified' WHERE evidence_source_id<>'' AND "
          "evidence_status='none'"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='7' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "7";
    migrated = true;
  }
  if (current_version == "7") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool facts_exist = false;
    bool has_idempotency_key = false;
    bool has_request_valid_from = false;
    ATX_TRY(auto columns, database_.prepare("PRAGMA table_info(facts)"));
    while (true) {
      ATX_TRY(const auto step, columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      facts_exist = true;
      const std::string_view name = columns.column_text(1);
      has_idempotency_key = has_idempotency_key || name == "idempotency_key";
      has_request_valid_from = has_request_valid_from || name == "request_valid_from";
    }
    if (facts_exist && !has_idempotency_key) {
      ATX_TRY_VOID(
          database_.exec("ALTER TABLE facts ADD COLUMN idempotency_key TEXT NOT NULL DEFAULT ''"));
    }
    if (facts_exist && !has_request_valid_from) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE facts ADD COLUMN request_valid_from TEXT NOT NULL DEFAULT ''"));
    }
    if (facts_exist) {
      ATX_TRY_VOID(database_.exec("CREATE UNIQUE INDEX IF NOT EXISTS facts_idempotency_idx "
                                  "ON facts(workspace,idempotency_key) WHERE idempotency_key<>''"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='8' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "8";
    migrated = true;
  }
  if (current_version == "8") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool events_exist = false;
    bool has_subject = false;
    ATX_TRY(auto columns, database_.prepare("PRAGMA table_info(agent_events)"));
    while (true) {
      ATX_TRY(const auto step, columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      events_exist = true;
      has_subject = has_subject || std::string_view{columns.column_text(1)} == "subject";
    }
    if (events_exist && !has_subject) {
      ATX_TRY_VOID(
          database_.exec("ALTER TABLE agent_events ADD COLUMN subject TEXT NOT NULL DEFAULT ''"));
    }
    if (events_exist) {
      ATX_TRY_VOID(
          database_.exec("UPDATE agent_events SET subject=CASE "
                         "WHEN event_type LIKE 'task.%' AND task_id<>'' THEN 'tasks/'||task_id "
                         "WHEN event_type LIKE 'agent.%' AND agent_id<>'' THEN 'agents/'||agent_id "
                         "WHEN event_type LIKE 'run.%' AND run_id<>'' THEN 'runs/'||run_id "
                         "ELSE subject END WHERE subject='';"
                         "CREATE INDEX IF NOT EXISTS agent_events_subject_idx "
                         "ON agent_events(workspace,subject,sequence) WHERE subject<>'';"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='9' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "9";
    migrated = true;
  }
  if (current_version == "9") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    ATX_TRY_VOID(database_.exec(
        "CREATE TABLE IF NOT EXISTS event_consumers("
        "workspace TEXT NOT NULL,name TEXT NOT NULL,subject_filter TEXT NOT NULL DEFAULT '',"
        "start_sequence INTEGER NOT NULL DEFAULT 0 CHECK(start_sequence>=0),"
        "cursor_sequence INTEGER NOT NULL DEFAULT 0 CHECK(cursor_sequence>=start_sequence),"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1),"
        "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        "updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        "PRIMARY KEY(workspace,name)) WITHOUT ROWID, STRICT;"
        "CREATE INDEX IF NOT EXISTS event_consumers_cursor_idx "
        "ON event_consumers(workspace,cursor_sequence,name);"
        "CREATE TABLE IF NOT EXISTS event_consumer_checkpoints("
        "id INTEGER PRIMARY KEY,workspace TEXT NOT NULL,consumer_name TEXT NOT NULL,"
        "checkpoint_token TEXT NOT NULL,request_revision INTEGER NOT NULL "
        "CHECK(request_revision>=1),previous_sequence INTEGER NOT NULL "
        "CHECK(previous_sequence>=0),through_sequence INTEGER NOT NULL "
        "CHECK(through_sequence>previous_sequence),result_revision INTEGER NOT NULL "
        "CHECK(result_revision=request_revision+1),created_at TEXT NOT NULL DEFAULT "
        "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        "UNIQUE(workspace,consumer_name,checkpoint_token),"
        "UNIQUE(workspace,consumer_name,result_revision),"
        "FOREIGN KEY(workspace,consumer_name) REFERENCES event_consumers(workspace,name)) STRICT;"
        "CREATE INDEX IF NOT EXISTS event_consumer_checkpoints_sequence_idx "
        "ON event_consumer_checkpoints(workspace,consumer_name,through_sequence);"
        "UPDATE agent_db_meta SET value='10' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "10";
    migrated = true;
  }
  if (current_version == "10") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_active_token = false;
    bool has_active_owner = false;
    bool has_active_previous = false;
    bool has_active_through = false;
    bool has_active_attempt = false;
    bool has_active_expiry = false;
    ATX_TRY(auto consumer_columns, database_.prepare("PRAGMA table_info(event_consumers)"));
    while (true) {
      ATX_TRY(const auto step, consumer_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view name = consumer_columns.column_text(1);
      has_active_token = has_active_token || name == "active_delivery_token";
      has_active_owner = has_active_owner || name == "active_delivery_owner";
      has_active_previous = has_active_previous || name == "active_delivery_previous_sequence";
      has_active_through = has_active_through || name == "active_delivery_through_sequence";
      has_active_attempt = has_active_attempt || name == "active_delivery_attempt";
      has_active_expiry = has_active_expiry || name == "active_delivery_expires_at";
    }
    if (!has_active_token) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_token TEXT NOT NULL DEFAULT ''"));
    }
    if (!has_active_owner) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_owner TEXT NOT NULL DEFAULT ''"));
    }
    if (!has_active_previous) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_previous_sequence INTEGER NOT "
          "NULL DEFAULT 0 CHECK(active_delivery_previous_sequence>=0)"));
    }
    if (!has_active_through) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_through_sequence INTEGER NOT "
          "NULL DEFAULT 0 CHECK(active_delivery_through_sequence>=0)"));
    }
    if (!has_active_attempt) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_attempt INTEGER NOT NULL "
          "DEFAULT 0 CHECK(active_delivery_attempt>=0)"));
    }
    if (!has_active_expiry) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_expires_at TEXT NOT NULL "
          "DEFAULT ''"));
    }
    bool has_checkpoint_delivery_token = false;
    ATX_TRY(auto checkpoint_columns,
            database_.prepare("PRAGMA table_info(event_consumer_checkpoints)"));
    while (true) {
      ATX_TRY(const auto step, checkpoint_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      has_checkpoint_delivery_token =
          has_checkpoint_delivery_token || checkpoint_columns.column_text(1) == "delivery_token";
    }
    if (!has_checkpoint_delivery_token) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_checkpoints ADD COLUMN delivery_token TEXT NOT NULL "
          "DEFAULT ''"));
    }
    ATX_TRY_VOID(database_.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS event_consumers_active_delivery_idx ON "
        "event_consumers(workspace,active_delivery_token) WHERE active_delivery_token<>'';"
        "CREATE TABLE IF NOT EXISTS event_consumer_deliveries("
        "id INTEGER PRIMARY KEY,workspace TEXT NOT NULL,consumer_name TEXT NOT NULL,"
        "delivery_token TEXT NOT NULL,owner TEXT NOT NULL,request_token TEXT NOT NULL,"
        "request_revision INTEGER NOT NULL CHECK(request_revision>=1),"
        "previous_sequence INTEGER NOT NULL CHECK(previous_sequence>=0),"
        "through_sequence INTEGER NOT NULL CHECK(through_sequence>previous_sequence),"
        "attempt INTEGER NOT NULL CHECK(attempt>=1),requested_limit INTEGER NOT NULL "
        "CHECK(requested_limit BETWEEN 1 AND 1000),lease_seconds INTEGER NOT NULL "
        "CHECK(lease_seconds BETWEEN 1 AND 86400),state TEXT NOT NULL DEFAULT 'active' "
        "CHECK(state IN ('active','settled','expired')),acquired_at TEXT NOT NULL DEFAULT "
        "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),expires_at TEXT NOT NULL,"
        "finished_at TEXT NOT NULL DEFAULT '',UNIQUE(workspace,consumer_name,delivery_token),"
        "UNIQUE(workspace,consumer_name,request_token),FOREIGN KEY(workspace,consumer_name) "
        "REFERENCES event_consumers(workspace,name)) STRICT;"
        "CREATE INDEX IF NOT EXISTS event_consumer_deliveries_state_idx ON "
        "event_consumer_deliveries(workspace,consumer_name,state,id);"
        "UPDATE agent_db_meta SET value='11' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "11";
    migrated = true;
  }
  if (current_version == "11") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_max_delivery_attempts = false;
    ATX_TRY(auto consumer_columns, database_.prepare("PRAGMA table_info(event_consumers)"));
    while (true) {
      ATX_TRY(const auto step, consumer_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      has_max_delivery_attempts =
          has_max_delivery_attempts || consumer_columns.column_text(1) == "max_delivery_attempts";
    }
    if (!has_max_delivery_attempts) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN max_delivery_attempts INTEGER NOT NULL "
          "DEFAULT 0 CHECK(max_delivery_attempts BETWEEN 0 AND 1000)"));
    }
    bool has_checkpoint_outcome = false;
    ATX_TRY(auto checkpoint_columns,
            database_.prepare("PRAGMA table_info(event_consumer_checkpoints)"));
    while (true) {
      ATX_TRY(const auto step, checkpoint_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      has_checkpoint_outcome =
          has_checkpoint_outcome || checkpoint_columns.column_text(1) == "outcome";
    }
    if (!has_checkpoint_outcome) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_checkpoints ADD COLUMN outcome TEXT NOT NULL DEFAULT "
          "'processed' CHECK(outcome IN ('processed','dead_lettered'))"));
    }
    bool has_preceding_dead_lettered_batches = false;
    bool has_preceding_dead_lettered_events = false;
    ATX_TRY(auto delivery_columns,
            database_.prepare("PRAGMA table_info(event_consumer_deliveries)"));
    while (true) {
      ATX_TRY(const auto step, delivery_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view name = delivery_columns.column_text(1);
      has_preceding_dead_lettered_batches =
          has_preceding_dead_lettered_batches || name == "preceding_dead_lettered_batches";
      has_preceding_dead_lettered_events =
          has_preceding_dead_lettered_events || name == "preceding_dead_lettered_events";
    }
    if (!has_preceding_dead_lettered_batches) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN preceding_dead_lettered_batches "
          "INTEGER NOT NULL DEFAULT 0 CHECK(preceding_dead_lettered_batches>=0)"));
    }
    if (!has_preceding_dead_lettered_events) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN preceding_dead_lettered_events "
          "INTEGER NOT NULL DEFAULT 0 CHECK(preceding_dead_lettered_events>=0)"));
    }
    ATX_TRY_VOID(database_.exec(
        "CREATE TABLE IF NOT EXISTS event_consumer_dead_letters("
        "id INTEGER PRIMARY KEY,workspace TEXT NOT NULL,consumer_name TEXT NOT NULL,"
        "delivery_token TEXT NOT NULL,previous_sequence INTEGER NOT NULL "
        "CHECK(previous_sequence>=0),through_sequence INTEGER NOT NULL "
        "CHECK(through_sequence>previous_sequence),delivery_attempts INTEGER NOT NULL "
        "CHECK(delivery_attempts>=1),event_count INTEGER NOT NULL "
        "CHECK(event_count BETWEEN 1 AND 1000),reason TEXT NOT NULL CHECK(reason<>''),"
        "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        "UNIQUE(workspace,consumer_name,delivery_token),FOREIGN KEY(workspace,consumer_name) "
        "REFERENCES event_consumers(workspace,name),FOREIGN KEY(workspace,consumer_name,"
        "delivery_token) REFERENCES event_consumer_deliveries(workspace,consumer_name,"
        "delivery_token)) STRICT;"
        "CREATE INDEX IF NOT EXISTS event_consumer_dead_letters_consumer_idx ON "
        "event_consumer_dead_letters(workspace,consumer_name,id);"
        "UPDATE agent_db_meta SET value='12' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "12";
    migrated = true;
  }
  if (current_version == "12") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_status = false;
    bool has_redrive_token = false;
    bool has_redriven_at = false;
    ATX_TRY(auto dead_letter_columns,
            database_.prepare("PRAGMA table_info(event_consumer_dead_letters)"));
    while (true) {
      ATX_TRY(const auto step, dead_letter_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view name = dead_letter_columns.column_text(1);
      has_status = has_status || name == "status";
      has_redrive_token = has_redrive_token || name == "redrive_token";
      has_redriven_at = has_redriven_at || name == "redriven_at";
    }
    if (!has_status) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_dead_letters ADD COLUMN status TEXT NOT NULL DEFAULT "
          "'open' CHECK(status IN ('open','redriven'))"));
    }
    if (!has_redrive_token) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_dead_letters ADD COLUMN redrive_token TEXT NOT NULL "
          "DEFAULT ''"));
    }
    if (!has_redriven_at) {
      ATX_TRY_VOID(database_.exec("ALTER TABLE event_consumer_dead_letters ADD COLUMN redriven_at "
                                  "TEXT NOT NULL DEFAULT ''"));
    }
    ATX_TRY_VOID(database_.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS event_consumer_dead_letters_redrive_token_idx ON "
        "event_consumer_dead_letters(workspace,consumer_name,redrive_token) WHERE "
        "redrive_token<>'';"
        "CREATE TABLE IF NOT EXISTS event_consumer_redrive_events("
        "workspace TEXT NOT NULL,consumer_name TEXT NOT NULL,dead_letter_id INTEGER NOT NULL,"
        "original_sequence INTEGER NOT NULL,redriven_sequence INTEGER NOT NULL,created_at TEXT "
        "NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),PRIMARY KEY(workspace,"
        "consumer_name,dead_letter_id,original_sequence),UNIQUE(workspace,consumer_name,"
        "dead_letter_id,redriven_sequence),FOREIGN KEY(dead_letter_id) REFERENCES "
        "event_consumer_dead_letters(id),FOREIGN KEY(original_sequence) REFERENCES "
        "agent_events(sequence),FOREIGN KEY(redriven_sequence) REFERENCES agent_events(sequence)) "
        "WITHOUT ROWID, STRICT;"
        "UPDATE agent_db_meta SET value='13' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "13";
    migrated = true;
  }
  if (current_version == "13") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_retry_backoff = false;
    bool has_retry_backoff_max = false;
    bool has_active_retry_at = false;
    ATX_TRY(auto consumer_columns, database_.prepare("PRAGMA table_info(event_consumers)"));
    while (true) {
      ATX_TRY(const auto step, consumer_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view name = consumer_columns.column_text(1);
      has_retry_backoff = has_retry_backoff || name == "retry_backoff_seconds";
      has_retry_backoff_max = has_retry_backoff_max || name == "retry_backoff_max_seconds";
      has_active_retry_at = has_active_retry_at || name == "active_delivery_retry_at";
    }
    if (!has_retry_backoff) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN retry_backoff_seconds INTEGER NOT NULL "
          "DEFAULT 0 CHECK(retry_backoff_seconds BETWEEN 0 AND 86400)"));
    }
    if (!has_retry_backoff_max) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN retry_backoff_max_seconds INTEGER NOT NULL "
          "DEFAULT 0 CHECK(retry_backoff_max_seconds BETWEEN 0 AND 86400)"));
    }
    if (!has_active_retry_at) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_retry_at TEXT NOT NULL "
          "DEFAULT ''"));
    }
    bool has_retry_not_before = false;
    ATX_TRY(auto delivery_columns,
            database_.prepare("PRAGMA table_info(event_consumer_deliveries)"));
    while (true) {
      ATX_TRY(const auto step, delivery_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      has_retry_not_before =
          has_retry_not_before || delivery_columns.column_text(1) == "retry_not_before";
    }
    if (!has_retry_not_before) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN retry_not_before TEXT NOT NULL "
          "DEFAULT ''"));
    }
    ATX_TRY_VOID(database_.exec(
        "UPDATE event_consumers SET active_delivery_retry_at=active_delivery_expires_at "
        "WHERE active_delivery_token<>'' AND active_delivery_retry_at='';"
        "UPDATE event_consumer_deliveries SET retry_not_before=expires_at "
        "WHERE retry_not_before='';"
        "UPDATE agent_db_meta SET value='14' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "14";
    migrated = true;
  }
  if (current_version == "14") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_rejection_token = false;
    bool has_rejection_reason = false;
    bool has_rejected_at = false;
    ATX_TRY(auto delivery_columns,
            database_.prepare("PRAGMA table_info(event_consumer_deliveries)"));
    while (true) {
      ATX_TRY(const auto step, delivery_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view name = delivery_columns.column_text(1);
      has_rejection_token = has_rejection_token || name == "rejection_token";
      has_rejection_reason = has_rejection_reason || name == "rejection_reason";
      has_rejected_at = has_rejected_at || name == "rejected_at";
    }
    if (!has_rejection_token) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN rejection_token TEXT NOT NULL "
          "DEFAULT ''"));
    }
    if (!has_rejection_reason) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN rejection_reason TEXT NOT NULL "
          "DEFAULT ''"));
    }
    if (!has_rejected_at) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN rejected_at TEXT NOT NULL DEFAULT ''"));
    }
    ATX_TRY_VOID(database_.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS event_consumer_deliveries_rejection_token_idx ON "
        "event_consumer_deliveries(workspace,consumer_name,rejection_token) WHERE "
        "rejection_token<>'';"
        "UPDATE agent_db_meta SET value='15' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "15";
    migrated = true;
  }
  if (current_version == "15") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_rejection_disposition = false;
    ATX_TRY(auto delivery_columns,
            database_.prepare("PRAGMA table_info(event_consumer_deliveries)"));
    while (true) {
      ATX_TRY(const auto step, delivery_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      has_rejection_disposition =
          has_rejection_disposition || delivery_columns.column_text(1) == "rejection_disposition";
    }
    if (!has_rejection_disposition) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN rejection_disposition TEXT NOT NULL "
          "DEFAULT '' CHECK(rejection_disposition IN ('','retry','dead_letter'))"));
    }
    ATX_TRY_VOID(
        database_.exec("UPDATE event_consumer_deliveries SET rejection_disposition='retry' WHERE "
                       "rejection_token<>'' AND rejection_disposition='';"
                       "UPDATE agent_db_meta SET value='16' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "16";
    migrated = true;
  }
  if (current_version == "16") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_retry_jitter = false;
    bool has_active_retry_delay = false;
    ATX_TRY(auto consumer_columns, database_.prepare("PRAGMA table_info(event_consumers)"));
    while (true) {
      ATX_TRY(const auto step, consumer_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view name = consumer_columns.column_text(1);
      has_retry_jitter = has_retry_jitter || name == "retry_jitter";
      has_active_retry_delay =
          has_active_retry_delay || name == "active_delivery_retry_delay_seconds";
    }
    if (!has_retry_jitter) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN retry_jitter TEXT NOT NULL DEFAULT 'none' "
          "CHECK(retry_jitter IN ('none','full'))"));
    }
    if (!has_active_retry_delay) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN active_delivery_retry_delay_seconds INTEGER "
          "NOT NULL DEFAULT 0 CHECK(active_delivery_retry_delay_seconds BETWEEN 0 AND 86400)"));
    }
    bool has_retry_delay = false;
    ATX_TRY(auto delivery_columns,
            database_.prepare("PRAGMA table_info(event_consumer_deliveries)"));
    while (true) {
      ATX_TRY(const auto step, delivery_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      has_retry_delay = has_retry_delay || delivery_columns.column_text(1) == "retry_delay_seconds";
    }
    if (!has_retry_delay) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumer_deliveries ADD COLUMN retry_delay_seconds INTEGER NOT NULL "
          "DEFAULT 0 CHECK(retry_delay_seconds BETWEEN 0 AND 86400)"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE event_consumers SET retry_jitter='none'"));

    std::vector<std::array<i64, 2>> delivery_delays;
    ATX_TRY(auto deliveries,
            database_.prepare("SELECT d.id,c.retry_backoff_seconds,c.retry_backoff_max_seconds,"
                              "d.attempt FROM event_consumer_deliveries d JOIN event_consumers c "
                              "ON c.workspace=d.workspace AND c.name=d.consumer_name ORDER BY "
                              "d.id"));
    while (true) {
      ATX_TRY(const auto step, deliveries.step());
      if (step == Statement::Step::Done) {
        break;
      }
      delivery_delays.push_back(
          {deliveries.column_int(0),
           delivery_retry_backoff(deliveries.column_int(1), deliveries.column_int(2),
                                  deliveries.column_int(3))});
    }
    ATX_TRY_VOID(deliveries.reset());
    ATX_TRY(auto update_delivery,
            database_.prepare("UPDATE event_consumer_deliveries SET retry_delay_seconds=?2 "
                              "WHERE id=?1"));
    for (const auto &entry : delivery_delays) {
      ATX_TRY_VOID(update_delivery.bind(1, entry[0]));
      ATX_TRY_VOID(update_delivery.bind(2, entry[1]));
      ATX_TRY_VOID(step_done(update_delivery));
      ATX_TRY_VOID(update_delivery.reset());
      ATX_TRY_VOID(update_delivery.clear_bindings());
    }

    struct ActiveRetryDelay {
      std::string workspace;
      std::string name;
      i64 delay{};
    };
    std::vector<ActiveRetryDelay> active_delays;
    ATX_TRY(auto active,
            database_.prepare("SELECT workspace,name,retry_backoff_seconds,"
                              "retry_backoff_max_seconds,active_delivery_attempt FROM "
                              "event_consumers WHERE active_delivery_token<>'' ORDER BY "
                              "workspace,name"));
    while (true) {
      ATX_TRY(const auto step, active.step());
      if (step == Statement::Step::Done) {
        break;
      }
      active_delays.push_back({std::string{active.column_text(0)},
                               std::string{active.column_text(1)},
                               delivery_retry_backoff(active.column_int(2), active.column_int(3),
                                                      active.column_int(4))});
    }
    ATX_TRY_VOID(active.reset());
    ATX_TRY(auto update_active,
            database_.prepare("UPDATE event_consumers SET "
                              "active_delivery_retry_delay_seconds=?3 WHERE workspace=?1 AND "
                              "name=?2"));
    for (const auto &entry : active_delays) {
      ATX_TRY_VOID(update_active.bind(1, entry.workspace));
      ATX_TRY_VOID(update_active.bind(2, entry.name));
      ATX_TRY_VOID(update_active.bind(3, entry.delay));
      ATX_TRY_VOID(step_done(update_active));
      ATX_TRY_VOID(update_active.reset());
      ATX_TRY_VOID(update_active.clear_bindings());
    }
    ATX_TRY_VOID(
        database_.exec("UPDATE event_consumers SET active_delivery_retry_delay_seconds=0 WHERE "
                       "active_delivery_token='';"
                       "UPDATE agent_db_meta SET value='17' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "17";
    migrated = true;
  }
  if (current_version == "17") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool has_redrive_rate = false;
    bool has_redrive_burst = false;
    bool has_redrive_tokens = false;
    bool has_redrive_refilled_at = false;
    ATX_TRY(auto consumer_columns, database_.prepare("PRAGMA table_info(event_consumers)"));
    while (true) {
      ATX_TRY(const auto step, consumer_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view name = consumer_columns.column_text(1);
      has_redrive_rate = has_redrive_rate || name == "redrive_rate_per_second";
      has_redrive_burst = has_redrive_burst || name == "redrive_burst_events";
      has_redrive_tokens = has_redrive_tokens || name == "redrive_token_millis";
      has_redrive_refilled_at = has_redrive_refilled_at || name == "redrive_refilled_at";
    }
    if (!has_redrive_rate) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN redrive_rate_per_second INTEGER NOT NULL "
          "DEFAULT 0 CHECK(redrive_rate_per_second BETWEEN 0 AND 1000)"));
    }
    if (!has_redrive_burst) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN redrive_burst_events INTEGER NOT NULL DEFAULT "
          "0 CHECK(redrive_burst_events BETWEEN 0 AND 1000)"));
    }
    if (!has_redrive_tokens) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN redrive_token_millis INTEGER NOT NULL DEFAULT "
          "0 CHECK(redrive_token_millis BETWEEN 0 AND 1000000)"));
    }
    if (!has_redrive_refilled_at) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN redrive_refilled_at TEXT NOT NULL DEFAULT ''"));
    }
    ATX_TRY_VOID(database_.exec(
        "UPDATE event_consumers SET redrive_rate_per_second=0,redrive_burst_events=0,"
        "redrive_token_millis=0,redrive_refilled_at='';"
        "CREATE TABLE IF NOT EXISTS event_consumer_redrive_budget_charges("
        "id INTEGER PRIMARY KEY,workspace TEXT NOT NULL,consumer_name TEXT NOT NULL,"
        "dead_letter_id INTEGER NOT NULL,redrive_token TEXT NOT NULL,event_count INTEGER NOT NULL "
        "CHECK(event_count BETWEEN 1 AND 1000),refilled_token_millis INTEGER NOT NULL "
        "CHECK(refilled_token_millis>=event_count*1000),result_token_millis INTEGER NOT NULL "
        "CHECK(result_token_millis=refilled_token_millis-event_count*1000),refilled_at TEXT NOT "
        "NULL,created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        "UNIQUE(workspace,consumer_name,dead_letter_id),UNIQUE(workspace,consumer_name,"
        "redrive_token),FOREIGN KEY(workspace,consumer_name) REFERENCES "
        "event_consumers(workspace,name),FOREIGN KEY(dead_letter_id) REFERENCES "
        "event_consumer_dead_letters(id)) STRICT;"
        "CREATE INDEX IF NOT EXISTS event_consumer_redrive_budget_charges_consumer_idx ON "
        "event_consumer_redrive_budget_charges(workspace,consumer_name,id);"
        "UPDATE agent_db_meta SET value='18' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "18";
    migrated = true;
  }
  if (current_version == "18") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool events_exist = false;
    bool has_root_sequence = false;
    bool has_redrive_count = false;
    ATX_TRY(auto event_columns, database_.prepare("PRAGMA table_info(agent_events)"));
    while (true) {
      ATX_TRY(const auto step, event_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      events_exist = true;
      const std::string_view name = event_columns.column_text(1);
      has_root_sequence = has_root_sequence || name == "root_sequence";
      has_redrive_count = has_redrive_count || name == "redrive_count";
    }
    if (events_exist && !has_root_sequence) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE agent_events ADD COLUMN root_sequence INTEGER NOT NULL DEFAULT 0 "
          "CHECK(root_sequence>=0)"));
    }
    if (events_exist && !has_redrive_count) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE agent_events ADD COLUMN redrive_count INTEGER NOT NULL DEFAULT 0 "
          "CHECK(redrive_count>=0)"));
    }
    bool has_max_redrive_count = false;
    ATX_TRY(auto consumer_columns, database_.prepare("PRAGMA table_info(event_consumers)"));
    while (true) {
      ATX_TRY(const auto step, consumer_columns.step());
      if (step == Statement::Step::Done) {
        break;
      }
      has_max_redrive_count =
          has_max_redrive_count || consumer_columns.column_text(1) == "max_redrive_count";
    }
    if (!has_max_redrive_count) {
      ATX_TRY_VOID(database_.exec(
          "ALTER TABLE event_consumers ADD COLUMN max_redrive_count INTEGER NOT NULL DEFAULT 0 "
          "CHECK(max_redrive_count BETWEEN 0 AND 1000)"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE event_consumers SET max_redrive_count=0"));

    struct RedriveLineageMigration {
      std::string workspace;
      i64 original_sequence{};
      i64 redriven_sequence{};
    };
    std::vector<RedriveLineageMigration> mappings;
    if (events_exist) {
      ATX_TRY_VOID(
          database_.exec("UPDATE agent_events SET root_sequence=sequence,redrive_count=0"));
      ATX_TRY(auto mapping_query,
              database_.prepare("SELECT workspace,original_sequence,redriven_sequence FROM "
                                "event_consumer_redrive_events ORDER BY redriven_sequence"));
      while (true) {
        ATX_TRY(const auto step, mapping_query.step());
        if (step == Statement::Step::Done) {
          break;
        }
        mappings.push_back({std::string{mapping_query.column_text(0)}, mapping_query.column_int(1),
                            mapping_query.column_int(2)});
      }
      ATX_TRY_VOID(mapping_query.reset());
      for (const auto &mapping : mappings) {
        if (mapping.original_sequence < 1 ||
            mapping.redriven_sequence <= mapping.original_sequence) {
          return Err(ErrorCode::Internal, "historical redrive lineage order is invalid");
        }
        ATX_TRY(auto source,
                database_.prepare("SELECT root_sequence,redrive_count FROM agent_events WHERE "
                                  "workspace=?1 AND sequence=?2"));
        ATX_TRY_VOID(source.bind(1, mapping.workspace));
        ATX_TRY_VOID(source.bind(2, mapping.original_sequence));
        ATX_TRY(const auto source_step, source.step());
        if (source_step != Statement::Step::Row || source.column_int(0) < 1 ||
            source.column_int(1) < 0 || source.column_int(1) == std::numeric_limits<i64>::max()) {
          return Err(ErrorCode::Internal, "historical redrive lineage source is invalid");
        }
        const i64 root_sequence = source.column_int(0);
        const i64 redrive_count = source.column_int(1) + 1;
        ATX_TRY_VOID(source.reset());
        ATX_TRY(auto target,
                database_.prepare("UPDATE agent_events SET root_sequence=?3,redrive_count=?4 WHERE "
                                  "workspace=?1 AND sequence=?2 AND root_sequence=sequence AND "
                                  "redrive_count=0"));
        ATX_TRY_VOID(target.bind(1, mapping.workspace));
        ATX_TRY_VOID(target.bind(2, mapping.redriven_sequence));
        ATX_TRY_VOID(target.bind(3, root_sequence));
        ATX_TRY_VOID(target.bind(4, redrive_count));
        ATX_TRY_VOID(step_done(target));
        if (database_.changes() != 1) {
          return Err(ErrorCode::Internal, "historical redrive lineage target is invalid");
        }
      }
      ATX_TRY_VOID(database_.exec("CREATE INDEX IF NOT EXISTS agent_events_lineage_idx ON "
                                  "agent_events(workspace,root_sequence,redrive_count,sequence)"));
    }
    ATX_TRY_VOID(database_.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS event_consumer_redrive_events_target_idx ON "
        "event_consumer_redrive_events(redriven_sequence);"
        "UPDATE agent_db_meta SET value='19' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "19";
    migrated = true;
  }
  if (current_version == "19") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    ATX_TRY_VOID(database_.exec(
        "CREATE TABLE IF NOT EXISTS event_consumer_dead_letter_quarantines("
        "id INTEGER PRIMARY KEY,workspace TEXT NOT NULL,consumer_name TEXT NOT NULL,"
        "dead_letter_id INTEGER NOT NULL,quarantine_token TEXT NOT NULL "
        "CHECK(quarantine_token<>''),quarantined_by TEXT NOT NULL CHECK(quarantined_by<>''),"
        "reason TEXT NOT NULL CHECK(reason<>''),quarantined_at TEXT NOT NULL DEFAULT "
        "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),UNIQUE(workspace,consumer_name,dead_letter_id),"
        "UNIQUE(workspace,consumer_name,quarantine_token),FOREIGN KEY(workspace,consumer_name) "
        "REFERENCES event_consumers(workspace,name),FOREIGN KEY(dead_letter_id) REFERENCES "
        "event_consumer_dead_letters(id)) STRICT;"
        "CREATE INDEX IF NOT EXISTS event_consumer_dead_letter_quarantines_consumer_idx ON "
        "event_consumer_dead_letter_quarantines(workspace,consumer_name,id);"
        "UPDATE agent_db_meta SET value='20' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "20";
    migrated = true;
  }
  if (current_version == "20") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    ATX_TRY(auto lifecycle_sources,
            database_.prepare("SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN "
                              "('event_consumers','agent_events','event_consumer_dead_letters',"
                              "'event_consumer_dead_letter_quarantines')"));
    ATX_TRY(const auto lifecycle_sources_step, lifecycle_sources.step());
    if (lifecycle_sources_step != Statement::Step::Row) {
      return Err(ErrorCode::Internal, "could not inspect lifecycle migration sources");
    }
    const bool can_backfill_lifecycle = lifecycle_sources.column_int(0) == 4;
    ATX_TRY_VOID(database_.exec(
        "CREATE TABLE IF NOT EXISTS event_consumer_lifecycle_epochs("
        "workspace TEXT PRIMARY KEY,activated_at TEXT NOT NULL,event_high_watermark INTEGER NOT "
        "NULL CHECK(event_high_watermark>=0)) WITHOUT ROWID, STRICT;"
        "CREATE TABLE IF NOT EXISTS event_consumer_dead_letter_lifecycle_events("
        "workspace TEXT NOT NULL,consumer_name TEXT NOT NULL,dead_letter_id INTEGER NOT NULL,"
        "transition TEXT NOT NULL CHECK(transition IN ('dead_lettered','redriven','quarantined')),"
        "event_sequence INTEGER,transition_at TEXT NOT NULL,legacy INTEGER NOT NULL DEFAULT 0 "
        "CHECK(legacy IN (0,1)),PRIMARY KEY(workspace,consumer_name,dead_letter_id,transition),"
        "UNIQUE(event_sequence),FOREIGN KEY(workspace,consumer_name) REFERENCES "
        "event_consumers(workspace,name),FOREIGN KEY(dead_letter_id) REFERENCES "
        "event_consumer_dead_letters(id),FOREIGN KEY(event_sequence) REFERENCES "
        "agent_events(sequence),CHECK((legacy=1 AND event_sequence IS NULL) OR (legacy=0 AND "
        "event_sequence IS NOT NULL AND event_sequence>0))) WITHOUT ROWID, STRICT;"
        "CREATE INDEX IF NOT EXISTS event_consumer_dead_letter_lifecycle_consumer_idx ON "
        "event_consumer_dead_letter_lifecycle_events(workspace,consumer_name,dead_letter_id);"));
    if (can_backfill_lifecycle) {
      ATX_TRY_VOID(database_.exec(
          "INSERT OR IGNORE INTO event_consumer_lifecycle_epochs(workspace,activated_at,"
          "event_high_watermark) SELECT w.workspace,max(strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
          "COALESCE((SELECT max(l.created_at) FROM event_consumer_dead_letters l WHERE "
          "l.workspace=w.workspace),''),COALESCE((SELECT max(l.redriven_at) FROM "
          "event_consumer_dead_letters l WHERE l.workspace=w.workspace),''),COALESCE((SELECT "
          "max(q.quarantined_at) FROM event_consumer_dead_letter_quarantines q WHERE "
          "q.workspace=w.workspace),'')),COALESCE((SELECT max(e.sequence) FROM agent_events e "
          "WHERE e.workspace=w.workspace),0) FROM (SELECT DISTINCT workspace FROM "
          "event_consumers) w;"
          "INSERT OR IGNORE INTO event_consumer_dead_letter_lifecycle_events(workspace,"
          "consumer_name,dead_letter_id,transition,event_sequence,transition_at,legacy) SELECT "
          "workspace,consumer_name,id,'dead_lettered',NULL,created_at,1 FROM "
          "event_consumer_dead_letters;"
          "INSERT OR IGNORE INTO event_consumer_dead_letter_lifecycle_events(workspace,"
          "consumer_name,dead_letter_id,transition,event_sequence,transition_at,legacy) SELECT "
          "workspace,consumer_name,id,'redriven',NULL,redriven_at,1 FROM "
          "event_consumer_dead_letters WHERE status='redriven';"
          "INSERT OR IGNORE INTO event_consumer_dead_letter_lifecycle_events(workspace,"
          "consumer_name,dead_letter_id,transition,event_sequence,transition_at,legacy) SELECT "
          "workspace,consumer_name,dead_letter_id,'quarantined',NULL,quarantined_at,1 FROM "
          "event_consumer_dead_letter_quarantines;"));
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='21' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "21";
    migrated = true;
  }
  if (current_version == "21") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    bool consumers_exist = false;
    bool events_exist = false;
    ATX_TRY(auto sources,
            database_.prepare("SELECT name FROM sqlite_schema WHERE type='table' AND name IN "
                              "('event_consumers','agent_events')"));
    while (true) {
      ATX_TRY(const auto step, sources.step());
      if (step == Statement::Step::Done) {
        break;
      }
      consumers_exist = consumers_exist || sources.column_text(0) == "event_consumers";
      events_exist = events_exist || sources.column_text(0) == "agent_events";
    }
    if (consumers_exist) {
      bool has_self_control_cutoff = false;
      ATX_TRY(auto columns, database_.prepare("PRAGMA table_info(event_consumers)"));
      while (true) {
        ATX_TRY(const auto step, columns.step());
        if (step == Statement::Step::Done) {
          break;
        }
        has_self_control_cutoff = has_self_control_cutoff ||
                                  columns.column_text(1) == "self_control_event_cutoff_sequence";
      }
      if (!has_self_control_cutoff) {
        ATX_TRY_VOID(database_.exec(
            "ALTER TABLE event_consumers ADD COLUMN self_control_event_cutoff_sequence INTEGER "
            "NOT NULL DEFAULT 0 CHECK(self_control_event_cutoff_sequence>=0)"));
      }
      if (events_exist) {
        ATX_TRY_VOID(database_.exec(
            "UPDATE event_consumers SET self_control_event_cutoff_sequence=COALESCE((SELECT "
            "max(e.sequence) FROM agent_events e WHERE e.workspace=event_consumers.workspace),"
            "0)"));
      }
    }
    ATX_TRY_VOID(database_.exec("UPDATE agent_db_meta SET value='22' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "22";
    migrated = true;
  }
  if (current_version == "22") {
    ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
    ATX_TRY_VOID(database_.exec(kConsumerStateRevisionSchema));
    ATX_TRY_VOID(database_.exec(
        "INSERT OR IGNORE INTO event_consumer_state_revisions(workspace,revision,updated_at) "
        "SELECT DISTINCT workspace,1,strftime('%Y-%m-%dT%H:%M:%fZ','now') FROM "
        "event_consumers;"
        "UPDATE agent_db_meta SET value='23' WHERE key='schema_version'"));
    ATX_TRY_VOID(transaction.commit());
    current_version = "23";
    migrated = true;
  }
  if (current_version != std::to_string(kSchemaVersion)) {
    return Err(ErrorCode::NotImplemented, "unsupported atx-db schema version " + current_version);
  }
  return migrated ? database_.exec(kSchema) : Ok();
}

Result<RunRecord> AgentDatabase::create_run(std::string_view objective,
                                            std::string_view idempotency_key) {
  if (!valid_field(objective, kMaximumPayloadBytes, false) ||
      !valid_field(idempotency_key, kMaximumIdBytes)) {
    return Err(ErrorCode::InvalidArgument, "run objective or idempotency key is invalid");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  if (!idempotency_key.empty()) {
    ATX_TRY(auto existing,
            database_.prepare("SELECT id,objective,status,revision,created_at,updated_at FROM runs "
                              "WHERE workspace=?1 AND idempotency_key=?2"));
    ATX_TRY_VOID(existing.bind(1, workspace_));
    ATX_TRY_VOID(existing.bind(2, idempotency_key));
    ATX_TRY(const auto step, existing.step());
    if (step == Statement::Step::Row) {
      ATX_TRY(auto result, read_run(existing));
      if (result.objective != objective) {
        return Err(ErrorCode::InvalidArgument,
                   "run idempotency key was reused with a different objective");
      }
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(result));
    }
  }
  ATX_TRY(const auto run_id, new_id(database_, "run_"));
  ATX_TRY(auto insert, database_.prepare("INSERT INTO runs(workspace,id,objective,idempotency_key) "
                                         "VALUES(?1,?2,?3,?4)"));
  ATX_TRY_VOID(insert.bind(1, workspace_));
  ATX_TRY_VOID(insert.bind(2, run_id));
  ATX_TRY_VOID(insert.bind(3, objective));
  ATX_TRY_VOID(insert.bind(4, idempotency_key));
  ATX_TRY_VOID(step_done(insert));
  ATX_TRY_VOID(insert_event(database_, workspace_, "run.created", objective, run_id, {}, {}));
  ATX_TRY_VOID(transaction.commit());
  ATX_TRY(auto query,
          database_.prepare("SELECT id,objective,status,revision,created_at,updated_at FROM runs "
                            "WHERE workspace=?1 AND id=?2"));
  ATX_TRY_VOID(query.bind(1, workspace_));
  ATX_TRY_VOID(query.bind(2, run_id));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "created run disappeared");
  }
  return read_run(query);
}

Status AgentDatabase::finish_run(std::string_view run_id, std::string_view status) {
  if ((status != "completed" && status != "failed" && status != "cancelled") ||
      !valid_field(run_id, kMaximumIdBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid run id or terminal status");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  std::vector<std::string> tasks_to_cancel;
  if (status == "completed") {
    ATX_TRY(auto unfinished,
            database_.prepare("SELECT count(*) FROM tasks WHERE workspace=?1 AND run_id=?2 AND "
                              "status NOT IN ('completed','cancelled')"));
    ATX_TRY_VOID(unfinished.bind(1, workspace_));
    ATX_TRY_VOID(unfinished.bind(2, run_id));
    ATX_TRY(const auto step, unfinished.step());
    if (step != Statement::Step::Row || unfinished.column_int(0) != 0) {
      return Err(ErrorCode::InvalidArgument, "run still has unfinished tasks");
    }
  } else {
    ATX_TRY(auto active_tasks,
            database_.prepare("SELECT id FROM tasks WHERE workspace=?1 AND run_id=?2 AND "
                              "status IN ('queued','leased')"));
    ATX_TRY_VOID(active_tasks.bind(1, workspace_));
    ATX_TRY_VOID(active_tasks.bind(2, run_id));
    while (true) {
      ATX_TRY(const auto step, active_tasks.step());
      if (step == Statement::Step::Done) {
        break;
      }
      tasks_to_cancel.emplace_back(active_tasks.column_text(0));
    }
  }
  ATX_TRY(auto update, database_.prepare("UPDATE runs SET status=?3,revision=revision+1,"
                                         "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                                         "WHERE workspace=?1 AND id=?2 AND status='active'"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, run_id));
  ATX_TRY_VOID(update.bind(3, status));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::NotFound, "active run was not found");
  }
  if (!tasks_to_cancel.empty()) {
    ATX_TRY(auto cancel,
            database_.prepare("UPDATE tasks SET status='cancelled',lease_owner='',lease_token='',"
                              "lease_expires_at='',revision=revision+1,updated_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                              "run_id=?2 AND status IN ('queued','leased')"));
    ATX_TRY_VOID(cancel.bind(1, workspace_));
    ATX_TRY_VOID(cancel.bind(2, run_id));
    ATX_TRY_VOID(step_done(cancel));
    for (const auto &task_id : tasks_to_cancel) {
      ATX_TRY_VOID(insert_event(database_, workspace_, "task.cancelled",
                                "run." + std::string{status}, run_id, task_id, {}));
    }
  }
  ATX_TRY(auto stop_agents,
          database_.prepare("UPDATE agents SET status='stopped',revision=revision+1 WHERE "
                            "workspace=?1 AND run_id=?2 AND status<>'stopped'"));
  ATX_TRY_VOID(stop_agents.bind(1, workspace_));
  ATX_TRY_VOID(stop_agents.bind(2, run_id));
  ATX_TRY_VOID(step_done(stop_agents));
  ATX_TRY_VOID(
      insert_event(database_, workspace_, "run." + std::string{status}, {}, run_id, {}, {}));
  ATX_TRY_VOID(transaction.commit());
  return Ok();
}

Result<AgentRecord> AgentDatabase::register_agent(std::string_view run_id,
                                                  std::string_view agent_id, std::string_view role,
                                                  std::string_view capabilities) {
  if (!valid_field(run_id, kMaximumIdBytes, false) ||
      !valid_field(agent_id, kMaximumIdBytes, false) ||
      !valid_field(role, kMaximumTitleBytes, false) ||
      !valid_field(capabilities, kMaximumPayloadBytes)) {
    return Err(ErrorCode::InvalidArgument, "agent registration contains an invalid field");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  {
    ATX_TRY(auto run, database_.prepare("SELECT 1 FROM runs WHERE workspace=?1 AND id=?2 AND "
                                        "status='active'"));
    ATX_TRY_VOID(run.bind(1, workspace_));
    ATX_TRY_VOID(run.bind(2, run_id));
    ATX_TRY(const auto step, run.step());
    if (step != Statement::Step::Row) {
      return Err(ErrorCode::NotFound, "active run was not found");
    }
  }
  {
    ATX_TRY(auto existing,
            database_.prepare("SELECT run_id FROM agents WHERE workspace=?1 AND id=?2"));
    ATX_TRY_VOID(existing.bind(1, workspace_));
    ATX_TRY_VOID(existing.bind(2, agent_id));
    ATX_TRY(const auto step, existing.step());
    if (step == Statement::Step::Row && existing.column_text(0) != run_id) {
      return Err(ErrorCode::AlreadyExists, "agent id is already bound to another run");
    }
  }
  ATX_TRY(auto upsert,
          database_.prepare("INSERT INTO agents(workspace,id,run_id,role,capabilities) "
                            "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(workspace,id) DO UPDATE SET "
                            "role=excluded.role,capabilities=excluded.capabilities,status='active',"
                            "revision=agents.revision+1,last_heartbeat_at="
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
  ATX_TRY_VOID(upsert.bind(1, workspace_));
  ATX_TRY_VOID(upsert.bind(2, agent_id));
  ATX_TRY_VOID(upsert.bind(3, run_id));
  ATX_TRY_VOID(upsert.bind(4, role));
  ATX_TRY_VOID(upsert.bind(5, capabilities));
  ATX_TRY_VOID(step_done(upsert));
  ATX_TRY_VOID(insert_event(database_, workspace_, "agent.registered", role, run_id, {}, agent_id));
  ATX_TRY_VOID(transaction.commit());
  ATX_TRY(auto query,
          database_.prepare("SELECT id,run_id,role,capabilities,status,revision,last_heartbeat_at "
                            "FROM agents WHERE workspace=?1 AND id=?2"));
  ATX_TRY_VOID(query.bind(1, workspace_));
  ATX_TRY_VOID(query.bind(2, agent_id));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "registered agent disappeared");
  }
  return read_agent(query);
}

Status AgentDatabase::heartbeat(std::string_view agent_id, i64 expected_revision) {
  if (!valid_field(agent_id, kMaximumIdBytes, false) || expected_revision < -1) {
    return Err(ErrorCode::InvalidArgument, "invalid agent id or revision");
  }
  const std::string sql =
      "UPDATE agents SET last_heartbeat_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
      "revision=revision+1,status='active' WHERE workspace=?1 AND id=?2" +
      std::string{expected_revision >= 0 ? " AND revision=?3" : ""};
  ATX_TRY(auto update, database_.prepare(sql));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, agent_id));
  if (expected_revision >= 0) {
    ATX_TRY_VOID(update.bind(3, expected_revision));
  }
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "agent revision conflict or agent was not found");
  }
  return Ok();
}

Result<TaskRecord> AgentDatabase::add_task(const TaskSpec &task) {
  if (!valid_field(task.run_id, kMaximumIdBytes, false) ||
      !valid_field(task.title, kMaximumTitleBytes, false) ||
      !valid_field(task.description, kMaximumPayloadBytes) ||
      !valid_field(task.idempotency_key, kMaximumIdBytes) || task.max_attempts < 1 ||
      task.max_attempts > 1'000 || task.dependencies.size() > 10'000) {
    return Err(ErrorCode::InvalidArgument, "task contains an invalid field");
  }
  auto unique_dependencies = task.dependencies;
  std::sort(unique_dependencies.begin(), unique_dependencies.end());
  if (std::adjacent_find(unique_dependencies.begin(), unique_dependencies.end()) !=
      unique_dependencies.end()) {
    return Err(ErrorCode::InvalidArgument, "task dependencies contain a duplicate");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  if (!task.idempotency_key.empty()) {
    ATX_TRY(auto existing,
            database_.prepare("SELECT " + std::string{kTaskColumns} +
                              " FROM tasks WHERE workspace=?1 AND idempotency_key=?2"));
    ATX_TRY_VOID(existing.bind(1, workspace_));
    ATX_TRY_VOID(existing.bind(2, task.idempotency_key));
    ATX_TRY(const auto step, existing.step());
    if (step == Statement::Step::Row) {
      ATX_TRY(auto result, read_task(existing));
      std::vector<std::string> stored_dependencies;
      ATX_TRY(auto dependency_query,
              database_.prepare("SELECT depends_on_task_id FROM task_dependencies WHERE "
                                "workspace=?1 AND task_id=?2 ORDER BY depends_on_task_id"));
      ATX_TRY_VOID(dependency_query.bind(1, workspace_));
      ATX_TRY_VOID(dependency_query.bind(2, result.id));
      while (true) {
        ATX_TRY(const auto dependency_step, dependency_query.step());
        if (dependency_step == Statement::Step::Done) {
          break;
        }
        stored_dependencies.emplace_back(dependency_query.column_text(0));
      }
      auto requested_dependencies = task.dependencies;
      std::sort(requested_dependencies.begin(), requested_dependencies.end());
      if (result.run_id != task.run_id || result.title != task.title ||
          result.description != task.description || result.priority != task.priority ||
          result.max_attempts != task.max_attempts ||
          stored_dependencies != requested_dependencies) {
        return Err(ErrorCode::InvalidArgument,
                   "task idempotency key was reused with a different request");
      }
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(result));
    }
  }
  {
    ATX_TRY(auto run, database_.prepare("SELECT 1 FROM runs WHERE workspace=?1 AND id=?2 AND "
                                        "status='active'"));
    ATX_TRY_VOID(run.bind(1, workspace_));
    ATX_TRY_VOID(run.bind(2, task.run_id));
    ATX_TRY(const auto step, run.step());
    if (step != Statement::Step::Row) {
      return Err(ErrorCode::NotFound, "active run was not found");
    }
  }
  for (const auto &dependency : task.dependencies) {
    if (!valid_field(dependency, kMaximumIdBytes, false)) {
      return Err(ErrorCode::InvalidArgument, "dependency id is invalid");
    }
    ATX_TRY(auto query, database_.prepare(
                            "SELECT status FROM tasks WHERE workspace=?1 AND run_id=?2 AND id=?3"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY_VOID(query.bind(2, task.run_id));
    ATX_TRY_VOID(query.bind(3, dependency));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row) {
      return Err(ErrorCode::NotFound, "dependency task was not found in the run");
    }
    if (query.column_text(0) == "failed" || query.column_text(0) == "cancelled") {
      return Err(ErrorCode::InvalidArgument,
                 "a new task cannot depend on a terminal unsuccessful task");
    }
  }
  ATX_TRY(const auto task_id, new_id(database_, "task_"));
  ATX_TRY(auto insert,
          database_.prepare("INSERT INTO tasks(workspace,id,run_id,title,description,priority,"
                            "max_attempts,idempotency_key) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"));
  ATX_TRY_VOID(insert.bind(1, workspace_));
  ATX_TRY_VOID(insert.bind(2, task_id));
  ATX_TRY_VOID(insert.bind(3, task.run_id));
  ATX_TRY_VOID(insert.bind(4, task.title));
  ATX_TRY_VOID(insert.bind(5, task.description));
  ATX_TRY_VOID(insert.bind(6, task.priority));
  ATX_TRY_VOID(insert.bind(7, task.max_attempts));
  ATX_TRY_VOID(insert.bind(8, task.idempotency_key));
  ATX_TRY_VOID(step_done(insert));
  for (const auto &dependency : task.dependencies) {
    ATX_TRY(auto edge, database_.prepare("INSERT INTO task_dependencies(workspace,run_id,task_id,"
                                         "depends_on_task_id) VALUES(?1,?2,?3,?4)"));
    ATX_TRY_VOID(edge.bind(1, workspace_));
    ATX_TRY_VOID(edge.bind(2, task.run_id));
    ATX_TRY_VOID(edge.bind(3, task_id));
    ATX_TRY_VOID(edge.bind(4, dependency));
    ATX_TRY_VOID(step_done(edge));
  }
  ATX_TRY_VOID(
      insert_event(database_, workspace_, "task.created", task.title, task.run_id, task_id, {}));
  ATX_TRY_VOID(transaction.commit());
  return find_task(database_, workspace_, task_id);
}

Result<TaskRecord> AgentDatabase::claim_next(std::string_view agent_id, i64 lease_seconds) {
  if (!valid_field(agent_id, kMaximumIdBytes, false) || lease_seconds < 1 ||
      lease_seconds > kMaximumLeaseSeconds) {
    return Err(ErrorCode::InvalidArgument, "invalid agent id or lease duration");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(const auto reclaim_time,
          scalar_text(database_, "SELECT strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
  struct ExpiredLease {
    std::string task_id;
    std::string run_id;
    bool terminal{};
  };
  std::vector<ExpiredLease> expired;
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT id,run_id,attempts>=max_attempts FROM tasks WHERE "
                              "workspace=?1 AND status='leased' AND lease_expires_at<=?2"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY_VOID(query.bind(2, reclaim_time));
    while (true) {
      ATX_TRY(const auto step, query.step());
      if (step == Statement::Step::Done) {
        break;
      }
      expired.push_back({std::string{query.column_text(0)}, std::string{query.column_text(1)},
                         query.column_int(2) != 0});
    }
  }
  for (const auto &lease : expired) {
    ATX_TRY(auto reclaim,
            database_.prepare("UPDATE tasks SET status=CASE WHEN attempts>=max_attempts THEN "
                              "'failed' ELSE 'queued' END,lease_owner='',lease_token='',"
                              "lease_expires_at='',last_error='lease expired',revision=revision+1,"
                              "last_transition_token=lease_token,"
                              "last_transition_kind='lease_expired',"
                              "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                              "AND id=?2 AND status='leased' AND lease_expires_at<=?3"));
    ATX_TRY_VOID(reclaim.bind(1, workspace_));
    ATX_TRY_VOID(reclaim.bind(2, lease.task_id));
    ATX_TRY_VOID(reclaim.bind(3, reclaim_time));
    ATX_TRY_VOID(step_done(reclaim));
    if (database_.changes() != 1) {
      return Err(ErrorCode::Internal, "expired lease changed during an immediate transaction");
    }
    ATX_TRY_VOID(insert_event(database_, workspace_, "task.lease_expired", {}, lease.run_id,
                              lease.task_id, {}));
    if (lease.terminal) {
      ATX_TRY_VOID(insert_event(database_, workspace_, "task.failed", "lease expired", lease.run_id,
                                lease.task_id, {}));
      ATX_TRY(auto ignored, cancel_dependency_descendants(database_, workspace_, lease.run_id,
                                                          lease.task_id, "failed"));
      (void)ignored;
    }
  }
  std::string run_id;
  {
    ATX_TRY(auto agent, database_.prepare(
                            "SELECT a.run_id FROM agents a JOIN runs r ON r.workspace=a.workspace "
                            "AND r.id=a.run_id WHERE a.workspace=?1 AND a.id=?2 AND "
                            "a.status='active' AND r.status='active'"));
    ATX_TRY_VOID(agent.bind(1, workspace_));
    ATX_TRY_VOID(agent.bind(2, agent_id));
    ATX_TRY(const auto step, agent.step());
    if (step != Statement::Step::Row) {
      return Err(ErrorCode::NotFound, "active agent and run were not found");
    }
    run_id = agent.column_text(0);
  }
  {
    const std::string sql = "SELECT " + std::string{kTaskColumns} +
                            " FROM tasks WHERE workspace=?1 AND run_id=?2 AND status='leased' "
                            "AND lease_owner=?3 AND lease_expires_at>"
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now') ORDER BY created_at,id LIMIT 1";
    ATX_TRY(auto existing_lease, database_.prepare(sql));
    ATX_TRY_VOID(existing_lease.bind(1, workspace_));
    ATX_TRY_VOID(existing_lease.bind(2, run_id));
    ATX_TRY_VOID(existing_lease.bind(3, agent_id));
    ATX_TRY(const auto step, existing_lease.step());
    if (step == Statement::Step::Row) {
      ATX_TRY(auto result, read_task(existing_lease));
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(result));
    }
  }
  std::string task_id;
  {
    ATX_TRY(
        auto candidate,
        database_.prepare("SELECT t.id FROM tasks t WHERE t.workspace=?1 AND t.run_id=?2 AND "
                          "t.status='queued' AND t.attempts<t.max_attempts AND NOT EXISTS("
                          "SELECT 1 FROM task_dependencies d JOIN tasks parent ON "
                          "parent.workspace=d.workspace AND parent.id=d.depends_on_task_id "
                          "WHERE d.workspace=t.workspace AND d.task_id=t.id AND "
                          "parent.status<>'completed') ORDER BY t.priority DESC,t.created_at,t.id "
                          "LIMIT 1"));
    ATX_TRY_VOID(candidate.bind(1, workspace_));
    ATX_TRY_VOID(candidate.bind(2, run_id));
    ATX_TRY(const auto step, candidate.step());
    if (step != Statement::Step::Row) {
      return Err(ErrorCode::NotFound, "no eligible task is available");
    }
    task_id = candidate.column_text(0);
  }
  ATX_TRY(const auto lease_token, new_id(database_, "lease_"));
  ATX_TRY(auto claim,
          database_.prepare("UPDATE tasks SET status='leased',lease_owner=?3,lease_token=?4,"
                            "lease_expires_at=strftime('%Y-%m-%dT%H:%M:%fZ','now','+' || ?5 || "
                            "' seconds'),attempts=attempts+1,revision=revision+1,"
                            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                            "AND id=?2 AND status='queued'"));
  ATX_TRY_VOID(claim.bind(1, workspace_));
  ATX_TRY_VOID(claim.bind(2, task_id));
  ATX_TRY_VOID(claim.bind(3, agent_id));
  ATX_TRY_VOID(claim.bind(4, lease_token));
  ATX_TRY_VOID(claim.bind(5, lease_seconds));
  ATX_TRY_VOID(step_done(claim));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "task claim lost an atomic update race");
  }
  ATX_TRY_VOID(insert_event(database_, workspace_, "task.claimed", {}, run_id, task_id, agent_id));
  ATX_TRY(auto result, find_task(database_, workspace_, task_id));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(result));
}

Result<TaskRecord> AgentDatabase::renew_lease(std::string_view task_id, std::string_view agent_id,
                                              std::string_view lease_token, i64 lease_seconds) {
  if (!valid_field(task_id, kMaximumIdBytes, false) ||
      !valid_field(agent_id, kMaximumIdBytes, false) ||
      !valid_field(lease_token, kMaximumIdBytes, false) || lease_seconds < 1 ||
      lease_seconds > kMaximumLeaseSeconds) {
    return Err(ErrorCode::InvalidArgument, "invalid lease renewal request");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(auto update,
          database_.prepare("UPDATE tasks SET lease_expires_at="
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now','+' || ?5 || ' seconds'),"
                            "revision=revision+1,updated_at="
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND id=?2 "
                            "AND status='leased' AND lease_owner=?3 AND lease_token=?4 AND "
                            "lease_expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now') AND EXISTS("
                            "SELECT 1 FROM runs r WHERE r.workspace=tasks.workspace AND "
                            "r.id=tasks.run_id AND r.status='active')"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, task_id));
  ATX_TRY_VOID(update.bind(3, agent_id));
  ATX_TRY_VOID(update.bind(4, lease_token));
  ATX_TRY_VOID(update.bind(5, lease_seconds));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "lease token is stale or the lease has expired");
  }
  ATX_TRY(auto result, find_task(database_, workspace_, task_id));
  ATX_TRY_VOID(insert_event(database_, workspace_, "task.lease_renewed", {}, result.run_id, task_id,
                            agent_id));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(result));
}

Status AgentDatabase::complete_task(std::string_view task_id, std::string_view agent_id,
                                    std::string_view lease_token,
                                    std::string_view result_source_id) {
  return complete_task_internal(task_id, agent_id, lease_token, result_source_id, 0, {});
}

Status AgentDatabase::complete_task_verified(std::string_view task_id, std::string_view agent_id,
                                             std::string_view lease_token,
                                             std::string_view result_source_id,
                                             i64 result_observation_id,
                                             atx::kb::KnowledgeBase &knowledge_base) {
  ATX_TRY(auto source, knowledge_base.get_source(result_source_id));
  const bool observation_exists =
      std::any_of(source.observations.begin(), source.observations.end(),
                  [&](const auto &observation) { return observation.id == result_observation_id; });
  if (!observation_exists) {
    return Err(ErrorCode::InvalidArgument,
               "task result observation does not belong to the requested knowledge source");
  }
  return complete_task_internal(task_id, agent_id, lease_token, result_source_id,
                                result_observation_id, source.content_hash);
}

Status AgentDatabase::complete_task_internal(std::string_view task_id, std::string_view agent_id,
                                             std::string_view lease_token,
                                             std::string_view result_source_id,
                                             i64 result_observation_id,
                                             std::string_view result_content_hash) {
  if (!valid_field(task_id, kMaximumIdBytes, false) ||
      !valid_field(agent_id, kMaximumIdBytes, false) ||
      !valid_field(lease_token, kMaximumIdBytes, false) ||
      !valid_field(result_source_id, kMaximumIdBytes) || result_observation_id < 0 ||
      (!result_content_hash.empty() && !valid_sha256(result_content_hash))) {
    return Err(ErrorCode::InvalidArgument, "invalid task completion request");
  }
  const bool verified = result_observation_id > 0 && !result_content_hash.empty();
  if ((result_source_id.empty() && (result_observation_id != 0 || !result_content_hash.empty())) ||
      (!verified && result_observation_id != 0) || (verified && result_source_id.empty())) {
    return Err(ErrorCode::InvalidArgument, "task result evidence fields are inconsistent");
  }
  const std::string_view evidence_status =
      verified ? "verified" : (result_source_id.empty() ? "none" : "unverified");
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(auto before, find_task(database_, workspace_, task_id));
  if (before.status == "completed") {
    ATX_TRY(auto prior,
            database_.prepare("SELECT last_transition_token,last_transition_kind FROM tasks "
                              "WHERE workspace=?1 AND id=?2"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, task_id));
    ATX_TRY(const auto step, prior.step());
    if (step == Statement::Step::Row && prior.column_text(0) == lease_token &&
        prior.column_text(1) == "completed" && before.result_source_id == result_source_id &&
        before.result_observation_id == result_observation_id &&
        before.result_content_hash == result_content_hash &&
        before.result_evidence_status == evidence_status) {
      ATX_TRY_VOID(transaction.commit());
      return Ok();
    }
    return Err(ErrorCode::Unavailable, "task was completed by a different transition");
  }
  ATX_TRY(auto update,
          database_.prepare("UPDATE tasks SET status='completed',result_source_id=?5,"
                            "result_observation_id=?6,result_content_hash=?7,"
                            "result_evidence_status=?8,"
                            "lease_owner='',lease_token='',lease_expires_at='',revision=revision+1,"
                            "last_transition_token=?4,last_transition_kind='completed',"
                            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                            "AND id=?2 AND status='leased' AND lease_owner=?3 AND lease_token=?4 "
                            "AND lease_expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now') AND EXISTS("
                            "SELECT 1 FROM runs r WHERE r.workspace=tasks.workspace AND "
                            "r.id=tasks.run_id AND r.status='active')"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, task_id));
  ATX_TRY_VOID(update.bind(3, agent_id));
  ATX_TRY_VOID(update.bind(4, lease_token));
  ATX_TRY_VOID(update.bind(5, result_source_id));
  ATX_TRY_VOID(update.bind(6, result_observation_id));
  ATX_TRY_VOID(update.bind(7, result_content_hash));
  ATX_TRY_VOID(update.bind(8, evidence_status));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "lease token is stale or the lease has expired");
  }
  ATX_TRY_VOID(insert_event(database_, workspace_, "task.completed", result_source_id,
                            before.run_id, task_id, agent_id));
  if (verified) {
    ATX_TRY_VOID(insert_event(database_, workspace_, "task.result_verified", result_source_id,
                              before.run_id, task_id, agent_id));
  }
  ATX_TRY_VOID(transaction.commit());
  return Ok();
}

Status AgentDatabase::fail_task(std::string_view task_id, std::string_view agent_id,
                                std::string_view lease_token, std::string_view error) {
  if (!valid_field(task_id, kMaximumIdBytes, false) ||
      !valid_field(agent_id, kMaximumIdBytes, false) ||
      !valid_field(lease_token, kMaximumIdBytes, false) ||
      !valid_field(error, kMaximumPayloadBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid task failure request");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(auto before, find_task(database_, workspace_, task_id));
  if (before.status != "leased") {
    ATX_TRY(auto prior,
            database_.prepare("SELECT last_transition_token,last_transition_kind,last_error FROM "
                              "tasks WHERE workspace=?1 AND id=?2"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, task_id));
    ATX_TRY(const auto step, prior.step());
    if (step == Statement::Step::Row && prior.column_text(0) == lease_token &&
        prior.column_text(1) == "failed" && prior.column_text(2) == error) {
      ATX_TRY_VOID(transaction.commit());
      return Ok();
    }
    return Err(ErrorCode::Unavailable, "task was transitioned by a different lease");
  }
  ATX_TRY(auto update, database_.prepare(
                           "UPDATE tasks SET status=CASE WHEN attempts>=max_attempts THEN 'failed' "
                           "ELSE 'queued' END,last_error=?5,lease_owner='',lease_token='',"
                           "last_transition_token=?4,last_transition_kind='failed',"
                           "lease_expires_at='',revision=revision+1,updated_at="
                           "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND id=?2 "
                           "AND status='leased' AND lease_owner=?3 AND lease_token=?4 AND "
                           "lease_expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now') AND EXISTS("
                           "SELECT 1 FROM runs r WHERE r.workspace=tasks.workspace AND "
                           "r.id=tasks.run_id AND r.status='active')"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, task_id));
  ATX_TRY_VOID(update.bind(3, agent_id));
  ATX_TRY_VOID(update.bind(4, lease_token));
  ATX_TRY_VOID(update.bind(5, error));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "lease token is stale or the lease has expired");
  }
  ATX_TRY(auto after, find_task(database_, workspace_, task_id));
  const std::string event_type = after.status == "failed" ? "task.failed" : "task.requeued";
  ATX_TRY_VOID(
      insert_event(database_, workspace_, event_type, error, before.run_id, task_id, agent_id));
  if (after.status == "failed") {
    ATX_TRY(auto ignored,
            cancel_dependency_descendants(database_, workspace_, before.run_id, task_id, "failed"));
    (void)ignored;
  }
  ATX_TRY_VOID(transaction.commit());
  return Ok();
}

Result<TaskRecord> AgentDatabase::get_task(std::string_view task_id) {
  if (!valid_field(task_id, kMaximumIdBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "task id is invalid");
  }
  ATX_TRY(auto task, find_task(database_, workspace_, task_id));
  task.lease_token.clear();
  return Ok(std::move(task));
}

Result<std::vector<TaskRecord>> AgentDatabase::list_tasks(std::string_view run_id, usize limit) {
  if (!valid_field(run_id, kMaximumIdBytes) || limit == 0 || limit > 1'000) {
    return Err(ErrorCode::InvalidArgument, "invalid run filter or task limit");
  }
  std::string sql = "SELECT " + std::string{kTaskColumns} + " FROM tasks WHERE workspace=?1";
  if (!run_id.empty()) {
    sql += " AND run_id=?2";
  }
  sql += " ORDER BY created_at,id LIMIT ?" + std::to_string(run_id.empty() ? 2 : 3);
  ATX_TRY(auto query, database_.prepare(sql));
  ATX_TRY_VOID(query.bind(1, workspace_));
  i64 limit_parameter = 2;
  if (!run_id.empty()) {
    ATX_TRY_VOID(query.bind(2, run_id));
    limit_parameter = 3;
  }
  ATX_TRY_VOID(query.bind(limit_parameter, static_cast<i64>(limit)));
  std::vector<TaskRecord> result;
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    ATX_TRY(auto task, read_task(query));
    task.lease_token.clear();
    result.push_back(std::move(task));
  }
  return Ok(std::move(result));
}

Result<i64> AgentDatabase::append_event(std::string_view type, std::string_view payload,
                                        std::string_view run_id, std::string_view task_id,
                                        std::string_view agent_id, std::string_view idempotency_key,
                                        std::string_view subject) {
  if (!valid_field(type, kMaximumTitleBytes, false) ||
      !valid_field(payload, kMaximumPayloadBytes) || !valid_field(run_id, kMaximumIdBytes) ||
      !valid_field(task_id, kMaximumIdBytes) || !valid_field(agent_id, kMaximumIdBytes) ||
      !valid_field(idempotency_key, kMaximumIdBytes) || !valid_field(subject, kMaximumTitleBytes)) {
    return Err(ErrorCode::InvalidArgument, "event contains an invalid field");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(const auto sequence, insert_event(database_, workspace_, type, payload, run_id, task_id,
                                            agent_id, idempotency_key, subject));
  ATX_TRY_VOID(transaction.commit());
  return Ok(sequence);
}

Result<std::vector<TaskEvent>> AgentDatabase::events_after(i64 sequence, usize limit,
                                                           std::string_view subject) {
  if (sequence < 0 || limit == 0 || limit > 1'000 || !valid_field(subject, kMaximumTitleBytes)) {
    return Err(ErrorCode::InvalidArgument, "invalid event cursor or limit");
  }
  return events_after_filtered(database_, workspace_, sequence, limit, subject);
}

Result<EventConsumerRecord> AgentDatabase::register_event_consumer(
    std::string_view name, std::string_view subject_filter, i64 start_sequence,
    i64 max_delivery_attempts, i64 retry_backoff_seconds, i64 retry_backoff_max_seconds,
    EventConsumerRetryJitter retry_jitter, i64 redrive_rate_per_second, i64 redrive_burst_events,
    i64 max_redrive_count) {
  if (!valid_field(name, kMaximumIdBytes, false) ||
      !valid_field(subject_filter, kMaximumTitleBytes) || start_sequence < 0 ||
      max_delivery_attempts < 0 || max_delivery_attempts > 1'000 || retry_backoff_seconds < 0 ||
      retry_backoff_seconds > kMaximumLeaseSeconds || retry_backoff_max_seconds < 0 ||
      retry_backoff_max_seconds > kMaximumLeaseSeconds ||
      ((retry_backoff_seconds == 0 || retry_backoff_max_seconds == 0) &&
       retry_backoff_seconds != retry_backoff_max_seconds) ||
      retry_backoff_max_seconds < retry_backoff_seconds || redrive_rate_per_second < 0 ||
      redrive_rate_per_second > kMaximumRedriveRatePerSecond || redrive_burst_events < 0 ||
      redrive_burst_events > kMaximumRedriveBurstEvents ||
      ((redrive_rate_per_second == 0 || redrive_burst_events == 0) &&
       redrive_rate_per_second != redrive_burst_events) ||
      max_redrive_count < 0 || max_redrive_count > 1'000) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer registration");
  }
  std::string_view retry_jitter_name;
  switch (retry_jitter) {
  case EventConsumerRetryJitter::None:
    retry_jitter_name = "none";
    break;
  case EventConsumerRetryJitter::Full:
    retry_jitter_name = "full";
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "invalid event consumer retry jitter policy");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY_VOID(ensure_event_consumer_lifecycle_epoch(database_, workspace_));
  ATX_TRY(
      auto high_watermark,
      database_.prepare("SELECT COALESCE(max(sequence),0) FROM agent_events WHERE workspace=?1"));
  ATX_TRY_VOID(high_watermark.bind(1, workspace_));
  ATX_TRY(const auto high_watermark_step, high_watermark.step());
  if (high_watermark_step != Statement::Step::Row ||
      start_sequence > high_watermark.column_int(0)) {
    return Err(ErrorCode::InvalidArgument,
               "event consumer cannot start beyond the current event high watermark");
  }
  ATX_TRY(auto insert,
          database_.prepare("INSERT OR IGNORE INTO event_consumers("
                            "workspace,name,subject_filter,start_sequence,cursor_sequence,"
                            "max_delivery_attempts,retry_backoff_seconds,"
                            "retry_backoff_max_seconds,retry_jitter,redrive_rate_per_second,"
                            "redrive_burst_events,redrive_token_millis,redrive_refilled_at,"
                            "max_redrive_count) "
                            "VALUES(?1,?2,?3,?4,?4,?5,?6,?7,?8,?9,?10,?11,CASE WHEN ?9=0 "
                            "THEN '' ELSE strftime('%Y-%m-%dT%H:%M:%fZ','now') END,?12)"));
  ATX_TRY_VOID(insert.bind(1, workspace_));
  ATX_TRY_VOID(insert.bind(2, name));
  ATX_TRY_VOID(insert.bind(3, subject_filter));
  ATX_TRY_VOID(insert.bind(4, start_sequence));
  ATX_TRY_VOID(insert.bind(5, max_delivery_attempts));
  ATX_TRY_VOID(insert.bind(6, retry_backoff_seconds));
  ATX_TRY_VOID(insert.bind(7, retry_backoff_max_seconds));
  ATX_TRY_VOID(insert.bind(8, retry_jitter_name));
  ATX_TRY_VOID(insert.bind(9, redrive_rate_per_second));
  ATX_TRY_VOID(insert.bind(10, redrive_burst_events));
  ATX_TRY_VOID(insert.bind(11, redrive_burst_events * kRedriveTokenUnitsPerEvent));
  ATX_TRY_VOID(insert.bind(12, max_redrive_count));
  ATX_TRY_VOID(step_done(insert));
  const bool inserted = database_.changes() == 1;
  ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
  if (!inserted) {
    if (consumer.subject_filter != subject_filter || consumer.start_sequence != start_sequence ||
        consumer.max_delivery_attempts != max_delivery_attempts ||
        consumer.retry_backoff_seconds != retry_backoff_seconds ||
        consumer.retry_backoff_max_seconds != retry_backoff_max_seconds ||
        consumer.retry_jitter != retry_jitter_name ||
        consumer.redrive_rate_per_second != redrive_rate_per_second ||
        consumer.redrive_burst_events != redrive_burst_events ||
        consumer.max_redrive_count != max_redrive_count) {
      return Err(ErrorCode::InvalidArgument,
                 "event consumer name was reused with a different configuration");
    }
    ATX_TRY_VOID(transaction.commit());
    return Ok(std::move(consumer));
  }
  ATX_TRY_VOID(insert_event(database_, workspace_, "consumer.registered", subject_filter, {}, {},
                            {}, {}, "consumers/" + std::string{name}));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(consumer));
}

Result<EventConsumerRecord> AgentDatabase::get_event_consumer(std::string_view name) {
  if (!valid_field(name, kMaximumIdBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer name");
  }
  return find_event_consumer(database_, workspace_, name);
}

[[nodiscard]] Result<EventConsumerStatus>
find_event_consumer_status(Database &database, std::string_view workspace, std::string_view name,
                           std::string_view observed_at, i64 event_high_watermark = -1,
                           i64 consumer_state_revision = -1) {
  ATX_TRY(auto query,
          database.prepare_cached(
              "WITH selected_consumer AS (SELECT workspace," + std::string{kEventConsumerColumns} +
              ",active_delivery_token,active_delivery_owner,active_delivery_through_sequence,"
              "active_delivery_attempt,active_delivery_expires_at,active_delivery_retry_at,CASE "
              "WHEN ?3='' THEN strftime('%Y-%m-%dT%H:%M:%fZ','now') ELSE ?3 END AS observed_at "
              "FROM event_consumers WHERE workspace=?1 AND name=?2),visible_events AS MATERIALIZED "
              "(SELECT e.sequence,e.created_at FROM agent_events e INDEXED BY "
              "agent_events_poll_idx JOIN selected_consumer c ON e.workspace=c.workspace WHERE "
              "c.subject_filter='' AND e.sequence>c.cursor_sequence AND "
              "(e.sequence<=c.self_control_event_cutoff_sequence OR "
              "substr(e.event_type,1,9)<>'consumer.' OR e.subject<>'consumers/'||c.name) UNION ALL "
              "SELECT e.sequence,e.created_at FROM agent_events e INDEXED BY "
              "agent_events_subject_idx JOIN selected_consumer c ON e.workspace=c.workspace WHERE "
              "c.subject_filter<>'' AND e.subject<>'' AND e.subject=c.subject_filter AND "
              "e.sequence>c.cursor_sequence AND "
              "(e.sequence<=c.self_control_event_cutoff_sequence OR "
              "substr(e.event_type,1,9)<>'consumer.' OR e.subject<>'consumers/'||c.name)),"
              "dead_letters AS MATERIALIZED (SELECT "
              "l.status,l.event_count,l.created_at,EXISTS(SELECT 1 FROM "
              "event_consumer_dead_letter_quarantines q WHERE q.workspace=l.workspace AND "
              "q.consumer_name=l.consumer_name AND q.dead_letter_id=l.id) AS quarantined FROM "
              "event_consumer_dead_letters l JOIN selected_consumer c ON l.workspace=c.workspace "
              "AND l.consumer_name=c.name) SELECT " +
              std::string{kEventConsumerColumns} +
              ",CASE WHEN ?4<0 THEN (SELECT COALESCE(MAX(e.sequence),0) FROM agent_events e JOIN "
              "selected_consumer c ON e.workspace=c.workspace) ELSE ?4 END,(SELECT COUNT(*) FROM "
              "visible_events),(SELECT COALESCE(MIN(sequence),0) FROM visible_events),(SELECT "
              "COALESCE(MAX(sequence),0) FROM visible_events),COALESCE((SELECT created_at FROM "
              "visible_events ORDER BY sequence LIMIT 1),''),CASE WHEN active_delivery_token='' "
              "THEN 'idle' WHEN active_delivery_expires_at>observed_at THEN 'in_flight' WHEN "
              "max_delivery_attempts>0 AND active_delivery_attempt>=max_delivery_attempts THEN "
              "'dead_letter_ready' WHEN active_delivery_retry_at>observed_at THEN "
              "'retry_backoff' ELSE "
              "'redelivery_ready' END,active_delivery_owner,active_delivery_attempt,"
              "active_delivery_through_sequence,active_delivery_expires_at,"
              "active_delivery_retry_at,(SELECT COUNT(*) FROM visible_events WHERE "
              "sequence<=active_delivery_through_sequence),(SELECT COUNT(*) FROM dead_letters),"
              "(SELECT COUNT(*) FROM dead_letters WHERE status='open' AND NOT quarantined),"
              "(SELECT COALESCE(SUM(event_count),0) FROM dead_letters WHERE status='open' AND "
              "NOT quarantined),COALESCE((SELECT created_at FROM dead_letters WHERE "
              "status='open' AND NOT quarantined ORDER BY created_at LIMIT 1),''),(SELECT COUNT(*) "
              "FROM dead_letters WHERE status='redriven'),(SELECT COUNT(*) FROM dead_letters "
              "WHERE quarantined),observed_at,CASE WHEN ?5<0 THEN COALESCE((SELECT r.revision "
              "FROM event_consumer_state_revisions r JOIN selected_consumer c ON "
              "r.workspace=c.workspace),0) ELSE ?5 END FROM selected_consumer"));
  ATX_TRY_VOID(query->bind(1, workspace));
  ATX_TRY_VOID(query->bind(2, name));
  ATX_TRY_VOID(query->bind(3, observed_at));
  ATX_TRY_VOID(query->bind(4, event_high_watermark));
  ATX_TRY_VOID(query->bind(5, consumer_state_revision));
  ATX_TRY(const auto step, query->step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::NotFound, "event consumer was not found");
  }
  ATX_TRY(auto status, read_event_consumer_status(*query));
  ATX_TRY_VOID(query->reset());
  return Ok(std::move(status));
}

Result<EventConsumerStatus> AgentDatabase::get_event_consumer_status(std::string_view name) {
  if (!valid_field(name, kMaximumIdBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer name");
  }
  return find_event_consumer_status(database_, workspace_, name, {});
}

namespace {

[[nodiscard]] Result<EventConsumerFleetCacheValidation>
read_event_consumer_fleet(Database &database, std::string_view workspace,
                          const EventConsumerFleetValidator *cached) {
  constexpr i64 kMaximumFleetConsumers = 1'000;
  ATX_TRY(auto transaction, Transaction::begin(database));
  ATX_TRY(auto metadata,
          database.prepare("SELECT strftime('%Y-%m-%dT%H:%M:%fZ','now'),(SELECT "
                           "COALESCE(MAX(sequence),0) FROM agent_events WHERE workspace=?1),"
                           "(SELECT COUNT(*) FROM (SELECT 1 FROM event_consumers WHERE "
                           "workspace=?1 LIMIT 1001)),COALESCE((SELECT revision FROM "
                           "event_consumer_state_revisions WHERE workspace=?1),0)"));
  ATX_TRY_VOID(metadata.bind(1, workspace));
  ATX_TRY(const auto metadata_step, metadata.step());
  if (metadata_step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "event consumer fleet metadata is unavailable");
  }
  EventConsumerFleetStatus fleet;
  fleet.workspace = workspace;
  fleet.observed_at = metadata.column_text(0);
  fleet.event_high_watermark = metadata.column_int(1);
  const i64 consumer_count = metadata.column_int(2);
  fleet.consumer_state_revision = metadata.column_int(3);
  if (consumer_count < 0 || consumer_count > kMaximumFleetConsumers) {
    return Err(ErrorCode::OutOfRange, "event consumer fleet exceeds the complete snapshot limit");
  }
  if (fleet.consumer_state_revision < 0 ||
      (consumer_count > 0 && fleet.consumer_state_revision == 0)) {
    return Err(ErrorCode::Internal, "event consumer fleet state revision is invalid");
  }
  ATX_TRY_VOID(metadata.reset());
  EventConsumerFleetCacheValidation validation;
  validation.validated_at = fleet.observed_at;
  validation.current.workspace = fleet.workspace;
  validation.current.event_high_watermark = fleet.event_high_watermark;
  validation.current.consumer_state_revision = fleet.consumer_state_revision;
  bool authoritative_transition_computed = false;
  if (cached != nullptr && cached->event_high_watermark == fleet.event_high_watermark &&
      cached->consumer_state_revision == fleet.consumer_state_revision) {
    ATX_TRY(auto transition,
            database.prepare(
                "SELECT COALESCE(MIN(CASE WHEN active_delivery_token<>'' AND "
                "active_delivery_expires_at>?2 THEN active_delivery_expires_at WHEN "
                "active_delivery_token<>'' AND NOT(max_delivery_attempts>0 AND "
                "active_delivery_attempt>=max_delivery_attempts) AND active_delivery_retry_at>?2 "
                "THEN active_delivery_retry_at END),'') FROM event_consumers WHERE workspace=?1"));
    ATX_TRY_VOID(transition.bind(1, workspace));
    ATX_TRY_VOID(transition.bind(2, fleet.observed_at));
    ATX_TRY(const auto transition_step, transition.step());
    if (transition_step != Statement::Step::Row) {
      return Err(ErrorCode::Internal, "event consumer fleet transition is unavailable");
    }
    validation.current.next_dynamic_transition_at = transition.column_text(0);
    authoritative_transition_computed = true;
    if (!validation.current.next_dynamic_transition_at.empty() &&
        (!canonical_utc_timestamp(validation.current.next_dynamic_transition_at) ||
         validation.current.next_dynamic_transition_at <= fleet.observed_at)) {
      return Err(ErrorCode::Internal, "event consumer fleet transition is invalid");
    }
    ATX_TRY_VOID(transition.reset());
    if (cached->next_dynamic_transition_at == validation.current.next_dynamic_transition_at &&
        (cached->next_dynamic_transition_at.empty() ||
         fleet.observed_at < cached->next_dynamic_transition_at)) {
      validation.cache_valid = true;
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(validation));
    }
  }
  ATX_TRY(
      auto statuses,
      database.prepare(
          "WITH consumers AS MATERIALIZED (SELECT workspace," + std::string{kEventConsumerColumns} +
          ",active_delivery_token,active_delivery_owner,active_delivery_through_sequence,"
          "active_delivery_attempt,active_delivery_expires_at,active_delivery_retry_at FROM "
          "event_consumers WHERE workspace=?1),visible_aggregates AS MATERIALIZED (SELECT "
          "c.name,COUNT(*) AS pending_count,MIN(e.sequence) AS first_sequence,MAX(e.sequence) AS "
          "last_sequence,SUM(CASE WHEN e.sequence<=c.active_delivery_through_sequence THEN 1 ELSE "
          "0 END) AS head_count FROM consumers c JOIN agent_events e INDEXED BY "
          "agent_events_poll_idx ON e.workspace=c.workspace AND e.sequence>c.cursor_sequence WHERE "
          "c.subject_filter='' AND (e.sequence<=c.self_control_event_cutoff_sequence OR "
          "substr(e.event_type,1,9)<>'consumer.' OR e.subject<>'consumers/'||c.name) GROUP BY "
          "c.name "
          "UNION ALL SELECT c.name,COUNT(*),MIN(e.sequence),MAX(e.sequence),SUM(CASE WHEN "
          "e.sequence<=c.active_delivery_through_sequence THEN 1 ELSE 0 END) FROM consumers c JOIN "
          "agent_events e INDEXED BY agent_events_subject_idx ON e.workspace=c.workspace AND "
          "e.subject=c.subject_filter AND e.sequence>c.cursor_sequence WHERE "
          "c.subject_filter<>'' AND e.subject<>'' AND "
          "(e.sequence<=c.self_control_event_cutoff_sequence OR "
          "substr(e.event_type,1,9)<>'consumer.' OR e.subject<>'consumers/'||c.name) GROUP BY "
          "c.name),dlq_aggregates AS MATERIALIZED (SELECT c.name,COUNT(l.id) AS retained_count,"
          "SUM(CASE WHEN l.status='open' AND q.id IS NULL THEN 1 ELSE 0 END) AS "
          "open_count,SUM(CASE "
          "WHEN l.status='open' AND q.id IS NULL THEN l.event_count ELSE 0 END) AS "
          "open_event_count,MIN(CASE WHEN l.status='open' AND q.id IS NULL THEN l.created_at END) "
          "AS "
          "oldest_open_at,SUM(CASE WHEN l.status='redriven' THEN 1 ELSE 0 END) AS redriven_count,"
          "SUM(CASE WHEN q.id IS NOT NULL THEN 1 ELSE 0 END) AS quarantined_count FROM consumers c "
          "LEFT JOIN event_consumer_dead_letters l INDEXED BY "
          "event_consumer_dead_letters_consumer_idx ON l.workspace=c.workspace AND "
          "l.consumer_name=c.name LEFT JOIN event_consumer_dead_letter_quarantines q ON "
          "q.workspace=l.workspace AND q.consumer_name=l.consumer_name AND q.dead_letter_id=l.id "
          "GROUP BY c.name) SELECT " +
          std::string{kQualifiedEventConsumerColumns} +
          ",?3,COALESCE(v.pending_count,0),COALESCE(v.first_sequence,0),"
          "COALESCE(v.last_sequence,0),COALESCE(first_event.created_at,''),CASE WHEN "
          "c.active_delivery_token='' THEN 'idle' WHEN c.active_delivery_expires_at>?2 THEN "
          "'in_flight' WHEN c.max_delivery_attempts>0 AND "
          "c.active_delivery_attempt>=c.max_delivery_attempts THEN 'dead_letter_ready' WHEN "
          "c.active_delivery_retry_at>?2 THEN 'retry_backoff' ELSE 'redelivery_ready' END,"
          "c.active_delivery_owner,c.active_delivery_attempt,c.active_delivery_through_sequence,"
          "c.active_delivery_expires_at,c.active_delivery_retry_at,COALESCE(v.head_count,0),"
          "COALESCE(d.retained_count,0),COALESCE(d.open_count,0),"
          "COALESCE(d.open_event_count,0),COALESCE(d.oldest_open_at,''),"
          "COALESCE(d.redriven_count,0),COALESCE(d.quarantined_count,0),?2,?4 FROM consumers c "
          "LEFT "
          "JOIN visible_aggregates v ON v.name=c.name LEFT JOIN agent_events first_event ON "
          "first_event.workspace=c.workspace AND first_event.sequence=v.first_sequence LEFT JOIN "
          "dlq_aggregates d ON d.name=c.name ORDER BY c.name"));
  ATX_TRY_VOID(statuses.bind(1, workspace));
  ATX_TRY_VOID(statuses.bind(2, fleet.observed_at));
  ATX_TRY_VOID(statuses.bind(3, fleet.event_high_watermark));
  ATX_TRY_VOID(statuses.bind(4, fleet.consumer_state_revision));
  fleet.consumers.reserve(static_cast<usize>(consumer_count));
  std::string previous_name;
  while (true) {
    ATX_TRY(const auto step, statuses.step());
    if (step == Statement::Step::Done) {
      break;
    }
    ATX_TRY(auto status, read_event_consumer_status(statuses));
    if (status.event_high_watermark != fleet.event_high_watermark ||
        status.consumer_state_revision != fleet.consumer_state_revision ||
        status.observed_at != fleet.observed_at) {
      return Err(ErrorCode::Internal, "event consumer fleet snapshot metadata drifted");
    }
    if (!previous_name.empty() && status.consumer.name <= previous_name) {
      return Err(ErrorCode::Internal, "event consumer fleet order is invalid");
    }
    previous_name = status.consumer.name;
    if (!status.next_dynamic_transition_at.empty() &&
        (fleet.next_dynamic_transition_at.empty() ||
         status.next_dynamic_transition_at < fleet.next_dynamic_transition_at)) {
      fleet.next_dynamic_transition_at = status.next_dynamic_transition_at;
    }
    fleet.consumers.push_back(std::move(status));
  }
  if (static_cast<i64>(fleet.consumers.size()) != consumer_count) {
    return Err(ErrorCode::Internal, "event consumer fleet snapshot count drifted");
  }
  if (authoritative_transition_computed &&
      validation.current.next_dynamic_transition_at != fleet.next_dynamic_transition_at) {
    return Err(ErrorCode::Internal, "event consumer fleet transition aggregation drifted");
  }
  validation.current.next_dynamic_transition_at = fleet.next_dynamic_transition_at;
  validation.snapshot.emplace(std::move(fleet));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(validation));
}

} // namespace

Result<EventConsumerFleetStatus> AgentDatabase::list_event_consumer_statuses() {
  ATX_TRY(auto validation, read_event_consumer_fleet(database_, workspace_, nullptr));
  if (validation.cache_valid || !validation.snapshot.has_value()) {
    return Err(ErrorCode::Internal, "unconditional event consumer fleet snapshot is unavailable");
  }
  return Ok(std::move(*validation.snapshot));
}

Result<EventConsumerFleetCacheValidation>
AgentDatabase::list_event_consumer_statuses_if_current(const EventConsumerFleetValidator &cached) {
  if (cached.workspace != workspace_ || cached.event_high_watermark < 0 ||
      cached.consumer_state_revision < 0 ||
      (!cached.next_dynamic_transition_at.empty() &&
       !canonical_utc_timestamp(cached.next_dynamic_transition_at))) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer fleet validator");
  }
  return read_event_consumer_fleet(database_, workspace_, &cached);
}

Result<EventConsumerBatch> AgentDatabase::poll_event_consumer(std::string_view name, usize limit) {
  if (!valid_field(name, kMaximumIdBytes, false) || limit == 0 || limit > 1'000) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer poll request");
  }
  ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
  ATX_TRY(auto events, events_after_filtered(database_, workspace_, consumer.cursor_sequence, limit,
                                             consumer.subject_filter, consumer.name,
                                             consumer.self_control_event_cutoff_sequence));
  EventConsumerBatch batch;
  batch.consumer = std::move(consumer);
  batch.events = std::move(events);
  return Ok(std::move(batch));
}

Result<EventConsumerDelivery> AgentDatabase::receive_event_consumer(std::string_view name,
                                                                    std::string_view owner,
                                                                    std::string_view request_token,
                                                                    i64 lease_seconds,
                                                                    usize limit) {
  if (!valid_field(name, kMaximumIdBytes, false) || !valid_field(owner, kMaximumIdBytes, false) ||
      !valid_field(request_token, kMaximumIdBytes, false) || lease_seconds < 1 ||
      lease_seconds > kMaximumLeaseSeconds || limit == 0 || limit > 1'000) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer receive request");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  {
    ATX_TRY(auto prior, database_.prepare("SELECT " + std::string{kEventDeliveryColumns} +
                                          " FROM event_consumer_deliveries WHERE workspace=?1 AND "
                                          "consumer_name=?2 AND request_token=?3"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, name));
    ATX_TRY_VOID(prior.bind(3, request_token));
    ATX_TRY(const auto prior_step, prior.step());
    if (prior_step == Statement::Step::Row) {
      auto state = read_event_delivery(prior);
      if (state.owner != owner || state.requested_limit != static_cast<i64>(limit) ||
          state.lease_seconds != lease_seconds) {
        return Err(ErrorCode::InvalidArgument,
                   "receive request token was reused with different intent");
      }
      ATX_TRY(auto active,
              database_.prepare("SELECT 1 FROM event_consumers WHERE workspace=?1 AND name=?2 "
                                "AND active_delivery_token=?3 AND active_delivery_expires_at>"
                                "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
      ATX_TRY_VOID(active.bind(1, workspace_));
      ATX_TRY_VOID(active.bind(2, name));
      ATX_TRY_VOID(active.bind(3, state.delivery_token));
      ATX_TRY(const auto active_step, active.step());
      if (state.state != "active" || active_step != Statement::Step::Row) {
        return Err(ErrorCode::Unavailable, "receive request no longer owns an active delivery");
      }
      ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
      ATX_TRY(auto delivery,
              make_event_delivery(database_, workspace_, std::move(consumer), std::move(state)));
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(delivery));
    }
  }

  ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
  ATX_TRY(auto active,
          database_.prepare("SELECT active_delivery_token,active_delivery_previous_sequence,"
                            "active_delivery_through_sequence,active_delivery_attempt,"
                            "active_delivery_expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
                            "active_delivery_retry_at<=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                            "FROM event_consumers WHERE workspace=?1 AND name=?2"));
  ATX_TRY_VOID(active.bind(1, workspace_));
  ATX_TRY_VOID(active.bind(2, name));
  ATX_TRY(const auto active_step, active.step());
  if (active_step != Statement::Step::Row) {
    return Err(ErrorCode::NotFound, "event consumer was not found");
  }
  const std::string expired_token{active.column_text(0)};
  i64 previous_sequence = consumer.cursor_sequence;
  i64 through_sequence{};
  i64 attempt = 1;
  i64 preceding_dead_lettered_batches{};
  i64 preceding_dead_lettered_events{};
  std::vector<TaskEvent> selected_events;
  if (!expired_token.empty()) {
    if (active.column_int(4) != 0) {
      return Err(ErrorCode::Unavailable, "event consumer already has an active delivery");
    }
    previous_sequence = active.column_int(1);
    through_sequence = active.column_int(2);
    const i64 expired_attempt = active.column_int(3);
    attempt = expired_attempt + 1;
    if (previous_sequence != consumer.cursor_sequence || through_sequence <= previous_sequence ||
        attempt < 2) {
      return Err(ErrorCode::Internal, "expired event delivery state is invalid");
    }
    ATX_TRY(selected_events,
            events_between(database_, workspace_, previous_sequence, through_sequence,
                           consumer.subject_filter, consumer.name,
                           consumer.self_control_event_cutoff_sequence));
    const bool delivery_limit_reached =
        consumer.max_delivery_attempts > 0 && expired_attempt >= consumer.max_delivery_attempts;
    if (!delivery_limit_reached && active.column_int(5) == 0) {
      return Err(ErrorCode::Unavailable, "event consumer delivery is in retry backoff");
    }
    if (!delivery_limit_reached && selected_events.size() > limit) {
      return Err(ErrorCode::Unavailable,
                 "receive limit is smaller than the pending redelivery batch");
    }
    ATX_TRY(auto expire,
            database_.prepare("UPDATE event_consumer_deliveries SET state='expired',finished_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                              "consumer_name=?2 AND delivery_token=?3 AND state='active'"));
    ATX_TRY_VOID(expire.bind(1, workspace_));
    ATX_TRY_VOID(expire.bind(2, name));
    ATX_TRY_VOID(expire.bind(3, expired_token));
    ATX_TRY_VOID(step_done(expire));
    if (database_.changes() != 1) {
      return Err(ErrorCode::Internal, "expired event delivery audit state is invalid");
    }
    if (delivery_limit_reached) {
      constexpr std::string_view reason = "max_delivery_attempts_exceeded";
      ATX_TRY(auto dead_letter,
              database_.prepare("INSERT INTO event_consumer_dead_letters("
                                "workspace,consumer_name,delivery_token,previous_sequence,"
                                "through_sequence,delivery_attempts,event_count,reason) "
                                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"));
      ATX_TRY_VOID(dead_letter.bind(1, workspace_));
      ATX_TRY_VOID(dead_letter.bind(2, name));
      ATX_TRY_VOID(dead_letter.bind(3, expired_token));
      ATX_TRY_VOID(dead_letter.bind(4, previous_sequence));
      ATX_TRY_VOID(dead_letter.bind(5, through_sequence));
      ATX_TRY_VOID(dead_letter.bind(6, expired_attempt));
      ATX_TRY_VOID(dead_letter.bind(7, static_cast<i64>(selected_events.size())));
      ATX_TRY_VOID(dead_letter.bind(8, reason));
      ATX_TRY_VOID(step_done(dead_letter));
      const i64 dead_letter_id = database_.last_insert_rowid();
      ATX_TRY_VOID(append_event_consumer_dead_letter_lifecycle(database_, workspace_, name,
                                                               dead_letter_id, "dead_lettered"));
      ATX_TRY(
          auto advance,
          database_.prepare("UPDATE event_consumers SET cursor_sequence=?3,"
                            "revision=revision+1,active_delivery_token='',"
                            "active_delivery_owner='',active_delivery_previous_sequence=0,"
                            "active_delivery_through_sequence=0,active_delivery_attempt=0,"
                            "active_delivery_retry_delay_seconds=0,active_delivery_expires_at='',"
                            "active_delivery_retry_at='',updated_at="
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                            "name=?2 AND active_delivery_token=?4 AND cursor_sequence=?5 AND "
                            "active_delivery_expires_at<=strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
      ATX_TRY_VOID(advance.bind(1, workspace_));
      ATX_TRY_VOID(advance.bind(2, name));
      ATX_TRY_VOID(advance.bind(3, through_sequence));
      ATX_TRY_VOID(advance.bind(4, expired_token));
      ATX_TRY_VOID(advance.bind(5, previous_sequence));
      ATX_TRY_VOID(step_done(advance));
      if (database_.changes() != 1) {
        return Err(ErrorCode::Unavailable, "dead-letter cursor transition lost its lease");
      }
      ATX_TRY(const auto dead_letter_token, new_id(database_, "deadletter_"));
      ATX_TRY(
          auto checkpoint,
          database_.prepare("INSERT INTO event_consumer_checkpoints("
                            "workspace,consumer_name,checkpoint_token,delivery_token,outcome,"
                            "request_revision,previous_sequence,through_sequence,"
                            "result_revision) VALUES(?1,?2,?3,?4,'dead_lettered',?5,?6,?7,?8)"));
      ATX_TRY_VOID(checkpoint.bind(1, workspace_));
      ATX_TRY_VOID(checkpoint.bind(2, name));
      ATX_TRY_VOID(checkpoint.bind(3, dead_letter_token));
      ATX_TRY_VOID(checkpoint.bind(4, expired_token));
      ATX_TRY_VOID(checkpoint.bind(5, consumer.revision));
      ATX_TRY_VOID(checkpoint.bind(6, previous_sequence));
      ATX_TRY_VOID(checkpoint.bind(7, through_sequence));
      ATX_TRY_VOID(checkpoint.bind(8, consumer.revision + 1));
      ATX_TRY_VOID(step_done(checkpoint));
      preceding_dead_lettered_batches = 1;
      preceding_dead_lettered_events = static_cast<i64>(selected_events.size());
      ATX_TRY(consumer, find_event_consumer(database_, workspace_, name));
      previous_sequence = consumer.cursor_sequence;
      attempt = 1;
      ATX_TRY(selected_events, events_after_filtered(database_, workspace_, previous_sequence,
                                                     limit, consumer.subject_filter, consumer.name,
                                                     consumer.self_control_event_cutoff_sequence));
      if (selected_events.empty()) {
        EventConsumerDelivery empty;
        empty.consumer = std::move(consumer);
        empty.previous_sequence = previous_sequence;
        empty.through_sequence = previous_sequence;
        empty.dead_lettered_batches = preceding_dead_lettered_batches;
        empty.dead_lettered_events = preceding_dead_lettered_events;
        ATX_TRY_VOID(transaction.commit());
        return Ok(std::move(empty));
      }
      through_sequence = selected_events.back().sequence;
    }
  } else {
    ATX_TRY(selected_events, events_after_filtered(database_, workspace_, previous_sequence, limit,
                                                   consumer.subject_filter, consumer.name,
                                                   consumer.self_control_event_cutoff_sequence));
    if (selected_events.empty()) {
      EventConsumerDelivery empty;
      empty.consumer = std::move(consumer);
      empty.previous_sequence = previous_sequence;
      empty.through_sequence = previous_sequence;
      empty.dead_lettered_batches = preceding_dead_lettered_batches;
      empty.dead_lettered_events = preceding_dead_lettered_events;
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(empty));
    }
    through_sequence = selected_events.back().sequence;
  }

  ATX_TRY(const auto delivery_token, new_id(database_, "delivery_"));
  const i64 retry_window = delivery_retry_backoff(consumer.retry_backoff_seconds,
                                                  consumer.retry_backoff_max_seconds, attempt);
  ATX_TRY(const auto retry_delay,
          sample_delivery_retry_delay(database_, consumer.retry_jitter, retry_window));
  ATX_TRY(auto update,
          database_.prepare("UPDATE event_consumers SET active_delivery_token=?3,"
                            "active_delivery_owner=?4,active_delivery_previous_sequence=?5,"
                            "active_delivery_through_sequence=?6,active_delivery_attempt=?7,"
                            "active_delivery_retry_delay_seconds=?9,"
                            "active_delivery_expires_at=strftime('%Y-%m-%dT%H:%M:%fZ','now','+' "
                            "|| ?8 || ' seconds'),active_delivery_retry_at=strftime("
                            "'%Y-%m-%dT%H:%M:%fZ','now','+' || (?8+?9) || ' seconds'),"
                            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                            "WHERE workspace=?1 AND name=?2 AND cursor_sequence=?5"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, name));
  ATX_TRY_VOID(update.bind(3, delivery_token));
  ATX_TRY_VOID(update.bind(4, owner));
  ATX_TRY_VOID(update.bind(5, previous_sequence));
  ATX_TRY_VOID(update.bind(6, through_sequence));
  ATX_TRY_VOID(update.bind(7, attempt));
  ATX_TRY_VOID(update.bind(8, lease_seconds));
  ATX_TRY_VOID(update.bind(9, retry_delay));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "event consumer delivery lost a cursor race");
  }
  ATX_TRY(auto insert,
          database_.prepare("INSERT INTO event_consumer_deliveries("
                            "workspace,consumer_name,delivery_token,owner,request_token,"
                            "request_revision,previous_sequence,through_sequence,attempt,"
                            "requested_limit,lease_seconds,preceding_dead_lettered_batches,"
                            "preceding_dead_lettered_events,expires_at,retry_delay_seconds,"
                            "retry_not_before) SELECT "
                            "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,"
                            "active_delivery_expires_at,active_delivery_retry_delay_seconds,"
                            "active_delivery_retry_at FROM event_consumers "
                            "WHERE workspace=?1 AND name=?2 AND active_delivery_token=?3"));
  ATX_TRY_VOID(insert.bind(1, workspace_));
  ATX_TRY_VOID(insert.bind(2, name));
  ATX_TRY_VOID(insert.bind(3, delivery_token));
  ATX_TRY_VOID(insert.bind(4, owner));
  ATX_TRY_VOID(insert.bind(5, request_token));
  ATX_TRY_VOID(insert.bind(6, consumer.revision));
  ATX_TRY_VOID(insert.bind(7, previous_sequence));
  ATX_TRY_VOID(insert.bind(8, through_sequence));
  ATX_TRY_VOID(insert.bind(9, attempt));
  ATX_TRY_VOID(insert.bind(10, static_cast<i64>(limit)));
  ATX_TRY_VOID(insert.bind(11, lease_seconds));
  ATX_TRY_VOID(insert.bind(12, preceding_dead_lettered_batches));
  ATX_TRY_VOID(insert.bind(13, preceding_dead_lettered_events));
  ATX_TRY_VOID(step_done(insert));
  ATX_TRY(auto state, find_event_delivery(database_, workspace_, name, delivery_token));
  ATX_TRY(auto current, find_event_consumer(database_, workspace_, name));
  ATX_TRY(auto delivery,
          make_event_delivery(database_, workspace_, std::move(current), std::move(state)));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(delivery));
}

Result<EventConsumerDelivery>
AgentDatabase::renew_event_consumer_delivery(std::string_view name, std::string_view owner,
                                             std::string_view delivery_token, i64 lease_seconds) {
  if (!valid_field(name, kMaximumIdBytes, false) || !valid_field(owner, kMaximumIdBytes, false) ||
      !valid_field(delivery_token, kMaximumIdBytes, false) || lease_seconds < 1 ||
      lease_seconds > kMaximumLeaseSeconds) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer delivery renewal");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(auto policy,
          database_.prepare("SELECT active_delivery_retry_delay_seconds FROM event_consumers "
                            "WHERE workspace=?1 "
                            "AND name=?2 AND active_delivery_owner=?3 AND "
                            "active_delivery_token=?4 AND active_delivery_expires_at>"
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
  ATX_TRY_VOID(policy.bind(1, workspace_));
  ATX_TRY_VOID(policy.bind(2, name));
  ATX_TRY_VOID(policy.bind(3, owner));
  ATX_TRY_VOID(policy.bind(4, delivery_token));
  ATX_TRY(const auto policy_step, policy.step());
  if (policy_step != Statement::Step::Row) {
    return Err(ErrorCode::Unavailable, "event consumer delivery lease is stale or expired");
  }
  const i64 retry_delay = policy.column_int(0);
  ATX_TRY_VOID(policy.reset());
  ATX_TRY(auto update,
          database_.prepare("UPDATE event_consumers SET active_delivery_expires_at="
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now','+' || ?5 || ' seconds'),"
                            "active_delivery_retry_at=strftime('%Y-%m-%dT%H:%M:%fZ','now','+' "
                            "|| (?5+?6) || ' seconds'),"
                            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                            "AND name=?2 AND active_delivery_owner=?3 AND "
                            "active_delivery_token=?4 AND active_delivery_expires_at>"
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, name));
  ATX_TRY_VOID(update.bind(3, owner));
  ATX_TRY_VOID(update.bind(4, delivery_token));
  ATX_TRY_VOID(update.bind(5, lease_seconds));
  ATX_TRY_VOID(update.bind(6, retry_delay));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "event consumer delivery lease is stale or expired");
  }
  ATX_TRY(auto audit,
          database_.prepare("UPDATE event_consumer_deliveries SET expires_at=(SELECT "
                            "active_delivery_expires_at FROM event_consumers WHERE workspace=?1 "
                            "AND name=?2),retry_not_before=(SELECT active_delivery_retry_at FROM "
                            "event_consumers WHERE workspace=?1 AND name=?2) WHERE workspace=?1 "
                            "AND consumer_name=?2 AND delivery_token=?3 AND state='active'"));
  ATX_TRY_VOID(audit.bind(1, workspace_));
  ATX_TRY_VOID(audit.bind(2, name));
  ATX_TRY_VOID(audit.bind(3, delivery_token));
  ATX_TRY_VOID(step_done(audit));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Internal, "event consumer delivery audit state is invalid");
  }
  ATX_TRY(auto state, find_event_delivery(database_, workspace_, name, delivery_token));
  ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
  ATX_TRY(auto delivery,
          make_event_delivery(database_, workspace_, std::move(consumer), std::move(state)));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(delivery));
}

Result<EventConsumerRejection> AgentDatabase::reject_event_consumer_delivery(
    std::string_view name, std::string_view owner, std::string_view delivery_token,
    std::string_view rejection_token, std::string_view reason,
    EventConsumerRejectionDisposition disposition) {
  if (!valid_field(name, kMaximumIdBytes, false) || !valid_field(owner, kMaximumIdBytes, false) ||
      !valid_field(delivery_token, kMaximumIdBytes, false) ||
      !valid_field(rejection_token, kMaximumIdBytes, false) ||
      !valid_field(reason, kMaximumTitleBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer delivery rejection");
  }
  const std::string_view disposition_name =
      disposition == EventConsumerRejectionDisposition::DeadLetter ? "dead_letter" : "retry";
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  {
    ATX_TRY(auto prior, database_.prepare("SELECT " + std::string{kEventDeliveryColumns} +
                                          " FROM event_consumer_deliveries WHERE workspace=?1 "
                                          "AND consumer_name=?2 AND rejection_token=?3"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, name));
    ATX_TRY_VOID(prior.bind(3, rejection_token));
    ATX_TRY(const auto prior_step, prior.step());
    if (prior_step == Statement::Step::Row) {
      auto state = read_event_delivery(prior);
      if (state.delivery_token != delivery_token || state.owner != owner ||
          state.rejection_disposition != disposition_name || state.rejection_reason != reason) {
        return Err(ErrorCode::InvalidArgument,
                   "rejection token was reused with a different request");
      }
      ATX_TRY_VOID(prior.reset());
      ATX_TRY(auto rejection, make_event_rejection(database_, workspace_, name, std::move(state)));
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(rejection));
    }
  }

  ATX_TRY(auto state, find_event_delivery(database_, workspace_, name, delivery_token));
  if (state.owner != owner || state.state != "active") {
    return Err(ErrorCode::Unavailable, "event consumer delivery rejection is stale");
  }
  if (!state.rejection_token.empty() || !state.rejection_disposition.empty() ||
      !state.rejection_reason.empty() || !state.rejected_at.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "event consumer delivery was already rejected with a different token");
  }
  ATX_TRY(auto active,
          database_.prepare("SELECT active_delivery_previous_sequence,"
                            "active_delivery_through_sequence,active_delivery_attempt,"
                            "max_delivery_attempts,revision,"
                            "active_delivery_retry_delay_seconds FROM event_consumers WHERE "
                            "workspace=?1 "
                            "AND name=?2 AND active_delivery_owner=?3 AND "
                            "active_delivery_token=?4 AND active_delivery_expires_at>"
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
  ATX_TRY_VOID(active.bind(1, workspace_));
  ATX_TRY_VOID(active.bind(2, name));
  ATX_TRY_VOID(active.bind(3, owner));
  ATX_TRY_VOID(active.bind(4, delivery_token));
  ATX_TRY(const auto active_step, active.step());
  if (active_step != Statement::Step::Row) {
    return Err(ErrorCode::Unavailable, "event consumer delivery rejection is stale or expired");
  }
  const i64 previous_sequence = active.column_int(0);
  const i64 through_sequence = active.column_int(1);
  const i64 attempt = active.column_int(2);
  const i64 max_delivery_attempts = active.column_int(3);
  const i64 request_revision = active.column_int(4);
  const i64 retry_delay = active.column_int(5);
  if (state.previous_sequence != previous_sequence || state.through_sequence != through_sequence ||
      state.attempt != attempt || state.request_revision != request_revision ||
      state.retry_delay_seconds != retry_delay) {
    return Err(ErrorCode::Internal, "event consumer rejection head audit mismatch");
  }
  ATX_TRY_VOID(active.reset());
  const bool terminal = disposition == EventConsumerRejectionDisposition::DeadLetter ||
                        (max_delivery_attempts > 0 && attempt >= max_delivery_attempts);
  if (!terminal) {
    ATX_TRY(auto reject_head,
            database_.prepare("UPDATE event_consumers SET active_delivery_expires_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
                              "active_delivery_retry_at=strftime('%Y-%m-%dT%H:%M:%fZ','now','+' "
                              "|| ?5 || ' seconds'),updated_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                              "name=?2 AND active_delivery_owner=?3 AND active_delivery_token=?4 "
                              "AND active_delivery_expires_at>"
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
    ATX_TRY_VOID(reject_head.bind(1, workspace_));
    ATX_TRY_VOID(reject_head.bind(2, name));
    ATX_TRY_VOID(reject_head.bind(3, owner));
    ATX_TRY_VOID(reject_head.bind(4, delivery_token));
    ATX_TRY_VOID(reject_head.bind(5, retry_delay));
    ATX_TRY_VOID(step_done(reject_head));
    if (database_.changes() != 1) {
      return Err(ErrorCode::Unavailable, "event consumer delivery rejection lost its lease");
    }
    ATX_TRY(auto reject_audit,
            database_.prepare("UPDATE event_consumer_deliveries SET expires_at=(SELECT "
                              "active_delivery_expires_at FROM event_consumers WHERE workspace=?1 "
                              "AND name=?2),retry_not_before=(SELECT active_delivery_retry_at FROM "
                              "event_consumers WHERE workspace=?1 AND name=?2),rejection_token=?4,"
                              "rejection_disposition=?5,rejection_reason=?6,rejected_at=(SELECT "
                              "active_delivery_expires_at "
                              "FROM event_consumers WHERE workspace=?1 AND name=?2) WHERE "
                              "workspace=?1 AND consumer_name=?2 AND delivery_token=?3 AND "
                              "state='active' AND rejection_token=''"));
    ATX_TRY_VOID(reject_audit.bind(1, workspace_));
    ATX_TRY_VOID(reject_audit.bind(2, name));
    ATX_TRY_VOID(reject_audit.bind(3, delivery_token));
    ATX_TRY_VOID(reject_audit.bind(4, rejection_token));
    ATX_TRY_VOID(reject_audit.bind(5, disposition_name));
    ATX_TRY_VOID(reject_audit.bind(6, reason));
    ATX_TRY_VOID(step_done(reject_audit));
    if (database_.changes() != 1) {
      return Err(ErrorCode::Internal, "event consumer rejection audit update failed");
    }
  } else {
    ATX_TRY(auto current_consumer, find_event_consumer(database_, workspace_, name));
    ATX_TRY(auto events, events_between(database_, workspace_, previous_sequence, through_sequence,
                                        current_consumer.subject_filter, current_consumer.name,
                                        current_consumer.self_control_event_cutoff_sequence));
    ATX_TRY(auto reject_audit,
            database_.prepare("UPDATE event_consumer_deliveries SET state='expired',expires_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now'),retry_not_before=strftime("
                              "'%Y-%m-%dT%H:%M:%fZ','now','+' || ?4 || ' seconds'),"
                              "rejection_token=?5,rejection_disposition=?6,rejection_reason=?7,"
                              "rejected_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now'),finished_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                              "consumer_name=?2 AND delivery_token=?3 AND state='active' AND "
                              "rejection_token=''"));
    ATX_TRY_VOID(reject_audit.bind(1, workspace_));
    ATX_TRY_VOID(reject_audit.bind(2, name));
    ATX_TRY_VOID(reject_audit.bind(3, delivery_token));
    ATX_TRY_VOID(reject_audit.bind(4, retry_delay));
    ATX_TRY_VOID(reject_audit.bind(5, rejection_token));
    ATX_TRY_VOID(reject_audit.bind(6, disposition_name));
    ATX_TRY_VOID(reject_audit.bind(7, reason));
    ATX_TRY_VOID(step_done(reject_audit));
    if (database_.changes() != 1) {
      return Err(ErrorCode::Unavailable, "event consumer terminal rejection lost its lease");
    }
    ATX_TRY(auto dead_letter,
            database_.prepare("INSERT INTO event_consumer_dead_letters("
                              "workspace,consumer_name,delivery_token,previous_sequence,"
                              "through_sequence,delivery_attempts,event_count,reason) "
                              "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"));
    ATX_TRY_VOID(dead_letter.bind(1, workspace_));
    ATX_TRY_VOID(dead_letter.bind(2, name));
    ATX_TRY_VOID(dead_letter.bind(3, delivery_token));
    ATX_TRY_VOID(dead_letter.bind(4, previous_sequence));
    ATX_TRY_VOID(dead_letter.bind(5, through_sequence));
    ATX_TRY_VOID(dead_letter.bind(6, attempt));
    ATX_TRY_VOID(dead_letter.bind(7, static_cast<i64>(events.size())));
    ATX_TRY_VOID(dead_letter.bind(8, disposition == EventConsumerRejectionDisposition::DeadLetter
                                         ? std::string_view{"explicit_rejection"}
                                         : std::string_view{"max_delivery_attempts_rejected"}));
    ATX_TRY_VOID(step_done(dead_letter));
    const i64 dead_letter_id = database_.last_insert_rowid();
    ATX_TRY_VOID(append_event_consumer_dead_letter_lifecycle(database_, workspace_, name,
                                                             dead_letter_id, "dead_lettered"));
    ATX_TRY(auto advance,
            database_.prepare("UPDATE event_consumers SET cursor_sequence=?5,"
                              "revision=revision+1,active_delivery_token='',"
                              "active_delivery_owner='',active_delivery_previous_sequence=0,"
                              "active_delivery_through_sequence=0,active_delivery_attempt=0,"
                              "active_delivery_retry_delay_seconds=0,active_delivery_expires_at='',"
                              "active_delivery_retry_at='',"
                              "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                              "AND name=?2 AND active_delivery_owner=?3 AND "
                              "active_delivery_token=?4 AND cursor_sequence=?6"));
    ATX_TRY_VOID(advance.bind(1, workspace_));
    ATX_TRY_VOID(advance.bind(2, name));
    ATX_TRY_VOID(advance.bind(3, owner));
    ATX_TRY_VOID(advance.bind(4, delivery_token));
    ATX_TRY_VOID(advance.bind(5, through_sequence));
    ATX_TRY_VOID(advance.bind(6, previous_sequence));
    ATX_TRY_VOID(step_done(advance));
    if (database_.changes() != 1) {
      return Err(ErrorCode::Unavailable, "event consumer rejection cursor transition lost lease");
    }
    ATX_TRY(const auto checkpoint_token, new_id(database_, "reject_deadletter_"));
    ATX_TRY(
        auto checkpoint,
        database_.prepare("INSERT INTO event_consumer_checkpoints("
                          "workspace,consumer_name,checkpoint_token,delivery_token,outcome,"
                          "request_revision,previous_sequence,through_sequence,result_revision) "
                          "VALUES(?1,?2,?3,?4,'dead_lettered',?5,?6,?7,?8)"));
    ATX_TRY_VOID(checkpoint.bind(1, workspace_));
    ATX_TRY_VOID(checkpoint.bind(2, name));
    ATX_TRY_VOID(checkpoint.bind(3, checkpoint_token));
    ATX_TRY_VOID(checkpoint.bind(4, delivery_token));
    ATX_TRY_VOID(checkpoint.bind(5, request_revision));
    ATX_TRY_VOID(checkpoint.bind(6, previous_sequence));
    ATX_TRY_VOID(checkpoint.bind(7, through_sequence));
    ATX_TRY_VOID(checkpoint.bind(8, request_revision + 1));
    ATX_TRY_VOID(step_done(checkpoint));
  }
  ATX_TRY(state, find_event_delivery(database_, workspace_, name, delivery_token));
  ATX_TRY(auto rejection, make_event_rejection(database_, workspace_, name, std::move(state)));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(rejection));
}

Result<EventConsumerRecord>
AgentDatabase::settle_event_consumer_delivery(std::string_view name, std::string_view owner,
                                              std::string_view delivery_token,
                                              std::string_view checkpoint_token) {
  if (!valid_field(name, kMaximumIdBytes, false) || !valid_field(owner, kMaximumIdBytes, false) ||
      !valid_field(delivery_token, kMaximumIdBytes, false) ||
      !valid_field(checkpoint_token, kMaximumIdBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer delivery settlement");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  {
    ATX_TRY(auto prior, database_.prepare(
                            "SELECT c.delivery_token,d.owner FROM event_consumer_checkpoints c "
                            "JOIN event_consumer_deliveries d ON d.workspace=c.workspace AND "
                            "d.consumer_name=c.consumer_name AND d.delivery_token=c.delivery_token "
                            "WHERE c.workspace=?1 AND c.consumer_name=?2 AND "
                            "c.checkpoint_token=?3 AND c.outcome='processed'"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, name));
    ATX_TRY_VOID(prior.bind(3, checkpoint_token));
    ATX_TRY(const auto prior_step, prior.step());
    if (prior_step == Statement::Step::Row) {
      if (prior.column_text(0) != delivery_token || prior.column_text(1) != owner) {
        return Err(ErrorCode::InvalidArgument,
                   "checkpoint token was reused for a different delivery");
      }
      ATX_TRY(auto current, find_event_consumer(database_, workspace_, name));
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(current));
    }
  }
  ATX_TRY(auto active,
          database_.prepare("SELECT active_delivery_previous_sequence,"
                            "active_delivery_through_sequence FROM event_consumers WHERE "
                            "workspace=?1 AND name=?2 AND active_delivery_owner=?3 AND "
                            "active_delivery_token=?4 AND active_delivery_expires_at>"
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
  ATX_TRY_VOID(active.bind(1, workspace_));
  ATX_TRY_VOID(active.bind(2, name));
  ATX_TRY_VOID(active.bind(3, owner));
  ATX_TRY_VOID(active.bind(4, delivery_token));
  ATX_TRY(const auto active_step, active.step());
  if (active_step != Statement::Step::Row) {
    return Err(ErrorCode::Unavailable, "event consumer delivery lease is stale or expired");
  }
  const i64 previous_sequence = active.column_int(0);
  const i64 through_sequence = active.column_int(1);
  ATX_TRY(auto before, find_event_consumer(database_, workspace_, name));
  if (before.cursor_sequence != previous_sequence || through_sequence <= previous_sequence) {
    return Err(ErrorCode::Internal, "event consumer delivery cursor invariant failed");
  }
  ATX_TRY(auto update, database_.prepare(
                           "UPDATE event_consumers SET cursor_sequence=?5,revision=revision+1,"
                           "active_delivery_token='',active_delivery_owner='',"
                           "active_delivery_previous_sequence=0,active_delivery_through_sequence=0,"
                           "active_delivery_attempt=0,active_delivery_retry_delay_seconds=0,"
                           "active_delivery_expires_at='',"
                           "active_delivery_retry_at='',"
                           "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                           "AND name=?2 AND active_delivery_owner=?3 AND active_delivery_token=?4 "
                           "AND cursor_sequence=?6 AND active_delivery_expires_at>"
                           "strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, name));
  ATX_TRY_VOID(update.bind(3, owner));
  ATX_TRY_VOID(update.bind(4, delivery_token));
  ATX_TRY_VOID(update.bind(5, through_sequence));
  ATX_TRY_VOID(update.bind(6, previous_sequence));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "event consumer delivery settlement lost its lease");
  }
  ATX_TRY(auto checkpoint,
          database_.prepare("INSERT INTO event_consumer_checkpoints("
                            "workspace,consumer_name,checkpoint_token,delivery_token,"
                            "request_revision,previous_sequence,through_sequence,result_revision) "
                            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"));
  ATX_TRY_VOID(checkpoint.bind(1, workspace_));
  ATX_TRY_VOID(checkpoint.bind(2, name));
  ATX_TRY_VOID(checkpoint.bind(3, checkpoint_token));
  ATX_TRY_VOID(checkpoint.bind(4, delivery_token));
  ATX_TRY_VOID(checkpoint.bind(5, before.revision));
  ATX_TRY_VOID(checkpoint.bind(6, previous_sequence));
  ATX_TRY_VOID(checkpoint.bind(7, through_sequence));
  ATX_TRY_VOID(checkpoint.bind(8, before.revision + 1));
  ATX_TRY_VOID(step_done(checkpoint));
  ATX_TRY(auto settle,
          database_.prepare("UPDATE event_consumer_deliveries SET state='settled',finished_at="
                            "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                            "consumer_name=?2 AND delivery_token=?3 AND state='active'"));
  ATX_TRY_VOID(settle.bind(1, workspace_));
  ATX_TRY_VOID(settle.bind(2, name));
  ATX_TRY_VOID(settle.bind(3, delivery_token));
  ATX_TRY_VOID(step_done(settle));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Internal, "event consumer delivery audit settlement failed");
  }
  ATX_TRY(auto after, find_event_consumer(database_, workspace_, name));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(after));
}

Result<std::vector<EventConsumerDeadLetter>>
AgentDatabase::list_event_consumer_dead_letters(std::string_view name, usize limit) {
  if (!valid_field(name, kMaximumIdBytes, false) || limit == 0 || limit > 1'000) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer dead-letter request");
  }
  ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
  ATX_TRY(auto query,
          database_.prepare("SELECT id FROM event_consumer_dead_letters WHERE workspace=?1 AND "
                            "consumer_name=?2 ORDER BY id DESC LIMIT ?3"));
  ATX_TRY_VOID(query.bind(1, workspace_));
  ATX_TRY_VOID(query.bind(2, name));
  ATX_TRY_VOID(query.bind(3, static_cast<i64>(limit)));
  std::vector<i64> ids;
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    ids.push_back(query.column_int(0));
  }
  ATX_TRY_VOID(query.reset());
  std::vector<EventConsumerDeadLetter> result;
  result.reserve(ids.size());
  for (const auto id : ids) {
    ATX_TRY(auto dead_letter,
            find_event_consumer_dead_letter(database_, workspace_, consumer, name, id));
    result.push_back(std::move(dead_letter));
  }
  return Ok(std::move(result));
}

Result<EventConsumerDeadLetter>
AgentDatabase::redrive_event_consumer_dead_letter(std::string_view name, i64 dead_letter_id,
                                                  std::string_view redrive_token) {
  if (!valid_field(name, kMaximumIdBytes, false) || dead_letter_id < 1 ||
      !valid_field(redrive_token, kMaximumIdBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer redrive request");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
  ATX_TRY(auto dead_letter,
          find_event_consumer_dead_letter(database_, workspace_, consumer, name, dead_letter_id));
  if (dead_letter.status == "quarantined") {
    return Err(ErrorCode::InvalidArgument, "dead-letter batch is quarantined");
  }
  if (dead_letter.status == "redriven") {
    if (dead_letter.redrive_token != redrive_token) {
      return Err(ErrorCode::InvalidArgument,
                 "dead-letter batch was already redriven with a different token");
    }
    ATX_TRY_VOID(transaction.commit());
    return Ok(std::move(dead_letter));
  }
  if (dead_letter.status != "open" || !dead_letter.redrive_token.empty() ||
      !dead_letter.redriven_at.empty()) {
    return Err(ErrorCode::Internal, "event consumer dead-letter state is invalid");
  }
  for (const auto &event : dead_letter.events) {
    if (event.root_sequence < 1 || event.redrive_count < 0 ||
        event.redrive_count == std::numeric_limits<i64>::max()) {
      return Err(ErrorCode::Internal, "event consumer dead-letter lineage is invalid");
    }
    if (consumer.max_redrive_count > 0 && event.redrive_count >= consumer.max_redrive_count) {
      return Err(ErrorCode::InvalidArgument,
                 "dead-letter batch reached the consumer redrive generation limit");
    }
  }
  if (consumer.redrive_rate_per_second > 0) {
    const i64 event_count = static_cast<i64>(dead_letter.events.size());
    if (event_count < 1 || event_count > kMaximumRedriveBurstEvents) {
      return Err(ErrorCode::Internal, "event consumer dead-letter event count is invalid");
    }
    if (event_count > consumer.redrive_burst_events) {
      return Err(ErrorCode::InvalidArgument,
                 "dead-letter batch exceeds the consumer redrive burst capacity");
    }
    const i64 capacity = consumer.redrive_burst_events * kRedriveTokenUnitsPerEvent;
    if (consumer.redrive_token_millis < 0 || consumer.redrive_token_millis > capacity ||
        !canonical_utc_timestamp(consumer.redrive_refilled_at)) {
      return Err(ErrorCode::Internal, "event consumer redrive budget state is invalid");
    }
    ATX_TRY(const auto wall_time,
            scalar_text(database_, "SELECT strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
    const std::string effective_refilled_at = std::max(wall_time, consumer.redrive_refilled_at);
    ATX_TRY(const auto elapsed,
            elapsed_milliseconds(consumer.redrive_refilled_at, effective_refilled_at));
    if (elapsed < 0) {
      return Err(ErrorCode::Internal, "event consumer redrive refill time moved backward");
    }
    const i64 headroom = capacity - consumer.redrive_token_millis;
    const i64 earned = elapsed > headroom / consumer.redrive_rate_per_second
                           ? headroom
                           : elapsed * consumer.redrive_rate_per_second;
    const i64 refilled = consumer.redrive_token_millis + earned;
    const i64 cost = event_count * kRedriveTokenUnitsPerEvent;
    if (refilled < cost) {
      const i64 deficit = cost - refilled;
      const i64 retry_after_milliseconds =
          (deficit + consumer.redrive_rate_per_second - 1) / consumer.redrive_rate_per_second;
      return Err(ErrorCode::Unavailable,
                 "event consumer redrive budget is exhausted; retry after " +
                     std::to_string(retry_after_milliseconds) + " ms");
    }
    const i64 result_tokens = refilled - cost;
    ATX_TRY(auto budget,
            database_.prepare("UPDATE event_consumers SET redrive_token_millis=?3,"
                              "redrive_refilled_at=?4,updated_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                              "name=?2 AND redrive_rate_per_second=?5 AND "
                              "redrive_burst_events=?6 AND redrive_token_millis=?7 AND "
                              "redrive_refilled_at=?8"));
    ATX_TRY_VOID(budget.bind(1, workspace_));
    ATX_TRY_VOID(budget.bind(2, name));
    ATX_TRY_VOID(budget.bind(3, result_tokens));
    ATX_TRY_VOID(budget.bind(4, effective_refilled_at));
    ATX_TRY_VOID(budget.bind(5, consumer.redrive_rate_per_second));
    ATX_TRY_VOID(budget.bind(6, consumer.redrive_burst_events));
    ATX_TRY_VOID(budget.bind(7, consumer.redrive_token_millis));
    ATX_TRY_VOID(budget.bind(8, consumer.redrive_refilled_at));
    ATX_TRY_VOID(step_done(budget));
    if (database_.changes() != 1) {
      return Err(ErrorCode::Unavailable, "event consumer redrive budget lost a state race");
    }
    ATX_TRY(auto charge,
            database_.prepare("INSERT INTO event_consumer_redrive_budget_charges("
                              "workspace,consumer_name,dead_letter_id,redrive_token,event_count,"
                              "refilled_token_millis,result_token_millis,refilled_at) "
                              "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"));
    ATX_TRY_VOID(charge.bind(1, workspace_));
    ATX_TRY_VOID(charge.bind(2, name));
    ATX_TRY_VOID(charge.bind(3, dead_letter_id));
    ATX_TRY_VOID(charge.bind(4, redrive_token));
    ATX_TRY_VOID(charge.bind(5, event_count));
    ATX_TRY_VOID(charge.bind(6, refilled));
    ATX_TRY_VOID(charge.bind(7, result_tokens));
    ATX_TRY_VOID(charge.bind(8, effective_refilled_at));
    ATX_TRY_VOID(step_done(charge));
    dead_letter.redrive_budget_event_count = event_count;
    dead_letter.redrive_budget_before_millis = refilled;
    dead_letter.redrive_budget_after_millis = result_tokens;
    dead_letter.redrive_budget_refilled_at = effective_refilled_at;
  }
  for (const auto &event : dead_letter.events) {
    ATX_TRY(const auto redriven_sequence,
            insert_event(database_, workspace_, event.type, event.payload, event.run_id,
                         event.task_id, event.agent_id, {}, event.subject, event.root_sequence,
                         event.redrive_count + 1));
    ATX_TRY(auto mapping,
            database_.prepare("INSERT INTO event_consumer_redrive_events("
                              "workspace,consumer_name,dead_letter_id,original_sequence,"
                              "redriven_sequence) VALUES(?1,?2,?3,?4,?5)"));
    ATX_TRY_VOID(mapping.bind(1, workspace_));
    ATX_TRY_VOID(mapping.bind(2, name));
    ATX_TRY_VOID(mapping.bind(3, dead_letter_id));
    ATX_TRY_VOID(mapping.bind(4, event.sequence));
    ATX_TRY_VOID(mapping.bind(5, redriven_sequence));
    ATX_TRY_VOID(step_done(mapping));
  }
  ATX_TRY(auto update,
          database_.prepare("UPDATE event_consumer_dead_letters SET status='redriven',"
                            "redrive_token=?4,redriven_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                            "WHERE id=?1 AND workspace=?2 AND consumer_name=?3 AND status='open'"));
  ATX_TRY_VOID(update.bind(1, dead_letter_id));
  ATX_TRY_VOID(update.bind(2, workspace_));
  ATX_TRY_VOID(update.bind(3, name));
  ATX_TRY_VOID(update.bind(4, redrive_token));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "event consumer dead-letter redrive lost a state race");
  }
  ATX_TRY(const auto redriven_event_sequence,
          append_event_consumer_dead_letter_lifecycle(database_, workspace_, name, dead_letter_id,
                                                      "redriven"));
  dead_letter.status = "redriven";
  dead_letter.redrive_token = std::string{redrive_token};
  dead_letter.redriven_event_sequence = redriven_event_sequence;
  ATX_TRY(auto time,
          database_.prepare("SELECT redriven_at FROM event_consumer_dead_letters WHERE id=?1"));
  ATX_TRY_VOID(time.bind(1, dead_letter_id));
  ATX_TRY(const auto time_step, time.step());
  if (time_step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "redriven dead letter disappeared");
  }
  dead_letter.redriven_at = time.column_text(0);
  ATX_TRY(dead_letter.redriven_events,
          redriven_events_for(database_, workspace_, name, dead_letter_id));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(dead_letter));
}

Result<EventConsumerDeadLetter> AgentDatabase::quarantine_event_consumer_dead_letter(
    std::string_view name, i64 dead_letter_id, std::string_view quarantined_by,
    std::string_view quarantine_token, std::string_view reason) {
  if (!valid_field(name, kMaximumIdBytes, false) || dead_letter_id < 1 ||
      !valid_field(quarantined_by, kMaximumIdBytes, false) ||
      !valid_field(quarantine_token, kMaximumIdBytes, false) ||
      !valid_field(reason, kMaximumTitleBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer quarantine request");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(auto consumer, find_event_consumer(database_, workspace_, name));
  {
    ATX_TRY(auto prior,
            database_.prepare("SELECT dead_letter_id,quarantined_by,reason FROM "
                              "event_consumer_dead_letter_quarantines WHERE workspace=?1 AND "
                              "consumer_name=?2 AND quarantine_token=?3"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, name));
    ATX_TRY_VOID(prior.bind(3, quarantine_token));
    ATX_TRY(const auto prior_step, prior.step());
    if (prior_step == Statement::Step::Row) {
      if (prior.column_int(0) != dead_letter_id || prior.column_text(1) != quarantined_by ||
          prior.column_text(2) != reason) {
        return Err(ErrorCode::InvalidArgument,
                   "quarantine token was reused with a different request");
      }
      ATX_TRY(auto dead_letter, find_event_consumer_dead_letter(database_, workspace_, consumer,
                                                                name, dead_letter_id));
      if (dead_letter.status != "quarantined") {
        return Err(ErrorCode::Internal, "event consumer quarantine outcome disappeared");
      }
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(dead_letter));
    }
  }
  ATX_TRY(auto dead_letter,
          find_event_consumer_dead_letter(database_, workspace_, consumer, name, dead_letter_id));
  if (dead_letter.status == "redriven") {
    return Err(ErrorCode::InvalidArgument, "redriven dead-letter batch cannot be quarantined");
  }
  if (dead_letter.status == "quarantined") {
    return Err(ErrorCode::InvalidArgument,
               "dead-letter batch was already quarantined with a different token");
  }
  if (dead_letter.status != "open" || !dead_letter.redrive_token.empty() ||
      !dead_letter.redriven_at.empty()) {
    return Err(ErrorCode::Internal, "event consumer dead-letter state is invalid");
  }
  ATX_TRY(auto insert, database_.prepare("INSERT INTO event_consumer_dead_letter_quarantines("
                                         "workspace,consumer_name,dead_letter_id,quarantine_token,"
                                         "quarantined_by,reason) VALUES(?1,?2,?3,?4,?5,?6)"));
  ATX_TRY_VOID(insert.bind(1, workspace_));
  ATX_TRY_VOID(insert.bind(2, name));
  ATX_TRY_VOID(insert.bind(3, dead_letter_id));
  ATX_TRY_VOID(insert.bind(4, quarantine_token));
  ATX_TRY_VOID(insert.bind(5, quarantined_by));
  ATX_TRY_VOID(insert.bind(6, reason));
  ATX_TRY_VOID(step_done(insert));
  ATX_TRY_VOID(append_event_consumer_dead_letter_lifecycle(database_, workspace_, name,
                                                           dead_letter_id, "quarantined"));
  ATX_TRY(dead_letter,
          find_event_consumer_dead_letter(database_, workspace_, consumer, name, dead_letter_id));
  if (dead_letter.status != "quarantined") {
    return Err(ErrorCode::Internal, "event consumer quarantine was not persisted");
  }
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(dead_letter));
}

Result<EventConsumerRecord>
AgentDatabase::checkpoint_event_consumer(std::string_view name, i64 expected_revision,
                                         i64 through_sequence, std::string_view checkpoint_token) {
  if (!valid_field(name, kMaximumIdBytes, false) || expected_revision < 1 || through_sequence < 1 ||
      !valid_field(checkpoint_token, kMaximumIdBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "invalid event consumer checkpoint request");
  }
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  {
    ATX_TRY(auto prior, database_.prepare(
                            "SELECT request_revision,through_sequence,delivery_token,outcome FROM "
                            "event_consumer_checkpoints WHERE workspace=?1 AND "
                            "consumer_name=?2 AND checkpoint_token=?3"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, name));
    ATX_TRY_VOID(prior.bind(3, checkpoint_token));
    ATX_TRY(const auto prior_step, prior.step());
    if (prior_step == Statement::Step::Row) {
      if (prior.column_int(0) != expected_revision || prior.column_int(1) != through_sequence ||
          !prior.column_text(2).empty() || prior.column_text(3) != "processed") {
        return Err(ErrorCode::InvalidArgument,
                   "checkpoint token was reused with a different request");
      }
      ATX_TRY(auto current, find_event_consumer(database_, workspace_, name));
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(current));
    }
  }
  ATX_TRY(auto before, find_event_consumer(database_, workspace_, name));
  {
    ATX_TRY(auto active,
            database_.prepare("SELECT active_delivery_token FROM event_consumers WHERE "
                              "workspace=?1 AND name=?2"));
    ATX_TRY_VOID(active.bind(1, workspace_));
    ATX_TRY_VOID(active.bind(2, name));
    ATX_TRY(const auto active_step, active.step());
    if (active_step != Statement::Step::Row) {
      return Err(ErrorCode::NotFound, "event consumer was not found");
    }
    if (!active.column_text(0).empty()) {
      return Err(ErrorCode::Unavailable,
                 "manual checkpoint cannot bypass an active event delivery");
    }
  }
  if (before.revision != expected_revision) {
    return Err(ErrorCode::Unavailable, "event consumer revision is stale");
  }
  if (through_sequence <= before.cursor_sequence) {
    return Err(ErrorCode::InvalidArgument, "event consumer checkpoint must advance the cursor");
  }
  ATX_TRY(auto event,
          database_.prepare("SELECT 1 FROM agent_events WHERE workspace=?1 AND sequence=?2 AND "
                            "(?3='' OR subject=?3) AND (sequence<=?4 OR "
                            "substr(event_type,1,9)<>'consumer.' OR subject<>?5)"));
  ATX_TRY_VOID(event.bind(1, workspace_));
  ATX_TRY_VOID(event.bind(2, through_sequence));
  ATX_TRY_VOID(event.bind(3, before.subject_filter));
  ATX_TRY_VOID(event.bind(4, before.self_control_event_cutoff_sequence));
  ATX_TRY_VOID(event.bind(5, "consumers/" + before.name));
  ATX_TRY(const auto event_step, event.step());
  if (event_step != Statement::Step::Row) {
    return Err(ErrorCode::InvalidArgument,
               "checkpoint sequence is not an event visible to this consumer");
  }
  ATX_TRY(auto update,
          database_.prepare("UPDATE event_consumers SET cursor_sequence=?4,revision=revision+1,"
                            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 "
                            "AND name=?2 AND revision=?3 AND cursor_sequence=?5"));
  ATX_TRY_VOID(update.bind(1, workspace_));
  ATX_TRY_VOID(update.bind(2, name));
  ATX_TRY_VOID(update.bind(3, expected_revision));
  ATX_TRY_VOID(update.bind(4, through_sequence));
  ATX_TRY_VOID(update.bind(5, before.cursor_sequence));
  ATX_TRY_VOID(step_done(update));
  if (database_.changes() != 1) {
    return Err(ErrorCode::Unavailable, "event consumer checkpoint lost a revision race");
  }
  ATX_TRY(auto checkpoint,
          database_.prepare("INSERT INTO event_consumer_checkpoints("
                            "workspace,consumer_name,checkpoint_token,request_revision,"
                            "previous_sequence,through_sequence,result_revision) "
                            "VALUES(?1,?2,?3,?4,?5,?6,?7)"));
  ATX_TRY_VOID(checkpoint.bind(1, workspace_));
  ATX_TRY_VOID(checkpoint.bind(2, name));
  ATX_TRY_VOID(checkpoint.bind(3, checkpoint_token));
  ATX_TRY_VOID(checkpoint.bind(4, expected_revision));
  ATX_TRY_VOID(checkpoint.bind(5, before.cursor_sequence));
  ATX_TRY_VOID(checkpoint.bind(6, through_sequence));
  ATX_TRY_VOID(checkpoint.bind(7, expected_revision + 1));
  ATX_TRY_VOID(step_done(checkpoint));
  ATX_TRY(auto after, find_event_consumer(database_, workspace_, name));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(after));
}

Result<EpisodeRecord> AgentDatabase::record_episode(const EpisodeInput &episode) {
  return record_episode_internal(episode, {});
}

Result<EpisodeRecord>
AgentDatabase::record_verified_episode(const EpisodeInput &episode,
                                       atx::kb::KnowledgeBase &knowledge_base) {
  ATX_TRY(auto source, knowledge_base.get_source(episode.source_id));
  const bool observation_exists = std::any_of(
      source.observations.begin(), source.observations.end(),
      [&](const auto &observation) { return observation.id == episode.observation_id; });
  if (!observation_exists) {
    return Err(ErrorCode::InvalidArgument,
               "episode observation does not belong to the requested knowledge source");
  }
  return record_episode_internal(episode, source.content_hash);
}

Result<EpisodeRecord>
AgentDatabase::record_episode_internal(const EpisodeInput &episode,
                                       std::string_view evidence_content_hash) {
  if (!valid_field(episode.idempotency_key, kMaximumIdBytes, false) ||
      !valid_field(episode.run_id, kMaximumIdBytes, false) ||
      !valid_field(episode.task_id, kMaximumIdBytes) ||
      !valid_field(episode.agent_id, kMaximumIdBytes, false) ||
      !valid_field(episode.source_id, kMaximumIdBytes, false) ||
      !valid_field(episode.type, kMaximumTitleBytes, false) || episode.observation_id <= 0 ||
      (!evidence_content_hash.empty() && !valid_sha256(evidence_content_hash))) {
    return Err(ErrorCode::InvalidArgument, "episode contains an invalid field");
  }
  const bool verified = !evidence_content_hash.empty();
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  ATX_TRY(auto insert, database_.prepare(
                           "INSERT OR IGNORE INTO episodes(workspace,idempotency_key,run_id,"
                           "task_id,agent_id,source_id,observation_id,episode_type,evidence_status,"
                           "evidence_content_hash,evidence_verified_at) "
                           "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,CASE WHEN ?9='verified' "
                           "THEN strftime('%Y-%m-%dT%H:%M:%fZ','now') ELSE '' END)"));
  ATX_TRY_VOID(insert.bind(1, workspace_));
  ATX_TRY_VOID(insert.bind(2, episode.idempotency_key));
  ATX_TRY_VOID(insert.bind(3, episode.run_id));
  ATX_TRY_VOID(insert.bind(4, episode.task_id));
  ATX_TRY_VOID(insert.bind(5, episode.agent_id));
  ATX_TRY_VOID(insert.bind(6, episode.source_id));
  ATX_TRY_VOID(insert.bind(7, episode.observation_id));
  ATX_TRY_VOID(insert.bind(8, episode.type));
  ATX_TRY_VOID(insert.bind(9, verified ? "verified" : "unverified"));
  ATX_TRY_VOID(insert.bind(10, evidence_content_hash));
  ATX_TRY_VOID(step_done(insert));
  const bool inserted = database_.changes() != 0;
  bool upgraded = false;
  if (verified && !inserted) {
    ATX_TRY(auto certify,
            database_.prepare("UPDATE episodes SET evidence_status='verified',"
                              "evidence_content_hash=?3,evidence_verified_at="
                              "strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE workspace=?1 AND "
                              "idempotency_key=?2 AND evidence_status='unverified'"));
    ATX_TRY_VOID(certify.bind(1, workspace_));
    ATX_TRY_VOID(certify.bind(2, episode.idempotency_key));
    ATX_TRY_VOID(certify.bind(3, evidence_content_hash));
    ATX_TRY_VOID(step_done(certify));
    upgraded = database_.changes() != 0;
  }
  ATX_TRY(auto query, database_.prepare(
                          "SELECT id,run_id,task_id,agent_id,source_id,observation_id,episode_type,"
                          "evidence_status,evidence_content_hash,evidence_verified_at,created_at "
                          "FROM episodes WHERE workspace=?1 AND idempotency_key=?2"));
  ATX_TRY_VOID(query.bind(1, workspace_));
  ATX_TRY_VOID(query.bind(2, episode.idempotency_key));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "recorded episode disappeared");
  }
  EpisodeRecord result;
  result.id = query.column_int(0);
  result.run_id = query.column_text(1);
  result.task_id = query.column_text(2);
  result.agent_id = query.column_text(3);
  result.source_id = query.column_text(4);
  result.observation_id = query.column_int(5);
  result.type = query.column_text(6);
  result.evidence_status = query.column_text(7);
  result.evidence_content_hash = query.column_text(8);
  result.evidence_verified_at = query.column_text(9);
  result.created_at = query.column_text(10);
  if (result.run_id != episode.run_id || result.task_id != episode.task_id ||
      result.agent_id != episode.agent_id || result.source_id != episode.source_id ||
      result.observation_id != episode.observation_id || result.type != episode.type ||
      (verified && (result.evidence_status != "verified" ||
                    result.evidence_content_hash != evidence_content_hash))) {
    return Err(ErrorCode::InvalidArgument,
               "episode idempotency key was reused with a different request");
  }
  if (inserted) {
    ATX_TRY_VOID(insert_event(database_, workspace_, "episode.recorded", result.source_id,
                              result.run_id, result.task_id, result.agent_id, {},
                              "episodes/" + std::to_string(result.id)));
  }
  if ((inserted && verified) || upgraded) {
    ATX_TRY_VOID(insert_event(database_, workspace_, "episode.verified", result.source_id,
                              result.run_id, result.task_id, result.agent_id, {},
                              "episodes/" + std::to_string(result.id)));
  }
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(result));
}

Result<FactRecord> AgentDatabase::put_fact(const FactInput &fact) {
  return put_fact_internal(fact, 0, {});
}

Result<FactRecord> AgentDatabase::put_verified_fact(const FactInput &fact,
                                                    i64 evidence_observation_id,
                                                    atx::kb::KnowledgeBase &knowledge_base) {
  ATX_TRY(auto source, knowledge_base.get_source(fact.evidence_source_id));
  const bool observation_exists = std::any_of(
      source.observations.begin(), source.observations.end(),
      [&](const auto &observation) { return observation.id == evidence_observation_id; });
  if (!observation_exists) {
    return Err(ErrorCode::InvalidArgument,
               "fact observation does not belong to the requested knowledge source");
  }
  return put_fact_internal(fact, evidence_observation_id, source.content_hash);
}

Result<FactRecord> AgentDatabase::put_fact_internal(const FactInput &fact,
                                                    i64 evidence_observation_id,
                                                    std::string_view evidence_content_hash) {
  if (!valid_field(fact.subject, kMaximumTitleBytes, false) ||
      !valid_field(fact.predicate, kMaximumTitleBytes, false) ||
      !valid_field(fact.object, kMaximumPayloadBytes, false) ||
      !valid_field(fact.valid_from, kMaximumTitleBytes) ||
      !valid_field(fact.valid_to, kMaximumTitleBytes) ||
      !valid_field(fact.evidence_source_id, kMaximumIdBytes) || !std::isfinite(fact.confidence) ||
      !valid_field(fact.idempotency_key, kMaximumIdBytes) || fact.confidence < 0.0 ||
      fact.confidence > 1.0 || evidence_observation_id < 0 ||
      (!evidence_content_hash.empty() && !valid_sha256(evidence_content_hash)) ||
      (!fact.valid_from.empty() && !canonical_utc_timestamp(fact.valid_from)) ||
      (!fact.valid_to.empty() && !canonical_utc_timestamp(fact.valid_to)) ||
      (!fact.valid_from.empty() && !fact.valid_to.empty() && fact.valid_from >= fact.valid_to)) {
    return Err(ErrorCode::InvalidArgument, "fact contains an invalid field or interval");
  }
  const bool no_evidence = fact.evidence_source_id.empty() && evidence_observation_id == 0 &&
                           evidence_content_hash.empty();
  const bool unverified = !fact.evidence_source_id.empty() && evidence_observation_id == 0 &&
                          evidence_content_hash.empty();
  const bool verified = !fact.evidence_source_id.empty() && evidence_observation_id > 0 &&
                        valid_sha256(evidence_content_hash);
  if (!no_evidence && !unverified && !verified) {
    return Err(ErrorCode::InvalidArgument, "fact evidence fields are inconsistent");
  }
  const std::string_view evidence_status =
      verified ? "verified" : (unverified ? "unverified" : "none");
  ATX_TRY(auto transaction, Transaction::begin_immediate(database_));
  if (!fact.idempotency_key.empty()) {
    ATX_TRY(auto prior, database_.prepare("SELECT " + std::string{kFactColumns} +
                                          " FROM facts WHERE workspace=?1 AND idempotency_key=?2"));
    ATX_TRY_VOID(prior.bind(1, workspace_));
    ATX_TRY_VOID(prior.bind(2, fact.idempotency_key));
    ATX_TRY(const auto step, prior.step());
    if (step == Statement::Step::Row) {
      ATX_TRY(auto existing, read_fact(prior));
      if (existing.subject != fact.subject || existing.predicate != fact.predicate ||
          existing.object != fact.object || existing.request_valid_from != fact.valid_from ||
          existing.valid_to != fact.valid_to ||
          existing.evidence_source_id != fact.evidence_source_id ||
          existing.evidence_observation_id != evidence_observation_id ||
          existing.evidence_content_hash != evidence_content_hash ||
          existing.evidence_status != evidence_status || existing.confidence != fact.confidence) {
        return Err(ErrorCode::InvalidArgument,
                   "fact idempotency key was reused with a different request");
      }
      ATX_TRY_VOID(transaction.commit());
      return Ok(std::move(existing));
    }
  }
  ATX_TRY(const auto transaction_time, monotonic_wall_time(database_, workspace_));
  ATX_TRY(const i64 transaction_sequence, next_temporal_sequence(database_, workspace_));
  const std::string valid_from = fact.valid_from.empty() ? transaction_time : fact.valid_from;
  if (!fact.valid_to.empty() && valid_from >= fact.valid_to) {
    return Err(ErrorCode::InvalidArgument, "fact valid interval is empty or reversed");
  }

  std::vector<FactRecord> overlaps;
  {
    ATX_TRY(auto current,
            database_.prepare("SELECT " + std::string{kFactColumns} +
                              " FROM facts WHERE workspace=?1 AND subject=?2 AND predicate=?3 "
                              "AND transaction_to IS NULL AND (?4='' OR valid_from<?4) AND "
                              "(valid_to IS NULL OR valid_to>?5) ORDER BY valid_from,id"));
    ATX_TRY_VOID(current.bind(1, workspace_));
    ATX_TRY_VOID(current.bind(2, fact.subject));
    ATX_TRY_VOID(current.bind(3, fact.predicate));
    ATX_TRY_VOID(current.bind(4, fact.valid_to));
    ATX_TRY_VOID(current.bind(5, valid_from));
    while (true) {
      ATX_TRY(const auto step, current.step());
      if (step == Statement::Step::Done) {
        break;
      }
      ATX_TRY(auto previous, read_fact(current));
      overlaps.push_back(std::move(previous));
    }
  }
  for (const auto &previous : overlaps) {
    ATX_TRY(auto close,
            database_.prepare("UPDATE facts SET transaction_to=?2,transaction_to_sequence=?4 "
                              "WHERE workspace=?1 AND id=?3 AND transaction_to IS NULL"));
    ATX_TRY_VOID(close.bind(1, workspace_));
    ATX_TRY_VOID(close.bind(2, transaction_time));
    ATX_TRY_VOID(close.bind(3, previous.id));
    ATX_TRY_VOID(close.bind(4, transaction_sequence));
    ATX_TRY_VOID(step_done(close));
  }

  const auto insert_version =
      [&](std::string_view object, std::string_view interval_start, std::string_view interval_end,
          std::string_view evidence_source, i64 evidence_observation, std::string_view content_hash,
          std::string_view status, std::string_view idempotency_key,
          std::string_view request_valid_from, double confidence, i64 supersedes) -> Result<i64> {
    ATX_TRY(auto insert,
            database_.prepare("INSERT INTO facts(workspace,subject,predicate,object,valid_from,"
                              "valid_to,transaction_from,evidence_source_id,"
                              "evidence_observation_id,evidence_content_hash,evidence_status,"
                              "idempotency_key,request_valid_from,confidence,supersedes_fact_id,"
                              "transaction_from_sequence) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,"
                              "?10,?11,?12,?13,?14,?15,?16)"));
    ATX_TRY_VOID(insert.bind(1, workspace_));
    ATX_TRY_VOID(insert.bind(2, fact.subject));
    ATX_TRY_VOID(insert.bind(3, fact.predicate));
    ATX_TRY_VOID(insert.bind(4, object));
    ATX_TRY_VOID(insert.bind(5, interval_start));
    if (interval_end.empty()) {
      ATX_TRY_VOID(insert.bind_null(6));
    } else {
      ATX_TRY_VOID(insert.bind(6, interval_end));
    }
    ATX_TRY_VOID(insert.bind(7, transaction_time));
    ATX_TRY_VOID(insert.bind(8, evidence_source));
    ATX_TRY_VOID(insert.bind(9, evidence_observation));
    ATX_TRY_VOID(insert.bind(10, content_hash));
    ATX_TRY_VOID(insert.bind(11, status));
    ATX_TRY_VOID(insert.bind(12, idempotency_key));
    ATX_TRY_VOID(insert.bind(13, request_valid_from));
    ATX_TRY_VOID(insert.bind(14, confidence));
    if (supersedes == 0) {
      ATX_TRY_VOID(insert.bind_null(15));
    } else {
      ATX_TRY_VOID(insert.bind(15, supersedes));
    }
    ATX_TRY_VOID(insert.bind(16, transaction_sequence));
    ATX_TRY_VOID(step_done(insert));
    return Ok(database_.last_insert_rowid());
  };

  // Preserve non-overlapping portions of the prior current-time belief. A
  // later fact valid from July 18 must not erase what is currently believed
  // about July 10.
  for (const auto &previous : overlaps) {
    if (previous.valid_from < valid_from) {
      ATX_TRY_VOID(insert_version(previous.object, previous.valid_from, valid_from,
                                  previous.evidence_source_id, previous.evidence_observation_id,
                                  previous.evidence_content_hash, previous.evidence_status, {}, {},
                                  previous.confidence, previous.id));
    }
    if (!fact.valid_to.empty() &&
        (previous.valid_to.empty() || fact.valid_to < previous.valid_to)) {
      ATX_TRY_VOID(insert_version(previous.object, fact.valid_to, previous.valid_to,
                                  previous.evidence_source_id, previous.evidence_observation_id,
                                  previous.evidence_content_hash, previous.evidence_status, {}, {},
                                  previous.confidence, previous.id));
    }
  }
  const i64 supersedes = overlaps.empty() ? 0 : overlaps.front().id;
  ATX_TRY(const i64 fact_id,
          insert_version(fact.object, valid_from, fact.valid_to, fact.evidence_source_id,
                         evidence_observation_id, evidence_content_hash, evidence_status,
                         fact.idempotency_key,
                         fact.idempotency_key.empty() ? std::string_view{} : fact.valid_from,
                         fact.confidence, supersedes));
  const std::string fact_subject = "facts/" + std::to_string(fact_id);
  ATX_TRY_VOID(insert_event(database_, workspace_, "fact.put", fact.predicate, {}, {}, {}, {},
                            fact_subject));
  if (verified) {
    ATX_TRY_VOID(insert_event(database_, workspace_, "fact.verified", fact.evidence_source_id, {},
                              {}, {}, {}, fact_subject));
  }
  ATX_TRY(auto query, database_.prepare("SELECT " + std::string{kFactColumns} +
                                        " FROM facts WHERE workspace=?1 AND id=?2"));
  ATX_TRY_VOID(query.bind(1, workspace_));
  ATX_TRY_VOID(query.bind(2, fact_id));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row) {
    return Err(ErrorCode::Internal, "inserted fact disappeared");
  }
  ATX_TRY(auto result, read_fact(query));
  ATX_TRY_VOID(transaction.commit());
  return Ok(std::move(result));
}

Result<std::vector<FactRecord>>
AgentDatabase::facts_as_of(std::string_view valid_time, std::string_view transaction_time,
                           std::string_view subject, std::string_view predicate, usize limit) {
  if (!valid_field(valid_time, kMaximumTitleBytes, false) ||
      !valid_field(transaction_time, kMaximumTitleBytes, false) ||
      !valid_field(subject, kMaximumTitleBytes) || !valid_field(predicate, kMaximumTitleBytes) ||
      !canonical_utc_timestamp(valid_time) || !canonical_utc_timestamp(transaction_time) ||
      limit == 0 || limit > 1'000) {
    return Err(ErrorCode::InvalidArgument, "invalid bitemporal query");
  }
  std::string sql = "SELECT " + std::string{kFactColumns} +
                    " FROM facts WHERE workspace=?1 AND valid_from<=?2 AND "
                    "(valid_to IS NULL OR valid_to>?2) AND transaction_from<=?3 AND "
                    "(transaction_to IS NULL OR transaction_to>?3)";
  i64 next_parameter = 4;
  if (!subject.empty()) {
    sql += " AND subject=?" + std::to_string(next_parameter++);
  }
  if (!predicate.empty()) {
    sql += " AND predicate=?" + std::to_string(next_parameter++);
  }
  sql += " ORDER BY subject,predicate,id LIMIT ?" + std::to_string(next_parameter);
  ATX_TRY(auto query, database_.prepare(sql));
  ATX_TRY_VOID(query.bind(1, workspace_));
  ATX_TRY_VOID(query.bind(2, valid_time));
  ATX_TRY_VOID(query.bind(3, transaction_time));
  i64 parameter = 4;
  if (!subject.empty()) {
    ATX_TRY_VOID(query.bind(parameter++, subject));
  }
  if (!predicate.empty()) {
    ATX_TRY_VOID(query.bind(parameter++, predicate));
  }
  ATX_TRY_VOID(query.bind(parameter, static_cast<i64>(limit)));
  std::vector<FactRecord> result;
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    ATX_TRY(auto fact, read_fact(query));
    result.push_back(std::move(fact));
  }
  return Ok(std::move(result));
}

Result<std::vector<FactRecord>> AgentDatabase::facts_as_of_sequence(std::string_view valid_time,
                                                                    i64 transaction_sequence,
                                                                    std::string_view subject,
                                                                    std::string_view predicate,
                                                                    usize limit) {
  if (!valid_field(valid_time, kMaximumTitleBytes, false) || transaction_sequence < 1 ||
      !valid_field(subject, kMaximumTitleBytes) || !valid_field(predicate, kMaximumTitleBytes) ||
      !canonical_utc_timestamp(valid_time) || limit == 0 || limit > 1'000) {
    return Err(ErrorCode::InvalidArgument, "invalid bitemporal sequence query");
  }
  std::string sql = "SELECT " + std::string{kFactColumns} +
                    " FROM facts WHERE workspace=?1 AND valid_from<=?2 AND "
                    "(valid_to IS NULL OR valid_to>?2) AND transaction_from_sequence<=?3 AND "
                    "(transaction_to_sequence IS NULL OR transaction_to_sequence>?3)";
  i64 next_parameter = 4;
  if (!subject.empty()) {
    sql += " AND subject=?" + std::to_string(next_parameter++);
  }
  if (!predicate.empty()) {
    sql += " AND predicate=?" + std::to_string(next_parameter++);
  }
  sql += " ORDER BY subject,predicate,id LIMIT ?" + std::to_string(next_parameter);
  ATX_TRY(auto query, database_.prepare(sql));
  ATX_TRY_VOID(query.bind(1, workspace_));
  ATX_TRY_VOID(query.bind(2, valid_time));
  ATX_TRY_VOID(query.bind(3, transaction_sequence));
  i64 parameter = 4;
  if (!subject.empty()) {
    ATX_TRY_VOID(query.bind(parameter++, subject));
  }
  if (!predicate.empty()) {
    ATX_TRY_VOID(query.bind(parameter++, predicate));
  }
  ATX_TRY_VOID(query.bind(parameter, static_cast<i64>(limit)));
  std::vector<FactRecord> result;
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    ATX_TRY(auto fact, read_fact(query));
    result.push_back(std::move(fact));
  }
  return Ok(std::move(result));
}

Status AgentDatabase::verify_integrity() {
  {
    ATX_TRY(auto query, database_.prepare("PRAGMA integrity_check"));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_text(0) != "ok") {
      return Err(ErrorCode::Internal, "SQLite integrity_check failed");
    }
  }
  {
    ATX_TRY(auto query, database_.prepare("PRAGMA foreign_key_check"));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Done) {
      return Err(ErrorCode::Internal, "foreign key integrity check failed");
    }
  }
  {
    ATX_TRY(auto query, database_.prepare(
                            "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND name IN ("
                            "'event_consumers_state_revision_insert',"
                            "'event_consumers_state_revision_update',"
                            "'event_consumers_state_revision_delete',"
                            "'event_consumer_dead_letters_state_revision_insert',"
                            "'event_consumer_dead_letters_state_revision_update',"
                            "'event_consumer_dead_letters_state_revision_delete',"
                            "'event_consumer_quarantines_state_revision_insert',"
                            "'event_consumer_quarantines_state_revision_update',"
                            "'event_consumer_quarantines_state_revision_delete')"));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 9) {
      return Err(ErrorCode::Internal, "event consumer state revision triggers are incomplete");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT (SELECT count(*) FROM event_consumers WHERE workspace=?1),"
                              "COALESCE((SELECT revision FROM event_consumer_state_revisions WHERE "
                              "workspace=?1),0),COALESCE((SELECT updated_at FROM "
                              "event_consumer_state_revisions WHERE workspace=?1),'')"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row) {
      return Err(ErrorCode::Internal, "event consumer state revision is unavailable");
    }
    const i64 consumer_count = query.column_int(0);
    const i64 state_revision = query.column_int(1);
    const std::string updated_at{query.column_text(2)};
    if (consumer_count < 0 || state_revision < 0 || (consumer_count > 0 && state_revision == 0) ||
        (state_revision == 0) != updated_at.empty() ||
        (state_revision > 0 && !canonical_utc_timestamp(updated_at))) {
      return Err(ErrorCode::Internal, "event consumer state revision invariant failed");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare(
                "SELECT count(*) FROM agent_events e LEFT JOIN agent_events root ON "
                "root.sequence=e.root_sequence WHERE e.workspace=?1 AND (e.root_sequence<1 OR "
                "e.redrive_count<0 OR root.sequence IS NULL OR root.workspace<>e.workspace OR "
                "root.root_sequence<>root.sequence OR root.redrive_count<>0 OR "
                "(e.redrive_count=0 AND (e.root_sequence<>e.sequence OR EXISTS(SELECT 1 FROM "
                "event_consumer_redrive_events m WHERE m.redriven_sequence=e.sequence))) OR "
                "(e.redrive_count>0 AND ((SELECT count(*) FROM "
                "event_consumer_redrive_events m WHERE m.redriven_sequence=e.sequence)<>1 OR "
                "NOT EXISTS(SELECT 1 FROM event_consumer_redrive_events m JOIN agent_events o "
                "ON o.sequence=m.original_sequence WHERE m.redriven_sequence=e.sequence AND "
                "m.workspace=e.workspace AND o.workspace=e.workspace AND "
                "o.root_sequence=e.root_sequence AND o.redrive_count=e.redrive_count-1))))"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "event redrive lineage invariant failed");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT event_type,subject,run_id,task_id,agent_id FROM agent_events "
                              "WHERE workspace=?1 ORDER BY sequence"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    while (true) {
      ATX_TRY(const auto step, query.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view type = query.column_text(0);
      const std::string_view subject = query.column_text(1);
      const std::string_view run_id = query.column_text(2);
      const std::string_view task_id = query.column_text(3);
      const std::string_view agent_id = query.column_text(4);
      if (!valid_field(subject, kMaximumTitleBytes)) {
        return Err(ErrorCode::Internal, "event subject contains invalid text");
      }
      if (type.starts_with("task.") && !task_id.empty() &&
          subject != "tasks/" + std::string{task_id}) {
        return Err(ErrorCode::Internal, "task event subject invariant failed");
      }
      if (type.starts_with("agent.") && !agent_id.empty() &&
          subject != "agents/" + std::string{agent_id}) {
        return Err(ErrorCode::Internal, "agent event subject invariant failed");
      }
      if (type.starts_with("run.") && !run_id.empty() && subject != "runs/" + std::string{run_id}) {
        return Err(ErrorCode::Internal, "run event subject invariant failed");
      }
      if (type.starts_with("episode.") && !subject.empty() && !subject.starts_with("episodes/")) {
        return Err(ErrorCode::Internal, "episode event subject invariant failed");
      }
      if (type.starts_with("fact.") && !subject.empty() && !subject.starts_with("facts/")) {
        return Err(ErrorCode::Internal, "fact event subject invariant failed");
      }
      if (type.starts_with("consumer.") && !subject.empty() && !subject.starts_with("consumers/")) {
        return Err(ErrorCode::Internal, "consumer event subject invariant failed");
      }
    }
  }
  {
    ATX_TRY(auto consumers,
            database_.prepare("SELECT name,subject_filter,max_delivery_attempts,start_sequence,"
                              "retry_backoff_seconds,retry_backoff_max_seconds,retry_jitter,"
                              "redrive_rate_per_second,redrive_burst_events,redrive_token_millis,"
                              "redrive_refilled_at,max_redrive_count,"
                              "self_control_event_cutoff_sequence,cursor_sequence,revision,"
                              "active_delivery_token,active_delivery_owner,"
                              "active_delivery_previous_sequence,active_delivery_through_sequence,"
                              "active_delivery_attempt,active_delivery_retry_delay_seconds,"
                              "active_delivery_expires_at,active_delivery_retry_at,(SELECT "
                              "COALESCE(max(sequence),0) FROM agent_events WHERE workspace=?1) "
                              "FROM event_consumers WHERE workspace=?1 ORDER BY name"));
    ATX_TRY_VOID(consumers.bind(1, workspace_));
    while (true) {
      ATX_TRY(const auto consumer_step, consumers.step());
      if (consumer_step == Statement::Step::Done) {
        break;
      }
      const std::string name{consumers.column_text(0)};
      const std::string subject_filter{consumers.column_text(1)};
      const i64 max_delivery_attempts = consumers.column_int(2);
      const i64 start_sequence = consumers.column_int(3);
      const i64 retry_backoff_seconds = consumers.column_int(4);
      const i64 retry_backoff_max_seconds = consumers.column_int(5);
      const std::string retry_jitter{consumers.column_text(6)};
      const i64 redrive_rate_per_second = consumers.column_int(7);
      const i64 redrive_burst_events = consumers.column_int(8);
      const i64 redrive_token_millis = consumers.column_int(9);
      const std::string redrive_refilled_at{consumers.column_text(10)};
      const i64 max_redrive_count = consumers.column_int(11);
      const i64 self_control_cutoff = consumers.column_int(12);
      const i64 cursor_sequence = consumers.column_int(13);
      const i64 revision = consumers.column_int(14);
      const std::string active_token{consumers.column_text(15)};
      const std::string active_owner{consumers.column_text(16)};
      const i64 active_previous = consumers.column_int(17);
      const i64 active_through = consumers.column_int(18);
      const i64 active_attempt = consumers.column_int(19);
      const i64 active_retry_delay = consumers.column_int(20);
      const std::string active_expires_at{consumers.column_text(21)};
      const std::string active_retry_at{consumers.column_text(22)};
      const i64 event_high_watermark = consumers.column_int(23);
      if (!valid_field(name, kMaximumIdBytes, false) ||
          !valid_field(subject_filter, kMaximumTitleBytes) || max_delivery_attempts < 0 ||
          max_delivery_attempts > 1'000 || start_sequence < 0 || retry_backoff_seconds < 0 ||
          retry_backoff_seconds > kMaximumLeaseSeconds || retry_backoff_max_seconds < 0 ||
          retry_backoff_max_seconds > kMaximumLeaseSeconds ||
          (retry_jitter != "none" && retry_jitter != "full") || redrive_rate_per_second < 0 ||
          redrive_rate_per_second > kMaximumRedriveRatePerSecond || redrive_burst_events < 0 ||
          redrive_burst_events > kMaximumRedriveBurstEvents || redrive_token_millis < 0 ||
          max_redrive_count < 0 || max_redrive_count > 1'000 || self_control_cutoff < 0 ||
          self_control_cutoff > event_high_watermark ||
          ((retry_backoff_seconds == 0 || retry_backoff_max_seconds == 0) &&
           retry_backoff_seconds != retry_backoff_max_seconds) ||
          retry_backoff_max_seconds < retry_backoff_seconds || cursor_sequence < start_sequence ||
          revision < 1) {
        return Err(ErrorCode::Internal, "event consumer state invariant failed");
      }
      const bool redrive_limited = redrive_rate_per_second > 0;
      const i64 redrive_capacity = redrive_burst_events * kRedriveTokenUnitsPerEvent;
      if ((!redrive_limited && (redrive_burst_events != 0 || redrive_token_millis != 0 ||
                                !redrive_refilled_at.empty())) ||
          (redrive_limited &&
           (redrive_burst_events < 1 || redrive_token_millis > redrive_capacity ||
            !canonical_utc_timestamp(redrive_refilled_at)))) {
        return Err(ErrorCode::Internal, "event consumer redrive budget invariant failed");
      }
      const bool has_active_delivery = !active_token.empty();
      if ((!has_active_delivery &&
           (!active_owner.empty() || active_previous != 0 || active_through != 0 ||
            active_attempt != 0 || active_retry_delay != 0 || !active_expires_at.empty() ||
            !active_retry_at.empty())) ||
          (has_active_delivery &&
           (!valid_field(active_token, kMaximumIdBytes, false) ||
            !valid_field(active_owner, kMaximumIdBytes, false) ||
            active_previous != cursor_sequence || active_through <= active_previous ||
            active_attempt < 1 || !canonical_utc_timestamp(active_expires_at) ||
            !canonical_utc_timestamp(active_retry_at) || active_retry_at < active_expires_at))) {
        return Err(ErrorCode::Internal, "event consumer active delivery invariant failed");
      }
      if (has_active_delivery) {
        const i64 retry_window = delivery_retry_backoff(retry_backoff_seconds,
                                                        retry_backoff_max_seconds, active_attempt);
        if (active_retry_delay < 0 || active_retry_delay > retry_window ||
            (retry_jitter == "none" && active_retry_delay != retry_window)) {
          return Err(ErrorCode::Internal, "event consumer active retry jitter invariant failed");
        }
      }
      i64 expected_revision = 1;
      i64 expected_cursor = start_sequence;
      ATX_TRY(
          auto checkpoints,
          database_.prepare("SELECT checkpoint_token,delivery_token,outcome,request_revision,"
                            "previous_sequence,through_sequence,result_revision FROM "
                            "event_consumer_checkpoints "
                            "WHERE workspace=?1 AND consumer_name=?2 ORDER BY result_revision"));
      ATX_TRY_VOID(checkpoints.bind(1, workspace_));
      ATX_TRY_VOID(checkpoints.bind(2, name));
      while (true) {
        ATX_TRY(const auto checkpoint_step, checkpoints.step());
        if (checkpoint_step == Statement::Step::Done) {
          break;
        }
        const std::string_view token = checkpoints.column_text(0);
        const std::string_view delivery_token = checkpoints.column_text(1);
        const std::string_view outcome = checkpoints.column_text(2);
        const i64 request_revision = checkpoints.column_int(3);
        const i64 previous_sequence = checkpoints.column_int(4);
        const i64 through_sequence = checkpoints.column_int(5);
        const i64 result_revision = checkpoints.column_int(6);
        if (!valid_field(token, kMaximumIdBytes, false) || request_revision != expected_revision ||
            previous_sequence != expected_cursor || through_sequence <= previous_sequence ||
            result_revision != request_revision + 1 ||
            (outcome != "processed" && outcome != "dead_lettered") ||
            (delivery_token.empty() && outcome != "processed")) {
          return Err(ErrorCode::Internal, "event consumer checkpoint chain invariant failed");
        }
        ATX_TRY(auto event,
                database_.prepare("SELECT 1 FROM agent_events WHERE workspace=?1 AND "
                                  "sequence=?2 AND (?3='' OR subject=?3) AND (sequence<=?4 OR "
                                  "substr(event_type,1,9)<>'consumer.' OR subject<>?5)"));
        ATX_TRY_VOID(event.bind(1, workspace_));
        ATX_TRY_VOID(event.bind(2, through_sequence));
        ATX_TRY_VOID(event.bind(3, subject_filter));
        ATX_TRY_VOID(event.bind(4, self_control_cutoff));
        ATX_TRY_VOID(event.bind(5, "consumers/" + name));
        ATX_TRY(const auto event_step, event.step());
        if (event_step != Statement::Step::Row) {
          return Err(ErrorCode::Internal,
                     "event consumer checkpoint references an invisible event");
        }
        if (!delivery_token.empty()) {
          std::string delivery_sql =
              "SELECT 1 FROM event_consumer_deliveries d WHERE d.workspace=?1 AND "
              "d.consumer_name=?2 AND d.delivery_token=?3 AND d.state=?4 AND "
              "d.request_revision=?5 AND d.previous_sequence=?6 AND d.through_sequence=?7";
          if (outcome == "dead_lettered") {
            delivery_sql += " AND EXISTS(SELECT 1 FROM event_consumer_dead_letters l WHERE "
                            "l.workspace=d.workspace AND l.consumer_name=d.consumer_name AND "
                            "l.delivery_token=d.delivery_token)";
          }
          ATX_TRY(auto delivery, database_.prepare(delivery_sql));
          ATX_TRY_VOID(delivery.bind(1, workspace_));
          ATX_TRY_VOID(delivery.bind(2, name));
          ATX_TRY_VOID(delivery.bind(3, delivery_token));
          ATX_TRY_VOID(delivery.bind(4, outcome == "processed" ? "settled" : "expired"));
          ATX_TRY_VOID(delivery.bind(5, request_revision));
          ATX_TRY_VOID(delivery.bind(6, previous_sequence));
          ATX_TRY_VOID(delivery.bind(7, through_sequence));
          ATX_TRY(const auto delivery_step, delivery.step());
          if (delivery_step != Statement::Step::Row) {
            return Err(ErrorCode::Internal,
                       "event consumer checkpoint delivery audit invariant failed");
          }
        }
        expected_revision = result_revision;
        expected_cursor = through_sequence;
      }
      if (revision != expected_revision || cursor_sequence != expected_cursor) {
        return Err(ErrorCode::Internal, "event consumer head does not match checkpoint history");
      }
      i64 active_audit_count{};
      ATX_TRY(auto deliveries,
              database_.prepare("SELECT " + std::string{kEventDeliveryColumns} +
                                ",finished_at FROM event_consumer_deliveries WHERE workspace=?1 "
                                "AND consumer_name=?2 ORDER BY id"));
      ATX_TRY_VOID(deliveries.bind(1, workspace_));
      ATX_TRY_VOID(deliveries.bind(2, name));
      while (true) {
        ATX_TRY(const auto delivery_step, deliveries.step());
        if (delivery_step == Statement::Step::Done) {
          break;
        }
        const auto delivery = read_event_delivery(deliveries);
        const std::string_view finished_at = deliveries.column_text(20);
        const bool rejected = !delivery.rejection_token.empty();
        const bool rejection_metadata_present =
            rejected || !delivery.rejection_disposition.empty() ||
            !delivery.rejection_reason.empty() || !delivery.rejected_at.empty();
        if (!valid_field(delivery.delivery_token, kMaximumIdBytes, false) ||
            !valid_field(delivery.owner, kMaximumIdBytes, false) ||
            !valid_field(delivery.request_token, kMaximumIdBytes, false) ||
            delivery.request_revision < 1 || delivery.previous_sequence < start_sequence ||
            delivery.through_sequence <= delivery.previous_sequence || delivery.attempt < 1 ||
            delivery.requested_limit < 1 || delivery.requested_limit > 1'000 ||
            delivery.lease_seconds < 1 || delivery.lease_seconds > kMaximumLeaseSeconds ||
            delivery.preceding_dead_lettered_batches < 0 ||
            delivery.preceding_dead_lettered_batches > 1 ||
            delivery.preceding_dead_lettered_events < 0 ||
            ((delivery.preceding_dead_lettered_batches == 0) !=
             (delivery.preceding_dead_lettered_events == 0)) ||
            !canonical_utc_timestamp(delivery.acquired_at) ||
            !canonical_utc_timestamp(delivery.expires_at) || delivery.retry_delay_seconds < 0 ||
            !canonical_utc_timestamp(delivery.retry_not_before) ||
            delivery.retry_not_before < delivery.expires_at ||
            (rejection_metadata_present &&
             (!rejected || !valid_field(delivery.rejection_token, kMaximumIdBytes, false) ||
              (delivery.rejection_disposition != "retry" &&
               delivery.rejection_disposition != "dead_letter") ||
              !valid_field(delivery.rejection_reason, kMaximumTitleBytes, false) ||
              !canonical_utc_timestamp(delivery.rejected_at) ||
              delivery.rejected_at != delivery.expires_at)) ||
            (!rejected && rejection_metadata_present)) {
          return Err(ErrorCode::Internal, "event consumer delivery audit invariant failed");
        }
        const i64 retry_window = delivery_retry_backoff(
            retry_backoff_seconds, retry_backoff_max_seconds, delivery.attempt);
        if (delivery.retry_delay_seconds > retry_window ||
            (retry_jitter == "none" && delivery.retry_delay_seconds != retry_window)) {
          return Err(ErrorCode::Internal, "event consumer delivery retry jitter is invalid");
        }
        ATX_TRY(auto retry_schedule,
                database_.prepare("SELECT ?1=strftime('%Y-%m-%dT%H:%M:%fZ',?2,'+' || ?3 || "
                                  "' seconds')"));
        ATX_TRY_VOID(retry_schedule.bind(1, delivery.retry_not_before));
        ATX_TRY_VOID(retry_schedule.bind(2, delivery.expires_at));
        ATX_TRY_VOID(retry_schedule.bind(3, delivery.retry_delay_seconds));
        ATX_TRY(const auto retry_schedule_step, retry_schedule.step());
        if (retry_schedule_step != Statement::Step::Row || retry_schedule.column_int(0) == 0) {
          return Err(ErrorCode::Internal, "event consumer delivery retry schedule is invalid");
        }
        ATX_TRY(auto event,
                database_.prepare("SELECT 1 FROM agent_events WHERE workspace=?1 AND "
                                  "sequence=?2 AND (?3='' OR subject=?3) AND (sequence<=?4 OR "
                                  "substr(event_type,1,9)<>'consumer.' OR subject<>?5)"));
        ATX_TRY_VOID(event.bind(1, workspace_));
        ATX_TRY_VOID(event.bind(2, delivery.through_sequence));
        ATX_TRY_VOID(event.bind(3, subject_filter));
        ATX_TRY_VOID(event.bind(4, self_control_cutoff));
        ATX_TRY_VOID(event.bind(5, "consumers/" + name));
        ATX_TRY(const auto event_step, event.step());
        if (event_step != Statement::Step::Row) {
          return Err(ErrorCode::Internal, "event consumer delivery references an invisible event");
        }
        ATX_TRY(auto event_count,
                database_.prepare("SELECT count(*) FROM agent_events WHERE workspace=?1 AND "
                                  "sequence>?2 AND sequence<=?3 AND (?4='' OR subject=?4) AND "
                                  "(sequence<=?5 OR substr(event_type,1,9)<>'consumer.' OR "
                                  "subject<>?6)"));
        ATX_TRY_VOID(event_count.bind(1, workspace_));
        ATX_TRY_VOID(event_count.bind(2, delivery.previous_sequence));
        ATX_TRY_VOID(event_count.bind(3, delivery.through_sequence));
        ATX_TRY_VOID(event_count.bind(4, subject_filter));
        ATX_TRY_VOID(event_count.bind(5, self_control_cutoff));
        ATX_TRY_VOID(event_count.bind(6, "consumers/" + name));
        ATX_TRY(const auto count_step, event_count.step());
        if (count_step != Statement::Step::Row || event_count.column_int(0) < 1 ||
            event_count.column_int(0) > delivery.requested_limit) {
          return Err(ErrorCode::Internal, "event consumer delivery batch limit invariant failed");
        }
        if (delivery.preceding_dead_lettered_batches == 1) {
          ATX_TRY(auto preceding,
                  database_.prepare("SELECT l.event_count FROM event_consumer_checkpoints c JOIN "
                                    "event_consumer_dead_letters l ON l.workspace=c.workspace AND "
                                    "l.consumer_name=c.consumer_name AND "
                                    "l.delivery_token=c.delivery_token WHERE c.workspace=?1 AND "
                                    "c.consumer_name=?2 AND c.outcome='dead_lettered' AND "
                                    "c.result_revision=?3 AND c.through_sequence=?4"));
          ATX_TRY_VOID(preceding.bind(1, workspace_));
          ATX_TRY_VOID(preceding.bind(2, name));
          ATX_TRY_VOID(preceding.bind(3, delivery.request_revision));
          ATX_TRY_VOID(preceding.bind(4, delivery.previous_sequence));
          ATX_TRY(const auto preceding_step, preceding.step());
          if (preceding_step != Statement::Step::Row ||
              preceding.column_int(0) != delivery.preceding_dead_lettered_events) {
            return Err(ErrorCode::Internal, "event delivery dead-letter response audit mismatch");
          }
        }
        ATX_TRY(auto dead_letter,
                database_.prepare("SELECT id,previous_sequence,through_sequence,delivery_attempts,"
                                  "event_count,reason,status,redrive_token,redriven_at,"
                                  "(SELECT count(*) FROM "
                                  "event_consumer_redrive_budget_charges b WHERE "
                                  "b.workspace=l.workspace AND b.consumer_name=l.consumer_name "
                                  "AND b.dead_letter_id=l.id),(SELECT count(*) FROM "
                                  "event_consumer_dead_letter_quarantines q WHERE "
                                  "q.workspace=l.workspace AND q.consumer_name=l.consumer_name "
                                  "AND q.dead_letter_id=l.id) FROM "
                                  "event_consumer_dead_letters l WHERE "
                                  "workspace=?1 AND consumer_name=?2 AND delivery_token=?3"));
        ATX_TRY_VOID(dead_letter.bind(1, workspace_));
        ATX_TRY_VOID(dead_letter.bind(2, name));
        ATX_TRY_VOID(dead_letter.bind(3, delivery.delivery_token));
        ATX_TRY(const auto dead_letter_step, dead_letter.step());
        const bool is_dead_lettered = dead_letter_step == Statement::Step::Row;
        if (delivery.state == "active") {
          ++active_audit_count;
          if (is_dead_lettered || !finished_at.empty() || delivery.delivery_token != active_token ||
              delivery.owner != active_owner || delivery.previous_sequence != active_previous ||
              delivery.through_sequence != active_through || delivery.attempt != active_attempt ||
              delivery.retry_delay_seconds != active_retry_delay ||
              delivery.expires_at != active_expires_at ||
              delivery.retry_not_before != active_retry_at ||
              delivery.request_revision != revision) {
            return Err(ErrorCode::Internal, "event consumer active delivery audit mismatch");
          }
        } else if (delivery.state == "settled") {
          if (rejected || is_dead_lettered || finished_at.empty()) {
            return Err(ErrorCode::Internal, "settled event delivery has no finish time");
          }
          ATX_TRY(auto checkpoint,
                  database_.prepare("SELECT 1 FROM event_consumer_checkpoints WHERE workspace=?1 "
                                    "AND consumer_name=?2 AND delivery_token=?3 AND "
                                    "request_revision=?4 AND previous_sequence=?5 AND "
                                    "through_sequence=?6 AND outcome='processed'"));
          ATX_TRY_VOID(checkpoint.bind(1, workspace_));
          ATX_TRY_VOID(checkpoint.bind(2, name));
          ATX_TRY_VOID(checkpoint.bind(3, delivery.delivery_token));
          ATX_TRY_VOID(checkpoint.bind(4, delivery.request_revision));
          ATX_TRY_VOID(checkpoint.bind(5, delivery.previous_sequence));
          ATX_TRY_VOID(checkpoint.bind(6, delivery.through_sequence));
          ATX_TRY(const auto checkpoint_step, checkpoint.step());
          if (checkpoint_step != Statement::Step::Row) {
            return Err(ErrorCode::Internal, "settled event delivery has no checkpoint");
          }
        } else if (delivery.state == "expired") {
          if (finished_at.empty()) {
            return Err(ErrorCode::Internal, "expired event delivery has no finish time");
          }
          ATX_TRY(auto successor,
                  database_.prepare("SELECT 1 FROM event_consumer_deliveries WHERE workspace=?1 "
                                    "AND consumer_name=?2 AND previous_sequence=?3 AND "
                                    "through_sequence=?4 AND attempt=?5"));
          ATX_TRY_VOID(successor.bind(1, workspace_));
          ATX_TRY_VOID(successor.bind(2, name));
          ATX_TRY_VOID(successor.bind(3, delivery.previous_sequence));
          ATX_TRY_VOID(successor.bind(4, delivery.through_sequence));
          ATX_TRY_VOID(successor.bind(5, delivery.attempt + 1));
          ATX_TRY(const auto successor_step, successor.step());
          const bool has_successor = successor_step == Statement::Step::Row;
          if (has_successor == is_dead_lettered) {
            return Err(ErrorCode::Internal,
                       "expired event delivery must have one successor or dead letter");
          }
          const bool explicit_rejection = delivery.rejection_disposition == "dead_letter";
          const std::string_view expected_dead_letter_reason =
              explicit_rejection ? "explicit_rejection"
                                 : (rejected ? "max_delivery_attempts_rejected"
                                             : "max_delivery_attempts_exceeded");
          if (is_dead_lettered &&
              (dead_letter.column_int(1) != delivery.previous_sequence ||
               dead_letter.column_int(2) != delivery.through_sequence ||
               dead_letter.column_int(3) != delivery.attempt ||
               dead_letter.column_int(4) != event_count.column_int(0) ||
               dead_letter.column_text(5) != expected_dead_letter_reason ||
               (!explicit_rejection &&
                (max_delivery_attempts == 0 || delivery.attempt < max_delivery_attempts)))) {
            return Err(ErrorCode::Internal, "event consumer dead-letter audit mismatch");
          }
          if (is_dead_lettered) {
            const i64 dead_letter_id = dead_letter.column_int(0);
            const i64 dead_letter_event_count = dead_letter.column_int(4);
            const std::string status{dead_letter.column_text(6)};
            const std::string redrive_token{dead_letter.column_text(7)};
            const std::string redriven_at{dead_letter.column_text(8)};
            const i64 budget_charge_count = dead_letter.column_int(9);
            const i64 quarantine_count = dead_letter.column_int(10);
            ATX_TRY(
                auto mappings,
                database_.prepare("SELECT count(*),COALESCE(sum(CASE WHEN o.workspace<>?4 OR "
                                  "r.workspace<>?4 OR o.sequence<=?5 OR o.sequence>?6 OR "
                                  "r.sequence<=?6 OR o.run_id<>r.run_id OR o.task_id<>r.task_id "
                                  "OR o.agent_id<>r.agent_id OR o.event_type<>r.event_type OR "
                                  "o.subject<>r.subject OR o.payload<>r.payload OR "
                                  "o.root_sequence<>r.root_sequence OR "
                                  "r.redrive_count<>o.redrive_count+1 OR "
                                  "(?7>0 AND r.redrive_count>?7) THEN 1 ELSE 0 "
                                  "END),0) FROM event_consumer_redrive_events m JOIN "
                                  "agent_events o ON o.sequence=m.original_sequence JOIN "
                                  "agent_events r ON r.sequence=m.redriven_sequence WHERE "
                                  "m.workspace=?1 AND m.consumer_name=?2 AND "
                                  "m.dead_letter_id=?3"));
            ATX_TRY_VOID(mappings.bind(1, workspace_));
            ATX_TRY_VOID(mappings.bind(2, name));
            ATX_TRY_VOID(mappings.bind(3, dead_letter_id));
            ATX_TRY_VOID(mappings.bind(4, workspace_));
            ATX_TRY_VOID(mappings.bind(5, delivery.previous_sequence));
            ATX_TRY_VOID(mappings.bind(6, delivery.through_sequence));
            ATX_TRY_VOID(mappings.bind(7, max_redrive_count));
            ATX_TRY(const auto mappings_step, mappings.step());
            if (mappings_step != Statement::Step::Row) {
              return Err(ErrorCode::Internal, "event consumer redrive mapping query failed");
            }
            const i64 mapping_count = mappings.column_int(0);
            const i64 mapping_mismatches = mappings.column_int(1);
            const bool open = status == "open" && redrive_token.empty() && redriven_at.empty() &&
                              mapping_count == 0 && budget_charge_count == 0 &&
                              quarantine_count >= 0 && quarantine_count <= 1;
            const bool redriven =
                status == "redriven" && valid_field(redrive_token, kMaximumIdBytes, false) &&
                canonical_utc_timestamp(redriven_at) && mapping_count == dead_letter_event_count &&
                mapping_mismatches == 0 && budget_charge_count == (redrive_limited ? 1 : 0) &&
                quarantine_count == 0;
            if (!open && !redriven) {
              return Err(ErrorCode::Internal,
                         "event consumer dead-letter redrive invariant failed");
            }
            if (quarantine_count == 1) {
              ATX_TRY(auto quarantine,
                      database_.prepare("SELECT quarantine_token,quarantined_by,reason,"
                                        "quarantined_at FROM "
                                        "event_consumer_dead_letter_quarantines WHERE "
                                        "workspace=?1 AND consumer_name=?2 AND dead_letter_id=?3"));
              ATX_TRY_VOID(quarantine.bind(1, workspace_));
              ATX_TRY_VOID(quarantine.bind(2, name));
              ATX_TRY_VOID(quarantine.bind(3, dead_letter_id));
              ATX_TRY(const auto quarantine_step, quarantine.step());
              if (quarantine_step != Statement::Step::Row || status != "open" ||
                  !valid_field(quarantine.column_text(0), kMaximumIdBytes, false) ||
                  !valid_field(quarantine.column_text(1), kMaximumIdBytes, false) ||
                  !valid_field(quarantine.column_text(2), kMaximumTitleBytes, false) ||
                  !canonical_utc_timestamp(quarantine.column_text(3)) || mapping_count != 0 ||
                  budget_charge_count != 0) {
                return Err(ErrorCode::Internal,
                           "event consumer dead-letter quarantine invariant failed");
              }
            }
          }
        } else {
          return Err(ErrorCode::Internal, "event consumer delivery state is invalid");
        }
      }
      if (active_audit_count != (has_active_delivery ? 1 : 0)) {
        return Err(ErrorCode::Internal, "event consumer active delivery count invariant failed");
      }
      i64 budget_charge_count{};
      i64 previous_budget_result = redrive_capacity;
      std::string previous_budget_time;
      ATX_TRY(auto charges,
              database_.prepare("SELECT b.dead_letter_id,b.redrive_token,b.event_count,"
                                "b.refilled_token_millis,b.result_token_millis,b.refilled_at,"
                                "l.status,l.redrive_token,l.event_count FROM "
                                "event_consumer_redrive_budget_charges b JOIN "
                                "event_consumer_dead_letters l ON l.id=b.dead_letter_id WHERE "
                                "b.workspace=?1 AND b.consumer_name=?2 ORDER BY b.id"));
      ATX_TRY_VOID(charges.bind(1, workspace_));
      ATX_TRY_VOID(charges.bind(2, name));
      while (true) {
        ATX_TRY(const auto charge_step, charges.step());
        if (charge_step == Statement::Step::Done) {
          break;
        }
        ++budget_charge_count;
        const i64 event_count = charges.column_int(2);
        const i64 refilled_tokens = charges.column_int(3);
        const i64 result_tokens = charges.column_int(4);
        const std::string refilled_at{charges.column_text(5)};
        if (!redrive_limited || charges.column_int(0) < 1 ||
            !valid_field(charges.column_text(1), kMaximumIdBytes, false) || event_count < 1 ||
            event_count > redrive_burst_events || refilled_tokens < 0 ||
            refilled_tokens > redrive_capacity ||
            result_tokens != refilled_tokens - event_count * kRedriveTokenUnitsPerEvent ||
            !canonical_utc_timestamp(refilled_at) || charges.column_text(6) != "redriven" ||
            charges.column_text(7) != charges.column_text(1) ||
            charges.column_int(8) != event_count) {
          return Err(ErrorCode::Internal, "event consumer redrive budget charge is invalid");
        }
        i64 expected_refilled = redrive_capacity;
        if (budget_charge_count > 1) {
          if (refilled_at < previous_budget_time) {
            return Err(ErrorCode::Internal, "event consumer redrive budget time moved backward");
          }
          ATX_TRY(const auto elapsed, elapsed_milliseconds(previous_budget_time, refilled_at));
          const i64 headroom = redrive_capacity - previous_budget_result;
          const i64 earned = elapsed > headroom / redrive_rate_per_second
                                 ? headroom
                                 : elapsed * redrive_rate_per_second;
          expected_refilled = previous_budget_result + earned;
        }
        if (refilled_tokens != expected_refilled) {
          return Err(ErrorCode::Internal, "event consumer redrive budget charge chain is invalid");
        }
        previous_budget_result = result_tokens;
        previous_budget_time = refilled_at;
      }
      if ((!redrive_limited && budget_charge_count != 0) ||
          (redrive_limited && budget_charge_count == 0 &&
           redrive_token_millis != redrive_capacity) ||
          (redrive_limited && budget_charge_count > 0 &&
           (redrive_token_millis != previous_budget_result ||
            redrive_refilled_at != previous_budget_time))) {
        return Err(ErrorCode::Internal, "event consumer redrive budget head is invalid");
      }
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM event_consumer_redrive_events m JOIN "
                              "event_consumer_dead_letters d ON d.id=m.dead_letter_id WHERE "
                              "(m.workspace=?1 OR d.workspace=?1) AND (m.workspace<>d.workspace OR "
                              "m.consumer_name<>d.consumer_name)"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "cross-consumer dead-letter redrive mapping detected");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM event_consumer_redrive_budget_charges b JOIN "
                              "event_consumer_dead_letters d ON d.id=b.dead_letter_id WHERE "
                              "(b.workspace=?1 OR d.workspace=?1) AND (b.workspace<>d.workspace OR "
                              "b.consumer_name<>d.consumer_name)"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "cross-consumer redrive budget charge detected");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM event_consumer_dead_letter_quarantines q JOIN "
                              "event_consumer_dead_letters d ON d.id=q.dead_letter_id WHERE "
                              "(q.workspace=?1 OR d.workspace=?1) AND (q.workspace<>d.workspace OR "
                              "q.consumer_name<>d.consumer_name)"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "cross-consumer dead-letter quarantine detected");
    }
  }
  {
    ATX_TRY(auto count_query,
            database_.prepare("SELECT count(*) FROM event_consumers WHERE workspace=?1"));
    ATX_TRY_VOID(count_query.bind(1, workspace_));
    ATX_TRY(const auto count_step, count_query.step());
    if (count_step != Statement::Step::Row) {
      return Err(ErrorCode::Internal, "event consumer lifecycle epoch query failed");
    }
    const i64 consumer_count = count_query.column_int(0);
    ATX_TRY(auto epoch, database_.prepare("SELECT activated_at,event_high_watermark FROM "
                                          "event_consumer_lifecycle_epochs WHERE workspace=?1"));
    ATX_TRY_VOID(epoch.bind(1, workspace_));
    ATX_TRY(const auto epoch_step, epoch.step());
    if ((consumer_count == 0 && epoch_step != Statement::Step::Done) ||
        (consumer_count > 0 &&
         (epoch_step != Statement::Step::Row || !canonical_utc_timestamp(epoch.column_text(0)) ||
          epoch.column_int(1) < 0))) {
      return Err(ErrorCode::Internal, "event consumer lifecycle epoch invariant failed");
    }
    if (epoch_step == Statement::Step::Row) {
      ATX_TRY(auto high_watermark,
              database_.prepare("SELECT COALESCE(max(sequence),0) FROM agent_events WHERE "
                                "workspace=?1"));
      ATX_TRY_VOID(high_watermark.bind(1, workspace_));
      ATX_TRY(const auto high_watermark_step, high_watermark.step());
      if (high_watermark_step != Statement::Step::Row ||
          epoch.column_int(1) > high_watermark.column_int(0)) {
        return Err(ErrorCode::Internal, "event consumer lifecycle activation watermark is invalid");
      }
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare(
                "SELECT count(*) FROM event_consumer_dead_letters d WHERE d.workspace=?1 AND ("
                "(SELECT count(*) FROM event_consumer_dead_letter_lifecycle_events x WHERE "
                "x.workspace=d.workspace AND x.consumer_name=d.consumer_name AND "
                "x.dead_letter_id=d.id AND x.transition='dead_lettered')<>1 OR (SELECT count(*) "
                "FROM event_consumer_dead_letter_lifecycle_events x WHERE "
                "x.workspace=d.workspace AND x.consumer_name=d.consumer_name AND "
                "x.dead_letter_id=d.id AND x.transition='redriven')<>(d.status='redriven') OR "
                "(SELECT count(*) FROM event_consumer_dead_letter_lifecycle_events x WHERE "
                "x.workspace=d.workspace AND x.consumer_name=d.consumer_name AND "
                "x.dead_letter_id=d.id AND x.transition='quarantined')<>(SELECT count(*) FROM "
                "event_consumer_dead_letter_quarantines q WHERE q.workspace=d.workspace AND "
                "q.consumer_name=d.consumer_name AND q.dead_letter_id=d.id))"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "event consumer lifecycle coverage invariant failed");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare(
                "SELECT count(*) FROM event_consumer_dead_letter_lifecycle_events x JOIN "
                "event_consumer_dead_letters d ON d.id=x.dead_letter_id JOIN "
                "event_consumer_lifecycle_epochs p ON p.workspace=x.workspace LEFT JOIN "
                "event_consumer_dead_letter_quarantines q ON q.workspace=x.workspace AND "
                "q.consumer_name=x.consumer_name AND q.dead_letter_id=x.dead_letter_id LEFT JOIN "
                "agent_events e ON e.sequence=x.event_sequence WHERE (x.workspace=?1 OR "
                "d.workspace=?1) AND (x.workspace<>d.workspace OR "
                "x.consumer_name<>d.consumer_name OR "
                "(x.transition='dead_lettered' AND x.transition_at<>d.created_at) OR "
                "(x.transition='redriven' AND (d.status<>'redriven' OR "
                "x.transition_at<>d.redriven_at)) OR (x.transition='quarantined' AND (q.id IS "
                "NULL OR d.status<>'open' OR x.transition_at<>q.quarantined_at)) OR (x.legacy=1 "
                "AND (x.event_sequence IS NOT NULL OR x.transition_at>p.activated_at)) OR "
                "(x.legacy=0 AND (x.event_sequence IS NULL OR "
                "x.event_sequence<=p.event_high_watermark OR e.sequence IS NULL OR "
                "e.workspace<>x.workspace OR e.event_type<>CASE x.transition WHEN "
                "'dead_lettered' THEN 'consumer.dead_lettered' WHEN 'redriven' THEN "
                "'consumer.dead_letter_redriven' ELSE 'consumer.dead_letter_quarantined' END OR "
                "e.subject<>'consumers/'||x.consumer_name OR e.payload<>CAST(x.dead_letter_id AS "
                "TEXT) OR e.run_id<>'' OR e.task_id<>'' OR e.agent_id<>'' OR "
                "e.root_sequence<>e.sequence OR e.redrive_count<>0 OR "
                "e.created_at<x.transition_at)))"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "event consumer lifecycle event invariant failed");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT transition_at FROM "
                              "event_consumer_dead_letter_lifecycle_events WHERE workspace=?1"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    while (true) {
      ATX_TRY(const auto step, query.step());
      if (step == Statement::Step::Done) {
        break;
      }
      if (!canonical_utc_timestamp(query.column_text(0))) {
        return Err(ErrorCode::Internal, "event consumer lifecycle transition time is invalid");
      }
    }
  }
  {
    ATX_TRY(
        auto query,
        database_.prepare(
            "SELECT count(*) FROM agent_events e WHERE e.workspace=?1 AND e.event_type IN "
            "('consumer.dead_lettered','consumer.dead_letter_redriven',"
            "'consumer.dead_letter_quarantined') AND e.redrive_count=0 AND (SELECT count(*) FROM "
            "event_consumer_dead_letter_lifecycle_events x WHERE "
            "x.event_sequence=e.sequence)<>1"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "orphan event consumer lifecycle occurrence detected");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM tasks WHERE (status='leased' AND "
                              "(lease_owner='' OR lease_token='' OR lease_expires_at='')) OR "
                              "(status<>'leased' AND (lease_owner<>'' OR lease_token<>'' OR "
                              "lease_expires_at<>'')) OR attempts>max_attempts"));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "task lease invariant failed");
    }
  }
  {
    ATX_TRY(auto query, database_.prepare(
                            "SELECT count(*) FROM task_dependencies d JOIN tasks child ON "
                            "child.workspace=d.workspace AND child.id=d.task_id JOIN tasks parent "
                            "ON parent.workspace=d.workspace AND parent.id=d.depends_on_task_id "
                            "WHERE d.run_id<>child.run_id OR d.run_id<>parent.run_id"));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "cross-run dependency invariant failed");
    }
  }
  {
    ATX_TRY(auto query, database_.prepare(
                            "WITH RECURSIVE reach(start,node) AS ("
                            "SELECT task_id,depends_on_task_id FROM task_dependencies WHERE "
                            "workspace=?1 UNION SELECT reach.start,d.depends_on_task_id FROM "
                            "reach JOIN task_dependencies d ON d.workspace=?1 AND "
                            "d.task_id=reach.node) SELECT 1 FROM reach WHERE start=node LIMIT 1"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Done) {
      return Err(ErrorCode::Internal, "task dependency cycle detected");
    }
  }
  {
    ATX_TRY(auto query, database_.prepare(
                            "SELECT count(*) FROM task_dependencies d JOIN tasks child ON "
                            "child.workspace=d.workspace AND child.id=d.task_id JOIN tasks parent "
                            "ON parent.workspace=d.workspace AND parent.id=d.depends_on_task_id "
                            "WHERE d.workspace=?1 AND child.status IN ('queued','leased') AND "
                            "parent.status IN ('failed','cancelled')"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal,
                 "live task remains blocked by a terminal unsuccessful dependency");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM episodes e LEFT JOIN tasks t ON "
                              "t.workspace=e.workspace AND t.id=e.task_id WHERE e.workspace=?1 AND "
                              "e.task_id<>'' AND (t.id IS NULL OR t.run_id<>e.run_id)"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "episode task linkage invariant failed");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM facts a JOIN facts b ON "
                              "a.workspace=b.workspace AND a.subject=b.subject AND "
                              "a.predicate=b.predicate AND a.id<b.id WHERE a.workspace=?1 AND "
                              "a.transaction_to IS NULL AND b.transaction_to IS NULL AND "
                              "(a.valid_to IS NULL OR b.valid_from<a.valid_to) AND "
                              "(b.valid_to IS NULL OR a.valid_from<b.valid_to)"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "current bitemporal fact intervals overlap");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM facts WHERE transaction_from_sequence<=0 OR "
                              "(transaction_to_sequence IS NOT NULL AND "
                              "transaction_to_sequence<=transaction_from_sequence)"));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "temporal sequence invariant failed");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT count(*) FROM tasks t JOIN runs r ON "
                              "r.workspace=t.workspace AND r.id=t.run_id WHERE t.workspace=?1 AND "
                              "r.status<>'active' AND t.status IN ('queued','leased')"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    ATX_TRY(const auto step, query.step());
    if (step != Statement::Step::Row || query.column_int(0) != 0) {
      return Err(ErrorCode::Internal, "terminal run retains live tasks");
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT evidence_status,evidence_content_hash,evidence_verified_at "
                              "FROM episodes WHERE workspace=?1"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    while (true) {
      ATX_TRY(const auto step, query.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view status = query.column_text(0);
      const std::string_view content_hash = query.column_text(1);
      const std::string_view verified_at = query.column_text(2);
      const bool unverified = status == "unverified" && content_hash.empty() && verified_at.empty();
      const bool verified = status == "verified" && valid_sha256(content_hash) &&
                            canonical_utc_timestamp(verified_at);
      if (!unverified && !verified) {
        return Err(ErrorCode::Internal, "episode evidence certification invariant failed");
      }
    }
  }
  {
    ATX_TRY(auto query, database_.prepare("SELECT status,result_source_id,result_observation_id,"
                                          "result_content_hash,result_evidence_status FROM tasks "
                                          "WHERE workspace=?1"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    while (true) {
      ATX_TRY(const auto step, query.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view task_status = query.column_text(0);
      const std::string_view source_id = query.column_text(1);
      const i64 observation_id = query.column_int(2);
      const std::string_view content_hash = query.column_text(3);
      const std::string_view evidence_status = query.column_text(4);
      const bool none = evidence_status == "none" && source_id.empty() && observation_id == 0 &&
                        content_hash.empty();
      const bool unverified = evidence_status == "unverified" && !source_id.empty() &&
                              observation_id == 0 && content_hash.empty();
      const bool verified = evidence_status == "verified" && !source_id.empty() &&
                            observation_id > 0 && valid_sha256(content_hash);
      if ((!none && !unverified && !verified) || (task_status != "completed" && !none)) {
        return Err(ErrorCode::Internal, "task result evidence invariant failed");
      }
    }
  }
  {
    ATX_TRY(auto query,
            database_.prepare("SELECT evidence_source_id,evidence_observation_id,"
                              "evidence_content_hash,evidence_status,idempotency_key,"
                              "request_valid_from,valid_from FROM facts WHERE workspace=?1"));
    ATX_TRY_VOID(query.bind(1, workspace_));
    while (true) {
      ATX_TRY(const auto step, query.step());
      if (step == Statement::Step::Done) {
        break;
      }
      const std::string_view source_id = query.column_text(0);
      const i64 observation_id = query.column_int(1);
      const std::string_view content_hash = query.column_text(2);
      const std::string_view status = query.column_text(3);
      const std::string_view idempotency_key = query.column_text(4);
      const std::string_view request_valid_from = query.column_text(5);
      const std::string_view valid_from = query.column_text(6);
      const bool none =
          status == "none" && source_id.empty() && observation_id == 0 && content_hash.empty();
      const bool unverified = status == "unverified" && !source_id.empty() && observation_id == 0 &&
                              content_hash.empty();
      const bool verified = status == "verified" && !source_id.empty() && observation_id > 0 &&
                            valid_sha256(content_hash);
      if (!none && !unverified && !verified) {
        return Err(ErrorCode::Internal, "fact evidence certification invariant failed");
      }
      const bool unkeyed = idempotency_key.empty() && request_valid_from.empty();
      const bool keyed =
          valid_field(idempotency_key, kMaximumIdBytes, false) &&
          (request_valid_from.empty() ||
           (request_valid_from == valid_from && canonical_utc_timestamp(request_valid_from)));
      if (!unkeyed && !keyed) {
        return Err(ErrorCode::Internal, "fact idempotency invariant failed");
      }
    }
  }
  return Ok();
}

Status AgentDatabase::verify_evidence_links(atx::kb::KnowledgeBase &knowledge_base) {
  ATX_TRY(auto query,
          database_.prepare("SELECT source_id,observation_id,evidence_status,evidence_content_hash "
                            "FROM episodes WHERE workspace=?1 ORDER BY source_id,observation_id"));
  ATX_TRY_VOID(query.bind(1, workspace_));
  while (true) {
    ATX_TRY(const auto step, query.step());
    if (step == Statement::Step::Done) {
      break;
    }
    const std::string source_id{query.column_text(0)};
    const i64 observation_id = query.column_int(1);
    const std::string evidence_status{query.column_text(2)};
    const std::string evidence_content_hash{query.column_text(3)};
    auto source = knowledge_base.get_source(source_id);
    if (!source) {
      return Err(ErrorCode::Internal, "episode references missing atx-kb source " + source_id);
    }
    const bool observation_exists =
        std::any_of(source->observations.begin(), source->observations.end(),
                    [&](const auto &observation) { return observation.id == observation_id; });
    if (!observation_exists) {
      return Err(ErrorCode::Internal,
                 "episode references missing atx-kb observation " + std::to_string(observation_id));
    }
    if (evidence_status == "verified" && source->content_hash != evidence_content_hash) {
      return Err(ErrorCode::Internal,
                 "verified episode content hash drifted for atx-kb source " + source_id);
    }
  }
  ATX_TRY(auto tasks,
          database_.prepare("SELECT id,result_source_id,result_observation_id,"
                            "result_content_hash,result_evidence_status FROM tasks WHERE "
                            "workspace=?1 AND result_evidence_status<>'none' ORDER BY id"));
  ATX_TRY_VOID(tasks.bind(1, workspace_));
  while (true) {
    ATX_TRY(const auto step, tasks.step());
    if (step == Statement::Step::Done) {
      break;
    }
    const std::string task_id{tasks.column_text(0)};
    const std::string source_id{tasks.column_text(1)};
    const i64 observation_id = tasks.column_int(2);
    const std::string content_hash{tasks.column_text(3)};
    const std::string_view status = tasks.column_text(4);
    auto source = knowledge_base.get_source(source_id);
    if (!source) {
      return Err(ErrorCode::Internal, "task result references missing atx-kb source " + source_id);
    }
    if (status == "verified") {
      const bool observation_exists =
          std::any_of(source->observations.begin(), source->observations.end(),
                      [&](const auto &observation) { return observation.id == observation_id; });
      if (!observation_exists || source->content_hash != content_hash) {
        return Err(ErrorCode::Internal,
                   "task result evidence certification failed for task " + task_id);
      }
    }
  }
  ATX_TRY(auto facts,
          database_.prepare("SELECT id,evidence_source_id,evidence_observation_id,"
                            "evidence_content_hash,evidence_status FROM facts WHERE workspace=?1 "
                            "AND evidence_status<>'none' ORDER BY id"));
  ATX_TRY_VOID(facts.bind(1, workspace_));
  while (true) {
    ATX_TRY(const auto step, facts.step());
    if (step == Statement::Step::Done) {
      break;
    }
    const i64 fact_id = facts.column_int(0);
    const std::string source_id{facts.column_text(1)};
    const i64 observation_id = facts.column_int(2);
    const std::string content_hash{facts.column_text(3)};
    const std::string_view status = facts.column_text(4);
    auto source = knowledge_base.get_source(source_id);
    if (!source) {
      return Err(ErrorCode::Internal, "fact references missing atx-kb source " + source_id);
    }
    if (status == "verified") {
      const bool observation_exists =
          std::any_of(source->observations.begin(), source->observations.end(),
                      [&](const auto &observation) { return observation.id == observation_id; });
      if (!observation_exists || source->content_hash != content_hash) {
        return Err(ErrorCode::Internal,
                   "fact evidence certification failed for fact " + std::to_string(fact_id));
      }
    }
  }
  return Ok();
}

Result<atx::core::db::BackupReport>
AgentDatabase::backup_to(std::string_view destination_path,
                         const atx::core::db::BackupOptions &options) {
  if (!valid_field(destination_path, kMaximumPayloadBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "agent database backup path is invalid");
  }
  namespace fs = std::filesystem;
  const fs::path destination{std::string{destination_path}};
  fs::path partial = destination;
  partial += ".partial";
  std::error_code filesystem_error;
  if (fs::exists(destination, filesystem_error)) {
    return Err(ErrorCode::AlreadyExists, "agent database backup destination already exists");
  }
  if (filesystem_error) {
    return Err(ErrorCode::IoError, "cannot inspect agent database backup destination");
  }
  if (fs::exists(partial, filesystem_error)) {
    return Err(ErrorCode::AlreadyExists, "agent database partial backup already exists");
  }
  if (filesystem_error) {
    return Err(ErrorCode::IoError, "cannot inspect agent database partial backup path");
  }
  const auto cleanup = [&] {
    std::error_code ignored;
    fs::remove(partial, ignored);
    fs::remove(partial.string() + "-wal", ignored);
    fs::remove(partial.string() + "-shm", ignored);
  };
  auto copied = [&]() -> Result<atx::core::db::BackupReport> {
    ATX_TRY(auto backup_database, Database::open(partial.string()));
    return database_.backup_to(backup_database, options);
  }();
  if (!copied) {
    cleanup();
    return Err(std::move(copied).error());
  }
  auto verified = [&]() -> Status {
    ATX_TRY(auto restored, AgentDatabase::open(partial.string(), workspace_));
    return restored.verify_integrity();
  }();
  if (!verified) {
    cleanup();
    return Err(std::move(verified).error());
  }
  auto checkpointed = [&]() -> Status {
    ATX_TRY(auto restored_database, Database::open(partial.string()));
    ATX_TRY_VOID(restored_database.set_busy_timeout(5'000));
    ATX_TRY(auto checkpoint, restored_database.prepare("PRAGMA wal_checkpoint(TRUNCATE)"));
    ATX_TRY(const auto step, checkpoint.step());
    if (step != Statement::Step::Row || checkpoint.column_int(0) != 0) {
      return Err(ErrorCode::Unavailable, "agent database backup WAL checkpoint is busy");
    }
    return Ok();
  }();
  if (!checkpointed) {
    cleanup();
    return Err(std::move(checkpointed).error());
  }
  {
    std::error_code ignored;
    fs::remove(partial.string() + "-wal", ignored);
    fs::remove(partial.string() + "-shm", ignored);
  }
  fs::create_hard_link(partial, destination, filesystem_error);
  if (filesystem_error) {
    cleanup();
    return Err(ErrorCode::IoError, "cannot publish verified agent database backup");
  }
  fs::remove(partial, filesystem_error);
  return copied;
}

Result<BackupPairReport> AgentDatabase::backup_pair(atx::kb::KnowledgeBase &knowledge_base,
                                                    std::string_view destination_prefix,
                                                    const atx::core::db::BackupOptions &options) {
  if (!valid_field(destination_prefix, kMaximumPayloadBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "backup-pair destination prefix is invalid");
  }
  namespace fs = std::filesystem;
  const std::string prefix{destination_prefix};
  const fs::path coordination_path{prefix + ".atx-db.sqlite"};
  const fs::path knowledge_path{prefix + ".atx-kb.sqlite"};
  const fs::path manifest_path{prefix + ".manifest.sqlite"};
  fs::path manifest_partial = manifest_path;
  manifest_partial += ".partial";
  std::error_code filesystem_error;
  for (const auto &path : {coordination_path, knowledge_path, manifest_path, manifest_partial}) {
    if (fs::exists(path, filesystem_error)) {
      return Err(ErrorCode::AlreadyExists, "backup-pair destination already exists");
    }
    if (filesystem_error) {
      return Err(ErrorCode::IoError, "cannot inspect backup-pair destination");
    }
  }
  bool coordination_created = false;
  bool knowledge_created = false;
  const auto remove_database = [](const fs::path &path) {
    std::error_code ignored;
    fs::remove(path, ignored);
    fs::remove(path.string() + "-wal", ignored);
    fs::remove(path.string() + "-shm", ignored);
    fs::remove(path.string() + ".partial", ignored);
    fs::remove(path.string() + ".partial-wal", ignored);
    fs::remove(path.string() + ".partial-shm", ignored);
  };
  const auto cleanup = [&] {
    if (coordination_created) {
      remove_database(coordination_path);
    }
    if (knowledge_created) {
      remove_database(knowledge_path);
    }
    remove_database(manifest_path);
  };

  auto coordination_backup = backup_to(coordination_path.string(), options);
  if (!coordination_backup) {
    return Err(std::move(coordination_backup).error());
  }
  coordination_created = true;
  auto knowledge_backup = knowledge_base.backup_to(knowledge_path.string(), options);
  if (!knowledge_backup) {
    cleanup();
    return Err(std::move(knowledge_backup).error());
  }
  knowledge_created = true;

  auto evidence =
      verify_backup_pair_domains(coordination_path.string(), knowledge_path.string(), workspace_);
  if (!evidence) {
    cleanup();
    return Err(std::move(evidence).error());
  }

  BackupPairReport report;
  report.coordination_path = coordination_path.string();
  report.knowledge_path = knowledge_path.string();
  report.manifest_path = manifest_path.string();
  report.coordination_backup = *coordination_backup;
  report.knowledge_backup = *knowledge_backup;
  auto coordination_sha256 = atx::core::sha256_file(coordination_path.string());
  auto knowledge_sha256 = atx::core::sha256_file(knowledge_path.string());
  if (!coordination_sha256) {
    cleanup();
    return Err(std::move(coordination_sha256).error());
  }
  if (!knowledge_sha256) {
    cleanup();
    return Err(std::move(knowledge_sha256).error());
  }
  report.coordination_sha256 = *coordination_sha256;
  report.knowledge_sha256 = *knowledge_sha256;

  {
    auto coordination =
        Database::open(coordination_path.string(), atx::core::db::OpenMode::ReadOnly);
    if (!coordination) {
      cleanup();
      return Err(std::move(coordination).error());
    }
    auto metrics = coordination->prepare(
        "SELECT (SELECT coalesce(max(sequence),0) FROM agent_events WHERE workspace=?1),"
        "(SELECT count(*) FROM episodes WHERE workspace=?1)");
    if (!metrics) {
      cleanup();
      return Err(std::move(metrics).error());
    }
    if (!metrics->bind(1, workspace_)) {
      cleanup();
      return Err(ErrorCode::Internal, "cannot bind backup-pair workspace metrics");
    }
    auto step = metrics->step();
    if (!step || *step != Statement::Step::Row) {
      cleanup();
      return Err(ErrorCode::Internal, "cannot read backup-pair coordination metrics");
    }
    report.event_high_watermark = metrics->column_int(0);
    report.episode_count = metrics->column_int(1);
  }
  {
    auto knowledge = Database::open(knowledge_path.string(), atx::core::db::OpenMode::ReadOnly);
    if (!knowledge) {
      cleanup();
      return Err(std::move(knowledge).error());
    }
    auto metric = knowledge->prepare("SELECT coalesce(max(id),0) FROM source_observations");
    if (!metric) {
      cleanup();
      return Err(std::move(metric).error());
    }
    auto step = metric->step();
    if (!step || *step != Statement::Step::Row) {
      cleanup();
      return Err(ErrorCode::Internal, "cannot read backup-pair knowledge metrics");
    }
    report.knowledge_observation_high_watermark = metric->column_int(0);
  }

  auto manifest = [&]() -> Status {
    ATX_TRY(auto database, Database::open(manifest_partial.string()));
    ATX_TRY_VOID(database.pragma("journal_mode", "DELETE"));
    ATX_TRY_VOID(database.pragma("synchronous", "FULL"));
    ATX_TRY_VOID(database.exec(R"sql(
      CREATE TABLE backup_pair_manifest(
        id INTEGER PRIMARY KEY CHECK(id=1),
        format_version TEXT NOT NULL CHECK(format_version='atx-backup-pair-v1'),
        workspace TEXT NOT NULL,
        coordination_file TEXT NOT NULL,
        coordination_sha256 TEXT NOT NULL CHECK(length(coordination_sha256)=64),
        coordination_pages INTEGER NOT NULL CHECK(coordination_pages>=0),
        knowledge_file TEXT NOT NULL,
        knowledge_sha256 TEXT NOT NULL CHECK(length(knowledge_sha256)=64),
        knowledge_pages INTEGER NOT NULL CHECK(knowledge_pages>=0),
        event_high_watermark INTEGER NOT NULL CHECK(event_high_watermark>=0),
        episode_count INTEGER NOT NULL CHECK(episode_count>=0),
        knowledge_observation_high_watermark INTEGER NOT NULL
          CHECK(knowledge_observation_high_watermark>=0),
        created_at TEXT NOT NULL
      ) STRICT;
    )sql"));
    ATX_TRY(const auto created_at,
            scalar_text(database, "SELECT strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
    ATX_TRY(auto insert,
            database.prepare("INSERT INTO backup_pair_manifest VALUES(1,'atx-backup-pair-v1',"
                             "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"));
    ATX_TRY_VOID(insert.bind(1, workspace_));
    ATX_TRY_VOID(insert.bind(2, coordination_path.filename().string()));
    ATX_TRY_VOID(insert.bind(3, report.coordination_sha256));
    ATX_TRY_VOID(insert.bind(4, static_cast<i64>(report.coordination_backup.page_count)));
    ATX_TRY_VOID(insert.bind(5, knowledge_path.filename().string()));
    ATX_TRY_VOID(insert.bind(6, report.knowledge_sha256));
    ATX_TRY_VOID(insert.bind(7, static_cast<i64>(report.knowledge_backup.page_count)));
    ATX_TRY_VOID(insert.bind(8, report.event_high_watermark));
    ATX_TRY_VOID(insert.bind(9, report.episode_count));
    ATX_TRY_VOID(insert.bind(10, report.knowledge_observation_high_watermark));
    ATX_TRY_VOID(insert.bind(11, created_at));
    ATX_TRY_VOID(step_done(insert));
    ATX_TRY(auto integrity, database.prepare("PRAGMA integrity_check"));
    ATX_TRY(const auto step, integrity.step());
    if (step != Statement::Step::Row || integrity.column_text(0) != "ok") {
      return Err(ErrorCode::IoError, "backup-pair manifest integrity check failed");
    }
    return Ok();
  }();
  if (!manifest) {
    cleanup();
    return Err(std::move(manifest).error());
  }
  fs::create_hard_link(manifest_partial, manifest_path, filesystem_error);
  if (filesystem_error) {
    cleanup();
    return Err(ErrorCode::IoError, "cannot publish backup-pair manifest");
  }
  fs::remove(manifest_partial, filesystem_error);
  auto manifest_sha256 = atx::core::sha256_file(manifest_path.string());
  if (!manifest_sha256) {
    cleanup();
    return Err(std::move(manifest_sha256).error());
  }
  report.manifest_sha256 = *manifest_sha256;
  return Ok(std::move(report));
}

Result<BackupPairReport>
AgentDatabase::verify_backup_pair(std::string_view manifest_path,
                                  std::string_view expected_manifest_sha256) {
  if (!valid_field(manifest_path, kMaximumPayloadBytes, false)) {
    return Err(ErrorCode::InvalidArgument, "backup-pair manifest path is invalid");
  }
  namespace fs = std::filesystem;
  const fs::path manifest{std::string{manifest_path}};
  if (!expected_manifest_sha256.empty() && !valid_sha256(expected_manifest_sha256)) {
    return Err(ErrorCode::InvalidArgument, "expected manifest SHA-256 is invalid");
  }
  ATX_TRY(const auto manifest_sha256, atx::core::sha256_file(manifest.string()));
  if (!expected_manifest_sha256.empty() && manifest_sha256 != expected_manifest_sha256) {
    return Err(ErrorCode::IoError, "backup-pair manifest digest mismatch");
  }
  ATX_TRY(auto manifest_database,
          Database::open(manifest.string(), atx::core::db::OpenMode::ReadOnly));
  ATX_TRY(auto integrity, manifest_database.prepare("PRAGMA integrity_check"));
  ATX_TRY(const auto integrity_step, integrity.step());
  if (integrity_step != Statement::Step::Row || integrity.column_text(0) != "ok") {
    return Err(ErrorCode::IoError, "backup-pair manifest is corrupt");
  }
  ATX_TRY(auto query, manifest_database.prepare(
                          "SELECT format_version,workspace,coordination_file,coordination_sha256,"
                          "coordination_pages,knowledge_file,knowledge_sha256,knowledge_pages,"
                          "event_high_watermark,episode_count,knowledge_observation_high_watermark "
                          "FROM backup_pair_manifest WHERE id=1"));
  ATX_TRY(const auto step, query.step());
  if (step != Statement::Step::Row || query.column_text(0) != "atx-backup-pair-v1") {
    return Err(ErrorCode::InvalidArgument, "unsupported backup-pair manifest");
  }
  const std::string workspace{query.column_text(1)};
  const fs::path coordination_file{std::string{query.column_text(2)}};
  const std::string coordination_sha256{query.column_text(3)};
  const i64 coordination_pages = query.column_int(4);
  const fs::path knowledge_file{std::string{query.column_text(5)}};
  const std::string knowledge_sha256{query.column_text(6)};
  const i64 knowledge_pages = query.column_int(7);
  const i64 event_high_watermark = query.column_int(8);
  const i64 episode_count = query.column_int(9);
  const i64 knowledge_observation_high_watermark = query.column_int(10);
  if (!valid_field(workspace, kMaximumWorkspaceBytes, false) || coordination_file.empty() ||
      knowledge_file.empty() || coordination_file.is_absolute() || knowledge_file.is_absolute() ||
      coordination_file.filename() != coordination_file ||
      knowledge_file.filename() != knowledge_file || !valid_sha256(coordination_sha256) ||
      !valid_sha256(knowledge_sha256) || coordination_pages < 0 || knowledge_pages < 0 ||
      coordination_pages > std::numeric_limits<atx::i32>::max() ||
      knowledge_pages > std::numeric_limits<atx::i32>::max() || event_high_watermark < 0 ||
      episode_count < 0 || knowledge_observation_high_watermark < 0) {
    return Err(ErrorCode::InvalidArgument, "backup-pair manifest fields are invalid");
  }
  const fs::path directory = manifest.has_parent_path() ? manifest.parent_path() : fs::path{"."};
  const fs::path coordination_path = directory / coordination_file;
  const fs::path knowledge_path = directory / knowledge_file;
  ATX_TRY(const auto actual_coordination_sha256,
          atx::core::sha256_file(coordination_path.string()));
  ATX_TRY(const auto actual_knowledge_sha256, atx::core::sha256_file(knowledge_path.string()));
  if (actual_coordination_sha256 != coordination_sha256 ||
      actual_knowledge_sha256 != knowledge_sha256) {
    return Err(ErrorCode::IoError, "backup-pair file digest mismatch");
  }

  {
    ATX_TRY(auto coordination,
            Database::open(coordination_path.string(), atx::core::db::OpenMode::ReadOnly));
    ATX_TRY(auto metrics,
            coordination.prepare(
                "SELECT (SELECT coalesce(max(sequence),0) FROM agent_events WHERE workspace=?1),"
                "(SELECT count(*) FROM episodes WHERE workspace=?1)"));
    ATX_TRY_VOID(metrics.bind(1, workspace));
    ATX_TRY(const auto metrics_step, metrics.step());
    if (metrics_step != Statement::Step::Row || metrics.column_int(0) != event_high_watermark ||
        metrics.column_int(1) != episode_count) {
      return Err(ErrorCode::IoError, "backup-pair coordination watermark mismatch");
    }
  }
  {
    ATX_TRY(auto knowledge,
            Database::open(knowledge_path.string(), atx::core::db::OpenMode::ReadOnly));
    ATX_TRY(auto metric, knowledge.prepare("SELECT coalesce(max(id),0) FROM source_observations"));
    ATX_TRY(const auto metric_step, metric.step());
    if (metric_step != Statement::Step::Row ||
        metric.column_int(0) != knowledge_observation_high_watermark) {
      return Err(ErrorCode::IoError, "backup-pair knowledge watermark mismatch");
    }
  }

  ATX_TRY_VOID(
      verify_backup_pair_domains(coordination_path.string(), knowledge_path.string(), workspace));
  ATX_TRY(const auto stable_coordination_sha256,
          atx::core::sha256_file(coordination_path.string()));
  ATX_TRY(const auto stable_knowledge_sha256, atx::core::sha256_file(knowledge_path.string()));
  if (stable_coordination_sha256 != coordination_sha256 ||
      stable_knowledge_sha256 != knowledge_sha256) {
    return Err(ErrorCode::IoError, "backup-pair verification mutated a snapshotted file");
  }

  BackupPairReport report;
  report.coordination_path = coordination_path.string();
  report.knowledge_path = knowledge_path.string();
  report.manifest_path = manifest.string();
  report.coordination_sha256 = coordination_sha256;
  report.knowledge_sha256 = knowledge_sha256;
  report.manifest_sha256 = manifest_sha256;
  report.event_high_watermark = event_high_watermark;
  report.episode_count = episode_count;
  report.knowledge_observation_high_watermark = knowledge_observation_high_watermark;
  report.coordination_backup.page_count = static_cast<atx::i32>(coordination_pages);
  report.knowledge_backup.page_count = static_cast<atx::i32>(knowledge_pages);
  return Ok(std::move(report));
}

} // namespace atx::agent
