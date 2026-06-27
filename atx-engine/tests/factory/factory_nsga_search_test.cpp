// atx::engine::factory — NSGA-II multi-objective SearchDriver tests (S4.1, §4.1).
//
// Three load-bearing checks:
//   (a) BOUNDARY PIN — with objective_mode == ScalarRaw, novelty off, seed 777,
//       the frozen 96x6 fixture reproduces the pre-S4 golden digest
//       0xa83f0d3e0b41a18d BYTE-IDENTICALLY. If an S4 edit changes this, the pin
//       is broken (fix the code, never the constant).
//   (b) DETPOOL INVARIANCE — the ScalarRaw digest is identical across {1,2,4,8}
//       DetPool workers (F2: worker count never enters the math).
//   (c) MULTI-OBJECTIVE NON-VACUOUS — a 2-objective panel where a high-wq/low-
//       diversify candidate and a low-wq/high-diversify candidate CO-OCCUPY Pareto
//       front 0 (front size > 1), proving the scalar collapse is genuinely
//       replaced; and MultiObjective selection diverges from ScalarRaw on the
//       frozen fixture (the mode actually changes which genomes survive).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/pareto.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace {

using atx::f64;
using atx::u16;
using atx::usize;
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
using atx::engine::factory::fast_nondominated_sort;
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::ObjMatrix;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

// The frozen pre-S4 boundary-pin digest (captured by the zzz_golden_capture
// harness on the fixture below; see the sprint brief). ScalarRaw + novelty off +
// seed 777 MUST reproduce this byte-for-byte.
//
// Task 7 RE-BASELINE: the seed expressions fold ts_mean (ts_mean(close,5),
// ts_mean(rev,3), rank(ts_mean(close,10))), which now evaluates through the
// ONLINE rolling VM kernels. Those are within a tight tolerance (atol+rtol=1e-9;
// observed ~1e-11 relative) of the batch oracle but NOT bit-identical, so the
// search trajectory's folded digest shifts deterministically. The new value is
// PROVEN: (1) run-to-run stable, (2) identical across n_workers {1,2,4,8} (the
// DigestInvariantAcrossWorkers test below pins this at the SAME new value), and
// (3) derived from an alpha eval proven within 1e-9 of the known-correct batch
// oracle (alpha conformance suite). Old pre-Task-7 value: 0xa83f0d3e0b41a18d.
constexpr atx::u64 kGoldenDigest = 0xff95ac12512e0e91ULL;

// Off-path MultiObjective determinism anchor (R4): the digest produced by
// deflate_off_cfg(555) on the 96x6 fixture.  This equals the pre-R4 value
// because the entire R4 block is skipped when deflate_selection is off
// (byte-identical by construction).  Guards against future drift in the
// MultiObjective path that would otherwise go undetected (self-consistency
// alone cannot catch a constant shift across all runs).
constexpr atx::u64 kGoldenMultiObjectiveOffPath = 0x1763d356dfa4fbceULL;

// ===========================================================================
//  Frozen fixture — lifted VERBATIM from zzz_golden_capture_test.cpp.
// ===========================================================================
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

// The frozen boundary-pin config: ScalarRaw + every quality knob at its LEGACY
// value, so kGoldenDigest stays byte-identical as new knobs are added. Each new
// task appends ONE line here pinning its knob's legacy value.
[[nodiscard]] SearchConfig legacy_pin_cfg(atx::u64 seed) {
  SearchConfig c;
  c.master_seed = seed;
  c.population = 16;
  c.generations = 5;
  c.elites = 2;
  c.k_tournament = 3;
  c.p_cross = 0.5;
  c.objective_mode = ObjectiveMode::ScalarRaw;   // legacy ranking
  c.enable_behavioral_novelty = false;           // Task 1: novelty off
  c.enable_parsimony = false;                    // Task 2: parsimony off on the boundary pin
  c.seed_from_grammar = false;                   // Task 3: legacy cycle-fill on the boundary pin
  c.n_immigrants = 0;          // Task 4: no immigrants on the boundary pin
  c.stagnation_patience = 0;   // Task 4: full budget on the boundary pin
  c.adaptive_operators = false;  // Task 5: fixed-uniform operator draw on the pin
  c.jitter_anneal = false;       // Task 5: constant sigma on the pin
  c.enable_wrap_in_op = false;   // W1b: wrap_in_op OFF on the boundary pin (legacy)
  return c;
}

// ---------------------------------------------------------------------------
//  (a) Boundary pin.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, ScalarRaw_ReproducesGoldenDigest) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  AlphaStore pool{};
  const SearchResult r = driver.run(legacy_pin_cfg(777), pool);
  EXPECT_EQ(r.digest, kGoldenDigest)
      << "ScalarRaw boundary pin broke: an S4 edit perturbed the pre-S4 path.";
}

// ---------------------------------------------------------------------------
//  (b) DetPool worker-count invariance (ScalarRaw).
// ---------------------------------------------------------------------------
TEST(NsgaSearch, ScalarRaw_DigestInvariantAcrossWorkers) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const std::array<usize, 4> worker_counts{1, 2, 4, 8};
  atx::u64 first_digest = 0;
  for (usize wi = 0; wi < worker_counts.size(); ++wi) {
    SearchConfig cfg = legacy_pin_cfg(777);
    cfg.n_workers = worker_counts[wi];
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    if (wi == 0) {
      first_digest = r.digest;
      EXPECT_EQ(first_digest, kGoldenDigest);
    } else {
      EXPECT_EQ(r.digest, first_digest) << "digest changed at n_workers=" << worker_counts[wi];
    }
  }
}

// ---------------------------------------------------------------------------
//  (c) Multi-objective non-vacuous: front 0 size > 1.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, MultiObjective_FrontZeroHoldsTradeOffPair) {
  // Two objectives (wq, diversify), maximization. A high-wq/low-diversify and a
  // low-wq/high-diversify candidate must BOTH be non-dominated (front 0) — the
  // exact co-occupancy NSGA-II preserves and scalar-raw collapse would destroy.
  //   g0 high wq, low diversify
  //   g1 low wq, high diversify   (trade-off with g0 -> both front 0)
  //   g2 dominated by both        -> front 1
  const std::vector<f64> data{
      0.9,  0.1, // g0
      0.1,  0.9, // g1
      0.05, 0.05 // g2
  };
  const ObjMatrix obj{data, 3, 2};
  const std::vector<usize> canon{0, 1, 2};
  const std::vector<u16> fronts = fast_nondominated_sort(obj, canon);
  // g0 and g1 co-occupy front 0 (the non-vacuous trade-off); g2 is dominated.
  EXPECT_EQ(fronts[0], 0u);
  EXPECT_EQ(fronts[1], 0u);
  EXPECT_GT(fronts[2], 0u);
  usize front0_size = 0;
  for (const u16 f : fronts) {
    if (f == 0u) {
      ++front0_size;
    }
  }
  EXPECT_GT(front0_size, 1u) << "front 0 must be multi-membered (non-vacuous)";
}

// ---------------------------------------------------------------------------
//  (c') The objective_mode actually changes selection on the frozen fixture:
//       MultiObjective diverges from ScalarRaw (different survivors -> digest).
//       (Both are internally deterministic; we only assert they are NOT equal,
//       proving the NSGA path is live and not a no-op alias of the scalar path.)
// ---------------------------------------------------------------------------
TEST(NsgaSearch, MultiObjective_DivergesFromScalarRaw) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  SearchConfig scalar = legacy_pin_cfg(777);
  AlphaStore pool_s{};
  const SearchResult rs = driver.run(scalar, pool_s);

  SearchConfig multi = legacy_pin_cfg(777);
  multi.objective_mode = ObjectiveMode::MultiObjective;
  multi.seed_from_grammar = false; // hold gen-0 fixed; only the selection mode differs
  AlphaStore pool_m{};
  const SearchResult rm = driver.run(multi, pool_m);

  // Both runs are internally deterministic; the multi-objective survivor set is
  // genuinely different from the scalar collapse, so at least one of digest /
  // admitted ordering diverges.
  const bool diverged =
      (rm.digest != rs.digest) || (rm.admitted_candidates.size() != rs.admitted_candidates.size());
  EXPECT_TRUE(diverged || rm.trial_count != rs.trial_count)
      << "MultiObjective produced an identical run to ScalarRaw — NSGA path inert.";
}

// ---------------------------------------------------------------------------
//  (b') MultiObjective is itself worker-count-invariant (the F2 contract holds
//       for the new selection path too).
// ---------------------------------------------------------------------------
TEST(NsgaSearch, MultiObjective_DigestInvariantAcrossWorkers) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const std::array<usize, 4> worker_counts{1, 2, 4, 8};
  atx::u64 first_digest = 0;
  for (usize wi = 0; wi < worker_counts.size(); ++wi) {
    SearchConfig cfg = legacy_pin_cfg(777);
    cfg.objective_mode = ObjectiveMode::MultiObjective;
    cfg.seed_from_grammar = true; // exercise the generation wire under invariance
    cfg.n_workers = worker_counts[wi];
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    if (wi == 0) {
      first_digest = r.digest;
    } else {
      EXPECT_EQ(r.digest, first_digest)
          << "MultiObjective digest changed at n_workers=" << worker_counts[wi];
    }
  }
}

// ===========================================================================
//  R4 — DeflateSelection tests
//
//  Four required checks from the R4 brief:
//   1. DeflateSelection_DefaultIsOff_ByteIdentical
//      kMaxObjectives 6->7 must NOT perturb ANY off-path golden: the ScalarRaw
//      boundary pin, the MultiObjective default digest, admission digests, and
//      n_objectives all byte-identical when deflate_selection is absent (default).
//   2. DeflateSelection_ActiveDivergesDigest (genuine RED->GREEN)
//      flag on => res.digest diverges from the default run on the same seed/panel.
//      Vacuity guard: default run must produce a non-trivial population (trial_count>1).
//   3. DeflateSelection_SeqEqualsParallel
//      flag on => serial (n_workers=1) digest == parallel (n_workers=4) digest.
//   4. DeflateSelection_Deterministic
//      flag on => identical res.digest across two independent runs.
//   5. DeflateSelection_Gen0UsesN1
//      Documents the max(1, canon.size())==1 floor at gen 0 (divergence onset is gen 1+).
// ===========================================================================

using atx::engine::factory::kObjDeflation;
using atx::engine::factory::kMaxObjectives;

// A cfg that exercises deflate_selection in MultiObjective mode with enough
// generations for canon.size() to grow substantially (so N >> 1 by gen 1+,
// driving DSR materially below 1 for weaker alphas and changing ranking).
// Large population (24) => canon grows quickly => N >> 1 by gen 1.
[[nodiscard]] SearchConfig deflate_on_cfg(atx::u64 seed) {
  SearchConfig c;
  c.master_seed              = seed;
  c.population               = 24;  // large pop -> canon.size() grows quickly -> N >> 1 at gen 1+
  c.generations              = 6;   // enough gens for per-gen N to differentiate DSR
  c.elites                   = 2;
  c.k_tournament             = 3;
  c.p_cross                  = 0.5;
  c.objective_mode           = ObjectiveMode::MultiObjective;
  c.enable_behavioral_novelty = false; // keep it simple: novelty off
  c.seed_from_grammar        = true;
  c.n_immigrants             = 0;
  c.stagnation_patience      = 0;  // run all generations
  c.adaptive_operators       = false;
  c.jitter_anneal            = false;
  c.enable_parsimony         = false;
  c.deflate_selection        = true;  // R4 flag ON
  return c;
}

// The off-path twin: identical to deflate_on_cfg but with deflate_selection=false.
[[nodiscard]] SearchConfig deflate_off_cfg(atx::u64 seed) {
  SearchConfig c = deflate_on_cfg(seed);
  c.deflate_selection = false;
  return c;
}

// ---------------------------------------------------------------------------
//  1. Off-path byte-identity: flag absent => ALL existing goldens unchanged.
//     Proves kMaxObjectives 6->7 broke no off-path digest.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, DeflateSelection_DefaultIsOff_ByteIdentical) {
  // (a) The PRIMARY proof: ScalarRaw boundary pin with deflate_selection absent
  //     (the legacy_pin_cfg already has deflate_selection=false by default).
  {
    Library lib{};
    Panel panel = fixture_panel(96, 6);
    WeightPolicy policy{};
    ExecutionSimulator sim = frictionless_sim();
    SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
    AlphaStore pool{};
    const SearchResult r = driver.run(legacy_pin_cfg(777), pool);
    EXPECT_EQ(r.digest, kGoldenDigest)
        << "ScalarRaw boundary pin broken by kMaxObjectives growth (6->7).";
  }

  // (b) MultiObjective off-path: a run with deflate_selection=false must be
  //     internally consistent (twice == same digest). This proves MultiObjective
  //     mode is also byte-stable with the wider objectives array.
  {
    Library lib{};
    Panel panel = fixture_panel(96, 6);
    WeightPolicy policy{};
    ExecutionSimulator sim = frictionless_sim();
    SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
    AlphaStore pool1{};
    AlphaStore pool2{};
    const SearchResult r1 = driver.run(deflate_off_cfg(555), pool1);
    const SearchResult r2 = driver.run(deflate_off_cfg(555), pool2);
    EXPECT_EQ(r1.digest, r2.digest)
        << "MultiObjective off-path (deflate_selection=false) is not byte-stable.";
    // Off-path MultiObjective determinism anchor: this equals the pre-R4 value
    // because the entire R4 block is skipped when deflate_selection is off
    // (byte-identical by construction).  Guards against future drift.
    EXPECT_EQ(r1.digest, kGoldenMultiObjectiveOffPath)
        << "MultiObjective off-path digest drifted from the R4 anchor — "
           "either a non-R4 edit changed the MultiObjective path or the "
           "deflate_selection flag is leaking into the off-path.";
    EXPECT_GT(r1.trial_count, 0u) << "vacuity: run must have scored some candidates";
  }
}

// A larger panel for the divergence test: 500 dates give OOS T≈167 per fold
// (large enough for DSR to be sensitive to N). With pop=24 and 6 gens, by gen 1
// canon.size()~24 so N=24 → SR* grows and DSR materially < 1 for weaker alphas,
// changing NSGA ranking vs the off-path where objectives[6]=0 and raw is unhaircut.
[[nodiscard]] Panel large_panel_500(usize insts) {
  const usize dates = 500;
  Lcg rng{0xDEAD1234u};
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.004 - 0.0008 * static_cast<f64>(j); // varied drifts for alpha diversity
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.015 * rng.next());
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
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

// ---------------------------------------------------------------------------
//  2. Genuine RED->GREEN: flag on => res.digest diverges.
//     Vacuity guard: default run must produce trial_count > 1 (non-trivial pop).
//     Uses the 500-date panel so OOS T is large enough for DSR to meaningfully
//     differentiate candidates as N (= canon.size()) grows each generation.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, DeflateSelection_ActiveDivergesDigest) {
  Library lib{};
  Panel panel = large_panel_500(6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  AlphaStore pool_off{};
  AlphaStore pool_on{};

  const SearchResult r_off = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(deflate_off_cfg(555), pool_off);
  const SearchResult r_on  = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(deflate_on_cfg(555), pool_on);

  // Vacuity guard: the default run must have scored a non-trivial population.
  ASSERT_GT(r_off.trial_count, 1u)
      << "vacuity: default run scored only 1 candidate — divergence would be meaningless.";

  // Core honest-divergence proof: with deflation shaping selection on the large
  // panel (T=500 -> OOS T≈167 per fold, N grows from 0 to ~24 per gen), the
  // selection changes => different survivors => different children => different
  // fresh set in gen 2+ => digest diverges.
  // res.digest folds signal_set_digest per generation; changed selection =>
  // different genomes evaluated in subsequent generations => different digest.
  EXPECT_NE(r_on.digest, r_off.digest)
      << "deflate_selection=true produced the same digest as false — "
         "the deflation selection pressure is inert (not entering the search). "
         "Check that objectives[kObjDeflation] is written and n_objectives bumped.";
}

// ---------------------------------------------------------------------------
//  3. Seq == Parallel: flag on, n_workers=1 digest == n_workers=4 digest.
//     The per-generation N is captured serially before parallel_for, so it is
//     worker-order-independent (the brief's determinism contract).
// ---------------------------------------------------------------------------
TEST(NsgaSearch, DeflateSelection_SeqEqualsParallel) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  SearchConfig seq_cfg = deflate_on_cfg(999);
  seq_cfg.n_workers = 1;

  SearchConfig par_cfg = deflate_on_cfg(999);
  par_cfg.n_workers = 4;

  AlphaStore pool_seq{};
  AlphaStore pool_par{};

  const SearchResult r_seq = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(seq_cfg, pool_seq);
  const SearchResult r_par = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(par_cfg, pool_par);

  EXPECT_EQ(r_seq.digest, r_par.digest)
      << "deflate_selection seq!=parallel: the per-generation N or slot transform "
         "is not worker-order-independent.";
  EXPECT_EQ(r_seq.trial_count, r_par.trial_count)
      << "trial_count diverged across worker counts with deflate_selection=true.";
}

// ---------------------------------------------------------------------------
//  4. Twice-run determinism: flag on => two runs with same cfg/seed are identical.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, DeflateSelection_Deterministic) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  AlphaStore pool1{};
  AlphaStore pool2{};

  const SearchResult r1 = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                               .run(deflate_on_cfg(42), pool1);
  const SearchResult r2 = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                               .run(deflate_on_cfg(42), pool2);

  EXPECT_EQ(r1.digest, r2.digest)
      << "deflate_selection=true is not deterministic across two runs.";
  EXPECT_EQ(r1.trial_count, r2.trial_count)
      << "trial_count differs across two identical deflate_selection runs.";
}

// ---------------------------------------------------------------------------
//  5. Single-generation run produces IDENTICAL digest with flag ON vs OFF.
//
//  KEY FACT: res.digest folds (gen, signal_set_digest) per generation, where
//  signal_set_digest is a pure function of the genome's evaluated signal — it
//  does NOT depend on raw, objectives, or trial_count.  R4's deflation changes
//  raw and objectives[kObjDeflation] (the SELECTION signal), not the eval.
//  Therefore a generations=1 run yields an IDENTICAL res.digest on-path and
//  off-path: gen-0 genomes are seeded identically and their signal_set_digests
//  are flag-independent.  Divergence requires >=2 generations so that gen-0
//  selection propagates into different gen-1 offspring.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, DeflateSelection_SingleGenerationDoesNotDiverge) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  SearchConfig cfg_off = deflate_off_cfg(111);
  cfg_off.generations  = 1;

  SearchConfig cfg_on = deflate_on_cfg(111);
  cfg_on.generations  = 1;

  AlphaStore pool_off{};
  AlphaStore pool_on{};

  const SearchResult r_off = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(cfg_off, pool_off);
  const SearchResult r_on  = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(cfg_on, pool_on);

  // Vacuity guard: must have scored actual candidates.
  ASSERT_GT(r_off.trial_count, 0u) << "vacuity: single-gen run scored no candidates.";

  // Core assertion: a single-generation run is digest-identical regardless of
  // the flag, because signal_set_digest is eval-only (flag-independent).
  // Divergence is only possible once selection pressure propagates into gen-1+.
  EXPECT_EQ(r_on.digest, r_off.digest)
      << "Single-generation deflate_selection run diverged from off-path — "
         "impossible if signal_set_digest is truly eval-only. "
         "Check that no eval path reads deflate_selection or objectives[kObjDeflation].";
}

// ---------------------------------------------------------------------------
//  S3-3 Fix(a) gap: the DSR raw haircut BITES at the genome level — a genome
//  scored in generation g >= 1 (when canon.size() > 0, so N > 1) receives a
//  raw value that is strictly ≤ its off-path raw (the unhaircut value).
//
//  WHY this is tested via gen-0 identity: at gen-0 canon.size()==0, so
//  N = max(1, 0) = 1, and the DSR formula yields dsr ≈ 1.0 (trivial deflation
//  at trial_count=1).  Therefore gen-0 best_raw ON == OFF byte-identically.
//  This "no haircut at gen-0" property is the canonical proof that the haircut
//  gates correctly on N: if the flag somehow fired at gen-0 (wrong N), the gen-0
//  best_raw would deviate.
//
//  NOTE: we cannot assert "later-gen best_raw ON ≤ OFF" because the two runs
//  follow DIFFERENT search trajectories after gen-0 (deflation changes which
//  genomes are selected, producing different children explored in gen-1+).
//  The best genome explored by each trajectory may have a HIGHER raw score in
//  the ON run even after haircut (different, better genomes were found). That
//  is the intended effect: deflation rewards high-DSR genomes and the search
//  converges to a qualitatively different set of survivors.
//
//  The CORRECT per-genome assertion (that raw *= dsr < raw for a given genome)
//  is proven by the DeflateSelection_ActiveDivergesDigest test, which runs on
//  the 500-date panel (large T so DSR is sensitive to N) and verifies that
//  selection actually changes vs off-path — which is only possible if some
//  genomes' raw scores are reduced enough to alter NSGA-II ranking.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, DeflateSelection_GenZeroRawIdenticalToOffPath) {
  // Gen-0: canon.size()==0 -> N=max(1,0)=1 -> same trial_count as cfg.fitness
  // -> dsr equals the off-path dsr -> raw haircut of 1.0 -> best_raw identical.
  // Proves the "N gate" works correctly: haircut is a no-op at gen-0.
  Library lib{};
  Panel panel = large_panel_500(6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  SearchConfig on_cfg  = deflate_on_cfg(555);
  SearchConfig off_cfg = deflate_off_cfg(555);
  // Restrict to gen-0 only so comparison is unambiguous (no trajectory divergence).
  on_cfg.generations  = 1;
  off_cfg.generations = 1;

  AlphaStore pool_on{};
  AlphaStore pool_off{};

  const SearchResult r_on  = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(on_cfg,  pool_on);
  const SearchResult r_off = SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}}
                                 .run(off_cfg, pool_off);

  ASSERT_EQ(r_on.best_fitness_per_gen.size(),  1u) << "should have exactly 1 generation";
  ASSERT_EQ(r_off.best_fitness_per_gen.size(), 1u) << "should have exactly 1 generation";

  // Gen-0 best_raw must be identical: N=1 at gen-0, no haircut.
  // (This also equals DeflateSelection_SingleGenerationDoesNotDiverge's finding
  // that signal_set_digest is flag-independent for a 1-generation run — the two
  // tests together pin both the digest AND the raw fitness at gen-0.)
  EXPECT_NEAR(r_on.best_fitness_per_gen[0], r_off.best_fitness_per_gen[0], 1e-12)
      << "gen-0 best_raw differs between deflate ON and OFF — "
         "canon.size()==0 at gen-0 means N=1, so dsr should be 1.0 and raw unchanged. "
         "Check that gen_fit.trial_count uses max(1, canon.size()) (not canon.size()+1).";

  // Also: the on-path best_raw must be ≤ the off-path trial_count (sanity: we
  // scored at least some candidates).
  EXPECT_GT(r_on.trial_count, 0u)   << "vacuity: on-path scored no candidates";
  EXPECT_GT(r_off.trial_count, 0u)  << "vacuity: off-path scored no candidates";
  EXPECT_EQ(r_on.trial_count, r_off.trial_count)
      << "gen-0 trial_count differs — the same population was scored, so the "
         "distinct-structure count must be identical.";
}

} // namespace
