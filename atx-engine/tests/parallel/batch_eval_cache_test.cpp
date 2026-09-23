// Lane 2 — strategy A (parallel_evaluate) with a shared SubtreeCache: byte-identical
// to the cache-free path for every worker count, and a repeated batch is served
// almost entirely from the cache (the GP re-evaluation case the search driver hits).

#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/wq101_battery.hpp"
#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/digest.hpp"

namespace atx_test_l2_vmcache_batch_eval_cache {

namespace alpha = atx::engine::alpha;
namespace par = atx::engine::parallel;

[[nodiscard]] const alpha::Library &lib() {
  static const alpha::Library l;
  return l;
}

[[nodiscard]] const alpha::Panel &panel() {
  static const alpha::Panel p = alpha::make_wq101_panel(260, 15).value();
  return p;
}

[[nodiscard]] std::vector<alpha::Program> per_alpha_programs() {
  std::vector<alpha::Program> progs;
  for (const std::string_view s : alpha::wq101_alphas()) {
    progs.push_back(alpha::compile_batch(std::vector<std::string_view>{s}, lib()).value());
  }
  return progs;
}

TEST(ParallelBatchEvalCache_Identity, CachedEqualsUncachedForWorkerCounts) {
  const std::vector<alpha::Program> progs = per_alpha_programs();
  par::DetPool p1{1};
  const atx::u64 want = par::signal_set_digest(par::parallel_evaluate(progs, panel(), p1).value());
  for (const atx::usize w : {1U, 3U, 8U}) {
    par::DetPool pool{w};
    alpha::SubtreeCache cache{std::size_t{512} << 20};
    auto cold = par::parallel_evaluate(progs, panel(), pool, &cache);
    ASSERT_TRUE(cold.has_value());
    EXPECT_EQ(want, par::signal_set_digest(cold.value())) << "cold, workers=" << w;
    auto warm = par::parallel_evaluate(progs, panel(), pool, &cache);
    ASSERT_TRUE(warm.has_value());
    EXPECT_EQ(want, par::signal_set_digest(warm.value())) << "warm, workers=" << w;
  }
}

TEST(ParallelBatchEvalCache_Hits, RepeatedBatchIsServedFromCache) {
  const std::vector<alpha::Program> progs = per_alpha_programs();
  par::DetPool pool{4};
  alpha::SubtreeCache cache{std::size_t{512} << 20};
  ASSERT_TRUE(par::parallel_evaluate(progs, panel(), pool, &cache).has_value());
  const alpha::CacheStats a = cache.stats();
  ASSERT_TRUE(par::parallel_evaluate(progs, panel(), pool, &cache).has_value());
  const alpha::CacheStats b = cache.stats();
  const double hits = static_cast<double>(b.hits - a.hits);
  const double misses = static_cast<double>(b.misses - a.misses);
  ASSERT_GT(hits + misses, 0.0);
  EXPECT_GE(100.0 * hits / (hits + misses), 90.0);
}

TEST(ParallelBatchEvalCache_Null, NullCacheIsTheOriginalPath) {
  const std::vector<alpha::Program> progs = per_alpha_programs();
  par::DetPool pool{2};
  EXPECT_EQ(par::signal_set_digest(par::parallel_evaluate(progs, panel(), pool).value()),
            par::signal_set_digest(par::parallel_evaluate(progs, panel(), pool, nullptr).value()));
}

} // namespace atx_test_l2_vmcache_batch_eval_cache
