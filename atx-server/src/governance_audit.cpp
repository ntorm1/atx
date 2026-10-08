#include "governance_store.hpp"
#include "store_sql.hpp"
#include "workspace_validation.hpp"
#include "library_validation.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace atx::server::governance_detail {
using namespace store_detail;
void validate_actor(std::string_view actor) {
    if (actor.empty() || actor.size() > 64 || !std::all_of(actor.begin(), actor.end(),
        [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-'; })) {
        throw Error(ErrorCode::invalid, "actor must be an ASCII identifier of 1..64 bytes");
    }
}
void validate_state(std::string_view state, bool filter) {
    if ((filter && state.empty()) || state == "submitted" || state == "approved" ||
        state == "rejected" || state == "withdrawn") return;
    throw Error(ErrorCode::invalid, "invalid research review state");
}
void audit(sqlite3* db, std::string_view type, std::string_view id, std::string_view action,
    std::string_view actor, std::uint64_t revision, const nlohmann::json& metadata,
    std::int64_t now) {
    validate_actor(actor);
    const auto encoded = metadata.dump();
    if (encoded.size() > 4096 || revision >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error(ErrorCode::invalid, "audit metadata exceeds resource limits");
    }
    auto query = prepare(db, "INSERT INTO audit_records(entity_type,entity_id,action,actor_id,"
        "revision,metadata_json,created_at_ms) VALUES(?,?,?,?,?,?,?)");
    bind_value(query, 1, type); bind_value(query, 2, id); bind_value(query, 3, action);
    bind_value(query, 4, actor); bind_value(query, 5, static_cast<std::int64_t>(revision));
    bind_value(query, 6, encoded); bind_value(query, 7, now); static_cast<void>(step(query));
}
AuditPage list_audit(sqlite3* db, std::uint32_t limit, std::uint32_t offset,
    std::string_view type, std::string_view id, std::string_view actor) {
    library_detail::validate_page(limit, offset);
    constexpr std::array types{"job", "workspace", "alpha", "experiment", "review", "workflow"};
    if (!type.empty() && std::find(types.begin(), types.end(), type) == types.end()) {
        throw Error(ErrorCode::invalid, "invalid audit entity type");
    }
    if (!id.empty()) workspace_detail::validate_id(id);
    if (!actor.empty()) validate_actor(actor);
    // Build only fixed SQL predicates so indexed filters remain sargable at large history sizes.
    std::string filter = " FROM audit_records WHERE 1=1 ";
    if (!type.empty()) filter += "AND entity_type=? ";
    if (!id.empty()) filter += "AND entity_id=? ";
    if (!actor.empty()) filter += "AND actor_id=? ";
    const auto bind_filter = [&](Statement& query) {
        auto index = 1;
        if (!type.empty()) bind_value(query, index++, type);
        if (!id.empty()) bind_value(query, index++, id);
        if (!actor.empty()) bind_value(query, index++, actor);
        return index;
    };
    auto count = prepare(db, ("SELECT count(*)" + filter).c_str());
    static_cast<void>(bind_filter(count));
    if (step(count) != SQLITE_ROW) database_error(db);
    AuditPage page;
    page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    auto query = prepare(db, ("SELECT sequence,entity_type,entity_id,action,actor_id,revision,"
        "metadata_json,created_at_ms" + filter + "ORDER BY sequence DESC LIMIT ? OFFSET ?")
        .c_str());
    auto index = bind_filter(query); bind_value(query, index++, static_cast<std::int64_t>(limit));
    bind_value(query, index, static_cast<std::int64_t>(offset));
    page.records.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.records.push_back({static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 0)),
            column(query, 1), column(query, 2), column(query, 3), column(query, 4),
            static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 5)), column(query, 6),
            sqlite3_column_int64(query.get(), 7)});
    }
    return page;
}
} // namespace atx::server::governance_detail
