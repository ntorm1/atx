// atx::engine::alpha — P3c-1 batch / cross-alpha evaluation + CSE-metric proof.
//
// Batch evaluation ALREADY EXISTS structurally: parse_program parses N named
// assignments into one Ast; analyze + compile fold them into ONE hash-consed DAG
// (cross-alpha CSE falls out for free); and Engine::evaluate returns a SignalSet
// holding one alpha per root. P3c-1 adds (a) `compile_batch(span<string_view>)`,
// a thin convenience over that pipeline, and (b) the intern cache-hit telemetry
// beside the pre-existing unique/total-node ratio. This suite proves:
//
//   1. batch == singly — compiling+evaluating N alphas as ONE Program yields
//      cell-for-cell (NaN==NaN) the SAME values as compiling+evaluating each
//      alpha ALONE. Batching (and its cross-alpha CSE) does not change results.
//   2. order-independence (determinism) — the same N alphas submitted in two
//      different orders produce an identical hash AFTER sorting alphas by name.
//   3. CSE metric — `unique_nodes < total_ast_nodes` (and cache_hits>0) on a
//      shared-subexpression battery; boundaries: batch-of-1, fully-disjoint
//      (unique≈total, cache_hits==0), fully-shared/identical (heavy dedup).
//
// Naming: Subject_Condition_ExpectedResult.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace {

using atx::core::hash_combine;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::compile_batch;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// Process-lifetime Library: the Ast borrows OpSig pointers from it, so it must
// outlive every parse result.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN or exactly value-equal (covers ±inf, ±0). The VM
// is deterministic, so equality — not a tolerance — is the right bar.
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// ---- pipeline helpers (mirror alpha_proof_test.cpp) ------------------------

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] Program compile_batch_ok(const std::vector<std::string_view> &srcs) {
  auto prog = compile_batch(std::span<const std::string_view>{srcs}, shared_lib());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] SignalSet eval_ok(const Program &prog, const Panel &panel) {
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  return out.value_or(SignalSet{});
}

// ---- synthetic panel (a trimmed mirror of the proof suite's generator) -----

[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments, std::uint64_t seed) {
  const atx::usize cells = dates * instruments;
  std::vector<std::string> names = {"open", "high", "low", "close", "volume", "vwap", "returns"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(cells));

  // A cheap, fully-deterministic LCG (no <random>) — fixed seed, never clocked.
  std::uint64_t state = seed | 1ULL;
  auto next = [&state]() noexcept -> atx::f64 {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(state >> 11) / static_cast<atx::f64>(1ULL << 53);
  };
  for (atx::usize i = 0; i < cells; ++i) {
    const atx::f64 base = 10.0 + next() * 190.0;
    const atx::f64 spread = next() * 5.0;
    const atx::f64 hi = base + spread;
    const atx::f64 lo = base - spread;
    cols[0][i] = base;                         // open
    cols[1][i] = hi;                           // high
    cols[2][i] = lo;                           // low
    cols[3][i] = lo + (hi - lo) * 0.5;         // close
    cols[4][i] = 1.0e4 + next() * 9.9e5;       // volume
    cols[5][i] = (hi + lo + cols[3][i]) / 3.0; // vwap
    cols[6][i] = next() * 0.1 - 0.05;          // returns
  }

  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  // Delist a couple of instruments mid-sample (no survivorship; reads as NaN).
  if (instruments >= 3 && dates >= 4) {
    const atx::usize delist_date = dates * 2 / 3;
    for (atx::usize d = delist_date; d < dates; ++d) {
      universe[d * instruments + 1] = 0;
      universe[d * instruments + (instruments - 1)] = 0;
    }
  }
  // Scatter a few NaN source cells (data gaps) so NaN==NaN paths are exercised.
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  if (cells >= 8) {
    cols[3][cells / 5] = nan;
    cols[4][cells / 3] = nan;
    cols[3][cells * 4 / 5] = nan;
  }

  auto p = Panel::create(dates, instruments, names, cols, universe);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// signal_hash — canonical digest of a SignalSet over the ORDERED cell stream
// (alpha_index, date, instrument, bits(value)). A NaN cell folds a fixed
// sentinel so the hash is canonical w.r.t. the SAME NaN-equivalence `same_cell`
// uses. Mirrors alpha_proof_test.cpp::signal_hash. NOTE: this folds in the
// PROVIDED alpha order, so callers that compare across submission orders must
// canonicalize (sort by name) first.
[[nodiscard]] std::size_t signal_hash(const SignalSet &ss) noexcept {
  constexpr std::uint64_t kNanSentinel = 0x7FF8'0000'0000'0001ULL;
  std::size_t seed = 0;
  seed = hash_combine(seed, static_cast<std::size_t>(ss.alphas.size()),
                      static_cast<std::size_t>(ss.dates), static_cast<std::size_t>(ss.instruments));
  for (atx::usize a = 0; a < ss.alphas.size(); ++a) {
    const std::vector<atx::f64> &v = ss.alphas[a].values;
    for (atx::usize d = 0; d < ss.dates; ++d) {
      for (atx::usize inst = 0; inst < ss.instruments; ++inst) {
        const atx::usize idx = d * ss.instruments + inst;
        const atx::f64 cell = idx < v.size() ? v[idx] : std::numeric_limits<atx::f64>::quiet_NaN();
        const std::uint64_t bits =
            std::isnan(cell) ? kNanSentinel : std::bit_cast<std::uint64_t>(cell);
        seed = hash_combine(seed, static_cast<std::size_t>(a), static_cast<std::size_t>(d),
                            static_cast<std::size_t>(inst), static_cast<std::size_t>(bits));
      }
    }
  }
  return seed;
}

// Reorder a SignalSet's alphas into ascending name order (a stable canonical
// view), so a hash over it is independent of submission order. Returns a copy.
[[nodiscard]] SignalSet sorted_by_name(const SignalSet &ss) {
  SignalSet out = ss;
  std::sort(out.alphas.begin(), out.alphas.end(),
            [](const SignalSet::Alpha &x, const SignalSet::Alpha &y) { return x.name < y.name; });
  return out;
}

// ===========================================================================
//  1. batch == singly — the differential that proves batching is value-neutral.
// ===========================================================================

TEST(AlphaBatch_BatchEqualsSingly, NAlphas_BatchMatchesPerAlphaEval_CellForCell) {
  const atx::usize dates = 24;
  const atx::usize instruments = 12;
  const Panel panel = make_panel(dates, instruments, 0xBA7C001ULL);

  // A battery with real cross-alpha overlap (rank(close), ts_mean(close,5), …)
  // AND distinct families (element-wise / cross-section / time-series).
  const std::vector<std::string_view> srcs = {
      "rank(close) + 1",
      "rank(close) * 2",
      "ts_mean(close, 5) - close",
      "correlation(close, volume, 10)",
      "rank(close) - ts_mean(close, 5)",
      "(close > open) ? rank(volume) : -rank(volume)",
  };

  const Program batch = compile_batch_ok(srcs);
  ASSERT_EQ(batch.roots.size(), srcs.size());
  const SignalSet batch_out = eval_ok(batch, panel);
  ASSERT_EQ(batch_out.alphas.size(), srcs.size());

  for (atx::usize i = 0; i < srcs.size(); ++i) {
    // compile_batch names each entry `a<i>`; roots preserve submission order.
    const std::vector<std::string_view> one = {srcs[i]};
    const Program single = compile_batch_ok(one);
    ASSERT_EQ(single.roots.size(), 1U);
    const SignalSet single_out = eval_ok(single, panel);
    ASSERT_EQ(single_out.alphas.size(), 1U);

    const std::vector<atx::f64> &bv = batch_out.alphas[i].values;
    const std::vector<atx::f64> &sv = single_out.alphas[0].values;
    ASSERT_EQ(bv.size(), sv.size()) << "alpha " << i << " value-vector size mismatch";
    for (atx::usize c = 0; c < bv.size(); ++c) {
      EXPECT_TRUE(same_cell(bv[c], sv[c]))
          << "alpha " << i << " cell " << c << " (date " << c / instruments << ", inst "
          << c % instruments << "): batch=" << bv[c] << " single=" << sv[c];
    }
  }
}

// ===========================================================================
//  2. order-independence (determinism) — submission order does not change the
//     result once alphas are canonicalized by name.
// ===========================================================================

TEST(AlphaBatch_OrderIndependence, TwoSubmissionOrders_SameHashAfterSortByName) {
  const atx::usize dates = 20;
  const atx::usize instruments = 10;
  const Panel panel = make_panel(dates, instruments, 0x0DDE12ULL);

  // Same FIVE alphas, two different submission orders. compile_batch names them
  // by their POSITION (a0,a1,…), so we hand-author named programs here to make
  // the by-name canonicalization meaningful (a fixed name per expression across
  // both orders), then evaluate both via the existing pipeline.
  const std::string_view order_a = "x_rank = rank(close)\n"
                                   "y_mean = ts_mean(close, 5)\n"
                                   "z_corr = correlation(close, volume, 10)\n"
                                   "w_delt = delta(close, 2)\n"
                                   "v_comb = rank(close) - ts_mean(close, 5)\n";
  const std::string_view order_b = "v_comb = rank(close) - ts_mean(close, 5)\n"
                                   "z_corr = correlation(close, volume, 10)\n"
                                   "x_rank = rank(close)\n"
                                   "w_delt = delta(close, 2)\n"
                                   "y_mean = ts_mean(close, 5)\n";

  const Program prog_a = compile_ok(order_a);
  const Program prog_b = compile_ok(order_b);
  ASSERT_EQ(prog_a.roots.size(), 5U);
  ASSERT_EQ(prog_b.roots.size(), 5U);

  const SignalSet out_a = eval_ok(prog_a, panel);
  const SignalSet out_b = eval_ok(prog_b, panel);

  // Raw (submission-order) hashes differ — the orders genuinely differ.
  EXPECT_NE(signal_hash(out_a), signal_hash(out_b))
      << "the two submission orders must NOT be incidentally identical";

  // After canonicalizing by alpha name, the digests are identical: the result is
  // a function of the alpha SET, not the submission order.
  EXPECT_EQ(signal_hash(sorted_by_name(out_a)), signal_hash(sorted_by_name(out_b)))
      << "result depends on submission order — batch evaluation is not order-independent";

  // The CSE metric is itself order-independent (same hash-consed graph either way).
  EXPECT_EQ(prog_a.unique_nodes, prog_b.unique_nodes);
  EXPECT_EQ(prog_a.total_ast_nodes, prog_b.total_ast_nodes);
}

// ===========================================================================
//  3a. CSE metric — overlap ⇒ unique < total and cache_hits > 0.
// ===========================================================================

TEST(AlphaBatch_CseMetric, SharedSubexpression_DedupHappens_CacheHitsNonZero) {
  // Both alphas share `rank(close)`.
  const std::vector<std::string_view> srcs = {"rank(close) + 1", "rank(close) * 2"};
  const Program prog = compile_batch_ok(srcs);

  EXPECT_LT(prog.unique_nodes, prog.total_ast_nodes)
      << "shared subexpression did not deduplicate (unique=" << prog.unique_nodes
      << " total=" << prog.total_ast_nodes << ")";
  EXPECT_GT(prog.cache_hits, 0U) << "no intern cache hit recorded on an overlapping battery";

  // Invariant: every intern attempt either hit or appended exactly one node.
  EXPECT_EQ(prog.intern_attempts, prog.cache_hits + prog.unique_nodes);
  EXPECT_GT(prog.cache_hit_pct(), 0.0);
}

// ===========================================================================
//  3b. boundary — batch of 1 equals a single eval and has no cross-alpha dedup.
// ===========================================================================

TEST(AlphaBatch_CseMetric, BatchOfOne_EqualsSingleEval_NoCrossAlphaHits) {
  const atx::usize dates = 12;
  const atx::usize instruments = 8;
  const Panel panel = make_panel(dates, instruments, 0x0FF1CEULL);

  const std::vector<std::string_view> one = {"rank(close) - open"};
  const Program batch = compile_batch_ok(one);
  ASSERT_EQ(batch.roots.size(), 1U);

  const Program direct = compile_ok("a0 = rank(close) - open\n");
  ASSERT_EQ(direct.roots.size(), 1U);

  // Same graph as the directly-compiled single alpha.
  EXPECT_EQ(batch.unique_nodes, direct.unique_nodes);
  EXPECT_EQ(batch.total_ast_nodes, direct.total_ast_nodes);

  const SignalSet bo = eval_ok(batch, panel);
  const SignalSet d = eval_ok(direct, panel);
  ASSERT_EQ(bo.alphas.size(), 1U);
  ASSERT_EQ(d.alphas.size(), 1U);
  ASSERT_EQ(bo.alphas[0].values.size(), d.alphas[0].values.size());
  for (atx::usize c = 0; c < bo.alphas[0].values.size(); ++c) {
    EXPECT_TRUE(same_cell(bo.alphas[0].values[c], d.alphas[0].values[c])) << "cell " << c;
  }
}

// ===========================================================================
//  3c. boundary — fully-disjoint alphas: ratio ≈ 1, no cross-alpha hits.
//
//  Two alphas over DIFFERENT fields with no shared subtree. The only possible
//  hits are intra-alpha (none here), so unique == total and cache_hits == 0.
// ===========================================================================

TEST(AlphaBatch_CseMetric, FullyDisjointAlphas_UniqueEqualsTotal_NoHits) {
  const std::vector<std::string_view> srcs = {"open + high", "low * volume"};
  const Program prog = compile_batch_ok(srcs);

  EXPECT_EQ(prog.unique_nodes, prog.total_ast_nodes)
      << "disjoint alphas should not deduplicate any node";
  EXPECT_EQ(prog.cache_hits, 0U) << "disjoint alphas must record zero cache hits";
}

// ===========================================================================
//  3d. boundary — fully-shared (identical) alphas: heavy dedup, ratio small.
//
//  N copies of the SAME expression collapse to ONE graph; every copy after the
//  first is entirely cache hits. unique stays the single-alpha node count.
// ===========================================================================

TEST(AlphaBatch_CseMetric, IdenticalAlphas_CollapseToOneGraph_HeavyDedup) {
  const std::string_view expr = "rank(close) - ts_mean(close, 5)";
  const std::vector<std::string_view> many = {expr, expr, expr, expr};
  const Program prog = compile_batch_ok(many);
  ASSERT_EQ(prog.roots.size(), 4U);

  const Program single = compile_batch_ok({expr});

  // Four identical alphas intern no MORE unique nodes than one (the StoreAlpha
  // +1 refcounts differ, but the node set is the same hash-consed graph).
  EXPECT_EQ(prog.unique_nodes, single.unique_nodes)
      << "identical alphas should not grow the unique-node set";
  EXPECT_LT(prog.unique_nodes, prog.total_ast_nodes);
  // ~3/4 of the lowered nodes are dedup'd away (3 of the 4 copies are all hits).
  EXPECT_GT(prog.cache_hit_pct(), 50.0)
      << "four identical alphas should record a >50% cache-hit rate";
  EXPECT_EQ(prog.intern_attempts, prog.cache_hits + prog.unique_nodes);
}

// ===========================================================================
//  4. malformed source in a batch ⇒ Err, never a throw.
// ===========================================================================

TEST(AlphaBatch_Errors, MalformedSource_PropagatesErr_NoThrow) {
  const std::vector<std::string_view> srcs = {"rank(close)", "close + + "};
  auto prog = compile_batch(std::span<const std::string_view>{srcs}, shared_lib());
  EXPECT_FALSE(prog.has_value()) << "a malformed batch entry must surface as Err";
}

TEST(AlphaBatch_Errors, EmptyBatch_YieldsZeroRootProgram) {
  const std::vector<std::string_view> none;
  auto prog = compile_batch(std::span<const std::string_view>{none}, shared_lib());
  ASSERT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  EXPECT_EQ(prog.value().roots.size(), 0U);
}

} // namespace
