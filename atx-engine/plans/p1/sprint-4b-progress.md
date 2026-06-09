# Sprint S4b — The Automated Alpha Engine (progress ledger)

**Status:** 🚧 OPEN
**Branch:** `feat/atx-core-stdlib` (shared — explicit pathspecs only, `git add -- <path>`; do NOT push)
**Base SHA:** `518af61`
**Spec:** [`sprint-4b-automated-alpha-engine.md`](sprint-4b-automated-alpha-engine.md)
**Plan:** [`sprint-4b-automated-alpha-engine-implementation-plan.md`](sprint-4b-automated-alpha-engine-implementation-plan.md)
**ROADMAP:** [`ROADMAP.md`](../ROADMAP.md)

---

## Per-unit status

| Unit  | Title                                                        | Status | Commit(s) |
|-------|-------------------------------------------------------------|--------|-----------|
| S4b-0 | Marker + ledger + as-built seam                             | ✅     | `4b6e2ed` |
| S4b-1 | unparse(Ast)→string + round-trip soundness                  | ⬜     | — |
| S4b-2 | PoolView seam + library-backed pool-aware fitness           | ⬜     | — |
| S4b-3 | Factory::mine_into — factory→library admit bridge           | ⬜     | — |
| S4b-4 | ResearchDriver — continuous mine→admit→repeat loop          | ⬜     | — |
| S4b-5 | E2E engine (F1/F4/F6) + bench + close                       | ⬜     | — |

---

## As-built seam (verified)

The following is the integration surface that S4b units build on. Later units must NOT re-derive these signatures.

```
// LIBRARY (S4, persistent) — the admit target
library::AlphaCandidate{ u64 canon_hash; span<const f64> pnl; span<const f64> pos_flat;
                         combine::AlphaMetrics metrics; library::Provenance prov; usize as_of;
                         ISignalSource* source=nullptr; }            // library/library.hpp:90
library::AdmitVerdict library::Library::admit(const AlphaCandidate&, const combine::AlphaGate&);
enum class library::AdmitKind:u8 { Accept, Duplicate, RejectSharpe, RejectFitness,
                                   RejectTurnover, RejectCorrelated };
f64 library::online_corr_to_pool(span<const f64> cand_pnl, const LibraryStore&, CorrNeighborIndex&);
library::Provenance{ string expr_source; vector<u64> parent_hashes; u16 mutation_op; u64 seed; }  // record.hpp:138
library::Library::{ open(dir,GateConfig,vector<u64> seeds), n_alphas(), pnl(id), snapshot()->LibraryManifest }
  LibraryManifest.version_id (u32 content-address); rebuild_equals(manifest,dir,cfg,seeds)->bool

// FACTORY (S3) — produces per candidate: canon_hash (Genome.canon_hash), pnl+pos_flat
//   (detail_eval_streams/extract_streams -> owned vectors), metrics (combine::compute_metrics),
//   dsr (pool_aware_fitness -> FitnessReport.dsr).
factory::pool_aware_fitness(const Genome&, const combine::AlphaStore&, const alpha::Panel&,
  const WeightPolicy&, const exec::ExecutionSimulator&, const FitnessCfg&,
  const alpha::Panel* weak=nullptr) -> Result<FitnessReport>            // factory/fitness.hpp:304
FitnessReport{ f64 wq, redundancy, diversify, robust, raw, dsr, haircut_sharpe }  // :146
FitnessCfg{ usize trial_count; eval::CpcvConfig cpcv; f64 book_size }              // :165
factory::Factory(const alpha::Library&, const alpha::Panel&, const exec::ExecutionSimulator&,
  const WeightPolicy&); Factory::mine(const FactoryConfig&, combine::AlphaStore&,
  const combine::AlphaGate&) -> FactoryReport                          // factory/factory.hpp:161,178
FactoryConfig{ SearchConfig search; vector<string> seed_exprs, panel_fields; f64 min_dsr, book_size }
FactoryReport{ usize admitted, evaluated, trials; f64 dedup_pct, cse_pct; u64 seed, digest }
rank_by_deflated_fitness(scored, const FitnessCfg&, pool) -> vector<Ranked>         // :329
Genome{ alpha::Ast ast; alpha::Analysis analysis; u64 canon_hash }                 // genome.hpp:51
SearchDriver(lib,panel,policy,sim,seed_exprs,panel_fields).run(SearchConfig) -> SearchResult
SearchConfig{ u64 master_seed; usize population,generations,elites,k_tournament; f64 p_cross,
  novelty_w; u16 max_lookback; usize n_workers; FitnessCfg fitness; bool enable_op_swap }
SearchResult{ u64 digest; usize trial_count, candidates_generated; f64 dedup_pct;
  vector<f64> best_fitness_per_gen; vector<Genome> all_scored, admitted_candidates; u64 seed }
detail::seed_for(master_seed, gen, idx) -> u64   // pure SplitMix; NO worker/thread/time (F1)
// GAP (built by S4b): no Ast->string (unparse), no PoolView, no Factory::mine_into, no ResearchDriver.
```

---

## Kickoff risks

1. **PoolView corr-math unity.** The `PoolView` seam introduced in S4b-2 must NOT fork the correlation math — one corr implementation per backing (factory-side `AlphaStore` and library-side `LibraryStore` + `CorrNeighborIndex`), both returning *max* |corr| so the two paths agree and the gate screen is consistent end-to-end.
2. **Deflation bar stays factory-side.** The S1 deflation guard `dsr ≥ min_dsr` remains a factory/search-driver concern; admission is double-gated: factory screens first, *then* `library::Library::admit(...)` screens via `AlphaGate`. Do not move the dsr bar into the library layer.
3. **Seeded replay must include manifest `version_id`.** Whole-engine seeded replay (F1) must fold `LibraryManifest.version_id` (the library's u32 content-address) into the replay digest; skipping it breaks the reproducibility contract when the persisted library has changed between runs.
4. **`unparse` must round-trip through `canonical_hash`.** S4b-1's `unparse(Ast)→string` is not done until `canonical_hash(parse(unparse(ast))) == canonical_hash(ast)` for the full expression grammar; passing a visual inspection is insufficient.

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|

---

## Residuals / Baton

TBD at close.
