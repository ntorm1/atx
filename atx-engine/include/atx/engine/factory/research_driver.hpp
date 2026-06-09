#pragma once

// atx::engine::factory — ResearchDriver: the continuous automated alpha engine
// (S4b-4). This is the ACROSS-RUN orchestration layer — it sits above Factory
// exactly as Factory sits above SearchDriver.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  S4b-3 built Factory::mine_into(cfg, library, gate) — ONE mine -> deflate ->
//  library::admit run into the persistent library. ResearchDriver owns the
//  borrowed library::Library and drives a BUDGET-BOUNDED mine -> admit -> REPEAT
//  loop over a FIXED research panel, growing the persistent, deduplicated library
//  across runs until a stop condition. Each iteration is a complete mine_into call
//  (with its own per-run seed); the library deduplicates and grows in place, so a
//  motif already admitted in an earlier run is rejected (Duplicate) in a later one
//  — the library IS the accumulating across-run memory.
//
// ===========================================================================
//  The loop (authoritative)
// ===========================================================================
//   rep.seed = cfg.master_seed; digest_acc seeded from cfg.master_seed.
//   for run in [0, max_runs) while (patience == 0 || dry < patience):
//     run_cfg = cfg.per_run; run_cfg.search.master_seed = seed_for_run(seed, run)
//     fr = factory.mine_into(run_cfg, library, gate)   // S4b-3
//     rep.runs = run+1; accumulate mined/admitted/duplicates; fold fr.digest
//     dry = (fr.admitted == 0) ? dry+1 : 0             // novelty-exhaustion counter
//   m = library.snapshot(); rep.manifest_version_id = m.version_id;
//   rep.library_size = library.n_alphas(); build lifecycle_histogram from m.entries.
//
// ===========================================================================
//  F1 — SEED BY ID, end to end. The engine-level seed axis is (master_seed, run);
//  the per-run seed is detail::seed_for_run(master_seed, run) (a pure SplitMix mix,
//  mirroring SearchDriver's seed_for, NO worker/thread/time/atomic). That run seed
//  becomes the SearchConfig.master_seed mine_into hands to SearchDriver, which
//  derives its own (gen, idx) stream via seed_for — so the FULL deterministic seed
//  axis is (master_seed, run, gen, idx) WITHOUT touching SearchDriver. Same
//  ResearchConfig + same starting library => byte-identical rep.digest AND
//  rep.manifest_version_id (the per-run FactoryReport digests folded in run order,
//  then the content-addressed manifest version_id over the grown library).
//
//  The engine digest is the SAME hash_combine fold the Factory digest uses
//  (atx::core::hash_combine, cast to u64), seeded from cfg.master_seed and folded
//  with each run's FactoryReport.digest in run order — so a different per-run
//  outcome (a different admit set) shifts the engine digest, and an identical
//  mine+admit sequence replays byte-identical.
//
//  Header-only; every function inline. mine_into is a COLD path (one search +
//  admit per run), so the per-run std::vector / Factory allocation is acceptable
//  (the VM hot loop is untouched — F8).

#include <array>   // std::array (lifecycle_histogram)
#include <cstddef> // std::size_t (hash_combine seed type)

#include "atx/core/hash.hpp"  // atx::core::hash_combine (engine digest fold)
#include "atx/core/types.hpp" // atx::u32, atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/panel.hpp"        // alpha::Panel
#include "atx/engine/alpha/registry.hpp"     // alpha::Library
#include "atx/engine/combine/gate.hpp"       // combine::AlphaGate
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/library/library.hpp"   // library::Library, LibraryManifest
#include "atx/engine/library/lifecycle.hpp" // library::LifecycleState (histogram cardinality)
#include "atx/engine/library/manifest.hpp"  // library::ManifestEntry

#include "atx/engine/factory/factory.hpp" // factory::Factory, FactoryConfig, FactoryReport

namespace atx::engine::factory {

// =========================================================================
//  ResearchConfig — the engine knobs (the across-run budget + stop condition).
//
//  per_run     : the inner mine config (search budget per run). Its
//                per_run.search.master_seed is OVERWRITTEN per run with
//                detail::seed_for_run(master_seed, run) (the run's seed axis).
//  max_runs    : a HARD budget — the upper bound on the number of mine_into runs.
//  patience    : early-stop after this many CONSECUTIVE zero-admit runs (novelty
//                exhaustion: the library has absorbed everything the grammar finds
//                novel against the fixed panel). 0 DISABLES the early stop (run the
//                full max_runs regardless).
//  master_seed : the engine root entropy; the per-run seed is
//                detail::seed_for_run(master_seed, run). Surfaced as rep.seed.
// =========================================================================
struct ResearchConfig {
  FactoryConfig per_run;   // inner mine config (per_run.search.master_seed is overwritten)
  atx::usize max_runs{1};  // hard upper bound on runs
  atx::usize patience{0};  // stop after this many consecutive zero-admit runs (0 disables)
  atx::u64 master_seed{0}; // engine seed; per-run seed = seed_for_run(master_seed, run)
};

// =========================================================================
//  ResearchReport — the engine's across-run summary.
//
//  runs               : the number of mine_into runs actually executed (<= max_runs).
//  total_mined        : sum of FactoryReport.evaluated over the runs (distinct
//                       candidates scored — the multiple-testing N each run deflated).
//  total_admitted     : sum of FactoryReport.admitted (alphas inserted into the library).
//  total_duplicates   : sum of FactoryReport.duplicates (library-wide F6 dedup hits —
//                       a motif an earlier run already admitted).
//  library_size       : library.n_alphas() at engine end (== Accept admits, no decay yet).
//  lifecycle_histogram: count of alphas per LifecycleState (0..5), from the FINAL
//                       manifest entries (entries[i].lifecycle_at_snapshot). Every
//                       freshly-admitted alpha is in Admitted (index 1).
//  dedup_pct          : total_duplicates / max(1, total_admitted + total_duplicates)
//                       — the fraction of NON-rejected (gate-passing) candidates the
//                       library deduplicated as already-known. Denominator excludes
//                       gate/deflation rejects (they are not dedup events): it is the
//                       share of "would-be-admits" that collided with the library.
//                       max(1, .) guards an all-zero run (=> 0.0).
//  digest             : the engine fingerprint — each run's FactoryReport.digest
//                       folded in run order into an accumulator seeded from
//                       master_seed (F1). Same config + same starting library =>
//                       byte-identical digest.
//  manifest_version_id: the content-address of the FINAL library snapshot (F1).
//  seed               : == master_seed (the artifact key).
// =========================================================================
struct ResearchReport {
  atx::usize runs{0};
  atx::usize total_mined{0};
  atx::usize total_admitted{0};
  atx::usize total_duplicates{0};
  atx::u64 library_size{0};
  std::array<atx::usize, 6> lifecycle_histogram{}; // by LifecycleState (final manifest)
  atx::f64 dedup_pct{0.0};                         // see field doc for the exact definition
  atx::u64 digest{0};                              // per-run digests folded in run order (F1)
  atx::u32 manifest_version_id{0};
  atx::u64 seed{0}; // == master_seed
};

namespace detail {

// seed_for_run — the F1 per-RUN seed derivation. A fixed SplitMix-style mix of
// (master_seed, run); pure, portable (no std::hash / platform-width hash), and
// depends on NOTHING else (never worker/thread/time/atomic/address). Mirrors
// SearchDriver's detail::seed_for constants/style exactly (the same golden-ratio
// avalanche + the same two odd multipliers), but over the (master, run) axis only.
//
// This composes with SearchDriver's existing (gen, idx) derivation: the value
// returned here is fed in as SearchConfig.master_seed, which seed_for then mixes
// with (gen, idx) — giving the full (master_seed, run, gen, idx) seed axis WITHOUT
// touching SearchDriver. Distinct runs get well-separated RNG streams.
[[nodiscard]] inline atx::u64 seed_for_run(atx::u64 master, atx::u64 run) noexcept {
  auto mix = [](atx::u64 x) noexcept -> atx::u64 {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
  };
  atx::u64 h = mix(master);
  h = mix(h ^ (run + 0x9E3779B97F4A7C15ULL));
  return h;
}

} // namespace detail

// =========================================================================
//  ResearchDriver — the continuous mine -> admit -> repeat engine (S4b-4).
//
//  Borrows the persistent library::Library + the run-wide DSL alpha::Library +
//  Panel + ExecutionSimulator + WeightPolicy + AlphaGate for the engine's lifetime
//  (every run() call). The DSL Library owns every OpSig the mined genomes' Expr::op
//  pointers alias (genome.hpp SAFETY); it MUST outlive the engine and every produced
//  genome. The library::Library is GROWN in place across runs. The Panel is FIXED
//  (one research panel for the whole engine — the across-run dedup is meaningful only
//  against a stable panel). None of the borrows is copied or stored by value.
// =========================================================================
class ResearchDriver {
public:
  // SAFETY: `lib`, `dsl`, `panel`, `sim`, `policy`, `gate` are BORROWED for the
  // engine's lifetime (and every run() call). `lib` (the persistent library) is grown
  // in place. `dsl` (the run-wide alpha::Library) owns the op rows every mined
  // genome's Expr::op aliases and MUST outlive the engine and all produced genomes.
  // `panel` is the FIXED research panel. The engine stores only references.
  ResearchDriver(library::Library &lib, const alpha::Library &dsl, const alpha::Panel &panel,
                 const exec::ExecutionSimulator &sim, const WeightPolicy &policy,
                 const combine::AlphaGate &gate) noexcept
      : lib_{lib}, dsl_{dsl}, panel_{panel}, sim_{sim}, policy_{policy}, gate_{gate} {}

  // Drive the budget-bounded mine -> admit -> repeat loop. Deterministic: same cfg +
  // same starting library contents => byte-identical rep.digest AND
  // rep.manifest_version_id (F1). Stops at max_runs, or early once `patience`
  // consecutive runs admit nothing (novelty exhaustion).
  [[nodiscard]] ResearchReport run(const ResearchConfig &cfg) {
    ResearchReport rep;
    rep.seed = cfg.master_seed;

    // Engine digest accumulator seeded from the engine seed (the SAME hash_combine
    // the Factory digest uses, cast to u64). Each run's FactoryReport.digest is
    // folded in below in run order (F1).
    atx::u64 digest_acc = static_cast<atx::u64>(
        atx::core::hash_combine(std::size_t{0}, cfg.master_seed));

    // One Factory over the FIXED panel — reused across every run (it carries no
    // per-run state; a fresh seeded SearchDriver is built inside each mine_into).
    Factory factory{dsl_, panel_, sim_, policy_};

    atx::usize dry = 0; // consecutive zero-admit runs (the patience counter)
    for (atx::usize run = 0; run < cfg.max_runs && (cfg.patience == 0U || dry < cfg.patience);
         ++run) {
      // Overwrite the per-run search seed with the (master_seed, run) derivation.
      FactoryConfig run_cfg = cfg.per_run;
      run_cfg.search.master_seed = detail::seed_for_run(cfg.master_seed, run);

      const FactoryReport fr = factory.mine_into(run_cfg, lib_, gate_);

      rep.runs = run + 1U;
      rep.total_mined += fr.evaluated;
      rep.total_admitted += fr.admitted;
      rep.total_duplicates += fr.duplicates;

      // Fold this run's deterministic mine+admit fingerprint into the engine digest
      // (in run order, so a reordered or differing run sequence shifts the digest).
      digest_acc = static_cast<atx::u64>(
          atx::core::hash_combine(static_cast<std::size_t>(digest_acc), fr.digest));

      // Novelty-exhaustion counter: a zero-admit run is "dry"; `patience` consecutive
      // dry runs trip the early stop. A run that admits ANYTHING resets the counter.
      dry = (fr.admitted == 0U) ? (dry + 1U) : 0U;
    }

    // Snapshot the FINAL library: flushes + seals every staged alpha and computes the
    // content-addressed version_id (the manifest entries carry the per-alpha lifecycle).
    const library::LibraryManifest m = lib_.snapshot();
    rep.manifest_version_id = m.version_id;
    rep.library_size = lib_.n_alphas();

    // Lifecycle histogram from the FINAL manifest entries (NOT a journal query). Each
    // entry's lifecycle_at_snapshot is a LifecycleState 0..5; bounds-guard the cast so
    // an out-of-range value (impossible for valid enumerators) can never index OOB.
    for (const library::ManifestEntry &e : m.entries) {
      const atx::usize bucket = static_cast<atx::usize>(e.lifecycle_at_snapshot);
      if (bucket < rep.lifecycle_histogram.size()) {
        ++rep.lifecycle_histogram[bucket];
      }
    }

    // dedup_pct: the share of gate-passing "would-be-admits" the library recognized as
    // already-known (Duplicate). Denominator is (admitted + duplicates) — gate/deflation
    // rejects are excluded (they are not dedup events). max(1, .) guards an all-zero run.
    const atx::usize denom = rep.total_admitted + rep.total_duplicates;
    rep.dedup_pct = (denom == 0U)
                        ? 0.0
                        : static_cast<atx::f64>(rep.total_duplicates) /
                              static_cast<atx::f64>(denom);

    rep.digest = digest_acc;
    return rep;
  }

private:
  // SAFETY: each borrow is held for the engine's lifetime. lib_ is grown in place;
  // dsl_ owns every OpSig the mined genomes' Expr::op pointers alias and must outlive
  // the engine and all produced genomes (genome.hpp SAFETY). panel_ is the fixed
  // research panel; sim_/policy_/gate_ are the run-wide screens.
  library::Library &lib_;
  const alpha::Library &dsl_;
  const alpha::Panel &panel_;
  const exec::ExecutionSimulator &sim_;
  const WeightPolicy &policy_;
  const combine::AlphaGate &gate_;
};

} // namespace atx::engine::factory
