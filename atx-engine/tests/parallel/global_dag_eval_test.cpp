// Lane 2 — strategy B (level-scheduled union DAG) is byte-identical to the serial
// Engine::evaluate of the union program and to strategy A (parallel_evaluate over
// per-alpha programs), for every worker count and chunking; a warm SubtreeCache
// serves a repeated battery almost entirely.

#include <cstring>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/fusion.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/alpha/wq101_battery.hpp"
#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/digest.hpp"
#include "atx/engine/parallel/global_dag_eval.hpp"

namespace atx_test_l2_vmcache_global_dag {

namespace alpha = atx::engine::alpha;
namespace par = atx::engine::parallel;

[[nodiscard]] const alpha::Library &lib() {
  static const alpha::Library l;
  return l;
}

[[nodiscard]] std::vector<std::string_view> battery() {
  const auto all = alpha::wq101_alphas();
  return {all.begin(), all.end()};
}

[[nodiscard]] const alpha::Panel &panel() {
  static const alpha::Panel p = alpha::make_wq101_panel(280, 19).value();
  return p;
}

[[nodiscard]] const alpha::Program &union_prog() {
  static const alpha::Program p = alpha::compile_batch(battery(), lib()).value();
  return p;
}

[[nodiscard]] bool same_bytes(const std::vector<atx::f64> &a, const std::vector<atx::f64> &b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size() * sizeof(atx::f64)) == 0;
}

TEST(ParallelGlobalDag_Identity, EqualsSerialUnionForWorkerCounts) {
  alpha::Engine serial{panel()};
  const atx::u64 want = par::signal_set_digest(serial.evaluate(union_prog()).value());
  for (const atx::usize w : {1U, 2U, 4U, 8U}) {
    par::DetPool pool{w};
    par::GlobalDagStats st{};
    auto got = par::global_dag_evaluate(union_prog(), panel(), pool, nullptr, &st);
    ASSERT_TRUE(got.has_value()) << got.error().message();
    EXPECT_EQ(want, par::signal_set_digest(got.value())) << "workers=" << w;
    EXPECT_GT(st.levels, 1U);
    EXPECT_EQ(st.computed, st.nodes);
  }
}

TEST(ParallelGlobalDag_Identity, TinyChunksStillIdentical) {
  alpha::Engine serial{panel()};
  const atx::u64 want = par::signal_set_digest(serial.evaluate(union_prog()).value());
  par::DetPool pool{3};
  par::GlobalDagOptions opt;
  opt.min_chunk_cells = 1;
  opt.chunks_per_worker = 7;
  auto got = par::global_dag_evaluate(union_prog(), panel(), pool, nullptr, nullptr, opt);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(want, par::signal_set_digest(got.value()));
}

TEST(ParallelGlobalDag_Identity, EqualsStrategyA) {
  std::vector<alpha::Program> progs;
  for (const std::string_view s : battery()) {
    progs.push_back(alpha::compile_batch(std::vector<std::string_view>{s}, lib()).value());
  }
  par::DetPool pool{4};
  auto a = par::parallel_evaluate(progs, panel(), pool);
  ASSERT_TRUE(a.has_value());
  auto b = par::parallel_evaluate_shared(battery(), lib(), panel(), pool, nullptr);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a.value().alphas.size(), b.value().alphas.size());
  for (atx::usize k = 0; k < a.value().alphas.size(); ++k) {
    EXPECT_TRUE(same_bytes(a.value().alphas[k].values, b.value().alphas[k].values))
        << battery()[k];
  }
}

TEST(ParallelGlobalDag_Cache, WarmSecondPassServedFromCache) {
  alpha::SubtreeCache cache{std::size_t{1} << 30};
  par::DetPool pool{4};
  par::GlobalDagStats cold{};
  auto first = par::global_dag_evaluate(union_prog(), panel(), pool, &cache, &cold);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(cold.cache_hits, 0U);
  const alpha::CacheStats after_first = cache.stats();
  par::GlobalDagStats warm{};
  auto second = par::global_dag_evaluate(union_prog(), panel(), pool, &cache, &warm);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(par::signal_set_digest(first.value()), par::signal_set_digest(second.value()));
  const alpha::CacheStats after_second = cache.stats();
  const atx::u64 hits = after_second.hits - after_first.hits;
  const atx::u64 misses = after_second.misses - after_first.misses;
  ASSERT_GT(hits + misses, 0U);
  EXPECT_GE(100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses), 90.0);
  EXPECT_LT(warm.computed, cold.computed / 4U);
}

// Regression for "publish roots last": under a byte budget that holds little more
// than the root values, the roots must survive the cold pass (they are the MRU
// entries), so the warm pass is served by root hits that prune every cone.
TEST(ParallelGlobalDag_Cache, TightBudgetKeepsRootsWarm) {
  const std::size_t panel_bytes = panel().cells() * sizeof(atx::f64);
  const std::size_t roots = union_prog().roots.size();
  alpha::SubtreeCache cache{(roots + 2U) * panel_bytes};
  par::DetPool pool{4};
  auto first = par::global_dag_evaluate(union_prog(), panel(), pool, &cache);
  ASSERT_TRUE(first.has_value());
  ASSERT_GT(cache.stats().evictions, 0U) << "budget must actually be tight";
  const alpha::CacheStats before = cache.stats();
  par::GlobalDagStats warm{};
  auto second = par::global_dag_evaluate(union_prog(), panel(), pool, &cache, &warm);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(par::signal_set_digest(first.value()), par::signal_set_digest(second.value()));
  const alpha::CacheStats after = cache.stats();
  const atx::u64 hits = after.hits - before.hits;
  const atx::u64 misses = after.misses - before.misses;
  ASSERT_GT(hits + misses, 0U);
  EXPECT_GE(100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses), 90.0);
  EXPECT_EQ(warm.computed, 0U);
}

TEST(ParallelGlobalDag_Fused, FusedUnionIdentical) {
  auto fp = alpha::fuse(union_prog());
  ASSERT_TRUE(fp.has_value());
  alpha::Engine serial{panel()};
  const atx::u64 want = par::signal_set_digest(serial.evaluate(union_prog()).value());
  par::DetPool pool{4};
  auto got = par::global_dag_evaluate(fp.value(), panel(), pool);
  ASSERT_TRUE(got.has_value()) << got.error().message();
  EXPECT_EQ(want, par::signal_set_digest(got.value()));
}

TEST(ParallelGlobalDag_Errors, UnknownFieldIsAnError) {
  auto prog = alpha::compile_batch(std::vector<std::string_view>{"rank(no_such_field)"}, lib());
  ASSERT_TRUE(prog.has_value());
  par::DetPool pool{2};
  EXPECT_FALSE(par::global_dag_evaluate(prog.value(), panel(), pool).has_value());
}

} // namespace atx_test_l2_vmcache_global_dag
