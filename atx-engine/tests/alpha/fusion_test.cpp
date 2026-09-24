// Lane 2 — element-wise fusion: a fused program is BIT-identical to the unfused one
// on every WQ101 battery alpha (singly and as one union program), fuses what it
// should, and leaves non-element-wise instructions untouched.

#include <cstring>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/fusion.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/alpha/wq101_battery.hpp"

namespace atx_test_l2_vmcache_fusion {

namespace alpha = atx::engine::alpha;

[[nodiscard]] const alpha::Library &lib() {
  static const alpha::Library l;
  return l;
}

[[nodiscard]] alpha::Program compile_srcs(const std::vector<std::string_view> &srcs) {
  auto p = alpha::compile_batch(srcs, lib());
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message()) << " in: "
                              << (srcs.size() == 1 ? srcs.front() : "<batch>");
  return p.value_or(alpha::Program{});
}

[[nodiscard]] const alpha::Panel &panel() {
  static const alpha::Panel p = alpha::make_wq101_panel(300, 17).value();
  return p;
}

void expect_bit_identical(const alpha::SignalSet &a, const alpha::SignalSet &b,
                          std::string_view what) {
  ASSERT_EQ(a.alphas.size(), b.alphas.size()) << what;
  for (atx::usize k = 0; k < a.alphas.size(); ++k) {
    const auto &x = a.alphas[k].values;
    const auto &y = b.alphas[k].values;
    ASSERT_EQ(x.size(), y.size()) << what;
    EXPECT_EQ(0, std::memcmp(x.data(), y.data(), x.size() * sizeof(atx::f64)))
        << what << " alpha " << k;
    EXPECT_EQ(a.alphas[k].name, b.alphas[k].name);
  }
}

TEST(AlphaFusion_Battery, EveryAlphaBitIdentical) {
  for (const std::string_view src : alpha::wq101_alphas()) {
    const alpha::Program prog = compile_srcs({src});
    auto fused = alpha::fuse(prog);
    ASSERT_TRUE(fused.has_value()) << src;
    alpha::Engine plain{panel()};
    alpha::Engine fast{panel()};
    auto want = plain.evaluate(prog);
    auto got = fast.evaluate(fused.value());
    ASSERT_TRUE(want.has_value()) << src << ": " << want.error().message();
    ASSERT_TRUE(got.has_value()) << src << ": " << got.error().message();
    expect_bit_identical(want.value(), got.value(), src);
  }
}

TEST(AlphaFusion_Battery, UnionProgramBitIdenticalAndFuses) {
  const auto all = alpha::wq101_alphas();
  const alpha::Program prog = compile_srcs({all.begin(), all.end()});
  auto fused = alpha::fuse(prog);
  ASSERT_TRUE(fused.has_value());
  EXPECT_GT(fused.value().kernels.size(), 10U);
  EXPECT_GT(fused.value().fused_instrs, fused.value().kernels.size());
  alpha::Engine plain{panel()};
  alpha::Engine fast{panel()};
  expect_bit_identical(plain.evaluate(prog).value(), fast.evaluate(fused.value()).value(),
                       "union");
  // Warm re-run on the same Engine (scratch reuse) stays identical.
  expect_bit_identical(plain.evaluate(prog).value(), fast.evaluate(fused.value()).value(),
                       "union warm");
}

TEST(AlphaFusion_Shape, PureElementwiseTreeIsOneKernel) {
  const alpha::Program prog = compile_srcs({"(close - open) / ((high - low) + 0.001)"});
  auto fused = alpha::fuse(prog);
  ASSERT_TRUE(fused.has_value());
  const alpha::FusedProgram &fp = fused.value();
  ASSERT_EQ(fp.kernels.size(), 1U);
  // LoadField x4 + Const + Sub x2 + Add + Div absorbed; zero-copy fields.
  EXPECT_EQ(fp.fused_instrs, 9U);
  atx::usize computes = 0;
  for (const alpha::Instr &in : fp.prog.code) {
    computes += (in.op != alpha::OpCode::Free && in.op != alpha::OpCode::StoreAlpha) ? 1U : 0U;
  }
  EXPECT_EQ(computes, 1U);
  EXPECT_EQ(fp.prog.num_slots, 1U);
}

TEST(AlphaFusion_Shape, NonElementwiseBoundariesStay) {
  // rank() and ts_mean() are not fusible; the Const window 5 feeds ts_mean only and
  // must stay a materialized slot.
  const alpha::Program prog = compile_srcs({"rank(ts_mean(close, 5) * 2) + open"});
  auto fused = alpha::fuse(prog);
  ASSERT_TRUE(fused.has_value());
  bool saw_rank = false;
  bool saw_mean = false;
  for (const alpha::Instr &in : fused.value().prog.code) {
    saw_rank = saw_rank || in.op == alpha::OpCode::CsRank;
    saw_mean = saw_mean || in.op == alpha::OpCode::TsMean;
  }
  EXPECT_TRUE(saw_rank);
  EXPECT_TRUE(saw_mean);
  alpha::Engine plain{panel()};
  alpha::Engine fast{panel()};
  expect_bit_identical(plain.evaluate(prog).value(), fast.evaluate(fused.value()).value(),
                       "boundaries");
}

TEST(AlphaFusion_Shape, SharedRootValueIsNotSwallowed) {
  // `close - open` is a root AND an interior value of the second root: it must be
  // materialized (stored) and still correct.
  const alpha::Program prog = compile_srcs({"close - open", "(close - open) * volume"});
  auto fused = alpha::fuse(prog);
  ASSERT_TRUE(fused.has_value());
  alpha::Engine plain{panel()};
  alpha::Engine fast{panel()};
  expect_bit_identical(plain.evaluate(prog).value(), fast.evaluate(fused.value()).value(),
                       "shared root");
}

TEST(AlphaFusion_Shape, NoElementwiseMeansNoKernels) {
  const alpha::Program prog = compile_srcs({"rank(close)"});
  auto fused = alpha::fuse(prog);
  ASSERT_TRUE(fused.has_value());
  EXPECT_TRUE(fused.value().kernels.empty());
  EXPECT_EQ(fused.value().prog.code.size(), prog.code.size());
}

} // namespace atx_test_l2_vmcache_fusion
