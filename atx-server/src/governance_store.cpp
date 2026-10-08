#include "governance_store.hpp"
#include "store_sql.hpp"
#include "library_store.hpp"
#include "workspace_validation.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace atx::server::governance_detail {
namespace {
using namespace store_detail;
using Json = nlohmann::json;
constexpr auto joined = " FROM review_submissions s JOIN review_heads h ON h.id=s.id "
    "JOIN alpha_heads a ON a.id=s.alpha_id JOIN alpha_revisions r "
    "ON r.alpha_id=a.id AND r.revision=a.revision ";
std::string fields(bool evidence) {
    return std::string("s.id,s.alpha_id,s.alpha_revision,s.request_sha256,s.requester_actor_id,"
        "s.author_actor_id,h.state,h.revision,s.note,") +
        (evidence ? "s.evidence_json" : "''") + ",s.created_at_ms,h.updated_at_ms,"
        "(a.revision=s.alpha_revision AND r.stage='research')";
}
ReviewRecord row(Statement& query) {
    return {column(query, 0), column(query, 1),
        static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 2)), column(query, 3),
        column(query, 4), column(query, 5), column(query, 6),
        static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 7)), column(query, 8),
        column(query, 9), sqlite3_column_int64(query.get(), 10),
        sqlite3_column_int64(query.get(), 11), sqlite3_column_int(query.get(), 12) != 0};
}
void capacity(sqlite3* db, bool submission) {
    auto count = prepare(db, submission ? "SELECT count(*) FROM review_submissions" :
        "SELECT count(*) FROM review_decisions");
    if (step(count) != SQLITE_ROW) database_error(db);
    if (sqlite3_column_int64(count.get(), 0) >= (submission ? 2048 : 8192)) {
        throw Error(ErrorCode::capacity, "research review capacity reached");
    }
}
} // namespace
ReviewRecord get(sqlite3* db, std::string_view id, bool include_evidence) {
    auto query = prepare(db, ("SELECT " + fields(include_evidence) + joined + "WHERE s.id=?")
        .c_str());
    bind_value(query, 1, id);
    if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "review not found");
    return row(query);
}
ReviewRecord submit(sqlite3* db, const ReviewInput& input, std::int64_t now) {
    workspace_detail::validate_id(input.alpha_id);
    workspace_detail::validate_revision(input.alpha_revision);
    validate_actor(input.actor_id);
    workspace_detail::validate_text(input.note, 4096);
    workspace_detail::validate_text(input.idempotency_key, 128);
    if (input.job_ids.empty() || input.job_ids.size() > 12) {
        throw Error(ErrorCode::invalid, "review requires 1..12 distinct evidence jobs");
    }
    auto jobs = input.job_ids;
    for (const auto& id : jobs) workspace_detail::validate_id(id);
    std::sort(jobs.begin(), jobs.end());
    if (std::adjacent_find(jobs.begin(), jobs.end()) != jobs.end()) {
        throw Error(ErrorCode::invalid, "review evidence jobs must be distinct");
    }
    const auto semantic = Json{{"alpha_id", input.alpha_id},
        {"alpha_revision", input.alpha_revision}, {"jobs", jobs}, {"note", input.note},
        {"actor_id", input.actor_id}}.dump();
    if (semantic.size() > 8192) throw Error(ErrorCode::invalid, "review request exceeds limits");
    Transaction transaction(db);
    if (!input.idempotency_key.empty()) {
        auto query = prepare(db, "SELECT id,semantic_json FROM review_submissions "
            "WHERE idempotency_key=?");
        bind_value(query, 1, input.idempotency_key);
        if (step(query) == SQLITE_ROW) {
            if (column(query, 1) != semantic) {
                throw Error(ErrorCode::conflict, "idempotency key belongs to a different review");
            }
            auto review = get(db, column(query, 0));
            transaction.commit();
            return review;
        }
    }
    capacity(db, true);
    const auto alpha = library_detail::get(db, input.alpha_id, 0);
    if (alpha.revision != input.alpha_revision || alpha.stage != "research") {
        throw Error(ErrorCode::conflict, "review submission requires current active alpha revision");
    }
    const auto author = library_detail::get(db, input.alpha_id, 1).actor_id;
    const auto frozen = evidence(db, alpha, jobs, author);
    const auto id = identifier();
    auto insert = prepare(db, "INSERT INTO review_submissions(id,alpha_id,alpha_revision,"
        "request_sha256,requester_actor_id,author_actor_id,note,evidence_json,created_at_ms,"
        "idempotency_key,semantic_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    bind_value(insert, 1, id); bind_value(insert, 2, alpha.id);
    bind_value(insert, 3, static_cast<std::int64_t>(alpha.revision));
    bind_value(insert, 4, alpha.request_sha256); bind_value(insert, 5, input.actor_id);
    bind_value(insert, 6, author); bind_value(insert, 7, input.note); bind_value(insert, 8, frozen);
    bind_value(insert, 9, now);
    if (input.idempotency_key.empty()) {
        if (sqlite3_bind_null(insert.get(), 10) != SQLITE_OK) database_error(db);
    } else bind_value(insert, 10, input.idempotency_key);
    bind_value(insert, 11, semantic); static_cast<void>(step(insert));
    auto head = prepare(db, "INSERT INTO review_heads(id,state,revision,updated_at_ms) "
        "VALUES(?,'submitted',1,?)");
    bind_value(head, 1, id); bind_value(head, 2, now); static_cast<void>(step(head));
    audit(db, "review", id, "review.submitted", input.actor_id, 1,
        {{"alpha_id", alpha.id}, {"alpha_revision", alpha.revision},
         {"request_sha256", alpha.request_sha256}, {"job_ids", jobs}}, now);
    auto review = get(db, id);
    transaction.commit();
    return review;
}
ReviewRecord decide(sqlite3* db, std::string_view id, std::uint64_t expected,
    std::string_view state, std::string_view rationale, std::string_view actor,
    bool independent, bool allow_withdraw_any, std::int64_t now) {
    workspace_detail::validate_id(id); workspace_detail::validate_revision(expected);
    validate_actor(actor); validate_state(state);
    workspace_detail::validate_text(rationale, 4096);
    if (state == "submitted" || workspace_detail::is_blank(rationale)) {
        throw Error(ErrorCode::invalid, "review decisions require state and nonblank rationale");
    }
    Transaction transaction(db);
    const auto review = get(db, id);
    if (review.revision != expected) throw Error(ErrorCode::conflict, "review revision changed");
    if (review.state != "submitted" && !(review.state == "approved" && state == "withdrawn")) {
        throw Error(ErrorCode::conflict, "review has a terminal decision");
    }
    if (state == "withdrawn" && !allow_withdraw_any && actor != review.requester_actor_id) {
        throw Error(ErrorCode::conflict, "only requester may withdraw this review");
    }
    if (state != "withdrawn" && independent) {
        // Resolve authoritative immutable history, including legacy evidence without this field.
        const auto revision_actor = library_detail::get(db, review.alpha_id,
            review.alpha_revision).actor_id;
        auto authors = prepare(db, "SELECT EXISTS(SELECT 1 FROM alpha_revisions WHERE "
            "alpha_id=? AND request_sha256=? AND revision<=? AND actor_id=?)");
        bind_value(authors, 1, review.alpha_id); bind_value(authors, 2, review.request_sha256);
        bind_value(authors, 3, static_cast<std::int64_t>(review.alpha_revision));
        bind_value(authors, 4, actor);
        if (step(authors) != SQLITE_ROW) database_error(db);
        const auto recipe_author = sqlite3_column_int(authors.get(), 0) != 0;
        if (actor == review.requester_actor_id || actor == review.author_actor_id ||
            actor == revision_actor || recipe_author) {
            throw Error(ErrorCode::conflict,
                "reviewer must be independent from requester, creator and revision author");
        }
    }
    if (state == "approved" && !review.current) {
        throw Error(ErrorCode::conflict, "edited or archived alpha cannot receive current approval");
    }
    capacity(db, false);
    if (expected == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error(ErrorCode::capacity, "review revision exhausted");
    }
    const auto timestamp = std::max(now, review.updated_at_ms);
    auto decision = prepare(db, "INSERT INTO review_decisions(id,review_id,revision,state,"
        "actor_id,rationale,created_at_ms) VALUES(?,?,?,?,?,?,?)");
    bind_value(decision, 1, identifier()); bind_value(decision, 2, id);
    bind_value(decision, 3, static_cast<std::int64_t>(expected + 1));
    bind_value(decision, 4, state); bind_value(decision, 5, actor);
    bind_value(decision, 6, rationale); bind_value(decision, 7, timestamp);
    static_cast<void>(step(decision));
    auto head = prepare(db, "UPDATE review_heads SET state=?,revision=revision+1,updated_at_ms=? "
        "WHERE id=? AND revision=?");
    bind_value(head, 1, state); bind_value(head, 2, timestamp); bind_value(head, 3, id);
    bind_value(head, 4, static_cast<std::int64_t>(expected)); static_cast<void>(step(head));
    if (sqlite3_changes(db) != 1) throw Error(ErrorCode::conflict, "review revision changed");
    audit(db, "review", id, "review." + std::string(state), actor, expected + 1,
        {{"alpha_id", review.alpha_id}, {"alpha_revision", review.alpha_revision},
         {"state", state}, {"current", review.current}}, timestamp);
    auto result = get(db, id);
    transaction.commit();
    return result;
}
ReviewPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset,
    std::string_view alpha, std::string_view state) {
    const auto filter = std::string(joined) +
        "WHERE (?='' OR s.alpha_id=?) AND (?='' OR h.state=?) ";
    const auto bind_filter = [&](Statement& query) {
        bind_value(query, 1, alpha); bind_value(query, 2, alpha);
        bind_value(query, 3, state); bind_value(query, 4, state);
    };
    auto count = prepare(db, ("SELECT count(*)" + filter).c_str());
    bind_filter(count);
    if (step(count) != SQLITE_ROW) database_error(db);
    ReviewPage page;
    page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    auto query = prepare(db, ("SELECT " + fields(false) + filter +
        "ORDER BY h.updated_at_ms DESC,s.id DESC LIMIT ? OFFSET ?").c_str());
    bind_filter(query); bind_value(query, 5, static_cast<std::int64_t>(limit));
    bind_value(query, 6, static_cast<std::int64_t>(offset));
    page.reviews.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.reviews.push_back(row(query));
    }
    return page;
}
ReviewDecisionPage decisions(sqlite3* db, std::string_view id,
    std::uint32_t limit, std::uint32_t offset) {
    static_cast<void>(get(db, id, false));
    auto count = prepare(db, "SELECT count(*) FROM review_decisions WHERE review_id=?");
    bind_value(count, 1, id);
    if (step(count) != SQLITE_ROW) database_error(db);
    ReviewDecisionPage page;
    page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    auto query = prepare(db, "SELECT id,review_id,revision,state,actor_id,rationale,created_at_ms "
        "FROM review_decisions WHERE review_id=? ORDER BY revision DESC LIMIT ? OFFSET ?");
    bind_value(query, 1, id); bind_value(query, 2, static_cast<std::int64_t>(limit));
    bind_value(query, 3, static_cast<std::int64_t>(offset));
    page.decisions.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.decisions.push_back({column(query, 0), column(query, 1),
            static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 2)), column(query, 3),
            column(query, 4), column(query, 5), sqlite3_column_int64(query.get(), 6)});
    }
    return page;
}
} // namespace atx::server::governance_detail
