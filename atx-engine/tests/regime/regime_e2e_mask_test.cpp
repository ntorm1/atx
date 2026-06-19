// atx::engine::regime — end-to-end proof: regime-conditioned alpha masking.
//
// Proves that "regime_vix < 20 ? close : 0" gates the signal using ONLY
// existing DSL ops (ternary Select + CmpLt) — NO new VM opcodes required.
// A 2-date × 1-instrument panel: vix=15 on date 0 (low vol, close passes
// through) and vix=25 on date 1 (high vol, signal gated to exactly 0).

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/tsdb/builder.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/regime/store.hpp"
#include "atx/engine/regime/with_regime_fields.hpp"

namespace atxtest_regime_e2e {
namespace fs = std::filesystem;
constexpr atx::i64 kDay = 86400LL * 1000000000LL;
using namespace atx::engine::alpha;
using atx::engine::regime::RegimeStore;
using atx::engine::regime::with_regime_fields;

[[nodiscard]] const Library &lib() { static const Library l; return l; }

[[nodiscard]] std::vector<atx::f64> eval(std::string_view expr, const Panel &p) {
  auto ast = parse_expr(expr, lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  Engine e{p};
  auto out = e.evaluate(prog.value_or(Program{}));
  EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message());
  return (out.has_value() && !out.value().alphas.empty()) ? out.value().alphas[0].values
                                                          : std::vector<atx::f64>{};
}

[[nodiscard]] std::string regime_seg() {
  std::vector<std::string> f = {"vix"}; std::vector<std::string> s = {"MACRO"};
  std::vector<atx::i64> axis = {0 * kDay, 1 * kDay};
  atx::tsdb::SegmentBuilder b(f, s, axis);
  // set(field, TIME_INDEX, inst, value): 2nd arg indexes `axis`, NOT nanos.
  b.set(0, 0, 0, 15.0);  // axis[0] (=0)      low vol
  b.set(0, 1, 0, 25.0);  // axis[1] (=1*kDay)  high vol
  const std::string p = (fs::temp_directory_path() / "atx_regime_e2e.seg").string();
  EXPECT_TRUE(b.write(p, 0).has_value());
  return p;
}

TEST(RegimeE2E, TernaryMaskGatesSignalOnVix) {
  auto store = RegimeStore::open(regime_seg()).value();
  const atx::usize dates = 2, inst = 1;
  std::vector<atx::i64> panel_dates = {0 * kDay, 1 * kDay};
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {{10.0, 20.0}};  // close = 10 (d0), 20 (d1)
  auto p = with_regime_fields(dates, inst, panel_dates, names, data, {}, store, {"vix"});
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());

  const std::vector<atx::f64> v = eval("regime_vix < 20 ? close : 0", p.value());
  ASSERT_EQ(v.size(), dates * inst);
  EXPECT_DOUBLE_EQ(v[0], 10.0);  // vix 15 < 20 -> close
  EXPECT_DOUBLE_EQ(v[1], 0.0);   // vix 25 !< 20 -> 0 (gated off)
}

}  // namespace atxtest_regime_e2e
