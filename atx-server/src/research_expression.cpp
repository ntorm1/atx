#include "atx/server/research.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "research_dataset.hpp"
#include "research_dsl.hpp"

namespace atx::server {

std::string ResearchBackend::validate_expression_json(std::string_view strategy_dsl,
    std::string_view dataset_id, std::string_view dataset_sha256) const {
    std::vector<std::string> fields{"close", "open", "volume", "returns"};
    if (dataset_id.empty()) {
        if (!dataset_sha256.empty())
            throw std::invalid_argument("a dataset revision requires a registered dataset ID");
    } else {
        const auto found = std::find_if(options_.datasets.begin(), options_.datasets.end(),
            [dataset_id](const auto& item) { return item.id == dataset_id; });
        if (found == options_.datasets.end())
            throw std::invalid_argument("unknown registered dataset ID");
        if (dataset_sha256 != found->manifest_sha256)
            throw std::invalid_argument("dataset revision SHA256 does not match registered pin");
        fields = research_detail::dataset_fields();
    }
    const auto program = research_detail::compile_research_dsl(strategy_dsl, fields);
    const auto hash = atx::core::sha256_hex(strategy_dsl);
    if (!hash) throw std::runtime_error(hash.error().message());
    return nlohmann::json{{"valid", true}, {"policy_version", "atx.research-dsl/v1"},
        {"synthetic", dataset_id.empty()}, {"dataset_id", dataset_id},
        {"dataset_sha256", dataset_sha256}, {"expression_sha256", *hash},
        {"required_lookback", program.required_lookback}, {"fields", program.fields},
        {"unique_nodes", program.unique_nodes}, {"peak_live_slots", program.peak_live_slots},
        {"instruction_count", program.code.size()},
        {"scope", "Syntax, fields and causality validated; "
                  "data coverage and execution are separate"}}
        .dump();
}

} // namespace atx::server
