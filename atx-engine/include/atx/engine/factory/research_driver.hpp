#pragma once

// atx::engine::factory â€” ResearchDriver: the continuous automated alpha engine
// (S4b-4). This is the ACROSS-RUN orchestration layer â€” it sits above Factory
// exactly as Factory sits above SearchDriver.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  S4b-3 built Factory::mine_into(cfg, library, gate) â€” ONE mine -> deflate ->
//  library::admit run into the persistent library. ResearchDriver owns the
//  borrowed library::Library and drives a BUDGET-BOUNDED mine -> admit -> REPEAT
//  loop over a FIXED research panel, growing the persistent, deduplicated library
//  across runs until a stop condition. Each iteration is a complete mine_into call
//  (with its own per-run seed); the library deduplicates and grows in place, so a
//  motif already admitted in an earlier run is rejected (Duplicate) in a later one
//  â€” the library IS the accumulating across-run memory.
//
// ===========================================================================
//  The loop (authoritative)
// ===========================================================================
//   rep.seed = cfg.master_seed; digest_acc seeded from cfg.master_seed.
//   for run in [0, max_runs) while (patience == 0 || dry < patience):
//     run_cfg = cfg.per_run; run_cfg.search.master_seed = seed_for_run(seed, run)
//     R2: if per_run.oos_n_windows > 0: run_cfg.oos_window = run % oos_n_windows
//     fr = factory.mine_into(run_cfg, library, gate)   // S4b-3
//     rep.runs = run+1; accumulate mined/admitted/duplicates; fold fr.digest
//     dry = (fr.admitted == 0) ? dry+1 : 0             // novelty-exhaustion counter
//   m = library.snapshot(); rep.manifest_version_id = m.version_id;
//   rep.library_size = library.n_alphas(); build lifecycle_histogram from m.entries.
//
// ===========================================================================
//  F1 â€” SEED BY ID, end to end. The engine-level seed axis is (master_seed, run);
//  the per-run seed is detail::seed_for_run(master_seed, run) (a pure SplitMix mix,
//  mirroring SearchDriver's seed_for, NO worker/thread/time/atomic). That run seed
//  becomes the SearchConfig.master_seed mine_into hands to SearchDriver, which
//  derives its own (gen, idx) stream via seed_for â€” so the FULL deterministic seed
//  axis is (master_seed, run, gen, idx) WITHOUT touching SearchDriver. Same
//  ResearchConfig + same starting library => byte-identical rep.digest AND
//  rep.manifest_version_id (the per-run FactoryReport digests folded in run order,
//  then the content-addressed manifest version_id over the grown library).
//
//  The engine digest is the SAME hash_combine fold the Factory digest uses
//  (atx::core::hash_combine, cast to u64), seeded from cfg.master_seed and folded
//  with each run's FactoryReport.digest in run order â€” so a different per-run
//  outcome (a different admit set) shifts the engine digest, and an identical
//  mine+admit sequence replays byte-identical.
//
//  Header-only; every function inline. mine_into is a COLD path (one search +
//  admit per run), so the per-run std::vector / Factory allocation is acceptable
//  (the VM hot loop is untouched â€” F8).

#include <array>  // std::array (lifecycle_histogram)
#include <vector> // std::vector (S4.5 robustness-gate regime labels)

#include "atx/core/types.hpp" // atx::u32, atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/panel.hpp"        // alpha::Panel
#include "atx/engine/alpha/registry.hpp"     // alpha::Library
#include "atx/engine/combine/gate.hpp"       // combine::AlphaGate
#include "atx/engine/eval/regime_slice.hpp"  // eval::RobustnessConfig (S4.5 gate seam)
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/library/library.hpp" // library::Library, LibraryManifest

#include "atx/engine/factory/factory.hpp" // factory::Factory, FactoryConfig, FactoryReport

// C2.1 — fwd decl for the OPTIONAL parallel substrate ResearchConfig threads
// through (a pointer field ⇒ a forward decl suffices; no heavy include). factory.hpp
// already fwd-declares this type, but we restate it here so research_driver.hpp is
// self-documenting about the seam it carries.
namespace atx::engine::parallel {
class IExecutor;
} // namespace atx::engine::parallel

namespace atx::engine::factory {

// =========================================================================
//  ResearchConfig â€” the engine knobs (the across-run budget + stop condition).
//
//  per_run     : the inner mine config (search budget per run). Its
//                per_run.search.master_seed is OVERWRITTEN per run with
//                detail::seed_for_run(master_seed, run) (the run's seed axis).
//  max_runs    : a HARD budget â€” the upper bound on the number of mine_into runs.
//  patience    : early-stop after this many CONSECUTIVE zero-admit runs (novelty
//                exhaustion: the library has absorbed everything the grammar finds
//                novel against the fixed panel). 0 DISABLES the early stop (run the
//                full max_runs regardless).
//  master_seed : the engine root entropy; the per-run seed is
//                detail::seed_for_run(master_seed, run). Surfaced as rep.seed.
// =========================================================================
//  robustness_gate : the S4.4b SEAM that S4.5 flips ON. DEFAULT-OFF and digest-
//                    NEUTRAL: when false (the default), run() takes EXACTLY today's
//                    path â€” same admissions, same digest, same manifest_version_id
//                    â€” because the only code it guards is skipped (no compute, no
//                    RNG, no ordering change, no digest fold). When true, S4.5
//                    measures a RobustnessVerdict per admitted survivor (over the
//                    SealedPanel.visible() region, via eval/regime_slice.hpp) and
//                    report-only-records or rejects non-robust survivors. S4.4b
//                    ships ONLY the off seam; the live gating is S4.5's.
//  robustness_cfg  : the RobustnessConfig the gate WOULD use (unused while
//                    robustness_gate is false â€” a pure config slot here).
struct ResearchConfig {
  FactoryConfig per_run;       // inner mine config (per_run.search.master_seed is overwritten)
  atx::usize max_runs{1};      // hard upper bound on runs
  atx::usize patience{0};      // stop after this many consecutive zero-admit runs (0 disables)
  atx::u64 master_seed{0};     // engine seed; per-run seed = seed_for_run(master_seed, run)
  bool robustness_gate{false}; // S4.5 seam; OFF == today's exact path (S4.4b)
  eval::RobustnessConfig robustness_cfg{}; // knobs the S4.5 gate would screen on (unused while OFF)
  // C2.1 — OPTIONAL parallel substrate. nullptr (default) ⇒ each run uses the serial
  // factory.mine_into(run_cfg, lib_, gate_) path, BYTE-IDENTICAL to today (this field is
  // NOT folded into the digest). When non-null, each run dispatches the substrate-aware
  // factory.mine_into(run_cfg, lib_, gate_, *exec); the engine guarantees that path is
  // bit-identical to serial (mine_into_oos_parallel == mine_into_oos), so rep.digest /
  // manifest_version_id are unchanged across substrate + worker count.
  parallel::IExecutor* exec{nullptr};
};

// =========================================================================
//  ResearchReport â€” the engine's across-run summary.
//
//  runs               : the number of mine_into runs actually executed (<= max_runs).
//  total_mined        : sum of FactoryReport.evaluated over the runs (distinct
//                       candidates scored â€” the multiple-testing N each run deflated).
//  total_admitted     : sum of FactoryReport.admitted (alphas inserted into the library).
//  total_duplicates   : sum of FactoryReport.duplicates (library-wide F6 dedup hits â€”
//                       a motif an earlier run already admitted).
//  library_size       : library.n_alphas() at engine end (== Accept admits, no decay yet).
//  lifecycle_histogram: count of alphas per LifecycleState (0..5), from the FINAL
//                       manifest entries (entries[i].lifecycle_at_snapshot). Every
//                       freshly-admitted alpha is in Admitted (index 1).
//  dedup_pct          : total_duplicates / max(1, total_admitted + total_duplicates)
//                       â€” the fraction of NON-rejected (gate-passing) candidates the
//                       library deduplicated as already-known. Denominator excludes
//                       gate/deflation rejects (they are not dedup events): it is the
//                       share of "would-be-admits" that collided with the library.
//                       max(1, .) guards an all-zero run (=> 0.0).
//  digest             : the engine fingerprint â€” each run's FactoryReport.digest
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
  atx::u64 seed{0};                   // == master_seed
  bool robustness_gate_active{false}; // ECHO of cfg.robustness_gate (S4.4b seam; OFF by default).
                                      // NOT folded into digest â€” a report-only flag, so toggling
                                      // the seam never shifts the engine fingerprint.
  // S4.5 robustness-gate telemetry (additive; default 0 so the gate-OFF path and every
  // pre-S4.5 test are byte-untouched). Populated ONLY when cfg.robustness_gate is true:
  // robust_screened counts survivors run through the per-survivor RobustnessVerdict over
  // the visible panel (every admit of every run), robust_passed counts those whose
  // worst_regime AND worst_window OOS Sharpe both clear robustness_cfg.min_regime_sharpe.
  // When the gate is ON these counts AND each survivor's verdict are folded into `digest`
  // (so the ON-path fingerprint is S4.5's own â€” the boundary pin keeps the gate OFF).
  atx::usize robust_screened{0};
  atx::usize robust_passed{0};
  // M1/sweep: the OOS-per-alpha metrics from the LAST mine_into run. Populated
  // whenever the last run produced oos_metrics; empty when OOS is off. Used by
  // stage_sweep to write IS/OOS columns to _manifest.txt in discover format.
  std::vector<factory::OosReportEntry> last_run_oos_metrics;
  // M1/sweep R2 telemetry: the oos_window value used for EACH run, in run order.
  // Populated by ResearchDriver::run — entry[i] is the run_cfg.oos_window used for
  // run i (after the `run % oos_n_windows` assignment when oos_n_windows > 0; the
  // unchanged cfg.per_run.oos_window when oos_n_windows == 0).
  // REPORT-ONLY: NOT folded into `digest` (mirrors last_run_oos_metrics). Adding this
  // field leaves the digest, admitted set, and library state completely unchanged —
  // the oos_n_windows==0 path stays byte-identical to the pre-M1 engine.
  std::vector<atx::usize> per_run_oos_window;

  // C2.2 measurement-only (REPORT-ONLY; NOT folded into `digest`). Cross-run redundant
  // EVALUATION: across the runs, how many distinct-per-run scored structures were ALREADY
  // scored in an earlier run (the VM work a cross-run scored-cache would save). total =
  // sum of per-run distinct-scored counts; distinct = |union of all runs' scored hashes|;
  // redundant = total - distinct; pct = redundant / max(1,total).
  atx::usize cross_run_total_evals{0};
  atx::usize cross_run_distinct_evals{0};
  atx::usize cross_run_redundant_evals{0};
  atx::f64   cross_run_redundant_pct{0.0};
};

namespace detail {

// seed_for_run â€” the F1 per-RUN seed derivation. A fixed SplitMix-style mix of
// (master_seed, run); pure, portable (no std::hash / platform-width hash), and
// depends on NOTHING else (never worker/thread/time/atomic/address). Mirrors
// SearchDriver's detail::seed_for constants/style exactly (the same golden-ratio
// avalanche + the same two odd multipliers), but over the (master, run) axis only.
//
// This composes with SearchDriver's existing (gen, idx) derivation: the value
// returned here is fed in as SearchConfig.master_seed, which seed_for then mixes
// with (gen, idx) â€” giving the full (master_seed, run, gen, idx) seed axis WITHOUT
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
//  ResearchDriver â€” the continuous mine -> admit -> repeat engine (S4b-4).
//
//  Borrows the persistent library::Library + the run-wide DSL alpha::Library +
//  Panel + ExecutionSimulator + WeightPolicy + AlphaGate for the engine's lifetime
//  (every run() call). The DSL Library owns every OpSig the mined genomes' Expr::op
//  pointers alias (genome.hpp SAFETY); it MUST outlive the engine and every produced
//  genome. The library::Library is GROWN in place across runs. The Panel is FIXED
//  (one research panel for the whole engine â€” the across-run dedup is meaningful only
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
  [[nodiscard]] ResearchReport run(const ResearchConfig &cfg);

private:
  // S4.5 robustness gate (run() fill point). Screen the survivors a single run
  // admitted â€” ids [n_before, n_after), the half-open range mine_into reports â€” with
  // a per-survivor eval::RobustnessVerdict over their stored OOS PnL (sliced by the
  // visible-panel vol-tercile `labels` + the walk-forward windows). For each survivor
  // it bumps rep.robust_screened, bumps rep.robust_passed when the verdict clears the
  // floor, and folds (worst_regime_sharpe, worst_window_sharpe, is_robust) into
  // `digest_acc` IN ASCENDING ID ORDER â€” so turning the gate ON shifts the engine
  // fingerprint deterministically (S4.5 owns the ON-path digest) while the gate-OFF
  // path never calls this and stays byte-identical. `labels` is the vol-tercile
  // partition of panel_ (computed ONCE per run() call). PURE: no RNG, reads only the
  // caller's library spans within each iteration (no store growth). Returns the
  // updated accumulator (the caller threads it back into digest_acc).
  [[nodiscard]] atx::u64 screen_run_robustness(const FactoryReport &fr,
                                               const std::vector<atx::u8> &labels,
                                               const eval::RobustnessConfig &robustness_cfg,
                                               atx::u64 digest_acc, ResearchReport &rep) const;

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
