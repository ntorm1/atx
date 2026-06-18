#include "stages.hpp"

#include "atx/core/error.hpp"

namespace atx::impl {

atx::core::Result<atx::u64> run_panel([[maybe_unused]] const RunConfig& cfg) {
    return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                          "panel not implemented");
}

} // namespace atx::impl
