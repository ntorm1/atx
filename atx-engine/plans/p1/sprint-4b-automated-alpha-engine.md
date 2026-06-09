# Sprint S4b — The Automated Alpha Engine (sprint spec)

**Status:** ✅ CLOSED 2026-06-09 (`feat/atx-core-stdlib @ 54a53f4`; ledger [`sprint-4b-progress.md`](sprint-4b-progress.md) · user ref [`sprint-4b.md`](sprint-4b.md)). Depended on **S3** (the factory ✅ `5f57a34`), **S4** (the library ✅ merged), **S1** (deflation), **S2** (parallel), **P4** (gate/metrics).
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §2/§5/§9/§10 (the alpha factory + the persistent corr-gated library + online marginal-correlation gating at scale) + [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md) §1/§7 (continuous statistical discovery + ruthless OOS discipline).

---

## Why this sprint

S3 built the **factory** (it discovers alphas) and S4 built the **library** (it persistently stores, deduplicates, corr-indexes, and lifecycles them) — **but the two are disconnected**. Today:

- `factory::Factory::mine` admits survivors into an **ephemeral, in-memory `combine::AlphaStore`** (`pool.insert(nullptr, …)`), with an **O(N)** corr-to-pool scan, and the pool **evaporates** when the run ends. There are **zero** references from `factory/` to `library/`.
- `library::Library::admit` is the **persistent** admit path — library-wide dedup (S4-2) → **O(neighbors)** `online_corr_to_pool` (S4-3) → the P4 gate floors → segmented store (S4-1) + PIT lifecycle (S4-4) + a content-addressed manifest (S4-5) — but **nothing mines into it**.

S4b is the **integration milestone** that fuses the two finished halves into **one automated, persistent, deflation-gated discovery engine with end-to-end seeded replay** — the thing that actually *operates* like a WorldQuant/RenTech research loop instead of two subsystems that pass each other in the night. After S4b, a seeded engine run mines a population, scores each candidate's **marginal contribution to the *persistent* library** (at O(neighbors) scale), deflates it against the running trial count, admits the survivors into the durable, deduplicated, lifecycle-managed library, and **replays byte-identically — manifest `version_id` included**.

---

## The complete research system (where S4b sits)

```
        data ─► MINE ─────────► STORE / GATE ─────────► COMBINE ─► OPTIMIZE ─► COST ─► REPORT
               (S3 ✅)          (S4 ✅)                  (S5 / P4)   (S7)        (S6)    (S7)
                  └──────────────────┬──────────────────┘
                              ►►►  SPRINT-4b  ◄◄◄
                    fuse S3 + S4 into ONE automated, persistent,
                    deflation-gated, replayable discovery engine
```

The full p1 arc is the WorldQuant "alpha factory" (mass formulaic discovery) fused with the RenTech "one unified, ruthlessly-OOS-validated model." S4b closes the **discover→store** seam; the rest of the arc — already specced in the ROADMAP — remains:

| Stage | Sprint | What it adds beyond S4b |
|---|---|---|
| **Combine** | S5 / P4 | learned signals (ridge/GBM/HMM) + an ML/regime mega-combiner over the library pool |
| **Cost** | S6 | calibrated √-impact δ/Y/γ/η + capacity curves + cost-aware fitness |
| **Optimize / Operate** | S7 | multi-period optimizer + alpha-decay monitor + dead-alpha→risk-factor + full E2E report |

S4b is deliberately scoped to the **integration engine** (discover→store, automated + replayable). It does **not** build the combiner, cost calibration, or portfolio layer — those are the documented remaining sprints.

---

## Architecture

Mirror the S3 split (`SearchDriver` = per-generation, `Factory` = mine→admit) by adding one more orchestration layer:

```
ResearchDriver  (S4b-4)  — across-run continuous loop, owns the library + telemetry
   └─ Factory::mine_into(library)  (S4b-3)  — per-run mine → deflate → library::admit
        ├─ SearchDriver (S3-5)              — per-generation seeded evolutionary search
        ├─ pool_aware_fitness[library]  (S4b-2)  — diversification via O(neighbors) online corr
        └─ library::Library::admit (S4-5)  — dedup → corr → P4 floors → persist → lifecycle
```

Three load-bearing design decisions:

1. **Library = single source of truth.** The factory's diversification term scores marginal corr against the **persistent library's `online_corr_to_pool`** (SimHash-LSH, O(neighbors)) — the scale lever that kills the O(N²) re-gate. A thin **`PoolView`** seam lets `pool_aware_fitness` run against *either* the ephemeral `combine::AlphaStore` (small fixtures / the retained S3 tests) *or* the persistent library (scale / the real engine), with no duplicated corr math.

2. **Deflation stays factory-side.** `library::Library::admit` does dedup + corr + the four P4 gate floors + persistence + lifecycle — but it has **no S1 deflation bar**. The factory keeps the `dsr ≥ min_dsr` bar (F4) wrapped *around* the library admit: the engine admits a candidate iff **`dsr ≥ min_dsr` AND `library.admit(...) == Accept`**. The deflation N is the **running trial count** (the S3-6 `5f57a34` auto-scaling behavior, carried forward).

3. **Provenance recorded — the formula is the artifact.** A discovered alpha is worthless if you cannot read its formula. Each admitted alpha stores `Provenance{expr_source, parent_hashes, mutation_op, seed}`. `expr_source` requires an **`unparse(alpha::Ast)→string`** that does not yet exist (the S3 genome is an Ast, and there is no Ast→string in the codebase) — S4b-1 builds it, with a **round-trip soundness** contract.

### The seam — as-built API (verified)

```cpp
// LIBRARY (S4, persistent) — the admit target
struct library::AlphaCandidate {                            // library/library.hpp:90
  atx::u64 canon_hash; std::span<const atx::f64> pnl; std::span<const atx::f64> pos_flat;
  combine::AlphaMetrics metrics; library::Provenance prov; atx::usize as_of;
  atx::engine::ISignalSource *source = nullptr;             // nullptr OK (no re-eval in 4b)
};
library::AdmitVerdict library::Library::admit(const AlphaCandidate&, const combine::AlphaGate&);
atx::f64 library::online_corr_to_pool(std::span<const atx::f64> cand_pnl,
                                      const library::LibraryStore&, library::CorrNeighborIndex&);
struct library::Provenance { std::string expr_source; std::vector<atx::u64> parent_hashes;
                             atx::u16 mutation_op; atx::u64 seed; };  // library/record.hpp:138

// FACTORY (S3) — already produces everything AlphaCandidate needs, per candidate:
//   canon_hash (genome.canon_hash), pnl+pos_flat (extract_streams → owned vectors),
//   metrics (combine::compute_metrics), dsr (pool_aware_fitness → FitnessReport.dsr).
// SHARED already: combine::AlphaId, combine::AlphaMetrics, compute_metrics, GateConfig/AlphaGate.
// GAP: pool_aware_fitness takes const combine::AlphaStore& (O(N)); needs a library-backed path.
```

---

## Scope — units

> Sequential dispatch, subagent-driven (fresh implementer → spec-compliance review → code-quality review → fix loop → ledger SHA), per `superpowers:subagent-driven-development`. **Shared branch `feat/atx-core-stdlib` → explicit-pathspec commits, no push.** Every commit ends with the `Co-Authored-By: Claude Opus 4.8` trailer; clang-tidy disabled (the `/W4 /permissive- /WX` + strict-FP build is the gate).

### S4b-0 — Marker + ledger + the complete-system architecture doc
Open `sprint-4b-progress.md` (the S3/S4 ledger shape: base SHA, shared-branch note, per-unit table S4b-0…S4b-5, commits table, residuals/baton placeholders). Record the as-built seam (the API block above) + the kickoff risks (the `PoolView` abstraction must not fork the corr math; the deflation bar must stay factory-side; whole-engine replay must include the persisted manifest `version_id`).

### S4b-1 — `unparse(alpha::Ast)→string` + round-trip soundness (the formula record)
`unparse(const alpha::Ast&, alpha::ExprId root) -> std::string` (and a whole-Ast overload) that renders a genome back to a DSL expression string, plus its population into `Provenance.expr_source`. **Load-bearing contract (unparse soundness):** `parse(unparse(ast))` succeeds and yields the **same `canonical_hash`** as the original — the formula round-trips losslessly through the canonical key (fail-on-bad: a deliberately-wrong unparse flips the hash). This makes every admitted alpha inspectable AND re-parseable (the enabler for any future re-eval without a stored live handle).

### S4b-2 — Library-backed pool-aware fitness (the scale lever)
Introduce a minimal **`PoolView`** seam — the single operation fitness needs from a pool is *"max |corr| of this candidate against the pool."* Provide two backings: the existing O(N) `combine::AlphaStore` scan (retained for the S3 fixtures) and the library's O(neighbors) `online_corr_to_pool`. Add a `pool_aware_fitness` overload (or `PoolView` parameter) so the diversification term `diversify = 1 − mean|corr-to-pool|` and the redundancy screen run against the **persistent library** at scale, with **no second corr implementation**. This requires `library::Library` to expose a small **read-only `worst_corr_to_pool(std::span<const f64> pnl) const`** accessor over its (currently private) `corr_` index + `store_` — a minimal additive change to the merged S4 facade (it already computes exactly this inside `verdict_for`), made under the same explicit-pathspec discipline. Contract: on a shared fixture the library-backed `diversify` equals the AlphaStore-backed `diversify` within the S4-3 recall caveat (documented), and the corr-query cost is benched O(neighbors) vs O(N).

### S4b-3 — Factory→library admit bridge
Add `Factory::mine_into(const FactoryConfig&, library::Library&, const combine::AlphaGate&) -> FactoryReport` (the **real** admit path; the existing ephemeral-`AlphaStore` `mine()` is **retained test-only** for the S3 suite). Per ranked candidate: realize streams (§0.6 owned vectors), `compute_metrics`, compute `dsr` against the library-backed fitness (S4b-2), build an `AlphaCandidate{canon_hash, pnl, pos_flat, metrics, Provenance{expr_source (S4b-1), parent_hashes, mutation_op, seed}, as_of, source=nullptr}`, gate on **`dsr ≥ cfg.min_dsr` AND `library.admit(...) == Accept`**. Extend `FactoryReport` with library-growth telemetry (`admitted`, `duplicates`, library `n_alphas` delta, per-`AdmitKind` reject histogram). The mine→library digest folds the search digest + each admission decision (F1/F2).

### S4b-4 — `ResearchDriver` — the continuous automated engine
`ResearchDriver` owns a `library::Library` + drives a budget-bounded **mine→admit→repeat** loop over a **fixed research panel**, growing the persistent, deduplicated library across runs/generations until a stop condition (budget exhausted OR K consecutive runs admit nothing new — novelty exhaustion). Every RNG seed is `(master_seed, run, gen, idx)` (extends S3's `(master_seed, gen, idx)` with the run axis); **checkpoint/resume** via the library manifest (a snapshot replays byte-identically on reopen). Emits a `ResearchReport{runs, total_mined, total_admitted, total_duplicates, library_size, lifecycle_histogram, dedup_pct, digest, manifest_version_id, seed}`.

### S4b-5 — E2E integration test + anti-snooping-at-scale proof + bench + close
The capstone suite `ResearchEngine`:
- **`SeededEngineReplaysByteIdentical` (F1):** two `ResearchDriver` runs (same seed, fresh temp-dir libraries) produce equal `digest` **AND** equal manifest `version_id` (reopen → identical snapshot).
- **`NoiseGrowsLibraryByNothing` (F4 at scale):** a pure-noise panel over a large budget admits ~0 into the *persistent* library (deflation N = the running trial count); a real-signal panel grows it — same gate + dsr bar.
- **`CrossRunDedupNeverReadmits` (F6):** an expression mined in run 2 that is structurally-equivalent to a run-1 admit is deduped by the S4-2 library-wide index (never re-admitted, even across the persistence boundary).
- **`UnparseRoundTripsThroughCanonicalHash` (S4b-1):** every admitted alpha's stored `expr_source` re-parses to the same `canonical_hash`.
- **Bench** (`research_bench.cpp`): alphas mined/sec, admitted/hour, dedup%, library growth curve, and the **online-corr-to-pool speedup vs the O(N) scan** (the scale-lever number). No ideal-speedup claim.
- **Close** (per `../docs/sprint.md`): fill the ledger; add an `S4b` row to the ROADMAP (between S4 and S5) + bump `Last reviewed`; mark this spec `✅ closed`; create `sprint-4b.md` user reference; lift residuals.

---

## Load-bearing contracts (proven by non-vacuous tests — fail-on-bad AND pass-on-good)

- **F1 — whole-engine seeded replay:** a seeded `ResearchDriver` run replays byte-identically, **including the persisted manifest `version_id`** (reopen the library → identical snapshot). Every RNG seed is `(master_seed, run, gen, idx)`; nothing worker/thread/time enters the digest or admit ordering.
- **F4 — anti-snooping at scale:** a pure-noise panel grows the *persistent* library by ~0 after deflation; a real-signal panel grows it — same gate + `min_dsr` bar, deflation N = the running trial count.
- **F6 — library-wide cross-run dedup:** structurally-equivalent expressions mined in *different runs* are deduped by `canonical_hash` (the S4-2 persistent index), never re-admitted across the persistence boundary.
- **Scale lever:** the diversification/redundancy corr-to-pool runs at **O(neighbors)** via the library's SimHash index, benched against the O(N) ephemeral scan.
- **Unparse soundness:** `parse(unparse(ast))` yields the same `canonical_hash` — the formula round-trips losslessly.

## Decisions locked (brainstorm)

1. **The ephemeral `combine::AlphaStore` admit path is retained test-only** — `Factory::mine_into(library)` is the real path; the old in-memory `mine()` stays so the green S3 `FactoryIntegration` suite is untouched. Two entry points, shared internals.
2. **The continuous loop is fixed-panel discovery** — it mines NEW candidates against a fixed research panel, growing the persistent library; re-evaluating library alphas on NEW data (decay/live monitoring) is explicitly **S7**.

## NOT in scope (→ the remaining arc)

- **Research-panel `ISignalSource` re-eval adapter** (re-parse `expr_source` → compile → eval over an `alpha::Panel`) — admitted alphas store `source = nullptr` + the round-trippable `expr_source` (the enabler), but the live re-eval handle is an **S5/S7** unit.
- **The combiner** (linear rungs over the library pool / ML / regime) → **S5**.
- **Cost-aware fitness + capacity** → **S6**.
- **Multi-period optimizer + alpha-decay monitor + dead-alpha recycling + the full E2E report** → **S7**.

## Dependencies

S3 (`factory::{Factory, SearchDriver, pool_aware_fitness, canonical_hash, Genome}`), S4 (`library::{Library, LibraryStore, CorrNeighborIndex, online_corr_to_pool, DedupIndex, LifecycleJournal, LibraryManifest, Provenance}`), S1 (`eval::deflated_sharpe`), P4 (`combine::{AlphaGate, GateConfig, AlphaMetrics, compute_metrics, AlphaId}`), the alpha DSL (`alpha::{Ast, parse_expr, compile, Engine, analyze}` + the new `unparse`). atx-core untouched (no new general-purpose primitive).
