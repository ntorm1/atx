#include <atx/server/core.hpp>
#include "workspace_validation.hpp"
#include "store_sql.hpp"
#include "library_store.hpp"
#include "experiment_store.hpp"
#include "job_row.hpp"
#include "governance_store.hpp"
#include "workflow_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <list>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace atx::server {
namespace {
using namespace store_detail;
using job_detail::row;
using job_detail::fields;
WorkspaceDocument workspace_row(Statement& stmt) {
    return {column(stmt, 0), column(stmt, 1), column(stmt, 2), column(stmt, 3),
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 4)),
        sqlite3_column_int64(stmt.get(), 5), sqlite3_column_int64(stmt.get(), 6), column(stmt, 7)};
}
bool valid_status(std::string_view status) {
    return status.empty() || status == "QUEUED" || status == "RUNNING" ||
           status == "SUCCEEDED" || status == "FAILED" || status == "CANCELLED";
}
std::size_t job_bytes(const Job& job) {
    // Account string capacities and conservative index/LRU overhead; allocator RSS is external.
    return sizeof(Job) + 128 + 3 * (job.id.capacity() + 1) + job.status.capacity() + 1 +
           job.request_json.capacity() + 1 + job.result_json.capacity() + 1 +
           job.error.capacity() + 1 + job.label.capacity() + 1 + job.alpha_id.capacity() + 1 +
           job.experiment_id.capacity() + 1 + job.actor_id.capacity() + 1 +
           job.workflow_id.capacity() + 1;
}
} // namespace

Error::Error(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}
ErrorCode Error::code() const noexcept { return code_; }
std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Store::Impl {
    struct TransparentHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };
    struct CacheEntry {
        Job job;
        std::chrono::steady_clock::time_point expires;
        std::list<std::string>::iterator position;
        std::size_t bytes{};
    };
    Database db;
    CacheOptions options;
    const std::function<std::int64_t()> clock;
    std::mutex mutex;
    std::list<std::string> lru;
    std::unordered_map<std::string, CacheEntry, TransparentHash, std::equal_to<>> cache;
    std::size_t cache_bytes{};
    std::uint64_t hits{};
    std::uint64_t misses{};

    Impl(std::string_view path, CacheOptions cache_options,
         std::function<std::int64_t()> clock_callback)
        : options(cache_options), clock(clock_callback ? std::move(clock_callback)
            : std::function<std::int64_t()>{unix_ms}) {
        if (path.empty() || options.ttl.count() < 0 || options.ttl > std::chrono::minutes(1) ||
            options.entries > 100000 || options.bytes > 1024ULL * 1024 * 1024) {
            throw Error(ErrorCode::invalid, "invalid database path or cache resource limits");
        }
        sqlite3* handle = nullptr;
        const std::string filename(path);
        const auto opened = sqlite3_open_v2(filename.c_str(), &handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        db.reset(handle);
        if (opened != SQLITE_OK) database_error(db.get());
        if (sqlite3_busy_timeout(db.get(), 5000) != SQLITE_OK) database_error(db.get());
        execute(db.get(), "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
                          "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;");
        {
            auto query = prepare(db.get(), "PRAGMA journal_mode");
            if (step(query) != SQLITE_ROW ||
                (column(query, 0) != "wal" && filename != ":memory:")) {
                throw Error(ErrorCode::unavailable, "persistent database requires WAL support");
            }
        }
        static_cast<void>(sqlite3_limit(db.get(), SQLITE_LIMIT_LENGTH, 20 * 1024 * 1024));
        static_cast<void>(sqlite3_limit(db.get(), SQLITE_LIMIT_SQL_LENGTH, 8192));
        const auto version = [&] {
            auto query = prepare(db.get(), "PRAGMA user_version");
            if (step(query) != SQLITE_ROW) database_error(db.get());
            return sqlite3_column_int(query.get(), 0);
        }();
        if (version > 7) {
            throw Error(ErrorCode::unavailable, "database schema is newer than server");
        }
        if (version == 0) {
            execute(db.get(), "BEGIN IMMEDIATE; CREATE TABLE jobs ("
                "id TEXT PRIMARY KEY, status TEXT NOT NULL CHECK(status IN "
                "('QUEUED','RUNNING','SUCCEEDED','FAILED','CANCELLED')),"
                "request_json TEXT NOT NULL,result_json TEXT NOT NULL DEFAULT '',"
                "error TEXT NOT NULL DEFAULT '',progress REAL NOT NULL DEFAULT 0,"
                "created_at_ms INTEGER NOT NULL,scheduled_at_ms INTEGER NOT NULL,"
                "started_at_ms INTEGER NOT NULL DEFAULT 0,"
                "finished_at_ms INTEGER NOT NULL DEFAULT 0,"
                "label TEXT NOT NULL DEFAULT '',idempotency_key TEXT UNIQUE);"
                "CREATE INDEX jobs_due ON jobs(status,scheduled_at_ms,created_at_ms);"
                "CREATE INDEX jobs_created ON jobs(created_at_ms DESC,id DESC);"
                "PRAGMA user_version=1; COMMIT;");
        }
        if (version < 2) {
            execute(db.get(), "BEGIN IMMEDIATE; CREATE TABLE metadata ("
                "key TEXT PRIMARY KEY,value INTEGER NOT NULL CHECK(value>=0));"
                "INSERT INTO metadata(key,value) VALUES('event_sequence',0);"
                "PRAGMA user_version=2; COMMIT;");
        }
        if (version < 3) {
            execute(db.get(), "BEGIN IMMEDIATE; CREATE TABLE workspaces ("
                "id TEXT PRIMARY KEY, name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 80),"
                "description TEXT NOT NULL CHECK(length(CAST(description AS BLOB))<=512),"
                "document_json TEXT NOT NULL CHECK(length(CAST(document_json AS BLOB))<=98304),"
                "revision INTEGER NOT NULL CHECK(revision>=1),"
                "created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL);"
                "CREATE INDEX workspaces_updated ON workspaces(updated_at_ms DESC,id DESC);"
                "PRAGMA user_version=3; COMMIT;");
        }
        if (version < 4) library_detail::migrate(db.get());
        if (version < 5) experiment_detail::migrate(db.get());
        if (version < 6) governance_detail::migrate(db.get());
        if (version < 7) workflow_detail::migrate(db.get());
    }
    std::int64_t now() const {
        const auto value = clock();
        if (value < 0) throw Error(ErrorCode::invalid, "clock returned a negative Unix timestamp");
        return value;
    }
    void invalidate(std::string_view id) noexcept {
        // Heterogeneous lookup keeps committed-write invalidation allocation-free.
        const auto found = cache.find(id);
        if (found == cache.end()) return;
        cache_bytes -= found->second.bytes;
        lru.erase(found->second.position);
        cache.erase(found);
    }
    Job load(std::string_view id) {
        const auto sql = std::string("SELECT ") + fields + " FROM jobs WHERE id=?";
        auto query = prepare(db.get(), sql.c_str());
        bind_value(query, 1, id);
        if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "job not found");
        return row(query);
    }
    WorkspaceDocument load_workspace(std::string_view id) {
        auto query = prepare(db.get(), "SELECT id,name,description,document_json,revision,"
            "created_at_ms,updated_at_ms,actor_id FROM workspaces WHERE id=?");
        bind_value(query, 1, id);
        if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "workspace not found");
        return workspace_row(query);
    }
    void put(const Job& job) {
        const auto bytes = job_bytes(job);
        if (options.entries == 0 || bytes > options.bytes || options.ttl.count() == 0) return;
        for (auto remaining = cache.size(); remaining > 0 &&
             (cache.size() >= options.entries || cache_bytes > options.bytes - bytes);
             --remaining) {
            invalidate(lru.back());
        }
        lru.push_front(job.id);
        try {
            cache.emplace(job.id, CacheEntry{job, std::chrono::steady_clock::now() + options.ttl,
                                             lru.begin(), bytes});
            cache_bytes += bytes;
        } catch (...) {
            lru.pop_front();
            throw;
        }
    }
    std::uint64_t count(std::string_view status) {
        auto query = prepare(db.get(), status.empty() ? "SELECT count(*) FROM jobs" :
                             "SELECT count(*) FROM jobs WHERE status=?");
        if (!status.empty()) bind_value(query, 1, status);
        if (step(query) != SQLITE_ROW) database_error(db.get());
        return static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0));
    }
};

Store::Store(std::string_view path, CacheOptions cache, std::function<std::int64_t()> clock)
    : impl_(std::make_unique<Impl>(path, cache, std::move(clock))) {}
Store::~Store() = default;
Job Store::create(std::string_view request, std::string_view key,
    std::int64_t scheduled_at_ms, std::size_t capacity) {
    return create(request, key, scheduled_at_ms, capacity, {});
}
Job Store::create(std::string_view request, std::string_view key,
    std::int64_t scheduled_at_ms, std::size_t capacity, const JobContext& context) {
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return experiment_detail::create_job(impl_->db.get(), request, key,
        scheduled_at_ms, capacity, context, now);
}
Job Store::get(std::string_view id) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->cache.find(id);
    if (found != impl_->cache.end()) {
        if (found->second.expires > std::chrono::steady_clock::now()) {
            ++impl_->hits;
            impl_->lru.splice(impl_->lru.begin(), impl_->lru, found->second.position);
            return found->second.job;
        }
        impl_->invalidate(id);
    }
    ++impl_->misses;
    auto job = impl_->load(id);
    impl_->put(job);
    return job;
}
JobPage Store::list(std::uint32_t limit, std::uint32_t offset, std::string_view status) {
    if (limit == 0 || limit > 100 || offset > 1000000 || !valid_status(status)) {
        throw Error(ErrorCode::invalid, "invalid pagination or status");
    }
    std::lock_guard lock(impl_->mutex);
    JobPage page;
    page.total = impl_->count(status);
    const std::string sql = std::string("SELECT ") + job_detail::summaries + " FROM jobs " +
        std::string(status.empty() ? "" : "WHERE status=? ") +
        "ORDER BY created_at_ms DESC,id DESC LIMIT ? OFFSET ?";
    auto query = prepare(impl_->db.get(), sql.c_str());
    auto index = 1;
    if (!status.empty()) bind_value(query, index++, status);
    bind_value(query, index++, static_cast<std::int64_t>(limit));
    bind_value(query, index, static_cast<std::int64_t>(offset));
    page.jobs.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.jobs.push_back(row(query));
    }
    return page;
}
std::optional<Job> Store::claim_due() {
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    // UPDATE RETURNING is atomic between independent database connections.
    const auto sql = std::string("UPDATE jobs SET status='RUNNING',started_at_ms=? WHERE id=") +
        "(SELECT id FROM jobs WHERE status='QUEUED' AND scheduled_at_ms<=? "
        "ORDER BY max(scheduled_at_ms,created_at_ms),created_at_ms,id LIMIT 1) "
        "AND status='QUEUED' RETURNING " + fields;
    auto query = prepare(impl_->db.get(), sql.c_str());
    bind_value(query, 1, now);
    bind_value(query, 2, now);
    if (step(query) != SQLITE_ROW) return std::nullopt;
    auto job = row(query);
    static_cast<void>(step(query));
    impl_->invalidate(job.id);
    return job;
}
Job Store::cancel(std::string_view id) { return cancel(id, "local"); }
Job Store::cancel(std::string_view id, std::string_view actor_id) {
    governance_detail::validate_actor(actor_id);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db.get());
    auto query = prepare(impl_->db.get(), "UPDATE jobs SET status='CANCELLED',finished_at_ms=? "
                         "WHERE id=? AND status IN ('QUEUED','RUNNING')");
    bind_value(query, 1, now);
    bind_value(query, 2, id);
    static_cast<void>(step(query));
    const auto changed = sqlite3_changes(impl_->db.get()) != 0;
    auto result = impl_->load(id);
    if (changed) governance_detail::audit(impl_->db.get(), "job", id, "job.cancelled",
        actor_id, 0, {{"status", "CANCELLED"}}, now);
    transaction.commit();
    impl_->invalidate(id);
    return result;
}
void Store::progress(std::string_view id, double value) {
    if (!std::isfinite(value) || value < 0 || value > 1) {
        throw Error(ErrorCode::invalid, "progress must be finite and between zero and one");
    }
    std::lock_guard lock(impl_->mutex);
    auto query = prepare(impl_->db.get(), "UPDATE jobs SET progress=max(progress,?) "
                         "WHERE id=? AND status='RUNNING'");
    if (sqlite3_bind_double(query.get(), 1, value) != SQLITE_OK) database_error(impl_->db.get());
    bind_value(query, 2, id);
    static_cast<void>(step(query));
    impl_->invalidate(id);
}
void Store::finish(std::string_view id, std::string_view status,
                   std::string_view result, std::string_view error) {
    if ((status != "SUCCEEDED" && status != "FAILED") || result.size() > 16 * 1024 * 1024 ||
        error.size() > 2048) throw Error(ErrorCode::invalid, "invalid job completion");
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    auto query = prepare(impl_->db.get(), "UPDATE jobs SET status=?,result_json=?,error=?,"
        "finished_at_ms=?,progress=CASE WHEN ?='SUCCEEDED' THEN 1 ELSE progress END "
        "WHERE id=? AND status='RUNNING'");
    bind_value(query, 1, status);
    bind_value(query, 2, result);
    bind_value(query, 3, error);
    bind_value(query, 4, now);
    bind_value(query, 5, status);
    bind_value(query, 6, id);
    static_cast<void>(step(query));
    impl_->invalidate(id);
}
void Store::recover() {
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    auto query = prepare(impl_->db.get(), "UPDATE jobs SET status='FAILED',"
        "error='Server restarted during execution; submit a new job to retry',finished_at_ms=? "
        "WHERE status='RUNNING'");
    bind_value(query, 1, now);
    static_cast<void>(step(query));
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->cache_bytes = 0;
}
StoreStats Store::stats() {
    std::lock_guard lock(impl_->mutex);
    return {impl_->count("QUEUED"), impl_->count("RUNNING"), impl_->hits, impl_->misses,
            static_cast<std::uint64_t>(impl_->cache.size()),
            static_cast<std::uint64_t>(impl_->cache_bytes)};
}
std::uint64_t Store::reserve_event_sequences(std::uint64_t count) {
    if (count == 0 || count > 1000000) {
        throw Error(ErrorCode::invalid, "invalid event sequence reservation");
    }
    std::lock_guard lock(impl_->mutex);
    auto query = prepare(impl_->db.get(), "UPDATE metadata SET value=value+? "
        "WHERE key='event_sequence' AND value<=? RETURNING value");
    bind_value(query, 1, static_cast<std::int64_t>(count));
    bind_value(query, 2,
               std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(count));
    if (step(query) != SQLITE_ROW) {
        throw Error(ErrorCode::unavailable, "event sequence space exhausted");
    }
    const auto value = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0));
    static_cast<void>(step(query));
    return value - count;
}
WorkspaceDocument Store::create_workspace(std::string_view name, std::string_view description,
    std::string_view document_json) {
    return create_workspace(name, description, document_json, "local");
}
WorkspaceDocument Store::create_workspace(std::string_view name, std::string_view description,
    std::string_view document_json, std::string_view actor_id) {
    governance_detail::validate_actor(actor_id);
    auto input = workspace_detail::validate(name, description, document_json);
    const auto now = impl_->now();
    WorkspaceDocument result{identifier(), std::move(input.name), std::move(input.description),
        std::move(input.document), 1, now, now, std::string(actor_id)};
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db.get());
    {
        auto count = prepare(impl_->db.get(), "SELECT count(*) FROM workspaces");
        if (step(count) != SQLITE_ROW) database_error(impl_->db.get());
        if (sqlite3_column_int64(count.get(), 0) >= 256) {
            throw Error(ErrorCode::capacity, "workspace capacity of 256 reached");
        }
    }
    auto insert = prepare(impl_->db.get(), "INSERT INTO workspaces"
        "(id,name,description,document_json,revision,created_at_ms,updated_at_ms,actor_id) "
        "VALUES(?,?,?,?,1,?,?,?)");
    bind_value(insert, 1, result.id);
    bind_value(insert, 2, result.name);
    bind_value(insert, 3, result.description);
    bind_value(insert, 4, result.document_json);
    bind_value(insert, 5, now);
    bind_value(insert, 6, now); bind_value(insert, 7, actor_id);
    static_cast<void>(step(insert));
    governance_detail::audit(impl_->db.get(), "workspace", result.id, "workspace.created",
        actor_id, 1, nlohmann::json::object(), now);
    transaction.commit();
    return result; // All allocations precede commit, so the persisted result can move safely.
}
WorkspaceDocument Store::get_workspace(std::string_view id) {
    workspace_detail::validate_id(id);
    std::lock_guard lock(impl_->mutex);
    return impl_->load_workspace(id);
}
WorkspacePage Store::list_workspaces(std::uint32_t limit, std::uint32_t offset) {
    if (limit == 0 || limit > 100 || offset > 1000000) {
        throw Error(ErrorCode::invalid, "invalid workspace pagination");
    }
    std::lock_guard lock(impl_->mutex);
    WorkspacePage page;
    {
        auto count = prepare(impl_->db.get(), "SELECT count(*) FROM workspaces");
        if (step(count) != SQLITE_ROW) database_error(impl_->db.get());
        page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    }
    auto query = prepare(impl_->db.get(), "SELECT id,name,description,'',revision,created_at_ms,"
        "updated_at_ms,actor_id FROM workspaces ORDER BY updated_at_ms DESC,id DESC LIMIT ? OFFSET ?");
    bind_value(query, 1, static_cast<std::int64_t>(limit));
    bind_value(query, 2, static_cast<std::int64_t>(offset));
    page.workspaces.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.workspaces.push_back(workspace_row(query));
    }
    return page;
}
WorkspaceDocument Store::update_workspace(std::string_view id, std::uint64_t expected_revision,
    std::string_view name, std::string_view description, std::string_view document_json) {
    return update_workspace(id, expected_revision, name, description, document_json, "local");
}
WorkspaceDocument Store::update_workspace(std::string_view id, std::uint64_t expected_revision,
    std::string_view name, std::string_view description, std::string_view document_json,
    std::string_view actor_id) {
    governance_detail::validate_actor(actor_id);
    workspace_detail::validate_id(id);
    workspace_detail::validate_revision(expected_revision);
    auto input = workspace_detail::validate(name, description, document_json);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db.get());
    auto result = impl_->load_workspace(id);
    if (result.revision != expected_revision) {
        throw Error(ErrorCode::conflict, "workspace revision changed; reload before saving");
    }
    if (expected_revision == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error(ErrorCode::capacity, "workspace revision space exhausted");
    }
    result.name = std::move(input.name);
    result.description = std::move(input.description);
    result.document_json = std::move(input.document);
    ++result.revision;
    result.updated_at_ms = std::max(now, result.updated_at_ms);
    result.actor_id = actor_id;
    auto update = prepare(impl_->db.get(), "UPDATE workspaces SET name=?,description=?,"
        "document_json=?,revision=revision+1,updated_at_ms=?,actor_id=? WHERE id=? AND revision=?");
    bind_value(update, 1, result.name);
    bind_value(update, 2, result.description);
    bind_value(update, 3, result.document_json);
    bind_value(update, 4, result.updated_at_ms);
    bind_value(update, 5, actor_id); bind_value(update, 6, id);
    bind_value(update, 7, static_cast<std::int64_t>(expected_revision));
    static_cast<void>(step(update));
    if (sqlite3_changes(impl_->db.get()) != 1) {
        throw Error(ErrorCode::conflict, "workspace revision changed during save");
    }
    governance_detail::audit(impl_->db.get(), "workspace", id, "workspace.updated",
        actor_id, result.revision, nlohmann::json::object(), now);
    transaction.commit();
    return result;
}
void Store::delete_workspace(std::string_view id, std::uint64_t expected_revision) {
    delete_workspace(id, expected_revision, "local");
}
void Store::delete_workspace(std::string_view id, std::uint64_t expected_revision,
    std::string_view actor_id) {
    governance_detail::validate_actor(actor_id);
    const auto now = impl_->now();
    workspace_detail::validate_id(id);
    workspace_detail::validate_revision(expected_revision);
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db.get());
    const auto existing = impl_->load_workspace(id);
    if (existing.revision != expected_revision) {
        throw Error(ErrorCode::conflict, "workspace revision changed; reload before deleting");
    }
    auto remove = prepare(impl_->db.get(), "DELETE FROM workspaces WHERE id=? AND revision=?");
    bind_value(remove, 1, id);
    bind_value(remove, 2, static_cast<std::int64_t>(expected_revision));
    static_cast<void>(step(remove));
    if (sqlite3_changes(impl_->db.get()) != 1) {
        throw Error(ErrorCode::conflict, "workspace revision changed during deletion");
    }
    governance_detail::audit(impl_->db.get(), "workspace", id, "workspace.deleted",
        actor_id, expected_revision, nlohmann::json::object(), now);
    transaction.commit();
}
AlphaSnapshot Store::create_alpha(const AlphaInput& input) {
    const auto validated = library_detail::validate(input);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return library_detail::create(impl_->db.get(), validated, now);
}
AlphaSnapshot Store::update_alpha(std::string_view id, std::uint64_t revision,
    const AlphaInput& input) {
    workspace_detail::validate_id(id);
    workspace_detail::validate_revision(revision);
    const auto validated = library_detail::validate(input);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return library_detail::update(impl_->db.get(), id, revision, validated, now);
}
AlphaSnapshot Store::get_alpha(std::string_view id, std::uint64_t revision) {
    workspace_detail::validate_id(id);
    if (revision != 0) workspace_detail::validate_revision(revision);
    std::lock_guard lock(impl_->mutex);
    return library_detail::get(impl_->db.get(), id, revision);
}
AlphaPage Store::list_alphas(std::uint32_t limit, std::uint32_t offset,
    std::string_view search, std::string_view tag, std::string_view project, bool include_archived) {
    library_detail::validate_page(limit, offset);
    library_detail::validate_project(project);
    workspace_detail::validate_text(search, 128);
    workspace_detail::validate_text(tag, 64);
    std::lock_guard lock(impl_->mutex);
    return library_detail::list(impl_->db.get(), limit, offset, search, tag, include_archived);
}
AlphaPage Store::list_alpha_revisions(std::string_view id, std::uint32_t limit,
    std::uint32_t offset) {
    workspace_detail::validate_id(id);
    library_detail::validate_page(limit, offset);
    std::lock_guard lock(impl_->mutex);
    return library_detail::history(impl_->db.get(), id, limit, offset);
}
AlphaSnapshot Store::set_alpha_archived(std::string_view id, std::uint64_t revision,
    bool archived) { return set_alpha_archived(id, revision, archived, "local"); }
AlphaSnapshot Store::set_alpha_archived(std::string_view id, std::uint64_t revision,
    bool archived, std::string_view actor_id) {
    governance_detail::validate_actor(actor_id);
    workspace_detail::validate_id(id);
    workspace_detail::validate_revision(revision);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return library_detail::archive(impl_->db.get(), id, revision, archived, now, actor_id);
}
Experiment Store::create_experiment(const ExperimentInput& input, std::size_t capacity) {
    const auto validated = experiment_detail::validate(input);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return experiment_detail::create(impl_->db.get(), validated, capacity, now);
}
Experiment Store::get_experiment(std::string_view id) {
    workspace_detail::validate_id(id);
    std::lock_guard lock(impl_->mutex);
    return experiment_detail::get(impl_->db.get(), id);
}
ExperimentPage Store::list_experiments(std::uint32_t limit, std::uint32_t offset) {
    library_detail::validate_page(limit, offset);
    std::lock_guard lock(impl_->mutex);
    return experiment_detail::list(impl_->db.get(), limit, offset);
}
Experiment Store::cancel_experiment(std::string_view id) { return cancel_experiment(id, "local"); }
Experiment Store::cancel_experiment(std::string_view id, std::string_view actor_id) {
    governance_detail::validate_actor(actor_id);
    workspace_detail::validate_id(id);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    auto experiment = experiment_detail::cancel(impl_->db.get(), id, now, actor_id);
    for (const auto& job : experiment.jobs) impl_->invalidate(job.id);
    return experiment;
}
AuditPage Store::list_audit(std::uint32_t limit, std::uint32_t offset,
    std::string_view type, std::string_view id, std::string_view actor) {
    std::lock_guard lock(impl_->mutex);
    return governance_detail::list_audit(impl_->db.get(), limit, offset, type, id, actor);
}
ReviewRecord Store::submit_review(const ReviewInput& input) {
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return governance_detail::submit(impl_->db.get(), input, now);
}
ReviewRecord Store::get_review(std::string_view id) {
    workspace_detail::validate_id(id);
    std::lock_guard lock(impl_->mutex);
    return governance_detail::get(impl_->db.get(), id);
}
ReviewPage Store::list_reviews(std::uint32_t limit, std::uint32_t offset,
    std::string_view alpha, std::string_view state) {
    library_detail::validate_page(limit, offset);
    if (!alpha.empty()) workspace_detail::validate_id(alpha);
    governance_detail::validate_state(state, true);
    std::lock_guard lock(impl_->mutex);
    return governance_detail::list(impl_->db.get(), limit, offset, alpha, state);
}
ReviewRecord Store::decide_review(std::string_view id, std::uint64_t expected,
    std::string_view state, std::string_view rationale, std::string_view actor,
    bool independent, bool allow_withdraw_any) {
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return governance_detail::decide(impl_->db.get(), id, expected, state, rationale,
        actor, independent, allow_withdraw_any, now);
}
ReviewDecisionPage Store::list_review_decisions(std::string_view id,
    std::uint32_t limit, std::uint32_t offset) {
    workspace_detail::validate_id(id); library_detail::validate_page(limit, offset);
    std::lock_guard lock(impl_->mutex);
    return governance_detail::decisions(impl_->db.get(), id, limit, offset);
}
Workflow Store::create_workflow(const WorkflowInput& input) {
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return workflow_detail::create(impl_->db.get(), input, now);
}
Workflow Store::get_workflow(std::string_view id) {
    workspace_detail::validate_id(id);
    std::lock_guard lock(impl_->mutex);
    return workflow_detail::get(impl_->db.get(), id);
}
WorkflowPage Store::list_workflows(std::uint32_t limit, std::uint32_t offset) {
    library_detail::validate_page(limit, offset);
    std::lock_guard lock(impl_->mutex);
    return workflow_detail::list(impl_->db.get(), limit, offset);
}
Workflow Store::set_workflow_paused(std::string_view id, std::uint64_t expected,
    bool paused, std::string_view actor) {
    workspace_detail::validate_id(id); workspace_detail::validate_revision(expected);
    governance_detail::validate_actor(actor);
    const auto now = impl_->now();
    std::lock_guard lock(impl_->mutex);
    return workflow_detail::set_paused(impl_->db.get(), id, expected, paused, actor, now);
}
WorkflowRunPage Store::list_workflow_runs(std::string_view id, std::uint32_t limit,
    std::uint32_t offset) {
    workspace_detail::validate_id(id); library_detail::validate_page(limit, offset);
    std::lock_guard lock(impl_->mutex);
    return workflow_detail::runs(impl_->db.get(), id, limit, offset);
}
std::vector<Workflow> Store::due_workflows(std::int64_t now, std::uint32_t limit) {
    if (now < 0 || limit == 0 || limit > 256) {
        throw Error(ErrorCode::invalid, "invalid due workflow scan limit or timestamp");
    }
    std::lock_guard lock(impl_->mutex);
    return workflow_detail::due(impl_->db.get(), now, limit);
}
WorkflowDispatch Store::dispatch_workflow(std::string_view id, std::uint64_t observed,
    bool authorized, std::int64_t now, std::size_t capacity) {
    workspace_detail::validate_id(id); workspace_detail::validate_revision(observed);
    if (now < 0 || capacity == 0 || capacity > 100000) {
        throw Error(ErrorCode::invalid, "invalid workflow dispatch limits");
    }
    std::lock_guard lock(impl_->mutex);
    return workflow_detail::dispatch(impl_->db.get(), id, observed, authorized, now, capacity);
}
std::int64_t Store::current_time_ms() const { return impl_->now(); }
} // namespace atx::server
