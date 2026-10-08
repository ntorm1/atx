#include "experiment_store.hpp"
#include "workspace_validation.hpp"
#include "governance_store.hpp"

#include <utility>

namespace atx::server::experiment_detail {
Input validate(const ExperimentInput& input) {
    governance_detail::validate_actor(input.actor_id);
    if (input.simulations.empty() || input.simulations.size() > 32 ||
        input.idempotency_key.size() > 128 || input.scheduled_at_ms < 0) {
        throw Error(ErrorCode::invalid, "invalid experiment membership or submission limits");
    }
    workspace_detail::validate_text(input.description, 512);
    workspace_detail::validate_text(input.idempotency_key, 128);
    Input result{workspace_detail::validate_name(input.name), input.description,
        input.idempotency_key, {}, input.scheduled_at_ms, input.actor_id};
    result.simulations.reserve(input.simulations.size());
    std::size_t bytes{};
    for (const auto& simulation : input.simulations) {
        const auto linked = !simulation.alpha_id.empty();
        if (linked == !simulation.request_json.empty() ||
            (!linked && simulation.alpha_revision != 0)) {
            throw Error(ErrorCode::invalid, "simulation requires exactly one recipe source");
        }
        auto canonical = std::string{};
        if (linked) {
            workspace_detail::validate_id(simulation.alpha_id);
            workspace_detail::validate_revision(simulation.alpha_revision);
        } else {
            const auto json = workspace_detail::parse_json(simulation.request_json, 65536);
            if (!json.is_object()) throw Error(ErrorCode::invalid, "recipe must be a JSON object");
            canonical = json.dump();
            if (canonical.size() > 65536) throw Error(ErrorCode::invalid, "recipe exceeds limit");
            bytes += canonical.size();
            if (bytes > 96 * 1024) throw Error(ErrorCode::invalid, "batch recipes exceed 96 KiB");
        }
        const auto label = simulation.label.empty() ?
            "Simulation " + std::to_string(result.simulations.size() + 1) :
            workspace_detail::validate_name(simulation.label);
        result.simulations.push_back({label, std::move(canonical), simulation.alpha_id,
                                     simulation.alpha_revision});
    }
    return result;
}
} // namespace atx::server::experiment_detail
