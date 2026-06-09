// atx::engine::alpha — unparse(Ast) round-trip soundness (S4b-1, suite
// AlphaUnparse).
//
// A discovered alpha is only useful if its formula is re-parseable. The S3
// genome is an `alpha::Ast`; unparse(Ast)->string renders it back to DSL text.
// The LOAD-BEARING contract is NOT textual prettiness — it is round-trip
// through the canonical key: parse(unparse(ast)) must yield the SAME
// canonical_hash as the original. The hash equality (not string equality)
// drives the exact textual form: operand order, window slot, const precision.
//
// Fixture op names are the REAL registry spellings: infix `+ - *` are Binary
// nodes (NOT `add`/`sub`/`mul` functions — those do not exist), `rank` is the
// cross-sectional op (NOT `cs_rank`). See registry.hpp's builtin table.

#include <string>

#include <gtest/gtest.h>

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "atx/engine/factory/canonical.hpp"

namespace {
using namespace atx::engine;

// Parse `src`, unparse the root, re-parse, and assert the two canonical hashes
// match. The default-constructed Library IS the builtins catalogue (registry.hpp
// has no `Library::builtins()` factory — its ctor registers Appendix A).
void expect_round_trips(const std::string &src) {
  alpha::Library lib;
  auto a0 = alpha::parse_expr(src, lib);
  ASSERT_TRUE(a0.has_value()) << "fixture must parse: " << src;
  const alpha::ExprId r0 = a0->roots().front().root;
  const std::string text = alpha::unparse(*a0, r0);
  auto a1 = alpha::parse_expr(text, lib);
  ASSERT_TRUE(a1.has_value()) << "unparse must re-parse: " << text;
  const alpha::ExprId r1 = a1->roots().front().root;
  EXPECT_EQ(factory::canonical_hash(*a0, r0), factory::canonical_hash(*a1, r1))
      << "round-trip changed the canonical key\n src=" << src << "\n txt=" << text;
}

TEST(AlphaUnparse, RoundTripsLeafField) { expect_round_trips("close"); }
TEST(AlphaUnparse, RoundTripsBinaryOp) { expect_round_trips("close + open"); }
TEST(AlphaUnparse, RoundTripsConstAndWindow) { expect_round_trips("ts_mean(close, 8)"); }
TEST(AlphaUnparse, RoundTripsFractionalConst) { expect_round_trips("decay_linear(close, 8.22237)"); }
TEST(AlphaUnparse, RoundTripsNested) {
  expect_round_trips("ts_mean(close + open * 0.5, 5)");
}
TEST(AlphaUnparse, RoundTripsRankAndCs) { expect_round_trips("rank(ts_sum(close, 10))"); }

// fail-on-bad: non-commutative operand order matters (Sub is NOT hash-commutative,
// so a-b and b-a must hash differently — and unparse must preserve that order).
TEST(AlphaUnparse, WrongOrderFlipsHash) {
  alpha::Library lib;
  auto a = alpha::parse_expr("close - open", lib);
  ASSERT_TRUE(a.has_value());
  auto b = alpha::parse_expr("open - close", lib);
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(factory::canonical_hash(*a, a->roots().front().root),
            factory::canonical_hash(*b, b->roots().front().root));
}

// The two unparse overloads agree on a single-root Ast.
TEST(AlphaUnparse, AnonRootOverloadMatches) {
  alpha::Library lib;
  auto a = alpha::parse_expr("ts_mean(close, 8)", lib);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(alpha::unparse(*a), alpha::unparse(*a, a->roots().front().root));
}
} // namespace
