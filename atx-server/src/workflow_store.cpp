#include "workflow_store.hpp"
#include "experiment_jobs.hpp"
#include "governance_store.hpp"
#include "library_validation.hpp"
#include "workspace_validation.hpp"
#include <atx/core/sha256.hpp>

#include <limits>

namespace atx::server::workflow_detail {
namespace {
using namespace store_detail;
WorkflowInput validate(const WorkflowInput& input, std::int64_t now) {
    auto result = input;
    result.name = workspace_detail::validate_name(input.name);
    workspace_detail::validate_text(input.description, 512);
    workspace_detail::validate_text(input.idempotency_key, 128);
    governance_detail::validate_actor(input.actor_id);
    if (input.interval_ms < 60000 || input.interval_ms > 2592000000ULL ||
        input.max_runs == 0 || input.max_runs > 1000 || input.first_run_at_ms <= 0) {
        throw Error(ErrorCode::invalid, "invalid workflow interval, budget or first UTC slot");
    }
    constexpr auto horizon = 365LL * 86400000;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((input.first_run_at_ms > now && input.first_run_at_ms - now > horizon) ||
        input.interval_ms * input.max_runs >
            static_cast<std::uint64_t>(maximum - input.first_run_at_ms)) {
        throw Error(ErrorCode::invalid,
                    "workflow scheduling horizon overflows or exceeds one year");
    }
    if (input.alpha_id.empty()) {
        if (input.alpha_revision != 0 || input.request_json.empty()) {
            throw Error(ErrorCode::invalid,
                        "workflow requires a recipe or an exact alpha revision");
        }
        result.request_json = library_detail::canonical_recipe(input.request_json);
    } else {
        if (!input.request_json.empty()) {
            throw Error(ErrorCode::invalid, "workflow recipe and alpha reference are exclusive");
        }
        workspace_detail::validate_id(input.alpha_id);
        workspace_detail::validate_revision(input.alpha_revision);
    }
    return result;
}
std::string semantic(const WorkflowInput& input, std::string_view recipe) {
    auto text = nlohmann::json{{"name", input.name}, {"description", input.description},
        {"recipe", recipe}, {"alpha_id", input.alpha_id},
        {"alpha_revision", input.alpha_revision}, {"actor_id", input.actor_id},
        {"interval_ms", input.interval_ms}, {"max_runs", input.max_runs},
        {"first_run_at_ms", input.first_run_at_ms}}.dump();
    if (text.size() > 98304) throw Error(ErrorCode::invalid, "workflow semantics exceed 96 KiB");
    return text;
}
void insert_definition(sqlite3* db, const WorkflowInput& input, std::string_view id,
    std::string_view request, std::string_view fingerprint, std::int64_t now) {
    const auto digest = atx::core::sha256_hex(request);
    if (!digest) throw Error(ErrorCode::unavailable, "recipe hashing failed");
    auto query = prepare(db, "INSERT INTO workflow_definitions(id,name,description,request_json,"
        "request_sha256,alpha_id,alpha_revision,actor_id,interval_ms,max_runs,first_run_at_ms,"
        "created_at_ms,idempotency_key,semantic_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    bind_value(query, 1, id); bind_value(query, 2, input.name);
    bind_value(query, 3, input.description); bind_value(query, 4, request);
    bind_value(query, 5, *digest); bind_value(query, 6, input.alpha_id);
    bind_value(query, 7, static_cast<std::int64_t>(input.alpha_revision));
    bind_value(query, 8, input.actor_id);
    bind_value(query, 9, static_cast<std::int64_t>(input.interval_ms));
    bind_value(query, 10, static_cast<std::int64_t>(input.max_runs));
    bind_value(query, 11, input.first_run_at_ms); bind_value(query, 12, now);
    if (input.idempotency_key.empty()) {
        if (sqlite3_bind_null(query.get(), 13) != SQLITE_OK) database_error(db);
    } else bind_value(query, 13, input.idempotency_key);
    bind_value(query, 14, fingerprint); static_cast<void>(step(query));
    auto head = prepare(db, "INSERT INTO workflow_heads(id,revision,enabled,next_run_at_ms,"
        "updated_at_ms) VALUES(?,1,1,?,?)");
    bind_value(head, 1, id); bind_value(head, 2, input.first_run_at_ms);
    bind_value(head, 3, now); static_cast<void>(step(head));
}
void lifecycle(sqlite3* db, Workflow& workflow, bool paused, std::string_view reason,
    std::string_view actor, std::string_view action, std::int64_t now) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (workflow.revision >= static_cast<std::uint64_t>(maximum)) {
        throw Error(ErrorCode::unavailable, "workflow lifecycle revision exhausted");
    }
    auto query = prepare(db, "UPDATE workflow_heads SET revision=revision+1,enabled=?,"
        "pause_reason=?,last_dispatch_error='',updated_at_ms=? WHERE id=? AND revision=?");
    bind_value(query, 1, paused ? 0LL : 1LL); bind_value(query, 2, reason);
    bind_value(query, 3, now); bind_value(query, 4, workflow.id);
    bind_value(query, 5, static_cast<std::int64_t>(workflow.revision));
    static_cast<void>(step(query));
    if (sqlite3_changes(db) != 1) throw Error(ErrorCode::conflict, "workflow revision changed");
    ++workflow.revision; workflow.enabled = !paused; workflow.pause_reason = reason;
    workflow.last_dispatch_error.clear(); workflow.updated_at_ms = now;
    governance_detail::audit(db, "workflow", workflow.id, action, actor, workflow.revision,
        {{"reason", reason}, {"creator_actor_id", workflow.actor_id}}, now);
}
} // namespace
Workflow create(sqlite3* db, const WorkflowInput& input, std::int64_t now) {
    const auto validated = validate(input, now);
    Transaction transaction(db);
    const auto request = experiment_detail::resolve(db, validated.request_json,
        validated.alpha_id, validated.alpha_revision);
    const auto fingerprint = semantic(validated, request);
    if (!validated.idempotency_key.empty()) {
        auto query = prepare(db,
            "SELECT id,semantic_json FROM workflow_definitions WHERE idempotency_key=?");
        bind_value(query, 1, validated.idempotency_key);
        if (step(query) == SQLITE_ROW) {
            if (column(query, 1) != fingerprint) {
                throw Error(ErrorCode::conflict, "idempotency key belongs to a different workflow");
            }
            auto existing = get(db, column(query, 0));
            transaction.commit(); return existing;
        }
    }
    experiment_detail::require_active_alpha(db, validated.alpha_id);
    auto count = prepare(db, "SELECT count(*) FROM workflow_definitions");
    if (step(count) != SQLITE_ROW) database_error(db);
    if (sqlite3_column_int64(count.get(), 0) >= 256) {
        throw Error(ErrorCode::capacity, "workflow definition capacity reached");
    }
    const auto id = identifier();
    insert_definition(db, validated, id, request, fingerprint, now);
    governance_detail::audit(db, "workflow", id, "workflow.created", validated.actor_id, 1,
        {{"alpha_id", validated.alpha_id}, {"alpha_revision", validated.alpha_revision},
         {"max_runs", validated.max_runs}, {"interval_ms", validated.interval_ms}}, now);
    auto created = get(db, id);
    transaction.commit(); return created;
}
Workflow set_paused(sqlite3* db, std::string_view id, std::uint64_t expected,
    bool paused, std::string_view actor, std::int64_t now) {
    Transaction transaction(db);
    auto workflow = get(db, id);
    if (workflow.revision != expected) {
        throw Error(ErrorCode::conflict, "workflow revision changed");
    }
    if (!paused && workflow.admitted_runs >= workflow.max_runs) {
        throw Error(ErrorCode::conflict, "completed workflow budget cannot be resumed");
    }
    // Even an already-paused request is a CAS-protected, auditable lifecycle action.
    lifecycle(db, workflow, paused, paused ? "operator" : "", actor,
        paused ? "workflow.paused" : "workflow.resumed", now);
    workflow = get(db, id);
    transaction.commit(); return workflow;
}
void pause_automatically(sqlite3* db, Workflow& workflow, std::string_view reason,
    std::int64_t now) {
    lifecycle(db, workflow, true, reason, "system", "workflow.auto_paused", now);
}
} // namespace atx::server::workflow_detail
