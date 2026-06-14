// atx::engine::alpha — hash-consed DAG unit tests (P3-4).
//
// Covers the plan's DAG list:
//   * CSE / dedup: identical sub-expressions intern to one node; shared whole
//     subtrees across alphas collapse; unique_nodes() < total_ast_nodes().
//   * refcount: a shared node's refcount equals its consumer count (edges +
//     one per root store); Mul(x,x) counts x twice.
//   * strength reduction: pow(x,2) / x^2 lower to Mul(x,x) (no Pow node, the
//     dropped Const(2) stays unreferenced).
//   * boundary: single alpha (unique == total); fully-shared alphas; a diamond
//     apex with the right refcount, interned once.
//
// Naming: Subject_Condition_ExpectedResult.

#include <string_view>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/dag.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atxtest_alpha_dag_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::Ast;
using atx::engine::alpha::build_dag;
using atx::engine::alpha::Dag;
using atx::engine::alpha::kNoNode;
using atx::engine::alpha::Library;
using atx::engine::alpha::Node;
using atx::engine::alpha::NodeId;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::parse_program;

// A process-lifetime Library (must outlive any Ast that borrows its OpSig rows).
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Parse + analyze + build_dag, ASSERTing each stage succeeds.
[[nodiscard]] Dag build(std::string_view src) {
  auto parsed = parse_program(src, shared_lib());
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Dag{};
  }
  auto analysis = analyze(*parsed);
  EXPECT_TRUE(analysis.has_value()) << (analysis ? "" : analysis.error().message());
  if (!analysis) {
    return Dag{};
  }
  auto dag = build_dag(*parsed, *analysis);
  EXPECT_TRUE(dag.has_value()) << (dag ? "" : dag.error().message());
  if (!dag) {
    return Dag{};
  }
  return std::move(*dag);
}

// Count nodes carrying a given opcode.
[[nodiscard]] int count_op(const Dag &dag, OpCode op) {
  int n = 0;
  for (const Node &node : dag.nodes()) {
    if (node.op == op) {
      ++n;
    }
  }
  return n;
}

// ---- CSE / dedup ------------------------------------------------------------

TEST(AlphaDag_Cse, RepeatedRank_InternsOneNode) {
  // rank(close) appears in two alphas (once bare, once under +1) -> ONE CsRank.
  const Dag dag = build("a = rank(close)\nb = rank(close) + 1");
  EXPECT_EQ(count_op(dag, OpCode::CsRank), 1);
  EXPECT_LT(dag.unique_nodes(), dag.total_ast_nodes());
}

TEST(AlphaDag_Cse, SharedSubtreeAcrossAlphas_Collapses) {
  // ts_mean(close,5) is identical in both alphas -> a single TsMean node.
  const Dag dag = build("a = ts_mean(close, 5) + 1\nb = ts_mean(close, 5) * 2");
  EXPECT_EQ(count_op(dag, OpCode::TsMean), 1);
  EXPECT_LT(dag.unique_nodes(), dag.total_ast_nodes());
}

TEST(AlphaDag_Cse, IdenticalConstants_Dedup) {
  // The literal 5 used twice interns once (bit-cast key).
  const Dag dag = build("a = close + 5\nb = open + 5");
  EXPECT_EQ(count_op(dag, OpCode::Const), 1);
}

TEST(AlphaDag_Cse, DistinctConstants_DoNotDedup) {
  const Dag dag = build("a = close + 5\nb = open + 6");
  EXPECT_EQ(count_op(dag, OpCode::Const), 2);
}

TEST(AlphaDag_Cse, DollarSigil_IsSameFieldAsBare) {
  // $close and close must intern to the SAME LoadField (sigil ignored).
  const Dag dag = build("a = $close + 1\nb = close + 2");
  EXPECT_EQ(count_op(dag, OpCode::LoadField), 1);
  EXPECT_EQ(dag.fields().size(), 1U);
}

// ---- refcount ---------------------------------------------------------------

TEST(AlphaDag_Refcount, SharedNode_CountsAllConsumers) {
  // NOTE: the front-end does NOT resolve assignment NAMES back to their defining
  // subtree — `m` in `a = m + 1` parses as a *field* named "m", not a reference
  // to a prior `m = ...`. Sharing therefore arises only from repeated identical
  // *expressions*. Here `rank(close)` is alpha `a`'s root target AND (via CSE) an
  // interior operand of `b`'s Add, so it is both a store consumer and an edge.
  const Dag dag = build("a = rank(close)\nb = rank(close) + 1");
  NodeId rank_id = kNoNode;
  for (NodeId i = 0; i < dag.nodes().size(); ++i) {
    if (dag.node(i).op == OpCode::CsRank) {
      rank_id = i;
    }
  }
  ASSERT_NE(rank_id, kNoNode);
  EXPECT_EQ(count_op(dag, OpCode::CsRank), 1); // CSE collapsed the two ranks
  // Consumers: StoreAlpha(a) + Add edge(b) = 2.
  EXPECT_EQ(dag.node(rank_id).refcount, 2U);
}

TEST(AlphaDag_Refcount, MulOfSameChild_CountsTwice) {
  // close * close: the Mul references close twice -> close refcount >= 2.
  const Dag dag = build("a = close * close");
  NodeId field_id = kNoNode;
  for (NodeId i = 0; i < dag.nodes().size(); ++i) {
    if (dag.node(i).op == OpCode::LoadField) {
      field_id = i;
    }
  }
  ASSERT_NE(field_id, kNoNode);
  EXPECT_EQ(dag.node(field_id).refcount, 2U);
}

// ---- strength reduction -----------------------------------------------------

TEST(AlphaDag_StrengthReduce, PowerOfTwoCall_LowersToMul) {
  // power(close, 2) -> Mul(close, close); no Pow node survives.
  const Dag dag = build("a = power(close, 2)");
  EXPECT_EQ(count_op(dag, OpCode::Pow), 0);
  EXPECT_EQ(count_op(dag, OpCode::Mul), 1);
}

TEST(AlphaDag_StrengthReduce, CaretTwo_LowersToMul) {
  // close ^ 2 -> Mul(close, close).
  const Dag dag = build("a = close ^ 2");
  EXPECT_EQ(count_op(dag, OpCode::Pow), 0);
  EXPECT_EQ(count_op(dag, OpCode::Mul), 1);
}

TEST(AlphaDag_StrengthReduce, MulBothChildrenEqualX) {
  const Dag dag = build("a = close ^ 2");
  NodeId mul_id = kNoNode;
  for (NodeId i = 0; i < dag.nodes().size(); ++i) {
    if (dag.node(i).op == OpCode::Mul) {
      mul_id = i;
    }
  }
  ASSERT_NE(mul_id, kNoNode);
  const Node &mul = dag.node(mul_id);
  EXPECT_EQ(mul.in[0], mul.in[1]);
  EXPECT_NE(mul.in[0], kNoNode);
  // The squared child carries refcount 2 (two Mul edges).
  EXPECT_EQ(dag.node(mul.in[0]).refcount, 2U);
}

TEST(AlphaDag_StrengthReduce, DroppedConstTwo_StaysUnreferenced) {
  // The Const(2) operand of the reduced pow must end up with refcount 0.
  const Dag dag = build("a = close ^ 2");
  bool saw_const = false;
  for (const Node &node : dag.nodes()) {
    if (node.op == OpCode::Const && node.value == 2.0) {
      saw_const = true;
      EXPECT_EQ(node.refcount, 0U);
    }
  }
  // The Const may or may not have been interned at all; if present it is dead.
  EXPECT_TRUE(saw_const || count_op(dag, OpCode::Const) == 0);
}

TEST(AlphaDag_StrengthReduce, PowerOfThree_StaysPow) {
  // Only pow(x,2) is reduced; pow(x,3) stays a Pow.
  const Dag dag = build("a = close ^ 3");
  EXPECT_EQ(count_op(dag, OpCode::Pow), 1);
  EXPECT_EQ(count_op(dag, OpCode::Mul), 0);
}

// ---- boundary ---------------------------------------------------------------

TEST(AlphaDag_Boundary, SingleAlphaNoSharing_UniqueEqualsTotal) {
  // close + open: 3 Ast nodes (close, open, Add), no sharing -> unique == total.
  const Dag dag = build("a = close + open");
  EXPECT_EQ(dag.unique_nodes(), dag.total_ast_nodes());
}

TEST(AlphaDag_Boundary, FullyIdenticalAlphas_ShareEverything) {
  // Two identical alphas -> the second adds nothing; both roots point at one node.
  const Dag dag = build("a = ts_mean(close, 5)\nb = ts_mean(close, 5)");
  EXPECT_EQ(count_op(dag, OpCode::TsMean), 1);
  ASSERT_EQ(dag.roots().size(), 2U);
  EXPECT_EQ(dag.roots()[0].node, dag.roots()[1].node);
}

TEST(AlphaDag_Boundary, Diamond_ApexInternedOnceWithRefcountTwoPlusStore) {
  // A genuine diamond: `ts_mean(close,5)` is alpha `a`'s root target (a store
  // consumer) AND, via CSE, an interior operand of `b`'s Mul (an edge). The apex
  // is interned once with refcount 2. (Names like `m` do not back-reference; see
  // AlphaDag_Refcount.SharedNode_CountsAllConsumers — sharing is by expression.)
  const Dag dag = build("a = ts_mean(close, 5)\nb = ts_mean(close, 5) * 2");
  EXPECT_EQ(count_op(dag, OpCode::TsMean), 1);
  NodeId apex = kNoNode;
  for (NodeId i = 0; i < dag.nodes().size(); ++i) {
    if (dag.node(i).op == OpCode::TsMean) {
      apex = i;
    }
  }
  ASSERT_NE(apex, kNoNode);
  EXPECT_EQ(dag.node(apex).refcount, 2U); // StoreAlpha(a) + Mul edge(b)
}

TEST(AlphaDag_Lookback, PropagatesRequiredLookbackOntoDag) {
  // ts_mean(close,10) -> required lookback 9.
  const Dag dag = build("a = ts_mean(close, 10)");
  EXPECT_EQ(dag.required_lookback(), 9U);
}

TEST(AlphaDag_Const, StoresLiteralValueOnNode) {
  const Dag dag = build("a = close + 7");
  bool found = false;
  for (const Node &node : dag.nodes()) {
    if (node.op == OpCode::Const) {
      EXPECT_EQ(node.value, 7.0);
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// ---- measured CSE metric (sprint-ledger regression guard) -------------------

TEST(AlphaDag_Metric, SharedSubtree_DedupsThreeOfTenNodes) {
  // a = ts_mean(close,5) + 1 ; b = ts_mean(close,5) * 2.
  // Per-alpha Ast nodes: {close,5,ts_mean,1,Add} (5) + {close,5,ts_mean,2,Mul}
  // (5) = 10 total. CSE collapses the shared {close,5,ts_mean} -> 7 unique.
  const Dag dag = build("a = ts_mean(close, 5) + 1\nb = ts_mean(close, 5) * 2");
  EXPECT_EQ(dag.total_ast_nodes(), 10U);
  EXPECT_EQ(dag.unique_nodes(), 7U);
}

// ---- multi-output record ops (B7) -------------------------------------------

TEST(AlphaDag, SharedComputeOnePinPerProjection) {
  // split2(close).hi and split2(close).lo must share ONE compute node (CSE on
  // identical call + hparams) and produce TWO distinct Pin nodes with indices 0
  // and 1 respectively. The compute node must declare n_out == 2.
  Library lib;
  auto ast = parse_program("a = split2(close).hi\nb = split2(close).lo\n", lib);
  ASSERT_TRUE(ast) << (ast ? "" : ast.error().message());
  auto an = analyze(ast.value());
  ASSERT_TRUE(an) << (an ? "" : an.error().message());
  auto dag = build_dag(ast.value(), an.value());
  ASSERT_TRUE(dag) << (dag ? "" : dag.error().message());

  int compute = 0;
  int pins = 0;
  std::array<int, 2> pin_index_seen{0, 0};
  for (const Node &n : dag.value().nodes()) {
    if (n.op == OpCode::Split2) {
      ++compute;
    }
    if (n.op == OpCode::Pin) {
      ++pins;
      if (n.param < 2) {
        ++pin_index_seen[n.param];
      }
    }
  }
  EXPECT_EQ(compute, 1);           // single shared compute (CSE on identical args+hparams)
  EXPECT_EQ(pins, 2);              // hi and lo each get their own Pin node
  EXPECT_EQ(pin_index_seen[0], 1); // exactly one Pin with index 0 (hi)
  EXPECT_EQ(pin_index_seen[1], 1); // exactly one Pin with index 1 (lo)
  // The compute node must declare 2 output pins.
  for (const Node &n : dag.value().nodes()) {
    if (n.op == OpCode::Split2) {
      EXPECT_EQ(n.n_out, 2U);
    }
  }
}


}  // namespace atxtest_alpha_dag_test
