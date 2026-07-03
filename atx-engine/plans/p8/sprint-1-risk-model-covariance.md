# Sprint 1 — Risk-Model Covariance Spine

**Goal:** retire the diagonal / raw-MLE covariance the runnable pipeline uses today and wire the
already-built S8 Barra factor covariance (`FactorModel = X F Xᵀ + D`) into the combine and optimize
paths, so portfolio construction sizes against a real cross-sectional covariance instead of a
per-name variance. Add the built-but-orphaned dead-alpha crowding factors and make factor/industry
neutralization reachable from the runnable book. All opt-in behind an inert `RiskModelConfig`
default (= today's diagonal model); the no-flag path stays byte-identical.

**Owns (exclusive):**
`atx-engine/include/atx/engine/risk/factor_model.hpp` (builder adapter surface only — the estimation
math in `src/risk/factor_model.cpp` is frozen),
`atx-engine/include/atx/engine/risk/{stat_factor_model,shrinkage,eigen_adjust,specific_risk,psd_repair,dead_factor,exposures}.hpp`
(wiring/config threading only — no new estimator math),
`atx-engine/include/atx/engine/data/factor_model_artifact.hpp`,
`atx-impl/src/stage_optimize.cpp` (covariance-source swap),
`atx-impl/src/diag_risk.hpp` (route to a builder),
NEW `atx-impl/src/stage_riskmodel.{cpp,hpp}`;
tests under `atx-engine/tests/risk/` and `atx-impl/tests/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); `src/risk/factor_model.cpp`
estimation bodies (frozen — S1 calls `build_components`, it does not re-derive it);
`risk/{capacity,optimizer,garleanu_pedersen}.hpp` and `cost/*`, `loop/*`, `exec/*` (Sprint 4);
`fund/*` (Sprint 2); `learn/*`, `combine/regime_combiner.hpp`, `atx-impl/src/stage_combine.cpp`
(Sprint 3); the four hub files `atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}`
and `library/library.hpp`, `factory/factory.cpp` (Sprint 5).

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

The runnable optimizer builds a **diagonal** `FactorModel` (`X = M×1 zeros`, `F = [[1]]`,
`D = per-name TRI-return variance`) and feeds it to the multi-period optimizer for every period.
The full S8 estimator — `FactorModelBuilder::build_components` (robust Huber-IRLS cross-sectional
WLS → Ledoit-Wolf-shrunk factor covariance → Menchero-Wang-Orr eigenfactor de-biasing → VRA →
specific risk) — is tested and green but never called from `atx-impl`.

| Gap | File:line | Evidence |
|---|---|---|
| MVO path builds a diagonal model | `stage_optimize.cpp:202` | `ATX_TRY(auto model, diagonal_risk_model(research));` — the ONLY risk model on the MVO path |
| `model_at` returns the diagonal model for all periods | `stage_optimize.cpp:225-227` | `auto model_at = [&model](atx::usize){ return model; };` — no per-date factor estimation |
| The diagonal helper hard-codes `X=0`, `F=I₁` | `diag_risk.hpp:66-69` | `MatX X = MatX::Zero(M,1); MatX F = MatX::Identity(1,1);` — zero exposures ⇒ no cross-sectional correlation ⇒ optimizer sees only variance |
| Combiner fits weights on a raw MLE covariance | `stage_combine.cpp:755` | `const MatX cov = combine::detail::mle_covariance(centered, na);` — no shrinkage, no factor structure (out of S1's `stage_combine` scope; S1 exposes a cleaned artifact the combine reads — see S1-3) |
| `FactorModelBuilder`/`build_components` absent from `atx-impl` | grep `atx-impl/src` | zero hits — the entire S8 estimator (`p1/sprint-8a/8b-progress.md`, ~1133 tests) is dead weight in the runnable pipeline |
| Dead-alpha crowding factors absent | grep `atx-impl/src` for `dead_factor`/`extract_dead_factors` | zero hits — `risk/dead_factor.hpp` (Kakushadze-Yu holdings-overlap eigen-extraction) built + tested, never wired |
| Group/industry neutralize unreachable | `multi_horizon.hpp:154-157` (recon) | returns `ErrorCode::NotImplemented` when `n_dead_factors > 0`; `BacktestLoop`/optimize path never supplies a `group_map` for factor neutralization |

---

## Architecture note — what "wire the factor model" actually means

`FactorModel` (`factor_model.hpp:8-17`) stores a GIVEN `(X, F, D)` and applies it without ever
materializing the dense `M×M V`: `risk(w)` at `factor_model.hpp:181`, `apply_inverse` via cached
Woodbury capacitance (`factor_model.hpp:26-30`), `neutralize(s) = s − X(XᵀX)⁻¹Xᵀs` at
`factor_model.hpp:16`. The estimator that PRODUCES `(X, F, D)` from a cross-section is
`FactorModelBuilder::build_components → FactorComponents` (the P4-7b public API, memory obs 7745 /
10632). S1 does **not** touch that math. S1's job is three thin seams:

1. **Build** a real `FactorModel` per fit-window from the panel's style + industry exposures by
   calling `build_components` (S1-1), and expose it as a serializable `FactorModelArtifact` (S1-0).
2. **Feed** that model into `stage_optimize`'s `model_at` behind `RiskModelConfig` (S1-2) and expose
   the cleaned factor covariance to the combiner as an artifact it reads (S1-3).
3. **Augment** the model with dead-alpha crowding factors (S1-4) and make group neutralization
   reachable (S1-5).

The exposures `X` are the load-bearing input the diagonal stub is missing. S1-1 assembles the
standard Barra-style style block — **size** (log dollar-ADV or log-cap proxy), **volatility**
(trailing return vol), **momentum** (trailing cumulative return), **beta** (trailing market beta) —
plus **industry dummies** from the `group_map` when present. Every column is point-in-time
(computed from data strictly before the fit-window end) and winsorized/standardized cross-sectionally
per the existing `risk/exposures.hpp` helpers. No new estimator math — S1 assembles columns the
existing builder already knows how to regress.

---

## Determinism contract (Sprint 1)

S1 follows the **p8 opt-in / default-byte-identical** contract (ROADMAP §Determinism). Every new
capability lives behind a new `RiskModelConfig` field with an inert default:

- `RiskModelConfig::kind = RiskModelKind::Diagonal` — inert; `Diagonal` routes to today's
  `diagonal_risk_model`, byte-identical. `Factor` is the opt-in.
- `RiskModelConfig::dead_alpha_factors = false` — inert; no augmentation.
- `RiskModelConfig::group_neutralize = false` — inert; neutralize path unchanged.

At the inert defaults, `stage_optimize` produces the byte-identical books/turnover/cost digest it
does today. The factor estimator is deterministic (NO RNG; order-fixed reductions;
`factor_model.hpp:52-56`), so the on-path result is reproducible run-to-run and seq==parallel.

**Four test classes per opt-in field (mandatory):** (a) off-path byte-identity — `Diagonal` default,
digest unchanged vs the pre-S1 pinned optimize golden; (b) on-path RED→GREEN — `Factor` model on a
tiny fixture where the factor covariance provably de-levers a correlated pair the diagonal model
does not; (c) twice-run — same panel → same artifact bytes and same books; (d) seq==parallel — the
per-date factor build is order-independent (each date is an independent cross-sectional fit).

---

## Dependency / wiring map

```
NEW data/factor_model_artifact.hpp already EXISTS as a header ← S1-0 defines the on-disk POD +
    (RiskModelConfig lives in a shared engine config header the sprint adds, inert defaults)
risk/exposures.hpp            ← S1-1 assembles X columns (size/vol/mom/beta + industry dummies)
risk/factor_model.hpp:build_components ← S1-1 calls it to estimate (X,F,D) → FactorComponents (FROZEN math)
NEW atx-impl/stage_riskmodel.cpp ← S1-1 the deploy stage: panel → FactorModelArtifact (per fit-window)
atx-impl/stage_optimize.cpp:202  ← S1-2 diagonal_risk_model → (RiskModelConfig.kind==Factor ? factor : diagonal)
atx-impl/stage_optimize.cpp:225  ← S1-2 model_at returns the per-period factor model from the artifact
  └─ consumes: FactorModel (already the type model_at returns — no optimizer signature change)
combine consumption of cov     ← S1-3 combine reads FactorModelArtifact.factor_cov() when RiskModelConfig.kind==Factor
risk/dead_factor.hpp:extract_dead_factors / augment_factor_model ← S1-4 (retired library alphas → risk factors)
risk/factor_model.hpp:neutralize + multi_horizon.hpp:154 ← S1-5 supply group_map; enable dead-factor path
tests/risk/factor_model_wire_test.cpp        ← S1-1/S1-2 (auto-globbed)
tests/risk/dead_factor_wire_test.cpp         ← S1-4
atx-impl/tests/stage_riskmodel_test.cpp      ← S1-1/S1-2/S1-3
```

---

## Tasks

### S1-0 — Open ledger + `RiskModelConfig` plumbing + artifact POD (do first; all units depend on this)

**Goal:** create the sprint ledger (marker commit); define `RiskModelConfig` (the inert-default
config that every downstream unit reads) and the `FactorModelArtifact` serializable POD (the seam
between the new `stage_riskmodel` producer and the `stage_optimize`/combine consumers). No behavior
change — the fields exist, nothing reads them non-inertly yet.

**Wiring:**
- Add `RiskModelConfig` to the engine risk config surface (a small POD near `risk/factor_model.hpp`
  config structs, or a new `risk/risk_model_config.hpp` if no existing home fits — prefer the
  existing home). Fields, all inert-default:
  ```cpp
  enum class RiskModelKind : atx::u8 { Diagonal = 0, Factor = 1 }; // append-only; Diagonal frozen at 0
  struct RiskModelConfig {
    RiskModelKind kind             = RiskModelKind::Diagonal; // inert => today's per-name variance
    bool          dead_alpha_factors = false;                // inert => no crowding augmentation
    bool          group_neutralize  = false;                 // inert => no factor/industry neutralize
    atx::u32      fit_lookback_days = 252U;                  // exposure/estimation window
    // style-block toggles (all on when kind==Factor; ignored when Diagonal):
    bool style_size = true, style_vol = true, style_mom = true, style_beta = true, industry = true;
  };
  ```
- Define/confirm `data/factor_model_artifact.hpp` carries the on-disk form of a per-fit-window
  `FactorModel`: the `(X, F, D)` blocks + `[fit_begin, fit_end)` + a schema/version tag + a content
  digest. It already exists as a header (confirm current contents; extend, do not fork). The artifact
  must round-trip: `FactorModel → serialize → bytes → deserialize → FactorModel` byte-identical.

**Determinism:** pure addition. No aggregate-initializer breakage (append fields at struct end).
Existing optimize/report goldens unchanged (nothing reads `kind != Diagonal` yet).

**Accept:**
- Project compiles (debug + release), all existing `risk_*`, `stage_optimize_*`, `stage_report_*`
  suites green.
- `factor_model_artifact_roundtrip` (new `tests/risk/`): a hand-built `FactorModel` (M=4, K=2)
  serializes and deserializes byte-identical; digest stable twice-run.
- `RiskModelConfig` default-constructs to the inert values; a `static_assert`/test pins
  `RiskModelKind::Diagonal == 0` (frozen enum index).

---

### S1-1 — `stage_riskmodel`: build a real factor model from the panel

**Goal:** a new deploy stage that consumes the research panel and produces a `FactorModelArtifact`
per fit-window by (a) assembling the Barra-style exposure matrix `X` and (b) calling the frozen
`build_components` to estimate `(X, F, D)`. This is the producer; S1-2/S1-3 are the consumers.

**Root cause:** `diag_risk.hpp:66-69` hard-codes `X = M×1 zeros` — the optimizer has no exposure
structure to correlate against. The real exposures exist in the panel (returns → vol/mom/beta;
dollar-ADV → size) and `group_map` (industry), and `build_components` already knows how to regress
them; nothing assembles the columns and calls it from `atx-impl`.

**Wiring:**
- NEW `atx-impl/src/stage_riskmodel.{cpp,hpp}`: `build_risk_model(const Panel& research, const
  RiskModelConfig& cfg) -> Result<FactorModelArtifact>`.
- Assemble `X` (M×K) from PIT columns using `risk/exposures.hpp` helpers (standardize + winsorize
  cross-sectionally): size = `log(dollar_adv)` proxy, vol = trailing return σ, momentum = trailing
  cumulative return (skip most-recent to avoid 1-day reversal per convention), beta = trailing OLS
  beta to the equal-weight market. Industry dummies appended from `group_map` when `cfg.industry`.
  Every column reads only rows strictly inside `[fit_end − fit_lookback_days, fit_end)`.
- Call `FactorModel::build_components(X, returns_window, …)` → `FactorComponents` → `FactorModel`,
  then serialize to `FactorModelArtifact`. The estimation math is NOT reimplemented — S1 assembles
  inputs and calls the frozen builder.
- When `cfg.kind == Diagonal`, `build_risk_model` returns the diagonal artifact (delegates to
  `diagonal_risk_model`) so the stage is a drop-in for both modes.

**Determinism:** each fit-window is an independent, order-fixed cross-sectional fit (no RNG). The
exposure columns are computed in canonical instrument order. seq==parallel: fitting window `s` never
reads window `s'` state.

**Accept:**
- `stage_riskmodel_factor_nondegenerate` (new `atx-impl/tests/`): on a fixture panel with a
  constructed 2-factor structure (two correlated instrument groups), the built `F` is non-diagonal
  and `risk(w)` for a long-one-group/short-other portfolio is provably LOWER than the diagonal
  model's estimate (the factor model sees the hedge; the diagonal model does not).
- `stage_riskmodel_diagonal_equiv`: `cfg.kind == Diagonal` → the artifact's `(X,F,D)` equals
  `diagonal_risk_model`'s output exactly (drop-in).
- PIT guard test: an exposure column computed at `fit_end` does not change when future rows
  (`t ≥ fit_end`) are perturbed (no look-ahead).
- Twice-run: same panel → same artifact bytes.

---

### S1-2 — Swap the covariance source in `stage_optimize`

**Goal:** route `stage_optimize`'s `model_at` to the factor `FactorModelArtifact` when
`RiskModelConfig.kind == Factor`, keeping the diagonal path byte-identical at the default.

**Root cause:** `stage_optimize.cpp:202` unconditionally builds the diagonal model; `model_at`
(`:225-227`) returns it for every period. The multi-period optimizer already consumes a
`const FactorModel&` — no optimizer signature change is needed, only the SOURCE of the model.

**Wiring:**
- `stage_optimize.cpp:199-227` — replace the unconditional `diagonal_risk_model(research)` with:
  ```cpp
  // S1-2: covariance source. Diagonal (default) => byte-identical to pre-S1. Factor => the
  // per-fit-window model built by stage_riskmodel, applied per period via the artifact's
  // window index. model_at still returns `const FactorModel&`; only the backing store changes.
  ATX_TRY(auto rm, load_or_build_risk_model(research, cfg.risk_model)); // Diagonal or Factor
  auto model_at = [&rm](atx::usize period) -> const risk::FactorModel& {
      return rm.model_for_period(period); // Diagonal: one model for all periods (today's behavior)
  };
  ```
- For `Factor`, `model_for_period` selects the fit-window covering `period` (piecewise-constant
  between rebalances, matching the existing single-model cadence semantics). The multi-period
  optimizer (`mpo.run`, `stage_optimize.cpp:229`) is unchanged.

**Determinism (inert default):** `kind == Diagonal` ⇒ `rm` holds exactly `diagonal_risk_model`'s
model and `model_for_period` returns it for every period ⇒ the `alpha_at`/`model_at`/`cost` inputs
to `mpo.run` are bit-identical ⇒ the books/turnover/cost digest is unchanged.

**Accept:**
- `stage_optimize_diagonal_byte_identical`: default `RiskModelConfig` → the pinned optimize-stage
  golden digest is unchanged (off-path byte-identity).
- `stage_optimize_factor_delevers`: on the S1-1 correlated-group fixture, `kind == Factor` produces a
  book with strictly lower ex-ante factor risk `wᵀVw` and lower gross on the crowded pair than the
  diagonal book, at equal alpha (the optimizer now hedges the shared factor).
- Twice-run + seq==parallel on the factor path.

---

### S1-3 — Expose the cleaned factor covariance to the combiner

**Goal:** let the combiner fit blend weights on the cleaned factor covariance (via
`FactorModel::factor_cov()` / a reconstructed cleaned alpha-return covariance) instead of the raw
`mle_covariance`, when `RiskModelConfig.kind == Factor` — without S1 editing `stage_combine.cpp`
(that file is Sprint 3's; S1 exposes a read-only artifact the combine path consumes).

**Root cause:** `stage_combine.cpp:755` fits on `mle_covariance(centered, na)` — an unshrunk MLE
covariance that is noise-dominated when `N ≈ T` (the exact regime here), the estimation-error source
HRP/shrinkage exist to fix. The combiner already reads engine helpers; S1 provides a cleaned
covariance accessor it can call behind the config flag.

**Wiring:**
- Add a `combine`-visible accessor on the artifact: `FactorModelArtifact::cleaned_alpha_cov(...)`
  returning the shrunk/denoised covariance over the fit window (reusing `risk/shrinkage.hpp` +
  `risk/eigen_adjust.hpp` — the RMT/Marchenko-Pastur eigen-clip — no new math). Provide it as a
  pure function in an S1-owned header the combine path can include.
- Do NOT edit `stage_combine.cpp` — instead, the accessor is the seam; Sprint 3 (which owns
  `stage_combine.cpp`) threads the call behind `RiskModelConfig.kind`. S1 ships the accessor + its
  unit tests + a documented seam note in the ledger. (This keeps file ownership disjoint.)

**Determinism:** the accessor is pure (deterministic shrinkage + eigen-clip on a given return
window). At `kind == Diagonal` the seam is never called; combine is byte-identical.

**Accept:**
- `cleaned_alpha_cov_shrinks`: on a fixture where the sample covariance has a spurious large
  eigenvalue (N≈T noise), the cleaned covariance clips it toward the Marchenko-Pastur bulk edge and
  the resulting min-variance weights are provably more diversified (lower max weight) than
  sample-cov weights.
- `cleaned_alpha_cov_psd`: the returned matrix is SPD (post `psd_repair`).
- Ledger records the Sprint-3 seam handoff explicitly (what call, where, behind which flag).

---

### S1-4 — Dead-alpha crowding factors

**Goal:** wire `risk/dead_factor.hpp` so retired/decayed library alphas become orthogonal risk
factors the optimizer steers off — the endogenous (non-vendor) crowding model that keeps `N_eff`
high as the library grows. Opt-in behind `RiskModelConfig.dead_alpha_factors`.

**Root cause:** Phase-D measured 30 admitted alphas collapsing to `N_eff = 8.76` — direct crowding.
`extract_dead_factors` (Kakushadze-Yu holdings-overlap eigen-extraction, arXiv:1709.06641) +
`augment_factor_model` are built and tested (`risk/dead_factor.hpp`, `risk_dead_factor_test.cpp`) but
absent from `atx-impl`.

**Wiring:**
- In `stage_riskmodel` (S1-1's producer), when `cfg.dead_alpha_factors`, load the retired/decayed
  alpha holdings from the library, call `extract_dead_factors` (holdings-overlap `M×M` → eigen →
  truncate by eRank) and `augment_factor_model(base, dead_factors)` to raise variance on those
  directions before serializing the artifact.
- The augmentation is a pure transform on the already-built `FactorModel`; it does not re-estimate
  the style/industry block.

**Determinism (inert default):** `dead_alpha_factors == false` ⇒ no augmentation ⇒ artifact
identical to S1-1. The eigen-extraction is order-fixed (no RNG).

**Accept:**
- `dead_factor_raises_crowded_variance`: with two synthetic "dead" alphas whose holdings overlap a
  live book, `dead_alpha_factors=true` makes `risk(w)` for a book aligned with the dead directions
  strictly higher than without augmentation (the optimizer will down-weight that alignment).
- `dead_factor_inert_off`: `false` ⇒ artifact byte-identical to S1-1.
- eRank truncation test: augmenting with `k` dead factors adds exactly `min(k, eRank)` columns.

---

### S1-5 — Make factor/industry neutralization reachable

**Goal:** supply the `group_map` and enable the group-neutralize path so the combined signal is
residualized against the factor/industry exposures (`s − X(XᵀX)⁻¹Xᵀs`), and resolve the
`NotImplemented` branch that fires when dead factors are present. Opt-in behind
`RiskModelConfig.group_neutralize`.

**Root cause:** `FactorModel::neutralize` exists (`factor_model.hpp:16`) but the optimize path never
supplies a `group_map`; `multi_horizon.hpp:154-157` returns `ErrorCode::NotImplemented` when
`n_dead_factors > 0`, blocking the augmented model from the neutralize path.

**Wiring:**
- In `stage_riskmodel`/`stage_optimize`, when `cfg.group_neutralize`, pass the industry `group_map`
  (already derivable from the panel's classification fields / `sector_groups.hpp`) into the model so
  `neutralize` residualizes against style + industry before sizing.
- Resolve `multi_horizon.hpp:154-157`: the augmented factor model (S1-4) must be neutralize-able; the
  `NotImplemented` guard is replaced with the factored neutralize that already handles a rank-`K+d`
  exposure block (the math exists in `FactorModel::neutralize`; the guard was a placeholder).

**Determinism (inert default):** `group_neutralize == false` ⇒ neutralize path unchanged ⇒
byte-identical. On-path, neutralize is a deterministic linear residualization.

**Accept:**
- `group_neutralize_removes_factor_bet`: a signal that is pure industry tilt (constant within each
  group) neutralizes to ≈0; a signal orthogonal to the groups passes through unchanged.
- `dead_factor_neutralize_no_longer_notimpl`: with `dead_alpha_factors=true` + `group_neutralize=true`
  the path returns `Ok` (not `NotImplemented`) and residualizes against the augmented block.
- `group_neutralize_inert_off`: `false` ⇒ optimize digest unchanged.

---

## Sequencing

1. **S1-0 first** (config + artifact POD + ledger marker) — every unit reads `RiskModelConfig` and
   the artifact type.
2. **S1-1** (producer) — builds the artifact; S1-2/S1-3/S1-4/S1-5 all consume/extend it.
3. **S1-2** and **S1-3** in parallel after S1-1 (disjoint: S1-2 edits `stage_optimize.cpp`, S1-3 adds
   a pure accessor + hands the combine seam to Sprint 3).
4. **S1-4** then **S1-5** — S1-5's neutralize must handle S1-4's augmented block, so S1-4 lands first.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| `build_components` signature differs from assumed | S1-1 won't compile | Read `factor_model.hpp` `FactorComponents`/`build_components` decl at kickoff; assemble inputs to the ACTUAL signature (memory obs 7745/10632 confirm it returns `FactorComponents`). |
| Factor model changes the default book | Golden drift; determinism contract broken | `kind == Diagonal` MUST route to the exact `diagonal_risk_model` path; the off-path byte-identity test (S1-2) is the gate. If it fails, the routing leaked. |
| Exposure columns leak future data | Silent look-ahead inflates the scorecard | Every column reads only `[fit_end − lookback, fit_end)`; the PIT guard test (S1-1) perturbs future rows and asserts no change. |
| `model_for_period` cadence differs from the single-model semantics | Books shift on the diagonal path too | Diagonal returns one model for all periods (today's exact behavior); the piecewise-constant factor cadence is a Factor-only concern. |
| Dead-factor holdings source unavailable in the deploy panel | S1-4 can't load retired alphas | Gate S1-4 behind library availability; if the library holdings aren't present, `dead_alpha_factors` is a no-op with a logged warning (fail-open to the un-augmented model, documented in the ledger). |
| `mle_covariance` seam (S1-3) collides with Sprint 3's `stage_combine` edits | Merge conflict / double-wire | S1 ships only the PURE ACCESSOR + a ledger seam note; S1 does NOT edit `stage_combine.cpp`. Sprint 3 threads the call. Confirmed disjoint in the ROADMAP ownership matrix. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** the pinned `stage_optimize`/`stage_report` goldens and
  `FactoryOos.MineIntoOffPathDigestUnchanged` unchanged with default `RiskModelConfig`.
- **Per-task RED→GREEN:** each opt-in has a test RED before the wire and GREEN after.
- **Factor-model win, measured:** on the correlated-group fixture, record ex-ante factor risk
  `wᵀVw` and max-weight for {diagonal, factor, factor+dead} books — the factor model must show lower
  factor risk and lower crowded-pair gross at equal alpha (the concrete, quantified S1 claim).
- **Twice-run + seq==parallel** on the factor path.
- **Dev-panel smoke ≤5 min** with `--risk-model=factor` (the flag itself is threaded in Sprint 5;
  S1 proves the engine path via a direct-call integration test, not the CLI).

---

## Out of scope

- CLI flag `--risk-model` / `--dead-alpha-factors` / `--group-neutralize` — Sprint 5 (hub).
- Editing `stage_combine.cpp` to consume the cleaned covariance — Sprint 3 (owns the file); S1 ships
  the accessor + seam note only.
- The optimizer's cap-clip-renorm dollar-neutrality fix and capacity — Sprint 4.
- NCO / RMT-clustered data-driven sectors as the neutralization group source — future-work backlog;
  S1 uses the existing industry `group_map`.
- Re-deriving any estimator math in `src/risk/factor_model.cpp` — frozen; S1 calls it.
