// Lane 2 — Engine::set_cs_pool: cross-sectional date-band parallelism is
// bit-identical to the serial row loop for every pool size (AuditExact).

#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/alpha/wq101_battery.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/digest.hpp"

namespace atx_test_l2_vmcache_vm_cs_pool {

namespace alpha = atx::engine::alpha;
using atx::engine::parallel::DetPool;
using atx::engine::parallel::signal_set_digest;

[[nodiscard]] const alpha::Library &lib() {
  static const alpha::Library l;
  return l;
}

[[nodiscard]] alpha::Program battery_program() {
  const auto all = alpha::wq101_alphas();
  const std::vector<std::string_view> srcs(all.begin(), all.end());
  auto p = alpha::compile_batch(srcs, lib());
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(alpha::Program{});
}

[[nodiscard]] alpha::Panel panel() {
  auto p = alpha::make_wq101_panel(260, 23);
  EXPECT_TRUE(p.has_value());
  return std::move(p).value();
}

[[nodiscard]] atx::u64 digest_with(const alpha::Panel &pnl, const alpha::Program &prog,
                                   DetPool *cs, DetPool *ts) {
  alpha::Engine e{pnl};
  e.set_cs_pool(cs);
  e.set_ts_pool(ts);
  auto r = e.evaluate(prog);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  return r ? signal_set_digest(r.value()) : 0U;
}

TEST(AlphaVmCsPool_Digest, Pool1EqualsPool8EqualsSerial) {
  const alpha::Panel pnl = panel();
  const alpha::Program prog = battery_program();
  const atx::u64 serial = digest_with(pnl, prog, nullptr, nullptr);
  DetPool p1{1};
  DetPool p8{8};
  EXPECT_EQ(serial, digest_with(pnl, prog, &p1, nullptr));
  EXPECT_EQ(serial, digest_with(pnl, prog, &p8, nullptr));
  // Cs and Ts pools may share one instance (never nested).
  EXPECT_EQ(serial, digest_with(pnl, prog, &p8, &p8));
}

TEST(AlphaVmCsPool_Digest, GroupedOpsBitIdentical) {
  const alpha::Panel pnl = panel();
  const std::vector<std::string_view> srcs = {
      "group_rank(close, IndClass.sector)",
      "group_zscore(volume, IndClass.industry)",
      "cs_residualize(returns, IndClass.sector, cap)",
      "quantile(close, 7) + vec_avg(open) + winsorize(returns, 2)",
      "group_scale(high - low, IndClass.subindustry)"};
  auto prog = alpha::compile_batch(srcs, lib());
  ASSERT_TRUE(prog.has_value()) << prog.error().message();
  DetPool p3{3};
  EXPECT_EQ(digest_with(pnl, prog.value(), nullptr, nullptr),
            digest_with(pnl, prog.value(), &p3, nullptr));
}

TEST(AlphaVmCsPool_Setup, NullPoolRestoresSerial) {
  alpha::Panel pnl = panel();
  alpha::Engine e{pnl};
  DetPool p2{2};
  e.set_cs_pool(&p2);
  EXPECT_EQ(e.cs_pool(), &p2);
  e.set_cs_pool(nullptr);
  EXPECT_EQ(e.cs_pool(), nullptr);
}

} // namespace atx_test_l2_vmcache_vm_cs_pool
