# Sprint S5 — Learned Signals & ML Combiner (sprint spec)

**Status:** ✅ CLOSED on `feat/atx-core-stdlib` (unmerged) — 8 units S5-0…S5-7 shipped, 1025/1025 engine tests, /WX clean. Implemented per [`sprint-5-learned-signals-ml-combiner-implementation-plan.md`](sprint-5-learned-signals-ml-combiner-implementation-plan.md) (§0 as-built amendments override this spec on conflict); see the [ledger](sprint-5-progress.md) for per-unit status + the as-built reconciliation. Depends on **S1** (deflated admission gate), **S2** (parallel training),
**S4** (feature/constituent source), **P4** (linear combiner baseline).
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§1 (no-theory statistical modeling; "do the simple things right" — simple regression at the core), §2
(HMM/Baum-Welch heritage; noisy-channel), §3 (the combination crown jewel: shrinkage → factor → regularized
regression → ML ensembles, with walk-forward + correlation/turnover budgets).

---

## Why this sprint

This is the **RenTech direction**. WorldQuant's formulaic alphas (S3) are one half of the proven thesis; the
other half is *learned* signals — statistical models that recover a faint predictable component from noisy
data **without an economic story**. RenTech's lineage is speech-recognition + cryptanalysis (HMM,
noisy-channel), and Patterson's tell is that **simple regression, done right, was the most important tool**.

Crucially, the p0 anti-roadmap *banned* ML mining — "ML overfits at this N." S5 is allowed **now** precisely
because **S1 exists**: every learned model is trained inside S1's purged CPCV and admitted only through the
DSR/PBO gate. The discipline that makes ML safe is built; S5 uses it.

A learned model is **just another alpha**: it emits a cross-section per date and plugs into the pool via the
existing `ISignalSource` seam. And the **mega-combiner** climbs past P4's linear rungs into nonlinear
ensembling and regime-conditional weighting — the layer the research calls the crown jewel.

---

## Scope — units

### S5.0 — Marker + ledger
Open `sprint-5-progress.md`, freeze scope, base SHA. Note per-unit atx-core numeric requests.

### S5.1 — PIT feature-matrix builder
Turn DSL signal outputs (from the S4 library / S3 mining) **plus** raw panel fields into a **point-in-time
feature tensor** (date × instrument × feature) suitable for model training. Strict no-look-ahead (features
at date `t` read only ≤ t sealed rows — reuses the Phase-3 causal panel discipline). Deterministic,
order-fixed. This is the input every learned model consumes.

### S5.2 — Linear learned alphas
**Ridge / lasso / elastic-net** cross-sectional regressions, **trained walk-forward inside S1's CPCV**, that
emit a cross-section → wrapped as an `ISignalSource` and admitted to the pool like any alpha. "Do the simple
things right" — this is the RenTech-core baseline and likely the highest value-per-complexity unit.
*atx-core (Pattern B): an L7 coordinate-descent / L-BFGS solver for the elastic-net penalty.*

### S5.3 — Gradient-boosted-tree learned alpha
A **deterministic** GBT model (fixed histogram splits, seeded, no time-dependent tie-breaking) as a
nonlinear learned alpha, trained inside CPCV, emitting a cross-section. Heaviest regularization + the DSR/PBO
gate — GBT overfits hardest, so admission is strictest. *atx-core (Pattern B): a deterministic GBT primitive
(histogram-based split finder).*

### S5.4 — HMM regime detector + regime-conditional combination
**Baum-Welch / forward-backward** (log-space) HMM over market-state observables to infer latent regimes
(quiet mean-reversion / trending / stressed — RenTech's explicit heritage). Use the inferred (PIT,
fit-on-trailing) regime posterior to make the combiner **regime-conditional** (different combination weights
per regime). *atx-core (Pattern B): an L7 HMM / forward-backward kernel.*

### S5.5 — Nonlinear ensemble mega-combiner + close
Climb past P4's linear rungs: a **stacking / ensemble combiner** over the gated pool (the constituents are
formulaic alphas *and* learned alphas), trained walk-forward, **admitted only through S1's deflated-Sharpe
gate** vs the P4 linear combiner as the benchmark — if the nonlinear combiner doesn't beat the linear one
*out-of-sample after deflation*, it is rejected (no ML-for-ML's-sake). Plugs into the loop as one
`ISignalSource`. Close ceremony.

---

## Exit criteria

- Feature-matrix builder is truncation-invariant (features at `t` identical with/without > t data).
- Linear/GBT/ensemble training is **deterministic**: same data + same seed → byte-identical model params and
  byte-identical emitted signal (the determinism invariant, the hard one for ML).
- Every learned model and the ensemble combiner is trained inside S1 CPCV and reports a **deflated** Sharpe;
  a model with no real edge is **rejected** by the DSR/PBO gate (non-vacuous — the anti-snooping proof that
  justifies lifting the p0 ML ban).
- The nonlinear combiner is admitted **only if** it beats the P4 linear combiner OOS-after-deflation; on a
  fixture where it doesn't, it is correctly rejected.
- HMM regimes are PIT (fit-on-trailing, applied forward); regime-conditional weights improve a fixture OOS.
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit; differential test per model
  vs a reference implementation.

## Invariants this sprint must prove

Determinism (seeded, order-fixed training → byte-identical params and signals — the make-or-break for ML in a
deterministic engine), no look-ahead (train inside CPCV; features and regimes fit-on-trailing/apply-forward),
no snooping (DSR/PBO admission gate; nonlinear-beats-linear-OOS requirement), differential correctness (each
model vs a reference solver within ULP/tolerance).

## Dependencies

- **Upstream:** S1 (CPCV + DSR/PBO), S2 (parallel training/folds), S4 (library + features), P4 (linear
  combiner benchmark + `ISignalSource` seam).
- **atx-core (Pattern B edges):** L7 elastic-net solver (S5.2), deterministic GBT (S5.3), HMM/forward-backward
  (S5.4). Each raised at S5 kickoff; the engine builds **none** of these itself.

## Explicitly NOT in this sprint

**No neural nets / deep learning** (anti-roadmap #7 — stop at linear + GBT + HMM, the models the research
supports at this N; revisit only with evidence the simpler combiners are saturated). No alternative-data
features (anti-roadmap #3 — price-volume + classifications + DSL outputs only). No online/streaming model
updates (batch walk-forward only). No GPU.

## Baton → next

S5 makes the pool's constituents and combiner *learned*, not just formulaic — completing the discover/predict/
combine arc. S7 then operates the combined book over time (multi-period, decay-monitored, capacity-bounded).
