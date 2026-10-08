#include "library_store.hpp"
#include "store_sql.hpp"
#include "governance_store.hpp"

#include <algorithm>
#include <limits>

namespace atx::server::library_detail {
namespace {
using namespace store_detail;
constexpr auto fields = "r.alpha_id,r.project_id,r.name,r.description,r.tags_json,r.stage,"
    "r.revision,r.hypothesis,r.request_json,r.request_sha256,r.source_job_id,"
    "r.created_at_ms,r.updated_at_ms,r.actor_id";
constexpr auto summaries = "r.alpha_id,r.project_id,r.name,r.description,r.tags_json,r.stage,"
    "r.revision,'','',r.request_sha256,r.source_job_id,r.created_at_ms,r.updated_at_ms,r.actor_id";
constexpr auto heads = " FROM alpha_heads h JOIN alpha_revisions r "
    "ON h.id=r.alpha_id AND h.revision=r.revision ";
AlphaSnapshot row(Statement& query) {
    return {column(query, 0), column(query, 1), column(query, 2), column(query, 3),
        column(query, 4), column(query, 5),
        static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 6)), column(query, 7),
        column(query, 8), column(query, 9), column(query, 10),
        sqlite3_column_int64(query.get(), 11), sqlite3_column_int64(query.get(), 12), column(query, 13)};
}
void admission(sqlite3* db, bool new_head) {
    auto query = prepare(db, "SELECT (SELECT count(*) FROM alpha_heads),"
        "(SELECT count(*) FROM alpha_revisions)");
    if (step(query) != SQLITE_ROW) database_error(db);
    if ((new_head && sqlite3_column_int64(query.get(), 0) >= 2048) ||
        sqlite3_column_int64(query.get(), 1) >= 16384) {
        throw Error(ErrorCode::capacity, "research library capacity reached");
    }
}
void source_job(sqlite3* db, const Input& input) {
    if (input.source_job_id.empty()) return;
    auto query = prepare(db, "SELECT request_json FROM jobs WHERE id=?");
    bind_value(query, 1, input.source_job_id);
    if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "source job not found");
    if (canonical_recipe(column(query, 0)) != input.request) {
        throw Error(ErrorCode::conflict, "source job recipe does not match alpha recipe");
    }
}
void append(sqlite3* db, const AlphaSnapshot& snapshot) {
    auto query = prepare(db, "INSERT INTO alpha_revisions(alpha_id,project_id,name,description,"
        "tags_json,stage,revision,hypothesis,request_json,request_sha256,source_job_id,"
        "created_at_ms,updated_at_ms,actor_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    bind_value(query, 1, snapshot.id);
    bind_value(query, 2, snapshot.project_id);
    bind_value(query, 3, snapshot.name);
    bind_value(query, 4, snapshot.description);
    bind_value(query, 5, snapshot.tags_json);
    bind_value(query, 6, snapshot.stage);
    bind_value(query, 7, static_cast<std::int64_t>(snapshot.revision));
    bind_value(query, 8, snapshot.hypothesis);
    bind_value(query, 9, snapshot.request_json);
    bind_value(query, 10, snapshot.request_sha256);
    bind_value(query, 11, snapshot.source_job_id);
    bind_value(query, 12, snapshot.created_at_ms);
    bind_value(query, 13, snapshot.updated_at_ms); bind_value(query, 14, snapshot.actor_id);
    static_cast<void>(step(query));
}
void advance(sqlite3* db, const AlphaSnapshot& snapshot, std::uint64_t expected) {
    auto query = prepare(db, "UPDATE alpha_heads SET revision=? WHERE id=? AND revision=?");
    bind_value(query, 1, static_cast<std::int64_t>(snapshot.revision));
    bind_value(query, 2, snapshot.id);
    bind_value(query, 3, static_cast<std::int64_t>(expected));
    static_cast<void>(step(query));
    if (sqlite3_changes(db) != 1) throw Error(ErrorCode::conflict, "alpha revision changed");
    append(db, snapshot);
}
AlphaSnapshot next(sqlite3* db, std::string_view id, std::uint64_t expected, std::int64_t now) {
    auto snapshot = get(db, id, 0);
    if (snapshot.revision != expected) {
        throw Error(ErrorCode::conflict, "alpha revision changed; refresh before editing");
    }
    if (snapshot.revision ==
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error(ErrorCode::capacity, "alpha revision exhausted");
    }
    admission(db, false);
    ++snapshot.revision;
    snapshot.updated_at_ms = std::max(now, snapshot.updated_at_ms);
    return snapshot;
}
} // namespace
void migrate(sqlite3* db) {
    Transaction transaction(db);
    execute(db, "CREATE TABLE alpha_heads (id TEXT PRIMARY KEY,"
        "revision INTEGER NOT NULL CHECK(revision>=1));"
        "CREATE TABLE alpha_revisions (alpha_id TEXT NOT NULL REFERENCES alpha_heads(id),"
        "project_id TEXT NOT NULL CHECK(project_id='local'),"
        "name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 80),"
        "description TEXT NOT NULL CHECK(length(CAST(description AS BLOB))<=512),"
        "tags_json TEXT NOT NULL CHECK(length(CAST(tags_json AS BLOB))<=8192),"
        "stage TEXT NOT NULL CHECK(stage IN ('research','archived')),"
        "revision INTEGER NOT NULL CHECK(revision>=1),"
        "hypothesis TEXT NOT NULL CHECK(length(CAST(hypothesis AS BLOB))<=4096),"
        "request_json TEXT NOT NULL CHECK(length(CAST(request_json AS BLOB))<=65536),"
        "request_sha256 TEXT NOT NULL CHECK(length(request_sha256)=64),"
        "source_job_id TEXT NOT NULL,created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY(alpha_id,revision));"
        "CREATE INDEX alpha_updated ON alpha_revisions(updated_at_ms DESC,alpha_id DESC);"
        "CREATE TRIGGER alpha_revisions_no_update BEFORE UPDATE ON alpha_revisions "
        "BEGIN SELECT RAISE(ABORT,'alpha revisions are immutable'); END;"
        "CREATE TRIGGER alpha_revisions_no_delete BEFORE DELETE ON alpha_revisions "
        "BEGIN SELECT RAISE(ABORT,'alpha revisions are immutable'); END;"
        "PRAGMA user_version=4;");
    transaction.commit();
}
AlphaSnapshot get(sqlite3* db, std::string_view id, std::uint64_t revision) {
    const auto sql = std::string("SELECT ") + fields + (revision == 0 ?
        std::string(heads) + "WHERE h.id=?" :
        " FROM alpha_revisions r WHERE r.alpha_id=? AND r.revision=?");
    auto query = prepare(db, sql.c_str());
    bind_value(query, 1, id);
    if (revision != 0) bind_value(query, 2, static_cast<std::int64_t>(revision));
    if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "alpha revision not found");
    return row(query);
}
AlphaSnapshot create(sqlite3* db, const Input& input, std::int64_t now) {
    Transaction transaction(db);
    admission(db, true);
    source_job(db, input);
    AlphaSnapshot snapshot{identifier(), input.project_id, input.name, input.description,
        input.tags, "research", 1, input.hypothesis, input.request, input.hash,
        input.source_job_id, now, now, input.actor_id};
    auto query = prepare(db, "INSERT INTO alpha_heads(id,revision) VALUES(?,1)");
    bind_value(query, 1, snapshot.id);
    static_cast<void>(step(query));
    append(db, snapshot);
    governance_detail::audit(db, "alpha", snapshot.id, "alpha.created", input.actor_id,
        snapshot.revision, {{"request_sha256", snapshot.request_sha256}}, now);
    transaction.commit();
    return snapshot;
}
AlphaSnapshot update(sqlite3* db, std::string_view id, std::uint64_t expected,
    const Input& input, std::int64_t now) {
    Transaction transaction(db);
    auto snapshot = next(db, id, expected, now);
    source_job(db, input);
    snapshot.name = input.name;
    snapshot.description = input.description;
    snapshot.tags_json = input.tags;
    snapshot.hypothesis = input.hypothesis;
    snapshot.request_json = input.request;
    snapshot.request_sha256 = input.hash;
    snapshot.source_job_id = input.source_job_id;
    snapshot.actor_id = input.actor_id;
    advance(db, snapshot, expected);
    governance_detail::audit(db, "alpha", snapshot.id, "alpha.updated", input.actor_id,
        snapshot.revision, {{"request_sha256", snapshot.request_sha256}}, now);
    transaction.commit();
    return snapshot;
}
AlphaSnapshot archive(sqlite3* db, std::string_view id, std::uint64_t expected,
    bool archived, std::int64_t now, std::string_view actor) {
    Transaction transaction(db);
    auto snapshot = next(db, id, expected, now);
    snapshot.stage = archived ? "archived" : "research";
    snapshot.actor_id = actor;
    advance(db, snapshot, expected);
    governance_detail::audit(db, "alpha", snapshot.id,
        archived ? "alpha.archived" : "alpha.restored", actor, snapshot.revision,
        {{"request_sha256", snapshot.request_sha256}, {"stage", snapshot.stage}}, now);
    transaction.commit();
    return snapshot;
}
AlphaPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset,
    std::string_view search, std::string_view tag, bool include_archived) {
    const std::string filter = std::string(heads) +
        "WHERE (?=1 OR r.stage='research') AND "
        "(?='' OR instr(lower(r.name || char(10) || r.description),lower(?))>0) AND "
        "(?='' OR EXISTS(SELECT 1 FROM json_each(r.tags_json) WHERE value=?)) ";
    const auto bind_filter = [&](Statement& query) {
        bind_value(query, 1, static_cast<std::int64_t>(include_archived));
        bind_value(query, 2, search); bind_value(query, 3, search);
        bind_value(query, 4, tag); bind_value(query, 5, tag);
    };
    AlphaPage page;
    auto count = prepare(db, ("SELECT count(*)" + filter).c_str());
    bind_filter(count);
    if (step(count) != SQLITE_ROW) database_error(db);
    page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    auto query = prepare(db, (std::string("SELECT ") + summaries + filter +
        "ORDER BY r.updated_at_ms DESC,r.alpha_id DESC LIMIT ? OFFSET ?").c_str());
    bind_filter(query);
    bind_value(query, 6, static_cast<std::int64_t>(limit));
    bind_value(query, 7, static_cast<std::int64_t>(offset));
    page.alphas.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.alphas.push_back(row(query));
    }
    return page;
}
AlphaPage history(sqlite3* db, std::string_view id, std::uint32_t limit, std::uint32_t offset) {
    auto count = prepare(db, "SELECT revision FROM alpha_heads WHERE id=?");
    bind_value(count, 1, id);
    if (step(count) != SQLITE_ROW) throw Error(ErrorCode::missing, "alpha not found");
    AlphaPage page;
    page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    auto query = prepare(db, (std::string("SELECT ") + fields +
        " FROM alpha_revisions r WHERE r.alpha_id=? ORDER BY r.revision DESC LIMIT ? OFFSET ?")
        .c_str());
    bind_value(query, 1, id);
    bind_value(query, 2, static_cast<std::int64_t>(limit));
    bind_value(query, 3, static_cast<std::int64_t>(offset));
    page.alphas.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.alphas.push_back(row(query));
    }
    return page;
}
} // namespace atx::server::library_detail
