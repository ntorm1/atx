#pragma once

#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/replay.hpp"
#include "panel_artifact.hpp"
#include "stages.hpp"

namespace atx::impl {

struct PipelinePanel;

// Caller-supplied allocation model and its frozen provenance. recipe is a JSON
// object in canonical nlohmann::json::dump() form, at most 1 MiB and depth 64.
// Up to 256 extra parents use unique printable ASCII roles (1..256 bytes) and
// lowercase SHA-256 digests. Standard report-parent roles cannot be reused.
// The caller binds any externally captured model state to this recipe/parents;
// a generic callback does not establish feasibility or investment validity.
struct ReplayReportPolicy {
    atx::engine::book::ReplayAllocationPolicy callback;
    std::string recipe;
    std::vector<PanelParent> parents;
    // Required only by run_policy_replay_report's file-loading wrapper. This is
    // an inclusive cap per serialized APNL input, not a process-memory limit.
    // The three expected IDs bind its bounded reads to the caller's admission
    // snapshot; coherent file replacements must not change the admitted trial.
    atx::u64 max_input_payload_bytes{};
    std::string expected_research_artifact_id;
    std::string expected_books_artifact_id;
    std::string expected_combo_artifact_id;
    // Exactly one callback is required. Intents preserve exact held units and
    // distinguish explicit closes from weight sizing; the legacy callback keeps
    // its existing report/accounting contract. Both populated is an error.
    atx::engine::book::ReplayIntentPolicy intent_callback;
};

// Uses the report stage's ordinary schedule/cost parser and requires identified
// artifacts, including combo. Requires a positive input cap and all three valid
// expected IDs, checks each bounded load against those IDs before invoking the
// policy. Applies the same policy descriptor and publication contract below.
[[nodiscard]] atx::core::Result<StageResult>
run_policy_replay_report(const RunConfig &config, const ReplayReportPolicy &policy);

// Identified research replay only. Inputs and schedule must already pass artifact
// parent/axis validation; this boundary checks again before writing. The optional
// combo must be the validated source of the books and its fit-boundary companion.
// Uses a fresh directory, retains pending/partial evidence on failure, and publishes
// manifest.json last. Existing outputs are never overwritten. Historical availability,
// eligibility, execution prices/fills, and investment validity remain unverified.
// A supplied policy treats books as preferences, publishes the accepted allocation
// dollars separately, and binds policy provenance into report identity. Validate and
// snapshot its descriptor before reserving output. A null policy preserves fixed-
// target report bytes. Callback exceptions return failure with no completed manifest.
[[nodiscard]] atx::core::Result<StageResult>
run_identified_replay_report(const RunConfig &config, const PipelinePanel &research,
                             const PipelinePanel &books,
                             std::span<const atx::usize> decision_periods,
                             std::span<const atx::f64> planning_cost_bps,
                             const PipelinePanel *combo = nullptr,
                             const ReplayReportPolicy *policy = nullptr);

} // namespace atx::impl
