#pragma once

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "config.hpp"

namespace atx::impl {

// Each stage returns a u64 digest on success (content hash / row count / etc.
// — exact semantics determined per sprint). Returns Err(NotImplemented) as a
// stub until the real logic is implemented in a later sprint.
[[nodiscard]] atx::core::Result<atx::u64> run_load(const RunConfig&);
[[nodiscard]] atx::core::Result<atx::u64> run_panel(const RunConfig&);
[[nodiscard]] atx::core::Result<atx::u64> run_discover(const RunConfig&);
[[nodiscard]] atx::core::Result<atx::u64> run_combine(const RunConfig&);
[[nodiscard]] atx::core::Result<atx::u64> run_optimize(const RunConfig&);
[[nodiscard]] atx::core::Result<atx::u64> run_report(const RunConfig&);
[[nodiscard]] atx::core::Result<atx::u64> run_all(const RunConfig&);

} // namespace atx::impl
