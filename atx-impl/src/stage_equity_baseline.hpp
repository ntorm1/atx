#pragma once

#include "stages.hpp"

namespace atx::impl {

// Fixed, unfit two-signal weekly shaping control. Requires an identified context,
// explicit training evaluation dates and complete feature warmup. Publishes a
// fresh directory; failed attempts retain failure.json and partial evidence.
// This software/book diagnostic does not certify source economics, historical
// availability, common-stock eligibility, neutrality or deployable capacity.
[[nodiscard]] atx::core::Result<StageResult> run_equity_baseline(const RunConfig &config);

} // namespace atx::impl
