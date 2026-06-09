# Sprint S4b — The Automated Alpha Engine (user reference)

**Status:** ✅ CLOSED 2026-06-09 (`feat/atx-core-stdlib @ 54a53f4`). Ledger: [`sprint-4b-progress.md`](sprint-4b-progress.md) · Spec: [`sprint-4b-automated-alpha-engine.md`](sprint-4b-automated-alpha-engine.md) · ROADMAP: [`ROADMAP.md`](ROADMAP.md).

S4b fuses the S3 **factory** (discovers alphas) and the S4 **library** (persistently stores / dedups / corr-indexes
/ lifecycles them) into **one automated, persistent, deflation-gated discovery engine** that replays byte-identically.
The library is the single source of truth; the S1 deflation bar stays factory-side, wrapped around `library::admit`.

---

## Public API (header-only, `namespace atx::engine`)

### `alpha::unparse` — the formula record (S4b-1)
```cpp
#include "atx/engine/alpha/unparse.hpp"
std::string alpha::unparse(const alpha::Ast& ast, alpha::ExprId root);  // render one subtree
std::string alpha::unparse(const alpha::Ast& ast);                      // render the single/anon root
```
Renders a genome's `Ast` back to a DSL expression string. **Soundness contract:** `parse_expr(unparse(ast))`
succeeds and yields the **same `canonical_hash`** as the original — the formula round-trips losslessly through the
canonical key (binary ops are infix and fully parenthesised so precedence can't reassociate; constants use
shortest-round-trip `to_chars` so the exact `bit_cast<u64>` bits reproduce). Every admitted alpha stores its
`unparse` in `Provenance.expr_source`, so it is inspectable AND re-parseable.

### `factory::PoolView` — the corr seam (S4b-2)
```cpp
#include "atx/engine/factory/pool_view.hpp"
struct factory::PoolView { virtual f64 worst_corr(std::span<const f64> pnl) const = 0; };
class  factory::AlphaStorePool : PoolView;   // O(N) exact scan over a combine::AlphaStore (S3 fixtures)
class  factory::LibraryPool     : PoolView;   // O(neighbors) SimHash scan over a persistent library (the engine)
// new overload (max-based diversification term; legacy mean-based AlphaStore overload retained):
Result<factory::FitnessReport> factory::pool_aware_fitness(
    const Genome&, const PoolView&, const alpha::Panel&, const WeightPolicy&,
    const exec::ExecutionSimulator&, const FitnessCfg&, const alpha::Panel* weak = nullptr);
// the one additive library accessor backing LibraryPool:
f64 library::Library::worst_corr_to_pool(std::span<const f64> pnl) const;  // 0.0 on empty pool
```
`worst_corr = max |corr|` of the candidate vs the pool. Both backings return *max* so they agree (within the S4-3
SimHash recall caveat — exact on the orthogonal-basis fixtures). The library-backed path kills the O(N²) re-gate.

### `factory::Factory::mine_into` — the admit bridge (S4b-3)
```cpp
#include "atx/engine/factory/factory.hpp"
FactoryReport factory::Factory::mine_into(const FactoryConfig&, library::Library&, const combine::AlphaGate&);
```
The **real** persistent admit path (the old ephemeral `mine(combine::AlphaStore&, …)` is retained test-only). Per
ranked candidate: realize streams → `compute_metrics` → re-score against the `LibraryPool` → build an
`AlphaCandidate{canon_hash (computed via canonical_hash), pnl, pos_flat, metrics, Provenance{expr_source, seed}, as_of, source=nullptr}`
→ admit iff **`dsr ≥ cfg.min_dsr`** (deflation N = the running trial count, factory-side) **AND**
`library.admit(...) == Accept`. `FactoryReport` gains `duplicates`, `library_n_alphas_before/after`, and a
per-`AdmitKind` `reject_histogram[6]`. The mine→library digest folds the search digest + each admission decision.

### `factory::ResearchDriver` — the continuous engine (S4b-4)
```cpp
#include "atx/engine/factory/research_driver.hpp"
struct factory::ResearchConfig { FactoryConfig per_run; usize max_runs, patience; u64 master_seed; };
struct factory::ResearchReport { usize runs, total_mined, total_admitted, total_duplicates;
                                 u64 library_size; std::array<usize,6> lifecycle_histogram;
                                 f64 dedup_pct; u64 digest; u32 manifest_version_id; u64 seed; };
class  factory::ResearchDriver {
  ResearchDriver(library::Library&, const alpha::Library& dsl, const alpha::Panel&,
                 const exec::ExecutionSimulator&, const WeightPolicy&, const combine::AlphaGate&);
  ResearchReport run(const ResearchConfig&);
};
```
Owns a `library::Library` and drives a budget-bounded **mine→admit→repeat** loop over a **fixed** research panel,
growing the persistent deduplicated library across runs until budget exhaustion OR `patience` consecutive
zero-admit runs (novelty exhaustion). Every per-run seed is `detail::seed_for_run(master_seed, run)` (pure SplitMix),
which feeds `SearchConfig.master_seed` and composes with SearchDriver's `(gen, idx)` → the full
`(master_seed, run, gen, idx)` axis without touching SearchDriver. Checkpoint/resume via the library manifest
(`snapshot()` seals; reopening the same dir replays byte-identically — `rebuild_equals`).

---

## The five proven contracts (`ResearchEngine` capstone, non-vacuous)

| Contract | Test | Proof |
|---|---|---|
| **F1** whole-engine seeded replay | `SeededEngineReplaysByteIdentical` | two seeded runs → equal `digest` AND manifest `version_id` |
| **F4** anti-snooping at scale | `NoiseGrowsLibraryByNothing` | 4 noise seeds admit 0; real signal admits >0 under the same bar |
| **F6** cross-run dedup across persistence | `CrossRunDedupNeverReadmits` | reopen-from-disk re-mine never re-admits (deduped off reloaded sqlite) |
| **Scale lever** O(neighbors) | `research_bench` | ~3.35× O(neighbors) vs O(N) at 4096 alphas, widening with size |
| **Unparse soundness** | `UnparseRoundTripsThroughCanonicalHash` | every admit's `expr_source` re-parses to the same canonical_hash |

Full engine suite **983/983** green; strict `/W4 /permissive- /WX` + strict-FP clean; **atx-core untouched**.

---

## Quick start

```cpp
using namespace atx::engine;
library::Library lib = library::Library::open(dir, gate_cfg, /*seeds=*/{master_seed});
factory::ResearchDriver engine{lib, dsl_ops, panel, sim, weight_policy, gate};
factory::ResearchConfig cfg{ /*per_run=*/factory_cfg, /*max_runs=*/16, /*patience=*/3, /*master_seed=*/master_seed };
factory::ResearchReport rep = engine.run(cfg);   // grows lib; rep.digest replays byte-identically
// every admitted alpha: lib.get(id).provenance.expr_source re-parses to the same canonical_hash
```

## NOT in S4b (→ the remaining arc)
- Re-eval adapter (re-parse `expr_source` → compile → eval over a NEW panel, for decay/live monitoring) → **S7**.
- The combiner (linear/ML/regime over the now-populated library pool) → **S5**.
- Cost-aware fitness + capacity → **S6**. Multi-period optimizer + decay monitor + dead-alpha recycling → **S7**.
