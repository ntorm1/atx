#pragma once
#include "store_sql.hpp"

namespace atx::server::experiment_detail {
[[nodiscard]] std::string resolve(sqlite3* db, std::string_view request,
    std::string_view alpha_id, std::uint64_t revision);
void require_active_alpha(sqlite3* db, std::string_view id);
void require_capacity(sqlite3* db, std::size_t capacity, std::size_t requested);
void insert_job(sqlite3* db, const Job& job, std::string_view key = {});
} // namespace atx::server::experiment_detail
