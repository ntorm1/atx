#include "atx/vol/api/core/version.hpp"

namespace atx::vol {

// Deliberately NOT a literal (plan 5.3): this returns the same string the
// CALLER compiled against, which version.hpp took from the generated
// detail/version_generated.hpp, which configure_file took from
// `project(atx VERSION ...)`. There is nothing here left to drift.
std::string_view version() noexcept {
  return kVersionString;
}

} // namespace atx::vol
