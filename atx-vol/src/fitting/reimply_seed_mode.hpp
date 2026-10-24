#pragma once

#include "pricing/american_iv_seed_batch.hpp"

namespace atx::vol {
// Actual immutable linked-library research mode. No environment/runtime toggle;
// callers and benchmark provenance consult the same compiled fitting definition.
[[nodiscard]] IvPriceSeedKernel reimply_seed_kernel() noexcept;
} // namespace atx::vol
