#pragma once
#include <atx/server/core.hpp>
#include <sqlite3.h>

namespace atx::server::experiment_detail {
struct Input { std::string name, description, key; std::vector<SimulationInput> simulations;
    std::int64_t scheduled_at_ms{}; std::string actor_id; };
[[nodiscard]] Input validate(const ExperimentInput& input);
void migrate(sqlite3* db);
[[nodiscard]] Job create_job(sqlite3* db, std::string_view request, std::string_view key,
    std::int64_t scheduled, std::size_t capacity, const JobContext& context, std::int64_t now);
[[nodiscard]] Experiment create(sqlite3* db, const Input& input,
    std::size_t capacity, std::int64_t now);
[[nodiscard]] Experiment get(sqlite3* db, std::string_view id);
[[nodiscard]] ExperimentPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset);
[[nodiscard]] Experiment cancel(sqlite3* db, std::string_view id, std::int64_t now, std::string_view actor);
} // namespace atx::server::experiment_detail
