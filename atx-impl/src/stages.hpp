#pragma once

#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "config.hpp"

namespace atx::impl {

// Each stage returns a digest + printed key/value pairs on success.
struct StageResult {
    atx::u64 digest = 0;
    std::vector<std::pair<std::string, std::string>> kvs;  // emitted on the digest line, in order
};

[[nodiscard]] atx::core::Result<StageResult> run_load(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_panel(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_discover(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_combine(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_optimize(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_report(const RunConfig&);
[[nodiscard]] atx::core::Result<StageResult> run_all(const RunConfig&);

} // namespace atx::impl
