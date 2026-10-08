#include "experiment_store.hpp"
#include "experiment_jobs.hpp"
#include "job_row.hpp"
#include "store_sql.hpp"
#include "governance_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace atx::server::experiment_detail {
namespace {
using namespace store_detail;
void aggregate(Experiment& experiment, const Job& job) {
    if (job.status == "QUEUED") ++experiment.queued;
    else if (job.status == "RUNNING") ++experiment.running;
    else if (job.status == "SUCCEEDED") ++experiment.succeeded;
    else if (job.status == "FAILED") ++experiment.failed;
    else if (job.status == "CANCELLED") ++experiment.cancelled;
    ++experiment.total;
    experiment.progress += job.status == "QUEUED" || job.status == "RUNNING" ? job.progress : 1;
}
void finish_aggregate(Experiment& experiment) {
    if (experiment.total == 0) throw Error(ErrorCode::unavailable, "experiment has no members");
    const auto terminal = experiment.succeeded + experiment.failed + experiment.cancelled;
    if (experiment.running != 0 || (experiment.queued != 0 && terminal != 0)) {
        experiment.status = "RUNNING";
    } else if (experiment.queued != 0) experiment.status = "QUEUED";
    else if (experiment.failed != 0) experiment.status = "FAILED";
    else if (experiment.cancelled != 0) experiment.status = "CANCELLED";
    else experiment.status = "SUCCEEDED";
    experiment.progress /= experiment.total;
}
Experiment load(sqlite3* db, std::string_view id, bool members) {
    auto query = prepare(db, "SELECT id,name,description,created_at_ms,scheduled_at_ms,actor_id "
        "FROM experiments WHERE id=?");
    bind_value(query, 1, id);
    if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "experiment not found");
    Experiment experiment;
    experiment.id = column(query, 0); experiment.name = column(query, 1);
    experiment.description = column(query, 2);
    experiment.created_at_ms = sqlite3_column_int64(query.get(), 3);
    experiment.scheduled_at_ms = sqlite3_column_int64(query.get(), 4);
    experiment.actor_id = column(query, 5);
    auto children = prepare(db, (std::string("SELECT ") + job_detail::summaries +
        " FROM jobs WHERE experiment_id=? ORDER BY "
        "(SELECT position FROM experiment_members m WHERE m.job_id=jobs.id) LIMIT 33").c_str());
    bind_value(children, 1, id);
    if (members) experiment.jobs.reserve(32);
    for (unsigned i = 0; i < 33 && step(children) == SQLITE_ROW; ++i) {
        auto job = job_detail::row(children);
        aggregate(experiment, job);
        if (members) experiment.jobs.push_back(std::move(job));
    }
    if (experiment.total > 32) {
        throw Error(ErrorCode::unavailable, "invalid experiment member count");
    }
    finish_aggregate(experiment);
    return experiment;
}
std::string semantics(const Input& input, const std::vector<Job>& jobs) {
    nlohmann::json members = nlohmann::json::array();
    for (const auto& job : jobs) {
        members.push_back({{"label", job.label},
            {"request", nlohmann::json::parse(job.request_json)},
            {"alpha_id", job.alpha_id}, {"alpha_revision", job.alpha_revision}});
    }
    return nlohmann::json{{"name", input.name}, {"description", input.description},
        {"scheduled_at_ms", input.scheduled_at_ms}, {"simulations", members},
        {"actor_id", input.actor_id}}.dump();
}
} // namespace
void migrate(sqlite3* db) {
    Transaction transaction(db);
    execute(db, "CREATE TABLE experiments (id TEXT PRIMARY KEY,"
        "name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 80),"
        "description TEXT NOT NULL CHECK(length(CAST(description AS BLOB))<=512),"
        "created_at_ms INTEGER NOT NULL,scheduled_at_ms INTEGER NOT NULL CHECK(scheduled_at_ms>=0),"
        "idempotency_key TEXT UNIQUE,semantic_json TEXT NOT NULL "
        "CHECK(length(CAST(semantic_json AS BLOB))<=131072));"
        "CREATE INDEX experiments_created ON experiments(created_at_ms DESC,id DESC);"
        "ALTER TABLE jobs ADD COLUMN alpha_id TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE jobs ADD COLUMN alpha_revision INTEGER NOT NULL DEFAULT 0 "
        "CHECK(alpha_revision>=0);"
        "ALTER TABLE jobs ADD COLUMN experiment_id TEXT NOT NULL DEFAULT '';"
        "CREATE INDEX jobs_experiment ON jobs(experiment_id);"
        "CREATE INDEX jobs_alpha ON jobs(alpha_id,alpha_revision,created_at_ms DESC);"
        "CREATE TABLE experiment_members (experiment_id TEXT NOT NULL REFERENCES experiments(id),"
        "position INTEGER NOT NULL CHECK(position BETWEEN 0 AND 31),"
        "job_id TEXT NOT NULL UNIQUE REFERENCES jobs(id),PRIMARY KEY(experiment_id,position));"
        "CREATE TRIGGER job_recipe_immutable BEFORE UPDATE OF request_json,alpha_id,alpha_revision,"
        "experiment_id,label,created_at_ms,scheduled_at_ms,idempotency_key ON jobs "
        "BEGIN SELECT RAISE(ABORT,'job recipe and lineage are immutable'); END;"
        "CREATE TRIGGER experiment_immutable BEFORE UPDATE ON experiments "
        "BEGIN SELECT RAISE(ABORT,'experiment submission is immutable'); END;"
        "CREATE TRIGGER experiment_members_immutable BEFORE UPDATE ON experiment_members "
        "BEGIN SELECT RAISE(ABORT,'experiment membership is immutable'); END;"
        "PRAGMA user_version=5;");
    transaction.commit();
}
Experiment get(sqlite3* db, std::string_view id) { return load(db, id, true); }
Experiment create(sqlite3* db, const Input& input, std::size_t capacity, std::int64_t now) {
    if (capacity == 0 || capacity > 100000) {
        throw Error(ErrorCode::invalid, "invalid queue capacity");
    }
    Transaction transaction(db);
    std::vector<Job> jobs;
    jobs.reserve(input.simulations.size());
    std::size_t bytes{};
    for (const auto& simulation : input.simulations) {
        Job job;
        job.status = "QUEUED";
        job.request_json = resolve(db, simulation.request_json, simulation.alpha_id,
                                  simulation.alpha_revision);
        bytes += job.request_json.size();
        if (bytes > 96 * 1024) throw Error(ErrorCode::invalid, "batch recipes exceed 96 KiB");
        job.label = simulation.label; job.alpha_id = simulation.alpha_id;
        job.alpha_revision = simulation.alpha_revision;
        job.created_at_ms = now; job.scheduled_at_ms = input.scheduled_at_ms;
        job.actor_id = input.actor_id;
        jobs.push_back(std::move(job));
    }
    const auto semantic = semantics(input, jobs);
    if (semantic.size() > 131072) throw Error(ErrorCode::invalid, "batch envelope exceeds limit");
    if (!input.key.empty()) {
        auto query = prepare(db,
            "SELECT id,semantic_json FROM experiments WHERE idempotency_key=?");
        bind_value(query, 1, input.key);
        if (step(query) == SQLITE_ROW) {
            if (column(query, 1) != semantic) {
                throw Error(ErrorCode::conflict,
                            "idempotency key belongs to a different experiment");
            }
            auto experiment = get(db, column(query, 0));
            transaction.commit();
            return experiment;
        }
    }
    for (const auto& job : jobs) require_active_alpha(db, job.alpha_id);
    require_capacity(db, capacity, jobs.size());
    auto count = prepare(db, "SELECT count(*) FROM experiments");
    if (step(count) != SQLITE_ROW) database_error(db);
    if (sqlite3_column_int64(count.get(), 0) >= 2048) {
        throw Error(ErrorCode::capacity, "durable experiment capacity reached");
    }
    const auto id = identifier();
    auto parent = prepare(db, "INSERT INTO experiments(id,name,description,created_at_ms,"
        "scheduled_at_ms,idempotency_key,semantic_json,actor_id) VALUES(?,?,?,?,?,?,?,?)");
    bind_value(parent, 1, id); bind_value(parent, 2, input.name);
    bind_value(parent, 3, input.description); bind_value(parent, 4, now);
    bind_value(parent, 5, input.scheduled_at_ms);
    if (input.key.empty()) {
        if (sqlite3_bind_null(parent.get(), 6) != SQLITE_OK) database_error(db);
    } else bind_value(parent, 6, input.key);
    bind_value(parent, 7, semantic); bind_value(parent, 8, input.actor_id);
    static_cast<void>(step(parent));
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        auto& job = jobs[i]; job.id = identifier(); job.experiment_id = id;
        insert_job(db, job);
        governance_detail::audit(db, "job", job.id, "job.submitted", input.actor_id, 0,
            {{"experiment_id", id}, {"alpha_id", job.alpha_id},
             {"alpha_revision", job.alpha_revision}}, now);
        auto member = prepare(db, "INSERT INTO experiment_members(experiment_id,position,job_id) "
            "VALUES(?,?,?)");
        bind_value(member, 1, id); bind_value(member, 2, static_cast<std::int64_t>(i));
        bind_value(member, 3, job.id); static_cast<void>(step(member));
    }
    governance_detail::audit(db, "experiment", id, "experiment.created", input.actor_id, 0,
        {{"members", jobs.size()}}, now);
    auto experiment = get(db, id); // All response allocations precede the irreversible commit.
    transaction.commit();
    return experiment;
}
ExperimentPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset) {
    auto count = prepare(db, "SELECT count(*) FROM experiments");
    if (step(count) != SQLITE_ROW) database_error(db);
    ExperimentPage page;
    page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    auto query = prepare(db, "SELECT id FROM experiments ORDER BY created_at_ms DESC,id DESC "
        "LIMIT ? OFFSET ?");
    bind_value(query, 1, static_cast<std::int64_t>(limit));
    bind_value(query, 2, static_cast<std::int64_t>(offset));
    page.experiments.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.experiments.push_back(load(db, column(query, 0), false));
    }
    return page;
}
Experiment cancel(sqlite3* db, std::string_view id, std::int64_t now, std::string_view actor) {
    Transaction transaction(db);
    const auto original = get(db, id);
    // Validate existence before issuing a bounded update; already terminal members stay intact.
    auto query = prepare(db, "SELECT 1 FROM experiments WHERE id=?");
    bind_value(query, 1, id);
    if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "experiment not found");
    auto update = prepare(db, "UPDATE jobs SET status='CANCELLED',finished_at_ms=? "
        "WHERE experiment_id=? AND status IN ('QUEUED','RUNNING')");
    bind_value(update, 1, now); bind_value(update, 2, id); static_cast<void>(step(update));
    if (sqlite3_changes(db) != 0) {
        for (const auto& job : original.jobs) {
            if (job.status == "QUEUED" || job.status == "RUNNING") {
                governance_detail::audit(db, "job", job.id, "job.cancelled", actor, 0,
                    {{"experiment_id", id}}, now);
            }
        }
        governance_detail::audit(db, "experiment", id, "experiment.cancelled", actor, 0,
            {{"members", original.queued + original.running}}, now);
    }
    auto experiment = get(db, id);
    transaction.commit();
    return experiment;
}
} // namespace atx::server::experiment_detail
