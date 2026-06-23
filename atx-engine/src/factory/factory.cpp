#include "atx/engine/factory/factory.hpp"

#include <algorithm> // std::sort, std::max, std::min
#include <cmath>     // std::sqrt (P2a holdout DSR de-annualization)
#include <cstddef>   // std::size_t (hash_combine seed type)
#include <limits>    // std::numeric_limits (W4b run-level PBO sentinel)
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
#include "atx/engine/eval/pbo.hpp"             // eval::pbo_cscv_checked, PboResult (W4b + R3b run-level PBO)
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, eval::excess_kurtosis (P2a DSR)

#include "atx/engine/library/record.hpp" // library::Provenance (admitted-alpha lineage)

#include "atx/engine/factory/canonical.hpp" // factory::canonical_hash (F6 dedup key)

#include "atx/engine/parallel/executor.hpp" // parallel::IExecutor, Substrate, SlotView (S7.5d seam)
#include "atx/engine/parallel/workload_mine.hpp" // parallel::serialize_mine_input / MineGenomeResult (S7.5d)

namespace atx::engine::factory {

namespace {

// W4a — the OPTIONAL split-sample stability floor over a REALIZED PnL stream (the
// library admit paths: mine_into + mine_into_oos, serial AND substrate-aware
// parallel). INACTIVE (the -inf disabling default) -> always true, so the admit
// expression is byte-identical to the pre-W4a screen. ACTIVE -> the candidate passes
// iff its OOS PnL (index-0 zero dropped, floor-midpoint split via
// detail::split_half_sharpe — the SAME rule as fitness_core) is split-stable (both
// halves share the full-sample Sharpe sign) AND both half-Sharpes clear the floor.
// The full-sample sign comes from metrics.sharpe (annualized; sign-preserving).
// Computed from cand_pnl + metrics (NOT a PoolView fit), so the serial and parallel
// mine_into paths — which both hold those locals but the parallel Mine wire format
// does NOT carry the split fields — reach the IDENTICAL decision (the byte-identity
// invariant). PURE; no RNG.
[[nodiscard]] bool split_floor_ok(atx::f64 min_split_sharpe,
                                  std::span<const atx::f64> realized_pnl,
                                  const combine::AlphaMetrics &metrics) noexcept {
  if (!std::isfinite(min_split_sharpe)) {
    return true; // floor disabled (default) -> byte-identical to the pre-W4a screen
  }
  const std::span<const atx::f64> moments =
      (realized_pnl.size() > 1U) ? realized_pnl.subspan(1) : realized_pnl;
  const atx::f64 full_sign = (metrics.sharpe > 0.0) ? 1.0 : (metrics.sharpe < 0.0 ? -1.0 : 0.0);
  const detail::SplitHalf sh = detail::split_half_sharpe(moments, full_sign);
  return sh.stable && sh.sharpe_h1 >= min_split_sharpe && sh.sharpe_h2 >= min_split_sharpe;
}

// C2.2 (measurement-only) — fill report.scored_canon_hashes from the run's DISTINCT
// scored set (res.all_scored; one entry per scored canon_hash, size == res.trial_count).
// REPORT-ONLY telemetry: a parent-side copy of already-computed canon_hashes — it is
// NOT folded into rep.digest and changes no admission decision, so the digest, admitted
// set, and library version_id stay byte-identical. Called at each rep.evaluated site.
inline void fill_scored_hashes(FactoryReport &rep, const SearchResult &res) {
  rep.scored_canon_hashes.clear();
  rep.scored_canon_hashes.reserve(res.all_scored.size());
  for (const Genome &g : res.all_scored) {
    rep.scored_canon_hashes.push_back(g.canon_hash);
  }
}

} // namespace

namespace detail {

// W4b — the run-level CSCV-PBO verdict over the admitted alphas' realized OOS PnL.
// POST-HOC + PURE (eval::pbo_cscv is order-fixed, no RNG): computing it ADDS to the
// report's PBO fields and touches NOTHING the admission digest folds, so the digest is
// byte-identical by construction. INACTIVE (the 1.0 default) -> skip everything,
// sentinels intact -> byte-identical legacy path. See the header contract.
void finalize_run_pbo(FactoryReport &rep,
                      const std::vector<std::vector<atx::f64>> &admitted_pnls,
                      atx::f64 max_pbo,
                      bool always_compute) {
  // (1) Off (the disabling 1.0 default): NO compute, all PBO fields stay at sentinels
  // (rep.pbo == NaN, gate passes) -> byte-identical to the pre-W4b path. A3: when
  // always_compute is true (the OOS always-on holdout diagnostic, oos_pbo), proceed to
  // compute even at the OFF default; the gate verdict still fail-opens (step 7).
  if (max_pbo >= 1.0 && !always_compute) {
    return;
  }

  // (2) Need >= 2 admitted rows to form a cross-section (pbo_cscv needs n_candidates
  // >= 2). Fewer -> infeasible: leave sentinels, gate passes (fail-OPEN).
  const atx::usize n_candidates = admitted_pnls.size();
  if (n_candidates < 2U) {
    return;
  }

  // (3) Drop index 0 of each row (the §0-F combine structural zero — the same
  // .subspan(1) / split_floor_ok convention). T is the post-drop length of row 0; the
  // rows ARE equal-length by construction (same eval window). A mismatch (or a row too
  // short to drop) is treated as infeasible: sentinels, gate passes.
  const atx::usize raw_t = admitted_pnls.front().size();
  if (raw_t < 2U) {
    return; // a single-element (or empty) row leaves no post-drop period.
  }
  const atx::usize periods = raw_t - 1U; // T after dropping the index-0 structural zero
  for (const std::vector<atx::f64> &row : admitted_pnls) {
    if (row.size() != raw_t) {
      return; // ragged input — never happens by construction; fail-OPEN if it ever does.
    }
  }

  // (4) n_splits = the largest EVEN value <= min(8, T). 8 is Bailey's standard CSCV S
  // (C(8,4)=70 splits); auto-clamp DOWN for short panels (CSCV needs S non-empty
  // sub-periods, S even). If the clamp drops below 2 the panel is too short -> skip.
  atx::usize n_splits = (periods < 8U) ? periods : 8U;
  if ((n_splits % 2U) != 0U) {
    --n_splits; // round DOWN to the nearest even split count
  }
  if (n_splits < 2U) {
    return; // too few periods to split -> infeasible (sentinels, gate passes).
  }

  // (5) Build the candidate-major flat matrix M[c*T + t] (each admitted alpha's
  // post-drop return row concatenated, deterministic admit order).
  std::vector<atx::f64> matrix;
  matrix.reserve(n_candidates * periods);
  for (const std::vector<atx::f64> &row : admitted_pnls) {
    matrix.insert(matrix.end(), row.begin() + 1, row.end()); // drop index 0
  }

  // (6) Run the CHECKED CSCV-PBO (handles an infeasible matrix via Err, never aborts).
  auto pbo_r =
      eval::pbo_cscv_checked(std::span<const atx::f64>{matrix}, n_candidates, n_splits);
  if (!pbo_r.has_value()) {
    return; // infeasible matrix -> leave sentinels, gate passes (fail-OPEN).
  }

  // (7) RECORD the verdict. The gate is ADVISORY-but-RECORDED: a breach sets
  // pbo_gate_passed = false (surfaced + warned downstream) but never un-persists an
  // alpha or alters admission. A3: when the gate is OFF (max_pbo >= 1.0, reachable
  // here only via always_compute), the PBO is recorded but the verdict FAIL-OPENS —
  // a run is never failed merely for the gate being off. When the gate is active
  // (max_pbo < 1.0), strict pbo > max_pbo is a real test (PBO ∈ [0,1]).
  const eval::PboResult &res = *pbo_r;
  rep.pbo = res.pbo;
  rep.pbo_mean_logit = res.mean_logit;
  rep.pbo_n_candidates = n_candidates;
  rep.pbo_n_splits = n_splits;
  rep.pbo_gate_passed = (max_pbo >= 1.0) ? true : !(rep.pbo > max_pbo);
}

} // namespace detail

[[nodiscard]] FactoryReport Factory::mine(const FactoryConfig &cfg, combine::AlphaStore &pool,
                                          const combine::AlphaGate &gate) {
  FactoryReport rep;

  // (1) run the S3-5 search. The driver re-derives a clean per-run state from the
  // seed, so a fresh driver per mine() preserves F1 replay (no carried state).
  SearchDriver driver{lib_,           panel_,           policy_,         sim_,
                      cfg.seed_exprs, cfg.panel_fields, cfg.weak_panel,  // W4a robust factor
                      cfg.numeric_excluded_fields, cfg.extra_group_fields}; // R1 typed-fields
  const SearchResult res = driver.run(cfg.search, pool);

  rep.evaluated = res.trial_count;
  fill_scored_hashes(rep, res); // C2.2 report-only: distinct scored canon_hashes (not in digest)
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

  // W4b — accumulate each admitted alpha's realized OOS PnL (deterministic admit order)
  // for the POST-HOC run-level CSCV-PBO verdict; finalized once after the loop. Empty +
  // unused at the max_pbo == 1.0 default (finalize_run_pbo returns immediately).
  std::vector<std::vector<atx::f64>> admitted_pnls;

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

    // W4a split-sample stability floor (OPTIONAL; default DISABLED). UNIFIED with the
    // three library admit paths (mine_into + mine_into_oos serial/parallel): gate via
    // the SAME split_floor_ok helper over the REALIZED full-OOS PnL stream + metrics
    // (cand_pnl, metrics — the locals computed at 3a/3b above), NOT the CPCV-aggregated
    // PoolView `fit->{split_stable,sharpe_h1,sharpe_h2}` fields. ONE statistical
    // contract behind the knob across all four mine paths: split-stability measured on
    // the realized stream. (fitness_core still COMPUTES the fit-> split fields for
    // observability/recording; the gate decision no longer reads them.) Inactive
    // (-inf default) -> split_floor_ok returns true immediately, so the accept
    // expression collapses to the exact pre-W4a (verdict==Accept) && (dsr>=min_dsr).
    const bool split_ok =
        split_floor_ok(cfg.min_split_sharpe, std::span<const atx::f64>{cand_pnl}, metrics);
    const bool accept =
        (verdict == combine::GateVerdict::Accept) && (dsr >= cfg.min_dsr) && split_ok;
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
      admitted_pnls.push_back(cand_pnl); // W4b: realized OOS PnL of an admitted alpha
    }
    // An insert Err (a period/shape mismatch against an established pool shape) is
    // a candidate that cannot coherently join this pool — it is screened out and
    // NOT counted as admitted, but the digest already recorded the accept decision
    // (the verdict was Accept; the structural reject is a downstream pool fact).
  }

  // W4b — POST-HOC run-level CSCV-PBO over the admitted set (no-op at the 1.0 default;
  // never alters rep.digest or any admission decision).
  detail::finalize_run_pbo(rep, admitted_pnls, cfg.max_pbo);

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
  SearchDriver driver{lib_,           panel_,           policy_,         sim_,
                      cfg.seed_exprs, cfg.panel_fields, cfg.weak_panel,  // W4a robust factor
                      cfg.numeric_excluded_fields, cfg.extra_group_fields}; // R1 typed-fields
  const SearchResult res = driver.run(cfg.search, search_pool, sink, resume);

  // The persistent library is the ADMISSION pool: the deflated-fitness ranking and
  // the per-candidate re-score below score marginal corr against it (O(neighbors)).
  LibraryPool view{lib_lib};

  rep.evaluated = res.trial_count;
  fill_scored_hashes(rep, res); // C2.2 report-only: distinct scored canon_hashes (not in digest)
  rep.trials = res.trial_count;
  rep.dedup_pct = res.dedup_pct;
  rep.seed = res.seed;
  rep.cse_pct = mean_cse_pct(res);
  rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

  // F4 / R1 — the admission deflation N is prior cumulative N + this run's N, so a
  // multi-run accumulation sweep is visible to the multiple-testing defense. When
  // prior == 0 (fresh library / single run) this equals res.trial_count — byte-
  // identical to the pre-R1 path.
  FitnessCfg admit_fit = cfg.search.fitness;
  const atx::u64 prior_r1 = lib_lib.cumulative_trials(); // R1: cross-run cumulative N
  if (res.trial_count > 0U) {
    admit_fit.trial_count = static_cast<atx::usize>(prior_r1) + res.trial_count;
  }

  // (2) rank the distinct scored candidates by deflated fitness against the LIBRARY
  // (the PoolView overload routes the corr-to-pool through the O(neighbors) index).
  std::vector<Ranked> ranked = rank_by_deflated_fitness(res.all_scored, admit_fit, view);

  // W4b — accumulate each admitted alpha's realized OOS PnL (deterministic admit order)
  // for the POST-HOC run-level CSCV-PBO verdict; finalized once after the loop. Empty +
  // unused at the max_pbo == 1.0 default.
  std::vector<std::vector<atx::f64>> admitted_pnls;

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

    // W4a split-sample stability floor (OPTIONAL; default DISABLED). Computed from
    // the REALIZED full-panel PnL stream + metrics (NOT the PoolView `fit`), so the
    // sequential and substrate-aware parallel mine_into paths — which both hold
    // cand_pnl + metrics but the parallel Mine wire format does NOT carry the split
    // fields — reach the IDENTICAL decision (the serial/parallel byte-identity
    // invariant). Inactive (-inf default) -> always true (byte-identical pre-W4a bar).
    const bool split_ok =
        split_floor_ok(cfg.min_split_sharpe, std::span<const atx::f64>{cand_pnl}, metrics);
    // The deflation bar is FACTORY-side: clear it BEFORE library::admit is consulted.
    library::AdmitKind kind =
        library::AdmitKind::RejectFitness; // non-accept sentinel for the histogram
    if (dsr >= cfg.min_dsr && split_ok) {
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
        admitted_pnls.push_back(cand_pnl); // W4b: realized OOS PnL of an admitted alpha
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

  // W4b — POST-HOC run-level CSCV-PBO over the admitted set (no-op at the 1.0 default).
  detail::finalize_run_pbo(rep, admitted_pnls, cfg.max_pbo);

  rep.library_n_alphas_after = lib_lib.n_alphas();
  // R1: increment the cumulative trial counter ONCE per mine run (by this run's N),
  // AFTER the admission loop so THIS run's survivors were deflated against
  // (prior + this_run_N) and the NEXT run sees the updated cumulative.
  if (res.trial_count > 0U) {
    lib_lib.add_trials(static_cast<atx::u64>(res.trial_count));
  }
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
  SearchDriver driver{lib_,           panel_,           policy_,         sim_,
                      cfg.seed_exprs, cfg.panel_fields, cfg.weak_panel,  // W4a robust factor
                      cfg.numeric_excluded_fields, cfg.extra_group_fields}; // R1 typed-fields
  const SearchResult res = driver.run(cfg.search, search_pool);

  rep.evaluated = res.trial_count;
  fill_scored_hashes(rep, res); // C2.2 report-only: distinct scored canon_hashes (not in digest)
  rep.trials = res.trial_count;
  rep.dedup_pct = res.dedup_pct;
  rep.seed = res.seed;
  rep.cse_pct = mean_cse_pct(res);
  rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

  // F4 / R1 — identical to the sequential path: prior + this run's N (see serial
  // mine_into above for the full reasoning). When prior == 0 this is res.trial_count.
  FitnessCfg admit_fit = cfg.search.fitness;
  const atx::u64 prior_r1_par = lib_lib.cumulative_trials(); // R1: cross-run cumulative N
  if (res.trial_count > 0U) {
    admit_fit.trial_count = static_cast<atx::usize>(prior_r1_par) + res.trial_count;
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

  // W4b — accumulate each admitted alpha's realized OOS PnL (deterministic admit order,
  // at the SEQUENTIAL parent admit-Ok point — NOT in workers) for the POST-HOC run-level
  // CSCV-PBO verdict; finalized once after the loop. Empty + unused at the 1.0 default.
  std::vector<std::vector<atx::f64>> admitted_pnls;

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

    // W4a split-sample stability floor — IDENTICAL rule + inputs (cand_pnl + metrics)
    // to the sequential mine_into path, so the serial/parallel byte-identity holds.
    // Default DISABLED (-inf) -> always true (byte-identical to the pre-W4a bar).
    const bool split_ok =
        split_floor_ok(cfg.min_split_sharpe, std::span<const atx::f64>{cand_pnl}, metrics);
    library::AdmitKind kind = library::AdmitKind::RejectFitness;
    if (dsr >= cfg.min_dsr && split_ok) {
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
        admitted_pnls.push_back(cand_pnl); // W4b: realized OOS PnL of an admitted alpha
      } else if (kind == library::AdmitKind::Duplicate) {
        ++rep.duplicates;
      }
    }
    ++rep.reject_histogram[static_cast<atx::usize>(kind)];

    rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
        static_cast<std::size_t>(rep.digest), canon_hash, static_cast<atx::u64>(kind)));
  }

  // W4b — POST-HOC run-level CSCV-PBO over the admitted set (no-op at the 1.0 default;
  // accumulated at the SEQUENTIAL parent admit-Ok point, so it is deterministic on every
  // substrate + worker count). Never alters rep.digest.
  detail::finalize_run_pbo(rep, admitted_pnls, cfg.max_pbo);

  rep.library_n_alphas_after = lib_lib.n_alphas();
  // R1: increment once per mine run, AFTER the loop — same as serial mine_into.
  if (res.trial_count > 0U) {
    lib_lib.add_trials(static_cast<atx::u64>(res.trial_count));
  }
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

// R2 — Pearson correlation of two equal-length non-empty vectors. Returns NaN when
// either series has zero variance (degenerate date — caller skips it). Distinct from
// the latent.hpp pearson() which returns 0.0 on a degenerate input; here NaN lets the
// caller distinguish "no relationship computable" from "correlation is 0" so degenerate
// dates are excluded from the loading average rather than being counted as zero.
[[nodiscard]] atx::f64 pearson_for_price_scale(std::span<const atx::f64> a,
                                               std::span<const atx::f64> b) noexcept {
  const atx::usize n = a.size();
  if (n < 2U) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 ma = 0.0;
  atx::f64 mb = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= static_cast<atx::f64>(n);
  mb /= static_cast<atx::f64>(n);
  atx::f64 cov = 0.0;
  atx::f64 va  = 0.0;
  atx::f64 vb  = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 da = a[i] - ma;
    const atx::f64 db = b[i] - mb;
    cov += da * db;
    va  += da * da;
    vb  += db * db;
  }
  if (va == 0.0 || vb == 0.0) {
    return std::numeric_limits<atx::f64>::quiet_NaN(); // degenerate: skip this date
  }
  return cov / std::sqrt(va * vb);
}

// R2 — price-scale loading: |time-averaged cross-sectional Pearson(w, 1/raw_close)|
// over the holdout window. Returns NaN when raw_close is absent from the panel (caller
// must check for field existence before calling) or when no holdout date is usable.
// hold_pos_flat is period-major (t*N+i layout, i.e. flatten_positions output).
// INACTIVE guard (cfg.max_price_scale_corr >= 1.0) -> never called -> zero overhead.
[[nodiscard]] atx::f64 price_scale_loading(const atx::f64 *hold_pos_flat, atx::usize T,
                                           atx::usize N, const alpha::Panel &holdout,
                                           alpha::FieldId raw_close_fid) noexcept {
  std::vector<atx::f64> w_buf;
  std::vector<atx::f64> f_buf;
  atx::f64  sum_corr = 0.0;
  atx::usize n_dates = 0U;
  for (atx::usize t = 0U; t < T; ++t) {
    const std::span<const atx::f64> rc = holdout.field_cross_section(raw_close_fid, t);
    w_buf.clear();
    f_buf.clear();
    for (atx::usize i = 0U; i < N; ++i) {
      const atx::f64 w = hold_pos_flat[t * N + i]; // period-major: t*N+i
      const atx::f64 p = rc[i];
      if (!std::isfinite(w) || w == 0.0) {
        continue; // not in book: skip
      }
      if (!std::isfinite(p) || p <= 0.0) {
        continue; // invalid price: skip
      }
      w_buf.push_back(w);
      f_buf.push_back(1.0 / p);
    }
    if (w_buf.size() < 2U) {
      continue; // fewer than 2 valid names: skip date
    }
    const atx::f64 c = pearson_for_price_scale(std::span<const atx::f64>{w_buf},
                                               std::span<const atx::f64>{f_buf});
    if (!std::isfinite(c)) {
      continue; // zero-variance date: skip
    }
    sum_corr += c;
    ++n_dates;
  }
  if (n_dates == 0U) {
    return std::numeric_limits<atx::f64>::quiet_NaN(); // no usable dates
  }
  return std::abs(sum_corr / static_cast<atx::f64>(n_dates));
}

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

  // (0) carve the TRAIN / HOLDOUT split.
  //  Default (oos_n_windows == 0): the terminal cfg.oos_fraction of dates is the holdout;
  //  an embargo gap precedes it. reserve_lockbox errors on a too-short panel / empty visible
  //  region. Byte-identical to the pre-R2 path.
  //  Walk-forward (oos_n_windows >= 1): the holdout is the oos_window-th of oos_n_windows
  //  disjoint fixed-length windows of width w = floor(oos_fraction * T) tiling the terminal
  //  region [T - oos_n_windows*w, T). holdout_begin = T - (oos_n_windows - oos_window) * w.
  //  Window (oos_n_windows-1) is the terminal window [T - w, T) — byte-identical to the
  //  default path for that run.
  const atx::usize T = panel_.dates();
  const atx::usize embargo_len =
      (cfg.oos_embargo > 0.0)
          ? eval::detail::embargo_len_from_cpcv(cfg.oos_embargo, T)
          : eval::detail::embargo_len_from_cpcv(eval::CpcvConfig{}.embargo, T);

  // sealed_opt owns the visible (train) panel; holdout_opt is a separate slice.
  // Both are std::optional because alpha::Panel has no public default constructor.
  std::optional<eval::SealedPanel> sealed_opt;
  std::optional<alpha::Panel> holdout_opt;

  if (cfg.oos_n_windows == 0U) {
    // --- Legacy terminal path (byte-identical default) ---
    auto sealed_r = eval::reserve_lockbox(panel_, cfg.oos_fraction, embargo_len);
    if (!sealed_r.has_value()) {
      // A too-short panel: surface a clear message (the controller may widen the panel
      // or shrink the fraction). No library mutation has occurred.
      return atx::core::Ok(std::move(rep)); // admitted == 0, oos_metrics empty; caller reads n_alphas unchanged
    }
    const atx::usize lockbox_begin_leg = sealed_r->reservation().lockbox_begin;
    auto holdout_r = eval::detail::slice_panel(panel_, lockbox_begin_leg, T);
    if (!holdout_r.has_value()) {
      return atx::core::Ok(std::move(rep)); // holdout empty / unbuildable — nothing admitted
    }
    holdout_opt.emplace(std::move(*holdout_r));
    sealed_opt.emplace(std::move(*sealed_r));
  } else {
    // --- Walk-forward windowed path (R2) ---
    const atx::usize w = static_cast<atx::usize>(static_cast<atx::f64>(T) * cfg.oos_fraction);
    if (w == 0U) {
      return atx::core::Ok(std::move(rep)); // oos_fraction too small to carve a window — admitted 0
    }
    if (cfg.oos_window >= cfg.oos_n_windows) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "mine_into_oos: oos_window must be < oos_n_windows");
    }
    // Validate that T is large enough for the EARLIEST window to have a non-empty train region.
    // Condition: T >= oos_n_windows * w + embargo_len + 1.
    // Guard against unsigned overflow by using division form: check T > embargo_len + 1 first
    // (so T - embargo_len - 1U cannot underflow), then oos_n_windows > (T-embargo_len-1)/w.
    // This avoids forming the product oos_n_windows * w which can wrap for pathological inputs.
    if (embargo_len + 1U >= T || cfg.oos_n_windows > (T - embargo_len - 1U) / w) {
      return atx::core::Ok(std::move(rep)); // panel too short for the earliest window — admitted 0
    }
    const atx::usize holdout_begin = T - (cfg.oos_n_windows - cfg.oos_window) * w;
    auto sealed_r = eval::reserve_window(panel_, holdout_begin, w, embargo_len);
    if (!sealed_r.has_value()) {
      return atx::core::Ok(std::move(rep)); // reserve_window error — mirror reserve_lockbox Err handling
    }
    auto holdout_r = eval::detail::slice_panel(panel_, holdout_begin, holdout_begin + w);
    if (!holdout_r.has_value()) {
      return atx::core::Ok(std::move(rep)); // holdout slice empty / unbuildable — nothing admitted
    }
    holdout_opt.emplace(std::move(*holdout_r));
    sealed_opt.emplace(std::move(*sealed_r));
  }

  const alpha::Panel &train = sealed_opt->visible(); // [0, holdout_begin - embargo_len)
  const alpha::Panel &holdout = *holdout_opt;

  // (1) run the S3-5 search over the TRAIN panel (NOT panel_). A fresh seeded driver
  // re-derives clean per-run state, preserving F1 replay. Selection scores against an
  // EMPTY scratch store (as the legacy path does); admission scores against the
  // persistent library below.
  combine::AlphaStore search_pool;
  // W4a: thread the weak panel into the OOS-path search too. NOTE: when both the OOS
  // holdout (search runs on `train`) and the robust holdout are active, the weak
  // panel's universe mask should be derived over the SAME panel the search optimizes
  // (the caller's responsibility); nullptr (default) is the byte-identical no-op.
  SearchDriver driver{lib_,           train,            policy_,         sim_,
                      cfg.seed_exprs, cfg.panel_fields, cfg.weak_panel,  // W4a robust factor
                      cfg.numeric_excluded_fields, cfg.extra_group_fields}; // R1 typed-fields
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
  fill_scored_hashes(rep, res); // C2.2 report-only: distinct scored canon_hashes (not in digest)
  rep.trials = res.trial_count;
  rep.dedup_pct = res.dedup_pct;
  rep.seed = res.seed;
  rep.cse_pct = mean_cse_pct(res);
  rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

  // F4 / R1 — prior cumulative N + this run's N (same reasoning as serial mine_into).
  FitnessCfg admit_fit = cfg.search.fitness;
  const atx::u64 prior_r1_oos = lib_lib.cumulative_trials(); // R1: cross-run cumulative N
  if (res.trial_count > 0U) {
    admit_fit.trial_count = static_cast<atx::usize>(prior_r1_oos) + res.trial_count;
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

  // W4b — accumulate each admitted alpha's realized HOLDOUT PnL (the SAME stream gated +
  // persisted, deterministic admit order) for the POST-HOC run-level CSCV-PBO verdict;
  // finalized once after the loop. Empty + unused at the max_pbo == 1.0 default.
  std::vector<std::vector<atx::f64>> admitted_pnls;

  // (3) the mine -> CONFIRM-ON-HOLDOUT -> admit loop, best-train-deflated first.
  // A3: the admitted holdout PnL streams are collected ONCE into `admitted_pnls`
  // (above) and feed the SINGLE post-loop finalize_run_pbo computation (oos_pbo now
  // aliases rep.pbo). The separate R3b admitted_hold_pnls vector was removed — it
  // carried identical bytes and fed a duplicate CSCV pass.
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

    // W4a split-sample stability floor on the HOLDOUT stream (OPTIONAL; default
    // DISABLED -> byte-identical to the pre-W4a OOS screen).
    const bool split_ok =
        split_floor_ok(cfg.min_split_sharpe, std::span<const atx::f64>{hold_pnl}, hold_metrics);
    // R2 price-scale admission gate (OPTIONAL; default OFF = 1.0 -> zero overhead).
    // Rejects a candidate whose holdout book is a trivial 1/price (price-scale) tilt.
    // INACTIVE (cfg.max_price_scale_corr >= 1.0): skips entirely, byte-identical to pre-R2.
    // INERT (raw_close absent from holdout OR no usable dates -> NaN loading): no rejection.
    // Applied BEFORE the dsr/split check so the histogram bucket is bumped correctly.
    bool price_scale_ok = true;
    if (cfg.max_price_scale_corr < 1.0) {
      const auto rc_fid_r = holdout.field_id("raw_close");
      if (rc_fid_r.has_value()) {
        const atx::usize ps_n_inst = holdout.instruments();
        const atx::usize T_hold = (ps_n_inst > 0U) ? (hold_pos_flat.size() / ps_n_inst) : 0U;
        const atx::f64 loading = price_scale_loading(
            hold_pos_flat.data(), T_hold, ps_n_inst, holdout, *rc_fid_r);
        if (std::isfinite(loading) && loading >= cfg.max_price_scale_corr) {
          price_scale_ok = false;
        }
      }
      // raw_close absent -> loading = NaN -> price_scale_ok stays true (gate inert)
    }
    // (3d) ADMISSION on the HOLDOUT: clear the factory deflation bar on the holdout
    // DSR, then library::admit on the HOLDOUT metrics + holdout pnl (so the durable
    // `metrics` are what was actually gated out-of-sample).
    library::AdmitKind kind = library::AdmitKind::RejectFitness; // non-accept sentinel
    if (!price_scale_ok) {
      kind = library::AdmitKind::RejectPriceScale;
    } else if (hold_dsr >= cfg.min_dsr && split_ok) {
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
        admitted_pnls.push_back(hold_pnl);      // realized HOLDOUT PnL of an admitted alpha
      } else if (kind == library::AdmitKind::Duplicate) {
        ++rep.duplicates;
      }
    }
    ++rep.reject_histogram[static_cast<atx::usize>(kind)];

    rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
        static_cast<std::size_t>(rep.digest), canon_hash, static_cast<atx::u64>(kind)));
  }

  // A3 — the SINGLE POST-HOC run-level CSCV-PBO over the admitted HOLDOUT set. PURE /
  // post-admission: runs AFTER all admission decisions; never alters rep.digest, the
  // admitted set, or the library version_id. always_compute=true so the always-on
  // holdout diagnostic (oos_pbo) is recorded even at the 1.0 default; the gate verdict
  // still fail-opens when the gate is off. oos_pbo ALIASES this single computation
  // (NaN when < 2 admitted or the holdout is too short for any split).
  detail::finalize_run_pbo(rep, admitted_pnls, cfg.max_pbo, /*always_compute=*/true);
  rep.oos_pbo = rep.pbo; // A3: oos_pbo aliases the single holdout PBO

  rep.library_n_alphas_after = lib_lib.n_alphas();
  // R1: increment once per mine run, AFTER the loop.
  if (res.trial_count > 0U) {
    lib_lib.add_trials(static_cast<atx::u64>(res.trial_count));
  }

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
  // R2: the same windowed-carve branch as serial mine_into_oos — same holdout_begin formula,
  // same reserve_window / reserve_lockbox call, same empty-report returns. seq==parallel is
  // preserved by construction: both sides run this exact code.
  const atx::usize T = panel_.dates();
  const atx::usize embargo_len =
      (cfg.oos_embargo > 0.0)
          ? eval::detail::embargo_len_from_cpcv(cfg.oos_embargo, T)
          : eval::detail::embargo_len_from_cpcv(eval::CpcvConfig{}.embargo, T);

  // std::optional used because alpha::Panel has no public default constructor.
  std::optional<eval::SealedPanel> sealed_opt_par;
  std::optional<alpha::Panel> holdout_opt_par;

  if (cfg.oos_n_windows == 0U) {
    // --- Legacy terminal path (byte-identical default) --- same as serial mine_into_oos
    auto sealed_r = eval::reserve_lockbox(panel_, cfg.oos_fraction, embargo_len);
    if (!sealed_r.has_value()) {
      return atx::core::Ok(std::move(rep)); // too-short panel — same empty report the serial path returns
    }
    const atx::usize lockbox_begin_leg = sealed_r->reservation().lockbox_begin;
    auto holdout_r = eval::detail::slice_panel(panel_, lockbox_begin_leg, T);
    if (!holdout_r.has_value()) {
      return atx::core::Ok(std::move(rep)); // holdout empty / unbuildable — nothing admitted (same as serial)
    }
    holdout_opt_par.emplace(std::move(*holdout_r));
    sealed_opt_par.emplace(std::move(*sealed_r));
  } else {
    // --- Walk-forward windowed path (R2) — IDENTICAL to serial mine_into_oos ---
    const atx::usize w = static_cast<atx::usize>(static_cast<atx::f64>(T) * cfg.oos_fraction);
    if (w == 0U) {
      return atx::core::Ok(std::move(rep));
    }
    if (cfg.oos_window >= cfg.oos_n_windows) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "mine_into_oos_parallel: oos_window must be < oos_n_windows");
    }
    // Guard against unsigned overflow: use division form, same as serial mine_into_oos.
    if (embargo_len + 1U >= T || cfg.oos_n_windows > (T - embargo_len - 1U) / w) {
      return atx::core::Ok(std::move(rep));
    }
    const atx::usize holdout_begin = T - (cfg.oos_n_windows - cfg.oos_window) * w;
    auto sealed_r = eval::reserve_window(panel_, holdout_begin, w, embargo_len);
    if (!sealed_r.has_value()) {
      return atx::core::Ok(std::move(rep));
    }
    auto holdout_r = eval::detail::slice_panel(panel_, holdout_begin, holdout_begin + w);
    if (!holdout_r.has_value()) {
      return atx::core::Ok(std::move(rep));
    }
    holdout_opt_par.emplace(std::move(*holdout_r));
    sealed_opt_par.emplace(std::move(*sealed_r));
  }

  const alpha::Panel &train = sealed_opt_par->visible(); // [0, holdout_begin - embargo_len)
  const alpha::Panel &holdout = *holdout_opt_par;

  // (1) run the S3-5 search over the TRAIN panel in the PARENT — IDENTICAL to serial
  // mine_into_oos (a fresh seeded driver over `train`, F1 replay). The search's own
  // internal parallelism is separate and unchanged; this task parallelizes ADMISSION
  // evals, not the search. (sink/resume are not threaded into the substrate-aware
  // mine_into entry point — same as the non-OOS parallel path, which also runs the
  // search with the no-progress-sink driver.run(cfg, store) overload.)
  combine::AlphaStore search_pool;
  // W4a: thread the weak panel into the OOS-path search too. NOTE: when both the OOS
  // holdout (search runs on `train`) and the robust holdout are active, the weak
  // panel's universe mask should be derived over the SAME panel the search optimizes
  // (the caller's responsibility); nullptr (default) is the byte-identical no-op.
  SearchDriver driver{lib_,           train,            policy_,         sim_,
                      cfg.seed_exprs, cfg.panel_fields, cfg.weak_panel,  // W4a robust factor
                      cfg.numeric_excluded_fields, cfg.extra_group_fields}; // R1 typed-fields
  const SearchResult res = driver.run(cfg.search, search_pool);

  rep.evaluated = res.trial_count;
  fill_scored_hashes(rep, res); // C2.2 report-only: distinct scored canon_hashes (not in digest)
  rep.trials = res.trial_count;
  rep.dedup_pct = res.dedup_pct;
  rep.seed = res.seed;
  rep.cse_pct = mean_cse_pct(res);
  rep.digest = res.digest; // seed the admission digest with the search fingerprint (F1/F2)

  // F4 / R1 — prior + this run's N, IDENTICAL to serial mine_into_oos.
  FitnessCfg admit_fit = cfg.search.fitness;
  const atx::u64 prior_r1_par_oos = lib_lib.cumulative_trials(); // R1: cross-run cumulative N
  if (res.trial_count > 0U) {
    admit_fit.trial_count = static_cast<atx::usize>(prior_r1_par_oos) + res.trial_count;
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

  // W4b — accumulate each admitted alpha's realized HOLDOUT PnL at the SEQUENTIAL parent
  // admit-Ok point (NOT in workers) for the POST-HOC run-level CSCV-PBO verdict; finalized
  // once after the loop. Empty + unused at the 1.0 default. Deterministic across substrate
  // + worker count (the admit fold runs sequentially in THIS parent).
  std::vector<std::vector<atx::f64>> admitted_pnls;

  // (3) the mine -> CONFIRM-ON-HOLDOUT -> admit loop, best-train-deflated first. SEQUENTIAL
  // in the parent — byte-identical to serial mine_into_oos:757-835 (same F5 drops, same
  // hold_dsr, same gate, same admit, same digest fold, same oos_metrics push).
  // A3: the admitted holdout PnL streams are collected ONCE into `admitted_pnls` (above)
  // in the SAME deterministic admit order as serial mine_into_oos, feeding the SINGLE
  // post-loop finalize_run_pbo computation — this PRESERVES the seq==parallel oos_pbo
  // match the old R3b block guaranteed (same admit-order vectors, same n_splits rule).
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

    // W4a split-sample stability floor on the HOLDOUT stream — IDENTICAL rule to the
    // serial mine_into_oos path (same hold_pnl + hold_metrics -> same decision), so
    // the serial/parallel byte-identity invariant holds. Default DISABLED (-inf).
    const bool split_ok =
        split_floor_ok(cfg.min_split_sharpe, std::span<const atx::f64>{hold_pnl}, hold_metrics);
    // R2 price-scale admission gate — IDENTICAL to serial mine_into_oos so the
    // serial/parallel byte-identity invariant holds. INACTIVE (>= 1.0): skips entirely.
    // INERT (raw_close absent or no usable dates -> NaN loading): no rejection.
    bool price_scale_ok = true;
    if (cfg.max_price_scale_corr < 1.0) {
      const auto rc_fid_r = holdout.field_id("raw_close");
      if (rc_fid_r.has_value()) {
        const atx::usize T_hold = (n_inst > 0U) ? (hold_pos_flat.size() / n_inst) : 0U;
        const atx::f64 loading = price_scale_loading(
            hold_pos_flat.data(), T_hold, n_inst, holdout, *rc_fid_r);
        if (std::isfinite(loading) && loading >= cfg.max_price_scale_corr) {
          price_scale_ok = false;
        }
      }
    }
    // (3d) ADMISSION on the HOLDOUT — IDENTICAL to serial mine_into_oos:813-830.
    library::AdmitKind kind = library::AdmitKind::RejectFitness; // non-accept sentinel
    if (!price_scale_ok) {
      kind = library::AdmitKind::RejectPriceScale;
    } else if (hold_dsr >= cfg.min_dsr && split_ok) {
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
        admitted_pnls.push_back(hold_pnl);          // realized HOLDOUT PnL of an admitted alpha
      } else if (kind == library::AdmitKind::Duplicate) {
        ++rep.duplicates;
      }
    }
    ++rep.reject_histogram[static_cast<atx::usize>(kind)];

    rep.digest = static_cast<atx::u64>(atx::core::hash_combine(
        static_cast<std::size_t>(rep.digest), canon_hash, static_cast<atx::u64>(kind)));
  }

  // A3 — the SINGLE POST-HOC run-level CSCV-PBO over the admitted HOLDOUT set, accumulated
  // at the SEQUENTIAL parent admit-Ok point in the SAME deterministic admit order as serial
  // mine_into_oos, so oos_pbo is bit-identical across substrate + worker count AND equal to
  // the serial path. always_compute=true records the always-on holdout diagnostic even at
  // the 1.0 default; the gate verdict fail-opens when off. Never alters rep.digest.
  detail::finalize_run_pbo(rep, admitted_pnls, cfg.max_pbo, /*always_compute=*/true);
  rep.oos_pbo = rep.pbo; // A3: oos_pbo aliases the single holdout PBO

  rep.library_n_alphas_after = lib_lib.n_alphas();
  // R1: increment once per mine run, AFTER the loop — IDENTICAL to serial mine_into_oos.
  if (res.trial_count > 0U) {
    lib_lib.add_trials(static_cast<atx::u64>(res.trial_count));
  }

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
