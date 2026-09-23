// Lane 2 — SubtreeCache, structural subtree hashing and Engine::evaluate_nodes.
//
// Contract under test: the cache is an LRU under a byte budget; keys isolate by
// panel digest and eval mode; a cached evaluation is BYTE-identical to a fresh one;
// a warm second pass over the same workload hits >= 90%; evaluate_nodes over a root
// subset equals the corresponding alphas of a full evaluate.

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/alpha/wq101_battery.hpp"

namespace atx_test_l2_vmcache_subtree_cache {

namespace alpha = atx::engine::alpha;
using alpha::Engine;
using alpha::EvalMode;
using alpha::Panel;
using alpha::PanelBuf;
using alpha::Program;
using alpha::SignalSet;
using alpha::SubtreeCache;
using alpha::SubtreeKey;

[[nodiscard]] const alpha::Library &lib() {
  static const alpha::Library l;
  return l;
}

[[nodiscard]] Program compile_srcs(const std::vector<std::string_view> &srcs) {
  auto p = alpha::compile_batch(srcs, lib());
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Program{});
}

[[nodiscard]] Panel small_panel(std::uint64_t seed = 0x5eed) {
  auto p = alpha::make_wq101_panel(300, 12, seed);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return std::move(p).value();
}

[[nodiscard]] bool bytes_equal(const std::vector<atx::f64> &a, const std::vector<atx::f64> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(atx::f64)) == 0);
}

void expect_same(const SignalSet &a, const SignalSet &b) {
  ASSERT_EQ(a.alphas.size(), b.alphas.size());
  for (atx::usize k = 0; k < a.alphas.size(); ++k) {
    EXPECT_EQ(a.alphas[k].name, b.alphas[k].name);
    EXPECT_TRUE(bytes_equal(a.alphas[k].values, b.alphas[k].values)) << "alpha " << k;
  }
}

[[nodiscard]] SubtreeKey key(atx::u64 lo, atx::u64 digest = 1) {
  return SubtreeKey{alpha::SubtreeHash{lo, lo * 3U}, digest, EvalMode::AuditExact};
}

// ---------------------------------------------------------------------------
//  Container behaviour
// ---------------------------------------------------------------------------

TEST(SubtreeCache_Lru, BudgetEvictsLeastRecentlyUsed) {
  SubtreeCache c{3 * 100 * sizeof(atx::f64)}; // room for three 100-cell buffers
  c.publish(key(1), PanelBuf(100, 1.0));
  c.publish(key(2), PanelBuf(100, 2.0));
  c.publish(key(3), PanelBuf(100, 3.0));
  ASSERT_NE(c.find(key(1)), nullptr); // touch 1 -> 2 is now LRU
  c.publish(key(4), PanelBuf(100, 4.0));
  EXPECT_TRUE(c.contains(key(1)));
  EXPECT_FALSE(c.contains(key(2)));
  EXPECT_TRUE(c.contains(key(3)));
  EXPECT_TRUE(c.contains(key(4)));
  const alpha::CacheStats s = c.stats();
  EXPECT_EQ(s.evictions, 1U);
  EXPECT_EQ(s.entries, 3U);
  EXPECT_EQ(s.bytes, 3 * 100 * sizeof(atx::f64));
}

TEST(SubtreeCache_Lru, OversizeAndDuplicateAreRejected) {
  SubtreeCache c{10 * sizeof(atx::f64)};
  c.publish(key(1), PanelBuf(11, 1.0));
  EXPECT_FALSE(c.contains(key(1)));
  c.publish(key(2), PanelBuf(4, 2.0));
  c.publish(key(2), PanelBuf(4, 9.0)); // duplicate: first value kept
  const auto v = c.find(key(2));
  ASSERT_NE(v, nullptr);
  EXPECT_EQ((*v)[0], 2.0);
  EXPECT_EQ(c.stats().rejected, 2U);
}

TEST(SubtreeCache_Lru, EvictedValueStaysAliveForHolder) {
  SubtreeCache c{4 * sizeof(atx::f64)};
  c.publish(key(1), PanelBuf(4, 7.0));
  const auto held = c.find(key(1));
  c.publish(key(2), PanelBuf(4, 8.0)); // evicts key 1
  EXPECT_FALSE(c.contains(key(1)));
  ASSERT_NE(held, nullptr);
  EXPECT_EQ((*held)[3], 7.0);
}

TEST(SubtreeCache_Key, IsolatesPanelDigestAndMode) {
  SubtreeCache c{1 << 20};
  c.publish(key(5, 111), PanelBuf(2, 1.0));
  EXPECT_FALSE(c.contains(key(5, 222)));
  SubtreeKey fast = key(5, 111);
  fast.mode = EvalMode::ResearchFast;
  EXPECT_FALSE(c.contains(fast));
  EXPECT_TRUE(c.contains(key(5, 111)));
}

// ---------------------------------------------------------------------------
//  Hashing
// ---------------------------------------------------------------------------

TEST(SubtreeCache_Hash, SameSubtreeAcrossProgramsHashesEqual) {
  // `rank(ts_mean(close, 5))` sits at different positions / slot numbers / field
  // dictionary ids in the two programs; its hash must not care.
  const Program a = compile_srcs({"rank(ts_mean(close, 5))"});
  const Program b = compile_srcs({"open + rank(ts_mean(close, 5)) * 2"});
  const auto ha = alpha::subtree_hashes(a);
  const auto hb = alpha::subtree_hashes(b);
  auto find_rank = [](const Program &p, const std::vector<alpha::SubtreeHash> &h) {
    for (atx::usize i = 0; i < p.code.size(); ++i) {
      if (p.code[i].op == alpha::OpCode::CsRank) {
        return h[i];
      }
    }
    return alpha::SubtreeHash{};
  };
  EXPECT_EQ(find_rank(a, ha), find_rank(b, hb));
  EXPECT_NE(find_rank(a, ha), alpha::SubtreeHash{});
}

TEST(SubtreeCache_Hash, DifferentFieldOrWindowHashesDiffer) {
  const auto h1 = alpha::subtree_hashes(compile_srcs({"ts_mean(close, 5)"}));
  const auto h2 = alpha::subtree_hashes(compile_srcs({"ts_mean(open, 5)"}));
  const auto h3 = alpha::subtree_hashes(compile_srcs({"ts_mean(close, 6)"}));
  auto last_compute = [](const Program &p, const std::vector<alpha::SubtreeHash> &h) {
    alpha::SubtreeHash r{};
    for (atx::usize i = 0; i < p.code.size(); ++i) {
      if (p.code[i].op == alpha::OpCode::TsMean) {
        r = h[i];
      }
    }
    return r;
  };
  const auto a = last_compute(compile_srcs({"ts_mean(close, 5)"}), h1);
  const auto b = last_compute(compile_srcs({"ts_mean(open, 5)"}), h2);
  const auto c = last_compute(compile_srcs({"ts_mean(close, 6)"}), h3);
  EXPECT_NE(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(b, c);
}

TEST(SubtreeCache_Hash, PanelDigestSeesOneBit) {
  const Panel p1 = small_panel(1);
  const Panel p2 = small_panel(1);
  const Panel p3 = small_panel(2);
  EXPECT_EQ(alpha::panel_content_digest(p1), alpha::panel_content_digest(p2));
  EXPECT_NE(alpha::panel_content_digest(p1), alpha::panel_content_digest(p3));
}

// ---------------------------------------------------------------------------
//  Engine integration
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string_view> battery() {
  const auto all = alpha::wq101_alphas();
  return {all.begin(), all.end()};
}

TEST(SubtreeCache_Engine, CachedEqualsFreshByteForByte) {
  const Panel panel = small_panel();
  SubtreeCache cache{std::size_t{512} << 20};
  for (const std::string_view src : battery()) {
    const Program prog = compile_srcs({src});
    Engine fresh{panel};
    auto want = fresh.evaluate(prog);
    ASSERT_TRUE(want.has_value()) << src << ": " << want.error().message();
    Engine cold{panel};
    auto got_cold = cold.evaluate(prog, &cache); // populates
    ASSERT_TRUE(got_cold.has_value()) << src;
    Engine warm{panel};
    auto got_warm = warm.evaluate(prog, &cache); // served from the cache
    ASSERT_TRUE(got_warm.has_value()) << src;
    expect_same(want.value(), got_cold.value());
    expect_same(want.value(), got_warm.value());
  }
  EXPECT_GT(cache.stats().hits, 0U);
}

TEST(SubtreeCache_Engine, WarmSecondPassHitsAtLeast90Pct) {
  const Panel panel = small_panel();
  SubtreeCache cache{std::size_t{1} << 30};
  std::vector<Program> progs;
  for (const std::string_view src : battery()) {
    progs.push_back(compile_srcs({src}));
  }
  Engine eng{panel};
  for (const Program &p : progs) {
    ASSERT_TRUE(eng.evaluate(p, &cache).has_value());
  }
  const alpha::CacheStats first = cache.stats();
  for (const Program &p : progs) {
    ASSERT_TRUE(eng.evaluate(p, &cache).has_value());
  }
  const alpha::CacheStats second = cache.stats();
  const atx::u64 hits = second.hits - first.hits;
  const atx::u64 misses = second.misses - first.misses;
  ASSERT_GT(hits + misses, 0U);
  const double pct = 100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses);
  EXPECT_GE(pct, 90.0);
  // The first pass already shares subtrees ACROSS alphas (cross-program CSE).
  EXPECT_GT(first.hits, 0U);
}

TEST(SubtreeCache_Engine, KeyIsolationAcrossPanels) {
  const Panel pa = small_panel(11);
  const Panel pb = small_panel(22);
  SubtreeCache cache{std::size_t{256} << 20};
  const Program prog = compile_srcs({"rank(ts_std(close, 10)) - rank(delta(volume, 3))"});
  Engine ea{pa};
  Engine eb{pb};
  ASSERT_TRUE(ea.evaluate(prog, &cache).has_value());
  auto gb = eb.evaluate(prog, &cache); // must NOT reuse panel A's values
  ASSERT_TRUE(gb.has_value());
  Engine fresh_b{pb};
  expect_same(fresh_b.evaluate(prog).value(), gb.value());
}

TEST(SubtreeCache_Engine, NullCacheIsPlainEvaluate) {
  const Panel panel = small_panel();
  const Program prog = compile_srcs(battery());
  Engine e1{panel};
  Engine e2{panel};
  expect_same(e1.evaluate(prog).value(), e2.evaluate(prog, nullptr).value());
}

TEST(AlphaVmNodes_Subset, SubsetEqualsFullEvaluateRoots) {
  const Panel panel = small_panel();
  const Program prog = compile_srcs(battery());
  Engine full_eng{panel};
  const SignalSet full = full_eng.evaluate(prog).value();
  const std::vector<atx::u32> pick = {7, 3, 42, 0};
  Engine eng{panel};
  auto sub = eng.evaluate_nodes(prog, pick, nullptr);
  ASSERT_TRUE(sub.has_value()) << sub.error().message();
  ASSERT_EQ(sub.value().alphas.size(), pick.size());
  for (atx::usize k = 0; k < pick.size(); ++k) {
    EXPECT_EQ(sub.value().alphas[k].name, full.alphas[pick[k]].name);
    EXPECT_TRUE(bytes_equal(sub.value().alphas[k].values, full.alphas[pick[k]].values));
  }
  // evaluate_root with a warm cache.
  SubtreeCache cache{std::size_t{256} << 20};
  for (int pass = 0; pass < 2; ++pass) {
    auto r = eng.evaluate_root(prog, 42, &cache);
    ASSERT_TRUE(r.has_value());
    const std::vector<atx::f64> got(r.value().begin(), r.value().end());
    EXPECT_TRUE(bytes_equal(got, full.alphas[42].values)) << "pass " << pass;
  }
}

TEST(AlphaVmNodes_Subset, RejectsBadRootIndices) {
  const Panel panel = small_panel();
  const Program prog = compile_srcs({"close", "open"});
  Engine eng{panel};
  const std::vector<atx::u32> oob = {2};
  EXPECT_FALSE(eng.evaluate_nodes(prog, oob, nullptr).has_value());
  const std::vector<atx::u32> dup = {1, 1};
  EXPECT_FALSE(eng.evaluate_nodes(prog, dup, nullptr).has_value());
  const std::vector<atx::u32> ok = {1};
  EXPECT_TRUE(eng.evaluate_nodes(prog, ok, nullptr).has_value()); // engine still usable
}

} // namespace atx_test_l2_vmcache_subtree_cache
