// atx::engine::factory — R1 typed-fields discipline tests (suite FactoryTypedFields).
//
// Tests the four TDD requirements from the R1 brief:
//   1. Off-path byte-identity: empty exclusion lists -> SAME digest as no-arg baseline.
//      The kGoldenDigest pin in factory_nsga_search_test.cpp is the PRIMARY proof;
//      this file adds an EXPLICIT assertion that empty lists reproduce the same digest
//      as a driver built with the legacy 7-arg constructor.
//   2. On-path exclusion (RED->GREEN): excluding a field removes it from
//      numeric_field_views_ and changes the digest.
//   3. Determinism: flag-ON run twice -> identical digest.
//   4. (Stage cardinality test lives in atx-impl: typed_fields_cardinality_test.cpp.)

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_factory_typed_fields {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
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
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;
using atx::engine::WeightPolicy;

// ---- helpers ----------------------------------------------------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

// Deterministic LCG — same idiom as factory_search_driver_test.cpp.
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    return 2.0 * static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U) - 1.0;
  }
};

// Build a tiny panel with "close" (continuous) and "flag" (binary: values ±1).
// "flag" is a synthetic binary event field — the kind R1 is designed to keep
// out of the numeric leaf pool.
[[nodiscard]] Panel make_typed_panel(usize dates, usize insts) {
  Lcg rng{0xDEADBEEFu};
  std::vector<f64> close(dates * insts);
  std::vector<f64> flag_col(dates * insts); // binary: -1 or +1
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      close[t * insts + j] = 100.0 * (1.0 + 0.01 * rng.next());
      flag_col[t * insts + j] = (rng.next() > 0.0) ? 1.0 : -1.0;
    }
  }
  auto r = Panel::create(dates, insts, {"close", "flag"}, {close, flag_col}, {});
  EXPECT_TRUE(r.has_value()) << "typed panel must build";
  return std::move(*r);
}

// Small search config.
[[nodiscard]] SearchConfig small_cfg(atx::u64 seed) {
  SearchConfig cfg;
  cfg.master_seed = seed;
  cfg.population  = 16;
  cfg.generations = 4;
  cfg.elites      = 2;
  cfg.k_tournament = 3;
  cfg.p_cross     = 0.5;
  return cfg;
}

// Seed expressions over both fields — so "flag" CAN be used as a numeric leaf
// when exclusion is OFF, giving the search a real chance to produce flag-containing
// genomes. This is the RED part: without exclusion, flag enters the grammar.
[[nodiscard]] std::vector<std::string> both_field_seeds() {
  return {"rank(close)", "rank(flag)", "ts_mean(close, 5)", "delta(close, 2)"};
}

// =============================================================================
// Test 1 — Off-path byte-identity.
//   Driver with EMPTY exclusion lists must produce the SAME digest as the
//   7-arg legacy constructor (no exclusion lists).
// =============================================================================
TEST(FactoryTypedFields, EmptyListsByteIdenticalToLegacy) {
  Library lib{};
  Panel   panel = make_typed_panel(64, 5);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  const std::vector<std::string> fields = {"close", "flag"};
  const std::vector<std::string> seeds  = both_field_seeds();
  const auto cfg = small_cfg(42U);

  // Legacy 7-arg constructor (no R1 params).
  SearchDriver drv_legacy{lib, panel, policy, sim, seeds, fields};
  AlphaStore pool1{};
  const SearchResult r_legacy = drv_legacy.run(cfg, pool1);

  // R1 constructor with EMPTY lists — must be identical.
  SearchDriver drv_empty{lib, panel, policy, sim, seeds, fields,
                         /*weak_panel=*/nullptr,
                         /*numeric_excluded=*/{},
                         /*extra_group=*/{}};
  AlphaStore pool2{};
  const SearchResult r_empty = drv_empty.run(cfg, pool2);

  EXPECT_EQ(r_legacy.digest, r_empty.digest)
      << "Empty exclusion lists must produce byte-identical digest to legacy ctor.";
}

// =============================================================================
// Test 2a — On-path structural assertion (RED->GREEN).
//   With "flag" in numeric_excluded_fields, it must NOT appear in
//   numeric_field_views_. This is a direct structural assertion — no digest diff
//   needed (though the digest will also change).
//
//   We verify via an internal accessor exposed by the PRIVATE section of
//   SearchDriver... but SearchDriver does NOT expose numeric_field_views_ publicly.
//   Instead we verify the BEHAVIORAL consequence: a driver with "flag" excluded
//   from numerics can still RUN (no crash) and produces a different digest from
//   an unrestricted driver. We also verify the digest reproducibility (run twice).
// =============================================================================
TEST(FactoryTypedFields, ExcludedFieldChangesDigest) {
  Library lib{};
  Panel   panel = make_typed_panel(64, 5);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  const std::vector<std::string> fields = {"close", "flag"};
  const std::vector<std::string> seeds  = both_field_seeds();
  const auto cfg = small_cfg(42U);

  // Baseline: no exclusion.
  SearchDriver drv_base{lib, panel, policy, sim, seeds, fields};
  AlphaStore pool_base{};
  const SearchResult r_base = drv_base.run(cfg, pool_base);

  // With "flag" excluded from numeric pool.
  SearchDriver drv_excl{lib, panel, policy, sim, seeds, fields,
                        /*weak_panel=*/nullptr,
                        /*numeric_excluded=*/{"flag"},
                        /*extra_group=*/{}};
  AlphaStore pool_excl{};
  const SearchResult r_excl = drv_excl.run(cfg, pool_excl);

  // The digest MUST change: the grammar is now restricted so "flag" cannot be a
  // numeric leaf -> different population evolved -> different digest.
  EXPECT_NE(r_base.digest, r_excl.digest)
      << "Excluding 'flag' from numerics must alter the search digest.";
}

// =============================================================================
// Test 2b — Excluded field never appears as numeric leaf.
//   We verify the behavioral contract: running the excluded driver does NOT crash
//   and still produces a positive trial count (the grammar can still evolve
//   using "close"). This is the "GREEN" after R1 wiring.
// =============================================================================
TEST(FactoryTypedFields, ExcludedDriverRunsCleanly) {
  Library lib{};
  Panel   panel = make_typed_panel(64, 5);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  const std::vector<std::string> fields = {"close", "flag"};
  const std::vector<std::string> seeds  = {"rank(close)", "ts_mean(close, 5)"};
  const auto cfg = small_cfg(99U);

  SearchDriver drv{lib, panel, policy, sim, seeds, fields,
                   /*weak_panel=*/nullptr,
                   /*numeric_excluded=*/{"flag"},
                   /*extra_group=*/{}};
  AlphaStore pool{};
  const SearchResult res = drv.run(cfg, pool);

  EXPECT_GT(res.trial_count, 0U) << "Search with 'flag' excluded must still produce candidates.";
}

// =============================================================================
// Test 3 — Determinism: flag-ON search run twice -> identical digest.
// =============================================================================
TEST(FactoryTypedFields, TypedFieldsDigestDeterministic) {
  Library lib{};
  Panel   panel = make_typed_panel(64, 5);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  const std::vector<std::string> fields = {"close", "flag"};
  const std::vector<std::string> seeds  = both_field_seeds();
  const auto cfg = small_cfg(77U);

  auto make_driver = [&]() {
    return SearchDriver{lib, panel, policy, sim, seeds, fields,
                        /*weak_panel=*/nullptr,
                        /*numeric_excluded=*/{"flag"},
                        /*extra_group=*/{}};
  };

  AlphaStore pool1{};
  const SearchResult r1 = make_driver().run(cfg, pool1);

  AlphaStore pool2{};
  const SearchResult r2 = make_driver().run(cfg, pool2);

  EXPECT_EQ(r1.digest, r2.digest)
      << "Flag-ON search must be deterministic across two identical runs.";
  EXPECT_EQ(r1.trial_count, r2.trial_count);
}

// =============================================================================
// Test 4 — Extra group field.
//   A field added to extra_group_fields is routed to the group pool (not numeric).
//   We verify: (a) the driver runs without crash, (b) the digest differs from
//   a driver that treats the same field as numeric.
// =============================================================================
TEST(FactoryTypedFields, ExtraGroupFieldChangesDigest) {
  Library lib{};
  Panel   panel = make_typed_panel(64, 5);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  const std::vector<std::string> fields = {"close", "flag"};
  const std::vector<std::string> seeds  = both_field_seeds();
  const auto cfg = small_cfg(55U);

  // Baseline: no extra_group.
  SearchDriver drv_base{lib, panel, policy, sim, seeds, fields};
  AlphaStore pool_base{};
  const SearchResult r_base = drv_base.run(cfg, pool_base);

  // With "flag" in BOTH numeric_excluded AND extra_group (the gics-style pattern).
  SearchDriver drv_grp{lib, panel, policy, sim, seeds, fields,
                       /*weak_panel=*/nullptr,
                       /*numeric_excluded=*/{"flag"},
                       /*extra_group=*/{"flag"}};
  AlphaStore pool_grp{};
  const SearchResult r_grp = drv_grp.run(cfg, pool_grp);

  // Digest must differ: "flag" is now out of numeric pool entirely.
  EXPECT_NE(r_base.digest, r_grp.digest)
      << "Routing 'flag' to extra_group (out of numeric) must change digest.";
}

} // namespace atxtest_factory_typed_fields
