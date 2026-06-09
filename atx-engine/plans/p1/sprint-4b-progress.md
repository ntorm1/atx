# Sprint S4b — The Automated Alpha Engine (progress ledger)

**Status:** ✅ CLOSED 2026-06-09 (`feat/atx-core-stdlib @ 54a53f4` — all code/tests; close paperwork this commit)
**Branch:** `feat/atx-core-stdlib` (shared — explicit pathspecs only, `git add -- <path>`; never pushed)
**Base SHA:** `518af61`
**Spec:** [`sprint-4b-automated-alpha-engine.md`](sprint-4b-automated-alpha-engine.md)
**Plan:** [`sprint-4b-automated-alpha-engine-implementation-plan.md`](sprint-4b-automated-alpha-engine-implementation-plan.md)
**ROADMAP:** [`ROADMAP.md`](ROADMAP.md) · **User ref:** [`sprint-4b.md`](sprint-4b.md)

---

## Per-unit status

| Unit  | Title                                                        | Status | Commit(s) |
|-------|-------------------------------------------------------------|--------|-----------|
| S4b-0 | Marker + ledger + as-built seam                             | ✅     | `aff5035` |
| S4b-1 | unparse(Ast)→string + round-trip soundness                  | ✅     | `ac12980`, `1dbce3d` |
| S4b-2 | PoolView seam + library-backed pool-aware fitness           | ✅     | `e6bf0f2`, `87ec686` |
| S4b-3 | Factory::mine_into — factory→library admit bridge           | ✅     | `64a1a3f` |
| S4b-4 | ResearchDriver — continuous mine→admit→repeat loop          | ✅     | `4d78651`, `cce1f56` |
| S4b-5 | E2E engine (F1/F4/F6) + bench + close                       | ✅     | `da96951`, `d0ebec5`, `54a53f4` |

Two-stage reviewed per unit (spec-compliance → code-quality → fix loop); every review caught ≥1 real defect. Plan commits: `42dd5c3` (implementation plan).

---

## As-built seam (verified)

The integration surface S4b units build on. Later sprints must NOT re-derive these signatures.

```
// LIBRARY (S4, persistent) — the admit target
library::AlphaCandidate{ u64 canon_hash; span<const f64> pnl; span<const f64> pos_flat;
                         combine::AlphaMetrics metrics; library::Provenance prov; usize as_of;
                         ISignalSource* source=nullptr; }            // library/library.hpp:90
library::AdmitVerdict library::Library::admit(const AlphaCandidate&, const combine::AlphaGate&);
enum class library::AdmitKind:u8 { Accept, Duplicate, RejectSharpe, RejectFitness,
                                   RejectTurnover, RejectCorrelated };
f64 library::online_corr_to_pool(span<const f64> cand_pnl, const LibraryStore&, CorrNeighborIndex&);
f64 library::Library::worst_corr_to_pool(span<const f64> pnl) const;   // S4b-2 additive accessor
library::Provenance{ string expr_source; vector<u64> parent_hashes; u16 mutation_op; u64 seed; }  // record.hpp:138
library::Library::{ open(dir,GateConfig,vector<u64> seeds), n_alphas(), pnl(id), snapshot()->LibraryManifest }
  LibraryManifest.version_id (u32 content-address); rebuild_equals(manifest,dir,cfg,seeds)->bool

// FACTORY (S3 + S4b) — produces per candidate: canon_hash (computed via canonical_hash; Genome.canon_hash is 0),
//   pnl+pos_flat (detail_eval_streams -> owned vectors), metrics (combine::compute_metrics),
//   dsr (pool_aware_fitness -> FitnessReport.dsr).
factory::PoolView{ virtual f64 worst_corr(span<const f64> pnl) const };  // pool_view.hpp
  factory::AlphaStorePool (O(N) exact) | factory::LibraryPool (O(neighbors) SimHash)
factory::pool_aware_fitness(const Genome&, const PoolView&, panel, policy, sim, FitnessCfg) -> Result<FitnessReport>
factory::Factory::mine_into(const FactoryConfig&, library::Library&, const combine::AlphaGate&) -> FactoryReport
  // gate: dsr >= cfg.min_dsr (deflation N = running trial count, factory-side) AND library.admit()==Accept
FactoryReport += { usize duplicates; u64 library_n_alphas_before/after; array<usize,6> reject_histogram }
factory::ResearchDriver(lib, dsl, panel, sim, policy, gate).run(ResearchConfig) -> ResearchReport
  ResearchConfig{ FactoryConfig per_run; usize max_runs, patience; u64 master_seed }
  ResearchReport{ runs, total_mined/admitted/duplicates, library_size, lifecycle_histogram[6],
                  dedup_pct, digest, manifest_version_id, seed }
  detail::seed_for_run(master, run) -> u64  // pure SplitMix; composes with seed_for(gen,idx) => (master,run,gen,idx)
alpha::unparse(const Ast&) / unparse(const Ast&, ExprId) -> std::string  // S4b-1; round-trips through canonical_hash
```

---

## What S4b proves

S4b fused the two finished halves (S3 factory + S4 library) into **one automated, persistent, deflation-gated
discovery engine with end-to-end seeded replay** — it operates like a WorldQuant/RenTech research loop instead
of two subsystems passing in the night. The five load-bearing contracts are each proven non-vacuous (fail-on-bad
AND pass-on-good) in the `ResearchEngine` capstone suite:

- **F1 — whole-engine seeded replay** (`SeededEngineReplaysByteIdentical`): two seeded `ResearchDriver` runs into
  fresh temp-dir libraries produce equal `digest` AND equal manifest `version_id`. Every RNG seed is
  `(master_seed, run, gen, idx)` — nothing worker/thread/time enters the digest or admit ordering.
- **F4 — anti-snooping at scale** (`NoiseGrowsLibraryByNothing`): pure-noise panels over a large budget admit
  **0** into the persistent library across **4 independent seeds** (deflation N = the running trial count); a
  real-signal panel admits survivors under the **same** gate + `min_dsr` bar. Same bar, opposite outcomes.
- **F6 — library-wide cross-run dedup ACROSS the persistence boundary** (`CrossRunDedupNeverReadmits`): a second
  pass that **reopens the library from disk** and re-mines the same population deduplicates the rediscoveries off
  the **reloaded** sqlite index — never re-admits, even across close/reopen.
- **Scale lever** (`research_bench`): the diversification/redundancy corr-to-pool runs at **O(neighbors)** via the
  SimHash index, benched against the O(N) ephemeral scan — at a 4096-alpha pool, `LibraryPool` ≈ **4361 µs** vs
  `AlphaStorePool` ≈ **14618 µs** (~**3.35×**, Debug; the gap widens with pool size — O(N) scales ~linearly while
  O(neighbors) stays far flatter). Measured times only — no ideal-speedup claim.
- **Unparse soundness** (`UnparseRoundTripsThroughCanonicalHash`): every admitted alpha's stored
  `Provenance.expr_source` re-parses to the same (non-zero) `canonical_hash` — the formula round-trips losslessly,
  making every alpha inspectable and re-parseable (the enabler for any future re-eval).

Engine throughput bench (`BM_ResearchEngine`, Debug): ~**85 alphas mined/sec**, admitting into the durable library.
Full engine suite **983/983** green; strict `/W4 /permissive- /WX` + strict-FP build clean. **atx-core byte-untouched**
(`git diff 518af61..54a53f4 -- atx-core/` empty) — no new general-purpose primitive.

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `42dd5c3` | plan | implementation plan — per-unit TDD |
| `aff5035` | S4b-0 | open ledger — automated alpha engine |
| `ac12980` | S4b-1 | unparse(Ast)→string with round-trip-through-canonical-hash soundness |
| `1dbce3d` | S4b-1 | check to_chars ec; guard unary/hparam/select arms with round-trip tests |
| `e6bf0f2` | S4b-2 | PoolView seam — library-backed pool-aware fitness at O(neighbors) |
| `87ec686` | S4b-2 | drop unused <algorithm>, add direct includes (utility/bytecode/vm) |
| `64a1a3f` | S4b-3 | Factory::mine_into — factory→library admit bridge (deflation factory-side) |
| `4d78651` | S4b-4 | ResearchDriver — continuous mine→admit→repeat over a fixed panel |
| `cce1f56` | S4b-4 | drop unused lifecycle include; de-vacuify histogram assertion |
| `da96951` | S4b-5 | E2E research engine — F1/F4/F6 + unparse soundness + scale-lever bench |
| `d0ebec5` | S4b-5 | F6 dedup across reopen boundary; multi-seed noise; IWYU includes |
| `54a53f4` | S4b-5 | bounds-guard library iteration; IWYU <system_error> in tests |

---

## Residuals / Baton

**Residuals (lifted to the p1 Pattern-B / follow-up backlog):**
1. **Ranking-tiebreak asymmetry.** `mine_into`'s `PoolView` `rank_by_deflated_fitness` overload adds an `idx`
   final tiebreak (since `Genome.canon_hash` is 0 on the search path, the canon tiebreak is degenerate); the
   legacy `AlphaStore` overload used by `mine()` does NOT. `mine()`'s determinism holds on a fixed toolchain
   (green S3 suite) but relies on `std::sort` being deterministic for identical input. Ticket: share ONE
   total-order key across both overloads (requires re-baselining the recorded S3 digest — deliberately deferred).
2. **Provenance lineage stub.** Admitted alphas record `parent_hashes={}`, `mutation_op=0` (S3's `all_scored`
   Genomes don't thread lineage); `expr_source` (round-trippable) + `seed` ARE recorded. F6 depends on
   `canon_hash`, not lineage. Threading parent hashes through `SearchResult.all_scored` is a future unit.
3. **In-search selection vs the persistent library.** `SearchDriver` has no `PoolView` overload, so the
   evolutionary selection/novelty pressure during a run scores against an empty scratch `AlphaStore` (identical
   to `mine()`'s run-start behavior); the persistent `LibraryPool` drives the **admission** ranking + per-candidate
   re-score (where "marginal contribution to the persistent library" is required, and satisfied). Wiring the
   library into in-loop selection needs a `SearchDriver` `PoolView` overload — out of S4b's 2-file-per-unit scope.
4. **F4 bar calibration.** The capstone noise-vs-signal split uses `min_dsr = 0.80` (vs the unit suites' 0.50);
   at 0.50 the luckiest in-sample noise structure cleared on some seeds. 0.80 gives 0 noise admits across 4 seeds
   while real signal still admits 2–3. Same bar for both panels — the anti-snooping proof is not rigged. A
   first-class noise-floor calibration is an S6/S7 concern.

**Baton → S5 / S7:**
- **S5 (combiner):** the persistent library is now *populated by the engine* (not just hand-fed) — the ML/regime
  mega-combiner has a real, deduplicated, corr-indexed pool to combine.
- **S7 (operate):** the **re-eval adapter** (re-parse `expr_source` → compile → eval over a NEW `alpha::Panel`)
  for decay monitoring / live-vs-backtest drift is the next enabler. S4b admits store `source = nullptr` + the
  round-trippable `expr_source` (the enabler is in place); the live re-eval handle is the S7 unit.
