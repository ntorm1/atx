#pragma once

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace atx::engine::alpha {
struct Program;
struct SignalSet;
class Panel;
}

namespace atx::server::research_detail {

/// Compile one causal numeric expression with bounded syntax and literal windows.
/// Field names must belong to the caller's dataset. Throws invalid_argument on rejection.
[[nodiscard]] atx::engine::alpha::Program compile_research_dsl(
    std::string_view text, std::span<const std::string> allowed_fields);

/// Weighted kernel work per panel cell, using the same bounded compilation policy.
/// Multi-expression jobs must sum this cost before evaluation, not admit each separately.
[[nodiscard]] std::uint64_t research_dsl_work_per_cell(
    std::string_view text, std::span<const std::string> allowed_fields);

/// Execute with a conservative allocation/work budget. Borrows panel only for this call.
/// Cancellation is checked before and after the bounded reference evaluator phase.
[[nodiscard]] atx::engine::alpha::SignalSet evaluate_research_signal(
    std::string_view text, const atx::engine::alpha::Panel& panel,
    std::uint64_t max_working_bytes = 512ULL * 1024ULL * 1024ULL,
    std::stop_token stop = {});

/// Catalog generated from the exact execution policy; includes canonical syntax and limits.
[[nodiscard]] nlohmann::json research_operator_catalog();

} // namespace atx::server::research_detail
