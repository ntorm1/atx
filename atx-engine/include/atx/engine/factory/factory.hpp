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

#include <algorithm> // std::sort, std::max
#include <cstddef>   // std::size_t (hash_combine seed type)
#include <span>      // std::span
#include <string>    // std::string (seed-expression / field source)
#include <utility>   // std::move
#include <vector>    // std::vector

#include "atx/core/error.hpp"  // Result, Ok, Err
#include "atx/core/hash.hpp"   // atx::core::hash_combine (digest fold)
#include "atx/core/types.hpp"  // atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program (cse_pct)
#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/streams.hpp"  // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine

#include "atx/engine/combine/gate.hpp"    // combine::AlphaGate, GateVerdict, GateConfig
#include "atx/engine/combine/metrics.hpp" // combine::compute_metrics, AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/factory/fitness.hpp"       // factory::pool_aware_fitness, FitnessCfg
#include "atx/engine/factory/genome.hpp"        // factory::Genome
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

    // (2) rank the DISTINCT scored candidates by deflated fitness (best first).
    // Re-score each against the pool AS IT STANDS NOW (run start) to get its dsr;
    // the per-candidate re-score INSIDE the admission loop below then reflects the
    // GROWING pool. all_scored is the set of distinct structures (F5/F6).
    std::vector<Ranked> ranked = rank_by_deflated_fitness(res.all_scored, cfg, pool);

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
      auto fit = pool_aware_fitness(cand, pool, panel_, policy_, sim_, cfg.search.fitness);
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

private:
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
                                                             const FactoryConfig &cfg,
                                                             const combine::AlphaStore &pool) const {
    std::vector<Ranked> ranked;
    ranked.reserve(scored.size());
    for (atx::usize i = 0U; i < scored.size(); ++i) {
      atx::f64 dsr = 0.0;
      atx::f64 raw = 0.0;
      auto fit = pool_aware_fitness(scored[i], pool, panel_, policy_, sim_, cfg.search.fitness);
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
