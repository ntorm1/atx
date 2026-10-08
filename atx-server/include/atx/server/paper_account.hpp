#pragma once

#include <atx/server/core.hpp>
#include <atx/server/paper.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace atx::server {
// Historical modeled accounts only. A TRI unit is not an exchange share or broker position.
struct PaperAccountInput {
    std::string name, description, review_id, evidence_job_id;
    std::string start_date, end_date, idempotency_key;
    std::string actor_id{"local"};
};
struct PaperAdvanceInput {
    std::string id;
    std::uint64_t expected_revision{};
    std::uint32_t sessions{};
    std::string idempotency_key;
    std::string actor_id{"local"};
};
struct PaperIntegrityPolicy {
    std::string runtime_build_sha256;
    bool require_independent_review{};
};
using PaperReplay = std::function<PaperReplayResult(const PaperReplayInput&,
    std::stop_token, const Progress&)>;
// Rechecks the admitting operator, not the account creator. Never runs under store/worker locks.
using PaperAuthorize = std::function<bool(std::string_view actor_id, std::int64_t now_ms)>;
struct PaperExecutionOptions {
    std::string runtime_build_sha256;
    bool require_independent_review{};
    PaperReplay replay;
    PaperAuthorize authorize;
};
struct PaperAccount {
    std::string id, name, description, actor_id, review_id;
    std::uint64_t review_revision{};
    std::string alpha_id;
    std::uint64_t alpha_revision{};
    std::string evidence_job_id, evidence_result_sha256, request_sha256;
    std::string dataset_id, dataset_sha256, runtime_build_sha256;
    std::string evidence_end_date, start_date, end_date;
    std::uint64_t revision{};
    std::string state;
    std::uint32_t through_session_count{}, total_sessions{};
    std::string last_session_date, chain_sha256, active_job_id, last_job_id;
    std::int64_t created_at_ms{}, updated_at_ms{};
    double initial_cash{}, last_cash{}, last_nav{};
    std::vector<std::string> warnings;
};
struct PaperAdmission { PaperAccount account; Job job; };
struct PaperAccountPage { std::vector<PaperAccount> accounts; std::uint64_t total{}; };
// Sequence expresses append/commit order. Market session indices can repeat across advances:
// valuation at T precedes executions at T, which become owned only on the following interval.
struct PaperLedgerEntry {
    std::uint64_t sequence{}, commit_revision{};
    std::string kind;
    std::uint32_t session_index{};
    std::string payload_json, job_id, previous_sha256, entry_sha256, chain_sha256;
};
struct PaperLedgerPage { std::vector<PaperLedgerEntry> entries; std::uint64_t total{}; };
struct PaperAccountDetails {
    std::vector<std::string> window_calendar;
    std::vector<std::uint64_t> cohort_security_ids;
    std::string latest_receipt_json;
};
// Internal worker context, obtained only through a durable paper-operation/job/account join.
// The previously committed full ledger prefix remains private to Store.
struct PaperTask {
    PaperReplayInput input;
    std::string account_id, admitting_actor_id, expected_prefix_sha256;
    std::uint64_t expected_revision{};
};
} // namespace atx::server
