#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <stop_token>
#include <string>

namespace atx::server {

struct ComparisonInput {
    std::string job_id;
    std::string label;
    std::string result_json;
};

struct ComparisonOptions {
    std::string start_date;
    std::string end_date;
    std::size_t max_chart_points{1000};
    bool allow_mixed_evidence{false};
    // Optional synchronous cancellation probe; called without shared locks.
    std::function<bool()> is_cancelled{};
};

/// Compare 2..12 completed results on the intersection of exact return periods.
/// All moments use complete observations; only the returned chart is sampled.
/// Inputs are borrowed and not mutated. Throws invalid_argument for missing or
/// incompatible evidence, runtime_error on cancellation. No portfolio fills are implied.
[[nodiscard]] std::string compare_research_results(
    std::span<const ComparisonInput> inputs, const ComparisonOptions& options = {},
    std::stop_token stop = {});

} // namespace atx::server
