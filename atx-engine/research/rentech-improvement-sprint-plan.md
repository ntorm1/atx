# Sprint S10 — Renaissance-Inspired Conviction Sizing & Regime-Adaptive Integration — Implementation Plan

**Status:** 📋 PLAN (frozen spec for kickoff — not yet opened)
**Proposed worktree:** `.claude/worktrees/s10-conviction-regime`
**Proposed branch:** `worktree-s10-conviction-regime`
**Base:** main @ `767c08b` (S8.8b — confirm against ROADMAP at kickoff)
**Source research:** [`rentech-structure-signals-domain-mapping.md`](rentech-structure-signals-domain-mapping.md)
**Prior progress:** S8.8a/b (FactorModel + MultiHorizon split), S9 (fund-level, partial)

> ⚠️ **Number is provisional.** Memory shows S9 = fund-level multi-sleeve (partial). Confirm
> the next free sprint id against the canonical ROADMAP before opening; renumber units if S10 is taken.

---

## Why this sprint exists

A deep-research pass on Renaissance Technologies (verified, 8 findings, see source research) was
mapped against the current atx-engine implementation. **The headline result: atx already implements
most of RenTech's documented philosophy** — systematic signal discovery (factory/genome/DSL),
overfitting control (PBO, deflated Sharpe, CPCV, OOF floors), a unified per-asset model with
portfolio-level combination, multi-horizon cost-aware optimization (Garleanu-Pedersen), the full
latent-state toolbox (HMM, TCN, autoencoder, ensemble), and obsessive determinism/data hygiene.

So this is **not** a "build RenTech from scratch" plan. It targets the **seven concrete gaps** where
the RenTech mapping exposes something atx does not yet do. The strongest, most defensible gaps cluster
around three ideas RenTech is explicit about but atx only partially realizes:

1. **Conviction-weighted sizing** — RenTech traded low-interpretability signals at *reduced size*
   while researching them (interpretability modulated conviction, not a hard gate). atx's admission is
   **binary** (DSR threshold + correlation screen); nothing discounts position size by confidence.
2. **Explicit fractional-Kelly leverage** — Berlekamp (Shannon/Kelly lineage) rebuilt Medallion in
   1989 around Kelly sizing of small edges. atx's optimizer is "Kelly-inspired marginal utility" with
   a quadratic leverage penalty — Kelly is *implicit*, not an explicit, conviction-scaled, fractional layer.
3. **Regime-adaptive combination + fast adaptation** — RenTech runs *one model integrating sub-models
   for different market conditions*, and Medallion "adapts quickly." atx has a trained HMM (S5-4) that is
   **not wired** into the combiner or risk sizing, and the pipeline is batch-only (no walk-forward re-fit).

## Gap analysis (RenTech finding → current atx state → gap → unit)

| # | RenTech finding (verified) | Current atx state | Gap | Unit |
|---|---|---|---|---|
| G1 | Unexplained signals trade at reduced size; interpretability → conviction | `combine/gate.hpp` binary admit (DSR + corr screen); optimizer sizes by E[r]/risk only | No continuous conviction score feeding size | **S10-1** |
| G2 | Berlekamp Kelly/fractional-Kelly sizing of small edges | `risk/garleanu_pedersen.hpp` Kelly-*inspired* utility + quadratic leverage penalty | No explicit fractional-Kelly, conviction-scaled, covariance-aware leverage layer | **S10-2** |
| G3 | One model integrating sub-models per market condition | `learn/hmm.hpp` regime model exists but unwired; `combine/combiner.hpp` is static linear | HMM regime posterior not feeding combination weights | **S10-3** |
| G4 | Thousands of weak signals combined & de-correlated (open research Q) | Combiner = linear combo via sample cov; `diversify` objective at *discovery* only | No crowding/capacity-aware de-correlation at *combine* | **S10-4** |
| G5 | Casino edge: many tiny uncorrelated bets, law of large numbers | `factory/fitness.hpp` rewards `1−mean|corr|`; no portfolio breadth metric | No breadth instrumentation (IR = IC·√breadth, effective-N) | **S10-5** |
| G6 | Medallion adapts quickly; horizon×leverage×adaptation coupled | Pipeline is batch; no walk-forward re-fit/re-admit; decay unmeasured | No adaptation harness / decay tracking | **S10-6** |
| G7 | Data validated against independent sources (yearbooks/WSJ) | tsdb/ORATS mature; survivorship frozen; corp-actions adjusted | No cross-source data-validation/anomaly gate | **S10-7 (stretch / defer)** |

**Explicitly NOT in scope** (already mature — do not rebuild): HMM/TCN/NN training, PBO/DSR/CPCV,
factor model, multi-horizon QP, ORATS loader, determinism substrate, bias audit. Intraday/tick data
(G7-adjacent) is a separate large track, deferred.

---

## Realistic scope for this sprint

Seven units + marker + close. If unit count or compile fan-out exceeds the ~7-unit guideline, split
per the existing `a`/`b`/`c` convention: **S10a** {S10-1, S10-2} (sizing), **S10b** {S10-3, S10-4}
(combination), **S10c** {S10-5, S10-6} (instrumentation + adaptation). S10-7 is a stretch unit,
defer to a future sprint unless cheap.

1. **S10-1** — Conviction score: continuous [0,1] per-alpha confidence from existing eval outputs.
2. **S10-2** — Fractional-Kelly sizing layer: explicit, conviction-scaled, covariance-aware leverage.
3. **S10-3** — Regime-conditioned combination: HMM posterior → per-regime combine weights.
4. **S10-4** — Crowding/capacity-aware de-correlation at the combine step.
5. **S10-5** — Breadth instrumentation: IR = IC·√breadth, effective number of bets, report wiring.
6. **S10-6** — Walk-forward adaptation harness: periodic re-fit/re-admit + decay measurement.
7. **S10-7** — *(stretch)* Cross-source data-validation gate.

**Defer to a later sprint (or skip):**
- Intraday/tick alignment + microstructure filtering (large, separate track).
- Live broker adapter / real-time regime switching (out of research scope).
- Attention/transformer alpha family (additive model family, not a RenTech gap).

---

## Per-unit specifications

> Every unit obeys the existing discipline. Quoted verbatim into each sub-agent brief:
> **"Marker-commit pattern is mandatory: commit before stopping or work is lost."**
> Plus the determinism contract (no RNG on result path; order-fixed reductions; two-runs-equal test;
> digest stability under FNV-1a-64) and the `plans/docs/implementation-quality.md` standard.

### S10-0 — Marker + ledger + scaffold
- **Scope:** Open `sprint-10-progress.md` ledger (header per `plans/docs/sprint.md` §required-structure).
  Add fwd-decl scaffold header `combine/conviction.hpp` (namespace `atx::engine::combine`) and
  `risk/kelly_sizing.hpp` (namespace `atx::engine::risk`). No logic.
- **Acceptance:** builds green under `/W4 /permissive- /WX`; `ConvictionScaffold 1/1/0/0`.
- **Commit:** `docs(s10-0): open sprint-10 ledger + conviction/kelly scaffold`.

### S10-1 — Conviction score (continuous confidence)
- **Rationale (RenTech G1):** interpretability modulated *size*, not a binary admit. Convert the
  gate's pass/fail into a continuous conviction in [0,1].
- **Scope:** New `combine/conviction.hpp` + `src/combine/conviction.cpp`.
  `ConvictionScore conviction(const AlphaMetrics&, const eval::DsrResult&, const eval::PboResult&,
  ExplainFlag)` → continuous score. Inputs already computed upstream — **reuse, do not recompute**:
  - deflated-Sharpe margin over threshold (from `eval/deflated_sharpe.hpp`),
  - PBO complement `1 − pbo` (from `eval/pbo.hpp`),
  - OOS/IS Sharpe stability ratio (from existing metrics),
  - `ExplainFlag` (enum: `Explained` | `PartlyExplained` | `HeadScratcher`) — the step-3 explanation
    gate from the RenTech 3-step pipeline, supplied by caller; `HeadScratcher` multiplies conviction
    by a config `head_scratcher_discount` (default 0.5), realizing "trade unexplained at reduced size."
  - Combine via a **fixed-weight, order-fixed** product/weighted-mean (named constants, no RNG).
- **Acceptance:** `ConvictionScore` suite ≥ 8 tests incl. monotonicity (↑DSR ⇒ ↑conviction),
  `HeadScratcher` halves an otherwise-identical score, degenerate NaN inputs → guarded (death-test),
  two-runs-equal. `Conviction total/0/0`.
- **Determinism:** pure function, no RNG, order-fixed reduction.
- **Commit:** `feat(s10-1): continuous conviction score from DSR/PBO/stability/explainability`.

### S10-2 — Fractional-Kelly sizing layer
- **Rationale (RenTech G2):** make Kelly *explicit* and conviction-scaled; fractional-Kelly is the
  realistic form (full Kelly overbets — Samuelson). Sits between combiner output and the optimizer.
- **Scope:** New `risk/kelly_sizing.hpp` + `src/risk/kelly_sizing.cpp`.
  `KellyWeights kelly_size(const VecX& expected_alpha, const FactorModel& cov, const VecX& conviction,
  KellyConfig cfg)` where:
  - base full-Kelly target `f* = Σ⁻¹ μ` solved via the **existing** `FactorModel::apply_inverse()`
    (Woodbury — do **not** materialize M×M),
  - scale by `cfg.kelly_fraction` (default 0.25 — quarter-Kelly),
  - scale per-name by `conviction` (G1 output): low-conviction names get less gross,
  - clamp to `cfg.max_gross` / `cfg.max_leverage`; feed result as the optimizer's risk-aversion / aim
    rather than replacing the QP (GP optimizer keeps cost-to-go; Kelly sets the *target* it tracks).
- **Acceptance:** `KellySizing` suite ≥ 7 tests: full-Kelly matches `Σ⁻¹μ` on a 2-asset closed form
  (1e-9), `kelly_fraction=0.25` scales gross by 0.25, zero-conviction name ⇒ zero weight, leverage
  clamp binds, two-runs-equal, NaN cov guarded. `KellySizing total/0/0`.
- **Determinism:** order-fixed; reuses deterministic `apply_inverse`.
- **Commit:** `feat(s10-2): fractional-Kelly conviction-scaled sizing over factored covariance`.

### S10-3 — Regime-conditioned signal combination
- **Rationale (RenTech G3):** "one model integrating sub-models for different market conditions."
  The HMM (S5-4) is trained but unwired; route its regime posterior into the combiner.
- **Scope:** Extend `combine/combiner.hpp` / `combined_source.hpp`. Add a `RegimeCombiner` that holds
  one linear combo per regime (fit on regime-masked history) and at eval time blends them by the HMM
  **posterior** `P(regime | history_t)` from `learn/hmm.hpp` (point-in-time — no look-ahead; posterior
  uses only data ≤ t). Falls back to the existing single static combo when no regime model attached
  (byte-identical to today in that path — guard test required).
- **Acceptance:** `RegimeCombiner` suite ≥ 8 tests: degenerate single-regime ⇒ **byte-identical** to
  existing `Combiner` (the critical guard), posterior blend convex (weights sum to 1, ≥0), no-look-ahead
  truncation-invariance (reuse `validation/bias_audit.hpp` style), two-runs-equal. `RegimeCombine total/0/0`.
- **Determinism:** PIT posterior, order-fixed blend, no RNG.
- **Commit:** `feat(s10-3): regime-posterior-blended signal combination (HMM → combiner)`.

### S10-4 — Crowding / capacity-aware de-correlation at combine
- **Rationale (RenTech G4, open research Q#3):** breadth only helps if bets are *independent*. Penalize
  crowded / capacity-limited signals when forming the combination, not just at discovery.
- **Scope:** Extend the combiner weight solve with a de-correlation penalty: down-weight signals with
  high mutual |corr| (reuse `combine/metrics.hpp` corr) and low remaining capacity (reuse
  `cost/capacity.hpp` ADV curve). Add `CrowdingConfig {corr_penalty, capacity_floor}`. Shrinks
  redundant/crowded signal weights toward zero in the combined book.
- **Acceptance:** `Crowding` suite ≥ 6 tests: two perfectly-correlated signals ⇒ combined weight ≈ one
  signal's (not double), capacity-floored name capped, `corr_penalty=0` ⇒ byte-identical to S10-3 path,
  two-runs-equal. `Crowding total/0/0`.
- **Determinism:** order-fixed; corr/capacity are existing deterministic kernels.
- **Commit:** `feat(s10-4): crowding + capacity-aware de-correlation in combiner`.

### S10-5 — Breadth instrumentation (law of large numbers)
- **Rationale (RenTech G5):** the casino edge is *breadth* — many independent bets. Make it measurable.
  Fundamental law of active management: `IR = IC · √breadth`.
- **Scope:** New `eval/breadth.hpp` + `src/eval/breadth.cpp`. Compute: effective number of independent
  bets `N_eff = (Σλ)² / Σλ²` from the signal/return covariance eigenvalues (reuse `atx-core/linalg/pca`),
  realized IC, and the implied IR decomposition. Wire into `atx-impl` `run_report()` so the report
  prints breadth, IC, IR-decomposition alongside Sharpe.
- **Acceptance:** `Breadth` suite ≥ 6 tests: `N_eff` = K for K orthogonal equal-var bets, `N_eff` = 1
  for K identical bets, `IR ≈ IC·√N_eff` on a planted-orthogonal fixture (1e-6), report line present,
  two-runs-equal. `Breadth total/0/0`.
- **Determinism:** eigen-reduction order-fixed (ascending eigenvalue), no RNG.
- **Commit:** `feat(s10-5): breadth / effective-N + IR=IC·√breadth report instrumentation`.

### S10-6 — Walk-forward adaptation harness
- **Rationale (RenTech G6):** Medallion adapts quickly; atx is batch. Add a deterministic walk-forward
  loop that re-runs discover→combine→admit at intervals and measures signal decay — the "adaptation
  speed" knob, coupled to horizon.
- **Scope:** New `loop/walk_forward.hpp` + `src/loop/walk_forward.cpp` (and an `atx-impl` stage
  `run_walkforward`). Expanding-or-rolling window (config `window_mode`, `step_days`); at each step
  re-fit combiner + re-run gate on the as-of panel (PIT — strictly data ≤ step), stitch the OOS
  segments into one walk-forward equity curve; emit per-step turnover of the *admitted set* (signal
  decay proxy). Reuses `eval/cpcv.hpp` purge/embargo for the as-of boundary.
- **Acceptance:** `WalkForward` suite ≥ 7 tests: no-look-ahead (step k uses only data < boundary_k —
  truncation-invariance), stitched curve = concatenated OOS segments (no overlap/gap), single-window
  degenerate case = existing batch result byte-identical (guard), decay metric monotone on a planted
  decaying-alpha fixture, two-runs-equal. `WalkForward total/0/0`.
- **Determinism:** lexicographic step enumeration, fixed boundaries, no RNG.
- **Commit:** `feat(s10-6): deterministic walk-forward re-fit/re-admit + decay instrumentation`.

### S10-7 — *(stretch)* Cross-source data-validation gate
- **Rationale (RenTech G7):** Straus validated data against independent sources. Add an anomaly/cross-
  source consistency gate to the loader (e.g., return-factor vs price-derived return reconciliation,
  cross-vendor close agreement when a second source exists).
- **Scope:** Extend `data/orats_history.hpp` ingest with a validation pass: flag rows where
  `closeUnadjPr × cumReturnFactor` disagrees with `totalReturn`-implied price beyond tolerance; emit a
  per-date anomaly count to the manifest. Non-fatal (flag, don't drop) by default; config to fail-closed.
- **Acceptance:** `DataValidate` suite ≥ 5 tests: planted inconsistent row flagged, clean data 0 flags,
  fail-closed mode errors, two-runs-equal. `DataValidate total/0/0`.
- **Commit:** `feat(s10-7): cross-source data-consistency validation gate in ORATS ingest`.

---

## Sprint commits (template — fill SHAs during execution)

| Commit | Unit | Test counts |
|--------|------|-------------|
| `<sha>` | S10-0 | ConvictionScaffold 1/1/0/0 |
| `<sha>` | S10-1 | Conviction … |
| `<sha>` | S10-2 | KellySizing … |
| `<sha>` | S10-3 | RegimeCombine … |
| `<sha>` | S10-4 | Crowding … |
| `<sha>` | S10-5 | Breadth … |
| `<sha>` | S10-6 | WalkForward … |
| `<sha>` | S10-7 | DataValidate … *(stretch)* |
| `<sha>` | close | docs(s10-close): close sprint-10 — N units, M tests |

**S10 adds N new tests on top of the prior total (full engine suite green under `/W4 /permissive- /WX`;
atx-core net diff EMPTY).** ← load-bearing close metric.

---

## Dependencies & dispatch order

```
S10-1 ─┐
       ├─► S10-2 (Kelly consumes conviction)
S10-1 ─┘
S10-3 ─► S10-4 (crowding extends regime combiner)
S10-5  (independent — parallel-eligible)
S10-2,S10-3,S10-4 ─► S10-6 (walk-forward exercises sizing+combine)
S10-7  (independent — parallel-eligible / stretch)
```

- **Sequential:** S10-1 → S10-2; S10-3 → S10-4; {S10-2,S10-4} → S10-6.
- **Parallel-eligible (disjoint files, no shared headers):** S10-5 and S10-7 can run alongside the
  S10-1→2 and S10-3→4 chains.
- Each sub-agent brief must carry: worktree+branch, verbatim unit scope (from this plan), acceptance
  criteria, the mandatory marker-commit line, expected commit message, predecessor SHAs, the ledger row
  to write, and the `implementation-quality.md` standard.

## Determinism contract (applies to every unit)
- No RNG on any result path (RNG only permitted in candidate init, already isolated).
- Order-fixed reductions; ascending-index / ascending-eigenvalue tie-breaks; Neumaier summation.
- Two-runs-equal test per unit; FNV-1a-64 digest stability where an artifact is produced.
- New code paths must include a **byte-identical guard** vs the pre-existing path when a config disables
  the new behavior (degenerate single-regime, `corr_penalty=0`, single-window walk-forward, etc.).

## What S10 will prove (baton target)
S10 turns atx from "binary-admit, statically-combined, batch" into "**conviction-sized, regime-adaptive,
breadth-instrumented, walk-forward**" — closing the precise gaps where the verified RenTech mapping
exceeded the current implementation, without rebuilding the (already mature) discovery, validation, and
optimization spines. Hands the fund-level S9 track a conviction-scaled, regime-aware combined book and a
breadth metric to allocate across sleeves.

## Open questions to resolve at kickoff
1. Confirm sprint number vs ROADMAP (S9 = fund-level; is S10 free?).
2. Canonical plans location on `main` (this doc lives in `research/`; promote to `atx-engine/plans/` on kickoff).
3. Quarter-Kelly default (`kelly_fraction=0.25`) — confirm vs any existing leverage policy in S8.6 constraints.
4. Whether S10-7 (data validation) is in-scope now or deferred with the intraday track.
