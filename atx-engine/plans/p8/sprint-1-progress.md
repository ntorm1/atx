# p8 Sprint 1 — Risk-Model Covariance Spine — Progress Ledger

Base: main @ 8e5f0e9 (p8 ROADMAP + 5 sprint plans added; no p8 code yet).

Branch: feat/p8  Worktree: C:\atx-wt\p8

## Kickoff anchor re-confirmation (per the brief's ambiguity resolutions)

- `FactorModelBuilder::build_components` confirmed at `risk/factor_model.hpp:253-255`,
  returns `FactorComponents{X, F, D, fit_end}` (`factor_model.hpp:217-222`).
- `diagonal_risk_model` confirmed at `atx-impl/src/diag_risk.hpp:29-70` (`X=M×1 zeros`,
  `F=Identity(1,1)`, `D`=per-instrument TRI-return population variance floored at 1e-4).
- `stage_optimize.cpp:202` confirmed: `ATX_TRY(auto model, diagonal_risk_model(research));`
  is the sole risk-model call on the MVO path; `model_at` (`:225-227`) returns it for
  every period.
- `data/factor_model_artifact.hpp` confirmed EXISTING (S6.6, `atx::engine::data`
  namespace): `FactorModelArtifact{MatX X, MatX F, VecX D, usize fit_begin, fit_end}`,
  lowered via `data::artifact_to_factor_model` (`data/adapt_factor.hpp/.cpp`) which
  forwards to `risk::FactorModel::create` (single validation point). EXTENDED, not
  forked, per the brief.
- `multi_horizon.hpp:154-157` does NOT carry the `n_dead_factors` `NotImplemented`
  guard the brief's gap table cites (that region is `MultiHorizonConfig`/`Result`
  fields). The REAL guard is `FactorModelBuilder::build()` at
  `src/risk/factor_model.cpp:354-358` (dead rung, precedence over stat) and
  `build_components()` at `factor_model.cpp:374-378` (rejects stat/dead — dispatched
  by `build()`, not `build_components`). Both are in the FROZEN estimation-body file
  (`src/risk/factor_model.cpp`) S1 must NOT edit. S1-4/S1-5 do not need to touch
  either guard: `stage_riskmodel` calls `build_components` directly (never routes
  through `build()`), so the dead/stat dispatch in `build()` is simply never on our
  call path — augmentation happens via the separate free function
  `risk::augment_factor_model` (`risk/dead_factor.hpp`), which is NOT gated by those
  guards. Neutralization is `FactorModel::neutralize` (`factor_model.hpp:164`), a
  method on the ALREADY-ASSEMBLED model — also untouched by the `build()` guards.
  **Ledger note (deviation from brief's file:line, kept because the actual call path
  bypasses the cited guards entirely — no code in the guarded functions needs to
  change for S1-4/S1-5 to work).**
- **Architecture wrinkle not anticipated by the brief:** `FactorModelBuilder::build_components`
  consumes `atx::engine::PanelView` (`loop/panel_types.hpp`), a NON-OWNING zero-copy
  ring-buffer accessor over `RollingPanel` storage — NOT `atx::engine::alpha::Panel`
  (the flat, file-backed, date-major research panel `atx-impl` actually reads/writes).
  No existing bridge converts one to the other (`RollingPanel` is a live-loop
  construct; grep of `atx-impl` for `PanelView(` returns zero hits). Existing engine
  tests (`risk_factor_builder_test.cpp`'s `PanelFixture`) build a `PanelView` by
  owning a small `(fields_, mask_)` buffer and constructing a `PanelView` over it —
  this is the precedented, cold-path-acceptable pattern `stage_riskmodel.cpp` follows
  (own a per-fit-window buffer sized to the lookback, populate it by reversing
  `alpha::Panel`'s date-major order into `PanelView`'s newest-first order, construct
  the view, call `build_components`, discard the buffer). Documented here since it
  is a real design decision the brief's dependency map did not spell out.
- `sector_groups.hpp` (`atx-impl/src`) already builds a dense `group_map` from a
  panel's "sector" field for `S1-5`'s group_map need — reused, not re-derived.
- `RunConfig`/`stages.hpp`'s `run_optimize(const RunConfig&)` signature is UNCHANGED.
  `config.hpp`/`config.cpp` are Sprint-5-owned hub files; the brief's own "Out of
  scope" section confirms `--risk-model`/CLI threading is S5's job and S1 "proves the
  engine path via a direct-call integration test, not the CLI." `RiskModelConfig` is
  therefore a pure engine/impl-internal struct, never added to `RunConfig`. The
  no-flag `run_optimize` call is edited to route through a DEFAULT-CONSTRUCTED
  `RiskModelConfig` (inert ⇒ `Diagonal`), preserving byte-identical output; the
  Factor path is exercised by calling the new `stage_riskmodel`/`stage_optimize`
  helpers directly from tests, exactly as the brief's bench section prescribes.

## Unit checklist
- [x] S1-0  RiskModelConfig + FactorModelArtifact POD + ledger (this file)
- [ ] S1-1  stage_riskmodel producer (build_risk_model)
- [ ] S1-2  stage_optimize covariance-source swap
- [ ] S1-3  cleaned_alpha_cov accessor (combine seam; Sprint-3 handoff)
- [ ] S1-4  dead-alpha crowding factors
- [ ] S1-5  factor/industry neutralization reachable

## Determinism contract (every unit)
- `RiskModelKind::Diagonal == 0` (frozen enum index); inert default routes to the
  EXACT existing `diagonal_risk_model` path — byte-identical books/digests.
- `dead_alpha_factors=false` / `group_neutralize=false` ⇒ no augmentation / neutralize
  path unchanged.
- No RNG anywhere on S1's path (FactorModelBuilder estimation, dead-factor
  eigen-extraction, neutralize are all order-fixed deterministic).
- `oracle.hpp` untouched; no golden re-baseline.

## Log
S1-0: complete. RiskModelConfig (kind/dead_alpha_factors/group_neutralize/
fit_lookback_days/style toggles) added to risk/factor_model.hpp next to the
FactorModel apply-math it configures (no estimation-body edit); RiskModelKind::
Diagonal pinned at enum value 0 via static_assert + runtime test.
FactorModelArtifact (data/factor_model_artifact.hpp, EXTENDED not forked) gained
serialize_artifact/deserialize_artifact (flat little-endian header + column-major
f64 payload) + digest_artifact (atx::core::hash_bytes, wyhash). New suite
RiskModelConfigSpine (atx-engine/tests/risk/risk_model_config_test.cpp) 5/5 green:
DefaultsAreInert, DiagonalIsFrozenAtZero, ArtifactRoundTripByteIdentical (bit-exact
X/F/D/fit_begin/fit_end + re-lowers through the existing S6.6
artifact_to_factor_model seam), ArtifactDigestStableTwiceRun, ArtifactDigestChangesOnMutation.
RED verified first (17 compile errors: RiskModelConfig/RiskModelKind/
serialize_artifact/deserialize_artifact/digest_artifact all undeclared) before
any implementation was written.
Full gate: atx-engine-risk-tests 256/258 pass (2 pre-existing RobustPipelineE2E
failures, confirmed independent of this change — same two tests fail with
risk_model_config_test.cpp removed from the build; documented in p8 ROADMAP.md as
a pre-p8 backlog item); atx-engine-data-tests 118/121 pass (3 skipped, real-data/
operator-only tests, pre-existing); atx-impl-tests 202/206 pass (4 skipped,
pre-existing). Zero new failures, zero touched files outside the owned set.
