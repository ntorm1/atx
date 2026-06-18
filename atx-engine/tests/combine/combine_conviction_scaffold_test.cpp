// atx::engine — S10-0 scaffold smoke test.
//
// Proves the two new S10 spine headers — combine/conviction.hpp (conviction
// score) and risk/kelly_sizing.hpp (fractional-Kelly sizing) — compile cleanly
// under the /W4 /permissive- /WX gate and link into a test binary. No conviction
// or sizing logic is exercised here; that lands in S10-1 (conviction) and S10-2
// (kelly). This mirrors combine/combine_scaffold_test.cpp (the P4-0 scaffold).

#include <gtest/gtest.h>

#include "atx/engine/combine/conviction.hpp"
#include "atx/engine/risk/kelly_sizing.hpp"

namespace atxtest_combine_conviction_scaffold_test {

// ConvictionScaffold — caught by `ctest -R ConvictionScaffold`.
TEST(ConvictionScaffold, HeadersCompileAndLink) {
  // conviction.hpp forward-declares ExplainFlag / ConvictionConfig /
  // ConvictionScore in atx::engine::combine; kelly_sizing.hpp forward-declares
  // KellyConfig / KellyWeights in atx::engine::risk. This test proves both
  // headers are well-formed and the translation unit compiles and links — the
  // full definitions arrive in S10-1 / S10-2.
  SUCCEED();
}

} // namespace atxtest_combine_conviction_scaffold_test
