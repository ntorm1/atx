#pragma once

// research_sim.hpp — shared frictionless ExecutionSimulator for atx-impl stages.
//
// Factored out of stage_discover.cpp (S4 §3 DRY) so both stage_discover and
// stage_combine include a single definition instead of duplicating the verbatim
// literal block.

#include "atx/engine/exec/execution_sim.hpp"

namespace atx::impl {

[[nodiscard]] inline atx::engine::exec::ExecutionSimulator frictionless_sim() {
    using namespace atx::engine::exec;
    return ExecutionSimulator{
        FillCfg{},
        SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
        ImpactCfg{0.0, 0.5, 0.0},
        CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
        LatencyCfg{},
        VolumeCapCfg{1.0}};
}

} // namespace atx::impl
