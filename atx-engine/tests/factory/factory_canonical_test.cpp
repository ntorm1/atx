// atx::engine::factory — canonical-hash dedup tests (S3-2, suite FactoryCanonical).
//
// The F6 soundness + discrimination proof (plan §4.4 / §0.5):
//   * CommutativeReorderHashesEqual   — add(x,y) and add(y,x) hash equal
//   * NonCommutativeOrderHashesDiffer — sub(x,y) and sub(y,x) hash differ
//   * DifferentWindowHashesDiffer     — ts_mean(close,10) != ts_mean(close,11)
//   * HashEqualImpliesBitIdenticalEval— the LOAD-BEARING soundness proof: two
//     hash-equal genomes evaluate BIT-IDENTICAL through the real VM (Add case)
//   * MinMaxCommuteIsBitIdentical     — the directed NaN-soundness check: a
//     non-Add commutative op (min/max) operand-swap stays bit-identical over a
//     panel that contains NaNs (if it did NOT, the op would have to be removed
//     from the hash-commutative set — soundness > dedup rate)
//   * StableAcrossRebuild             — clone() yields an identical hash (the
//     stable-key / field-by-NAME property)
//
// Local helpers mirror factory_genome_test (the engine exposes no make_genome /
// make_panel / eval helper): the test-only add/sub/greater aliases let the plan's
// call syntax parse, and a direct parse_expr -> analyze -> compile -> Engine path
// runs the real VM for the soundness checks.

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/factory/canonical.hpp"
#include "atx/engine/factory/genome.hpp"

namespace atxtest_factory_canonical_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::Ast;
using atx::engine::alpha::compile;
using atx::engine::alpha::DType;
using atx::engine::alpha::Engine;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::OpSig;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::shape_elementwise;
using atx::engine::alpha::SignalSet;
using atx::engine::factory::canonical_hash;
using atx::engine::factory::CanonSet;
using atx::engine::factory::Genome;

// ---- test-only op aliases (so the plan's call syntax resolves) --------------

inline void register_aliases(Library &lib) {
  const OpSig add{"add", 2, 2, OpCode::Add, DType::F64, true, {}, &shape_elementwise};
  const OpSig sub{"sub", 2, 2, OpCode::Sub, DType::F64, true, {}, &shape_elementwise};
  const OpSig mul{"mul", 2, 2, OpCode::Mul, DType::F64, true, {}, &shape_elementwise};
  const OpSig greater{"greater", 2, 2, OpCode::CmpGt, DType::Mask, true, {}, &shape_elementwise};
  static_cast<void>(lib.register_op(add));
  static_cast<void>(lib.register_op(sub));
  static_cast<void>(lib.register_op(mul));
  static_cast<void>(lib.register_op(greater));
}

[[nodiscard]] Genome make_genome(std::string_view src, Library &lib) {
  register_aliases(lib);
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// The genome's single root id (the expression entry point for canonical_hash).
[[nodiscard]] ExprId root(const Genome &g) { return g.ast.roots().front().root; }

// Build a dates x instruments Panel from a list of named, date-major columns.
[[nodiscard]] Panel make_panel(std::size_t dates, std::size_t insts,
                               const std::vector<std::string> &fields,
                               const std::vector<std::vector<atx::f64>> &cols) {
  auto r = Panel::create(dates, insts, fields, cols, {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// Evaluate genome `g`'s first (only) alpha root through the REAL VM and return
// its raw date-major value vector (NaN preserved). ASSERT-free: returns empty on
// any pipeline failure so the caller's size check surfaces the error.
[[nodiscard]] std::vector<atx::f64> eval_alpha0(const Genome &g, const Panel &panel) {
  auto prog = compile(g.ast, g.analysis);
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  if (!prog) {
    return {};
  }
  Engine engine{panel};
  auto out = engine.evaluate(prog.value());
  EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message());
  if (!out || out->alphas.empty()) {
    return {};
  }
  return out->alphas.front().values;
}

// A panel with close + volume + open columns over `dates` x `insts`; cell value
// is a simple separable form so the alphas have something non-degenerate to eat.
[[nodiscard]] Panel make_ohlc_panel(std::size_t dates, std::size_t insts) {
  const std::size_t cells = dates * insts;
  std::vector<atx::f64> close(cells), volume(cells), open(cells), high(cells);
  for (std::size_t d = 0; d < dates; ++d) {
    for (std::size_t n = 0; n < insts; ++n) {
      const std::size_t i = d * insts + n;
      const auto dd = static_cast<atx::f64>(d);
      const auto nn = static_cast<atx::f64>(n);
      close[i] = 100.0 + 10.0 * nn + dd;
      volume[i] = 1000.0 + 100.0 * nn + 10.0 * dd;
      open[i] = 99.0 + 10.0 * nn + dd;
      high[i] = 102.0 + 10.0 * nn + dd;
    }
  }
  return make_panel(dates, insts, {"close", "volume", "open", "high"},
                    {close, volume, open, high});
}

// A 1-date, 2-instrument panel whose two fields "za"/"zb" hold an OPPOSITE-signed-
// zero pair in SWAPPED positions: za = {-0.0, +0.0}, zb = {+0.0, -0.0}. This is
// the minimal fixture that exposes the min/max swap-asymmetry (`-0.0 < +0.0` is
// false ⇒ the kernel returns the second operand, so the signbit depends on order).
[[nodiscard]] Panel make_signed_zero_panel() {
  const atx::f64 pz = 0.0;
  const atx::f64 nz = -0.0;
  const std::vector<atx::f64> za{nz, pz};
  const std::vector<atx::f64> zb{pz, nz};
  return make_panel(1, 2, {"za", "zb"}, {za, zb});
}

// ---- tests ------------------------------------------------------------------

TEST(FactoryCanonical, CommutativeReorderHashesEqual) {
  Library lib;
  auto x = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  auto y = make_genome("add(ts_mean(volume, 10), rank(close))", lib); // operands swapped
  EXPECT_EQ(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
}

TEST(FactoryCanonical, NonCommutativeOrderHashesDiffer) {
  Library lib;
  auto x = make_genome("sub(rank(close), rank(high))", lib);
  auto y = make_genome("sub(rank(high), rank(close))", lib); // sub is NOT commutative
  EXPECT_NE(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
}

TEST(FactoryCanonical, DifferentWindowHashesDiffer) {
  Library lib;
  auto x = make_genome("ts_mean(close, 10)", lib);
  auto y = make_genome("ts_mean(close, 11)", lib);
  EXPECT_NE(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
}

TEST(FactoryCanonical, HashEqualImpliesBitIdenticalEval) { // soundness, the load-bearing one
  Library lib;
  Panel panel = make_ohlc_panel(24, 4);
  auto x = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  auto y = make_genome("add(ts_mean(volume, 10), rank(close))", lib);
  ASSERT_EQ(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
  auto vx = eval_alpha0(x, panel);
  auto vy = eval_alpha0(y, panel);
  ASSERT_EQ(vx.size(), vy.size());
  ASSERT_GT(vx.size(), 0U);
  for (std::size_t i = 0; i < vx.size(); ++i) {
    EXPECT_TRUE((std::isnan(vx[i]) && std::isnan(vy[i])) || vx[i] == vy[i]); // bit-for-bit
  }
}

// F6 soundness, the MinP/MaxP counterexample (the recon fix to cf30730). The VM
// kernels are `a<b?a:b` / `a>b?a:b`, so on an OPPOSITE-signed-zero pair a min/max
// operand swap is NOT bit-safe: `min(-0.0,+0.0)=+0.0` but `min(+0.0,-0.0)=-0.0`
// (signbit differs). This test PROVES the bit-difference through the real VM
// (non-vacuously — it asserts a signbit divergence actually occurred) AND proves
// the fix: canonical_hash now hashes the two orderings DIFFERENTLY (min/max are
// excluded from is_hash_commutative), so no equal-hash claim is made for a
// bit-different pair. If min/max were ever re-added to the hash-commutative set,
// the hash-EQ assertion below would fire — guarding the soundness invariant.
TEST(FactoryCanonical, MinMaxSwapIsNotBitSafe) {
  Library lib;
  Panel panel = make_signed_zero_panel();
  for (const char *op : {"min", "max"}) {
    auto x = make_genome(std::string{op} + "(za, zb)", lib);
    auto y = make_genome(std::string{op} + "(zb, za)", lib); // operands swapped

    // The fix: the hash must DISTINGUISH the two orderings (min/max not commuted).
    EXPECT_NE(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)))
        << op << ": min/max must NOT be hashed commutative (signed-zero asymmetry)";

    // The hazard the fix guards: the VM really does produce a bit-different vector.
    auto vx = eval_alpha0(x, panel);
    auto vy = eval_alpha0(y, panel);
    ASSERT_EQ(vx.size(), vy.size());
    ASSERT_GT(vx.size(), 0U);
    bool saw_signbit_diff = false;
    for (std::size_t i = 0; i < vx.size(); ++i) {
      // Same magnitude (both are zero here) but the sign bit flips with the order.
      EXPECT_FALSE(std::isnan(vx[i])) << op << " cell " << i;
      EXPECT_EQ(vx[i], 0.0) << op << " cell " << i; // ±0 compares == 0.0
      saw_signbit_diff = saw_signbit_diff || (std::signbit(vx[i]) != std::signbit(vy[i]));
    }
    EXPECT_TRUE(saw_signbit_diff)
        << op << ": fixture must actually exercise the signed-zero swap divergence";
  }
}

// The retained set stays SOUND: a commutative op KEPT in is_hash_commutative (Add)
// over the SAME opposite-signed-zero panel still hashes-equal AND evaluates
// bit-identical under swap (`-0.0 + +0.0 == +0.0 + -0.0 == +0.0`, both orders).
TEST(FactoryCanonical, RetainedCommutativeStaysSoundOnSignedZero) {
  Library lib;
  Panel panel = make_signed_zero_panel();
  auto x = make_genome("add(za, zb)", lib);
  auto y = make_genome("add(zb, za)", lib); // operands swapped
  ASSERT_EQ(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
  auto vx = eval_alpha0(x, panel);
  auto vy = eval_alpha0(y, panel);
  ASSERT_EQ(vx.size(), vy.size());
  ASSERT_GT(vx.size(), 0U);
  for (std::size_t i = 0; i < vx.size(); ++i) {
    EXPECT_TRUE((std::isnan(vx[i]) && std::isnan(vy[i])) || vx[i] == vy[i]) << "cell " << i;
    EXPECT_EQ(std::signbit(vx[i]), std::signbit(vy[i])) << "cell " << i; // ±0 sign matches too
  }
}

TEST(FactoryCanonical, StableAcrossRebuild) { // stable key (cross-run intent): clone -> same hash
  Library lib;
  auto x = make_genome("mul(close, add(open, high))", lib);
  EXPECT_EQ(canonical_hash(x.ast, root(x)), canonical_hash(x.clone().ast, root(x)));
}

TEST(FactoryCanonical, CanonSetDedupsOnHit) {
  Library lib;
  auto x = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  auto y = make_genome("add(ts_mean(volume, 10), rank(close))", lib); // canon-equal
  auto z = make_genome("sub(rank(close), rank(high))", lib);          // distinct
  CanonSet seen;
  EXPECT_TRUE(seen.insert(canonical_hash(x.ast, root(x))));   // first sighting
  EXPECT_FALSE(seen.insert(canonical_hash(y.ast, root(y))));  // commutative dup -> hit
  EXPECT_TRUE(seen.contains(canonical_hash(y.ast, root(y)))); // membership holds
  EXPECT_TRUE(seen.insert(canonical_hash(z.ast, root(z))));   // genuinely different
}


}  // namespace atxtest_factory_canonical_test
