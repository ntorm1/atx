#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "atx/agent/database.hpp"
#include "atx/kb/knowledge_base.hpp"

namespace {

[[nodiscard]] std::string json_escape(std::string_view text) {
  std::string result;
  result.reserve(text.size() + 8);
  for (const char character : text) {
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) >= 0x20U) {
        result.push_back(character);
      }
    }
  }
  return result;
}

[[nodiscard]] bool parse_i64(std::string_view text, std::int64_t &result) {
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool value(int argc, char **argv, int &index, std::string &result) {
  if (index + 1 >= argc) {
    return false;
  }
  result = argv[++index];
  return true;
}

void usage() {
  std::cerr << "atx-db: durable multi-agent coordination database\n\n"
            << "  atx-db init <db> [--workspace W]\n"
            << "  atx-db run-create <db> <objective> [--workspace W] [--key K]\n"
            << "  atx-db agent-register <db> <run> <agent> <role> [--capabilities TEXT]\n"
            << "  atx-db task-add <db> <run> <title> [--description TEXT] [--priority N]\n"
            << "                  [--max-attempts N] [--depends TASK]... [--key K]\n"
            << "  atx-db task-claim <db> <agent> [--lease-seconds N]\n"
            << "  atx-db task-renew <db> <task> <agent> <token> [--lease-seconds N]\n"
            << "  atx-db task-complete <db> <task> <agent> <token> [--source ID]\n"
            << "  atx-db task-complete-verified <db> <knowledge-db> <task> <agent> <token>"
               " <source> <observation>\n"
            << "  atx-db task-fail <db> <task> <agent> <token> <error>\n"
            << "  atx-db tasks <db> [--run ID]\n"
            << "  atx-db events <db> [--after N] [--subject SUBJECT]\n"
            << "  atx-db consumer-register <db> <name> [--subject SUBJECT] [--start N]"
               " [--max-deliveries N] [--retry-backoff-seconds N]"
               " [--retry-backoff-max-seconds N] [--retry-jitter none|full]"
               " [--redrive-rate-per-second N] [--redrive-burst-events N]"
               " [--max-redrives N]\n"
            << "  atx-db consumer-status <db> <name>\n"
            << "  atx-db consumer-statuses <db>\n"
            << "  atx-db consumer-statuses-if-current <db> <event-high-watermark>"
               " <consumer-state-revision> [--next-transition T]\n"
            << "  atx-db consumer-poll <db> <name> [--limit N]\n"
            << "  atx-db consumer-receive <db> <name> <owner> <request-token>"
               " [--lease-seconds N] [--limit N]\n"
            << "  atx-db consumer-renew <db> <name> <owner> <delivery-token>"
               " [--lease-seconds N]\n"
            << "  atx-db consumer-reject <db> <name> <owner> <delivery-token>"
               " <rejection-token> <retry|dead-letter> <reason>\n"
            << "  atx-db consumer-settle <db> <name> <owner> <delivery-token>"
               " <checkpoint-token>\n"
            << "  atx-db consumer-dead-letters <db> <name> [--limit N]\n"
            << "  atx-db consumer-redrive <db> <name> <dead-letter-id> <token>\n"
            << "  atx-db consumer-quarantine <db> <name> <dead-letter-id> <operator>"
               " <token> <reason>\n"
            << "  atx-db consumer-checkpoint <db> <name> <revision> <through> <token>\n"
            << "  atx-db episode-record <db> <key> <run> <agent> <source> <observation>\n"
            << "  atx-db episode-record-verified <db> <knowledge-db> <key> <run> <agent>"
               " <source> <observation>\n"
            << "  atx-db fact-put <db> <subject> <predicate> <object> [--valid-from T] [--key K]\n"
            << "  atx-db fact-put-verified <db> <knowledge-db> <subject> <predicate> <object>"
               " <source> <observation> [--valid-from T] [--key K]\n"
            << "  atx-db facts-as-of <db> <valid-time> <transaction-time>\n"
            << "  atx-db backup <db> <new-backup-path> [--workspace W]\n"
            << "  atx-db backup-pair <db> <knowledge-db> <destination-prefix> [--workspace W]\n"
            << "  atx-db backup-pair-verify <manifest> [--sha256 HEX]\n"
            << "  atx-db verify <db> [--workspace W]\n"
            << "All commands accept --workspace W (default: default).\n";
}

template <class T> [[nodiscard]] int report_error(const T &result) {
  std::cerr << result.error().to_string() << '\n';
  return 1;
}

void print_task(const atx::agent::TaskRecord &task) {
  std::cout << "{\"id\":\"" << json_escape(task.id) << "\",\"run_id\":\""
            << json_escape(task.run_id) << "\",\"title\":\"" << json_escape(task.title)
            << "\",\"status\":\"" << task.status << "\",\"attempts\":" << task.attempts
            << ",\"revision\":" << task.revision << ",\"lease_owner\":\""
            << json_escape(task.lease_owner) << "\",\"lease_token\":\""
            << json_escape(task.lease_token) << "\",\"lease_expires_at\":\""
            << json_escape(task.lease_expires_at) << "\",\"result_source_id\":\""
            << json_escape(task.result_source_id)
            << "\",\"result_observation_id\":" << task.result_observation_id
            << ",\"result_evidence_status\":\"" << task.result_evidence_status << "\"}";
}

void print_event(const atx::agent::TaskEvent &event) {
  std::cout << "{\"sequence\":" << event.sequence << ",\"root_sequence\":" << event.root_sequence
            << ",\"redrive_count\":" << event.redrive_count << ",\"type\":\""
            << json_escape(event.type) << "\",\"run_id\":\"" << event.run_id << "\",\"task_id\":\""
            << event.task_id << "\",\"agent_id\":\"" << json_escape(event.agent_id)
            << "\",\"payload\":\"" << json_escape(event.payload) << "\",\"subject\":\""
            << json_escape(event.subject) << "\",\"created_at\":\"" << event.created_at << "\"}";
}

void print_consumer(const atx::agent::EventConsumerRecord &consumer) {
  std::cout << "{\"name\":\"" << json_escape(consumer.name) << "\",\"subject_filter\":\""
            << json_escape(consumer.subject_filter)
            << "\",\"max_delivery_attempts\":" << consumer.max_delivery_attempts
            << ",\"retry_backoff_seconds\":" << consumer.retry_backoff_seconds
            << ",\"retry_backoff_max_seconds\":" << consumer.retry_backoff_max_seconds
            << ",\"retry_jitter\":\"" << consumer.retry_jitter << "\""
            << ",\"redrive_rate_per_second\":" << consumer.redrive_rate_per_second
            << ",\"redrive_burst_events\":" << consumer.redrive_burst_events
            << ",\"redrive_token_millis\":" << consumer.redrive_token_millis
            << ",\"redrive_refilled_at\":\"" << consumer.redrive_refilled_at << "\""
            << ",\"max_redrive_count\":" << consumer.max_redrive_count
            << ",\"self_control_event_cutoff_sequence\":"
            << consumer.self_control_event_cutoff_sequence
            << ",\"start_sequence\":" << consumer.start_sequence
            << ",\"cursor_sequence\":" << consumer.cursor_sequence
            << ",\"revision\":" << consumer.revision << ",\"created_at\":\"" << consumer.created_at
            << "\",\"updated_at\":\"" << consumer.updated_at << "\"}";
}

void print_consumer_status(const atx::agent::EventConsumerStatus &status) {
  std::cout << "{\"consumer\":";
  print_consumer(status.consumer);
  std::cout << ",\"observed_at\":\"" << status.observed_at
            << "\",\"event_high_watermark\":" << status.event_high_watermark
            << ",\"consumer_state_revision\":" << status.consumer_state_revision
            << ",\"next_dynamic_transition_at\":\"" << status.next_dynamic_transition_at << "\""
            << ",\"pending_visible_event_count\":" << status.pending_visible_event_count
            << ",\"first_pending_visible_sequence\":" << status.first_pending_visible_sequence
            << ",\"last_pending_visible_sequence\":" << status.last_pending_visible_sequence
            << ",\"oldest_pending_visible_event_at\":\"" << status.oldest_pending_visible_event_at
            << "\",\"delivery_head_state\":\"" << status.delivery_head_state
            << "\",\"delivery_head_owner\":\"" << json_escape(status.delivery_head_owner)
            << "\",\"delivery_head_attempt\":" << status.delivery_head_attempt
            << ",\"delivery_head_through_sequence\":" << status.delivery_head_through_sequence
            << ",\"delivery_head_event_count\":" << status.delivery_head_event_count
            << ",\"delivery_head_expires_at\":\"" << status.delivery_head_expires_at
            << "\",\"delivery_head_retry_at\":\"" << status.delivery_head_retry_at
            << "\",\"queued_visible_event_count\":" << status.queued_visible_event_count
            << ",\"available_visible_event_count\":" << status.available_visible_event_count
            << ",\"retained_dead_letter_count\":" << status.retained_dead_letter_count
            << ",\"open_dead_letter_count\":" << status.open_dead_letter_count
            << ",\"open_dead_letter_event_count\":" << status.open_dead_letter_event_count
            << ",\"oldest_open_dead_letter_at\":\"" << status.oldest_open_dead_letter_at
            << "\",\"redriven_dead_letter_count\":" << status.redriven_dead_letter_count
            << ",\"quarantined_dead_letter_count\":" << status.quarantined_dead_letter_count << "}";
}

void print_consumer_fleet_status(const atx::agent::EventConsumerFleetStatus &fleet) {
  std::cout << "{\"workspace\":\"" << json_escape(fleet.workspace) << "\",\"observed_at\":\""
            << fleet.observed_at << "\",\"event_high_watermark\":" << fleet.event_high_watermark
            << ",\"consumer_state_revision\":" << fleet.consumer_state_revision
            << ",\"next_dynamic_transition_at\":\"" << fleet.next_dynamic_transition_at << "\""
            << ",\"consumer_count\":" << fleet.consumers.size() << ",\"consumers\":[";
  for (std::size_t index = 0; index < fleet.consumers.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    print_consumer_status(fleet.consumers[index]);
  }
  std::cout << "]}";
}

void print_consumer_fleet_cache_validation(
    const atx::agent::EventConsumerFleetCacheValidation &validation) {
  std::cout << "{\"cache_valid\":" << (validation.cache_valid ? "true" : "false")
            << ",\"validated_at\":\"" << validation.validated_at << "\",\"workspace\":\""
            << json_escape(validation.current.workspace)
            << "\",\"event_high_watermark\":" << validation.current.event_high_watermark
            << ",\"consumer_state_revision\":" << validation.current.consumer_state_revision
            << ",\"next_dynamic_transition_at\":\"" << validation.current.next_dynamic_transition_at
            << "\"";
  if (validation.snapshot.has_value()) {
    std::cout << ",\"snapshot\":";
    print_consumer_fleet_status(*validation.snapshot);
  }
  std::cout << '}';
}

void print_consumer_delivery(const atx::agent::EventConsumerDelivery &delivery) {
  std::cout << "{\"consumer\":";
  print_consumer(delivery.consumer);
  std::cout << ",\"delivery_token\":\"" << json_escape(delivery.delivery_token) << "\",\"owner\":\""
            << json_escape(delivery.owner) << "\",\"request_token\":\""
            << json_escape(delivery.request_token)
            << "\",\"previous_sequence\":" << delivery.previous_sequence
            << ",\"through_sequence\":" << delivery.through_sequence
            << ",\"attempt\":" << delivery.attempt << ",\"acquired_at\":\"" << delivery.acquired_at
            << "\",\"retry_delay_seconds\":" << delivery.retry_delay_seconds << ",\"expires_at\":\""
            << delivery.expires_at << "\",\"retry_not_before\":\"" << delivery.retry_not_before
            << "\",\"dead_lettered_batches\":" << delivery.dead_lettered_batches
            << ",\"dead_lettered_events\":" << delivery.dead_lettered_events << ",\"events\":[";
  for (std::size_t index = 0; index < delivery.events.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    print_event(delivery.events[index]);
  }
  std::cout << "]}";
}

void print_consumer_rejection(const atx::agent::EventConsumerRejection &rejection) {
  std::cout << "{\"consumer\":";
  print_consumer(rejection.consumer);
  std::cout << ",\"delivery_token\":\"" << json_escape(rejection.delivery_token)
            << "\",\"owner\":\"" << json_escape(rejection.owner) << "\",\"rejection_token\":\""
            << json_escape(rejection.rejection_token) << "\",\"reason\":\""
            << json_escape(rejection.reason) << "\",\"attempt\":" << rejection.attempt
            << ",\"retry_delay_seconds\":" << rejection.retry_delay_seconds << ",\"rejected_at\":\""
            << rejection.rejected_at << "\",\"retry_not_before\":\"" << rejection.retry_not_before
            << "\",\"dead_lettered\":" << (rejection.dead_lettered ? "true" : "false")
            << ",\"dead_letter_id\":" << rejection.dead_letter_id << ",\"disposition\":\""
            << rejection.disposition << "\"}";
}

void print_consumer_dead_letter(const atx::agent::EventConsumerDeadLetter &dead_letter) {
  std::cout << "{\"id\":" << dead_letter.id << ",\"consumer_name\":\""
            << json_escape(dead_letter.consumer_name) << "\",\"delivery_token\":\""
            << json_escape(dead_letter.delivery_token)
            << "\",\"previous_sequence\":" << dead_letter.previous_sequence
            << ",\"through_sequence\":" << dead_letter.through_sequence
            << ",\"delivery_attempts\":" << dead_letter.delivery_attempts << ",\"reason\":\""
            << json_escape(dead_letter.reason) << "\",\"rejection_disposition\":\""
            << json_escape(dead_letter.rejection_disposition) << "\",\"rejection_reason\":\""
            << json_escape(dead_letter.rejection_reason) << "\",\"status\":\""
            << json_escape(dead_letter.status) << "\",\"redrive_token\":\""
            << json_escape(dead_letter.redrive_token)
            << "\",\"redrive_budget_event_count\":" << dead_letter.redrive_budget_event_count
            << ",\"redrive_budget_before_millis\":" << dead_letter.redrive_budget_before_millis
            << ",\"redrive_budget_after_millis\":" << dead_letter.redrive_budget_after_millis
            << ",\"redrive_budget_refilled_at\":\"" << dead_letter.redrive_budget_refilled_at
            << "\",\"quarantine_token\":\"" << json_escape(dead_letter.quarantine_token)
            << "\",\"quarantined_by\":\"" << json_escape(dead_letter.quarantined_by)
            << "\",\"quarantine_reason\":\"" << json_escape(dead_letter.quarantine_reason)
            << "\",\"quarantined_at\":\"" << dead_letter.quarantined_at
            << "\",\"dead_lettered_event_sequence\":" << dead_letter.dead_lettered_event_sequence
            << ",\"redriven_event_sequence\":" << dead_letter.redriven_event_sequence
            << ",\"quarantined_event_sequence\":" << dead_letter.quarantined_event_sequence
            << ",\"created_at\":\"" << dead_letter.created_at << "\",\"redriven_at\":\""
            << dead_letter.redriven_at << "\",\"events\":[";
  for (std::size_t index = 0; index < dead_letter.events.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    print_event(dead_letter.events[index]);
  }
  std::cout << "],\"redriven_events\":[";
  for (std::size_t index = 0; index < dead_letter.redriven_events.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    print_event(dead_letter.redriven_events[index]);
  }
  std::cout << "]}";
}

void print_backup_pair(const atx::agent::BackupPairReport &report) {
  std::cout << "{\"ok\":true,\"manifest_path\":\"" << json_escape(report.manifest_path)
            << "\",\"coordination_path\":\"" << json_escape(report.coordination_path)
            << "\",\"knowledge_path\":\"" << json_escape(report.knowledge_path)
            << "\",\"coordination_sha256\":\"" << report.coordination_sha256
            << "\",\"knowledge_sha256\":\"" << report.knowledge_sha256
            << "\",\"manifest_sha256\":\"" << report.manifest_sha256
            << "\",\"event_high_watermark\":" << report.event_high_watermark
            << ",\"episode_count\":" << report.episode_count
            << ",\"knowledge_observation_high_watermark\":"
            << report.knowledge_observation_high_watermark << "}\n";
}

[[nodiscard]] bool parse_workspace(int argc, char **argv, int option_start,
                                   std::string &workspace) {
  workspace = "default";
  bool found = false;
  for (int index = option_start; index < argc; ++index) {
    if (std::string_view{argv[index]} == "--workspace") {
      if (found || index + 1 >= argc || std::string_view{argv[index + 1]}.starts_with("--")) {
        return false;
      }
      found = true;
      workspace = argv[++index];
    }
  }
  return true;
}

[[nodiscard]] bool validate_options(int argc, char **argv, int option_start,
                                    std::initializer_list<std::string_view> allowed,
                                    std::string_view repeatable = {}) {
  std::vector<std::string_view> seen;
  int index = option_start;
  while (index < argc) {
    const std::string_view option{argv[index]};
    bool recognized = false;
    for (const auto candidate : allowed) {
      recognized = recognized || option == candidate;
    }
    if (!recognized || index + 1 >= argc || std::string_view{argv[index + 1]}.starts_with("--")) {
      return false;
    }
    if (option != repeatable) {
      for (const auto previous : seen) {
        if (previous == option) {
          return false;
        }
      }
      seen.push_back(option);
    }
    index += 2;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string_view command{argv[1]};
  int required_arguments{};
  int option_start{};
  if (command == "init" || command == "verify" || command == "tasks" || command == "events" ||
      command == "consumer-statuses") {
    required_arguments = 3;
    option_start = 3;
  } else if (command == "run-create" || command == "task-claim" || command == "backup" ||
             command == "consumer-register" || command == "consumer-status" ||
             command == "consumer-poll" || command == "consumer-dead-letters") {
    required_arguments = 4;
    option_start = 4;
  } else if (command == "backup-pair") {
    required_arguments = 5;
    option_start = 5;
  } else if (command == "backup-pair-verify") {
    required_arguments = 3;
    option_start = 3;
  } else if (command == "task-add" || command == "facts-as-of") {
    required_arguments = 5;
    option_start = 5;
  } else if (command == "consumer-statuses-if-current") {
    required_arguments = 5;
    option_start = 5;
  } else if (command == "agent-register" || command == "task-renew" || command == "task-complete" ||
             command == "fact-put" || command == "consumer-receive" ||
             command == "consumer-renew" || command == "consumer-redrive") {
    required_arguments = 6;
    option_start = 6;
  } else if (command == "task-fail" || command == "consumer-checkpoint" ||
             command == "consumer-settle") {
    required_arguments = 7;
    option_start = 7;
  } else if (command == "episode-record" || command == "consumer-quarantine") {
    required_arguments = 8;
    option_start = 8;
  } else if (command == "episode-record-verified" || command == "consumer-reject") {
    required_arguments = 9;
    option_start = 9;
  } else if (command == "task-complete-verified" || command == "fact-put-verified") {
    required_arguments = 9;
    option_start = 9;
  } else {
    usage();
    return 2;
  }
  if (argc < required_arguments) {
    usage();
    return 2;
  }
  bool options_valid = false;
  if (command == "backup-pair-verify") {
    options_valid = validate_options(argc, argv, option_start, {"--sha256"});
  } else if (command == "consumer-statuses-if-current") {
    options_valid =
        validate_options(argc, argv, option_start, {"--next-transition", "--workspace"});
  } else if (command == "init" || command == "verify" || command == "backup" ||
             command == "backup-pair" || command == "task-fail" || command == "episode-record" ||
             command == "episode-record-verified" || command == "task-complete-verified" ||
             command == "consumer-checkpoint" || command == "consumer-settle" ||
             command == "consumer-redrive" || command == "consumer-reject" ||
             command == "consumer-quarantine" || command == "consumer-status" ||
             command == "consumer-statuses" || command == "facts-as-of") {
    options_valid = validate_options(argc, argv, option_start, {"--workspace"});
  } else if (command == "fact-put-verified") {
    options_valid =
        validate_options(argc, argv, option_start, {"--key", "--valid-from", "--workspace"});
  } else if (command == "run-create") {
    options_valid = validate_options(argc, argv, option_start, {"--key", "--workspace"});
  } else if (command == "agent-register") {
    options_valid = validate_options(argc, argv, option_start, {"--capabilities", "--workspace"});
  } else if (command == "task-add") {
    options_valid = validate_options(
        argc, argv, option_start,
        {"--description", "--depends", "--key", "--priority", "--max-attempts", "--workspace"},
        "--depends");
  } else if (command == "task-claim" || command == "task-renew") {
    options_valid = validate_options(argc, argv, option_start, {"--lease-seconds", "--workspace"});
  } else if (command == "task-complete") {
    options_valid = validate_options(argc, argv, option_start, {"--source", "--workspace"});
  } else if (command == "tasks") {
    options_valid = validate_options(argc, argv, option_start, {"--run", "--workspace"});
  } else if (command == "events") {
    options_valid =
        validate_options(argc, argv, option_start, {"--after", "--subject", "--workspace"});
  } else if (command == "consumer-register") {
    options_valid = validate_options(argc, argv, option_start,
                                     {"--start", "--subject", "--max-deliveries",
                                      "--retry-backoff-seconds", "--retry-backoff-max-seconds",
                                      "--retry-jitter", "--redrive-rate-per-second",
                                      "--redrive-burst-events", "--max-redrives", "--workspace"});
  } else if (command == "consumer-poll") {
    options_valid = validate_options(argc, argv, option_start, {"--limit", "--workspace"});
  } else if (command == "consumer-dead-letters") {
    options_valid = validate_options(argc, argv, option_start, {"--limit", "--workspace"});
  } else if (command == "consumer-receive") {
    options_valid =
        validate_options(argc, argv, option_start, {"--lease-seconds", "--limit", "--workspace"});
  } else if (command == "consumer-renew") {
    options_valid = validate_options(argc, argv, option_start, {"--lease-seconds", "--workspace"});
  } else if (command == "fact-put") {
    options_valid =
        validate_options(argc, argv, option_start, {"--key", "--valid-from", "--workspace"});
  }
  if (!options_valid) {
    std::cerr << "invalid, missing, or repeated option\n";
    usage();
    return 2;
  }
  std::string workspace;
  if (!parse_workspace(argc, argv, option_start, workspace)) {
    std::cerr << "--workspace requires one non-option value and may appear only once\n";
    return 2;
  }
  if (command == "backup-pair-verify") {
    std::string expected_sha256;
    for (int index = option_start; index < argc; ++index) {
      if (std::string_view{argv[index]} == "--sha256" &&
          !value(argc, argv, index, expected_sha256)) {
        return 2;
      }
    }
    auto verified = atx::agent::AgentDatabase::verify_backup_pair(argv[2], expected_sha256);
    if (!verified) {
      return report_error(verified);
    }
    print_backup_pair(*verified);
    return 0;
  }
  auto opened = atx::agent::AgentDatabase::open(argv[2], workspace);
  if (!opened) {
    return report_error(opened);
  }
  if (command == "init") {
    std::cout << "{\"ok\":true,\"workspace\":\"" << json_escape(opened->workspace()) << "\"}\n";
    return 0;
  }
  if (command == "verify") {
    auto status = opened->verify_integrity();
    if (!status) {
      return report_error(status);
    }
    std::cout << "{\"ok\":true}\n";
    return 0;
  }
  if (command == "backup") {
    auto backup = opened->backup_to(argv[3]);
    if (!backup) {
      return report_error(backup);
    }
    std::cout << "{\"ok\":true,\"path\":\"" << json_escape(argv[3])
              << "\",\"pages\":" << backup->page_count << ",\"steps\":" << backup->steps
              << ",\"busy_retries\":" << backup->busy_retries << "}\n";
    return 0;
  }
  if (command == "backup-pair") {
    auto knowledge = atx::kb::KnowledgeBase::open(argv[3]);
    if (!knowledge) {
      return report_error(knowledge);
    }
    auto backup = opened->backup_pair(*knowledge, argv[4]);
    if (!backup) {
      return report_error(backup);
    }
    print_backup_pair(*backup);
    return 0;
  }
  if (command == "run-create") {
    if (argc < 4) {
      return 2;
    }
    std::string key;
    for (int index = 4; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--key" && !value(argc, argv, index, key)) {
        return 2;
      } else if (option == "--workspace") {
        ++index;
      } else if (option != "--key") {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto run = opened->create_run(argv[3], key);
    if (!run) {
      return report_error(run);
    }
    std::cout << "{\"id\":\"" << run->id << "\",\"status\":\"" << run->status
              << "\",\"revision\":" << run->revision << "}\n";
    return 0;
  }
  if (command == "agent-register") {
    if (argc < 6) {
      return 2;
    }
    std::string capabilities;
    for (int index = 6; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--capabilities" && !value(argc, argv, index, capabilities)) {
        return 2;
      } else if (option == "--workspace") {
        ++index;
      } else if (option != "--capabilities") {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto agent = opened->register_agent(argv[3], argv[4], argv[5], capabilities);
    if (!agent) {
      return report_error(agent);
    }
    std::cout << "{\"id\":\"" << json_escape(agent->id) << "\",\"run_id\":\"" << agent->run_id
              << "\",\"revision\":" << agent->revision << "}\n";
    return 0;
  }
  if (command == "task-add") {
    if (argc < 5) {
      return 2;
    }
    atx::agent::TaskSpec spec;
    spec.run_id = argv[3];
    spec.title = argv[4];
    for (int index = 5; index < argc; ++index) {
      const std::string_view option{argv[index]};
      std::string parsed;
      if (option == "--description" && value(argc, argv, index, spec.description)) {
      } else if (option == "--depends" && value(argc, argv, index, parsed)) {
        spec.dependencies.push_back(std::move(parsed));
      } else if (option == "--key" && value(argc, argv, index, spec.idempotency_key)) {
      } else if (option == "--priority" && value(argc, argv, index, parsed) &&
                 parse_i64(parsed, spec.priority)) {
      } else if (option == "--max-attempts" && value(argc, argv, index, parsed) &&
                 parse_i64(parsed, spec.max_attempts)) {
      } else if (option == "--workspace") {
        ++index;
      } else {
        std::cerr << "invalid task option: " << option << '\n';
        return 2;
      }
    }
    auto task = opened->add_task(spec);
    if (!task) {
      return report_error(task);
    }
    print_task(*task);
    std::cout << '\n';
    return 0;
  }
  if (command == "task-claim") {
    if (argc < 4) {
      return 2;
    }
    std::int64_t lease = 300;
    for (int index = 4; index < argc; ++index) {
      if (std::string_view{argv[index]} == "--lease-seconds") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, lease)) {
          return 2;
        }
      } else if (std::string_view{argv[index]} == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << argv[index] << '\n';
        return 2;
      }
    }
    auto task = opened->claim_next(argv[3], lease);
    if (!task) {
      return report_error(task);
    }
    print_task(*task);
    std::cout << '\n';
    return 0;
  }
  if (command == "task-renew") {
    if (argc < 6) {
      return 2;
    }
    std::int64_t lease = 300;
    for (int index = 6; index < argc; ++index) {
      if (std::string_view{argv[index]} == "--lease-seconds") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, lease)) {
          return 2;
        }
      } else if (std::string_view{argv[index]} == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << argv[index] << '\n';
        return 2;
      }
    }
    auto task = opened->renew_lease(argv[3], argv[4], argv[5], lease);
    if (!task) {
      return report_error(task);
    }
    print_task(*task);
    std::cout << '\n';
    return 0;
  }
  if (command == "task-complete") {
    if (argc < 6) {
      return 2;
    }
    std::string source;
    for (int index = 6; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--source" && !value(argc, argv, index, source)) {
        return 2;
      } else if (option == "--workspace") {
        ++index;
      } else if (option != "--source") {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto status = opened->complete_task(argv[3], argv[4], argv[5], source);
    if (!status) {
      return report_error(status);
    }
    std::cout << "{\"ok\":true}\n";
    return 0;
  }
  if (command == "task-complete-verified") {
    std::int64_t observation_id{};
    if (!parse_i64(argv[8], observation_id)) {
      return 2;
    }
    auto knowledge = atx::kb::KnowledgeBase::open(argv[3]);
    if (!knowledge) {
      return report_error(knowledge);
    }
    auto status = opened->complete_task_verified(argv[4], argv[5], argv[6], argv[7], observation_id,
                                                 *knowledge);
    if (!status) {
      return report_error(status);
    }
    std::cout << "{\"ok\":true,\"evidence_status\":\"verified\"}\n";
    return 0;
  }
  if (command == "task-fail") {
    if (argc < 7) {
      return 2;
    }
    auto status = opened->fail_task(argv[3], argv[4], argv[5], argv[6]);
    if (!status) {
      return report_error(status);
    }
    std::cout << "{\"ok\":true}\n";
    return 0;
  }
  if (command == "tasks") {
    std::string run_id;
    for (int index = 3; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--run" && !value(argc, argv, index, run_id)) {
        return 2;
      } else if (option == "--workspace") {
        ++index;
      } else if (option != "--run") {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto tasks = opened->list_tasks(run_id);
    if (!tasks) {
      return report_error(tasks);
    }
    std::cout << '[';
    for (std::size_t index = 0; index < tasks->size(); ++index) {
      if (index != 0) {
        std::cout << ',';
      }
      print_task((*tasks)[index]);
    }
    std::cout << "]\n";
    return 0;
  }
  if (command == "events") {
    std::int64_t after{};
    std::string subject;
    for (int index = 3; index < argc; ++index) {
      if (std::string_view{argv[index]} == "--after") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, after)) {
          return 2;
        }
      } else if (std::string_view{argv[index]} == "--subject") {
        if (!value(argc, argv, index, subject)) {
          return 2;
        }
      } else if (std::string_view{argv[index]} == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << argv[index] << '\n';
        return 2;
      }
    }
    auto events = opened->events_after(after, 100, subject);
    if (!events) {
      return report_error(events);
    }
    std::cout << '[';
    for (std::size_t index = 0; index < events->size(); ++index) {
      const auto &event = (*events)[index];
      if (index != 0) {
        std::cout << ',';
      }
      print_event(event);
    }
    std::cout << "]\n";
    return 0;
  }
  if (command == "consumer-register") {
    std::string subject;
    std::int64_t start_sequence{};
    std::int64_t max_delivery_attempts{};
    std::int64_t retry_backoff_seconds{};
    std::int64_t retry_backoff_max_seconds{};
    std::int64_t redrive_rate_per_second{};
    std::int64_t redrive_burst_events{};
    std::int64_t max_redrive_count{};
    auto retry_jitter = atx::agent::EventConsumerRetryJitter::None;
    for (int index = 4; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--subject") {
        if (!value(argc, argv, index, subject)) {
          return 2;
        }
      } else if (option == "--start") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, start_sequence)) {
          return 2;
        }
      } else if (option == "--max-deliveries") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, max_delivery_attempts)) {
          return 2;
        }
      } else if (option == "--retry-backoff-seconds") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, retry_backoff_seconds)) {
          return 2;
        }
      } else if (option == "--retry-backoff-max-seconds") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, retry_backoff_max_seconds)) {
          return 2;
        }
      } else if (option == "--retry-jitter") {
        std::string parsed;
        if (!value(argc, argv, index, parsed)) {
          return 2;
        }
        if (parsed == "none") {
          retry_jitter = atx::agent::EventConsumerRetryJitter::None;
        } else if (parsed == "full") {
          retry_jitter = atx::agent::EventConsumerRetryJitter::Full;
        } else {
          std::cerr << "consumer retry jitter must be none or full\n";
          return 2;
        }
      } else if (option == "--redrive-rate-per-second") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, redrive_rate_per_second)) {
          return 2;
        }
      } else if (option == "--redrive-burst-events") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, redrive_burst_events)) {
          return 2;
        }
      } else if (option == "--max-redrives") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, max_redrive_count)) {
          return 2;
        }
      } else if (option == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto consumer = opened->register_event_consumer(
        argv[3], subject, start_sequence, max_delivery_attempts, retry_backoff_seconds,
        retry_backoff_max_seconds, retry_jitter, redrive_rate_per_second, redrive_burst_events,
        max_redrive_count);
    if (!consumer) {
      return report_error(consumer);
    }
    print_consumer(*consumer);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-poll") {
    std::int64_t parsed_limit = 100;
    for (int index = 4; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--limit") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, parsed_limit) ||
            parsed_limit < 1 || parsed_limit > 1'000) {
          return 2;
        }
      } else if (option == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto batch = opened->poll_event_consumer(argv[3], static_cast<std::size_t>(parsed_limit));
    if (!batch) {
      return report_error(batch);
    }
    std::cout << "{\"consumer\":";
    print_consumer(batch->consumer);
    std::cout << ",\"events\":[";
    for (std::size_t index = 0; index < batch->events.size(); ++index) {
      if (index != 0) {
        std::cout << ',';
      }
      print_event(batch->events[index]);
    }
    std::cout << "]}\n";
    return 0;
  }
  if (command == "consumer-status") {
    auto status = opened->get_event_consumer_status(argv[3]);
    if (!status) {
      return report_error(status);
    }
    print_consumer_status(*status);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-statuses") {
    auto fleet = opened->list_event_consumer_statuses();
    if (!fleet) {
      return report_error(fleet);
    }
    print_consumer_fleet_status(*fleet);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-statuses-if-current") {
    atx::agent::EventConsumerFleetValidator cached;
    cached.workspace = opened->workspace();
    if (!parse_i64(argv[3], cached.event_high_watermark) ||
        !parse_i64(argv[4], cached.consumer_state_revision)) {
      return 2;
    }
    for (int index = 5; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--next-transition") {
        if (!value(argc, argv, index, cached.next_dynamic_transition_at)) {
          return 2;
        }
      } else if (option == "--workspace") {
        ++index;
      }
    }
    auto validation = opened->list_event_consumer_statuses_if_current(cached);
    if (!validation) {
      return report_error(validation);
    }
    print_consumer_fleet_cache_validation(*validation);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-receive") {
    std::int64_t lease_seconds = 30;
    std::int64_t parsed_limit = 100;
    for (int index = 6; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--lease-seconds") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, lease_seconds)) {
          return 2;
        }
      } else if (option == "--limit") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, parsed_limit) ||
            parsed_limit < 1 || parsed_limit > 1'000) {
          return 2;
        }
      } else if (option == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto delivery = opened->receive_event_consumer(argv[3], argv[4], argv[5], lease_seconds,
                                                   static_cast<std::size_t>(parsed_limit));
    if (!delivery) {
      return report_error(delivery);
    }
    print_consumer_delivery(*delivery);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-renew") {
    std::int64_t lease_seconds = 30;
    for (int index = 6; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--lease-seconds") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, lease_seconds)) {
          return 2;
        }
      } else if (option == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto delivery = opened->renew_event_consumer_delivery(argv[3], argv[4], argv[5], lease_seconds);
    if (!delivery) {
      return report_error(delivery);
    }
    print_consumer_delivery(*delivery);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-reject") {
    atx::agent::EventConsumerRejectionDisposition disposition;
    if (std::string_view{argv[7]} == "retry") {
      disposition = atx::agent::EventConsumerRejectionDisposition::Retry;
    } else if (std::string_view{argv[7]} == "dead-letter") {
      disposition = atx::agent::EventConsumerRejectionDisposition::DeadLetter;
    } else {
      std::cerr << "consumer rejection disposition must be retry or dead-letter\n";
      return 2;
    }
    auto rejection = opened->reject_event_consumer_delivery(argv[3], argv[4], argv[5], argv[6],
                                                            argv[8], disposition);
    if (!rejection) {
      return report_error(rejection);
    }
    print_consumer_rejection(*rejection);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-settle") {
    auto consumer = opened->settle_event_consumer_delivery(argv[3], argv[4], argv[5], argv[6]);
    if (!consumer) {
      return report_error(consumer);
    }
    print_consumer(*consumer);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-dead-letters") {
    std::int64_t parsed_limit = 100;
    for (int index = 4; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--limit") {
        std::string parsed;
        if (!value(argc, argv, index, parsed) || !parse_i64(parsed, parsed_limit) ||
            parsed_limit < 1 || parsed_limit > 1'000) {
          return 2;
        }
      } else if (option == "--workspace") {
        ++index;
      } else {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto dead_letters =
        opened->list_event_consumer_dead_letters(argv[3], static_cast<std::size_t>(parsed_limit));
    if (!dead_letters) {
      return report_error(dead_letters);
    }
    std::cout << '[';
    for (std::size_t index = 0; index < dead_letters->size(); ++index) {
      if (index != 0) {
        std::cout << ',';
      }
      print_consumer_dead_letter(dead_letters->at(index));
    }
    std::cout << "]\n";
    return 0;
  }
  if (command == "consumer-redrive") {
    std::int64_t dead_letter_id{};
    if (!parse_i64(argv[4], dead_letter_id)) {
      return 2;
    }
    auto dead_letter = opened->redrive_event_consumer_dead_letter(argv[3], dead_letter_id, argv[5]);
    if (!dead_letter) {
      return report_error(dead_letter);
    }
    print_consumer_dead_letter(*dead_letter);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-quarantine") {
    std::int64_t dead_letter_id{};
    if (!parse_i64(argv[4], dead_letter_id)) {
      return 2;
    }
    auto dead_letter = opened->quarantine_event_consumer_dead_letter(argv[3], dead_letter_id,
                                                                     argv[5], argv[6], argv[7]);
    if (!dead_letter) {
      return report_error(dead_letter);
    }
    print_consumer_dead_letter(*dead_letter);
    std::cout << '\n';
    return 0;
  }
  if (command == "consumer-checkpoint") {
    std::int64_t revision{};
    std::int64_t through_sequence{};
    if (!parse_i64(argv[4], revision) || !parse_i64(argv[5], through_sequence)) {
      return 2;
    }
    auto consumer = opened->checkpoint_event_consumer(argv[3], revision, through_sequence, argv[6]);
    if (!consumer) {
      return report_error(consumer);
    }
    print_consumer(*consumer);
    std::cout << '\n';
    return 0;
  }
  if (command == "episode-record") {
    if (argc < 8) {
      return 2;
    }
    atx::agent::EpisodeInput episode;
    episode.idempotency_key = argv[3];
    episode.run_id = argv[4];
    episode.agent_id = argv[5];
    episode.source_id = argv[6];
    if (!parse_i64(argv[7], episode.observation_id)) {
      return 2;
    }
    auto recorded = opened->record_episode(episode);
    if (!recorded) {
      return report_error(recorded);
    }
    std::cout << "{\"id\":" << recorded->id << ",\"source_id\":\""
              << json_escape(recorded->source_id)
              << "\",\"observation_id\":" << recorded->observation_id << ",\"evidence_status\":\""
              << recorded->evidence_status << "\"}\n";
    return 0;
  }
  if (command == "episode-record-verified") {
    atx::agent::EpisodeInput episode;
    episode.idempotency_key = argv[4];
    episode.run_id = argv[5];
    episode.agent_id = argv[6];
    episode.source_id = argv[7];
    if (!parse_i64(argv[8], episode.observation_id)) {
      return 2;
    }
    auto knowledge = atx::kb::KnowledgeBase::open(argv[3]);
    if (!knowledge) {
      return report_error(knowledge);
    }
    auto recorded = opened->record_verified_episode(episode, *knowledge);
    if (!recorded) {
      return report_error(recorded);
    }
    std::cout << "{\"id\":" << recorded->id << ",\"source_id\":\""
              << json_escape(recorded->source_id)
              << "\",\"observation_id\":" << recorded->observation_id << ",\"evidence_status\":\""
              << recorded->evidence_status << "\",\"content_hash\":\""
              << recorded->evidence_content_hash << "\"}\n";
    return 0;
  }
  if (command == "fact-put") {
    if (argc < 6) {
      return 2;
    }
    atx::agent::FactInput fact;
    fact.subject = argv[3];
    fact.predicate = argv[4];
    fact.object = argv[5];
    for (int index = 6; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--valid-from" && !value(argc, argv, index, fact.valid_from)) {
        return 2;
      } else if (option == "--key" && !value(argc, argv, index, fact.idempotency_key)) {
        return 2;
      } else if (option == "--workspace") {
        ++index;
      } else if (option != "--valid-from" && option != "--key") {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto recorded = opened->put_fact(fact);
    if (!recorded) {
      return report_error(recorded);
    }
    std::cout << "{\"id\":" << recorded->id << ",\"transaction_from\":\""
              << recorded->transaction_from << "\"}\n";
    return 0;
  }
  if (command == "fact-put-verified") {
    atx::agent::FactInput fact;
    fact.subject = argv[4];
    fact.predicate = argv[5];
    fact.object = argv[6];
    fact.evidence_source_id = argv[7];
    std::int64_t observation_id{};
    if (!parse_i64(argv[8], observation_id)) {
      return 2;
    }
    for (int index = 9; index < argc; ++index) {
      const std::string_view option{argv[index]};
      if (option == "--valid-from" && !value(argc, argv, index, fact.valid_from)) {
        return 2;
      } else if (option == "--key" && !value(argc, argv, index, fact.idempotency_key)) {
        return 2;
      } else if (option == "--workspace") {
        ++index;
      } else if (option != "--valid-from" && option != "--key") {
        std::cerr << "unknown option: " << option << '\n';
        return 2;
      }
    }
    auto knowledge = atx::kb::KnowledgeBase::open(argv[3]);
    if (!knowledge) {
      return report_error(knowledge);
    }
    auto recorded = opened->put_verified_fact(fact, observation_id, *knowledge);
    if (!recorded) {
      return report_error(recorded);
    }
    std::cout << "{\"id\":" << recorded->id << ",\"transaction_from\":\""
              << recorded->transaction_from << "\",\"evidence_status\":\""
              << recorded->evidence_status << "\",\"content_hash\":\""
              << recorded->evidence_content_hash << "\"}\n";
    return 0;
  }
  if (command == "facts-as-of") {
    if (argc < 5) {
      return 2;
    }
    auto facts = opened->facts_as_of(argv[3], argv[4]);
    if (!facts) {
      return report_error(facts);
    }
    std::cout << '[';
    for (std::size_t index = 0; index < facts->size(); ++index) {
      if (index != 0) {
        std::cout << ',';
      }
      const auto &fact = (*facts)[index];
      std::cout << "{\"id\":" << fact.id << ",\"subject\":\"" << json_escape(fact.subject)
                << "\",\"predicate\":\"" << json_escape(fact.predicate) << "\",\"object\":\""
                << json_escape(fact.object) << "\"}";
    }
    std::cout << "]\n";
    return 0;
  }
  usage();
  return 2;
}
