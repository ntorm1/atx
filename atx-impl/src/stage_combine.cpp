#include "stages.hpp"

#include "atx/core/error.hpp"

namespace atx::impl {

atx::core::Result<atx::u64> run_combine([[maybe_unused]] const RunConfig& cfg) {
    return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                          "combine not implemented");
}

} // namespace atx::impl
