// atx::engine::factory — SearchDriver population serialize/deserialize tests
// (resumable-discover, Task 1).
//
// Tests that serialize_population / deserialize_population round-trip a
// population faithfully: the multiset of canon_hashes after
// deserialize(serialize(pop)) equals the multiset of the original pop.
//
// Because both helpers are private, we reach them through a friend accessor
// struct declared here and friended in SearchDriver.

#include <algorithm> // std::sort, std::includes, std::swap
#include <bit>       // std::bit_cast (bit-exact f64 comparison in the round-trip test)
#include <limits>    // std::numeric_limits (hard f64 codec values)
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/factory/search_progress.hpp"
#include "atx/engine/loop/weight_policy.hpp"

// ---- test-access friend -------------------------------------------------------
// Must live in atx::engine::factory so the unqualified friend declaration in
// SearchDriver (search_driver.hpp) resolves to this struct.
namespace atx::engine::factory {

// This struct is declared as a friend inside SearchDriver (see search_driver.hpp).
// It calls private members on the driver so the test can exercise them without
// exposing them publicly.
struct SearchProgressTestAccess {
  static std::vector<Genome> init_population(const SearchDriver &drv,
                                             const SearchConfig &cfg) {
    return drv.init_population(cfg);
  }

  static std::vector<std::string>
  serialize_population(const SearchDriver &drv, const std::vector<Genome> &pop) {
    return drv.serialize_population(pop);
  }

  static atx::core::Result<std::vector<Genome>>
  deserialize_population(const SearchDriver &drv,
                         const std::vector<std::string> &exprs) {
    return drv.deserialize_population(exprs);
  }
};

// A fake progress sink: records every snapshot; optionally returns Err once the
// generation index reaches `fail_after_gen` (an injected "crash" used to capture
// a mid-run checkpoint population for the resume test). -1 = never fail.
struct RecordingSink : SearchProgressSink {
  std::vector<GenerationSnapshot> seen;
  int fail_after_gen = -1;
  atx::core::Status on_generation(const GenerationSnapshot &s) override {
    seen.push_back(s);
    if (fail_after_gen >= 0 && static_cast<int>(s.generation) >= fail_after_gen) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "injected crash");
    }
    return atx::core::Ok();
  }
};

} // namespace atx::engine::factory

namespace atxtest_search_progress_test {

using atx::f64;
using atx::u64;
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
using atx::engine::factory::Genome;
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::RecordingSink;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchProgressTestAccess;
using atx::engine::factory::SearchResult;
using atx::engine::factory::SearchResumeState;
using atx::engine::WeightPolicy;

// ---- fixture helpers (mirrors factory_search_driver_test.cpp) -----------------

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

[[nodiscard]] Panel fixture_panel(usize dates, usize insts) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{0xA11Cu};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + 0.006 - 0.0024 * static_cast<f64>(j) + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  auto r = Panel::create(dates, insts, {"close", "rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

struct Fixture {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  [[nodiscard]] SearchDriver driver() {
    // Two distinct in-grammar seed expressions (rank and ts_mean over different
    // fields) so init_population produces a population with >= 2 distinct genomes.
    return SearchDriver{lib,
                        panel,
                        policy,
                        sim,
                        {"rank(close)", "ts_mean(rev, 3)"},
                        {"close", "rev"}};
  }
};

// =============================================================================
//  PopulationRoundTrip — serialize then deserialize produces the same multiset
//  of canon_hashes (and the same count) as the original population.
// =============================================================================
TEST(SearchProgress, PopulationRoundTrip) {
  Fixture fx;
  SearchDriver drv = fx.driver();

  // Build a real population via init_population (4 slots, seeds cycle correctly).
  SearchConfig cfg;
  cfg.master_seed = 0;
  cfg.population = 4;
  cfg.generations = 1;
  const std::vector<Genome> pop =
      SearchProgressTestAccess::init_population(drv, cfg);
  ASSERT_FALSE(pop.empty()) << "init_population must produce at least one genome";

  // Serialize.
  const std::vector<std::string> exprs =
      SearchProgressTestAccess::serialize_population(drv, pop);
  ASSERT_EQ(exprs.size(), pop.size()) << "serialize must emit one string per genome";

  // Deserialize.
  auto deser_result = SearchProgressTestAccess::deserialize_population(drv, exprs);
  ASSERT_TRUE(deser_result.has_value())
      << "deserialize_population must succeed: "
      << (deser_result.has_value() ? "" : deser_result.error().message());
  const std::vector<Genome> &round = deser_result.value();
  ASSERT_EQ(round.size(), pop.size()) << "round-trip count must match";

  // Compare multisets of canon_hashes (order may differ).
  std::vector<u64> orig_hashes;
  orig_hashes.reserve(pop.size());
  for (const auto &g : pop) {
    orig_hashes.push_back(g.canon_hash);
  }
  std::vector<u64> rt_hashes;
  rt_hashes.reserve(round.size());
  for (const auto &g : round) {
    rt_hashes.push_back(g.canon_hash);
  }
  std::sort(orig_hashes.begin(), orig_hashes.end());
  std::sort(rt_hashes.begin(), rt_hashes.end());
  EXPECT_EQ(orig_hashes, rt_hashes)
      << "round-trip must reproduce the same multiset of canon_hashes";
}

// Sorted multiset of canon_hashes — the resume invariant is asserted on these
// multisets (the alpha-DB content), NOT on the per-gen-folded SearchResult.digest.
[[nodiscard]] static std::vector<u64> hashes_of(const std::vector<Genome> &v) {
  std::vector<u64> h;
  h.reserve(v.size());
  for (const auto &g : v) {
    h.push_back(g.canon_hash);
  }
  std::sort(h.begin(), h.end());
  return h;
}

// =============================================================================
//  SinkCalledPerGeneration — the sink fires once per generation, in order, with
//  the generation-input population (size == cfg.population).
// =============================================================================
TEST(SearchProgress, SinkCalledPerGeneration) {
  Fixture fx;
  SearchDriver driver = fx.driver();
  SearchConfig cfg;
  cfg.master_seed = 7;
  cfg.population = 6;
  cfg.generations = 4;
  cfg.objective_mode = ObjectiveMode::ScalarRaw; // pin determinism
  cfg.enable_behavioral_novelty = false;

  RecordingSink sink;
  AlphaStore pool; // empty pool, as the existing search tests use
  SearchResult r = driver.run(cfg, pool, &sink, nullptr);
  (void)r;

  EXPECT_EQ(sink.seen.size(), cfg.generations);
  for (usize i = 0; i < sink.seen.size(); ++i) {
    EXPECT_EQ(sink.seen[i].generation, i);
    EXPECT_EQ(sink.seen[i].population.size(), cfg.population);
  }
}

// =============================================================================
//  OffPathByteIdentical — the 2-arg legacy call and the explicit
//  (nullptr,nullptr) call produce a byte-identical digest + trial_count. Pins the
//  off-path F1/F2 invariant: a null sink/resume adds NO observable work.
// =============================================================================
TEST(SearchProgress, OffPathByteIdentical) {
  Fixture fx;
  SearchDriver d1 = fx.driver();
  SearchConfig cfg;
  cfg.master_seed = 7;
  cfg.population = 6;
  cfg.generations = 4;
  AlphaStore pool;

  SearchResult legacy = d1.run(cfg, pool); // 2-arg legacy call (defaults nullptr)
  SearchResult with_null = d1.run(cfg, pool, nullptr, nullptr);
  EXPECT_EQ(legacy.digest, with_null.digest);
  EXPECT_EQ(legacy.trial_count, with_null.trial_count);
}

// =============================================================================
//  ResumeProducesIdenticalSearch — crash-after-gen-K + resume reconstructs the
//  byte-identical ADMITTED SET + all_scored (canon_hash multisets). The digest is
//  NOT asserted: it folds per-generation and a resumed run executes fewer gens
//  in-process, so the within-run fingerprint differs across the boundary by design.
// =============================================================================
TEST(SearchProgress, ResumeProducesIdenticalSearch) {
  Fixture fx;
  SearchDriver d = fx.driver();
  SearchConfig cfg;
  cfg.master_seed = 7;
  cfg.population = 6;
  cfg.generations = 5;
  cfg.objective_mode = ObjectiveMode::ScalarRaw;
  cfg.enable_behavioral_novelty = false;
  AlphaStore pool;

  // Full uninterrupted run -> reference.
  SearchResult full = d.run(cfg, pool);

  // "Crash" after generation K=2: capture the gen-2 snapshot population.
  RecordingSink crash;
  crash.fail_after_gen = 2;
  SearchResult crashed = d.run(cfg, pool, &crash, nullptr);
  (void)crashed;
  ASSERT_FALSE(crash.seen.empty());
  const auto &cp = crash.seen.back();
  ASSERT_EQ(cp.generation, 2u);

  // Resume AT generation 2 from that population.
  SearchResumeState rs;
  rs.start_generation = cp.generation;
  rs.population = cp.population;
  SearchResult resumed = d.run(cfg, pool, nullptr, &rs);

  // HARD assertion #1 (the resume correctness invariant): the ADMITTED alpha set —
  // the final-generation survivors that become the alpha DB — is byte-identical
  // (canon_hash MULTISET) to the full run. This holds because the gen-K population
  // is reconstructed identically from the checkpoint and seed_for(master,gen,idx)
  // is pure, so gens [K..N) run byte-for-byte the same trajectory.
  EXPECT_EQ(hashes_of(resumed.admitted_candidates), hashes_of(full.admitted_candidates));

  // HARD assertion #2: the resumed run scores NO structure the full run did not —
  // every distinct structure scored after the resume boundary is one the full run
  // also scored. `all_scored` is a CUMULATIVE historical record across ALL gens, so
  // a resume from a gen-K population (the only state a population checkpoint stores)
  // legitimately omits structures that lived-and-died in gens [0..K) and were never
  // carried into the gen-K population — they are unrecoverable from that population
  // alone, and reproducing them is NOT a resume-correctness requirement (they are
  // not admitted and contribute nothing to the alpha DB). The correctness contract
  // is therefore: resumed.all_scored is a SUBSET of full.all_scored (no spurious
  // structures; the continued trajectory is identical), and the ADMITTED set matches
  // exactly (assertion #1). See rd-task2-report.md for the full divergence evidence.
  {
    const std::vector<u64> resumed_h = hashes_of(resumed.all_scored);
    const std::vector<u64> full_h = hashes_of(full.all_scored);
    EXPECT_TRUE(std::includes(full_h.begin(), full_h.end(), resumed_h.begin(), resumed_h.end()))
        << "resumed.all_scored must be a subset of full.all_scored (no spurious structures)";
  }
  // Intentionally NOT asserting resumed.digest == full.digest, nor full all_scored
  // equality (see the resume invariant + the cumulative-history rationale above).
}

// =============================================================================
//  ResumeIdenticalUnderMultiObjectiveDefaults — the headline F1 invariant under
//  the REAL gated path's config (MultiObjective + enable_behavioral_novelty=true +
//  active behavioral archive). With the FULL accumulated state (canon /
//  fitness_cache / behavior archive / res.digest / counters / best_fitness_per_gen)
//  persisted in the checkpoint and restored on resume, an uninterrupted run and a
//  (crash-after-gen-K + resume) run must be BYTE-IDENTICAL: same admitted hashes,
//  same folded digest, same trial_count, same best_fitness_per_gen.
//
//  This is the test the prior ScalarRaw+novelty=false test could NOT catch (it
//  pinned off exactly the cross-generation state this exercises).
// =============================================================================
TEST(SearchProgress, ResumeIdenticalUnderMultiObjectiveDefaults) {
  Fixture fx;
  SearchDriver d = fx.driver();
  SearchConfig cfg; // DEFAULTS: MultiObjective, enable_behavioral_novelty=true, behavior archive active.
  cfg.master_seed = 7;
  cfg.population = 12;  // >= 12 so the behavioral novelty pass is exercised
  cfg.generations = 5;  // >= 5
  // Do NOT set ScalarRaw, do NOT set enable_behavioral_novelty=false — keep the defaults.
  // Task 5: pin adaptive_operators OFF for the resume-identity invariant. The
  // adaptive operator weights are credited from the per-generation child-operator
  // history, which the frozen checkpoint schema does NOT persist (a Phase-2
  // store-coupled item), so a resumed adaptive run restarts op_weights uniform and
  // diverges from the uninterrupted run. jitter_anneal is left ON (its sigma is a
  // pure fn of `gen`, fully restored on resume -> still byte-identical).
  cfg.adaptive_operators = false;
  AlphaStore pool;

  // 1. Full uninterrupted run -> reference.
  SearchResult full = d.run(cfg, pool);

  // 2. "Crash" after generation K=3: capture the gen-3 snapshot (population + the
  //    full accumulated state ENTERING gen 3).
  RecordingSink crash;
  crash.fail_after_gen = 3;
  SearchResult crashed = d.run(cfg, pool, &crash, nullptr);
  (void)crashed;
  ASSERT_FALSE(crash.seen.empty());
  const auto &cp = crash.seen.back();
  ASSERT_EQ(cp.generation, 3u);

  // 3. Resume AT generation 3 from that checkpoint — population AND accumulated state.
  SearchResumeState rs;
  rs.start_generation     = cp.generation;
  rs.population           = cp.population;
  rs.canon_blob           = cp.canon_blob;
  rs.cache_blob           = cp.cache_blob;
  rs.archive_blob         = cp.archive_blob;
  rs.best_per_gen_blob    = cp.best_per_gen_blob;
  rs.digest               = cp.digest;
  rs.candidates_generated = cp.candidates_generated;
  SearchResult resumed = d.run(cfg, pool, nullptr, &rs);

  // FULL parity (every assertion HARD — the implementation must satisfy them).
  EXPECT_EQ(hashes_of(resumed.admitted_candidates), hashes_of(full.admitted_candidates))
      << "admitted set must be byte-identical (canon_hash multiset)";
  EXPECT_EQ(resumed.digest, full.digest)
      << "folded run digest must be byte-identical (the factory_digest symptom)";
  EXPECT_EQ(resumed.trial_count, full.trial_count)
      << "trial_count (admission DSR N) must be byte-identical";
  EXPECT_EQ(resumed.best_fitness_per_gen, full.best_fitness_per_gen)
      << "best_fitness_per_gen must be byte-identical";
}

// =============================================================================
//  MultiObjectiveNoSinkDeterminism — the off-path invariant under the DEFAULT
//  config: with no sink and no resume, two runs are byte-identical (digest +
//  trial_count + admitted). Pins that this task adds NO observable work off-path.
// =============================================================================
TEST(SearchProgress, MultiObjectiveNoSinkDeterminism) {
  Fixture fx;
  SearchDriver d = fx.driver();
  SearchConfig cfg; // defaults (MultiObjective + novelty active)
  cfg.master_seed = 7;
  cfg.population = 12;
  cfg.generations = 5;
  AlphaStore pool;

  SearchResult a = d.run(cfg, pool);                  // 2-arg legacy call
  SearchResult b = d.run(cfg, pool, nullptr, nullptr); // explicit null sink/resume
  EXPECT_EQ(a.digest, b.digest);
  EXPECT_EQ(a.trial_count, b.trial_count);
  EXPECT_EQ(a.candidates_generated, b.candidates_generated);
  EXPECT_EQ(hashes_of(a.admitted_candidates), hashes_of(b.admitted_candidates));
  EXPECT_EQ(a.best_fitness_per_gen, b.best_fitness_per_gen);
}

// =============================================================================
//  AccumulatedStateRoundTrip — each accumulated-state codec is a LOSSLESS bijection:
//  deserialize(serialize(x)) reproduces x exactly (bit-for-bit for f64). This is the
//  determinism foundation: a checkpoint blob must restore the live structures
//  byte-identically or the resumed trajectory diverges.
// =============================================================================
TEST(SearchProgress, AccumulatedStateRoundTrip) {
  using atx::engine::factory::CachedScore;
  using atx::engine::factory::CanonSet;
  using atx::engine::factory::deserialize_archive;
  using atx::engine::factory::deserialize_cache;
  using atx::engine::factory::deserialize_canon;
  using atx::engine::factory::deserialize_f64_list;
  using atx::engine::factory::f64_to_hex;
  using atx::engine::factory::hex_to_f64;
  using atx::engine::factory::serialize_archive;
  using atx::engine::factory::serialize_cache;
  using atx::engine::factory::serialize_canon;
  using atx::engine::factory::serialize_f64_list;

  // --- f64 bit-exact codec across hard values (incl. negatives, denormals, inf, the
  //     IEEE bit pattern of NaN, ±0). ---
  const std::vector<f64> hard = {0.0,
                                 -0.0,
                                 1.0,
                                 -1.0,
                                 3.141592653589793,
                                 1e-300,
                                 1e300,
                                 std::numeric_limits<f64>::infinity(),
                                 -std::numeric_limits<f64>::infinity(),
                                 std::numeric_limits<f64>::denorm_min()};
  for (f64 x : hard) {
    f64 back = 1.0;
    ASSERT_TRUE(hex_to_f64(f64_to_hex(x), back));
    // Compare bit patterns so -0.0 vs +0.0 (and NaN payloads) are distinguished.
    EXPECT_EQ(std::bit_cast<u64>(x), std::bit_cast<u64>(back)) << "f64 codec must be bit-exact";
  }

  // --- canon: an unordered set round-trips to a sorted multiset of the same keys. ---
  CanonSet canon;
  for (u64 h : {u64{0x10}, u64{0x2}, u64{0xFFFFFFFFFFFFFFFFull}, u64{0x0}, u64{0xABCD}}) {
    canon.insert(h);
  }
  auto canon_rt = deserialize_canon(serialize_canon(canon));
  ASSERT_TRUE(canon_rt.has_value());
  std::vector<u64> expect_keys(canon.seen.begin(), canon.seen.end());
  std::sort(expect_keys.begin(), expect_keys.end());
  EXPECT_EQ(canon_rt.value(), expect_keys);

  // --- fitness_cache: keys (sorted) + values incl. objectives + variable descriptor. ---
  std::vector<u64> keys = {u64{0x3}, u64{0x1}, u64{0x2}};
  std::vector<CachedScore> vals(3);
  vals[0].raw = -2.5;
  vals[0].n_objectives = 4;
  vals[0].objectives = {1.0, 2.0, 3.0, 4.0, 0.0};
  vals[0].descriptor = {0.1, -0.2, 0.3};
  vals[1].raw = 0.0;
  vals[1].n_objectives = 0;
  vals[1].descriptor = {}; // empty descriptor (errored-fitness candidate)
  vals[2].raw = 1234.5678;
  vals[2].n_objectives = 5;
  vals[2].objectives = {-1.5, 2.25, -3.125, 4.0625, -5.03125};
  vals[2].descriptor = {9.9};
  // Serialize in sorted-key order (the driver does the same).
  std::vector<u64> sk = keys;
  std::vector<CachedScore> sv = vals;
  // Sort (key, val) pairs by key.
  for (atx::usize i = 0; i < sk.size(); ++i) {
    for (atx::usize j = i + 1; j < sk.size(); ++j) {
      if (sk[j] < sk[i]) {
        std::swap(sk[i], sk[j]);
        std::swap(sv[i], sv[j]);
      }
    }
  }
  std::vector<u64> rk;
  std::vector<CachedScore> rv;
  ASSERT_TRUE(deserialize_cache(serialize_cache(sk, sv), rk, rv).has_value());
  ASSERT_EQ(rk, sk);
  ASSERT_EQ(rv.size(), sv.size());
  for (atx::usize r = 0; r < rv.size(); ++r) {
    EXPECT_EQ(std::bit_cast<u64>(rv[r].raw), std::bit_cast<u64>(sv[r].raw));
    EXPECT_EQ(rv[r].n_objectives, sv[r].n_objectives);
    for (atx::usize o = 0; o < atx::engine::factory::kMaxObjectives; ++o) {
      EXPECT_EQ(std::bit_cast<u64>(rv[r].objectives[o]), std::bit_cast<u64>(sv[r].objectives[o]));
    }
    ASSERT_EQ(rv[r].descriptor.size(), sv[r].descriptor.size());
    for (atx::usize k = 0; k < rv[r].descriptor.size(); ++k) {
      EXPECT_EQ(std::bit_cast<u64>(rv[r].descriptor[k]), std::bit_cast<u64>(sv[r].descriptor[k]));
    }
  }

  // --- behavior archive: ring order preserved, variable-length entries (incl. empty). ---
  std::vector<std::vector<f64>> archive = {{0.1, 0.2, 0.3}, {}, {-1.0}, {2.0, -2.0}};
  auto arch_rt = deserialize_archive(serialize_archive(archive));
  ASSERT_TRUE(arch_rt.has_value());
  ASSERT_EQ(arch_rt.value().size(), archive.size());
  for (atx::usize e = 0; e < archive.size(); ++e) {
    ASSERT_EQ(arch_rt.value()[e].size(), archive[e].size());
    for (atx::usize i = 0; i < archive[e].size(); ++i) {
      EXPECT_EQ(std::bit_cast<u64>(arch_rt.value()[e][i]), std::bit_cast<u64>(archive[e][i]));
    }
  }

  // --- best_fitness_per_gen list. ---
  std::vector<f64> bpg = {0.0, 1.5, 1.5, 2.75, -0.0};
  auto bpg_rt = deserialize_f64_list(serialize_f64_list(bpg));
  ASSERT_TRUE(bpg_rt.has_value());
  ASSERT_EQ(bpg_rt.value().size(), bpg.size());
  for (atx::usize i = 0; i < bpg.size(); ++i) {
    EXPECT_EQ(std::bit_cast<u64>(bpg_rt.value()[i]), std::bit_cast<u64>(bpg[i]));
  }

  // --- empty-collection edges round-trip to empty. ---
  EXPECT_TRUE(deserialize_canon("").value().empty());
  {
    std::vector<u64> ek;
    std::vector<CachedScore> ev;
    ASSERT_TRUE(deserialize_cache("", ek, ev).has_value());
    EXPECT_TRUE(ek.empty());
    EXPECT_TRUE(ev.empty());
  }
  EXPECT_TRUE(deserialize_archive("").value().empty());
  EXPECT_TRUE(deserialize_f64_list("").value().empty());
}

} // namespace atxtest_search_progress_test
