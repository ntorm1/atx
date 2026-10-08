#pragma once
#include <atx/server/core.hpp>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace atx::server::governance_detail {
void validate_actor(std::string_view actor);
void validate_state(std::string_view state, bool filter = false);
void audit(sqlite3* db, std::string_view type, std::string_view id, std::string_view action,
    std::string_view actor, std::uint64_t revision, const nlohmann::json& metadata,
    std::int64_t now);
void migrate(sqlite3* db);
[[nodiscard]] AuditPage list_audit(sqlite3* db, std::uint32_t limit, std::uint32_t offset,
    std::string_view type, std::string_view id, std::string_view actor);
[[nodiscard]] ReviewRecord submit(sqlite3* db, const ReviewInput& input, std::int64_t now);
[[nodiscard]] ReviewRecord get(sqlite3* db, std::string_view id, bool evidence = true);
[[nodiscard]] ReviewPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset,
    std::string_view alpha, std::string_view state);
[[nodiscard]] ReviewRecord decide(sqlite3* db, std::string_view id, std::uint64_t expected,
    std::string_view state, std::string_view rationale, std::string_view actor,
    bool independent, bool allow_withdraw_any, std::int64_t now);
[[nodiscard]] ReviewDecisionPage decisions(sqlite3* db, std::string_view id,
    std::uint32_t limit, std::uint32_t offset);
[[nodiscard]] std::string evidence(sqlite3* db, const AlphaSnapshot& alpha,
    const std::vector<std::string>& jobs, std::string_view author);
} // namespace atx::server::governance_detail
