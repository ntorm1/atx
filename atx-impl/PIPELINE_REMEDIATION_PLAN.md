# atx-impl Pipeline — Diagnosis & Remediation Plan

_2026-06-19. Investigated via 5 parallel code-review agents tracing the discover →
combine control flow + alpha-generation process. Symptom: a 30-minute deep run
(gen-15, pop-60) produced only 2 alphas, one degenerate (`zscore(zscore(sector))`)._

## TL;DR

The pipeline isn't "slow at finding alphas" — it has **four independent defects that
compound**:

1. **Search stops at ~gen 4** because the stagnation early-stop watches a
   monotone, elitism-pinned best-fitness curve that goes flat the instant no child
   sets a new record. ~60% of the eval budget is thrown away. (Not real convergence.)
2. **The admission gate rejects ~93% of survivors** on a fixed annualized-Sharpe
   floor (`min_sharpe=1.0`) applied to a holdout window so short its Sharpe is pure
   noise. The 331 reject bucket is `RejectSharpe`, **not** "holdout fitness" as
   STATUS.md claims.
3. **The one alpha that does admit is degenerate** because categorical `sector` is
   fed into the numeric grammar unfiltered and the type system has no input-dtype
   check, so `zscore(sector)` typechecks and runs on raw sector-id integers.
4. **You can never reach WorldQuant scale** because the OOS admission loop is
   single-threaded with ~3× redundant full-panel evals, and the alpha library is
   wiped every run, so nothing accumulates and the (already-built) strong combiners
   are never driven.

Fixes 1–3 are near-one-line config/wiring changes that should be validated FIRST.
Fix 4 is structural (throughput + persistence) and is what unlocks scale.

## STATUS.md corrections

- "binding reject is holdout *fitness* (331)" → **wrong**. `reject_hist` is indexed by
  `library::AdmitKind` (`library.hpp:106-113`). Bucket 2 = `RejectSharpe` = 331.
  `RejectFitness` is bucket 3 = 5. The wall is the **holdout Sharpe floor**.
- "search converges early (best fitness plateaus ~gen 4)" → it does not converge; the
  **stagnation early-stop fires** against a non-decreasing best-raw staircase
  (`search_driver.cpp:282-288`, patience=4). The population never gets to gen 15.
- `adaptive_operators` is ON in production (default `true`,
  `search_driver.hpp:177`); it is pinned `false` only in the resume-identity *tests*.

---

## P0 — Unblock alpha production (validate before anything else)

These three are tiny and should turn "2 degenerate alphas" into "many candidates,
many admits". Apply, run one deep search, compare `reject_hist`.

### P0.1 — Kill / fix the premature stagnation stop
- **Root cause:** `stagnation_patience=4` vs a monotone elitism-pinned `best_raw`
  (`search_driver.cpp:282-288`, `:238-241`). Earliest stop is gen ~5 → matches the
  observed plateau.
- **Fix (quick):** in `stage_discover.cpp` after the `SearchConfig` build (~line 374):
  ```cpp
  sc.stagnation_patience = 0;            // run the full generations budget
  ```
- **Fix (correct):** re-base the stop on a *collapse* signal — stop only if BOTH
  best-raw AND `mean_raw` (already computed, `search_driver.cpp:258`) are flat for the
  patience window, or use an epsilon-improvement window instead of strict `>`.

### P0.2 — Stop gating admission on raw holdout Sharpe
- **Root cause:** `min_sharpe=1.0` (`config.hpp:58`, `gate.hpp:67-71`) is the first
  check in `verdict_for` (`library.hpp:325`); a fixed annualized-Sharpe floor on a
  `floor(oos_fraction*T)`-length holdout whose Sharpe noise SE ≈ `sqrt(252/T_hold)`
  (~2.0 for ~60 days) ≥ the threshold itself. 331/356 die here.
- **Fix (quick):** run with `--min-sharpe 0.25` (or `0`) and instead gate on the
  already-computed, T- and trial-count-aware deflated Sharpe (`--min-dsr`, tune ~0.5).
  Note `config.hpp:49` defaults CLI `min_dsr=0.0` so DSR is currently OFF — turn it on.
- **Fix (correct):** replace the raw-Sharpe floor in the gate with a PSR/DSR
  significance test so admission rejects on statistical insignificance, not on an
  unattainable point estimate over a noisy window.

### P0.3 — Exclude categorical fields from the numeric grammar
- **Root cause:** `stage_discover.cpp:358-362` collects every panel field with zero
  filtering; `search_driver.cpp:398-399` jams that list into BOTH `numeric_fields` and
  `group_fields`. `OpSig` (`registry.hpp:276-306`) has `out_dtype` but no input-operand
  dtype, so `typecheck.cpp` never rejects a `Group`-typed `sector` flowing into
  `zscore`/`rank`/`ts_*`.
- **Fix:** partition fields by `alpha::detail::is_group_field(name)` at the source;
  set `numeric_fields` = numeric-only, `group_fields` = group-only; restrict
  `field_swap` (`search_driver.cpp:921`) and immigrant leaf candidates
  (`search_driver.cpp:827-829`) to the numeric partition.
- **Belt-and-suspenders:** add an input-dtype guard in `analyze_call`
  (`typecheck.cpp:327-393`) rejecting a `Group` primary on any non-group Cs/Ts op, so a
  degenerate *seed* can't reintroduce it. Optionally add a "has time variation"
  structural check (reject roots with `required_lookback==0` and a single static field
  leaf).

---

## P1 — Restore search diversity & align objectives

### P1.1 — Reduce Pareto objective count / drop the parsimony attractor
- **Root cause:** 5 live objectives (wq, diversify, robust, novelty, parsimony) over
  pop-60 → most genomes land in front 0; parsimony's smallest-tree genome gets
  `+inf` crowding (`pareto.hpp:218-223`) and becomes a tournament magnet → degenerate
  admits.
- **Fix:** drop `enable_parsimony` during search (use as a tie-break only); for pop<100
  prefer `objective_mode=ScalarRaw` with novelty folded into a penalized scalar, OR
  raise population well above the objective count.

### P1.2 — Stronger fresh-blood injection
- **Root cause:** `n_immigrants=2` (~3%/gen, `search_driver.cpp:819-839`); jitter sigma
  `0.5*0.9^gen` halves by gen ~7 (`search_driver.cpp:910-913`).
- **Fix:**
  ```cpp
  sc.n_immigrants       = std::max<atx::usize>(sc.population / 10, 4);
  sc.jitter_anneal_decay = 0.97;   // or floor sigma at ~0.15
  ```

### P1.3 — Align selection objective with admission
- **Root cause:** selection maximizes `raw = wq·diversify·robust` on TRAIN
  (`fitness.cpp:328`, `factory.cpp:688`); admission floors holdout raw Sharpe. `robust`
  is inert (=1.0, no weak panel, `fitness.cpp:276`).
- **Fix:** gate admission on the **same** holdout WQ-fitness/DSR the search optimizes;
  wire a weak-universe panel for the `robust` term or drop the inert factor and replace
  with a holdout-stability term.

### P1.4 — Population / budget sizing for deep runs
- **Root cause:** pop-60 × 5 objectives ≈ 12 genomes/objective — too few to maintain a
  meaningful frontier.
- **Fix:** for deep runs set `population >= 200`; prefer larger population over more
  generations when the objective space is wide (budget = gens × pop).

---

## P2 — Throughput (the path to WQ-scale mining)

### P2.1 — Parallelize the OOS admission evaluation  *(highest throughput leverage)*
- **Root cause:** `mine_into_oos` bypasses the process/thread executor whenever
  `oos_fraction>0` (`factory.cpp:366-368`); the admission loop (`factory.cpp:709`) runs
  single-threaded in the parent. ~Nx of the run is one core.
- **Fix:** extend the mine wire format to carry train+holdout panels (or eval on the
  full panel once and slice metrics in-parent), route OOS eval through
  `ProcessExecutor`/`DetPool`, keep only the order-sensitive `lib.admit()` serial.
- **Expected:** ~10-30× on a typical box.

### P2.2 — Eliminate the ~3× redundant per-candidate eval
- **Root cause:** each candidate is compiled+evaluated up to 3× — ranking
  `pool_aware_fitness(train)` (`factory.cpp:688`), report-only `metrics_on_panel(train)`
  (`factory.cpp:716`), and holdout (`factory.cpp:732-741`) — none reuse the SignalSet,
  though `fitness_core` accepts a precomputed `signals` arg (`fitness.cpp:230-244`) that
  the search loop already uses.
- **Fix:** drop / reuse the report-only train metrics; pass precomputed
  `signals`/`engine` into the admission evals; carry distinct genomes' streams out of
  search keyed on `canon_hash` so admission never recompiles an already-scored genome.
- **Expected:** 2-3× on the admission phase, compounding with P2.1 toward ~20-50×.

### P2.3 — Shared read-only panel + online rolling (after P2.1)
- Process path copies the whole panel into every shm segment
  (`workload_eval.cpp:266-279`) → put the panel in ONE shared mmap/shm segment → more
  workers fit per RAM. `Ts*` ops recompute over the full window per cell
  (`vm.hpp:714-754`) → add online/incremental rolling kernels for window-heavy genomes.

---

## P3 — Persistence & mega-alpha combination

### P3.1 — Make the library accumulate; load combine FROM it  *(unlocks mega alphas)*
- **Root cause:** `stage_discover.cpp:77-80` does `remove_all(lib_dir)` every run
  ("fresh library dir each run"); combine (`stage_combine.cpp:84-115`) enumerates loose
  `.dsl` files and never touches `library::Library`. No cross-run accumulation → the
  weak-signal thesis (many mediocre uncorrelated alphas) can't be realized.
- **Fix:** drop `remove_all`, point `lib_dir` at a stable `--library-dir` (admit()
  already dedups + journals, so accumulation is safe/idempotent); add a library-backed
  load path to `stage_combine` that builds the `AlphaStore` from all admitted records
  across seeds/runs, replacing the loose-`.dsl` enumeration.

### P3.2 — Drive the orphaned strong combiners
- **Root cause:** `RegimeCombiner`/`fit_regime_combiner`, `crowding::decorrelate_weights`,
  `conviction`, `CombinedSignalSource` are built + tested in the engine but referenced
  nowhere in `atx-impl` (`stage_combine.cpp` hand-rolls a static weighted sum of
  position rows, `:195-209`).
- **Fix:** wire `fit_regime_combiner` (regime labels already produced by `stage_regime`)
  and `decorrelate_weights` into `stage_combine`; assert
  `combo.weights.size() == streams.n_alphas()` and key the blend by `AlphaId`, not
  directory sort order (`stage_combine.cpp:194-208` is a latent silent-mismatch bug).
- **Note:** with only 2 inputs (one a constant sector tilt) shrinkage-MV collapses to
  ~50/50; this only pays off once P0/P3.1 populate the pool. Until then use `--method ic`
  and exclude constant-ish columns.

---

## Suggested execution order

1. **P0.1 + P0.2 + P0.3** together (≈half a day), then run ONE deep search and read
   `reject_hist`. Expectation: dozens of admits, no `zscore(sector)`-class junk.
2. If admits are now plentiful: **P1.1–P1.3** to raise quality/diversity, re-run.
3. **P2.1 + P2.2** to cut the run from ~1600s toward ~50-100s → enables many-seed
   sweeps per hour.
4. **P3.1 + P3.2** to accumulate across sweeps into a growing library and build real
   combined/mega alphas.
5. Merge the parked `feat/store-resumable-discover` commits (IS-Sharpe gate +
   RAM-aware worker cap) once P0/P2 land — they're complementary, not conflicting.

## Evidence map (file:line)

| Area | Key locations |
|------|---------------|
| Stagnation stop | `search_driver.cpp:282-288`, `:238-241`; `search_driver.hpp:173` |
| Diversity knobs | `search_driver.cpp:819-839` (immigrants), `:910-913` (jitter), `:296-317` (adaptive credit) |
| Pareto degeneracy | `pareto.hpp:208-264`; objectives in `search_driver.hpp:138-166` |
| Gate / reject buckets | `library.hpp:106-113`, `:319-344`; `config.hpp:49,58-61`; `gate.hpp:67-71` |
| Select/admit mismatch | `fitness.cpp:276,328`; `factory.cpp:688,716,732-767` |
| Sector degeneracy | `stage_discover.cpp:358-362`; `search_driver.cpp:398-399,827-829,921`; `registry.hpp:276-306`; `typecheck.cpp:327-393` |
| Throughput | `factory.cpp:366-368,709`; `fitness.cpp:230-244`; `workload_eval.cpp:266-279`; `vm.hpp:714-754` |
| Library / combine | `stage_discover.cpp:77-80`; `stage_combine.cpp:84-115,172,194-209`; `library/library.hpp`; `combine/regime_combiner.cpp`, `crowding.cpp`, `combined_source.hpp` |
