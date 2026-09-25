// W0-A0 — factory side of the alpha kernel lane.
//
//   * FactoryCanonCollision_*        — A-18: CanonSet stores the canonical string
//                                      and compares it on a hash hit, so a 64-bit
//                                      collision no longer reuses another genome.
//   * AlphaTypecheckScalarLiteral_*  — A-03: a 10k-child subtree-crossover stress
//                                      run never produces a non-literal scalar slot
//                                      (scale / winsorize / quantile / hump arg 2).

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/canonical.hpp"
#include "atx/engine/factory/crossover.hpp"
#include "atx/engine/factory/genome.hpp"

namespace atx_test_w0_a0_canon {

using atx::usize;
using atx::core::Xoshiro256pp;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Ast;
using atx::engine::alpha::Expr;
using atx::engine::alpha::kNoExpr;
using atx::engine::alpha::Library;
using atx::engine::alpha::parse_expr;
using atx::engine::factory::CanonSet;
using atx::engine::factory::canonical_hash;
using atx::engine::factory::canonical_string;
using atx::engine::factory::Genome;
using atx::engine::factory::subtree_crossover;

const Library &lib() {
  static const Library l;
  return l;
}

Genome make_genome(std::string_view src) {
  auto parsed = parse_expr(src, lib());
  EXPECT_TRUE(parsed.has_value()) << src << ": " << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto g = atx::engine::factory::analyze_into(std::move(*parsed));
  EXPECT_TRUE(g.has_value()) << src << ": " << (g ? "" : g.error().message());
  return g ? std::move(*g) : Genome{};
}

// ===========================================================================
//  A-18 — FactoryCanonCollision_*
// ===========================================================================

TEST(FactoryCanonCollision_Set, ForcedHashCollisionAdmitsBothStructures) {
  const Genome a = make_genome("rank(close)");
  const Genome b = make_genome("ts_mean(volume, 5)");
  const std::string fa = canonical_string(a);
  const std::string fb = canonical_string(b);
  ASSERT_NE(fa, fb);
  // Force the SAME 64-bit key for two different structures (what an FNV
  // collision looks like to the set).
  constexpr atx::u64 kKey = 0xC011151011ULL;
  CanonSet set;
  EXPECT_TRUE(set.insert(kKey, fa));
  EXPECT_TRUE(set.contains(kKey, fa));
  EXPECT_FALSE(set.contains(kKey, fb)); // a hash hit is NOT a duplicate anymore
  EXPECT_TRUE(set.insert(kKey, fb));    // admitted as fresh — no score reuse
  EXPECT_FALSE(set.insert(kKey, fa));   // the true duplicate is still caught
  EXPECT_FALSE(set.insert(kKey, fb));
  EXPECT_EQ(set.size(), 2U);
  EXPECT_EQ(set.collisions(), 1U);
  // The pre-W0 hash-only view would have merged them.
  CanonSet legacy;
  EXPECT_TRUE(legacy.insert(kKey));
  EXPECT_FALSE(legacy.insert(kKey));
  EXPECT_EQ(legacy.size(), 1U);
}

TEST(FactoryCanonCollision_Set, RealHashesAgreeWithStringsOnACorpus) {
  // Equal strings <=> equal structure mod commutativity; every pair with equal
  // strings must also have equal hashes (the string is the exact form behind
  // the hash), and no two distinct strings in the corpus share a hash.
  const std::vector<std::string_view> corpus = {
      "close + open",      "open + close",        "close - open",       "open - close",
      "close * volume",    "volume * close",      "min(close, open)",   "min(open, close)",
      "rank(close)",       "rank(open)",          "ts_mean(close, 5)",  "ts_mean(close, 6)",
      "scale(close, 2)",   "scale(close, 3)",     "hump(close, 0.1)",   "hump(close, 0.2)",
      "close > open",      "open > close",        "close == open",      "open == close",
      "(close > open) ? close : open", "(close > open) ? open : close", "-close", "abs(close)"};
  CanonSet set;
  usize dup = 0;
  for (usize i = 0; i < corpus.size(); ++i) {
    for (usize k = 0; k < corpus.size(); ++k) {
      const Genome gi = make_genome(corpus[i]);
      const Genome gk = make_genome(corpus[k]);
      const bool same_form = canonical_string(gi) == canonical_string(gk);
      const bool same_hash = canonical_hash(gi) == canonical_hash(gk);
      EXPECT_EQ(same_form, same_hash) << corpus[i] << " vs " << corpus[k];
    }
    const Genome g = make_genome(corpus[i]);
    dup += set.insert(canonical_hash(g), canonical_string(g)) ? 0U : 1U;
  }
  // Commutative pairs dedup (+, *, ==); min is NOT hash-commutative (signed zero).
  EXPECT_EQ(dup, 3U);
  EXPECT_EQ(set.size(), corpus.size() - 3U);
  EXPECT_EQ(set.collisions(), 0U);
}

TEST(FactoryCanonCollision_String, CommutativeOrderNormalizedNamesLengthPrefixed) {
  EXPECT_EQ(canonical_string(make_genome("close + rank(open)")),
            canonical_string(make_genome("rank(open) + close")));
  EXPECT_NE(canonical_string(make_genome("close - rank(open)")),
            canonical_string(make_genome("rank(open) - close")));
  // Literal bits distinguish 0.1 from 0.1000000000000001.
  EXPECT_NE(canonical_string(make_genome("scale(close, 0.1)")),
            canonical_string(make_genome("scale(close, 0.1000000000000001)")));
}

TEST(FactoryCanonCollision_Set, LegacyHashOnlyEntryIsNotDisproven) {
  // A resume snapshot restores bare hashes; a verified lookup of such a key
  // cannot compare forms and conservatively reports it as seen.
  CanonSet set;
  EXPECT_TRUE(set.insert(atx::u64{42}));
  EXPECT_TRUE(set.contains(42, "anything"));
  EXPECT_FALSE(set.insert(42, "anything"));
  EXPECT_EQ(set.size(), 1U);
  // Review fix 1: the verified insert on a hash-only key must not create a
  // (empty) forms entry, so the key stays "seen" for every later lookup.
  EXPECT_TRUE(set.contains(42, "anything"));
  EXPECT_TRUE(set.contains(42, "something else"));
  EXPECT_EQ(set.forms.find(atx::u64{42}), set.forms.end());
  EXPECT_TRUE(set.forms.empty());
  // Repeated verified inserts stay idempotent and never count a collision.
  for (int i = 0; i < 3; ++i) {
    EXPECT_FALSE(set.insert(42, "anything"));
    EXPECT_FALSE(set.insert(42, "other"));
  }
  EXPECT_TRUE(set.contains(42, "anything"));
  EXPECT_TRUE(set.forms.empty());
  EXPECT_EQ(set.size(), 1U);
  EXPECT_EQ(set.collisions(), 0U);
  // Defence in depth: an empty forms list (e.g. from a caller that default-
  // inserted one) is treated as hash-only, not as "no form matches".
  set.forms[atx::u64{42}];
  EXPECT_TRUE(set.contains(42, "anything"));
  EXPECT_FALSE(set.insert(42, "anything"));
  // A verified key still disproves a different form, and legacy insert of it is a no-op.
  EXPECT_TRUE(set.insert(7, "form-a"));
  EXPECT_TRUE(set.contains(7, "form-a"));
  EXPECT_FALSE(set.contains(7, "form-b"));
  EXPECT_FALSE(set.insert(atx::u64{7}));
  EXPECT_FALSE(set.contains(7, "form-b"));
}

// ===========================================================================
//  A-03 — AlphaTypecheckScalarLiteral_CrossoverStress
// ===========================================================================

struct SlotAudit {
  usize scalar_slots{0};
  usize non_literal{0};
};

SlotAudit audit_scalar_slots(const Ast &ast) {
  SlotAudit a;
  for (const Expr &e : ast.nodes()) {
    if (e.kind != Expr::Kind::Call || e.op == nullptr || e.b == kNoExpr ||
        !atx::engine::alpha::detail::has_scalar_literal_slot(e.op->opcode)) {
      continue;
    }
    ++a.scalar_slots;
    const Expr &s = ast.node(e.b);
    if (s.kind != Expr::Kind::Literal || !std::isfinite(s.value)) {
      ++a.non_literal;
    }
  }
  return a;
}

TEST(AlphaTypecheckScalarLiteral_CrossoverStress, TenThousandChildrenHaveOnlyLiteralScalarSlots) {
  std::vector<Genome> pop;
  for (const std::string_view src :
       {"scale(rank(close), 2)", "winsorize(ts_mean(close, 5), 3)", "quantile(volume, 4)",
        "hump(ts_mean(close, 5), 0.1)", "scale(close - open, 1) + rank(volume)",
        "winsorize(close / open, 2.5) * hump(rank(close), 0.05)",
        "quantile(ts_std(close, 10), 5) - scale(volume, 3)", "rank(close) * ts_mean(volume, 10)",
        "ts_std(close, 20) + open", "hump(close) + winsorize(volume)"}) {
    pop.push_back(make_genome(src));
  }
  const usize kSeeds = pop.size();
  Xoshiro256pp rng(0xA03A03ULL);
  constexpr usize kChildren = 10000;
  usize produced = 0;
  usize attempts = 0;
  usize slots_seen = 0;
  usize bad = 0;
  usize scalar_errs = 0;
  // Bounded: at most 20x the target attempts (crossover may return NotFound /
  // an analyze Err for other slots).
  while (produced < kChildren && attempts < 20 * kChildren) {
    ++attempts;
    const usize ia = static_cast<usize>(rng.next_u64() % pop.size());
    const usize ib = static_cast<usize>(rng.next_u64() % pop.size());
    auto child = subtree_crossover(pop[ia], pop[ib], rng);
    if (!child) {
      scalar_errs += child.error().message().find("scalar operand") != std::string::npos ? 1U : 0U;
      continue;
    }
    ++produced;
    const SlotAudit a = audit_scalar_slots(child->ast);
    slots_seen += a.scalar_slots;
    bad += a.non_literal;
    EXPECT_TRUE(analyze(child->ast).has_value());
    // Evolve: children churn through slots [kSeeds, 2*kSeeds) so the seeds (every
    // scalar-slot op) stay available as parents; genome size stays bounded.
    if (child->ast.nodes().size() < 48) {
      if (pop.size() < 2 * kSeeds) {
        pop.push_back(std::move(*child));
      } else {
        pop[kSeeds + static_cast<usize>(rng.next_u64() % kSeeds)] = std::move(*child);
      }
    }
  }
  std::printf("[w0a0] crossover stress: children=%zu attempts=%zu scalar_slots=%zu "
              "non_literal=%zu scalar_rejects=%zu\n",
              produced, attempts, slots_seen, bad, scalar_errs);
  EXPECT_EQ(produced, kChildren);
  EXPECT_GT(slots_seen, kChildren / 10); // the stress really exercised scalar slots
  EXPECT_EQ(bad, 0U);
  // Crossover never OFFERED a non-literal donor to a scalar slot (the analyze
  // backstop never had to reject one).
  EXPECT_EQ(scalar_errs, 0U);
}

} // namespace atx_test_w0_a0_canon
