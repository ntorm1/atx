// atx::engine::combine — scaffold smoke test (P4-0).
//
// Proves that `include/atx/engine/combine/fwd.hpp` compiles cleanly under the
// /W4 /permissive- /WX warnings gate and that the atx-engine-tests binary links
// with the new combine namespace in scope. No combination logic is tested here —
// that belongs to P4-1..P4-5.

#include <gtest/gtest.h>

#include "atx/engine/combine/fwd.hpp"

namespace {

// CombineScaffold — caught by `ctest -R Combine`.
TEST(CombineScaffold, NamespaceCompilesAndLinks) {
  // The fwd.hpp forward-declares all Phase-4a spine types in
  // atx::engine::combine. This test proves the header is well-formed and the
  // translation unit compiles and links cleanly — no combination logic yet.
  SUCCEED();
}

} // namespace
