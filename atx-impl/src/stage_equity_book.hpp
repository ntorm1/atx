#pragma once

#include "stages.hpp"

namespace atx::impl {
// Training-only constrained preference book. Consumes independently identified
// baseline artifacts even if their fixed-target replay failed. Preserves the
// complete original evaluation window and rejects missing held marks.
[[nodiscard]] atx::core::Result<StageResult> run_equity_book(const RunConfig &config);
} // namespace atx::impl
