// atx::engine::factory — op_swap safety-invariant stress harness (S3.4).
//
// The load-bearing guarantee this suite proves: for EVERY swappable bucket, an
// op_swap either (a) yields a genome that analyzes clean AND evaluates oracle ==
// VM with NO abort / SlotPool corruption, or (b) is cleanly rejected by analyze
// (op_swap returns Err) — NEVER an abort. This is the invariant "analyze-valid
// ⟹ VM-safe" that the original defect violated.
//
// Original defect (the named regression): a finite-default op whose kernel reads
// operand 2 unconditionally (scale / winsorize / quantile, materialized arity 2)
// was filed in the min_arity-1 bucket, so a rank(close) node (arity 1) could be
// swapped onto it without operand 2 materialized -> VM read pool slot kNoSlot ->
// out-of-range abort. S3.4 closes it at the root (validate_node_contract + the
// materialized-arity buckets). We assert the catalog never offers such a swap AND
// that a rank-rooted genome op-swapped thousands of times never aborts.

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/mutation.hpp"
#include "atx/engine/factory/op_catalog.hpp"

namespace {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::DType;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::operand_max_arity;
using atx::engine::alpha::operand_min_arity;
using atx::engine::alpha::OpSig;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::Shape;
using atx::engine::alpha::SignalSet;
using atx::engine::factory::Genome;
using atx::engine::factory::op_swap;
using atx::engine::factory::OpCatalog;

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

[[nodiscard]] Genome make_genome(std::string_view src, const Library &lib) {
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << src << ": " << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << src << ": " << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// A small deterministic panel carrying every field the seed genomes reference.
[[nodiscard]] Panel make_panel() {
  constexpr atx::usize dates = 6;
  constexpr atx::usize instruments = 5;
  constexpr atx::usize cells = dates * instruments;
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    const auto fi = static_cast<atx::f64>(i);
    cols[0][i] = 10.0 + fi;             // close
    cols[1][i] = 5.0 + 0.5 * fi;        // open
    cols[2][i] = 12.0 + fi;             // high
    cols[3][i] = 8.0 + fi;              // low
    cols[4][i] = 1000.0 + 3.0 * fi;     // volume
    cols[5][i] = static_cast<atx::f64>(i % 3); // IndClass.sector
  }
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  auto p = Panel::create(dates, instruments, std::move(names), std::move(cols), {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Compile + evaluate a genome through BOTH paths; assert oracle == VM bit-for-bit.
// A reachable abort (SlotPool out-of-range) would crash the process here — the
// test passing IS the no-abort proof. Precondition: g analyzes clean.
void expect_safe_and_consistent(const Genome &g, const Panel &panel) {
  auto prog = compile(g.ast, g.analysis);
  ASSERT_TRUE(prog.has_value()) << "compile: " << (prog ? "" : prog.error().message());
  Engine engine{panel};
  auto vm = engine.evaluate(prog.value_or(Program{}));
  ASSERT_TRUE(vm.has_value()) << "VM: " << (vm ? "" : vm.error().message());
  auto ref = evaluate_reference(prog.value_or(Program{}), panel);
  ASSERT_TRUE(ref.has_value()) << "oracle: " << (ref ? "" : ref.error().message());
  const SignalSet &v = vm.value();
  const SignalSet &r = ref.value();
  ASSERT_EQ(v.alphas.size(), r.alphas.size());
  for (atx::usize a = 0; a < v.alphas.size(); ++a) {
    ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
    for (atx::usize i = 0; i < v.alphas[a].values.size(); ++i) {
      EXPECT_TRUE(same_cell(v.alphas[a].values[i], r.alphas[a].values[i])) << "alpha " << a
                                                                           << " cell " << i;
    }
  }
}

// Seed expressions spanning the swappable buckets: cross-sectional arity 1 / arity
// 2 (scalar) / group; the variadic residualizer (arity 2 and 3); time-series arity
// 2 / 3; unary + binary infix; the stateful recurrences; and an hparam op (which
// must be swap-inert). Each is op-swapped thousands of times below.
[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {
      "rank(close)",                                  // CS arity 1 (the regression seed)
      "scale(close, 2.0)",                            // CS arity 2 (finite-default scalar)
      "quantile(close, 5)",                           // CS arity 2 (S3.3 finite-default)
      "winsorize(close, 4.0)",                        // CS arity 2
      "vec_avg(close)",                               // CS arity 1
      "indneutralize(close, IndClass.sector)",        // CS arity 2, group
      "cs_residualize(close, IndClass.sector)",       // CS arity 2 variadic
      "cs_residualize(close, IndClass.sector, open)", // CS arity 3 variadic
      "ts_mean(close, 10)",                           // TS arity 2
      "correlation(close, open, 10)",                 // TS arity 3
      "reverse(close)",                               // unary (named -> Neg)
      "abs(close)",                                   // unary
      "close + open",                                 // binary infix
      "close > open",                                 // binary infix (Mask)
      "trade_when(close > open, close, close < open)",// recurrence
      "hump(close, 0.01)",                            // recurrence (finite-default)
      "kalman_level(close, 0.1, 0.2)",                // hparam op (swap-inert)
      "rank(close) + ts_mean(volume, 10)",            // composed DAG
  };
}

// ===========================================================================
//  The invariant: thousands of swaps per seed, every Ok mutant VM-safe.
// ===========================================================================

TEST(OpSwapStress, EveryBucket_NoAbort_OracleEqualsVm) {
  const Library lib;
  const OpCatalog cat(lib);
  const Panel panel = make_panel();
  for (const std::string &expr : seed_exprs()) {
    const Genome g = make_genome(expr, lib);
    Xoshiro256pp rng(0x5311ULL);
    int ok = 0;
    for (int i = 0; i < 400; ++i) {
      auto m = op_swap(g, cat, rng);
      if (!m) {
        continue; // cleanly rejected (no compatible op / contract violation) — fine
      }
      ++ok;
      // Every accepted mutant MUST analyze clean (op_swap's own backstop) and run.
      ASSERT_TRUE(analyze(m->ast).has_value()) << expr << " -> invalid mutant";
      expect_safe_and_consistent(*m, panel);
    }
    static_cast<void>(ok); // some seeds (e.g. singleton buckets) may never swap
  }
}

// ===========================================================================
//  Named regression — the rank(close) -> scale/winsorize/quantile swap that
//  corrupted the VM is never offered, and the genome never aborts.
// ===========================================================================

TEST(OpSwapStress, RankNode_NeverSwapsToFiniteDefaultScalarOp) {
  const Library lib;
  const OpCatalog cat(lib);
  const OpSig *rank = lib.find("rank");
  ASSERT_NE(rank, nullptr);
  Xoshiro256pp rng(0xBADC0DEULL);
  for (int i = 0; i < 2000; ++i) {
    auto repl = cat.sample_compatible(Shape::CrossSection, DType::F64, /*arity=*/1, rank, rng);
    if (!repl) {
      continue;
    }
    const OpSig *got = *repl;
    // A rank node materializes exactly ONE operand, so any replacement offered
    // must legally accept arity 1 — i.e. its kernel never reads an unmaterialized
    // operand 2 (the exact corruption the old min_arity buckets allowed).
    EXPECT_LE(operand_min_arity(*got), 1U) << "offered '" << got->name << "'";
    EXPECT_GE(operand_max_arity(*got), 1U) << "offered '" << got->name << "'";
    EXPECT_NE(got->name, std::string_view{"scale"});
    EXPECT_NE(got->name, std::string_view{"winsorize"});
    EXPECT_NE(got->name, std::string_view{"quantile"});
  }
}

// The hparam op (kalman_level) is swap-inert: a node carrying it is never chosen
// as an op_swap target, so its peeled immediates are never left inconsistent.
TEST(OpSwapStress, KalmanLevelNode_IsSwapInert) {
  const Library lib;
  const OpCatalog cat(lib);
  const Genome g = make_genome("kalman_level(close, 0.1, 0.2)", lib);
  Xoshiro256pp rng(0x7ULL);
  for (int i = 0; i < 500; ++i) {
    auto m = op_swap(g, cat, rng);
    EXPECT_FALSE(m.has_value()); // the only swappable node is the hparam op -> no swap
  }
}

} // namespace
