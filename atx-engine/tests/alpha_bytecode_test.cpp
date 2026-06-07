// atx::engine::alpha — bytecode linearization unit tests (P3-4).
//
// Covers the plan's bytecode list:
//   * topo order: every Instr's src slots are produced by an earlier Instr.
//   * slot reuse: peak-live-slots for a deep chain is small (<< node count).
//   * Free once: every non-root slot is Freed exactly once; no double-free; no
//     read of a slot after its Free (no use-after-free).
//   * StoreAlpha per root with the right output index; src is the root's slot.
//   * strength reduction lowers to a Mul with both src equal; no Pow/live Const.
//   * required_lookback propagates onto the Program.
//
// Naming: Subject_Condition_ExpectedResult.

#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Instr;
using atx::engine::alpha::kNoSlot;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SlotId;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Parse + analyze + compile, ASSERTing each stage succeeds.
[[nodiscard]] Program build(std::string_view src) {
  auto parsed = parse_program(src, shared_lib());
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Program{};
  }
  auto analysis = analyze(*parsed);
  EXPECT_TRUE(analysis.has_value()) << (analysis ? "" : analysis.error().message());
  if (!analysis) {
    return Program{};
  }
  auto prog = compile(*parsed, *analysis);
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  if (!prog) {
    return Program{};
  }
  return std::move(*prog);
}

[[nodiscard]] int count_op(const Program &prog, OpCode op) {
  int n = 0;
  for (const Instr &i : prog.code) {
    if (i.op == op) {
      ++n;
    }
  }
  return n;
}

// ---- topological well-formedness --------------------------------------------

TEST(AlphaBytecode_Topo, EverySrcSlotProducedByEarlierInstr) {
  const Program prog = build("a = ts_mean(close, 5) + rank(open) * 2\nb = close - open");
  // Walk in order; a slot is "produced" by any non-Free instr writing dst, and
  // "consumed-as-source" by any later instr. Assert no src is read before its
  // most-recent producer wrote it.
  std::vector<bool> produced(prog.num_slots, false);
  for (const Instr &i : prog.code) {
    for (const SlotId s : i.src) {
      if (s != kNoSlot) {
        EXPECT_TRUE(produced[s]) << "src slot " << s << " read before produced";
      }
    }
    if (i.op == OpCode::Free) {
      // After Free the slot is no longer live; a re-acquire will re-produce it.
      produced[i.dst] = false;
    } else if (i.op != OpCode::StoreAlpha && i.dst != kNoSlot) {
      produced[i.dst] = true;
    }
  }
}

// ---- slot reuse -------------------------------------------------------------

TEST(AlphaBytecode_SlotReuse, DeepChain_PeakSlotsSmall) {
  // A purely linear chain: each value dies as soon as its single parent reads
  // it, so the allocator recycles down to a tiny peak (<< node count).
  const Program prog = build("a = ts_mean(ts_mean(ts_mean(close, 3), 3), 3)");
  // Nodes: close, 3, ts_mean, ts_mean, ts_mean = 5 live nodes. Peak should be
  // far smaller than the node count, and certainly <= node count.
  EXPECT_LE(prog.num_slots, prog.unique_nodes);
  EXPECT_LE(prog.num_slots, 3U); // a tiny working set recycles through the chain
}

TEST(AlphaBytecode_SlotReuse, PeakNeverExceedsNodeCount) {
  const Program prog = build("a = ts_mean(close, 5) + rank(open) * 2\nb = close - open");
  EXPECT_LE(prog.num_slots, prog.unique_nodes);
  EXPECT_EQ(prog.num_slots, prog.peak_live_slots);
}

// ---- Free discipline --------------------------------------------------------

TEST(AlphaBytecode_Free, NeverFreedTwiceWithoutReacquire_NoDoubleFree) {
  // A slot is legitimately reused (acquire -> Free -> acquire -> Free ...), so
  // counting total frees per slot is not the invariant. The real "no double
  // free" property is: a slot is never Freed while it is already free (i.e. two
  // Frees with no intervening producing instr that re-acquires it).
  const Program prog = build("a = ts_mean(close, 5) + rank(open) * 2\nb = close - open");
  std::vector<bool> live(prog.num_slots, false);
  for (const Instr &i : prog.code) {
    if (i.op == OpCode::Free) {
      EXPECT_TRUE(live[i.dst]) << "double free: slot " << i.dst << " freed while not live";
      live[i.dst] = false;
    } else if (i.op != OpCode::StoreAlpha && i.dst != kNoSlot) {
      live[i.dst] = true;
    }
  }
}

TEST(AlphaBytecode_Free, NoSlotReadAfterItsFree_NoUseAfterFree) {
  // Track liveness: a slot is live from its producing instr until its Free; no
  // instr may read a slot that is currently freed (and not yet re-acquired).
  const Program prog = build("a = ts_mean(close, 5) + rank(open) * 2\nb = close - open");
  std::vector<bool> live(prog.num_slots, false);
  for (const Instr &i : prog.code) {
    for (const SlotId s : i.src) {
      if (s != kNoSlot) {
        EXPECT_TRUE(live[s]) << "use-after-free: read slot " << s << " while not live";
      }
    }
    if (i.op == OpCode::Free) {
      live[i.dst] = false;
    } else if (i.op != OpCode::StoreAlpha && i.dst != kNoSlot) {
      live[i.dst] = true;
    }
  }
}

TEST(AlphaBytecode_Free, EveryProducedSlotEventuallyFreed) {
  // At program end no slot should remain live: every acquire is matched by a
  // Free (a root is stored then freed; interiors freed at last read).
  const Program prog = build("m = ts_mean(close, 5)\na = m + 1\nb = m * 2");
  std::vector<int> acquired(prog.num_slots, 0);
  std::vector<int> freed(prog.num_slots, 0);
  for (const Instr &i : prog.code) {
    if (i.op == OpCode::Free) {
      ++freed[i.dst];
    } else if (i.op != OpCode::StoreAlpha && i.dst != kNoSlot) {
      ++acquired[i.dst];
    }
  }
  for (SlotId s = 0; s < prog.num_slots; ++s) {
    EXPECT_EQ(acquired[s], freed[s]) << "slot " << s << " acquire/free imbalance";
  }
}

// ---- StoreAlpha -------------------------------------------------------------

TEST(AlphaBytecode_Store, OneStorePerRoot_WithSequentialOutputIndices) {
  const Program prog = build("a = ts_mean(close, 5)\nb = rank(open)");
  EXPECT_EQ(count_op(prog, OpCode::StoreAlpha), 2);
  ASSERT_EQ(prog.roots.size(), 2U);
  EXPECT_EQ(prog.roots[0].name, "a");
  EXPECT_EQ(prog.roots[0].output, 0U);
  EXPECT_EQ(prog.roots[1].name, "b");
  EXPECT_EQ(prog.roots[1].output, 1U);
}

TEST(AlphaBytecode_Store, TwoIdenticalAlphas_EmitOneStorePerRoot) {
  // Two alphas with byte-identical expressions CSE to ONE node, but each root
  // must still get its own StoreAlpha (and the node's slot is freed only after
  // both stores). Regression: a single shared store would drop output 0 and leak
  // the slot (remaining stuck > 0).
  const Program prog = build("a = rank(close)\nb = rank(close)");
  EXPECT_EQ(count_op(prog, OpCode::CsRank), 1);     // shared via CSE
  EXPECT_EQ(count_op(prog, OpCode::StoreAlpha), 2); // one store per root
  ASSERT_EQ(prog.roots.size(), 2U);
  EXPECT_EQ(prog.roots[0].output, 0U);
  EXPECT_EQ(prog.roots[1].output, 1U);
  // Both output indices are actually written by a StoreAlpha.
  std::vector<bool> written(prog.roots.size(), false);
  for (const Instr &i : prog.code) {
    if (i.op == OpCode::StoreAlpha) {
      ASSERT_LT(i.param, written.size());
      written[i.param] = true;
    }
  }
  EXPECT_TRUE(written[0] && written[1]);
  // Two slots are freed: the shared `close` leaf (after CsRank reads it) and the
  // shared CsRank slot (after BOTH stores). No leak (peak == 2), no double-free.
  EXPECT_EQ(count_op(prog, OpCode::Free), 2);
  EXPECT_EQ(prog.num_slots, 2U);
}

TEST(AlphaBytecode_Store, StoreReadsItsRootSlot) {
  const Program prog = build("a = rank(close)");
  // The StoreAlpha's src[0] must be the slot the CsRank wrote.
  SlotId rank_slot = kNoSlot;
  bool checked = false;
  for (const Instr &i : prog.code) {
    if (i.op == OpCode::CsRank) {
      rank_slot = i.dst;
    }
    if (i.op == OpCode::StoreAlpha) {
      EXPECT_EQ(i.src[0], rank_slot);
      EXPECT_EQ(i.param, 0U);
      checked = true;
    }
  }
  EXPECT_TRUE(checked);
}

TEST(AlphaBytecode_Store, RootThatIsAlsoInteriorChild_StoredOnceFreedAfterAllConsumers) {
  // `ts_mean(close,5)` is alpha a's root target AND (via CSE) b's Mul operand.
  // It must be Stored exactly once, and its slot must stay live until BOTH the
  // store and the interior Mul read have run, then be Freed exactly once.
  const Program prog = build("a = ts_mean(close, 5)\nb = ts_mean(close, 5) * 2");
  EXPECT_EQ(count_op(prog, OpCode::TsMean), 1);
  EXPECT_EQ(count_op(prog, OpCode::StoreAlpha), 2);

  // Locate the TsMean instr + its slot; verify its store reads that slot, the
  // Mul reads it too, and it is freed once, only after both reads.
  SlotId apex_slot = kNoSlot;
  int apex_reads = 0; // store(a) + Mul edge = 2 reads
  int apex_frees = 0;
  bool freed_yet = false;
  for (const Instr &i : prog.code) {
    if (i.op == OpCode::TsMean) {
      apex_slot = i.dst;
    }
    if (apex_slot != kNoSlot) {
      for (const SlotId s : i.src) {
        if (s == apex_slot) {
          ++apex_reads;
          EXPECT_FALSE(freed_yet) << "read of apex slot after it was freed";
        }
      }
      if (i.op == OpCode::Free && i.dst == apex_slot) {
        ++apex_frees;
        freed_yet = true;
      }
    }
  }
  EXPECT_EQ(apex_reads, 2); // StoreAlpha(a) + Mul(b)
  EXPECT_EQ(apex_frees, 1);
}

// ---- strength reduction in the linearized program --------------------------

TEST(AlphaBytecode_StrengthReduce, NoPowAndNoLiveConstTwo) {
  const Program prog = build("a = close ^ 2");
  EXPECT_EQ(count_op(prog, OpCode::Pow), 0);
  EXPECT_EQ(count_op(prog, OpCode::Mul), 1);
  // The dead Const(2) was refcount-0 -> never emitted, so no Const instr at all.
  EXPECT_EQ(count_op(prog, OpCode::Const), 0);
}

TEST(AlphaBytecode_StrengthReduce, MulHasBothSrcEqual) {
  const Program prog = build("a = close ^ 2");
  bool checked = false;
  for (const Instr &i : prog.code) {
    if (i.op == OpCode::Mul) {
      EXPECT_EQ(i.src[0], i.src[1]);
      EXPECT_NE(i.src[0], kNoSlot);
      checked = true;
    }
  }
  EXPECT_TRUE(checked);
}

// ---- metrics / boundary -----------------------------------------------------

TEST(AlphaBytecode_Lookback, PropagatesRequiredLookback) {
  const Program prog = build("a = ts_mean(close, 10)");
  EXPECT_EQ(prog.required_lookback, 9U);
}

TEST(AlphaBytecode_Boundary, SingleScalarAlpha_OneSlotStoredAndFreed) {
  // 2*3+1 folds to a single Const scalar; it is stored then freed -> 1 slot.
  const Program prog = build("a = 2 * 3 + 1");
  EXPECT_EQ(count_op(prog, OpCode::Const), 1);
  EXPECT_EQ(count_op(prog, OpCode::StoreAlpha), 1);
  EXPECT_EQ(count_op(prog, OpCode::Free), 1);
  EXPECT_EQ(prog.num_slots, 1U);
}

TEST(AlphaBytecode_Const, ImmediateValueCarriedOnInstr) {
  const Program prog = build("a = close + 7");
  bool found = false;
  for (const Instr &i : prog.code) {
    if (i.op == OpCode::Const) {
      EXPECT_EQ(i.imm[0], 7.0);
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(AlphaBytecode_Cse, SharedNode_EmittedOnce) {
  // ts_mean(close,5) shared -> exactly one TsMean instr in the linearized code.
  const Program prog = build("a = ts_mean(close, 5) + 1\nb = ts_mean(close, 5) * 2");
  EXPECT_EQ(count_op(prog, OpCode::TsMean), 1);
  EXPECT_LT(prog.unique_nodes, prog.total_ast_nodes);
}

// ---- measured peak-live-slots metric (sprint-ledger regression guard) -------

TEST(AlphaBytecode_Metric, DeepChain_PeakLiveSlotsIsThree) {
  // ts_mean(ts_mean(ts_mean(close,3),3),3): 5 unique nodes recycle through a
  // working set of 3 live slots (the value, its window const, and the result).
  const Program prog = build("a = ts_mean(ts_mean(ts_mean(close, 3), 3), 3)");
  EXPECT_EQ(prog.unique_nodes, 5U);
  EXPECT_EQ(prog.peak_live_slots, 3U);
  EXPECT_EQ(prog.num_slots, prog.peak_live_slots);
}

TEST(AlphaBytecode_Metric, SharedSubtree_PeakLiveSlotsIsThree) {
  // The 7-unique-node dedup graph (the shared ts_mean(close,5) computed once)
  // linearizes with a working set of just 3 live slots — the shared leaf and the
  // shared sub-result are freed as soon as their last consumer runs.
  const Program prog = build("a = ts_mean(close, 5) + 1\nb = ts_mean(close, 5) * 2");
  EXPECT_EQ(prog.unique_nodes, 7U);
  EXPECT_EQ(prog.peak_live_slots, 3U);
}

} // namespace
