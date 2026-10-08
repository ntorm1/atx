#pragma once
#include "store_sql.hpp"

namespace atx::server::workflow_detail {
void migrate(sqlite3* db);
[[nodiscard]] Workflow create(sqlite3* db, const WorkflowInput& input, std::int64_t now);
[[nodiscard]] Workflow get(sqlite3* db, std::string_view id, bool recipe = true);
[[nodiscard]] WorkflowPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset);
[[nodiscard]] Workflow set_paused(sqlite3* db, std::string_view id, std::uint64_t expected,
    bool paused, std::string_view actor, std::int64_t now);
[[nodiscard]] WorkflowRunPage runs(sqlite3* db, std::string_view id,
    std::uint32_t limit, std::uint32_t offset);
[[nodiscard]] std::vector<Workflow> due(sqlite3* db, std::int64_t now, std::uint32_t limit);
[[nodiscard]] WorkflowDispatch dispatch(sqlite3* db, std::string_view id,
    std::uint64_t observed, bool authorized, std::int64_t now, std::size_t capacity);
void pause_automatically(sqlite3* db, Workflow& workflow, std::string_view reason,
                         std::int64_t now);
} // namespace atx::server::workflow_detail
