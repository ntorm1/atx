# Sprint 3 — Nonlinear & Regime-Aware Combination

**Goal:** retire the assumption that the runnable book's combination is a single **linear** blend by
wiring the already-built-and-tested `learn::StackingCombiner` (GBT / elastic-net *alpha-of-alphas* on
positions) and the point-in-time HMM regime posterior (`learn::regime_posterior_at` →
`combine::RegimeCombiner`) into `atx-impl`'s combine stage. Each pool alpha's per-(date,instrument)
POSITION becomes a meta-feature column, the label is the instrument's forward return, and a
deterministic histogram GBT captures the cross-alpha interactions a linear combiner structurally
cannot. The nonlinear stack is **admitted only if it beats the linear base OUT-OF-SAMPLE-AFTER-DSR**
on the existing purged-CPCV folds; otherwise the stage falls back to today's linear combiner. The
regime overlay partitions the combine fit by the PIT HMM posterior and blends per-name by
`P(regime | history ≤ t)`. All opt-in behind two appended `CombineMethod` enumerators
(`Stack` / `RegimeStack`) whose inert default is today's linear method; the no-flag path stays
byte-identical.

**Owns (exclusive):**
`atx-engine/include/atx/engine/learn/{ensemble,gbt,elastic_net,hmm,learned_source}.hpp`
(WIRING / config threading only — the estimation math in `src/learn/*.cpp` is FROZEN; S3 calls
`fit_stack`/`fit_gbt`/`fit_linear`/`baum_welch`/`regime_posterior_at`, it does not re-derive them),
`atx-engine/include/atx/engine/combine/regime_combiner.hpp` (wiring),
`atx-engine/include/atx/engine/combine/combiner.hpp` (APPEND `CombineMethod::Stack` /
`CombineMethod::RegimeStack` enumerators + `CombinerConfig` fields — append-only, frozen order),
`atx-impl/src/stage_combine.cpp` (method dispatch — S3 OWNS this file for p8),
`atx-impl/src/stage_regime.cpp` (regime stage — S3 OWNS this file for p8);
tests under `atx-engine/tests/learn/`, `atx-engine/tests/combine/`, and `atx-impl/tests/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); `src/learn/*.cpp` estimation
bodies (FROZEN — S3 calls `fit_stack`/`fit_gbt`/`fit_linear`/`baum_welch`, it does not re-implement
them); `alpha/oracle.hpp`; `risk/*`, `data/factor_model_artifact.hpp`, `atx-impl/src/stage_optimize.cpp`,
`atx-impl/src/stage_riskmodel.*` (Sprints 1 & 4 — S1 hands S3 a `cleaned_alpha_cov` accessor SEAM,
see S3-4); `fund/*`, `atx-impl/src/stage_metabook.*` (Sprint 2); the four hub files
`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` + `library/library.hpp` +
`factory/factory.cpp` + `factory/fitness.hpp` (Sprint 5).

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics. Do not
follow weaker patterns that expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering,
crash/recovery semantics, and tricky domain rules. Do not comment obvious assignments or wrap
every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## The orphan gap (verified file:line)

The runnable combine stage fits a **single linear blend**: `method_from_string`
(`stage_combine.cpp:50-69`) maps the `--method` string to one of five linear `CombineMethod`
enumerators, `combiner.fit(pool, fit_begin, fit_end)` (`:527`) produces one static weight vector, and
the walk-forward harness (`:804-821`) re-fits the SAME linear method per fold. The nonlinear
`StackingCombiner` (`learn::fit_stack`), the GBT/elastic-net bases, the meta-feature builder
(`learn::meta_features_from_pool`), and the PIT HMM posterior (`learn::regime_posterior_at` →
`combine::fit_regime_combiner` / `RegimeCombiner::blend`) are built, tested (200+ tests), and green —
but grepping `atx-impl/src` for every one of their load-bearing symbols returns **zero hits**.

| Gap | File:line | Evidence |
|---|---|---|
| Combine dispatch is linear-only | `stage_combine.cpp:50-69` | `method_from_string` maps `""`/`shrinkage-mv`/`equal`/`rank`/`ic`/`bounded` → the five linear `CombineMethod`s; no `stack`/`regime-stack` arm exists |
| One static linear weight vector | `stage_combine.cpp:496-527` | `combiner.cfg.method = cm; … combiner.fit(pool, fit_begin, fit_end)` — a single `AlphaCombiner::fit` (combiner.hpp:467) over the whole window |
| Walk-forward re-fits the same LINEAR method | `stage_combine.cpp:804-821` | `wf.cfg.method = cm_wf;` per fold — no nonlinear/regime arm |
| `StackingCombiner`/`fit_stack` absent from `atx-impl` | grep `atx-impl/src` for `fit_stack`/`meta_features_from_pool`/`StackCandidate` | zero hits — `learn/ensemble.hpp` (`fit_stack`, the §0.4 deflation-gated stacking gate) is dead weight in the runnable pipeline |
| PIT HMM posterior absent | grep `atx-impl/src` for `regime_posterior_at`/`baum_welch`/`fit_regime_combiner`/`RegimeCombiner` | zero hits — `learn/hmm.hpp` (log-space Baum-Welch + `regime_posterior_at`) + `combine/regime_combiner.hpp` never called |
| `stage_regime.cpp` is a MACRO-DATA loader, not an HMM path | `stage_regime.cpp:15-83` | `run_regime` loads FRED/CBOE macro CSVs (`vix`,`move`,`hy_oas`,…) into a regime-history artifact; it never fits an HMM or produces a per-date PIT posterior — the file name is misleading (see architecture note) |
| Combiner fits on a raw MLE covariance | `stage_combine.cpp:755` | `combine::detail::mle_covariance(centered, na)` — no shrinkage / no factor cleaning; S1 ships a `cleaned_alpha_cov` accessor that S3 consumes behind `RiskModelConfig.kind==Factor` (S3-4) |
| Forward-return labels never materialized | `stage_combine.cpp:474-493` | the pool is built from `extract_streams` PnL/positions; NO forward-return vector is assembled — `meta_features_from_pool` REQUIRES a `forward_returns_flat` label span, so S3-1 must build it from the panel |

---

## Architecture note — what "wire the stacking / regime combiner" actually means

The linear `AlphaCombiner` (combiner.hpp:459) fits weights in the **realized-PnL / mean-variance
plane**: `fit()` reads each alpha's PnL sub-span `[fit_begin, fit_end)` and solves a Ledoit-Wolf
shrunk MV (or one of the other four §5.3 methods). It combines *returns*. The stacking combiner
combines *positions*: `meta_features_from_pool` (ensemble.hpp:255) makes each pool alpha's
per-(date,instrument) POSITION one feature column of a meta-`FeatureMatrix`, the label `Y[h]` is the
instrument's forward return over horizon `h`, and `fit_stack` (ensemble.hpp:271) fits a NONLINEAR base
(a deterministic histogram GBT) and a LINEAR base (elastic-net) on the SAME meta-matrix, scores BOTH
by the SAME per-date out-of-fold IC / deflated Sharpe over the SAME CPCV folds, and returns a
`StackingVerdict` whose `admitted` flag is TRUE iff the nonlinear base beats the linear base
OOS-after-deflation (ensemble.hpp:265-269). This is the "combine on **positions**, not returns; one
model integrating sub-models" lever WorldQuant/RenTech both emphasize — and it is the honest-gate
realization: a linearly-combinable pool gives the GBT no OOS edge → reject; a genuine
nonlinear-interaction pool → the GBT's OOS IC beats linear AND survives deflation → admit.

S3 does **not** touch that math. S3's job is four thin seams inside the two owned deploy files:

1. **Append** two `CombineMethod` enumerators (`Stack`, `RegimeStack`) + `CombinerConfig` fields, all
   inert-default (S3-0). The enum order is frozen — `Stack`/`RegimeStack` are appended AFTER
   `BoundedRegression`, mirroring S1's `RiskModelKind::Diagonal == 0` discipline.
2. **Build the meta + fit the stack** in `stage_combine`'s dispatch (S3-1): materialize the
   forward-return label vector from the research panel, call `meta_features_from_pool` +
   `fit_stack`, and — when admitted — synthesize the combined weights the stage ships from
   `stack_to_candidate`'s deployed cross-section.
3. **Enforce the anti-overfit contract** (S3-2): stacking is admitted as the SHIPPED combiner only if
   `fit_stack`'s verdict admits on the existing purged-CPCV `eval/cpcv.hpp` folds; otherwise the stage
   falls back to the linear `AlphaCombiner::fit` result. Phase-D measured `oos_pbo = 0.79` (textbook
   overfit), so an ungated stacking step would make overfit WORSE — the DSR gate is load-bearing.
4. **Wire the PIT HMM regime posterior** into `regime_combiner` (S3-3): fit an HMM on the panel's
   regime observable, assign each date its PIT-argmax regime, partition the combine fit by regime via
   `fit_regime_combiner`, and blend per-name by `regime_posterior_at`. `CombineMethod::RegimeStack`.
   GUARDED (anti-roadmap: regime is a guarded optional overlay, NEVER the spine; inert default off).

The load-bearing input the linear path is missing is the **forward-return label**: the pool carries
PnL and positions but no forward returns, so the meta cannot be built until S3-1 assembles the label
vector from the panel (period-major, instrument-minor, length `n_periods*n_instruments`), reading only
rows STRICTLY inside the fit window for the fit and honoring the horizon offset (row `t`'s label is the
return realized over `[t, t+h)`, so the last `h` rows carry no label). No new estimator math — S3
assembles the label the built `meta_features_from_pool` already knows how to consume.

**On `stage_regime.cpp`:** the existing file is a macro-series *loader* (FRED/CBOE CSVs → a
regime-history artifact), not an HMM regime path. S3 OWNS it for p8 and EXTENDS it (append-only) to
also fit the PIT HMM and emit a per-date regime observable / posterior artifact the combine stage
consumes for `RegimeStack`. The existing macro-loader path is untouched and byte-identical when the
new HMM knob is off.

---

## Determinism contract (Sprint 3)

S3 follows the **p8 opt-in / default-byte-identical** contract (ROADMAP §Determinism). Every new
capability lives behind an appended `CombineMethod` enumerator / `CombinerConfig` field with an inert
default:

- `CombinerConfig.method` default stays `ShrinkageMv` (combiner.hpp:102) — `Stack`/`RegimeStack` are
  the opt-ins; `method_from_string("")`/`"shrinkage-mv"` still resolve to the linear path,
  byte-identical.
- `CombineMethod::Stack` / `CombineMethod::RegimeStack` are APPENDED after `BoundedRegression`
  (frozen order; `EqualWeight == 0` unchanged), so the `: atx::u8` layout of the existing five is
  untouched (an enum-layout pin test guards it — S3-0).
- Stacking is trained under **purged-CPCV** (`eval/cpcv.hpp`): the `fit_stack` OOF walk purges any
  train label overlapping a test label and embargoes the forward window, so no leakage — cited at the
  call site.
- The HMM posterior reads only `[0..t]`: `regime_posterior_at` (hmm.hpp:296) runs the forward filter
  over a COPIED causal prefix `obs[0..d]` and physically cannot touch a row `> d` (truncation-invariant,
  M2) — a PIT guard test perturbs future rows and asserts no change.
- The GBT is deterministic (fixed-iteration histogram, seeded subsample via
  `seed_for(master_seed,…)`, order-fixed reductions — gbt.hpp:22-38); `fit_stack`'s `verdict_hash`
  (ensemble.hpp:237-239) pins the byte-identical verdict for a fixed `(meta, regime, cfg)`.

**Four test classes per opt-in enumerator (mandatory):** (a) off-path byte-identity — default
`ShrinkageMv` (and `equal`/`rank`/`ic`/`bounded`), combine digest unchanged vs the pre-S3 pinned
combine golden; (b) on-path RED→GREEN — `Stack` on a constructed nonlinear-interaction fixture where
the stack provably admits and beats linear OOS, and a linearly-combinable fixture where it provably
does NOT admit (falls back); (c) twice-run — same panel → same combo bytes and same `verdict_hash`;
(d) seq==parallel — the per-fold OOF walk and the per-regime partition are order-independent.

---

## Dependency / wiring map

```
combine/combiner.hpp:90  ← S3-0 APPEND CombineMethod::Stack, CombineMethod::RegimeStack (after BoundedRegression)
combine/combiner.hpp:101 ← S3-0 CombinerConfig += {stack knobs, regime knobs} (inert defaults)
                            (StackingCfg / GbtCfg / HmmCfg live in learn/*; combiner.hpp holds only
                             the thin toggle + a nested cfg the stage translates to StackingCfg)
learn/ensemble.hpp:255 meta_features_from_pool  ← S3-1 stage builds the meta from pool positions + fwd-return labels
learn/ensemble.hpp:271 fit_stack                ← S3-1/S3-2 the deflation-gated stacking gate (StackingVerdict)
learn/ensemble.hpp:318 stack_to_candidate       ← S3-1 synthesize the deployed cross-section → combined weights
learn/gbt.hpp:fit_gbt / learn/elastic_net.hpp   ← called BY fit_stack (frozen; S3 only threads cfg)
eval/cpcv.hpp:CpcvConfig / cpcv_folds           ← S3-2 the purged-CPCV folds fit_stack scores OOS-after-DSR on
atx-impl/stage_combine.cpp:50-69 method_from_string ← S3-0 add "stack"/"regime-stack" string arms
atx-impl/stage_combine.cpp:495-527 dispatch      ← S3-1 branch: Stack → build meta + fit_stack + fallback
learn/hmm.hpp:baum_welch / regime_posterior_at   ← S3-3 fit HMM on the regime observable, PIT posterior per date
combine/regime_combiner.hpp:fit_regime_combiner  ← S3-3 partition the combine fit by PIT-argmax regime
combine/regime_combiner.hpp:RegimeCombiner::blend← S3-3 blend per-name by P(regime | history ≤ t)
atx-impl/stage_regime.cpp:15 run_regime          ← S3-3 EXTEND (append-only) to also emit the HMM observable/posterior
S1 seam: FactorModelArtifact::cleaned_alpha_cov  ← S3-4 combine reads it when RiskModelConfig.kind==Factor
atx-impl/stage_combine.cpp:755 mle_covariance    ← S3-4 retire behind the flag (cleaned cov when Factor)
tests/learn/stack_wire_test.cpp                  ← S3-1/S3-2 (auto-globbed)
tests/combine/regime_stack_wire_test.cpp         ← S3-3
atx-impl/tests/stage_combine_stack_test.cpp      ← S3-1/S3-2/S3-4
atx-impl/tests/stage_combine_regime_test.cpp     ← S3-3
```

---

## Tasks

### S3-0 — Open ledger + append `CombineMethod::Stack`/`RegimeStack` + `CombinerConfig` fields (do first; all units depend on this)

**Goal:** create the sprint ledger (marker commit); append the two new `CombineMethod` enumerators
(frozen order) and the inert-default `CombinerConfig` fields every downstream unit reads. No behavior
change — the enumerators exist, nothing dispatches to them non-inertly yet.

**Root cause:** `CombineMethod` (combiner.hpp:90-96) has exactly five enumerators and the
`AlphaCombiner::fit` switch is EXHAUSTIVE (no default — combiner.hpp:492, "a new enumerator is a
compile error"). Appending `Stack`/`RegimeStack` there is a deliberate compile-break that forces every
switch to acknowledge the new arms; the stage-level dispatch (not `AlphaCombiner::fit`) implements
them, so `AlphaCombiner::fit` gets an explicit `Err`/`unreachable` arm for the two new values (they are
never fit through `AlphaCombiner` — the stage routes them to `fit_stack`/`RegimeCombiner`).

**Wiring:**
- `combiner.hpp:90` — append AFTER `BoundedRegression` (frozen order; do NOT reorder or renumber):
  ```cpp
  enum class CombineMethod : atx::u8 {
    EqualWeight,       // 0 — frozen
    RankAverage,       // 1
    IcWeighted,        // 2
    ShrinkageMv,       // 3 — default (inert)
    BoundedRegression, // 4
    Stack,             // 5 — NEW (S3): nonlinear alpha-of-alphas, stage-dispatched (not via AlphaCombiner::fit)
    RegimeStack,       // 6 — NEW (S3): PIT-HMM regime-conditional stack, stage-dispatched
  };
  ```
- `combiner.hpp:492` — the exhaustive `AlphaCombiner::fit` switch adds `Stack`/`RegimeStack` arms that
  return `Err(InvalidArgument, "Stack/RegimeStack are stage-dispatched, not fit via AlphaCombiner")`.
  This keeps the no-default guard intact WITHOUT re-implementing the stack inside `AlphaCombiner`.
- `combiner.hpp:101` — extend `CombinerConfig` (append at struct end — no aggregate-init breakage):
  ```cpp
  // S3 stacking / regime knobs (all inert unless method is Stack/RegimeStack).
  atx::u64 stack_master_seed = 0;      // determinism root threaded into StackingCfg/GbtCfg/HmmCfg
  atx::u32 stack_cpcv_groups = 6;      // eval::CpcvConfig.n_groups for the OOS-after-DSR gate
  atx::u32 stack_cpcv_test_groups = 2; // eval::CpcvConfig.n_test_groups
  atx::f64 stack_cpcv_embargo = 0.01;  // embargo fraction (forward-leakage guard)
  atx::u16 stack_horizon = 1;          // forward-return label horizon (bars)
  atx::u32 regime_n_states = 3;        // HmmCfg.n_states for RegimeStack (guarded overlay)
  ```
  These are the ONLY combine-side cfg; the stage translates them to the learn-domain `StackingCfg` /
  `HmmCfg` (which stay in `learn/*`, not duplicated into `combiner.hpp`).

**Determinism:** pure addition. `EqualWeight == 0` and the existing five indices are unchanged; the
appended values take 5/6. Existing combine/report goldens unchanged (nothing dispatches to `Stack`/
`RegimeStack` yet; `method_from_string` still rejects unknown strings).

**Accept:**
- Project compiles (debug + release), all existing `combine_*`, `stage_combine_*`, `regime_*` suites
  green (the `AlphaCombiner::fit` switch now compiles WITH the two new no-op arms).
- `combine_method_enum_layout_pin` (new `tests/combine/`): `static_assert`/`ATS_TEST` pins
  `EqualWeight==0`, `ShrinkageMv==3`, `BoundedRegression==4`, `Stack==5`, `RegimeStack==6` and
  `sizeof(CombineMethod)==1` — the frozen-order guard (mirrors S1's `RiskModelKind::Diagonal==0` pin).
- `CombinerConfig` default-constructs to the inert values; a test pins `method==ShrinkageMv` default.

---

### S3-1 — Wire `fit_stack` as `CombineMethod::Stack` in the `stage_combine` dispatch

**Goal:** implement the `Stack` arm end-to-end in the owned `stage_combine.cpp`: materialize the
forward-return label vector from the research panel, build the meta-`FeatureMatrix` via
`meta_features_from_pool`, fit the deflation-gated stack via `fit_stack`, and — when the verdict admits
— produce the combined weights the stage ships from `stack_to_candidate`'s deployed cross-section. This
unit produces the weights; S3-2 adds the admission-vs-fallback contract; both are in the same file so
they land as a tight sequence.

**Root cause:** the dispatch at `stage_combine.cpp:495-527` only knows `AlphaCombiner::fit` (linear).
The pool carries PnL + positions (`stage_combine.cpp:474-493`, `pool.insert(pnl, pos_flat, m)`) but NO
forward-return labels — `meta_features_from_pool` (ensemble.hpp:255) REQUIRES a `forward_returns_flat`
span (period-major, instrument-minor). Nothing assembles it and nothing calls `fit_stack` from
`atx-impl` (grep → zero hits).

**Wiring:**
- Add `"stack"` to `method_from_string` (`stage_combine.cpp:50-69`) → `CombineMethod::Stack`.
- After the pool is built (`stage_combine.cpp:493`) and BEFORE the linear `combiner.fit`
  (`:527`), branch on `cm == Stack`:
  ```cpp
  // S3-1: nonlinear alpha-of-alphas. Meta features = per-alpha positions (from the
  // pool), label = instrument forward return over cfg.stack_horizon, computed from the
  // research panel reading ONLY rows in the fit window (PIT). The frozen fit_stack fits
  // a GBT + elastic-net on the SAME meta / SAME CPCV folds and returns a deflation-gated
  // verdict. (exact line TBD by implementer — after pool.insert, before combiner.fit.)
  if (cm == combine::CombineMethod::Stack || cm == combine::CombineMethod::RegimeStack) {
      const std::vector<atx::f64> fwd = build_forward_returns(streams, cfg.stack_horizon); // period-major
      const std::array<atx::u16,1> horizons{cfg.stack_horizon};
      const learn::FeatureMatrix meta =
          learn::meta_features_from_pool(pool, fwd, std::span{horizons});
      learn::StackingCfg scfg = stacking_cfg_from(cfg); // seed + CpcvConfig + horizons
      const learn::StackingVerdict v = learn::fit_stack(meta, /*regime=*/nullptr, scfg); // S3-3: regime for RegimeStack
      // S3-2 decides admit vs fallback; on admit, synthesize the shipped weights:
      const learn::StackCandidate sc = learn::stack_to_candidate(v, meta, scfg);
      // combo.weights ← per-alpha weights implied by the deployed cross-section (see below).
  }
  ```
- **Combined weights from a stack.** `fit_stack`/`stack_to_candidate` produce a per-(date,instrument)
  predicted POSITION stream, not a per-alpha weight vector. The combine stage's downstream artifact is
  a per-alpha weight vector applied to the pool. Bridge: the shipped `combo.weights` for a `Stack`
  method are the pool-alpha loadings that best reproduce the deployed stack cross-section over the fit
  window (an order-fixed least-squares projection of the stack's position stream onto the pool's
  per-alpha position streams), so the existing `blend_window_sharpe` / report path stays unchanged. The
  exact projection is a deterministic normal-equation solve reusing `combine::detail` helpers (no new
  estimator math); document it at the call site. (Alternatively, if the stage's downstream can consume
  a synthesized position stream directly, ship `sc.pos_flat` — implementer picks the lower-risk seam;
  the projection is the conservative default that keeps the artifact shape identical.)
- `build_forward_returns(streams, h)`: for each `(t, inst)`, the return realized over `[t, t+h)` from
  the panel's close series; rows `t > n_periods - h` carry NaN (no label — dropped by `row_valid`).
  Reads only close prices at `t` and `t+h`; the fit window's labels never read past `fit_end`.

**Determinism:** `meta_features_from_pool` is PURE (ensemble.hpp:252, "PURE / deterministic"); the
label vector is an order-fixed walk over `(period, instrument)`; `fit_stack` is deterministic (M1,
`verdict_hash`). The projection solve is order-fixed (ascending alpha, `solve_spd`). seq==parallel:
each fold's OOF fit is independent.

**Accept:**
- `stack_meta_from_positions` (new `tests/learn/`): on a fixture pool + a hand-built forward-return
  label, `meta_features_from_pool` yields a meta whose column `f` equals pool alpha `f`'s position at
  each valid `(date,inst)` cell and whose `Y[0]` equals the supplied forward return (train/eval parity).
- `stage_combine_stack_produces_weights` (new `atx-impl/tests/`): `--method stack` on a fixture panel
  yields a well-formed combo (Σ|w|≈1, finite, length == pool.size()) and a stable `verdict_hash`.
- PIT guard: the forward-return label at `t < fit_end - h` does not change when panel rows `≥ fit_end`
  are perturbed (no look-ahead through the label).
- Twice-run: same panel → same combo bytes + same `verdict_hash`.

---

### S3-2 — The anti-overfit contract: admit stacking ONLY if it beats linear OOS-after-DSR

**Goal:** make stacking the SHIPPED combiner only when `fit_stack`'s verdict admits on the existing
purged-CPCV folds (nonlinear OOS IC > linear OOS IC AND nonlinear OOS DSR > 0); otherwise the stage
falls back to today's linear `AlphaCombiner::fit` result. This is the load-bearing honesty guard.

**Root cause:** Phase-D measured `oos_pbo = 0.79` (textbook overfit). A GBT is the highest-variance
learner in the codebase (gbt.hpp:63-65, "GBT overfits hardest") — an *ungated* stacking step would make
the overfit worse, not better. `fit_stack` already computes the honest same-fold same-metric verdict
(ensemble.hpp:31-70, §0.4 amendment) — the gap is that `atx-impl` never READS the verdict to decide
admit-vs-fallback.

**Wiring:**
- After `fit_stack` returns (S3-1's branch), gate on `v.admitted`:
  ```cpp
  // S3-2: the honest-gate. fit_stack scored the nonlinear base AND a linear base on
  // the SAME purged-CPCV folds (eval/cpcv.hpp) and the SAME oos_deflated_sharpe/oos_ic.
  // ADMIT the stack as the shipped combiner ONLY if it beats linear OOS-after-deflation;
  // else fall back to the linear AlphaCombiner fit (byte-identical to today's shrinkage-mv
  // book). Phase-D oos_pbo=0.79 — an ungated GBT combiner makes overfit worse, so this
  // gate is load-bearing, not advisory.
  if (v.admitted) {
      combo = weights_from_stack(v, meta, scfg);   // S3-1 projection
  } else {
      ATX_TRY(combo, combiner.fit(pool, fit_begin, fit_end)); // linear fallback (today's book)
  }
  ```
- Record `v.oos_dsr_nonlinear` / `v.oos_dsr_linear` / `v.oos_ic_nonlinear` / `v.oos_ic_linear` /
  `v.admitted` in the stage `kvs` telemetry (like the existing breadth telemetry at
  `stage_combine.cpp:737-768` — kvs-only, never folded into `combo.bin`/the hashed digest), so V1's
  scorecard can read the linear-vs-stacking comparison the ROADMAP's "Strategic decisions" fork needs.
- The `CpcvConfig` fed to `fit_stack` is built from the S3-0 cfg fields
  (`stack_cpcv_groups`/`test_groups`/`embargo`) — the SAME purged-CPCV `eval/cpcv.hpp` the engine
  already ships (López de Prado AFML Ch. 7; purge + embargo cited at the call site).

**Determinism:** the gate is a pure comparison of deterministic verdict scalars. On the fallback path
the shipped `combo` is exactly the linear `AlphaCombiner::fit` result — byte-identical to today's book
for that method. On the admit path the weights derive from the deterministic stack.

**Accept:**
- `stack_admits_on_interaction_fixture` (new `atx-impl/tests/`): a constructed pool whose optimal
  combination is a genuine cross-alpha INTERACTION (e.g. `sign(a₁)·a₂`, unlearnable by a linear blend)
  → `v.admitted == true`, `v.oos_ic_nonlinear > v.oos_ic_linear`, and the shipped combo is the stack.
- `stack_rejects_on_linear_fixture` (new `atx-impl/tests/`): a pool whose optimal combination is a
  linear sum → `v.admitted == false`, and the shipped combo is BYTE-IDENTICAL to the linear
  `--method shrinkage-mv` combo (the fallback proves no silent overfit).
- `stack_gate_purges_cpcv`: a fixture where an un-purged fold would leak an overlapping label inflates
  OOS IC; with purge+embargo on, the leaked edge disappears (the gate cannot be fooled by leakage).
- Twice-run + seq==parallel on the admit path.

---

### S3-3 — PIT HMM regime posterior → regime-conditional combine (`CombineMethod::RegimeStack`)

**Goal:** wire the point-in-time HMM posterior into `regime_combiner`: fit an HMM on the panel's
regime observable, assign each date its PIT-argmax regime, partition the combine fit by regime via
`fit_regime_combiner`, and blend per-name by `P(regime | history ≤ t)` from `regime_posterior_at`.
Exposed as `CombineMethod::RegimeStack`. GUARDED — anti-roadmap: regime conditioning is a guarded
optional overlay, NEVER the spine; inert default off.

**Root cause:** `regime_combiner.hpp` today combines per regime but its `fit_regime_combiner` takes
caller-supplied `regime_labels` and `blend` takes a caller-supplied `posterior` (regime_combiner.hpp:86,
107) — the HMM dependency "lives only at the CALL SITE" (regime_combiner.hpp:20-21). No call site in
`atx-impl` supplies either from the built HMM: `stage_regime.cpp` loads macro CSVs but never fits an
HMM (`stage_regime.cpp:15-83`), and `stage_combine.cpp` never calls `fit_regime_combiner`/`blend`. The
single-alpha-capacity findings show the low-vol premium is REGIME-DEPENDENT (BAB flipped sign
in-sample), which is exactly what a regime-conditional combine is for — but only as a guarded overlay,
never the primary book.

**Wiring:**
- EXTEND `stage_regime.cpp` (append-only; the macro-loader path untouched): when a new
  `cfg.regime_hmm` knob is on, fit `baum_welch` (hmm.hpp:252) on the regime observable (the panel's
  cross-sectional regime marker series, or the loaded macro series when present), and emit a per-date
  regime observable matrix `obs` + the fitted `Hmm` as an artifact the combine stage reads. The
  observable is built in fixed date order (no map); the HMM fit is deterministic (M1, seeded init).
- In `stage_combine.cpp`'s `RegimeStack` branch (S3-1): assign each date its PIT-argmax regime label
  via `regime_posterior_at(hmm, obs, d)` → argmax (lower index wins ties), pass the labels to
  `fit_regime_combiner(pool, labels, n_regimes, fit_begin, fit_end, cfg)`
  (regime_combiner.hpp:107), then at each apply date blend the per-regime weights by
  `regime_posterior_at(hmm, obs, t)` via `RegimeCombiner::blend` (regime_combiner.hpp:86). For the
  nonlinear arm, `fit_stack` takes the fitted `Hmm*` directly (ensemble.hpp:271, the `regime != nullptr`
  path) so the stack is fit PER REGIME (ensemble.hpp:225-233) — `RegimeStack` = stack + regime.
- The single-regime fallback is byte-identical: with `n_regimes == 1` (all labels equal),
  `fit_regime_combiner`'s single combo equals `AlphaCombiner::fit` over the full window
  (regime_combiner.hpp:100-101) — so `RegimeStack` with a degenerate one-state HMM ships today's book.

**Determinism (inert default):** `method != RegimeStack` ⇒ no HMM fit, no regime partition ⇒
byte-identical. On-path: `baum_welch` is seeded/order-fixed (hmm.hpp:19-31); `regime_posterior_at`
reads only `[0..d]` (M2, truncation-invariant); `blend` sums ascending regime then ascending alpha
(regime_combiner.hpp:80). seq==parallel: each regime partition is an independent fit.

**Accept:**
- `regime_posterior_pit_guard` (new `tests/combine/`): `regime_posterior_at(hmm, obs, d)` is
  byte-identical whether `obs` has `d+1` rows or many more (perturb rows `> d`, assert no change) — the
  PIT firewall, pinned.
- `regime_stack_single_state_byte_identical` (new `atx-impl/tests/`): a one-state HMM (n_regimes==1)
  ⇒ the `RegimeStack` combo is byte-identical to the corresponding non-regime combo (the critical
  fallback guard, regime_combiner.hpp:25-31).
- `regime_combine_partitions_by_posterior` (new `tests/combine/`): on a two-regime fixture where the
  optimal blend flips sign by regime (mirrors BAB flipping sign), the PIT-posterior-blended book tracks
  the regime-appropriate combo per date, and its OOS score beats a single flat fit.
- Twice-run + seq==parallel on the regime path.

---

### S3-4 — Sprint-1 seam: consume the cleaned factor covariance for the weight fit

**Goal:** consume the S1-shipped `cleaned_alpha_cov` accessor for the linear / shrinkage weight-fit
(and the stack's fallback path) when `RiskModelConfig.kind == Factor`, retiring the raw
`mle_covariance` at `stage_combine.cpp:755` behind the flag. This closes the S1→S3 seam the ROADMAP
ownership matrix assigns to S3.

**Root cause:** `stage_combine.cpp:755` fits/instruments on
`combine::detail::mle_covariance(centered, na)` — an unshrunk MLE covariance that is noise-dominated
when `N ≈ T` (Phase-D's exact regime, where `N_eff` collapsed to 8.76). S1 ships a pure
`FactorModelArtifact::cleaned_alpha_cov(...)` accessor (shrunk / RMT-eigen-clipped, no new math) and
records the seam handoff in its ledger (S1-3) — but S1 does NOT edit `stage_combine.cpp` (S1's Out of
Scope explicitly hands this file to S3). S3 owns the file, so S3 threads the call.

**Wiring:**
- Behind `cfg.risk_model.kind == RiskModelKind::Factor`, replace the `mle_covariance` call at
  `stage_combine.cpp:755` (the breadth-instrumentation covariance) AND the ShrinkageMv weight-fit's
  covariance source with `FactorModelArtifact::cleaned_alpha_cov(...)` over the same fit window:
  ```cpp
  // S3-4 (S1 seam): when the factor risk model is active, fit/instrument on the CLEANED
  // (shrunk + RMT-eigen-clipped) alpha covariance from S1's artifact instead of the raw
  // MLE covariance. Default (kind==Diagonal) => mle_covariance, byte-identical to today.
  const MatX cov = (cfg.risk_model.kind == risk::RiskModelKind::Factor)
      ? artifact.cleaned_alpha_cov(fit_begin, fit_end)   // S1-3 pure accessor
      : combine::detail::mle_covariance(centered, na);   // today's raw MLE (default)
  ```
- The accessor is READ-ONLY and pure (S1's contract); S3 only threads the call behind the inert flag.
  When the artifact is absent / `kind==Diagonal`, the `mle_covariance` path is byte-identical to today.

**Determinism (inert default):** `kind == Diagonal` (the p8 default) ⇒ the `mle_covariance` path is
unchanged ⇒ combine digest byte-identical. On-path: `cleaned_alpha_cov` is a deterministic shrinkage +
eigen-clip (S1's contract).

**Accept:**
- `combine_cleaned_cov_off_path` (new `atx-impl/tests/`): default `RiskModelConfig{kind=Diagonal}` ⇒
  the combine digest is unchanged vs the pre-S3 pinned golden (the seam is inert off-path).
- `combine_cleaned_cov_shrinks` (new `atx-impl/tests/`): with `kind==Factor` on an `N≈T` fixture where
  the raw MLE covariance has a spurious large eigenvalue, the cleaned-cov weight fit is provably more
  diversified (lower max weight, higher `effective_n`) than the raw-MLE fit.
- Ledger records the S1 seam consumption explicitly (which accessor, where, behind which flag) —
  closing the handoff S1-3 opened.

**Dependency note:** S3-4 hard-depends on S1-3 having shipped `cleaned_alpha_cov`. If S1 has NOT
landed at S3 kickoff, S3-4 is DEFERRED (documented in the ledger) and the `mle_covariance` path ships
unchanged — S3-1/2/3 do not depend on it (the stack fits on the meta, not the alpha covariance).

---

### S3-5 — Off-path byte-identity + twice-run + seq==parallel + enum-layout pin (the determinism battery)

**Goal:** the consolidated determinism gate for the whole sprint — prove the default linear path is
byte-identical, both opt-ins are reproducible run-to-run and seq==parallel, and the enum layout is
pinned. This is the sprint's honesty floor (mirrors the p8 four-test-classes contract).

**Root cause:** each opt-in unit ships its own RED→GREEN, but the sprint needs ONE place that proves
the aggregate contract: (a) the five legacy `CombineMethod`s produce the byte-identical pre-S3 combine
golden; (b) `Stack`/`RegimeStack` are twice-run reproducible; (c) the OOF walk / regime partition are
seq==parallel; (d) the appended enum indices are frozen.

**Wiring:** no new engine code — this unit is tests + the ledger row. It reuses the fixtures from
S3-1/2/3 and the pinned combine golden.

**Determinism:** N/A (this IS the determinism proof).

**Accept:**
- `combine_default_byte_identical` (new `atx-impl/tests/`): each of
  `""`/`shrinkage-mv`/`equal`/`rank`/`ic`/`bounded` produces the byte-identical pre-S3 combine digest
  (off-path byte-identity — the five legacy methods are untouched).
- `stack_twice_run` / `regime_stack_twice_run`: same panel → same combo bytes + same `verdict_hash`.
- `stack_seq_eq_parallel` / `regime_seq_eq_parallel`: the combo is byte-identical whether the OOF walk
  / regime partition runs sequentially or across the DetPool (each fold/partition is independent).
- `combine_method_enum_layout_pin` (re-asserted from S3-0): `Stack==5`, `RegimeStack==6`,
  `sizeof==1` — the frozen-order guard.
- The `FactoryOos.MineIntoOffPathDigestUnchanged` / combine goldens are unchanged with default cfg.

---

## Sequencing

1. **S3-0 first** (append enumerators + cfg fields + ledger marker) — every unit reads the new
   `CombineMethod` / `CombinerConfig` surface; the `AlphaCombiner::fit` exhaustive switch must compile
   with the two new arms before anything else builds.
2. **S3-1** (build the meta + fit the stack) — the producer; S3-2 gates it, S3-5 tests it.
3. **S3-2** immediately after S3-1 (same file `stage_combine.cpp`, tight sequence — NOT parallel):
   S3-2 adds the admit-vs-fallback gate around S3-1's `fit_stack` call.
4. **S3-3** after S3-1 (touches `stage_regime.cpp` + the `RegimeStack` branch of `stage_combine.cpp`;
   reuses S3-1's meta builder for the per-regime stack) — the guarded overlay.
5. **S3-4** after S1-3 lands (hard dependency on `cleaned_alpha_cov`); DEFERRED with a ledger note if
   S1 has not merged at S3 kickoff. Disjoint from S3-1/2/3 in intent but same file — land after S3-2.
6. **S3-5 last** — the consolidated determinism battery over all opt-ins + the enum pin.

**Intra-file ordering (binding):** S3-1 → S3-2 → S3-4 all edit `stage_combine.cpp`; S3-3 edits both
`stage_regime.cpp` and `stage_combine.cpp`. These are ONE owned file each — the units are SEQUENTIAL,
never parallel sub-agents, because parallel sub-agents cannot see each other's commits (per sprint.md).

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| `fit_stack` / `meta_features_from_pool` signature differs from assumed | S3-1 won't compile | Read `ensemble.hpp:255,271,318` decls at kickoff; assemble to the ACTUAL signatures (`meta_features_from_pool(pool, forward_returns_flat, horizons)`; `fit_stack(meta, const Hmm*, StackingCfg)`). Confirmed present in the header. |
| Stack→per-alpha-weight bridge is lossy or non-deterministic | Shipped book differs from the scored stack; determinism broken | Ship the conservative order-fixed projection (S3-1) OR the synthesized `sc.pos_flat` directly if the downstream can consume a position stream; pin it with the twice-run test. Implementer picks the lower-risk seam and documents it. |
| Ungated stacking overfits (Phase-D pbo=0.79) | Every reported stacking Sharpe is inflated; the honest null is lost | S3-2's admit-vs-fallback gate is MANDATORY and load-bearing: stack ships ONLY if `v.admitted` on purged-CPCV. `stack_rejects_on_linear_fixture` proves the fallback fires. No `Stack` ships without the gate. |
| Forward-return label leaks future data | Silent look-ahead inflates the meta's OOS IC | The label at `t` reads panel close at `t` and `t+h` only; the FIT reads only `[fit_begin, fit_end)` rows; rows `> fit_end - h` carry NaN. The PIT guard test (S3-1) perturbs future rows and asserts no change. |
| HMM regime becomes the spine (anti-roadmap violation) | Book depends on a guarded overlay as the primary path | `RegimeStack` is inert-default OFF; the single-state fallback is byte-identical (S3-3). The ledger records regime as a GUARDED overlay per the ROADMAP anti-roadmap constraint #6. |
| `regime_posterior_at` reads the future | PIT firewall broken; every regime number dishonest | `regime_posterior_at` copies `obs[0..d]` into a fresh prefix and runs the forward pass over THAT (hmm.hpp:302-310) — structurally cannot touch row `> d`. `regime_posterior_pit_guard` (S3-3) pins it. |
| S1's `cleaned_alpha_cov` not landed at S3 kickoff | S3-4 won't compile | S3-4 is gated on S1-3; DEFER with a ledger note and ship `mle_covariance` unchanged (S3-1/2/3 do not depend on it). Confirmed disjoint dependency in the ROADMAP ownership matrix. |
| `stage_regime.cpp` name implies HMM but is a macro loader | Wrong file edited / duplicated HMM path | S3-3 EXTENDS the existing macro-loader append-only; the macro path is untouched and byte-identical when `regime_hmm` is off. Documented in the architecture note + ledger. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** the pinned `stage_combine`/`stage_report` goldens and
  `FactoryOos.MineIntoOffPathDigestUnchanged` unchanged with default `CombinerConfig`
  (`method==ShrinkageMv`) and all five legacy `--method` strings.
- **Per-task RED→GREEN:** each opt-in has a test RED before the wire and GREEN after (S3-1 stack
  produces weights; S3-2 admits on interaction / rejects on linear; S3-3 regime partitions + PIT
  guard; S3-4 cleaned-cov shrinks).
- **Stacking win, measured:** on the nonlinear-interaction fixture, record
  `{oos_ic_linear, oos_ic_nonlinear, oos_dsr_linear, oos_dsr_nonlinear, admitted}` — the stack must
  show `oos_ic_nonlinear > oos_ic_linear` AND `oos_dsr_nonlinear > 0` (the concrete, quantified S3
  claim). On the linear fixture, record that it does NOT admit (the honest null).
- **Regime win, measured:** on the two-regime sign-flip fixture, record the PIT-posterior-blended OOS
  score vs the single-flat-fit OOS score — the regime-conditional book must beat the flat fit.
- **Twice-run + seq==parallel** on both opt-in paths (`verdict_hash` stable; combo bytes identical).
- **Dev-panel smoke ≤5 min** with `--method stack` and `--method regime-stack` (the CLI flag threading
  is Sprint 5's; S3 proves the engine path via a direct-call integration test, not the CLI).

---

## Out of scope

- CLI flag `--method stack` / `--method regime-stack` threading through the hub + `--regime-hmm` —
  Sprint 5 (owns the four hub files). S3 proves the engine path via direct-call integration tests.
- Editing `risk/*` / `data/factor_model_artifact.hpp` / `stage_riskmodel.*` to PRODUCE
  `cleaned_alpha_cov` — Sprint 1 (owns the accessor); S3 only CONSUMES it (S3-4).
- The `fund::MetaAllocator` / `MetaBook` sleeve layer — Sprint 2 (`stage_metabook.*`); the stack
  combines a single pool, not sleeves.
- Impact-in-selection / capacity / the cap-clip-renorm dollar-neutrality fix — Sprint 4.
- Re-deriving any estimation math in `src/learn/*.cpp` (`fit_gbt` / `fit_linear` / `baum_welch` /
  `regime_posterior_at`) — FROZEN; S3 calls it.
- Making the stacking / regime combiner the production DEFAULT — resolved at V1 by the OOS-after-DSR
  comparison S3-2 records (ROADMAP "Strategic decisions"); S3 ships it as an opt-in, linear stays
  default.

---

## Future-work STRETCH (roadmap-only; NOT in the S3 critical path — anti-roadmap)

- **Meta-labeling (López de Prado): triple-barrier events + a secondary classifier that sizes/filters
  the primary stacked signal + sample-uniqueness (average-uniqueness) weighting on the CPCV folds.**
  This is a genuinely greenfield SECOND-STAGE build (a new estimator + a new label geometry), NOT
  wiring — it belongs to a successor module per the ROADMAP anti-roadmap constraint #7 ("no
  meta-labeling in the critical path"). S3 ships the linear-vs-stacking + regime overlay first,
  honestly measured; meta-labeling is a stretch only if S3-0..S3-5 land with margin. Noted here so the
  next strategist sees the natural successor to the position-stacking gate.
