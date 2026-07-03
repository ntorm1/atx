# Sprint 3 — Search/Fitness: Net-of-Cost Selection + Seed-Warm-Start Mining

**Goal:** Make the GA discover tradeable alphas instead of leaning on seeds — by (a) folding
turnover/net-of-cost into the search signal so it stops climbing toward uneconomic high-turnover
structures, and (b) letting the GA grow and preserve the conditioning structure that makes seeds
work. All opt-in, default byte-identical.

**Owns (exclusive):**
`atx-engine/src/factory/{fitness,search_driver,mutation,genome}.cpp`,
`atx-engine/include/atx/engine/factory/{fitness,search_driver,generate,genome,behavior,mutation,pool_view}.hpp`,
tests `atx-engine/tests/factory/{factory_search_driver,search_quality,factory_nsga_search,factory_mutation,factory_genome}_test.cpp`.

**Must NOT touch:** `src/factory/factory.cpp` (Sprint 2), eval headers (Sprint 1),
`combine/{gate,metrics}.hpp` / `library/library.hpp` / `cost/cost_aware.hpp` (Sprint 4),
`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` (Sprint 7),
`tests/factory/oracle.hpp` (untouchable).

---

## Determinism contract

This sprint uses contract **(A) opt-in / default-byte-identical** (ROADMAP §Shared determinism
contract). Every output-changing capability is gated behind a new `SearchConfig`, `FitnessCfg`, or
`GenCfg` field that defaults to today's value. The pinned golden digests are UNCHANGED on the
default path: `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, MultiObjective/ScalarRaw digests,
`FactoryOos.MineIntoOffPathDigestUnchanged`, OOS goldens. `oracle.hpp` is untouched. Each opt-in
ships three test classes: (a) off-path byte-identity, (b) on-path RED→GREEN, (c) twice-run
determinism. Sprint 7's tradeable-alpha build profile turns the opt-ins on; nothing in this sprint
touches defaults.

---

## Why the GA mines junk — diagnosis table

| Root cause | Location (verified) | Effect |
|---|---|---|
| `raw = wq * diversify * robust` is turnover-blind | `fitness.hpp:14` (formula comment); `FitnessReport.raw` assembled in `detail::finish_report` (`fitness.hpp:332–338`); `FitnessCfg` struct has no turnover field (`fitness.hpp:242–248`) | GA maximizes fitness without any penalty for daily turnover; a high-turnover alpha netting 0.37 Sharpe is scored identically to a low-turnover alpha netting 1.2 Sharpe |
| Cost only enters as a separate NSGA objective gated on `target_aum > 0`; ScalarRaw elitism ignores it entirely | `fitness.hpp:184–189` (objective-slot comment), `fitness.hpp:230–248` (`FitnessCfg.target_aum = 0.0` default) | Default `ScalarRaw` path: cost has ZERO influence on selection |
| `enable_wrap_in_op = false` — the only mutation that can ADD a conditioning layer is OFF | `search_driver.hpp:211` | `wrap_in_op` (zscore/rank/signedpower/group_neutralize around a leaf) is implemented and tested but never fires; GA can never build the `signedpower(zscore(raw), p)` structure seeds bring |
| Novelty rewards failing genomes | `behavior.hpp:166–170` (degenerate comment: `< 2 valid pairs or zero-variance leg → corr 0 → distance 1`); `behavior.hpp:273–274` (empty-neighbourhood edge: `→ 1.0`) | A near-zero-PnL genome has a nearly-zero descriptor; PnlCorr distance to any viable peer defaults to 1.0 = "maximally novel" — junk genomes accumulate novelty score |
| Seeds lose survival pressure after gen 0 | `search_driver.cpp:421–426` (`!cfg.seed_from_grammar` path: `pop.push_back(seeds[i % seeds.size()].clone())` — all N slots filled with cycled clones, then subject to tournament selection with no elitism guarantee) | Gen-0 seeds can be displaced by high-turnover grammar-generated genomes in generation 1; the conditioned structure is not guaranteed to survive |

---

## Tasks

### S3-0 — Turnover/net-cost penalty in `raw` via `FitnessCfg.turnover_penalty_slope`

**Root cause:** `raw = wq * diversify * robust` (`fitness.hpp:14` formula comment; assembled in
`detail::finish_report`, `fitness.hpp:332–338`). `FitnessCfg` (`fitness.hpp:242–248`) has no
turnover field. Cost enters only via `objectives[4] = -cost_bps` when `target_aum > 0`
(`fitness.hpp:184–189`), which ScalarRaw mode ignores entirely.

**Fix:** Add two fields to `FitnessCfg` (`fitness.hpp`):
- `turnover_penalty_slope` (default `0.0`)
- `max_turnover_target` (default `+inf`)

In `detail::finish_report` (`fitness.hpp`), after computing `raw = wq * diversify * robust`, apply:

```
if (slope > 0.0) {
    const f64 excess = max(0.0, turnover - max_turnover_target);
    const f64 slack  = max(max_turnover_target * slope, kEps);
    const f64 mult   = clamp(1.0 - excess / slack, kFloor, 1.0);
    raw *= mult;
}
```

`turnover` is already present in the WQ-fitness path via `combine::compute_metrics()` — no
additional eval. The `kFloor` prevents `raw` going negative (suggested value: `0.0`).

**Determinism:** `turnover_penalty_slope` defaults to `0.0` → the `if` branch is never entered →
byte-identical to today. No new RNG draws.

**Accept:**
- (a) Default `slope=0`: `ScalarRaw_ReproducesGoldenDigest` unchanged; MultiObjective digest unchanged.
- (b) `slope > 0`: a synthetic high-turnover genome ranks strictly below an otherwise-equivalent
  low-turnover genome.
- (c) Twice-run with `slope > 0`: identical scores.

---

### S3-1 — Cache CPCV folds/label-spans per `(n_periods, cpcv)` (pure perf, byte-identical)

**Root cause:** `detail::fitness_core` (`fitness.cpp:293–295`) rebuilds `point_label_spans` and
`eval::cpcv_folds` for EVERY genome even though these depend only on `(n_periods, cfg.cpcv)`:

```cpp
const std::vector<eval::LabelSpan> spans = point_label_spans(strm.n_periods());
const std::vector<eval::CpcvFold> folds =
    eval::cpcv_folds(std::span<const eval::LabelSpan>{spans}, cfg.cpcv);
```

A run with `pop=60 × gen=15` rebuilds these 900+ times; the fold geometry is constant across all
of them.

**Fix:** Introduce a `CpcvCache` (map keyed on `(n_periods, cpcv)`) held on the `SearchDriver`
or threaded through `fitness_core` as an optional output-parameter cache. Populate on first call;
subsequent calls return the cached result. Pure perf: no value or RNG-stream change.

**Determinism:** Math is identical — same folds, same values. A diff test proves digest unchanged.

**Accept:**
- (a) Digest byte-identical before and after.
- (b) Allocation count per genome drops to O(1) (no `LabelSpan`/`CpcvFold` alloc past the first).
- (c) Measurable wall-time reduction on a `gen=15 × pop=60` run (record before/after bench line
  in commit body per [implementation-quality.md](../docs/implementation-quality.md)).

---

### S3-2 — `enable_wrap_in_op` honored + opt-in `protect_seed_elites` + `mutate_seed_copies`

**Root cause (wrap_in_op):** `enable_wrap_in_op{false}` is the default (`search_driver.hpp:211`).
`wrap_in_op` is the ONLY mutation that can add a conditioning layer
(zscore/rank/signedpower/winsorize/group_neutralize around a subtree). It is already implemented
and tested. The knob is wired — keeping DEFAULT false is sufficient for byte-identity; this task
ensures the knob is fully honored when true, and adds two new opt-in knobs for seed survival.

**Root cause (seed pressure):** `init_population` (`search_driver.cpp:421–426`): when
`seed_from_grammar = false` (the default boundary-pin path), ALL population slots are filled with
cycled clones of the seeds:

```cpp
if (!cfg.seed_from_grammar) {
    for (atx::usize i = 0; i < cfg.population; ++i) {
        pop.push_back(seeds[i % seeds.size()].clone());
        pop.back().canon_hash = seeds[i % seeds.size()].canon_hash;
    }
    return pop;
}
```

These clones enter tournament selection with no special protection; a high-turnover grammar genome
(once `seed_from_grammar = true`) can displace them from the elite set by generation 1.

**Fix — three opt-in knobs (all default to today's behavior):**

1. `SearchConfig.enable_wrap_in_op` (already exists, `search_driver.hpp:211`, default `false`) —
   verify the existing flag path is complete and correctly guards every RNG draw.

2. `SearchConfig.protect_seed_elites` (new, default `false`) — when `true`, tag each
   seed-derived `Genome` with a `from_seed` boolean (new field on `Genome`, default `false`).
   The selection step guarantees the top-ranked `from_seed` genome survives to
   `min(current_gen + 1, cfg.protect_until_gen)` (new field, default `3`). No RNG change; the
   protection is a post-selection insertion, not a tournament modification.

3. `SearchConfig.mutate_seed_copies` (new, default `false`) — when `true` and
   `seed_from_grammar = false`, the cycling fill at `search_driver.cpp:422–424` applies one
   seeded mutation to each cloned slot (using `detail::seed_for(master_seed, kMutateSeedAxis, i)`)
   instead of producing N identical clones. Each slot gets a structurally distinct mutant; the
   seed's original is still placed at slot 0.

**Determinism:**
- `protect_seed_elites = false` (default): selection path unchanged, no `from_seed` check
  entered, byte-identical.
- `mutate_seed_copies = false` (default): `init_population` path at `search_driver.cpp:421–426`
  unchanged, byte-identical.
- `enable_wrap_in_op = false` (default, already established): `search_driver.hpp:207–211`
  comment confirms zero new RNG draws on the disabled path.

**Accept:**
- (a) All three knobs at default: golden digest unchanged.
- (b) `protect_seed_elites = true`: a seed genome survives in the admitted set through gen 3 on a
  synthetic run where grammar genomes would otherwise dominate.
- (c) `mutate_seed_copies = true`: gen-0 population contains no duplicate canon hashes (all slots
  distinct, versus N identical clones today).
- (d) Twice-run with any knob on: identical.

---

### S3-3 — Opt-in `deflate_selection` test + `min_viable_raw` viable-only novelty

**Root cause (deflate_selection):** `deflate_selection = false` already exists
(`search_driver.hpp:203`). No dedicated `search_quality` test proves the on-path behavior (digest
changes, later-generation raw is haircut by DSR) or the off-path byte-identity.

**Root cause (junk novelty):** `behavioral_distance` (`behavior.hpp:166–170`) documents the
degenerate: `< 2 valid pairs or a zero-variance leg → corr 0 → distance 1`. A failing genome
(raw ≈ 0) produces a near-zero PnL descriptor; its PnlCorr distance to any viable peer is ≈ 1.0
= "maximally novel" (`behavior.hpp:273–274` also shows the empty-neighbourhood edge → `1.0`). The
`BehavioralArchive::novelty()` method then scores this junk genome as highly diverse, promoting it
in the NSGA-II objective space.

**Fix (a) — `deflate_selection` test:** Add a `search_quality_test.cpp` (or extend the existing
suite) with two cases:
- Off-path: `deflate_selection = false` → `ScalarRaw_ReproducesGoldenDigest` unchanged.
- On-path: `deflate_selection = true` → digest diverges; later-gen `raw` values are strictly
  less than or equal to the off-path values (the DSR haircut bites).

**Fix (b) — `SearchConfig.min_viable_raw`** (new field, default `0.0`): when `> 0`, zero the
behavioral descriptor for any genome whose `raw < min_viable_raw` before it is submitted to
`BehavioralArchive::novelty()`. Zeroed descriptors produce distance 1.0 against all peers — the
junk genome is still "maximally novel" against itself, but the archive never learns from it, and
viable genomes' novelty scores are no longer contaminated by junk peers. The viable floor is never
applied to admission or scoring — only to the descriptor submitted for novelty.

**Determinism:** `min_viable_raw = 0.0` (default) → zeroing condition never true → behavioral
objective path unchanged → byte-identical.

**Accept:**
- (a) `deflate_selection = false`: digest unchanged (existing golden).
- (b) `deflate_selection = true`: RED→GREEN; twice-run identical.
- (c) `min_viable_raw = 0.0`: digest unchanged.
- (d) `min_viable_raw > 0`: a synthetic junk genome (raw ≈ 0) no longer displaces a viable
  genome from the top-k novelty front; test shows viable genome novelty score improves.

---

### S3-4 — Opt-in weighted grammar productions + wider scalar pool

**Root cause:** `gen_f64` picks uniformly from 8 cases via `rng.next_u64() % 8`
(`generate.hpp:129–131`). Case distribution is flat: each of unary-elementwise, binary-arithmetic,
cs-simple, cs-scalar, group-aware, ts-unary, ts-binary, negate gets exactly 1/8 weight.
P(well-conditioned depth-3 expression with ≥1 cross-sectional + ≥1 time-series layer) ≈ 0.2%.
`emit_scalar` draws from a fixed 4-element pool `{0.5, 1.5, 2.0, 3.0}` (`generate.hpp:77`).

**Fix:** Add two opt-in fields to `GenCfg`:

1. `production_weights` (new, type `std::array<f64, 8>`, default = uniform `{1,1,1,1,1,1,1,1}`)
   — when non-uniform, use a weighted draw instead of `% 8`. The draw MUST consume exactly ONE
   `rng.next_u64()` call in the same position as the current `% 8` draw, so the RNG stream
   position is identical whether weights are uniform or not. Implement as: draw a `u64`, map to
   `[0, sum_weights)` via the same modulo arithmetic, then binary-search the prefix-sum table.
   When weights are all equal, `% sum_weights == % 8` (integer math) → byte-identical stream.

2. `scalar_pool` (new, type `std::vector<std::string_view>`, default = `{"0.5","1.5","2.0","3.0"}`)
   — `emit_scalar` draws from this pool using `pick_sv`. Default pool → same 4-element array →
   same modulo draw → byte-identical.

**Determinism:** Both fields default to today's exact values. The RNG call count per `gen_f64`
invocation is unchanged. Default path produces byte-identical genomes (F1).

**Accept:**
- (a) Default `production_weights` + default `scalar_pool`: `ScalarRaw_ReproducesGoldenDigest`
  unchanged; MultiObjective digest unchanged.
- (b) Non-uniform weights biased toward cs+ts: cross-sectional and time-series nodes appear more
  frequently in the generated population (verify by node-type histogram over 1000 genomes).
- (c) Wider `scalar_pool`: generates values outside `{0.5,1.5,2.0,3.0}` on-path.
- (d) Twice-run with non-default weights: identical.

---

## Sequencing

1. **S3-1** (CPCV cache) first — pure perf, zero risk, unblocks bench baseline.
2. **S3-0** (turnover penalty) — `FitnessCfg` change; self-contained to `fitness.hpp`.
3. **S3-2** (wrap/seed knobs) — `SearchConfig` + `Genome` changes; slightly wider scope.
4. **S3-3** (deflate test + viable novelty) — `behavior.hpp` + `SearchConfig`; depends on S3-2
   for `from_seed` plumbing clarity but otherwise independent.
5. **S3-4** (weighted grammar) — `generate.hpp` + `GenCfg`; fully independent, last.

S3-0 and S3-1 may be dispatched in parallel (disjoint files). S3-2 before S3-3 (S3-3 needs the
`Genome.from_seed` field established if the viable-novelty test uses it).

---

## Risks / guardrails

- **Turnover field source.** `turnover` must be read from the fitness path — confirm the
  `combine::compute_metrics()` return value carries per-period turnover before adding the penalty
  field. If it does not, the penalty cannot be computed without a new eval call (a blocker). Verify
  before starting S3-0 implementation.
- **RNG stream invariant for weighted productions (S3-4).** The weighted draw MUST use exactly
  one `rng.next_u64()` call at the same call-site position as `% 8`. Any extra draw breaks
  byte-identity on the default path. Validate with the golden digest test before merging.
- **`from_seed` tag through clone/rebuild.** `Genome.from_seed` must survive `clone()` and the
  mutation path (`mutate_one`, `wrap_in_op`). Verify that `rebuild_with` preserves the tag.
  `oracle.hpp` must not be touched.
- **`protect_seed_elites` and the canonical order invariant (F2).** Elite insertion must happen
  AFTER the canonical sort, not before, or it breaks the F2 determinism proof. Document the
  insertion point explicitly.

---

## Bench / acceptance

Per [implementation-quality.md](../docs/implementation-quality.md):

- Every performance claim (S3-1 CPCV cache) requires a recorded before/after bench line (wall-time
  per generation, allocation count per genome) in the commit body.
- Per-knob test matrix: three test classes per knob (off-path byte-identity, on-path divergence,
  twice-run), plus an integration `search_quality` assertion that with S3-0 + S3-3 both on, a
  high-turnover/junk genome ranks below a conditioned low-turnover genome.

**Sprint-level acceptance gate:**
1. Default golden digests (`ScalarRaw_ReproducesGoldenDigest`, MultiObjective, OOS) unchanged.
2. Each opt-in knob: RED→GREEN test exists; twice-run produces identical output.
3. S3-1 CPCV cache: digest byte-identical before/after; measurable wall-time improvement on a
   `gen=15 × pop=60` bench run.
4. `Genome.from_seed` field plumbed through `clone()` / `rebuild_with()` / `mutate_one()`.
5. No changes to `oracle.hpp`, `factory.cpp`, eval headers, gate/metrics/library headers, or
   `atx-impl/src/config.*`.

## Out of scope

Gate-side net-of-cost fitness floor (Sprint 4); factory admission ladder (Sprint 2); CLI flags and
turning the knobs on for the real production run (Sprint 7).
