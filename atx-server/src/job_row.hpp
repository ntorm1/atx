#pragma once
#include "store_sql.hpp"

namespace atx::server::job_detail {
inline constexpr auto fields = "id,status,request_json,result_json,error,progress,created_at_ms,"
    "scheduled_at_ms,started_at_ms,finished_at_ms,label,alpha_id,alpha_revision,experiment_id,"
    "actor_id,workflow_id,workflow_slot_ms";
inline constexpr auto summaries = "id,status,'','',error,progress,created_at_ms,"
    "scheduled_at_ms,started_at_ms,finished_at_ms,label,alpha_id,alpha_revision,experiment_id,"
    "actor_id,workflow_id,workflow_slot_ms";
inline Job row(store_detail::Statement& query) {
    using store_detail::column;
    return {column(query, 0), column(query, 1), column(query, 2), column(query, 3),
        column(query, 4), sqlite3_column_double(query.get(), 5),
        sqlite3_column_int64(query.get(), 6), sqlite3_column_int64(query.get(), 7),
        sqlite3_column_int64(query.get(), 8), sqlite3_column_int64(query.get(), 9),
        column(query, 10), column(query, 11),
        static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 12)), column(query, 13),
        column(query, 14), column(query, 15), sqlite3_column_int64(query.get(), 16)};
}
} // namespace atx::server::job_detail
