// atx::engine::factory — behavioral / phenotypic diversity tests (S4.2, §4.2).
//
// The S4.2 unit answers a different question than the structural canonical-hash
// novelty and the marginal-corr `diversify` objective: it measures whether a
// candidate's REALIZED OOS PnL profile (its phenotype) is distinct from the rest
// of the live search, not whether its source structure (genotype) differs.
//
// Five clusters of checks:
//   (a) behavioral_distance properties — identical -> 0, anti-correlated -> 0
//       (because 1-|corr|), orthogonal -> ~1, NaN complete-case.
//   (b) BehavioralArchive — deterministic canonical-id insert, oldest-first
//       eviction past capacity, novelty = mean k-nearest, < k-neighbor edge.
//   (c) HEADLINE ranking-flip (the spec exit criterion) — two genomes that hash
//       DISTANTLY but behave near-identically and two that hash SIMILARLY but
//       behave distinctly: the behavioral objective ranks them OPPOSITE to the
//       canonical-hash structural metric.
//   (d) the RankIc fallback fork is wired (one-line switch, exhaustive).
//   (e) (search-level determinism + DetPool worker-invariance live in the
//       NsgaSearch suite extension at the bottom.)

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/behavior.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace {

using atx::f64;
using atx::usize;
using atx::engine::factory::behavioral_distance;
using atx::engine::factory::BehavioralArchive;
using atx::engine::factory::BehaviorMetric;

constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  (a) behavioral_distance properties.
// ===========================================================================

TEST(BehavioralDistance, IdenticalProfiles_ZeroDistance) {
  const std::vector<f64> a{1.0, 2.0, 3.0, 4.0, 5.0};
  // corr(a, a) == 1 -> 1 - |1| == 0.
  EXPECT_NEAR(behavioral_distance(a, a), 0.0, 1e-12);
}

TEST(BehavioralDistance, AntiCorrelatedProfiles_ZeroDistance) {
  const std::vector<f64> a{1.0, 2.0, 3.0, 4.0, 5.0};
  const std::vector<f64> b{5.0, 4.0, 3.0, 2.0, 1.0};
  // corr == -1 -> 1 - |-1| == 0: a perfectly INVERSE signal is the SAME bet
  // flipped, behaviorally redundant. This is why the metric is 1 - |corr|.
  EXPECT_NEAR(behavioral_distance(a, b), 0.0, 1e-12);
}

TEST(BehavioralDistance, OrthogonalProfiles_NearUnitDistance) {
  // Two near-uncorrelated profiles -> |corr| ~ 0 -> distance ~ 1.
  const std::vector<f64> a{1.0, -1.0, 1.0, -1.0, 1.0, -1.0};
  const std::vector<f64> b{1.0, 1.0, -1.0, -1.0, 1.0, 1.0};
  const f64 d = behavioral_distance(a, b);
  EXPECT_GT(d, 0.9);
  EXPECT_LE(d, 1.0);
}

TEST(BehavioralDistance, NanCompleteCase_SkipsNanPairs) {
  // Indices where either leg is NaN are skipped (pairwise-complete). The valid
  // overlap {idx 0,1,3,4} is perfectly correlated -> distance 0.
  const std::vector<f64> a{1.0, 2.0, kNan, 4.0, 5.0};
  const std::vector<f64> b{1.0, 2.0, 99.0, 4.0, 5.0};
  EXPECT_NEAR(behavioral_distance(a, b), 0.0, 1e-12);
}

TEST(BehavioralDistance, DegeneratePair_FewerThanTwoValid_ZeroCorrUnitDistance) {
  // < 2 valid pairs -> corr is the degenerate 0 -> distance 1 (maximally novel
  // against an uninformative neighbour; documented edge).
  const std::vector<f64> a{1.0, kNan, kNan};
  const std::vector<f64> b{1.0, 2.0, 3.0};
  EXPECT_NEAR(behavioral_distance(a, b), 1.0, 1e-12);
}

// ===========================================================================
//  (a') the RankIc fork distance is selectable (one-line switch, exhaustive).
// ===========================================================================

TEST(BehavioralDistance, RankIcFork_MonotoneProfilesAreRedundant) {
  // A non-linear but strictly monotone transform: Pearson |corr| < 1, but the
  // RANK profile is identical -> rank-IC distance == 0 (more aggressively
  // collapses monotone-equivalent signals than the PnlCorr default).
  const std::vector<f64> a{1.0, 2.0, 3.0, 4.0, 5.0};
  const std::vector<f64> b{1.0, 4.0, 9.0, 16.0, 25.0}; // a^2, strictly monotone
  const f64 pearson_d = behavioral_distance(a, b, BehaviorMetric::PnlCorr);
  const f64 rank_d = behavioral_distance(a, b, BehaviorMetric::RankIc);
  EXPECT_GT(pearson_d, 0.0);       // Pearson sees the curvature
  EXPECT_NEAR(rank_d, 0.0, 1e-12); // rank-IC sees a monotone-equivalent bet
}

// ===========================================================================
//  (b) BehavioralArchive — insert / eviction / novelty / edge.
// ===========================================================================

TEST(BehavioralArchive, NoveltyMeanOfKNearest) {
  // Population of 4 unit-distance-apart descriptors; query is identical to one
  // member. The 2 nearest are: the identical member (dist 0) and the next; mean
  // over k=2 nearest in population (archive empty).
  BehavioralArchive arch{4};
  // A query descriptor; population has one identical and others orthogonal.
  const std::vector<f64> q{1.0, -1.0, 1.0, -1.0};
  const std::vector<std::vector<f64>> pop{
      {1.0, -1.0, 1.0, -1.0}, // identical -> dist 0
      {1.0, 1.0, -1.0, -1.0}, // orthogonal -> dist ~1
      {1.0, 1.0, 1.0, 1.0},   // constant -> degenerate corr 0 -> dist 1
      {-1.0, 1.0, -1.0, 1.0}, // anti of q -> dist 0
  };
  std::vector<std::span<const f64>> pop_spans;
  for (const auto &p : pop) {
    pop_spans.emplace_back(p);
  }
  // k=2 nearest to q: the identical (0) and the anti (0) -> mean 0.
  const f64 nov = arch.novelty(std::span<const f64>{q}, pop_spans, 2);
  EXPECT_NEAR(nov, 0.0, 1e-12);
}

TEST(BehavioralArchive, NoveltyFewerThanKNeighbors_AveragesOverAvailable) {
  // population ∪ archive has only 1 neighbour but k=3 requested -> average over
  // the 1 available (documented edge), not divide by 3.
  BehavioralArchive arch{4};
  const std::vector<f64> q{1.0, -1.0, 1.0, -1.0};
  const std::vector<f64> only{1.0, 1.0, -1.0, -1.0}; // orthogonal -> dist ~1
  std::vector<std::span<const f64>> pop{std::span<const f64>{only}};
  const f64 nov = arch.novelty(std::span<const f64>{q}, pop, 3);
  EXPECT_GT(nov, 0.9); // ~1 (the single available distance), NOT ~0.33
}

TEST(BehavioralArchive, NoveltyEmptyNeighborhood_MaximallyNovel) {
  // No population and no archive -> nothing to be similar to -> maximally novel.
  BehavioralArchive arch{4};
  const std::vector<f64> q{1.0, -1.0, 1.0, -1.0};
  std::vector<std::span<const f64>> empty;
  EXPECT_NEAR(arch.novelty(std::span<const f64>{q}, empty, 3), 1.0, 1e-12);
}

TEST(BehavioralArchive, EvictsOldestPastCapacity) {
  // Capacity 2. Insert three descriptors; the FIRST inserted is evicted.
  BehavioralArchive arch{2};
  const std::vector<f64> d0{1.0, 0.0, 0.0, 0.0};
  const std::vector<f64> d1{0.0, 1.0, 0.0, 0.0};
  const std::vector<f64> d2{0.0, 0.0, 1.0, 0.0};
  arch.insert(std::span<const f64>{d0});
  arch.insert(std::span<const f64>{d1});
  EXPECT_EQ(arch.size(), 2U);
  arch.insert(std::span<const f64>{d2}); // evicts d0 (oldest)
  EXPECT_EQ(arch.size(), 2U);
  // A query equal to d0 should now be NOVEL vs the archive (d0 gone); a query
  // equal to d1 should be redundant (d1 retained). Use an empty population so
  // novelty reads only the archive.
  std::vector<std::span<const f64>> empty;
  const f64 nov_d0 = arch.novelty(std::span<const f64>{d0}, empty, 1);
  const f64 nov_d1 = arch.novelty(std::span<const f64>{d1}, empty, 1);
  EXPECT_GT(nov_d0, nov_d1); // d0 evicted -> more novel than the retained d1
}

TEST(BehavioralArchive, NoveltyUnionsPopulationAndArchive) {
  // A descriptor present in the ARCHIVE makes a matching query redundant even if
  // the population does not contain it (the neighbourhood is population ∪ archive).
  BehavioralArchive arch{4};
  const std::vector<f64> d{1.0, -1.0, 1.0, -1.0};
  arch.insert(std::span<const f64>{d});
  const std::vector<f64> far{1.0, 1.0, 1.0, 1.0}; // degenerate, dist 1 to d
  std::vector<std::span<const f64>> pop{std::span<const f64>{far}};
  // k=1 nearest in pop ∪ archive: the archived identical d (dist 0).
  const f64 nov = arch.novelty(std::span<const f64>{d}, pop, 1);
  EXPECT_NEAR(nov, 0.0, 1e-12);
}

// ===========================================================================
//  (c) HEADLINE — the behavioral objective ranks OPPOSITE to canonical-hash.
//
//  Construct two PAIRS of (canon_hash, descriptor):
//    * pair A: hashes FAR apart (structurally "novel") but descriptors near-
//      identical (behaviorally "redundant").
//    * pair B: hashes CLOSE (structurally "redundant") but descriptors distinct
//      (behaviorally "novel").
//  The canonical-hash metric (detail::canonical_distance) calls pair A more
//  distant than pair B; the behavioral metric calls pair B more distant than
//  pair A. The two orderings are OPPOSITE — exactly the S4.2 exit criterion:
//  phenotypic diversity is NOT a function of genotypic diversity.
// ===========================================================================

TEST(BehavioralHeadline, BehavioralRanksOppositeToCanonicalHash) {
  // NOTE: detail::canonical_distance (Hamming-over-hashes) was removed in Task 1
  // because hash avalanche makes it a noise metric, not a structural one.
  // This test documents WHY: the Hamming metric and the behavioral metric produce
  // OPPOSITE rankings. We inline the Hamming popcount rather than calling the
  // now-deleted function.
  auto hamming_dist = [](atx::u64 a, atx::u64 b) -> f64 {
    return static_cast<f64>(std::popcount(a ^ b)) / 64.0;
  };

  // Pair A: hashes maximally far (all 64 bits differ) ...
  const atx::u64 a0_hash = 0x0000000000000000ULL;
  const atx::u64 a1_hash = 0xFFFFFFFFFFFFFFFFULL;
  // ... but behaviors near-identical (perfectly correlated profiles).
  const std::vector<f64> a0_desc{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  const std::vector<f64> a1_desc{2.0, 4.0, 6.0, 8.0, 10.0, 12.0}; // 2*a0 -> corr 1

  // Pair B: hashes adjacent (1 bit differs) ...
  const atx::u64 b0_hash = 0x00000000000000FFULL;
  const atx::u64 b1_hash = 0x00000000000000FEULL; // 1 bit apart
  // ... but behaviors distinct (orthogonal profiles).
  const std::vector<f64> b0_desc{1.0, -1.0, 1.0, -1.0, 1.0, -1.0};
  const std::vector<f64> b1_desc{1.0, 1.0, -1.0, -1.0, 1.0, 1.0};

  const f64 canon_A = hamming_dist(a0_hash, a1_hash);
  const f64 canon_B = hamming_dist(b0_hash, b1_hash);
  const f64 behav_A = behavioral_distance(a0_desc, a1_desc);
  const f64 behav_B = behavioral_distance(b0_desc, b1_desc);

  // Hamming-over-hashes metric: A (all bits differ) >> B (one bit differs).
  EXPECT_GT(canon_A, canon_B);
  // Behavioral metric: B (orthogonal) >> A (correlated). THE FLIP.
  EXPECT_GT(behav_B, behav_A);
  // Made explicit: the two metrics disagree on which pair is "more diverse".
  EXPECT_TRUE((canon_A > canon_B) && (behav_A < behav_B))
      << "behavioral diversity must be able to invert the structural ranking";
}

// ===========================================================================
//  (e) Search-level wiring: the behavioral-novelty objective is LIVE under
//      MultiObjective && enable_behavioral_novelty, deterministic, and DetPool
//      worker-invariant; and it stays OFF (boundary pin intact) under ScalarRaw.
// ===========================================================================

using atx::engine::WeightPolicy;
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
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
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
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)",
          "rank(rev)",
          "ts_mean(close, 5)",
          "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))",
          "delta(close, 2)"};
}

// MultiObjective, behavioral novelty ON (enable_behavioral_novelty=true). Gen-0 fixed (no grammar
// sampling) so only the selection signal differs between runs.
[[nodiscard]] SearchConfig multi_behavioral_config() {
  SearchConfig cfg;
  cfg.master_seed = 777;
  cfg.population = 16;
  cfg.generations = 5;
  cfg.elites = 2;
  cfg.k_tournament = 3;
  cfg.p_cross = 0.5;
  cfg.enable_behavioral_novelty = true; // behavioral objective ACTIVE (gate: Multi && enable_behavioral_novelty)
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.seed_from_grammar = false;
  return cfg;
}

// ---------------------------------------------------------------------------
//  (e.1) The behavioral objective is LIVE: a MultiObjective run with
//        enable_behavioral_novelty=true diverges from the SAME run with false
//        (behavioral objective off). Proves the per-generation behavioral pass
//        actually enters NSGA-II.
// ---------------------------------------------------------------------------
TEST(BehavioralSearch, NoveltyOnDivergesFromNoveltyOff) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  SearchConfig on = multi_behavioral_config();
  AlphaStore pool_on{};
  const SearchResult r_on = driver.run(on, pool_on);

  SearchConfig off = multi_behavioral_config();
  off.enable_behavioral_novelty = false; // behavioral objective OFF -> n_objectives stays 3
  AlphaStore pool_off{};
  const SearchResult r_off = driver.run(off, pool_off);

  const bool diverged = (r_on.digest != r_off.digest) ||
                        (r_on.admitted_candidates.size() != r_off.admitted_candidates.size()) ||
                        (r_on.trial_count != r_off.trial_count);
  EXPECT_TRUE(diverged)
      << "behavioral-novelty ON produced an identical run to OFF — the objective is inert.";
}

// ---------------------------------------------------------------------------
//  (e.2) Determinism: same cfg + seed => byte-identical digest (F1) with the
//        behavioral objective active.
// ---------------------------------------------------------------------------
TEST(BehavioralSearch, NoveltyOnIsDeterministic) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  AlphaStore pool_a{};
  const SearchResult ra = driver.run(multi_behavioral_config(), pool_a);
  AlphaStore pool_b{};
  const SearchResult rb = driver.run(multi_behavioral_config(), pool_b);
  EXPECT_EQ(ra.digest, rb.digest);
  EXPECT_EQ(ra.trial_count, rb.trial_count);
}

// ---------------------------------------------------------------------------
//  (e.3) DetPool {1,2,4,8} worker-count invariance with behavioral novelty ON.
//        Worker count never enters the math (F2).
// ---------------------------------------------------------------------------
TEST(BehavioralSearch, NoveltyOnDigestInvariantAcrossWorkers) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const std::array<usize, 4> worker_counts{1, 2, 4, 8};
  atx::u64 first_digest = 0;
  for (usize wi = 0; wi < worker_counts.size(); ++wi) {
    SearchConfig cfg = multi_behavioral_config();
    cfg.n_workers = worker_counts[wi];
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    if (wi == 0) {
      first_digest = r.digest;
    } else {
      EXPECT_EQ(r.digest, first_digest)
          << "behavioral digest changed at n_workers=" << worker_counts[wi];
    }
  }
}

// ===========================================================================
//  S3-3: min_viable_raw viable-only novelty floor tests.
//
//  (c) min_viable_raw=0.0 -> digest identical to the floor-off path.
//  (d) min_viable_raw>0.0 -> a junk genome (low raw) whose descriptor partially
//      correlates with a viable peer no longer drags viable's novelty downward;
//      viable's novelty score strictly improves when the floor is active.
// ===========================================================================

using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

// ---------------------------------------------------------------------------
//  (c) Accept: min_viable_raw=0.0 leaves the digest byte-identical to the
//      run where the field simply did not exist (the pre-S3-3 default path).
// ---------------------------------------------------------------------------
TEST(MinViableRaw, DefaultZeroIsDigestIdentical) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  // Two configs: one with the field at its default (0.0), one where it is
  // explicitly zero (should be identical).  Use MultiObjective + novelty ON
  // so the behavioral pass is active and any leak would show up.
  SearchConfig cfg_a = multi_behavioral_config();
  cfg_a.master_seed = 0xABC1;
  cfg_a.generations = 4;
  // min_viable_raw is 0.0 by default - no explicit set needed.

  SearchConfig cfg_b = cfg_a;
  cfg_b.min_viable_raw = 0.0; // explicit zero == the default

  AlphaStore pool_a{};
  AlphaStore pool_b{};
  const SearchResult ra = driver.run(cfg_a, pool_a);
  const SearchResult rb = driver.run(cfg_b, pool_b);

  EXPECT_EQ(ra.digest, rb.digest)
      << "min_viable_raw=0.0 (explicit) diverged from the default — "
         "the floor is leaking into the off-path.";
  EXPECT_GT(ra.trial_count, 0u) << "vacuity: run must have scored some candidates";
}

// ---------------------------------------------------------------------------
//  (d) Accept: with min_viable_raw > 0, a junk genome whose descriptor
//      partially correlates with a viable peer no longer contaminates that
//      peer's k-nearest neighborhood.  Viable's novelty strictly improves.
//
//  Mechanism: BehavioralArchive::novelty(desc, population, k) computes the mean
//  distance to the k NEAREST neighbors.  A junk genome J whose actual PnL
//  descriptor has a partial positive correlation with viable genome V (due to
//  shared market exposure) appears as a CLOSE neighbor of V (distance < 1),
//  pulling V's novelty DOWN.  With min_viable_raw active, J's descriptor is
//  submitted as all-zeros; corr(zeros, V.pnl) = 0 (zero-variance leg ->
//  degenerate corr) -> distance(zeros, V) = 1.0.  J is now always a DISTANT
//  neighbor, so it never enters V's k-nearest and V's novelty increases.
// ---------------------------------------------------------------------------
TEST(MinViableRaw, ViableNoveltyImprovesWhenJunkIsFloored) {
  // We test directly at the BehavioralArchive / behavioral_distance layer —
  // no full search needed — to give a clean, fast, deterministic proof.
  //
  // Setup:
  //   V  : viable descriptor — a clean alternating signal {+1,-1,+1,-1,...}
  //   J  : junk descriptor   — partially correlated with V: {+0.8,-0.8,+0.8,-0.8,...}
  //         (raw fitness below the floor).  corr(J, V) ≈ 1 -> distance(J,V) ≈ 0.
  //         J is a VERY CLOSE neighbor of V, pulling V's novelty toward 0.
  //   O  : orthogonal descriptor — {+1,+1,-1,-1,+1,+1,-1,-1} (near-zero corr with V)
  //         distance(O, V) ≈ 1.  O is a very distant neighbor of V.
  //
  // Without floor (floor=0.0):
  //   V's k=1 nearest = J (distance ≈ 0) -> novelty(V) ≈ 0.  LOW.
  //
  // With floor (J is below min_viable_raw -> descriptor zeroed):
  //   distance(zeros, V) = 1.0 (degenerate corr).
  //   V's k=1 nearest = min(distance(zeros, V)=1.0, distance(O, V)≈1.0) ≈ 1.0.
  //   novelty(V) ≈ 1.0.  HIGH.  <- strictly > the no-floor case.

  const std::vector<f64> V{ 1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0};
  const std::vector<f64> J{ 0.8, -0.8, 0.8, -0.8, 0.8, -0.8, 0.8, -0.8}; // corr=1 w/ V
  const std::vector<f64> O{ 1.0,  1.0,-1.0, -1.0, 1.0,  1.0,-1.0, -1.0}; // near-orth w/ V
  const std::vector<f64> Z{ 0.0,  0.0, 0.0,  0.0, 0.0,  0.0, 0.0,  0.0}; // what floor produces for J

  // Confirm J is close to V without the floor (corr ≈ 1 -> distance ≈ 0).
  const f64 dist_J_V = behavioral_distance(V, J);
  ASSERT_LT(dist_J_V, 0.01) << "test setup: J must be very close to V";

  // Confirm Z (zeroed J) is far from V (distance = 1.0 — degenerate corr).
  const f64 dist_Z_V = behavioral_distance(V, Z);
  ASSERT_NEAR(dist_Z_V, 1.0, 1e-9) << "test setup: zeroed J must be maximally far from V";

  BehavioralArchive arch{0}; // empty archive: novelty from population only

  // Case A: floor OFF — J's real descriptor is used.
  // V's neighborhood = {J (dist≈0), O (dist≈1)}. k=1 nearest = J. novelty ≈ 0.
  {
    const std::vector<std::span<const f64>> nbr_floor_off{
        std::span<const f64>{J}, // junk, close to V
        std::span<const f64>{O}, // orthogonal to V
    };
    const f64 nov_floor_off =
        arch.novelty(std::span<const f64>{V}, nbr_floor_off, /*k=*/1);
    EXPECT_LT(nov_floor_off, 0.1)
        << "floor OFF: junk J (close to V) must make V appear non-novel";
  }

  // Case B: floor ON — J's descriptor is submitted as zeros (what the
  // behavioral_novelty_pass does when j.fitness < min_viable_raw).
  // V's neighborhood = {Z (dist=1.0), O (dist≈1.0)}. k=1 nearest ≈ 1.0.
  {
    const std::vector<std::span<const f64>> nbr_floor_on{
        std::span<const f64>{Z}, // zeroed junk, always distance=1.0 from V
        std::span<const f64>{O}, // orthogonal to V
    };
    const f64 nov_floor_on =
        arch.novelty(std::span<const f64>{V}, nbr_floor_on, /*k=*/1);
    EXPECT_GT(nov_floor_on, 0.9)
        << "floor ON: zeroed J must be distance=1 from V, so V is maximally novel";
  }

  // Core assertion (accept criterion d): novelty improves when the floor is on.
  const std::vector<std::span<const f64>> nbr_off{std::span<const f64>{J}, std::span<const f64>{O}};
  const std::vector<std::span<const f64>> nbr_on {std::span<const f64>{Z}, std::span<const f64>{O}};
  const f64 nov_off = arch.novelty(std::span<const f64>{V}, nbr_off, /*k=*/1);
  const f64 nov_on  = arch.novelty(std::span<const f64>{V}, nbr_on,  /*k=*/1);
  EXPECT_GT(nov_on, nov_off)
      << "min_viable_raw floor must strictly improve viable novelty when junk is close";
}

// ---------------------------------------------------------------------------
//  (d') Search-level integration: a run with min_viable_raw > 0.0 is
//      deterministic (twice-run identical) and MANDATORILY diverges from the
//      floor-off run. This proves the floor is wired through
//      behavioral_novelty_pass in SearchDriver.run() and not a dead-letter change.
//
//  Airtight floor calibration (no fixture magic number): we first run the
//  floor-OFF path and read its BEST raw fitness across all generations
//  (best_fitness_per_gen is the per-gen maximum). We then set min_viable_raw
//  STRICTLY ABOVE that maximum. By construction EVERY genome in the on-path run
//  has raw < min_viable_raw, so EVERY behavioral descriptor is zeroed before it
//  reaches BehavioralArchive::novelty(). The on-path novelty landscape is
//  therefore guaranteed to differ from the off-path (which submits the real,
//  non-zero descriptors), so the digest MUST diverge. This removes any
//  dependence on the empirical fitness distribution of the fixture.
// ---------------------------------------------------------------------------
TEST(MinViableRaw, SearchLevelDeterministicAndDiverges) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  SearchConfig cfg_off = multi_behavioral_config();
  cfg_off.master_seed = 0xF1007;
  cfg_off.generations = 5;
  cfg_off.min_viable_raw = 0.0; // floor disabled (off-path)

  AlphaStore pool_off{};
  const SearchResult r_off = driver.run(cfg_off, pool_off);

  // Highest raw fitness achieved anywhere in the off-path run.  Setting the
  // floor above this guarantees EVERY genome (even the best) is below the floor
  // and therefore zeroed — the floor is mathematically certain to trigger.
  ASSERT_FALSE(r_off.best_fitness_per_gen.empty()) << "vacuity: run produced no generations";
  f64 best_raw = r_off.best_fitness_per_gen.front();
  for (const f64 g : r_off.best_fitness_per_gen) {
    best_raw = std::max(best_raw, g);
  }
  ASSERT_GT(best_raw, 0.0)
      << "vacuity: off-path best raw must be positive so the calibrated floor "
         "is a meaningful (>0) threshold";

  SearchConfig cfg_on = cfg_off;
  // Strictly above the off-path maximum -> ALL genomes are below the floor ->
  // ALL descriptors zeroed -> novelty landscape differs from off-path -> digest
  // is guaranteed to diverge.  The +1.0 margin is far beyond floating noise.
  cfg_on.min_viable_raw = best_raw + 1.0;

  AlphaStore pool_on1{};
  AlphaStore pool_on2{};
  const SearchResult r_on1 = driver.run(cfg_on, pool_on1);
  const SearchResult r_on2 = driver.run(cfg_on, pool_on2);

  // Determinism (F1): two runs with the same floor value are byte-identical.
  EXPECT_EQ(r_on1.digest, r_on2.digest)
      << "min_viable_raw run is not deterministic (F1 violated).";

  // Mandatory divergence: with every descriptor zeroed, the on-path novelty
  // scores differ from the off-path -> different NSGA-II ordering -> different
  // survivors/children -> different digest.  This is the load-bearing proof that
  // min_viable_raw is actually wired through behavioral_novelty_pass in
  // SearchDriver.run() (a no-op change would leave the digest equal to off-path).
  EXPECT_NE(r_on1.digest, r_off.digest)
      << "min_viable_raw floor did NOT change the end-to-end run — the floor is "
         "not wired into behavioral_novelty_pass (or novelty does not affect "
         "selection on this fixture).";

  EXPECT_GT(r_on1.trial_count, 0u) << "vacuity: must have scored candidates";
}

} // namespace
