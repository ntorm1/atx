#pragma once

// atx::engine::factory — Factory: the mine -> gate -> admit capstone (S3-6, plan
// §4.8). This is the FINAL integration unit of Sprint 3: it wires the S3-5
// SearchDriver (the seeded, deflated, pool-aware evolutionary search) into the P4
// AlphaGate admission screen + the S1 deflation bar, and returns the FactoryReport
// the sprint exit criteria + the S1 DSR/PBO accounting read.
//
// ===========================================================================
//  §4.8 — the mine -> gate -> admit loop (authoritative)
// ===========================================================================
//   res := run_search(cfg.search, pool)            // S3-5 SearchDriver
//   for cand in res.ranked_by_deflated_fitness():  // best DEFLATED first
//     cand_pnl := full_oos_pnl(cand)               // computed BEFORE insert (§0.6)
//     metrics  := compute_metrics(cand_pnl, cand_pos, n_inst, book)
//     verdict  := gate.admit(metrics, cand_pnl, pool)   // P4 fitness/turnover/MAX-corr
//     if verdict == Accept and cand.dsr >= cfg.min_dsr: // AND the S1 deflation bar
//       pool.insert(source, cand_pnl, cand_pos, metrics)
//       admitted += 1
//   return { admitted, evaluated = res.trial_count, dedup_pct, cse_pct, trials, seed }
//
// ===========================================================================
//  §0.6 — the DANGLING-SPAN discipline (load-bearing)
// ===========================================================================
//  AlphaStore::pnl() returns a span ALIASING the backing vector; it DANGLES after
//  the next insert() (store.hpp BORROW LIFETIME). BOTH pool_aware_fitness's
//  corr-to-pool AND AlphaGate::admit's max-corr-to-pool read those member spans.
//  So for each candidate we compute EVERYTHING that reads the pool (the deflated
//  fitness `dsr`, the OOS PnL/positions/metrics, the gate verdict) FIRST, and only
//  then — once no live span aliases the pool — insert(). The candidate's own
//  cand_pnl/cand_pos buffers are OWNED std::vectors (copies from extract_streams),
//  so they survive the insert that consumes them.
//
// ===========================================================================
//  RANK-BY-DEFLATED-FITNESS — re-scored against the RUNNING pool
// ===========================================================================
//  The search ranks candidates by RAW fitness (its maximized signal). Admission
//  ranks by DEFLATED fitness (§4.8): we re-run pool_aware_fitness on each distinct
//  scored genome ONCE up front (against the pool as it stands at run start) to
//  obtain its `dsr`/`raw`, sort DESCENDING by (dsr, raw) — the best-deflated
//  candidate is screened first — and then walk that order. Because admission
//  GROWS the pool, the gate's corr-to-pool and each later candidate's
//  diversification are re-evaluated against the CURRENT pool at the moment that
//  candidate is screened (a fresh pool_aware_fitness call inside the loop), so the
//  marginal-contribution thesis (a redundant survivor is rejected once an
//  equivalent one is already admitted) actually bites. The trial count N fed to
//  the deflation is res.trial_count (the search's distinct-candidate count — the
//  multiple-testing N the S1 DSR/PBO accounting expects).
//
// ===========================================================================
//  ISignalSource ownership decision (documented)
// ===========================================================================
//  A mined candidate is a Genome (Ast + Analysis), not an ISignalSource. AlphaStore
//  stores a NON-OWNING ISignalSource* purely so P4-5's CombinedSignalSource can
//  RE-EVALUATE a constituent point-in-time; store.hpp explicitly permits nullptr
//  for a caller that does not exercise re-evaluation. S3-6 mining does NOT
//  re-evaluate — it inserts the already-realized OOS PnL/position streams it just
//  computed — so we insert source = nullptr (the phase4_bench / store unit-test
//  precedent). The only concrete ISignalSource adapter (loop::VmSignalSource) is
//  hardwired to the loop's OHLCV PanelView, not the research alpha::Panel, so
//  fabricating one over a non-OHLCV research panel would be both wrong (field
//  mismatch) and a lifetime hazard. If a later sprint needs a re-evaluable handle,
//  it must build a research-panel ISignalSource adapter; that is out of S3-6 scope.
//
// ===========================================================================
//  cse_pct source (documented, NOT fabricated)
// ===========================================================================
//  §4.8 asks for "mean Program.cache_hit_pct() over generations". SearchResult
//  does NOT surface the per-generation compiled Programs (the driver compiles
//  single-root Programs internally and folds only their eval DIGEST). The reachable
//  CSE telemetry is Program::cache_hit_pct() on the run's distinct scored genomes
//  (res.all_scored): we re-compile each and average its cache_hit_pct(). For the
//  single-root seed grammar this measures intra-expression structural sharing (it
//  is typically small — the cross-alpha CSE lever is exercised by compile_batch,
//  not the per-genome single-root compile the driver uses), but it is a REAL
//  measurement off the as-built Program telemetry, not a fabricated constant. An
//  empty run (no scored genomes / all uncompilable) yields cse_pct = 0.
//
//  Header-only; every function inline. mine() is a COLD path (run once per search),
//  so std::vector / per-candidate allocation is acceptable (the VM hot loop is
//  untouched — F8).

#include <array>  // std::array (reject_histogram, indexed by library::AdmitKind)
#include <string> // std::string (seed-expression / field source)
#include <vector> // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err
#include "atx/core/types.hpp" // atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/panel.hpp"   // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/streams.hpp" // alpha::extract_streams, AlphaStreams

#include "atx/engine/combine/gate.hpp"       // combine::AlphaGate, GateVerdict, GateConfig
#include "atx/engine/combine/store.hpp"      // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/library/library.hpp" // library::Library, AlphaCandidate, AdmitKind, AdmitVerdict

#include "atx/engine/factory/fitness.hpp" // factory::pool_aware_fitness, FitnessCfg
#include "atx/engine/factory/genome.hpp"  // factory::Genome
#include "atx/engine/factory/pool_view.hpp" // factory::LibraryPool, PoolView, pool_aware_fitness overload (S4b-2)
#include "atx/engine/factory/search_driver.hpp" // factory::SearchDriver, SearchConfig, SearchResult

namespace atx::engine::parallel {
class IExecutor; // S7.5d substrate seam (fwd-declared; the .cpp pulls the full header)
} // namespace atx::engine::parallel

namespace atx::engine::factory {

// =========================================================================
//  FactoryConfig — the mine() knobs (§4.8).
//
//  The Factory ctor takes (lib, panel, sim, weight_policy) — the run-wide borrows
//  the SearchDriver needs. The PER-RUN search inputs (the SearchConfig knobs PLUS
//  the seed-expression templates and the field-swap candidate names the
//  SearchDriver ctor requires) live HERE in the cfg passed to mine(), so a single
//  Factory can mine different grammars/budgets without reconstruction. min_dsr is
//  the S1 deflation admission bar (F4): a candidate must clear BOTH the P4 gate AND
//  cand.dsr >= min_dsr to be admitted.
// =========================================================================
struct FactoryConfig {
  SearchConfig search{};                 // the S3-5 search budget + CPCV/deflation geometry
  std::vector<std::string> seed_exprs;   // in-grammar starting templates (SearchDriver ctor)
  std::vector<std::string> panel_fields; // field-swap candidate names (SearchDriver ctor)
  atx::f64 min_dsr = 0.5;                // S1 deflation bar (F4): admit iff dsr >= this
  atx::f64 book_size = 1.0;              // notional divisor for compute_metrics turnover
  // --- P2a out-of-sample (holdout) validation (additive; 0.0 == OFF, default).
  //  When oos_fraction > 0, mine_into SELECTS on a TRAIN window [0, lockbox_begin -
  //  embargo) but CONFIRMS the AlphaGate floors + the DSR bar on the HELD-OUT
  //  terminal window [lockbox_begin, T) the search never optimized on, and persists
  //  the HOLDOUT (admission) metrics. The terminal `oos_fraction` of dates is held
  //  out (eval::reserve_lockbox geometry); 0.0 keeps the legacy path byte-identical.
  atx::f64 oos_fraction = 0.0;           // terminal holdout fraction; 0 == OFF (legacy path)
  // Embargo gap fraction inserted BEFORE the holdout (eval::reserve_lockbox). 0 ⇒
  //  the eval::CpcvConfig default embargo when oos is on. Ignored when oos is off.
  atx::f64 oos_embargo = 0.0;
};

// =========================================================================
//  FactoryReport — the §4.8 return value (the sprint exit-criteria fields).
//
//  admitted   : candidates that cleared BOTH the P4 gate AND the dsr bar (inserted).
//  evaluated  : res.trial_count — distinct candidates scored (feeds S1 DSR/PBO N).
//  dedup_pct  : 1 - trial_count/candidates_generated (the F6 dedup lever).
//  cse_pct    : mean Program::cache_hit_pct() over the run's distinct scored genomes
//               (see the header cse_pct note — a real, reachable measurement).
//  trials     : == evaluated (the trial count, surfaced under its §4.8 name too).
//  seed       : res.seed == cfg.search.master_seed (the artifact key).
//  digest     : the F1/F2 byte-identical run fingerprint — the search digest FOLDED
//               with every admission decision (so two runs that mine + admit
//               identically replay to the same digest; F1/F2).
// =========================================================================
// =========================================================================
//  OosReportEntry — per-admitted-alpha IS+OOS metrics for the impl manifest
//  (P2a; additive). Surfaces BOTH the TRAIN (in-sample) metrics and the HOLDOUT
//  (out-of-sample / admission) metrics WITHOUT changing the persistent .alib
//  layout (the library stores ONE AlphaMetrics per alpha — the holdout/admission
//  metrics). Populated ONLY by the oos branch; default-empty on the legacy path.
// =========================================================================
struct OosReportEntry {
  atx::u64 canon_hash{0};            // the F6 dedup key (matches the library record)
  combine::AlphaMetrics is_metrics{};  // TRAIN-window realized metrics (reporting only)
  combine::AlphaMetrics oos_metrics{}; // HOLDOUT-window metrics (what was GATED + persisted)
};

struct FactoryReport {
  atx::usize admitted{0};
  atx::usize evaluated{0};
  atx::f64 dedup_pct{0.0};
  atx::f64 cse_pct{0.0};
  atx::usize trials{0};
  atx::u64 seed{0};
  atx::u64 digest{0};

  // --- S4b-3 mine_into() telemetry (additive; default-init so mine() is untouched).
  // These fields are populated ONLY by mine_into (the persistent-library admit path);
  // the ephemeral-AlphaStore mine() leaves them at their defaults.
  atx::usize duplicates{0};                     // library-wide F6 dedup hits (AdmitKind::Duplicate)
  atx::u64 library_n_alphas_before{0};          // library::n_alphas() at run start
  atx::u64 library_n_alphas_after{0};           // library::n_alphas() at run end
  std::array<atx::usize, 6> reject_histogram{}; // count per library::AdmitKind (0..5)

  // --- P2a OOS telemetry (additive; default-EMPTY so the legacy path is byte-
  //  identical). One entry per admitted alpha when oos_fraction > 0: its IS (train)
  //  and OOS (holdout) metrics. P2b (impl) reads this for the discover manifest.
  std::vector<OosReportEntry> oos_metrics;
};

// =========================================================================
//  Factory — the mine -> gate -> admit capstone (§4.8).
//
//  Borrows the run-wide Library + Panel + ExecutionSimulator + WeightPolicy for its
//  lifetime (the SearchDriver + every fitness/stream eval borrow them). The single
//  Library owns every OpSig row the genomes' Expr::op pointers alias; it MUST
//  outlive the Factory and every produced genome (genome.hpp SAFETY).
// =========================================================================
class Factory {
public:
  // SAFETY: `lib`, `panel`, `sim`, `policy` are BORROWED for the Factory's lifetime
  // (and every mine() call). The single run-wide Library owns the op rows every
  // genome's Expr::op aliases; it must outlive the Factory and all produced genomes.
  // The ctor signature matches the S3-6 verbatim tests: Factory(lib, panel, sim,
  // weight_policy). The SearchDriver's extra ctor inputs (seed_exprs, panel_fields)
  // come from the per-run cfg passed to mine(), not the ctor (see FactoryConfig).
  Factory(const alpha::Library &lib, const alpha::Panel &panel, const exec::ExecutionSimulator &sim,
          const WeightPolicy &policy) noexcept
      : lib_{lib}, panel_{panel}, sim_{sim}, policy_{policy} {}

  // Mine the search space, screen every distinct candidate through the P4 gate +
  // the S1 deflation bar, and insert each survivor into `pool`. `pool` is GROWN
  // in place (the admitted alphas); `gate` is the stateless P4 admission screen.
  // Returns the §4.8 FactoryReport. Deterministic: same cfg + same starting pool
  // contents => byte-identical report.digest (F1/F2).
  [[nodiscard]] FactoryReport mine(const FactoryConfig &cfg, combine::AlphaStore &pool,
                                   const combine::AlphaGate &gate);

  // =======================================================================
  //  mine_into — the REAL admit path: mine + deflate, then admit each
  //  survivor into the PERSISTENT library::Library (S4b-3).
  //
  //  Same seeded SearchDriver path as mine() (shared internals; no fork), but
  //  the admit target is the persistent library — library-wide F6 dedup ->
  //  O(neighbors) corr -> P4 gate floors -> segmented store + PIT lifecycle +
  //  manifest — instead of an ephemeral combine::AlphaStore. The S1 deflation
  //  bar stays FACTORY-side: a candidate must clear cand.dsr >= cfg.min_dsr
  //  BEFORE library::admit is consulted (so a noise candidate the library alone
  //  might pass is still rejected by the multiple-testing deflation). `lib_lib`
  //  is GROWN in place. Deterministic: same cfg + same starting library contents
  //  => byte-identical report.digest (the search digest folded with every
  //  admission decision; F1/F2).
  //
  //  §0.6 DANGLING-SPAN discipline (as in mine()): the AlphaCandidate's pnl /
  //  pos_flat are NON-OWNING spans into the OWNED cand_pnl / cand_pos vectors,
  //  which MUST outlive the admit() call. They are kept alive in the loop body
  //  across the admit, so the spans never dangle.
  // =======================================================================
  [[nodiscard]] FactoryReport mine_into(const FactoryConfig &cfg, library::Library &lib_lib,
                                        const combine::AlphaGate &gate,
                                        SearchProgressSink *sink = nullptr,
                                        const SearchResumeState *resume = nullptr);

  // =======================================================================
  //  mine_into (SUBSTRATE-AWARE, S7.5d) — the SAME mine_into admit path with the
  //  PURE expensive per-genome scoring map moved over the IExecutor seam.
  //
  //  run_search stays in the parent (seeded/deterministic, F1/F2). The per-genome
  //  compile+eval+extract_streams + pool-aware-fitness(dsr, raw) map — scored
  //  against the RUN-START library snapshot (a const) — is what crosses the seam:
  //    * InProcess (ThreadExecutor): delegates to the existing mine_into(cfg, lib,
  //      gate) verbatim (the in-process map).
  //    * MultiProcess (ProcessExecutor): serialize {genomes = res.all_scored, the
  //      run-start admitted-pnl snapshot, panel, cfg}; submit(WorkloadId::Mine);
  //      gather per-genome {ok, dsr, raw, streams}; then run the EXISTING
  //      rank_by_deflated_fitness (on the gathered dsr/raw) and the EXISTING
  //      sequential library::admit loop (fed the gathered streams).
  //  Because rank+admit runs in the PARENT identically, report.digest and the
  //  library version_id are byte-identical across every substrate and worker count
  //  BY CONSTRUCTION (the §0.9 sound design). An unknown substrate aborts (ATX_CHECK).
  // =======================================================================
  [[nodiscard]] FactoryReport mine_into(const FactoryConfig &cfg, library::Library &lib_lib,
                                        const combine::AlphaGate &gate, parallel::IExecutor &exec);

private:
  // The as-of period for an admitted alpha's Candidate->Admitted lifecycle transition.
  // A constant (the S4 fixtures use period 1); the realized OOS streams are not keyed
  // to a calendar period in the research panel, so a fixed admit period is sufficient.
  static constexpr atx::usize kAdmitAsOf = 1U;

  // A scored candidate's admission ranking key: its deflated Sharpe (primary) and
  // raw fitness (tiebreak), plus its index into all_scored.
  struct Ranked {
    atx::usize idx{0};
    atx::f64 dsr{0.0};
    atx::f64 raw{0.0};
  };

  // Compile + evaluate a genome over the research panel and extract its per-alpha
  // streams (the candidate is root 0). The single-thread Engine path — exactly
  // pool_aware_fitness's internal eval (S3-4) — so the realized streams match the
  // fitness oracle. Err propagates compile/eval/extract failure.
  [[nodiscard]] atx::core::Result<alpha::AlphaStreams>
  detail_eval_streams(const Genome &cand) const;

  // P2a: compile genome `g`, evaluate over an ARBITRARY sub-panel (train OR
  // holdout), extract_streams with this->policy_ / this->sim_, flatten positions,
  // and return compute_metrics(pnl, pos, n_inst, book_size). The realized single-
  // alpha PnL stream is returned via `pnl_out` so the caller can compute a holdout
  // DSR via the fitness.cpp deflated-Sharpe recipe. Uses the SAME compile/eval path
  // as detail_eval_streams, so metrics_on_panel(g, full_panel, ...) reproduces the
  // in-loop metrics exactly. Err propagates compile/eval/extract failure (or a
  // zero-alpha stream).
  [[nodiscard]] atx::core::Result<combine::AlphaMetrics>
  metrics_on_panel(const Genome &g, const alpha::Panel &sub_panel, atx::f64 book_size,
                   std::vector<atx::f64> &pnl_out) const;

  // P2a: the out-of-sample (holdout) admit path. Dispatched from mine_into when
  // cfg.oos_fraction > 0. SELECTS the search on a TRAIN sub-panel, but CONFIRMS the
  // AlphaGate floors + the DSR bar on a HELD-OUT terminal window the search never
  // optimized on, and persists the HOLDOUT (admission) metrics. Returns both IS and
  // OOS metrics per admitted alpha in FactoryReport::oos_metrics. The legacy
  // mine_into body is left untouched (this is a TOP-of-function additive branch).
  [[nodiscard]] FactoryReport mine_into_oos(const FactoryConfig &cfg, library::Library &lib_lib,
                                            const combine::AlphaGate &gate,
                                            SearchProgressSink *sink = nullptr,
                                            const SearchResumeState *resume = nullptr);

  // Flatten alpha 0's per-period position cross-sections into the period-major,
  // instrument-minor layout AlphaStore::insert / compute_metrics expect
  // (length == n_periods * n_instruments).
  [[nodiscard]] static std::vector<atx::f64> flatten_positions(const alpha::AlphaStreams &strm);

  // Rank the distinct scored genomes by deflated fitness (DESCENDING dsr, then raw,
  // then canon_hash for a deterministic total order — F1). Each genome is scored
  // ONCE against the pool as it stands at run start (the pool grows later, in the
  // admission loop). A genome whose fitness errors sorts last (dsr = raw = 0).
  [[nodiscard]] std::vector<Ranked> rank_by_deflated_fitness(const std::vector<Genome> &scored,
                                                             const FitnessCfg &fit_cfg,
                                                             const combine::AlphaStore &pool) const;

  // The PoolView overload (S4b-3): identical to the AlphaStore overload above, but
  // `pool` is a backing-agnostic PoolView and the inner score call uses the S4b-2
  // pool_aware_fitness(genome, view, ...) overload (the O(neighbors) MAX-|corr| seam).
  // Used by mine_into to rank against the PERSISTENT library. The sort key + total
  // order are identical (DESCENDING dsr, then raw, then canon_hash; F1). A genome
  // whose fitness errors sorts last (dsr = raw = 0). NOTE: Genome.canon_hash is left
  // 0 by the S3 search path, so the canon_hash tiebreak is degenerate here; an
  // `idx` final tiebreak therefore pins a TRUE total order (std::sort is not stable),
  // so the rank — and the digest folded from it — is deterministic regardless of the
  // sort's internal permutation of equal-key elements (all_scored is built in a
  // deterministic order, so equal idx never occurs and the order is reproducible; F1).
  [[nodiscard]] std::vector<Ranked> rank_by_deflated_fitness(const std::vector<Genome> &scored,
                                                             const FitnessCfg &fit_cfg,
                                                             const PoolView &pool) const;

  // Mean Program::cache_hit_pct() over the run's distinct scored genomes (the
  // reachable CSE telemetry — see the header cse_pct note). An uncompilable genome
  // is skipped; an empty / all-uncompilable run yields 0.
  [[nodiscard]] double mean_cse_pct(const SearchResult &res) const;

  // SAFETY: each borrow is held for the Factory's lifetime; the single run-wide
  // Library owns every OpSig the genomes' Expr::op pointers alias and must outlive
  // the Factory and all produced genomes (genome.hpp SAFETY).
  const alpha::Library &lib_;
  const alpha::Panel &panel_;
  const exec::ExecutionSimulator &sim_;
  const WeightPolicy &policy_;
};

} // namespace atx::engine::factory
