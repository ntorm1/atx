#include "atx/engine/factory/factory.hpp"

#include <algorithm> // std::sort, std::max
#include <cstddef>   // std::size_t (hash_combine seed type)
#include <span>      // std::span
#include <utility>   // std::move (admitted provenance / streams)
#include <vector>    // std::vector

#include "atx/core/hash.hpp" // atx::core::hash_combine (digest fold)

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program (cse_pct)
#include "atx/engine/alpha/unparse.hpp"  // alpha::unparse (admitted-alpha expr_source, S4b-1)
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine

#include "atx/engine/combine/metrics.hpp" // combine::compute_metrics, AlphaMetrics

#include "atx/engine/library/record.hpp" // library::Provenance (admitted-alpha lineage)

#include "atx/engine/factory/canonical.hpp" // factory::canonical_hash (F6 dedup key)

namespace atx::engine::factory {

[[nodiscard]] FactoryReport Factory::mine(const FactoryConfig &cfg, combine::AlphaStore &pool,
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

[[nodiscard]] FactoryReport Factory::mine_into(const FactoryConfig &cfg, library::Library &lib_lib,
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

[[nodiscard]] atx::core::Result<alpha::AlphaStreams>
Factory::detail_eval_streams(const Genome &cand) const {
  ATX_TRY(const alpha::Program prog, alpha::compile(cand.ast, cand.analysis));
  alpha::Engine engine{panel_};
  ATX_TRY(const alpha::SignalSet ss, engine.evaluate(prog));
  return alpha::extract_streams(ss, policy_, panel_, sim_);
}

[[nodiscard]] std::vector<atx::f64> Factory::flatten_positions(const alpha::AlphaStreams &strm) {
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

[[nodiscard]] std::vector<Factory::Ranked>
Factory::rank_by_deflated_fitness(const std::vector<Genome> &scored, const FitnessCfg &fit_cfg,
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

[[nodiscard]] std::vector<Factory::Ranked>
Factory::rank_by_deflated_fitness(const std::vector<Genome> &scored, const FitnessCfg &fit_cfg,
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

[[nodiscard]] double Factory::mean_cse_pct(const SearchResult &res) const {
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

} // namespace atx::engine::factory
