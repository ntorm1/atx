#pragma once

#include <functional>

#include "atx/vol/api/fitting/serve_book.hpp"

namespace atx::vol::detail {

// Private deterministic observation seam: called after phase 1 is published and
// before constructing phase 2. Empty in the public API; never runs on a reader.
[[nodiscard]] Result<ServeBuildStage> serve_build_observed(ServeBook &book,
    const ServeBookEntry &key, const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
    bool two_phase, const std::function<void()> &after_first);

} // namespace atx::vol::detail
