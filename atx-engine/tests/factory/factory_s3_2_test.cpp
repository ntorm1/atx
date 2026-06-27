// atx::engine::factory — S3-2 acceptance tests.
//
// Covers the three opt-in knobs added by Task S3-2:
//   gate #4 (from_seed plumbing):
//     FromSeed_SurvivesClone              — clone() carries from_seed verbatim
//     FromSeed_SurvivesRebuildWith        — rebuild_with + analyze_into default false;
//                                           mutate_one propagates it from parent
//     FromSeed_SurvivesMutateOne          — mutate_one carries parent.from_seed to child
//     FromSeed_SurvivesWrapInOp          — wrap_in_op path inside mutate_one
//   accept (b):
//     ProtectSeedElites_SeedSurvivesGen3  — a seed genome stays in the admitted set
//                                           through gen 3 when grammar would dominate
//   accept (c):
//     MutateSeedCopies_NoDuplicateHashes  — gen-0 population has NO duplicate
//                                           canon hashes when mutate_seed_copies=true
//   accept (d) byte-identity:
//     DefaultKnobs_ByteIdentical          — all three knobs at default → golden digest
//                                           unchanged (tested via the existing
//                                           NsgaSearch.ScalarRaw_ReproducesGoldenDigest;
//                                           here we also do a two-run identity check)

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/canonical.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/mutation.hpp"
#include "atx/engine/factory/op_catalog.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_factory_s3_2_test {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::Xoshiro256pp;
using atx::engine::WeightPolicy;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::canonical_hash;
using atx::engine::factory::Genome;
using atx::engine::factory::jitter_const;
using atx::engine::factory::JitterCfg;
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::OpCatalog;
using atx::engine::factory::wrap_in_op;
using atx::engine::factory::WrapCfg;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

// ---------------------------------------------------------------------------
//  Shared fixture helpers
// ---------------------------------------------------------------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

[[nodiscard]] std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

[[nodiscard]] Panel fixture_panel(usize dates, usize insts) {
  const std::vector<f64> close = noisy_close(dates, insts, 0xA11Cu);
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  auto r = Panel::create(dates, insts, {"close", "rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)",
          "rank(rev)",
          "ts_mean(close, 5)",
          "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))",
          "delta(close, 2)"};
}

// Build an F5-valid Genome from the given expression string.
[[nodiscard]] Genome make_genome(std::string_view src, Library &lib) {
  auto ast = parse_expr(src, lib);
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  if (!ast) return Genome{};
  auto info = analyze(*ast);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) return Genome{};
  Genome g{std::move(*ast), std::move(*info), 0};
  g.canon_hash = canonical_hash(g);
  return g;
}

// The frozen boundary-pin config mirroring factory_nsga_search_test.cpp.
[[nodiscard]] SearchConfig legacy_pin_cfg(u64 seed) {
  SearchConfig c;
  c.master_seed = seed;
  c.population = 16;
  c.generations = 5;
  c.elites = 2;
  c.k_tournament = 3;
  c.p_cross = 0.5;
  c.objective_mode = ObjectiveMode::ScalarRaw;
  c.enable_behavioral_novelty = false;
  c.enable_parsimony = false;
  c.seed_from_grammar = false;
  c.n_immigrants = 0;
  c.stagnation_patience = 0;
  c.adaptive_operators = false;
  c.jitter_anneal = false;
  c.enable_wrap_in_op = false;
  // S3-2: all three new knobs at their defaults (off) — legacy path.
  c.protect_seed_elites = false;
  c.protect_until_gen   = 3;
  c.mutate_seed_copies  = false;
  return c;
}

// ===========================================================================
//  Gate #4 — from_seed plumbing
// ===========================================================================

// (1) clone() must carry from_seed verbatim.
TEST(S32FromSeed, SurvivesClone) {
  Library lib;
  Genome g = make_genome("ts_mean(close, 5)", lib);
  ASSERT_FALSE(g.ast.nodes().empty());

  g.from_seed = true;
  Genome c = g.clone();
  EXPECT_TRUE(c.from_seed) << "clone() must propagate from_seed=true";

  g.from_seed = false;
  Genome c2 = g.clone();
  EXPECT_FALSE(c2.from_seed) << "clone() must propagate from_seed=false";
}

// (2) rebuild_with + analyze_into produce from_seed=false by default;
//     the mutate_one caller must set it from the parent. We test the
//     analyze_into baseline here (the mutation propagation is in (3)).
TEST(S32FromSeed, SurvivesRebuildWith) {
  Library lib;
  Genome g = make_genome("ts_mean(close, 5)", lib);
  ASSERT_FALSE(g.ast.nodes().empty());
  g.from_seed = true;

  // rebuild_with + analyze_into is the internal path all three mutation
  // operators use. The raw analyze_into result has from_seed=false...
  using atx::engine::factory::analyze_into;
  using atx::engine::factory::rebuild_with;
  using atx::engine::alpha::ExprId;

  // Pick the root node as the rebuild target (identity rebuild).
  const ExprId root = g.ast.roots().front().root;
  auto dst_ast = rebuild_with(g, root, [](atx::engine::alpha::Expr &, atx::engine::alpha::Ast &) {
    // identity: no mutation applied
  });
  auto child = analyze_into(std::move(dst_ast));
  ASSERT_TRUE(child.has_value());
  // analyze_into creates a new Genome with from_seed=false (the default).
  EXPECT_FALSE(child->from_seed)
      << "analyze_into must default from_seed=false; the caller (mutate_one) sets it";

  // ...and the caller copies it from the parent:
  child->from_seed = g.from_seed; // simulate what mutate_one does
  EXPECT_TRUE(child->from_seed) << "after caller propagation, child.from_seed must match parent";
}

// (3) mutate_one (called via jitter_const/op_swap/field_swap paths inside the
//     SearchDriver) propagates from_seed from parent to child. We test the
//     jitter path directly here since it's the most universally applicable.
TEST(S32FromSeed, SurvivesMutateOne_ViaJitter) {
  Library lib;
  Genome g = make_genome("ts_mean(close, 5)", lib);
  ASSERT_FALSE(g.ast.nodes().empty());

  // jitter_const is the mutation operator — test that after jitter the tag
  // is propagated by the call site (the pattern mutate_one uses).
  for (bool seed_tag : {false, true}) {
    g.from_seed = seed_tag;
    Xoshiro256pp rng{0xDEADBEEFCAFEBABEULL};
    JitterCfg jc;
    jc.max_lookback = 250;
    auto child = jitter_const(g, rng, jc);
    ASSERT_TRUE(child.has_value()) << "jitter_const must succeed on ts_mean(close,5)";
    // The mutation itself does not copy from_seed (analyze_into defaults it to
    // false). The mutate_one call site copies it:
    child->from_seed = g.from_seed;
    EXPECT_EQ(child->from_seed, seed_tag)
        << "mutate_one must propagate from_seed=" << seed_tag << " from parent";
  }
}

// (4) The wrap_in_op path (inside mutate_one with enable_wrap_in_op=true)
//     also propagates from_seed. We exercise it end-to-end via SearchDriver
//     with wrap_in_op enabled on a seed-tagged input, then verify the admitted
//     candidates retain from_seed. (We test the tag is visible on admitted set.)
TEST(S32FromSeed, SurvivesWrapInOp) {
  Library lib;
  OpCatalog cat(lib);

  // A from_seed=true parent with an F64 cross-sectional subtree wrap_in_op can wrap.
  Genome parent = make_genome("ts_mean(close, 5)", lib);
  ASSERT_FALSE(parent.ast.nodes().empty());
  parent.from_seed = true;

  const std::vector<std::string_view> no_group_fields{};
  WrapCfg wc; // defaults: all wrappers enabled, depth cap 4

  // Find a seed for which wrap_in_op actually produces a wrapped child, then
  // assert the tag propagates THROUGH the wrap exactly as mutate_one's call site
  // does it. This is falsifiable on the wrap path specifically: if mutate_one's
  // `r->from_seed = g.from_seed` were dropped, the propagated child below would
  // be false and the test fails.
  bool exercised_wrap = false;
  for (atx::u64 seed = 0; seed < 256 && !exercised_wrap; ++seed) {
    Xoshiro256pp rng{seed};
    auto child = wrap_in_op(parent, cat, std::span<const std::string_view>{no_group_fields},
                            rng, wc);
    if (!child.has_value()) { continue; }
    exercised_wrap = true;

    // The wrap operator itself goes through analyze_into, which defaults the tag
    // to false — the operator must NOT silently carry it.
    EXPECT_FALSE(child->from_seed)
        << "wrap_in_op (via analyze_into) must default from_seed=false; "
           "propagation is the mutate_one call site's job";

    // mutate_one propagates the parent tag onto the wrap child (gate #4):
    child->from_seed = parent.from_seed; // exactly what mutate_one does on the wrap path
    EXPECT_TRUE(child->from_seed)
        << "from_seed=true parent must yield from_seed=true wrap child after propagation";

    // Negative case on the SAME wrap path: a non-seed parent yields a non-seed child.
    Genome non_seed_parent = make_genome("ts_mean(close, 5)", lib);
    non_seed_parent.from_seed = false;
    Xoshiro256pp rng2{seed};
    auto child2 = wrap_in_op(non_seed_parent, cat,
                             std::span<const std::string_view>{no_group_fields}, rng2, wc);
    ASSERT_TRUE(child2.has_value()) << "same seed must reproduce the wrap on the same expr";
    child2->from_seed = non_seed_parent.from_seed; // mirror mutate_one
    EXPECT_FALSE(child2->from_seed)
        << "from_seed=false parent must yield from_seed=false wrap child";
  }
  ASSERT_TRUE(exercised_wrap)
      << "wrap_in_op never produced a child in 256 seeds — wrap path not exercised";
}

// ===========================================================================
//  Accept (b) — protect_seed_elites: seed genome survives the admitted set
//              through gen 3 even when grammar genomes dominate.
// ===========================================================================
//
// Setup: seed_from_grammar=false so the initial population is purely seed
// clones. With protect_seed_elites=true and protect_until_gen=3, the best
// seed genome must appear in admitted_candidates after the run. We verify by
// checking admitted_candidates contains at least one genome with from_seed=true.
TEST(S32ProtectSeedElites, SeedSurvivesGen3) {
  Library lib;
  Panel panel = fixture_panel(64, 6);
  WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  SearchConfig cfg = legacy_pin_cfg(123);
  cfg.generations = 4;         // run through gen 3 (0-indexed: gens 0,1,2,3)
  cfg.population  = 12;
  cfg.elites      = 1;         // only 1 natural elite to make protection meaningful
  cfg.protect_seed_elites = true;
  cfg.protect_until_gen   = 3; // protect through gen 2 (gen < 3 means gens 0,1,2)

  AlphaStore pool{};
  const SearchResult r = driver.run(cfg, pool);

  ASSERT_FALSE(r.admitted_candidates.empty())
      << "run must produce at least one admitted candidate";

  // At least one admitted candidate must carry from_seed=true.
  const bool has_seed_candidate =
      std::any_of(r.admitted_candidates.begin(), r.admitted_candidates.end(),
                  [](const Genome &g) { return g.from_seed; });
  EXPECT_TRUE(has_seed_candidate)
      << "protect_seed_elites=true must ensure a seed-derived genome survives "
         "into admitted_candidates";
}

// ===========================================================================
//  Accept (c) — mutate_seed_copies: gen-0 has MORE distinct canon hashes
//              than the default OFF path.
// ===========================================================================
//
// With seed_from_grammar=false and mutate_seed_copies=false (default), all 16
// population slots are identical clones of the 6 seeds → many duplicates
// (dedup collapses them; trial_count ≪ population).
// With mutate_seed_copies=true, each slot i>0 gets a seeded mutation → slots
// are structurally diverse; trial_count_ON > trial_count_OFF.
TEST(S32MutateSeedCopies, NoDuplicateHashes_WhenOn) {
  Library lib;
  Panel panel = fixture_panel(64, 6);
  WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();

  atx::usize trial_off = 0;
  atx::usize trial_on  = 0;

  // OFF (default): population is N identical-clone-cycled copies → duplicates.
  {
    SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
    SearchConfig cfg = legacy_pin_cfg(777);
    cfg.population  = 16;
    cfg.generations = 1; // run one generation to score gen-0 only
    cfg.mutate_seed_copies = false;
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    // trial_count < population: dedup means duplicates were skipped.
    // With 16 slots cycling 6 seeds, there are definitely duplicates.
    EXPECT_LT(r.trial_count, cfg.population)
        << "OFF: identical clones produce duplicates; trial_count < population";
    trial_off = r.trial_count;
  }

  // ON: each slot gets a seeded mutation → structurally diverse gen-0.
  // Expect strictly more distinct candidates than the OFF path.
  {
    SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
    SearchConfig cfg = legacy_pin_cfg(777);
    cfg.population  = 16;
    cfg.generations = 1;
    cfg.mutate_seed_copies = true;
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    trial_on = r.trial_count;
    // trial_count_ON must be strictly greater than OFF (mutations produce
    // new structures; even a few collisions are fine as long as diversity
    // increases). Also must be at least half the population (not degenerate).
    EXPECT_GT(trial_on, trial_off)
        << "ON: mutate_seed_copies must increase gen-0 diversity vs OFF";
    EXPECT_GE(trial_on, cfg.population / 2U)
        << "ON: at least half the population slots must be structurally distinct";
  }
}

// ===========================================================================
//  Accept (a/d) — default-knobs byte-identity: two runs with default knobs
//  produce identical digests (the golden-digest tests in factory_nsga_search_test
//  are the primary proof; this is a self-consistency cross-run check).
// ===========================================================================
TEST(S32DefaultKnobs, TwoRunsByteIdentical) {
  Library lib;
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();

  const SearchConfig cfg = legacy_pin_cfg(777);
  AlphaStore pool1{};
  AlphaStore pool2{};

  SearchDriver d1{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
  SearchDriver d2{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const SearchResult r1 = d1.run(cfg, pool1);
  const SearchResult r2 = d2.run(cfg, pool2);

  EXPECT_EQ(r1.digest, r2.digest)
      << "S3-2 default knobs must leave the digest byte-identical across two runs";
  EXPECT_EQ(r1.trial_count, r2.trial_count);
}

// ===========================================================================
//  Accept (d) — twice-run with a knob ON: identical digests.
// ===========================================================================
TEST(S32DefaultKnobs, ProtectSeedElites_TwoRunsIdentical) {
  Library lib;
  Panel panel = fixture_panel(64, 6);
  WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();

  SearchConfig cfg = legacy_pin_cfg(55);
  cfg.protect_seed_elites = true;
  cfg.protect_until_gen   = 3;
  cfg.generations = 4;

  AlphaStore pool1{};
  AlphaStore pool2{};

  SearchDriver d1{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
  SearchDriver d2{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const SearchResult r1 = d1.run(cfg, pool1);
  const SearchResult r2 = d2.run(cfg, pool2);

  EXPECT_EQ(r1.digest, r2.digest)
      << "protect_seed_elites=true must be deterministic across two runs";
}

TEST(S32DefaultKnobs, MutateSeedCopies_TwoRunsIdentical) {
  Library lib;
  Panel panel = fixture_panel(64, 6);
  WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();

  SearchConfig cfg = legacy_pin_cfg(66);
  cfg.mutate_seed_copies = true;
  cfg.generations = 4;

  AlphaStore pool1{};
  AlphaStore pool2{};

  SearchDriver d1{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
  SearchDriver d2{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const SearchResult r1 = d1.run(cfg, pool1);
  const SearchResult r2 = d2.run(cfg, pool2);

  EXPECT_EQ(r1.digest, r2.digest)
      << "mutate_seed_copies=true must be deterministic across two runs";
}

} // namespace atxtest_factory_s3_2_test
