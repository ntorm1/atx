// atx::engine::risk — scaffold smoke test (P4b-0).
//
// Proves that `include/atx/engine/risk/fwd.hpp` compiles cleanly under the
// /W4 /permissive- /WX warnings gate and that the atx-engine-tests binary links
// with the new risk namespace in scope. No risk logic is tested here —
// that belongs to P4-6..P4-10.

#include <gtest/gtest.h>

#include "atx/engine/risk/fwd.hpp"

namespace {

// RiskScaffold — caught by `ctest -R RiskScaffold`.
TEST(RiskScaffold, NamespaceCompilesAndLinks) {
  // The fwd.hpp forward-declares all Phase-4b spine types in
  // atx::engine::risk. This test proves the header is well-formed and the
  // translation unit compiles and links cleanly — no risk logic yet.
  SUCCEED();
}

} // namespace
