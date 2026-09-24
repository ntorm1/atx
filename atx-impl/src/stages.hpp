#pragma once

#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "config.hpp"

namespace atx::engine::data { struct PitMembershipImage; }

namespace atx::impl {

// Checkpoint 16 panel membership restriction: the allow-list the `panel` stage
// assigns to HistoryDataConfig::allow_ids. UNION of member securityIDs for the cut
// index `cut` over every rebalance whose EFFECTIVE session lies in
// [eval_start_nanos, end_exclusive_nanos), PLUS the single last rebalance effective
// strictly before eval_start_nanos (the cut a session at eval_start actually trades
// under). This is a window-union SUPERSET, deliberately NOT an as-of membership: it
// restricts which columns the panel can ever hold, and proves nothing about
// membership on any individual session. Returns ascending, deduplicated ids.
// Err(InvalidArgument) when `cut` is out of range for a rebalance or the union is empty.
[[nodiscard]] atx::core::Result<std::vector<atx::i64>>
pit_membership_allow_ids(const atx::engine::data::PitMembershipImage& image,
                         atx::usize cut, atx::i64 eval_start_nanos,
                         atx::i64 end_exclusive_nanos);

// Each stage returns a digest + printed key/value pairs on success.
struct StageResult {
    atx::u64 digest = 0;
    std::vector<std::pair<std::string, std::string>> kvs;  // emitted on the digest line, in order
};

[[nodiscard]] atx::core::Result<StageResult> run_load(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_panel(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_discover(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_sweep(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_combine(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_optimize(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_report(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_all(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_regime(const RunConfig&);
// S5-4: standalone "metabook" subcommand entry point (a thin wrapper over
// stage_metabook.hpp's 2-arg run_metabook, building its MetaBookStageConfig from
// cfg.sleeve_method — implemented in stage_run.cpp, the S5-owned hub; the 2-arg
// overload itself lives in stage_metabook.hpp/.cpp, S2-owned, untouched).
[[nodiscard]] atx::core::Result<StageResult> run_metabook(const RunConfig&);

} // namespace atx::impl
