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

#include <array>     // std::array (reject_histogram, indexed by library::AdmitKind)
#include <algorithm> // std::sort, std::max
#include <cstddef>   // std::size_t (hash_combine seed type)
#include <span>      // std::span
#include <string>    // std::string (seed-expression / field source)
#include <utility>   // std::move (admitted provenance / streams)
#include <vector>    // std::vector

#include "atx/core/error.hpp"  // Result, Ok, Err
#include "atx/core/hash.hpp"   // atx::core::hash_combine (digest fold)
#include "atx/core/types.hpp"  // atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program (cse_pct)
#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/streams.hpp"  // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/unparse.hpp"  // alpha::unparse (admitted-alpha expr_source, S4b-1)
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine

#include "atx/engine/combine/gate.hpp"    // combine::AlphaGate, GateVerdict, GateConfig
#include "atx/engine/combine/metrics.hpp" // combine::compute_metrics, AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/library/library.hpp" // library::Library, AlphaCandidate, AdmitKind, AdmitVerdict
#include "atx/engine/library/record.hpp"  // library::Provenance (admitted-alpha lineage)

#include "atx/engine/factory/canonical.hpp"     // factory::canonical_hash (F6 dedup key)
#include "atx/engine/factory/fitness.hpp"       // factory::pool_aware_fitness, FitnessCfg
#include "atx/engine/factory/genome.hpp"        // factory::Genome
#include "atx/engine/factory/pool_view.hpp"     // factory::LibraryPool, PoolView, pool_aware_fitness overload (S4b-2)
#include "atx/engine/factory/search_driver.hpp" // factory::SearchDriver, SearchConfig, SearchResult

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
  SearchConfig search{};                  // the S3-5 search budget + CPCV/deflation geometry
  std::vector<std::string> seed_exprs;    // in-grammar starting templates (SearchDriver ctor)
  std::vector<std::string> panel_fields;  // field-swap candidate names (SearchDriver ctor)
  atx::f64 min_dsr = 0.5;                 // S1 deflation bar (F4): admit iff dsr >= this
  atx::f64 book_size = 1.0;               // notional divisor for compute_metrics turnover
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
  Factory(const alpha::Library &lib, const alpha::Panel &panel,
          const exec::ExecutionSimulator &sim, const WeightPolicy &policy) noexcept
      : lib_{lib}, panel_{panel}, sim_{sim}, policy_{policy} {}

  // Mine the search space, screen every distinct candidate through the P4 gate +
  // the S1 deflation bar, and insert each survivor into `pool`. `pool` is GROWN
  // in place (the admitted alphas); `gate` is the stateless P4 admission screen.
  // Returns the §4.8 FactoryReport. Deterministic: same cfg + same starting pool
  // contents => byte-identical report.digest (F1/F2).
  [[nodiscard]] FactoryReport mine(const FactoryConfig &cfg, combine::AlphaStore &pool,
                                   const combine::AlphaGate &gate) {
    FactoryReport rep;

    // (1) run the S3-5 search. The driver re-derives a clean per-run state from the
    // seed, so a fresh driver per mine() preserves F1 replay (no carried state).
    SearchDriver driver{lib_, panel_, policy_, sim_, cfg.seed_exprs, cfg.panel_fields};
    const SearchResult res = driver.run(cfg.search, pool);

    rep.evaluated = res.trial_count;
    rep.trials = res.trial_count;
    rep.dedup_pct = res.dedup_pct;
    rep.seed = res.seed;
    rep.cse_pct = mean_cse_pct(res);
    // Seed the admission digest with the search's deterministic run fingerprint;
    // each admission decision is folded in below (F1/F2).
    rep.digest = res.digest;

    // F4 — the multiple-testing N fed to the ADMISSION deflation is the search's
    // RUNNING distinct-candidate count (res.trial_count), NOT the static config N:
    // each candidate is deflated against the number of trials the search ACTUALLY
    // performed, so the anti-snooping bar AUTO-SCALES with search effort (a larger
    // mine deflates harder — the multiple-testing correction the S1 DSR/PBO
    // accounting expects). The in-search SELECTION fitness keeps the config N (the
    // realized count is unknown mid-search); only admission — which runs AFTER the
    // search completes — knows the realized N. (res.trial_count == 0 only on a
    // degenerate empty run, where `ranked` is empty and N is never consumed; the
    // config N is the harmless fallback there.)
    FitnessCfg admit_fit = cfg.search.fitness;
    if (res.trial_count > 0U) {
      admit_fit.trial_count = res.trial_count;
    }

    // (2) rank the DISTINCT scored candidates by deflated fitness (best first).
    // Re-score each against the pool AS IT STANDS NOW (run start) to get its dsr;
    // the per-candidate re-score INSIDE the admission loop below then reflects the
    // GROWING pool. all_scored is the set of distinct structures (F5/F6).
    std::vector<Ranked> ranked = rank_by_deflated_fitness(res.all_scored, admit_fit, pool);

    // (3) the mine -> gate -> admit loop (§4.8), best-deflated first.
    for (const Ranked &r : ranked) {
      const Genome &cand = res.all_scored[r.idx];

      // (3a) realize the candidate's FULL OOS streams (PnL + positions). Computed
      // BEFORE any insert — the OWNED vectors below survive the insert (§0.6).
      auto strm_res = detail_eval_streams(cand);
      if (!strm_res.has_value()) {
        continue; // an un-evaluable candidate is silently dropped (F5 backstop)
      }
      const alpha::AlphaStreams &strm = *strm_res;
      // DEFENSIVE GUARD: every Factory access below indexes alpha 0 (strm.pnl(0) /
      // flatten_positions's strm.positions(0, .)), which ABORTS (ATX_ASSERT) on a
      // zero-alpha streams. A single-root Genome compiles to a single-root Program
      // (1 alpha), so this holds in practice — but a stream-extraction that returns
      // 0 alphas would otherwise abort mid-mine. Guard ONCE at the source: skip the
      // candidate gracefully (covers BOTH the pnl(0) read and flatten_positions).
      if (strm.n_alphas() == 0U) {
        continue;
      }
      const atx::usize n_inst = strm.n_instruments();
      // OWNED copies (the store's insert COPIES from these; they outlive the call).
      std::vector<atx::f64> cand_pnl(strm.pnl(0).begin(), strm.pnl(0).end());
      std::vector<atx::f64> cand_pos = flatten_positions(strm);

      // (3b) realized-performance metrics over the full OOS stream (§4.8).
      const combine::AlphaMetrics metrics = combine::compute_metrics(
          std::span<const atx::f64>{cand_pnl}, std::span<const atx::f64>{cand_pos}, n_inst,
          cfg.book_size);

      // (3c) re-score against the CURRENT (growing) pool for the deflated bar — the
      // dsr that gates admission must reflect the pool the candidate would join.
      atx::f64 dsr = r.dsr; // fall back to the run-start dsr if a re-score errs
      auto fit = pool_aware_fitness(cand, pool, panel_, policy_, sim_, admit_fit);
      if (fit.has_value()) {
        dsr = fit->dsr;
      }

      // (3d) the P4 gate verdict (reads pool member spans — BEFORE insert, §0.6).
      const combine::GateVerdict verdict =
          gate.admit(metrics, std::span<const atx::f64>{cand_pnl}, pool);

      const bool accept = (verdict == combine::GateVerdict::Accept) && (dsr >= cfg.min_dsr);
      // Fold the decision into the digest (every screened candidate, in order):
      // (canon_hash, accept-bit) — so a different admission outcome shifts the
      // digest, and an identical mine+admit replays byte-identical (F1/F2).
      rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
          static_cast<std::size_t>(rep.digest), cand.canon_hash,
          static_cast<atx::u64>(accept ? 1U : 0U)));

      if (!accept) {
        continue;
      }

      // (3e) ADMIT: insert the realized streams (source = nullptr — re-eval is not
      // exercised in mining; see the header ownership note). The owned cand_pnl /
      // cand_pos are copied into the store here; the pool's prior member spans are
      // now allowed to dangle (we have finished reading them for this candidate).
      const auto ins = pool.insert(/*source=*/nullptr, std::span<const atx::f64>{cand_pnl},
                                   std::span<const atx::f64>{cand_pos}, metrics);
      if (ins.has_value()) {
        ++rep.admitted;
      }
      // An insert Err (a period/shape mismatch against an established pool shape) is
      // a candidate that cannot coherently join this pool — it is screened out and
      // NOT counted as admitted, but the digest already recorded the accept decision
      // (the verdict was Accept; the structural reject is a downstream pool fact).
    }

    return rep;
  }

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
                                        const combine::AlphaGate &gate) {
    FactoryReport rep;
    rep.library_n_alphas_before = lib_lib.n_alphas();

    // (1) run the S3-5 search — IDENTICAL to mine(): a fresh seeded driver re-derives
    // clean per-run state, preserving F1 replay. The driver's SELECTION fitness scores
    // against a combine::AlphaStore (the S3-5 run() signature — there is no PoolView
    // run() overload), so the search runs against an EMPTY scratch store, exactly as
    // mine() does at run start. Admission below scores against the persistent library
    // via the LibraryPool seam (S4b-2). The empty store and an empty library agree
    // (both have worst_corr == 0), so the search is byte-identical to mine()'s search.
    combine::AlphaStore search_pool; // empty selection pool (the search's pre-pool)
    SearchDriver driver{lib_, panel_, policy_, sim_, cfg.seed_exprs, cfg.panel_fields};
    const SearchResult res = driver.run(cfg.search, search_pool);

    // The persistent library is the ADMISSION pool: the deflated-fitness ranking and
    // the per-candidate re-score below score marginal corr against it (O(neighbors)).
    LibraryPool view{lib_lib};

    rep.evaluated = res.trial_count;
    rep.trials = res.trial_count;
    rep.dedup_pct = res.dedup_pct;
    rep.seed = res.seed;
    rep.cse_pct = mean_cse_pct(res);
    rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

    // F4 — the admission deflation N is the search's RUNNING distinct-candidate count
    // (res.trial_count), NOT the static config N (identical to mine()).
    FitnessCfg admit_fit = cfg.search.fitness;
    if (res.trial_count > 0U) {
      admit_fit.trial_count = res.trial_count;
    }

    // (2) rank the distinct scored candidates by deflated fitness against the LIBRARY
    // (the PoolView overload routes the corr-to-pool through the O(neighbors) index).
    std::vector<Ranked> ranked = rank_by_deflated_fitness(res.all_scored, admit_fit, view);

    // (3) the mine -> deflate -> library::admit loop, best-deflated first.
    for (const Ranked &r : ranked) {
      const Genome &g = res.all_scored[r.idx];

      // (3a) realize the candidate's FULL OOS streams (PnL + positions). Computed
      // BEFORE any admit; the OWNED vectors below outlive the admit() call (§0.6).
      auto strm_res = detail_eval_streams(g);
      if (!strm_res.has_value()) {
        continue; // S3-6 0-alpha guard: an un-evaluable candidate is silently dropped (F5)
      }
      const alpha::AlphaStreams &strm = *strm_res;
      if (strm.n_alphas() == 0U) {
        continue; // guards strm.pnl(0) / flatten_positions's strm.positions(0,.) abort
      }
      const atx::usize n_inst = strm.n_instruments();
      // OWNED copies kept alive across admit() — the AlphaCandidate spans alias them.
      std::vector<atx::f64> cand_pnl(strm.pnl(0).begin(), strm.pnl(0).end());
      std::vector<atx::f64> cand_pos = flatten_positions(strm);

      // (3b) realized-performance metrics over the full OOS stream.
      const combine::AlphaMetrics metrics = combine::compute_metrics(
          std::span<const atx::f64>{cand_pnl}, std::span<const atx::f64>{cand_pos}, n_inst,
          cfg.book_size);

      // (3c) re-score against the CURRENT (growing) library for the deflated bar (the
      // O(neighbors) PoolView overload). Fall back to the run-start dsr if it errs.
      atx::f64 dsr = r.dsr;
      auto fit = pool_aware_fitness(g, view, panel_, policy_, sim_, admit_fit);
      if (fit.has_value()) {
        dsr = fit->dsr;
      }

      // (3d) F6 dedup key: Genome.canon_hash is left 0 by the S3 search path
      // (genome.hpp INVARIANT — canon_hash defaulted 0, not populated), so COMPUTE
      // the real canonical key here; an admit on canon_hash == 0 would break the
      // library-wide dedup gate. This is the SAME stable key the library deduped on.
      const atx::u64 canon_hash = canonical_hash(g.ast, g.ast.roots().front().root);

      // Provenance lineage: S3's all_scored Genomes do NOT track parent hashes /
      // mutation op (a deliberate S4b-3 simplification — F6 depends on canon_hash,
      // not lineage), so parent_hashes is empty and mutation_op is 0. The meaningful
      // provenance is the re-parseable formula text (S4b-1 unparse, round-trips to
      // canon_hash) and the run seed.
      library::Provenance prov{alpha::unparse(g.ast), /*parent_hashes=*/{},
                               /*mutation_op=*/0, /*seed=*/res.seed};

      // The deflation bar is FACTORY-side: clear it BEFORE library::admit is consulted.
      library::AdmitKind kind = library::AdmitKind::RejectFitness; // non-accept sentinel for the histogram
      if (dsr >= cfg.min_dsr) {
        const library::AlphaCandidate cand{canon_hash,
                                           std::span<const atx::f64>{cand_pnl},
                                           std::span<const atx::f64>{cand_pos},
                                           metrics,
                                           std::move(prov),
                                           /*as_of=*/kAdmitAsOf,
                                           /*source=*/nullptr};
        const library::AdmitVerdict v = lib_lib.admit(cand, gate);
        kind = v.kind;
        if (kind == library::AdmitKind::Accept) {
          ++rep.admitted;
        } else if (kind == library::AdmitKind::Duplicate) {
          ++rep.duplicates;
        }
      }
      ++rep.reject_histogram[static_cast<atx::usize>(kind)];

      // Fold the decision into the digest (every screened candidate, in rank order):
      // (canon_hash, AdmitKind) — a different admission outcome shifts the digest, and
      // an identical mine+admit replays byte-identical (F1/F2).
      rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
          static_cast<std::size_t>(rep.digest), canon_hash, static_cast<atx::u64>(kind)));
    }

    rep.library_n_alphas_after = lib_lib.n_alphas();
    return rep;
  }

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
  detail_eval_streams(const Genome &cand) const {
    ATX_TRY(const alpha::Program prog, alpha::compile(cand.ast, cand.analysis));
    alpha::Engine engine{panel_};
    ATX_TRY(const alpha::SignalSet ss, engine.evaluate(prog));
    return alpha::extract_streams(ss, policy_, panel_, sim_);
  }

  // Flatten alpha 0's per-period position cross-sections into the period-major,
  // instrument-minor layout AlphaStore::insert / compute_metrics expect
  // (length == n_periods * n_instruments).
  [[nodiscard]] static std::vector<atx::f64> flatten_positions(const alpha::AlphaStreams &strm) {
    const atx::usize periods = strm.n_periods();
    const atx::usize insts = strm.n_instruments();
    std::vector<atx::f64> out;
    out.reserve(periods * insts);
    for (atx::usize t = 0U; t < periods; ++t) {
      const std::span<const atx::f64> cs = strm.positions(0, t);
      out.insert(out.end(), cs.begin(), cs.end());
    }
    return out;
  }

  // Rank the distinct scored genomes by deflated fitness (DESCENDING dsr, then raw,
  // then canon_hash for a deterministic total order — F1). Each genome is scored
  // ONCE against the pool as it stands at run start (the pool grows later, in the
  // admission loop). A genome whose fitness errors sorts last (dsr = raw = 0).
  [[nodiscard]] std::vector<Ranked> rank_by_deflated_fitness(const std::vector<Genome> &scored,
                                                             const FitnessCfg &fit_cfg,
                                                             const combine::AlphaStore &pool) const {
    std::vector<Ranked> ranked;
    ranked.reserve(scored.size());
    for (atx::usize i = 0U; i < scored.size(); ++i) {
      atx::f64 dsr = 0.0;
      atx::f64 raw = 0.0;
      auto fit = pool_aware_fitness(scored[i], pool, panel_, policy_, sim_, fit_cfg);
      if (fit.has_value()) {
        dsr = fit->dsr;
        raw = fit->raw;
      }
      ranked.push_back(Ranked{i, dsr, raw});
    }
    std::sort(ranked.begin(), ranked.end(), [&scored](const Ranked &a, const Ranked &b) {
      if (a.dsr != b.dsr) {
        return a.dsr > b.dsr; // best DEFLATED first (§4.8)
      }
      if (a.raw != b.raw) {
        return a.raw > b.raw; // raw tiebreak
      }
      return scored[a.idx].canon_hash < scored[b.idx].canon_hash; // deterministic total order
    });
    return ranked;
  }

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
                                                             const PoolView &pool) const {
    std::vector<Ranked> ranked;
    ranked.reserve(scored.size());
    for (atx::usize i = 0U; i < scored.size(); ++i) {
      atx::f64 dsr = 0.0;
      atx::f64 raw = 0.0;
      auto fit = pool_aware_fitness(scored[i], pool, panel_, policy_, sim_, fit_cfg);
      if (fit.has_value()) {
        dsr = fit->dsr;
        raw = fit->raw;
      }
      ranked.push_back(Ranked{i, dsr, raw});
    }
    std::sort(ranked.begin(), ranked.end(), [&scored](const Ranked &a, const Ranked &b) {
      if (a.dsr != b.dsr) {
        return a.dsr > b.dsr; // best DEFLATED first (§4.8)
      }
      if (a.raw != b.raw) {
        return a.raw > b.raw; // raw tiebreak
      }
      if (scored[a.idx].canon_hash != scored[b.idx].canon_hash) {
        return scored[a.idx].canon_hash < scored[b.idx].canon_hash; // canon tiebreak
      }
      return a.idx < b.idx; // true total order (canon_hash is 0 on the S3 search path)
    });
    return ranked;
  }

  // Mean Program::cache_hit_pct() over the run's distinct scored genomes (the
  // reachable CSE telemetry — see the header cse_pct note). An uncompilable genome
  // is skipped; an empty / all-uncompilable run yields 0.
  [[nodiscard]] double mean_cse_pct(const SearchResult &res) const {
    double sum = 0.0;
    atx::usize n = 0U;
    for (const Genome &g : res.all_scored) {
      auto prog = alpha::compile(g.ast, g.analysis);
      if (!prog.has_value()) {
        continue;
      }
      sum += prog->cache_hit_pct();
      ++n;
    }
    return (n == 0U) ? 0.0 : sum / static_cast<double>(n);
  }

  // SAFETY: each borrow is held for the Factory's lifetime; the single run-wide
  // Library owns every OpSig the genomes' Expr::op pointers alias and must outlive
  // the Factory and all produced genomes (genome.hpp SAFETY).
  const alpha::Library &lib_;
  const alpha::Panel &panel_;
  const exec::ExecutionSimulator &sim_;
  const WeightPolicy &policy_;
};

} // namespace atx::engine::factory
