#include "atx/engine/factory/factory.hpp"

#include <algorithm> // std::sort, std::max
#include <cmath>     // std::sqrt (P2a holdout DSR de-annualization)
#include <cstddef>   // std::size_t (hash_combine seed type)
#include <optional>  // std::nullopt (P2a deflated_sharpe variance arg)
#include <span>      // std::span
#include <utility>   // std::move (admitted provenance / streams)
#include <vector>    // std::vector

#include "atx/core/hash.hpp" // atx::core::hash_combine (digest fold)

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program (cse_pct)
#include "atx/engine/alpha/unparse.hpp"  // alpha::unparse (admitted-alpha expr_source, S4b-1)
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine

#include "atx/engine/combine/metrics.hpp" // combine::compute_metrics, AlphaMetrics

#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe (P2a holdout DSR)
#include "atx/engine/eval/lockbox.hpp"         // eval::reserve_lockbox, detail::slice_panel (P2a)
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, eval::excess_kurtosis (P2a DSR)

#include "atx/engine/library/record.hpp" // library::Provenance (admitted-alpha lineage)

#include "atx/engine/factory/canonical.hpp" // factory::canonical_hash (F6 dedup key)

#include "atx/engine/parallel/executor.hpp" // parallel::IExecutor, Substrate, SlotView (S7.5d seam)
#include "atx/engine/parallel/workload_mine.hpp" // parallel::serialize_mine_input / MineGenomeResult (S7.5d)

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
    const combine::AlphaMetrics metrics =
        combine::compute_metrics(std::span<const atx::f64>{cand_pnl},
                                 std::span<const atx::f64>{cand_pos}, n_inst, cfg.book_size);

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
    rep.digest = static_cast<atx::u64>(
        atx::core::hash_combine(static_cast<std::size_t>(rep.digest), cand.canon_hash,
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

[[nodiscard]] atx::core::Result<FactoryReport>
Factory::mine_into(const FactoryConfig &cfg, library::Library &lib_lib,
                   const combine::AlphaGate &gate, SearchProgressSink *sink,
                   const SearchResumeState *resume) {
  // P2a — out-of-sample (holdout) validation is an ADDITIVE branch at the TOP:
  // when oos_fraction > 0 the search SELECTS on a train window and admission is
  // CONFIRMED on a held-out window. When oos_fraction == 0 (the default) this is
  // never taken and the EXISTING body below runs byte-identically to the legacy
  // path (same digest / admitted count / version id / reject histogram).
  if (cfg.oos_fraction > 0.0) {
    return mine_into_oos(cfg, lib_lib, gate, sink, resume);
  }

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
  const SearchResult res = driver.run(cfg.search, search_pool, sink, resume);

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
    const combine::AlphaMetrics metrics =
        combine::compute_metrics(std::span<const atx::f64>{cand_pnl},
                                 std::span<const atx::f64>{cand_pos}, n_inst, cfg.book_size);

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
    library::AdmitKind kind =
        library::AdmitKind::RejectFitness; // non-accept sentinel for the histogram
    if (dsr >= cfg.min_dsr) {
      const library::AlphaCandidate cand{canon_hash,
                                         std::span<const atx::f64>{cand_pnl},
                                         std::span<const atx::f64>{cand_pos},
                                         metrics,
                                         std::move(prov),
                                         /*as_of=*/kAdmitAsOf,
                                         /*source=*/nullptr};
      // Cross-run accumulation guard (Task 8 footgun): route the persistent-library
      // admit through the geometry-checked seam. On a matching/fresh geometry this
      // delegates to admit() VERBATIM (same verdict, same state mutation, digest-
      // identical); on a reopened --library-dir whose fixed t_ differs from this run's
      // pnl length it returns a CLEAN error that we propagate up so the run HALTS
      // instead of aborting (debug) / reading OOB (release).
      ATX_TRY(const library::AdmitVerdict v, lib_lib.try_admit(cand, gate));
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
  return atx::core::Ok(std::move(rep));
}

namespace {

// One gathered per-genome scoring result: ok (1 iff compiled+scored), the run-start
// pool-aware (dsr, raw), and the realized single-alpha streams (valid iff ok == 1).
// This is exactly what the in-process map produces per genome and what the MultiProcess
// worker ships back — so the parent's rank + admit loop is fed identically either way.
struct GatheredScore {
  atx::u32 ok{0};
  atx::f64 dsr{0.0};
  atx::f64 raw{0.0};
  alpha::AlphaStreams streams;
};

// Snapshot the run-start library's admitted-pnl streams (alpha-major [n_alphas * T]) so
// the worker rebuilds the SAME SimHash corr index library::worst_corr_to_pool uses. The
// pool is the const-at-run-start state; we copy it OUT before any admit grows the store
// (the §0.6 dangling-span discipline — store.pnl() aliases the segment/memtable). Empty
// library -> empty snapshot (worst_corr == 0 for every candidate).
[[nodiscard]] std::vector<atx::f64> snapshot_pool_pnl(const library::Library &lib) {
  const atx::usize n = static_cast<atx::usize>(lib.n_alphas());
  const atx::usize t = lib.n_periods();
  std::vector<atx::f64> out;
  if (n == 0U || t == 0U) {
    return out;
  }
  out.reserve(n * t);
  for (atx::usize a = 0; a < n; ++a) {
    const std::span<const atx::f64> p = lib.pnl(library::AlphaId{static_cast<atx::u32>(a)});
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

// Forward decl: the per-substrate gather (defined after decode_mine_slot below). A free
// function (not a Factory member) so its anonymous-namespace return type GatheredScore
// never leaks into the public header.
[[nodiscard]] std::vector<GatheredScore>
gather_mine_scores(const std::vector<Genome> &scored, const parallel::MineWorkItem &pool_item,
                   const FitnessCfg &admit_fit, const alpha::Panel &panel,
                   const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                   parallel::IExecutor &exec);

// Decode one Mine output slot { ok:u32, pad:u32, dsr:f64, raw:f64, pnl[T], pos[T*N] } into
// a GatheredScore, reconstructing the single-alpha AlphaStreams from the f64 tails. The
// slot is the parent's own buffer (already bounds-validated by the SlotView); n_periods /
// n_instruments are the known panel dims, so the tail sizes are fixed.
[[nodiscard]] GatheredScore decode_mine_slot(std::span<const std::byte> slot, atx::usize n_periods,
                                             atx::usize n_instruments) {
  // The slot is the parent's OWN buffer, sized by gather_mine_scores to exactly
  // hdr + n_periods*8 + n_periods*n_instruments*8 (the panel dims are this process's own
  // trusted Panel). ALWAYS-ON guards (not a debug-only assert an NDEBUG build elides)
  // precede every memcpy: a corrupt slot size here would over-read the SHM segment.
  GatheredScore g;
  const atx::usize hdr = 24U;    // kMineSlotHeaderBytes
  ATX_CHECK(slot.size() >= hdr); // the fixed header must be present before any read
  atx::u32 ok = 0;
  std::memcpy(&ok, slot.data() + 0, sizeof(atx::u32));
  std::memcpy(&g.dsr, slot.data() + 8, sizeof(atx::f64));
  std::memcpy(&g.raw, slot.data() + 16, sizeof(atx::f64));
  g.ok = ok;
  if (ok != 1U) {
    return g; // a failed genome: dsr=raw=0, no streams (the parent drops it)
  }
  // pnl[T] then pos[T*N]. Compute the byte offsets overflow-safe and require the slot to
  // hold the whole record before any tail memcpy.
  const atx::usize pnl_bytes = n_periods * sizeof(atx::f64);
  const atx::usize pos_cells = n_periods * n_instruments;
  const atx::usize pos_bytes = pos_cells * sizeof(atx::f64);
  ATX_CHECK(slot.size() >= hdr + pnl_bytes + pos_bytes); // whole fixed-shape record present
  alpha::AlphaStreams strm;
  strm.n_alphas_ = 1U;
  strm.n_periods_ = n_periods;
  strm.n_instruments_ = n_instruments;
  strm.pnl_flat.resize(n_periods);
  strm.pos_flat.resize(pos_cells);
  if (pnl_bytes != 0U) {
    std::memcpy(strm.pnl_flat.data(), slot.data() + hdr, pnl_bytes);
  }
  if (pos_bytes != 0U) {
    std::memcpy(strm.pos_flat.data(), slot.data() + hdr + pnl_bytes, pos_bytes);
  }
  g.streams = std::move(strm);
  return g;
}

} // namespace

[[nodiscard]] atx::core::Result<FactoryReport>
Factory::mine_into(const FactoryConfig &cfg, library::Library &lib_lib,
                   const combine::AlphaGate &gate, parallel::IExecutor &exec) {
  // P2a — OOS validation requires a train/holdout panel split. The MultiProcess wire
  // format serializes ONE panel and decodes streams sized to that panel's dims, so the
  // OOS path runs TWO submits (one per sub-panel) reusing the SAME wire format unchanged
  // (Task 5). InProcess delegates to the serial mine_into_oos (the reference); the
  // MultiProcess substrate takes the parallel two-submit OOS path below. Either way the
  // stateful rank+admit fold runs in THIS parent process, so the digest / admitted /
  // version_id are byte-identical to the serial mine_into_oos by construction.
  if (cfg.oos_fraction > 0.0) {
    switch (exec.substrate()) {
    case parallel::Substrate::InProcess:
      return mine_into(cfg, lib_lib, gate); // dispatches to the serial mine_into_oos
    case parallel::Substrate::MultiProcess:
      return mine_into_oos_parallel(cfg, lib_lib, gate, exec);
    }
    // Defensive: an executor returning neither substrate aborts rather than silently
    // producing a wrong OOS digest (the same fail-safe as the non-OOS path below).
    ATX_CHECK(false && "Factory::mine_into: unknown executor substrate (OOS)");
  }

  // InProcess: the existing in-process map IS the sound single-process path. Delegate
  // verbatim so the digest is, trivially, the sequential digest.
  switch (exec.substrate()) {
  case parallel::Substrate::InProcess:
    return mine_into(cfg, lib_lib, gate);
  case parallel::Substrate::MultiProcess:
    break; // handled below
  }
  // (Defensive: an executor returning neither substrate aborts rather than silently
  // producing a wrong digest — the same fail-safe as parallel_evaluate.)
  ATX_CHECK(exec.substrate() == parallel::Substrate::MultiProcess &&
            "Factory::mine_into: unknown executor substrate");

  FactoryReport rep;
  rep.library_n_alphas_before = lib_lib.n_alphas();

  // (1) run the S3-5 search — IDENTICAL to the sequential mine_into.
  combine::AlphaStore search_pool;
  SearchDriver driver{lib_, panel_, policy_, sim_, cfg.seed_exprs, cfg.panel_fields};
  const SearchResult res = driver.run(cfg.search, search_pool);

  rep.evaluated = res.trial_count;
  rep.trials = res.trial_count;
  rep.dedup_pct = res.dedup_pct;
  rep.seed = res.seed;
  rep.cse_pct = mean_cse_pct(res);
  rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

  // F4 — identical to the sequential path: the admission deflation N is res.trial_count.
  FitnessCfg admit_fit = cfg.search.fitness;
  if (res.trial_count > 0U) {
    admit_fit.trial_count = res.trial_count;
  }

  // (2) GATHER per-genome {ok, dsr, raw, streams} over the PROCESS boundary. The pure
  // map (compile+eval+extract_streams + pool-aware fitness vs. the RUN-START snapshot)
  // is what crosses; the snapshot is the library as it stands NOW (const at run start).
  const std::vector<atx::f64> pool_pnl = snapshot_pool_pnl(lib_lib);
  const atx::usize n_periods = lib_lib.n_periods();
  parallel::MineWorkItem pool_item;
  pool_item.pool_pnl_flat = std::span<const atx::f64>{pool_pnl};
  pool_item.pool_n_alphas = static_cast<atx::usize>(lib_lib.n_alphas());
  pool_item.n_periods = n_periods;
  pool_item.pool_seed = lib_lib.master_seeds().empty() ? 0ULL : lib_lib.master_seeds().front();

  const std::vector<GatheredScore> gathered =
      gather_mine_scores(res.all_scored, pool_item, admit_fit, panel_, policy_, sim_, exec);

  // (3) rank by deflated fitness (DESC dsr, then raw, then idx) over the GATHERED scores
  // — byte-identical to rank_by_deflated_fitness(all_scored, admit_fit, LibraryPool) at
  // run start, because the worker scored against the SAME snapshot (dsr is pool-
  // independent; raw's redundancy is the SAME SimHash MAX-|corr|). canon_hash is 0 on
  // the S3 search path, so the idx tiebreak pins the total order (F1).
  std::vector<Ranked> ranked;
  ranked.reserve(res.all_scored.size());
  for (atx::usize i = 0; i < res.all_scored.size(); ++i) {
    ranked.push_back(Ranked{i, gathered[i].dsr, gathered[i].raw});
  }
  std::sort(ranked.begin(), ranked.end(), [&res](const Ranked &a, const Ranked &b) {
    if (a.dsr != b.dsr) {
      return a.dsr > b.dsr;
    }
    if (a.raw != b.raw) {
      return a.raw > b.raw;
    }
    if (res.all_scored[a.idx].canon_hash != res.all_scored[b.idx].canon_hash) {
      return res.all_scored[a.idx].canon_hash < res.all_scored[b.idx].canon_hash;
    }
    return a.idx < b.idx;
  });

  // (4) the EXISTING sequential admit loop, fed the gathered streams — byte-identical to
  // the sequential mine_into loop (same metrics/dsr/canon/admit/digest fold). dsr is the
  // gathered run-start value, which equals the sequential 3c re-score (pool-INDEPENDENT).
  for (const Ranked &r : ranked) {
    const GatheredScore &gs = gathered[r.idx];
    const Genome &g = res.all_scored[r.idx];
    if (gs.ok != 1U) {
      continue; // an un-evaluable candidate is silently dropped (F5) — no digest fold.
    }
    const alpha::AlphaStreams &strm = gs.streams;
    if (strm.n_alphas() == 0U) {
      continue;
    }
    const atx::usize n_inst = strm.n_instruments();
    std::vector<atx::f64> cand_pnl(strm.pnl(0).begin(), strm.pnl(0).end());
    std::vector<atx::f64> cand_pos = flatten_positions(strm);

    const combine::AlphaMetrics metrics =
        combine::compute_metrics(std::span<const atx::f64>{cand_pnl},
                                 std::span<const atx::f64>{cand_pos}, n_inst, cfg.book_size);

    // (3c equivalent) the deflated bar's dsr. It is POOL-INDEPENDENT, so the gathered
    // run-start dsr equals the sequential loop's growing-pool re-score dsr exactly.
    const atx::f64 dsr = gs.dsr;

    const atx::u64 canon_hash = canonical_hash(g.ast, g.ast.roots().front().root);
    library::Provenance prov{alpha::unparse(g.ast), /*parent_hashes=*/{},
                             /*mutation_op=*/0, /*seed=*/res.seed};

    library::AdmitKind kind = library::AdmitKind::RejectFitness;
    if (dsr >= cfg.min_dsr) {
      const library::AlphaCandidate cand{canon_hash,
                                         std::span<const atx::f64>{cand_pnl},
                                         std::span<const atx::f64>{cand_pos},
                                         metrics,
                                         std::move(prov),
                                         /*as_of=*/kAdmitAsOf,
                                         /*source=*/nullptr};
      // Cross-run accumulation guard (Task 8): route the persistent-library admit
      // through the geometry-checked seam (try_admit delegates to admit() VERBATIM on
      // a matching/fresh geometry — digest-identical — and returns a CLEAN propagated
      // error on a reopened --library-dir whose fixed t_ differs from this run).
      ATX_TRY(const library::AdmitVerdict v, lib_lib.try_admit(cand, gate));
      kind = v.kind;
      if (kind == library::AdmitKind::Accept) {
        ++rep.admitted;
      } else if (kind == library::AdmitKind::Duplicate) {
        ++rep.duplicates;
      }
    }
    ++rep.reject_histogram[static_cast<atx::usize>(kind)];

    rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
        static_cast<std::size_t>(rep.digest), canon_hash, static_cast<atx::u64>(kind)));
  }

  rep.library_n_alphas_after = lib_lib.n_alphas();
  return atx::core::Ok(std::move(rep));
}

namespace {

[[nodiscard]] std::vector<GatheredScore>
gather_mine_scores(const std::vector<Genome> &scored, const parallel::MineWorkItem &pool_item,
                   const FitnessCfg &admit_fit, const alpha::Panel &panel,
                   const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                   parallel::IExecutor &exec) {
  const atx::usize n = scored.size();
  std::vector<GatheredScore> out(n);
  if (n == 0U) {
    return out;
  }

  // Serialize {genomes, run-start pool snapshot, panel, cfg} into ONE InputView.
  const std::vector<std::byte> input = parallel::serialize_mine_input(
      std::span<const Genome>{scored}, pool_item, panel, admit_fit, policy, sim);

  // Fixed-shape output slot per genome: header(24) + pnl[T]*8 + pos[T*N]*8, where T/N are
  // the PANEL dims (every realized stream has exactly those, single-alpha). One uniform
  // stride covers all shards (slot s written ONLY by shard s). T/N come from THIS process's
  // own trusted Panel (not untrusted bytes), so a wrap is a programmer error — assert the
  // products do not overflow rather than silently form an undersized slot (ATX_ASSERT for a
  // trusted-input precondition; slot_view_bytes also guards the n*stride product).
  const atx::usize t = panel.dates();
  const atx::usize ninst = panel.instruments();
  [[maybe_unused]] constexpr atx::usize kMax = static_cast<atx::usize>(-1);
  ATX_ASSERT(ninst == 0U || t <= kMax / ninst); // t * ninst (pos cells)
  const atx::usize pos_cells = t * ninst;
  ATX_ASSERT(t <= kMax / sizeof(atx::f64));         // pnl bytes
  ATX_ASSERT(pos_cells <= kMax / sizeof(atx::f64)); // pos bytes
  const atx::usize pnl_bytes = t * sizeof(atx::f64);
  const atx::usize pos_bytes = pos_cells * sizeof(atx::f64);
  // The SUM can wrap even though each product is asserted non-overflowing above; assert it
  // too (trusted panel dims -> a wrap is a programmer error, consistent with the product
  // asserts) so an undersized slot can never be silently formed and memcpy'd into.
  ATX_ASSERT(pnl_bytes <= kMax - 24U);
  ATX_ASSERT(pos_bytes <= kMax - (24U + pnl_bytes));
  const atx::usize slot_size = 24U + pnl_bytes + pos_bytes;

  std::vector<std::byte> buf(parallel::slot_view_bytes(n, slot_size), std::byte{0});
  parallel::SlotView slots = parallel::make_slot_view(buf, n, slot_size);
  const atx::core::Status s = exec.submit(
      parallel::WorkloadId::Mine, parallel::InputView{std::span<const std::byte>{input}}, n, slots);
  // A shard returns Err only on a malformed-input fault (never on a genome that simply
  // fails to compile — that is an in-band ok=0). An infra/parse fault is a programmer
  // error here (the parent serialized trusted, well-formed bytes), so fail loud.
  ATX_CHECK(s.has_value() && "Factory::gather_mine_scores: mine shard reported a fault");

  for (atx::usize k = 0; k < n; ++k) {
    out[k] = decode_mine_slot(slots.cslot(k), t, ninst);
  }
  return out;
}

} // namespace

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

[[nodiscard]] atx::core::Result<combine::AlphaMetrics>
Factory::metrics_on_panel(const Genome &g, const alpha::Panel &sub_panel, atx::f64 book_size,
                          std::vector<atx::f64> &pnl_out) const {
  // The SAME compile/eval/extract path as detail_eval_streams, but over an
  // arbitrary sub-panel (train OR holdout). So metrics_on_panel(g, full, ...)
  // reproduces the in-loop metrics exactly (train==holdout==full coincide).
  ATX_TRY(const alpha::Program prog, alpha::compile(g.ast, g.analysis));
  alpha::Engine engine{sub_panel};
  ATX_TRY(const alpha::SignalSet ss, engine.evaluate(prog));
  ATX_TRY(const alpha::AlphaStreams strm, alpha::extract_streams(ss, policy_, sub_panel, sim_));
  if (strm.n_alphas() == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Factory::metrics_on_panel: genome produced a zero-alpha stream");
  }
  const atx::usize n_inst = strm.n_instruments();
  // OWNED pnl copy handed back to the caller (it computes the holdout DSR from it
  // and the AlphaCandidate's pnl span aliases it across admit; §0.6).
  pnl_out.assign(strm.pnl(0).begin(), strm.pnl(0).end());
  const std::vector<atx::f64> pos = flatten_positions(strm);
  return atx::core::Ok(combine::compute_metrics(std::span<const atx::f64>{pnl_out},
                                                std::span<const atx::f64>{pos}, n_inst, book_size));
}

namespace {

// P2a — the holdout DSR, replicating the fitness.cpp deflated-Sharpe recipe over a
// REALIZED PnL stream (NOT the CPCV-aggregated stream): drop the structural index-0
// zero, de-annualize the metrics Sharpe by sqrt(252), compute population skew /
// excess-kurtosis of r[1..), and deflate by the running trial count N. Returns the
// DSR (== PSR against the expected-max benchmark). A too-short stream (< 2 real
// observations) returns 0.0 — it cannot clear any positive min_dsr bar.
[[nodiscard]] atx::f64 holdout_dsr(const combine::AlphaMetrics &metrics,
                                   std::span<const atx::f64> realized_pnl, atx::usize trial_count) {
  // Moments over r[1..) — drop the structural period-0 zero (combine §0-F).
  const std::span<const atx::f64> moments =
      (realized_pnl.size() > 1U) ? realized_pnl.subspan(1) : realized_pnl;
  const atx::usize T = moments.size();
  if (T < 2U) {
    return 0.0; // PSR is undefined for < 2 observations; fail the bar
  }
  // RECONCILIATION (§0.7): metrics.sharpe is ANNUALIZED; deflated_sharpe expects a
  // per-period Sharpe, so de-annualize by sqrt(252) (skew/kurtosis are scale-free).
  const atx::f64 per_period_sharpe = metrics.sharpe / std::sqrt(combine::kAnnualizationDays);
  const eval::DsrResult dsr =
      eval::deflated_sharpe(per_period_sharpe, T, eval::skewness(moments),
                            eval::excess_kurtosis(moments), trial_count, std::nullopt);
  return dsr.dsr;
}

} // namespace

[[nodiscard]] atx::core::Result<FactoryReport>
Factory::mine_into_oos(const FactoryConfig &cfg, library::Library &lib_lib,
                       const combine::AlphaGate &gate, SearchProgressSink *sink,
                       const SearchResumeState *resume) {
  FactoryReport rep;
  rep.library_n_alphas_before = lib_lib.n_alphas();

  // (0) carve the TRAIN / HOLDOUT split. The terminal cfg.oos_fraction of dates is
  // the holdout; an embargo gap (cfg.oos_embargo, else the CpcvConfig default)
  // precedes it. reserve_lockbox errors on a too-short panel / empty visible region.
  const atx::usize T = panel_.dates();
  const atx::usize embargo_len =
      (cfg.oos_embargo > 0.0)
          ? eval::detail::embargo_len_from_cpcv(cfg.oos_embargo, T)
          : eval::detail::embargo_len_from_cpcv(eval::CpcvConfig{}.embargo, T);
  auto sealed_r = eval::reserve_lockbox(panel_, cfg.oos_fraction, embargo_len);
  if (!sealed_r.has_value()) {
    // A too-short panel: surface a clear message (the controller may widen the panel
    // or shrink the fraction). No library mutation has occurred.
    return atx::core::Ok(std::move(rep)); // admitted == 0, oos_metrics empty; caller reads n_alphas unchanged
  }
  const eval::SealedPanel &sealed = *sealed_r;
  const atx::usize lockbox_begin = sealed.reservation().lockbox_begin;
  const alpha::Panel &train = sealed.visible(); // [0, lockbox_begin - embargo_len)

  auto holdout_r = eval::detail::slice_panel(panel_, lockbox_begin, T);
  if (!holdout_r.has_value()) {
    return atx::core::Ok(std::move(rep)); // holdout empty / unbuildable — nothing admitted
  }
  const alpha::Panel holdout = std::move(*holdout_r); // [lockbox_begin, T)

  // (1) run the S3-5 search over the TRAIN panel (NOT panel_). A fresh seeded driver
  // re-derives clean per-run state, preserving F1 replay. Selection scores against an
  // EMPTY scratch store (as the legacy path does); admission scores against the
  // persistent library below.
  combine::AlphaStore search_pool;
  SearchDriver driver{lib_, train, policy_, sim_, cfg.seed_exprs, cfg.panel_fields};
  const SearchResult res = driver.run(cfg.search, search_pool, sink, resume);

  // (8.C) OOS train-ranking corr length safety: rank the OOS candidates against an
  // EMPTY pool for the corr/diversify term. The candidate's ranking pnl here is
  // TRAIN-length, but the library's admitted streams (and thus the CorrNeighborIndex
  // t_) are HOLDOUT-length — so scoring a train-length candidate against a non-empty
  // library corr index is ill-defined (debug ATX_ASSERT / release garbage) AND
  // semantically meaningless (a train-window series vs a holdout-window pool). The
  // real decorrelation guarantee is enforced LATER by lib_lib.admit() → verdict_for →
  // worst_corr_to_pool(hold_pnl), which correlates HOLDOUT-vs-HOLDOUT-library
  // (consistent). The ranking only decides admission ORDER; the gate still rejects
  // over-correlated alphas. Mirrors the parallel submit#2's empty-pool snapshot.
  //
  // DIGEST-IDENTICAL today: lib_lib is always empty at mine_into_oos (discover wipes
  // the library each run), so LibraryPool{lib_lib}.worst_corr already returned 0 →
  // diversify == 1.0 → raw/dsr unchanged. An empty AlphaStorePool also returns 0
  // (corr_to_pool short-circuits to 0.0 for an empty store), so the ranking dsr/raw —
  // and hence the rank order, admitted set, and rep.digest — are byte-identical.
  const combine::AlphaStore empty_rank_pool;
  const AlphaStorePool rank_view{empty_rank_pool};

  rep.evaluated = res.trial_count;
  rep.trials = res.trial_count;
  rep.dedup_pct = res.dedup_pct;
  rep.seed = res.seed;
  rep.cse_pct = mean_cse_pct(res);
  rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

  // F4 — the admission deflation N is the search's running distinct-candidate count.
  FitnessCfg admit_fit = cfg.search.fitness;
  if (res.trial_count > 0U) {
    admit_fit.trial_count = res.trial_count;
  }

  // (2) rank the distinct scored candidates by deflated fitness against an EMPTY pool
  // (8.C — see rank_view above), scored over the TRAIN panel (the search's selection
  // domain). Mirrors rank_by_deflated_fitness(PoolView) but over `train` rather than
  // panel_, with the SAME total order (DESCENDING dsr, then raw, then canon_hash, then
  // idx; F1).
  //
  // PERF (Task 4): evaluate each candidate's train SignalSet EXACTLY ONCE. A single
  // panel-bound Engine is reused across candidates (byte-identical: Engine::evaluate
  // output depends only on (program, panel), never on prior engine state — see vm.hpp
  // comment). The precomputed SignalSet is threaded into pool_aware_fitness via the
  // `signals=` arg (fitness_core → eval_streams fast-path: skips compile+evaluate,
  // calls extract_streams directly — bit-identical per eval_streams precondition).
  // The SAME extract_streams result also produces train_metrics (flat compute_metrics
  // over all train periods — the same path metrics_on_panel used). Both the ranking
  // dsr/raw AND the report-only train_metrics therefore come from one train eval.
  // The small per-candidate result is cached (not the full streams) so the sort
  // (step 2) and the admission loop (step 3a) share the single eval.
  struct TrainResult {
    bool ok{false};
    combine::AlphaMetrics train_metrics{};
  };
  const atx::usize n_cands = res.all_scored.size();
  std::vector<TrainResult> train_cache(n_cands); // indexed by all_scored index

  std::vector<Ranked> ranked;
  ranked.reserve(n_cands);
  alpha::Engine train_engine{train}; // single Engine reused across all candidates (F4)
  for (atx::usize i = 0U; i < n_cands; ++i) {
    const Genome &cand = res.all_scored[i];
    atx::f64 dsr = 0.0;
    atx::f64 raw = 0.0;

    // Compile + evaluate ONCE on train; extract streams for both ranking and metrics.
    auto prog_r = alpha::compile(cand.ast, cand.analysis);
    if (prog_r.has_value()) {
      auto ss_r = train_engine.evaluate(*prog_r);
      if (ss_r.has_value()) {
        // (2a) ranking fitness — pass the precomputed SignalSet so pool_aware_fitness
        // skips compile+evaluate (eval_streams fast-path, bit-identical). Scored
        // against the EMPTY rank_view (8.C corr-length safety).
        auto fit = pool_aware_fitness(cand, rank_view, train, policy_, sim_, admit_fit,
                                      /*weak_panel=*/nullptr, /*engine=*/nullptr,
                                      /*signals=*/&(*ss_r));
        if (fit.has_value()) {
          dsr = fit->dsr;
          raw = fit->raw;
        }

        // (2b) train_metrics for the manifest's is_metrics — same SignalSet,
        // extract streams and compute flat metrics (identical to metrics_on_panel).
        auto strm_r = alpha::extract_streams(*ss_r, policy_, train, sim_);
        if (strm_r.has_value() && strm_r->n_alphas() > 0U) {
          const alpha::AlphaStreams &strm = *strm_r;
          const atx::usize n_inst = strm.n_instruments();
          std::vector<atx::f64> train_pnl(strm.pnl(0).begin(), strm.pnl(0).end());
          const std::vector<atx::f64> train_pos = flatten_positions(strm);
          train_cache[i] = TrainResult{
              /*ok=*/true,
              combine::compute_metrics(std::span<const atx::f64>{train_pnl},
                                       std::span<const atx::f64>{train_pos}, n_inst,
                                       cfg.book_size)};
        }
      }
    }
    ranked.push_back(Ranked{i, dsr, raw});
  }
  std::sort(ranked.begin(), ranked.end(), [&res](const Ranked &a, const Ranked &b) {
    if (a.dsr != b.dsr) {
      return a.dsr > b.dsr;
    }
    if (a.raw != b.raw) {
      return a.raw > b.raw;
    }
    if (res.all_scored[a.idx].canon_hash != res.all_scored[b.idx].canon_hash) {
      return res.all_scored[a.idx].canon_hash < res.all_scored[b.idx].canon_hash;
    }
    return a.idx < b.idx;
  });

  // (3) the mine -> CONFIRM-ON-HOLDOUT -> admit loop, best-train-deflated first.
  for (const Ranked &r : ranked) {
    const Genome &g = res.all_scored[r.idx];

    // (3a) TRAIN metrics (for the manifest's is_metrics — reporting only). Read from
    // the cache populated in step (2); no second train eval. A genome that failed to
    // evaluate on train (cache.ok == false) is dropped (F5), exactly as before.
    const TrainResult &tcache = train_cache[r.idx];
    if (!tcache.ok) {
      continue;
    }
    const combine::AlphaMetrics train_metrics = tcache.train_metrics;

    // (3b) HOLDOUT metrics + positions + DSR — the ADMISSION oracle. Evaluate the
    // genome ONCE on the holdout panel and realize BOTH the metrics (and pnl) and the
    // positions the durable record + the gate's corr-to-pool need. The owned hold_pnl
    // / hold_pos_flat outlive the admit() call (§0.6); the AlphaCandidate spans alias
    // them. A genome that cannot evaluate (or yields 0 alphas) on the holdout is
    // dropped, exactly as the legacy path drops an un-evaluable candidate (F5).
    std::vector<atx::f64> hold_pnl;
    std::vector<atx::f64> hold_pos_flat;
    combine::AlphaMetrics hold_metrics{};
    {
      auto prog_r = alpha::compile(g.ast, g.analysis);
      if (!prog_r.has_value()) {
        continue;
      }
      alpha::Engine engine{holdout};
      auto ss_r = engine.evaluate(*prog_r);
      if (!ss_r.has_value()) {
        continue;
      }
      auto strm_r = alpha::extract_streams(*ss_r, policy_, holdout, sim_);
      if (!strm_r.has_value() || strm_r->n_alphas() == 0U) {
        continue;
      }
      const alpha::AlphaStreams &strm = *strm_r;
      const atx::usize n_inst = strm.n_instruments();
      hold_pnl.assign(strm.pnl(0).begin(), strm.pnl(0).end());
      hold_pos_flat = flatten_positions(strm);
      hold_metrics = combine::compute_metrics(std::span<const atx::f64>{hold_pnl},
                                              std::span<const atx::f64>{hold_pos_flat}, n_inst,
                                              cfg.book_size);
    }

    const atx::f64 hold_dsr = holdout_dsr(hold_metrics, std::span<const atx::f64>{hold_pnl},
                                          admit_fit.trial_count);

    // (3c) F6 dedup key (canon_hash is 0 on the S3 search path; compute the real key).
    const atx::u64 canon_hash = canonical_hash(g.ast, g.ast.roots().front().root);

    library::Provenance prov{alpha::unparse(g.ast), /*parent_hashes=*/{},
                             /*mutation_op=*/0, /*seed=*/res.seed};

    // (3d) ADMISSION on the HOLDOUT: clear the factory deflation bar on the holdout
    // DSR, then library::admit on the HOLDOUT metrics + holdout pnl (so the durable
    // `metrics` are what was actually gated out-of-sample).
    library::AdmitKind kind = library::AdmitKind::RejectFitness; // non-accept sentinel
    if (hold_dsr >= cfg.min_dsr) {
      const library::AlphaCandidate cand{canon_hash,
                                         std::span<const atx::f64>{hold_pnl},
                                         std::span<const atx::f64>{hold_pos_flat},
                                         hold_metrics,
                                         std::move(prov),
                                         /*as_of=*/kAdmitAsOf,
                                         /*source=*/nullptr};
      // Cross-run accumulation guard (Task 8): the EXACT OOS-holdout geometry the
      // reopened-library footgun names. try_admit delegates to admit() VERBATIM on a
      // matching/fresh geometry (digest-identical) and propagates a CLEAN error when
      // hold_pnl.size() != the library's fixed t_ — HALTING the run instead of the
      // ATX_ASSERT abort (debug) / out-of-bounds projection read (release).
      ATX_TRY(const library::AdmitVerdict v, lib_lib.try_admit(cand, gate));
      kind = v.kind;
      if (kind == library::AdmitKind::Accept) {
        ++rep.admitted;
        rep.oos_metrics.push_back(OosReportEntry{canon_hash, train_metrics, hold_metrics});
      } else if (kind == library::AdmitKind::Duplicate) {
        ++rep.duplicates;
      }
    }
    ++rep.reject_histogram[static_cast<atx::usize>(kind)];

    rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
        static_cast<std::size_t>(rep.digest), canon_hash, static_cast<atx::u64>(kind)));
  }

  rep.library_n_alphas_after = lib_lib.n_alphas();
  return atx::core::Ok(std::move(rep));
}

[[nodiscard]] atx::core::Result<FactoryReport>
Factory::mine_into_oos_parallel(const FactoryConfig &cfg, library::Library &lib_lib,
                                const combine::AlphaGate &gate, parallel::IExecutor &exec) {
  // Task 5 — the PARALLEL out-of-sample admit path. This REPRODUCES the serial
  // mine_into_oos bit-for-bit (same digest / admitted / version_id / reject histogram /
  // oos_metrics), but the two expensive per-candidate VM evals — the TRAIN ranking eval
  // (serial step 2) and the HOLDOUT admission eval (serial step 3b) — cross the executor
  // seam via TWO gather_mine_scores submits, one per sub-panel, reusing the existing Mine
  // wire format UNCHANGED. Every step here is the EXACT serial mine_into_oos step it
  // mirrors (line refs below point at the serial reference); only the per-candidate eval
  // map moves to the workers. The stateful rank + library::admit fold stays SEQUENTIAL in
  // THIS parent process (workload_mine.hpp: admit CANNOT be partitioned byte-identically),
  // so the digest is byte-identical across substrates + worker counts by construction.
  FactoryReport rep;
  rep.library_n_alphas_before = lib_lib.n_alphas();

  // (0) carve the TRAIN / HOLDOUT split — IDENTICAL to serial mine_into_oos (the reserve
  // geometry, the embargo, the too-short-panel empty-report returns). The serial path is
  // the reference; this must produce the same train + holdout sub-panels bit-for-bit.
  const atx::usize T = panel_.dates();
  const atx::usize embargo_len =
      (cfg.oos_embargo > 0.0)
          ? eval::detail::embargo_len_from_cpcv(cfg.oos_embargo, T)
          : eval::detail::embargo_len_from_cpcv(eval::CpcvConfig{}.embargo, T);
  auto sealed_r = eval::reserve_lockbox(panel_, cfg.oos_fraction, embargo_len);
  if (!sealed_r.has_value()) {
    return atx::core::Ok(std::move(rep)); // too-short panel — same empty report the serial path returns
  }
  const eval::SealedPanel &sealed = *sealed_r;
  const atx::usize lockbox_begin = sealed.reservation().lockbox_begin;
  const alpha::Panel &train = sealed.visible(); // [0, lockbox_begin - embargo_len)

  auto holdout_r = eval::detail::slice_panel(panel_, lockbox_begin, T);
  if (!holdout_r.has_value()) {
    return atx::core::Ok(std::move(rep)); // holdout empty / unbuildable — nothing admitted (same as serial)
  }
  const alpha::Panel holdout = std::move(*holdout_r); // [lockbox_begin, T)

  // (1) run the S3-5 search over the TRAIN panel in the PARENT — IDENTICAL to serial
  // mine_into_oos (a fresh seeded driver over `train`, F1 replay). The search's own
  // internal parallelism is separate and unchanged; this task parallelizes ADMISSION
  // evals, not the search. (sink/resume are not threaded into the substrate-aware
  // mine_into entry point — same as the non-OOS parallel path, which also runs the
  // search with the no-progress-sink driver.run(cfg, store) overload.)
  combine::AlphaStore search_pool;
  SearchDriver driver{lib_, train, policy_, sim_, cfg.seed_exprs, cfg.panel_fields};
  const SearchResult res = driver.run(cfg.search, search_pool);

  rep.evaluated = res.trial_count;
  rep.trials = res.trial_count;
  rep.dedup_pct = res.dedup_pct;
  rep.seed = res.seed;
  rep.cse_pct = mean_cse_pct(res);
  rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

  // F4 — the admission deflation N is the search's running distinct-candidate count
  // (IDENTICAL to serial mine_into_oos).
  FitnessCfg admit_fit = cfg.search.fitness;
  if (res.trial_count > 0U) {
    admit_fit.trial_count = res.trial_count;
  }

  // (Submit #1) TRAIN RANKING eval over the executor — reproduces serial step 2
  // (mine_into_oos) bit-for-bit: the per-genome pool_aware_fitness(cand, EMPTY pool,
  // train) dsr/raw AND the train SignalSet -> train streams. gather_mine_scores serializes
  // `train` as the panel, so each worker's compile+eval+extract_streams runs over the SAME
  // train sub-panel the serial single Engine{train} runs over (Engine output depends only
  // on (program, panel) — vm.hpp). A worker's gathered ok==1 <=> the serial tcache.ok
  // (both require a successful single-alpha train eval); its dsr/raw == the serial step-2
  // ranking dsr/raw (the GenomeRoundTripsThroughSerializeParse equivalence proof).
  //
  // (8.C) EMPTY POOL on purpose — the corr-length safety fix, MIRRORING submit#2 below.
  // This submit ranks TRAIN-length candidate pnl; the library's admitted streams (and the
  // CorrNeighborIndex t_) are HOLDOUT-length. Ranking a train-length pnl against a
  // holdout-length library corr index is ill-defined (CorrNeighborIndex::signature
  // ATX_ASSERTs pnl.size() == t_ → debug assert / release garbage once the library is
  // non-empty) AND semantically meaningless. So submit#1 passes an EMPTY pool snapshot
  // (pool_n_alphas = 0): the train STREAMS + the `ok` flag are POOL-INDEPENDENT (compile +
  // evaluate + extract_streams, never the pool), and the only pool-fed term is the
  // candidate's worst-corr ranking (dsr/raw) — for which an empty pool yields worst_corr 0.
  // DIGEST-IDENTICAL today: the discover stage wipes the library each run, so it is empty
  // at mine_into_oos and worst_corr already returned 0; the serial step-2 fix uses the
  // SAME empty-pool ranking, so serial and parallel stay byte-identical and symmetric. The
  // real decorrelation gate (lib.admit() → worst_corr_to_pool(hold_pnl), HOLDOUT-vs-HOLDOUT)
  // is unaffected — ranking only decides admission ORDER. n_periods is the train length so
  // the empty snapshot is self-consistent; pool_seed is irrelevant when pool_n_alphas == 0
  // (no index is built). Pinned byte-identical by the OOS invariance test.
  parallel::MineWorkItem train_pool_item;
  train_pool_item.pool_pnl_flat = std::span<const atx::f64>{}; // empty: no admitted streams
  train_pool_item.pool_n_alphas = 0U;
  train_pool_item.n_periods = train.dates();
  train_pool_item.pool_seed = 0ULL; // unused (no corr index built for an empty pool)
  const std::vector<GatheredScore> train_gathered =
      gather_mine_scores(res.all_scored, train_pool_item, admit_fit, train, policy_, sim_, exec);

  // Derive each candidate's TRAIN metrics (serial step 2b — the manifest is_metrics) from
  // the gathered train streams via compute_metrics, IDENTICAL to the serial
  // compute_metrics(train_pnl, train_pos, n_inst, book_size). Keep only the SMALL metrics
  // (and the ok bit); the full train streams are then DISCARDED. MEMORY ARGUMENT: the
  // train streams are needed ONLY to derive train_metrics (reporting) and the dsr/raw
  // (already gathered) — they are NEVER admitted (only the HOLDOUT streams are). Holding
  // both panels' full streams for every candidate would ~double peak memory for no use;
  // the holdout streams alone are what the admit loop consumes.
  struct TrainResult {
    bool ok{false};
    combine::AlphaMetrics train_metrics{};
  };
  const atx::usize n_cands = res.all_scored.size();
  std::vector<TrainResult> train_cache(n_cands);
  for (atx::usize i = 0U; i < n_cands; ++i) {
    const GatheredScore &ts = train_gathered[i];
    if (ts.ok != 1U) {
      continue; // a genome that failed the train eval — serial tcache.ok == false (F5).
    }
    const alpha::AlphaStreams &strm = ts.streams;
    if (strm.n_alphas() == 0U) {
      continue; // defensive (gathered ok==1 already implies n_alphas>0); mirrors serial.
    }
    const atx::usize n_inst = strm.n_instruments();
    std::vector<atx::f64> train_pnl(strm.pnl(0).begin(), strm.pnl(0).end());
    const std::vector<atx::f64> train_pos = flatten_positions(strm);
    train_cache[i] = TrainResult{
        /*ok=*/true,
        combine::compute_metrics(std::span<const atx::f64>{train_pnl},
                                 std::span<const atx::f64>{train_pos}, n_inst, cfg.book_size)};
    // train streams discarded here (train_pnl/train_pos die at scope end).
  }

  // (2) RANK by deflated fitness over the gathered TRAIN dsr/raw — the SAME total order as
  // serial mine_into_oos (mine_into_oos:743-754): DESC dsr, DESC raw, ASC canon_hash, ASC
  // idx. canon_hash is 0 on the S3 search path, so the idx tiebreak pins a true total
  // order (std::sort is not stable), reproducing the serial rank deterministically (F1).
  std::vector<Ranked> ranked;
  ranked.reserve(n_cands);
  for (atx::usize i = 0U; i < n_cands; ++i) {
    ranked.push_back(Ranked{i, train_gathered[i].dsr, train_gathered[i].raw});
  }
  std::sort(ranked.begin(), ranked.end(), [&res](const Ranked &a, const Ranked &b) {
    if (a.dsr != b.dsr) {
      return a.dsr > b.dsr;
    }
    if (a.raw != b.raw) {
      return a.raw > b.raw;
    }
    if (res.all_scored[a.idx].canon_hash != res.all_scored[b.idx].canon_hash) {
      return res.all_scored[a.idx].canon_hash < res.all_scored[b.idx].canon_hash;
    }
    return a.idx < b.idx;
  });

  // (Submit #2) HOLDOUT eval over the executor — reproduces serial step 3b
  // (mine_into_oos:778-799) bit-for-bit: each worker compiles+evaluates+extract_streams
  // over the SAME `holdout` sub-panel the serial Engine{holdout} runs over (same lookback
  // warmup — NOT a full-panel-then-slice eval, which would change the warmup). We use ONLY
  // the holdout STREAMS + the `ok` bit per genome; the gathered dsr/raw are DISCARDED here
  // (the serial holdout path is not pool-ranked — it computes hold_dsr in-parent from the
  // realized stream).
  //
  // EMPTY POOL on purpose: this submit passes an EMPTY pool snapshot (pool_n_alphas = 0),
  // exactly like the train submit#1 above (8.C). The holdout streams + the `ok` flag are
  // POOL-INDEPENDENT — they come from compile + evaluate + extract_streams, never from the
  // pool — and the only thing the pool snapshot feeds is the candidate's worst-corr ranking
  // (dsr/raw), which we discard here. So an empty pool leaves the gathered streams + ok
  // BYTE-IDENTICAL while (a) avoiding a needless worst-corr computation per candidate and
  // (b) removing a latent length edge: the library corr index is built at t_ = the library's
  // stored (holdout-length) period count, and pool-ranking a holdout-length candidate pnl
  // against it is fine, but pool-ranking on the holdout submit was never needed. n_periods
  // is set to the holdout length so the empty snapshot is self-consistent; pool_seed is
  // irrelevant when pool_n_alphas == 0 (no index is built). This does NOT move report.digest
  // / admitted / version_id (only the discarded dsr/raw change) — pinned by the OOS
  // invariance test.
  parallel::MineWorkItem hold_pool_item;
  hold_pool_item.pool_pnl_flat = std::span<const atx::f64>{}; // empty: no admitted streams
  hold_pool_item.pool_n_alphas = 0U;
  hold_pool_item.n_periods = holdout.dates();
  hold_pool_item.pool_seed = 0ULL; // unused (no corr index built for an empty pool)
  const std::vector<GatheredScore> hold_gathered =
      gather_mine_scores(res.all_scored, hold_pool_item, admit_fit, holdout, policy_, sim_, exec);

  // (3) the mine -> CONFIRM-ON-HOLDOUT -> admit loop, best-train-deflated first. SEQUENTIAL
  // in the parent — byte-identical to serial mine_into_oos:757-835 (same F5 drops, same
  // hold_dsr, same gate, same admit, same digest fold, same oos_metrics push).
  for (const Ranked &r : ranked) {
    const Genome &g = res.all_scored[r.idx];

    // (3a) TRAIN metrics from the cache (serial step 3a). A genome that failed the train
    // eval (cache.ok == false) is dropped (F5) — IDENTICAL to serial mine_into_oos:763-766.
    const TrainResult &tcache = train_cache[r.idx];
    if (!tcache.ok) {
      continue;
    }
    const combine::AlphaMetrics train_metrics = tcache.train_metrics;

    // (3b) HOLDOUT metrics + positions + DSR — the ADMISSION oracle, from the gathered
    // holdout streams (serial mine_into_oos:769-799). A genome that could not evaluate (or
    // yielded 0 alphas) on the holdout is dropped (F5), exactly as the serial path drops it
    // (the gathered ok != 1 <=> the serial compile/eval/extract/0-alpha continue chain).
    const GatheredScore &hs = hold_gathered[r.idx];
    if (hs.ok != 1U) {
      continue;
    }
    const alpha::AlphaStreams &hstrm = hs.streams;
    if (hstrm.n_alphas() == 0U) {
      continue;
    }
    const atx::usize n_inst = hstrm.n_instruments();
    // OWNED holdout copies kept alive across admit() — the AlphaCandidate spans alias them.
    std::vector<atx::f64> hold_pnl(hstrm.pnl(0).begin(), hstrm.pnl(0).end());
    std::vector<atx::f64> hold_pos_flat = flatten_positions(hstrm);
    const combine::AlphaMetrics hold_metrics =
        combine::compute_metrics(std::span<const atx::f64>{hold_pnl},
                                 std::span<const atx::f64>{hold_pos_flat}, n_inst, cfg.book_size);

    const atx::f64 hold_dsr = holdout_dsr(hold_metrics, std::span<const atx::f64>{hold_pnl},
                                          admit_fit.trial_count);

    // (3c) F6 dedup key (canon_hash is 0 on the S3 search path; compute the real key).
    const atx::u64 canon_hash = canonical_hash(g.ast, g.ast.roots().front().root);

    library::Provenance prov{alpha::unparse(g.ast), /*parent_hashes=*/{},
                             /*mutation_op=*/0, /*seed=*/res.seed};

    // (3d) ADMISSION on the HOLDOUT — IDENTICAL to serial mine_into_oos:813-830.
    library::AdmitKind kind = library::AdmitKind::RejectFitness; // non-accept sentinel
    if (hold_dsr >= cfg.min_dsr) {
      const library::AlphaCandidate cand{canon_hash,
                                         std::span<const atx::f64>{hold_pnl},
                                         std::span<const atx::f64>{hold_pos_flat},
                                         hold_metrics,
                                         std::move(prov),
                                         /*as_of=*/kAdmitAsOf,
                                         /*source=*/nullptr};
      // Cross-run accumulation guard (Task 8) — same geometry-checked seam as serial
      // mine_into_oos: digest-identical on a match, CLEAN propagated error on mismatch.
      ATX_TRY(const library::AdmitVerdict v, lib_lib.try_admit(cand, gate));
      kind = v.kind;
      if (kind == library::AdmitKind::Accept) {
        ++rep.admitted;
        rep.oos_metrics.push_back(OosReportEntry{canon_hash, train_metrics, hold_metrics});
      } else if (kind == library::AdmitKind::Duplicate) {
        ++rep.duplicates;
      }
    }
    ++rep.reject_histogram[static_cast<atx::usize>(kind)];

    rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
        static_cast<std::size_t>(rep.digest), canon_hash, static_cast<atx::u64>(kind)));
  }

  rep.library_n_alphas_after = lib_lib.n_alphas();
  return atx::core::Ok(std::move(rep));
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
