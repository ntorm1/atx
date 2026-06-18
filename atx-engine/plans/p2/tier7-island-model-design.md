# Tier 7 (proposed) — Island-Model Search: the next concurrency ceiling-raiser

**Status:** design proposal, not yet implemented. This is the one *semantic* tier I judge
worth pursuing next. It is deliberately scoped as a NEW search mode behind a config flag,
not a change to the existing path — so the current `0xa83f0d3e0b41a18d` golden digest and
every shipped guarantee stay byte-identical and untouched.

---

## 1. Where we are, and why a new tier is needed

Tiers 0–6 made one `SearchDriver::run` as fast as it can reasonably go while staying
byte-identical:

- **Tier 0/1** — `evaluate_generation` runs in parallel over a `DetPool`, LPT-ordered.
- **Tier 2/3/4** — each genome is `Engine::evaluate`'d exactly once per generation on a
  per-worker engine that is reused within *and* across generations.
- **Tier 5** — `reproduce` runs in parallel.
- **Tier 6** — the VM cross-section scratch is pooled (no per-instruction alloc).

What remains is an **Amdahl ceiling intrinsic to a single population**:

1. **The serial spine of `run()`.** `assign_pareto_ranks` (O(P²·k) in MultiObjective),
   `canon_ordered_indices`, `update_archive`, and the digest fold are serial *barriers*
   between the parallel eval and the parallel reproduce. They are cheap per generation but
   they are sequential dependencies — generation N+1 cannot start until N is fully ranked.
2. **The longest single eval.** Within a generation, wall-clock is bounded below by the
   single most expensive genome's `evaluate` (LPT helps balance, but cannot beat the max).
3. **Population-size coupling.** Bigger populations parallelize better but change the
   search trajectory; you cannot simply grow P to soak up cores without changing results.

A single population therefore stops scaling once `n_workers` approaches the per-generation
parallel width. **The island model breaks that ceiling by parallelizing across whole
evolutionary runs, not within a generation.**

---

## 2. The idea

Run **K independent sub-populations ("islands")**, each its own miniature `SearchDriver`
loop with its own seed stream, evolving concurrently. Every `M` generations the islands
**migrate** a few elites along a fixed ring topology (island *i* sends its top-`m` to
island *(i+1) mod K*). After `G` generations the per-island results are merged into one
`SearchResult`.

This is the classic coarse-grained parallel GA (Cantú-Paz, *Efficient and Accurate
Parallel Genetic Algorithms*, 2000). It is attractive here because:

- **It scales with K, independent of the per-generation parallel width.** K=8 islands ×
  population 16 keeps 8 cores busy with *no* synchronization barrier between islands except
  at the migration points — far less serial coupling than one population of 128.
- **It is usually a *better search*, not just a faster one.** Independent islands explore
  different basins; periodic migration spreads elites. Coarse-grained GAs routinely beat a
  single large panmictic population on the same evaluation budget.
- **The engine already has the primitives.** A `DetPool`, per-worker engines, a fully
  deterministic `SearchDriver`, and id-seeded RNG (`seed_for`) — islands compose these.

---

## 3. Why this is "semantic" — the determinism reality

Tiers 0–6 were *throughput-only*: they never changed which candidates were scored or in
what order, so the golden digest was preserved by construction. **The island model changes
the search itself.** K islands with K independent seed streams + migration produce a
*different* set of scored candidates than one population — necessarily a different digest.

So this tier **cannot** preserve `0xa83f0d3e0b41a18d`, and that is fine *as long as*:

1. It is a **new mode**, gated by config (`SearchConfig::islands` / a `SearchMode`), with
   the default unchanged. K=1 (or mode off) must reproduce the existing single-population
   path **bit-for-bit** — i.e. the current golden digest still pins the K=1 path.
2. The new K>1 mode has **its own** byte-identical-determinism contract and **its own**
   golden-digest gate (a new pinned value), plus a worker-count-invariance gate.

The whole value of this design is that K>1 can still be made **fully deterministic** —
same `(master_seed, K, M, m, G, population)` ⇒ byte-identical result across any
`n_workers`. The determinism just has to be *re-established* for the new mode, not
*preserved* from the old one. Concretely:

- **Per-island, per-generation, per-index seeding.** Extend `seed_for(master, gen, idx)`
  to `seed_for(master, island, gen, idx)` (one more SplitMix round). Island *i*'s entire
  RNG stream is then a pure function of `(master, i, gen, idx)` — never worker/thread/order.
- **Fixed migration topology + fixed migration order.** The ring `i → (i+1) mod K` is a
  pure function of K. Migrants are island *i*'s top-`m` by the existing canonical survivor
  order (`pareto_ordered_indices`) — already deterministic. Migrants are *inserted* at the
  destination in canonical-id order before that island's next selection draw, so the
  receiving island's RNG stream does not shift based on arrival timing.
- **Disjoint single-writer islands.** Each island owns its own population vector, canon
  set, fitness cache, behavior archive, and **its own per-worker engine vector** (or the
  islands share the DetPool but never touch each other's mutable state). Migration is the
  *only* cross-island data flow and it happens at a serial barrier every M generations.
- **Deterministic merge.** The final `SearchResult.digest` folds the per-island digests in
  fixed island-id order; `all_scored` / `admitted_candidates` are merged in
  (island-id, canonical) order. No cross-island FP accumulation.

The migration barrier is the one new synchronization point. Between barriers, islands run
with zero coupling — that is where the scaling comes from.

---

## 4. New determinism gates required (the cost of admission)

Because the digest changes, this tier may NOT be merged until these exist and are green:

1. **`IslandSearch.ReproducesGoldenDigest`** — a new pinned digest for a fixed
   `(master_seed, K, M, m, G, population)`, asserting byte-identity run-to-run.
2. **`IslandSearch.WorkerCountInvariant`** — same K>1 config, byte-identical across
   `n_workers ∈ {1,2,4,8}` (islands may map to workers in any order; result must not move).
3. **`IslandSearch.IslandCountIsNotWorkerCount`** — K (a search parameter, affects results)
   is independent of `n_workers` (a fan-out parameter, must NOT affect results). Pin a
   K=4 digest and show it is identical at n_workers 1, 2, 4 — proving the two axes are
   orthogonal.
4. **`IslandSearch.K1_EqualsSinglePopulation`** — with K=1 the island path is byte-identical
   to the legacy `SearchDriver::run`, so the *existing* `0xa83f0d3e0b41a18d` gate still
   guards the default path through the new code.
5. **Migration determinism unit test** — a fixed 2-island fixture asserting the exact set
   of migrants and the exact post-migration populations.

These mirror the F1/F2 contracts the single-population path already has; the engineering is
"re-prove the same properties one level up."

---

## 5. Throughput argument (why it's worth the gate cost)

Let `T_gen` be one generation's wall-clock for a single population at the parallel-width
ceiling, and `s` the serial fraction of `run()` (rank/archive/fold ≈ 10–25% in
MultiObjective at small P, per the Tier-5 investigation). Single-population speedup is
Amdahl-capped at `1/s` regardless of cores.

K islands of the same population size each run their own loop; with K ≤ `n_workers` and
near-zero inter-island coupling (one barrier per M generations), aggregate candidate
throughput scales ~**linearly in K** until the migration barrier and memory bandwidth
dominate. The migration barrier is `O(K·m)` every M generations — negligible. So on an
8-core box, K=6–8 islands should deliver a multiple of the single-population throughput
that the per-generation parallelism alone cannot reach, *and* typically a better best-fitness
for the same total evaluation budget.

This must be **measured**, not assumed — see §7. (Note: the Release benchmark is currently
blocked by pre-existing `/WX` latent bugs unrelated to this work; those must be fixed first
to get honest Release numbers — see the SDD ledger "Quantification attempt" entry.)

---

## 6. Implementation sketch

**Config (`search_driver.hpp`, `SearchConfig`):**
```cpp
atx::usize islands{1};            // K: number of sub-populations (1 == legacy path)
atx::usize migration_interval{4}; // M: migrate every M generations (ignored if K==1)
atx::usize migration_count{1};    // m: elites exchanged per island per migration
// (master_seed, islands, migration_interval, migration_count, generations, population)
// is the full artifact key for the K>1 digest.
```

**Seeding (`detail::seed_for`):** add an `island` dimension —
`seed_for(master, island, gen, idx)` with one extra SplitMix mix round. The legacy
3-arg form becomes `seed_for(master, /*island=*/0, gen, idx)` so K=1 is bit-identical.

**Driver (`SearchDriver`):**
- Refactor the body of today's `run()` into `run_island(island_id, cfg, pool, &shared_pool,
  engines_for_island, …) -> IslandState` that advances ONE island by `migration_interval`
  generations and returns its population + caches + digest-so-far. Today's `run()` becomes
  `run_island(0, …)` looped to completion when `islands == 1` — *this is the K=1 equality
  proof.*
- New `run()`: construct K `IslandState`s; loop `G / M` migration epochs; in each epoch run
  all K islands (parallel — each island is independent work, dispatched over the DetPool or
  a thread per island with its own per-worker engine sub-pool); then apply the deterministic
  ring migration at the serial barrier; finally merge.
- **Engine ownership:** either K × `n_workers` engines (island × worker), or—simpler—each
  island gets its own `DetPool` slice; the Tier-2/4 idempotence proof carries over verbatim
  since every engine is still bound to the single const `panel_`.

**Merge:** fold per-island digests in island-id order; concat `all_scored` in
(island-id, canonical) order; pick global `admitted_candidates` by raw fitness across all
islands with the existing canonical tie-break.

**Touch list:** `include/atx/engine/factory/search_driver.hpp` (config + decls),
`src/factory/search_driver.cpp` (run/run_island/migrate/merge), a new
`tests/factory_island_search_test.cpp` (the 5 gates above), and a `BM_IslandSearch` bench
case sweeping K.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Digest changes → looks like a regression | New mode behind a flag; K=1 pins the OLD digest; K>1 gets its OWN pinned digest. Never touch the legacy path. |
| Migration timing races perturb results | Migration only at a serial barrier; migrants inserted in canonical order before the next RNG draw, so no stream shift. |
| Cross-island state bleed (a real data race) | Islands own disjoint state; the ONLY shared reads are `panel_`/`pool` (const). Reuse the Tier-5 reentrancy audit for the per-island reproduce path. |
| "Faster" but unverified | Gate the merge on the new Release benchmark (`BM_IslandSearch`, K-sweep). Fix the pre-existing Release `/WX` latent bugs first so numbers are honest. |
| Scope creep into the VM | None — this tier lives entirely in `factory/`. No VM/FP changes (those are the *other*, riskier path: SIMD/rolling kernels behind a ULP-tolerance gate — explicitly NOT this proposal). |

---

## 8. Why this over the other risky option

The alternative risky tier is **VM FP-reordering** (SIMD horizontal sums, O(1) rolling
time-series kernels). It is a single-thread throughput play, it requires abandoning
byte-identity for a ULP-tolerance differential oracle, and it touches the most sensitive
code in the engine. The island model, by contrast, is a *concurrency* capability (which is
the standing directive's theme), it can remain **fully deterministic** under a new gate, it
reuses primitives we have already proven, and it raises the scaling ceiling that Tiers 0–6
cannot. It is the higher-value, better-contained next step.

---

*Prepared as the hand-off artifact for the concurrency-optimization track. Tiers 0–6 are
complete, byte-identical, and independently reviewed; this is the recommended Tier 7.*
