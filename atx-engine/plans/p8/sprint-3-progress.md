# p8 Sprint 3 — Nonlinear & Regime-Aware Combination — Progress Ledger

Base: feat/p8 @ 5adfe76 (S2-5, last commit before S3 kickoff).

Branch: feat/p8  Worktree: C:\atx-wt\p8

## Kickoff — confirmed frozen call surfaces (re-read against the CURRENT tree)

- `learn::meta_features_from_pool(pool, forward_returns_flat, horizons)` (ensemble.hpp:255) —
  emits ONE row per (date, instrument) cell UNCONDITIONALLY (no universe gating — AlphaStore
  carries no universe concept), column f = `pool.positions(AlphaId{f}, date)[inst]`,
  `row_valid[row]` = all-columns-finite (not a universe flag). Confirmed by reading
  `src/learn/ensemble.cpp:123-155` (frozen, read-only).
- `learn::fit_stack(meta, const Hmm*, StackingCfg) -> StackingVerdict` (ensemble.hpp:271) — same
  meta/CPCV-scored linear-vs-nonlinear gate; `regime != nullptr` routes the nonlinear arm through
  `fit_regime_nonlinear` (per-regime partition of the SAME meta via `ensemble_detail::regime_observable`
  + `date_regimes`, both PUBLIC — not `.cpp`-local — symbols in `namespace ensemble_detail`).
- `learn::stack_to_candidate(verdict, meta, cfg) -> StackCandidate` (ensemble.hpp:318) — deploys a
  FLAT (non-regime) refit regardless of how the verdict was scored; `pos_flat` is period-major,
  instrument-minor, length `n_dates*n_instruments`.
- `learn::baum_welch(obs, HmmCfg) -> Hmm` (hmm.hpp:252), `learn::regime_posterior_at(hmm, obs, d)`
  (hmm.hpp:296, PIT forward-only filter over a COPIED causal prefix).
- `combine::fit_regime_combiner(pool, labels, n_regimes, fit_begin, fit_end, cfg)` /
  `RegimeCombiner::blend(posterior)` (regime_combiner.hpp:107, 86) — `n_regimes==1` guaranteed
  byte-identical to `AlphaCombiner{cfg}.fit(...)` (regime_combiner.hpp's own documented contract).
- **`data::cleaned_alpha_cov` — CORRECTED SIGNATURE** (S1-3, landed): the ACTUAL symbol is a pure
  free function `atx::engine::data::cleaned_alpha_cov(const MatX& centered) -> MatX`
  (`factor_model_artifact.hpp:262`) — NOT `FactorModelArtifact::cleaned_alpha_cov(fit_begin, fit_end)`
  as the sprint doc's illustrative pseudocode assumed (that pseudocode predates S1's actual landing
  shape). It takes the EXACT SAME `centered` (T×N, column-demeaned) matrix
  `combine::detail::mle_covariance(centered, na)` already takes — a genuine drop-in swap, simpler
  than the spec anticipated (no FactorModelArtifact/fit-window threading needed at the call site).

## S3-0: complete. Enum append + CombinerConfig fields + enum-layout pin.

`atx-engine/include/atx/engine/combine/combiner.hpp`:
- `CombineMethod` gains `Stack = 5`, `RegimeStack = 6`, appended immediately after
  `BoundedRegression = 4` (frozen order unchanged: `EqualWeight==0` .. `BoundedRegression==4`).
  `sizeof(CombineMethod) == 1` (still `: atx::u8`).
- `AlphaCombiner::fit`'s exhaustive switch (no `default:`) gains explicit `Stack`/`RegimeStack` arms
  returning `Err(InvalidArgument, "... stage-dispatched, not fit via AlphaCombiner")` — they are
  intentionally NOT folded into a catch-all so a THIRD future method with no arm is still a compile
  error (the switch stays an exhaustive gate).
- `CombinerConfig` gains six S3 knobs at struct end (append-only, no aggregate-init breakage):
  `stack_master_seed=0`, `stack_cpcv_groups=6`, `stack_cpcv_test_groups=2`, `stack_cpcv_embargo=0.01`,
  `stack_horizon=1`, `regime_n_states=3`. These are the ONLY combine-side stacking/regime cfg; the
  stage translates them into the learn-domain `StackingCfg`/`HmmCfg` (S3-1/S3-3).

**Deviation (discovered, not anticipated by the spec's dependency map): a SECOND exhaustive switch
over `CombineMethod` exists** at `combine/combined_source.hpp:213` (`CombinedSignalSource::evaluate`,
the P4-5 apply-side live blend) — RED confirmed it (a genuine `-Werror -Wswitch` compile failure,
`enumeration values 'Stack' and 'RegimeStack' not handled in switch`) on the FIRST build attempt
after appending the enum. `combined_source.hpp` is not in S3's explicit owned-file list, but an
append-only enum change forces every exhaustive switch over it to acknowledge the new arms —
unavoidable ripple, same category as the `AlphaCombiner::fit` switch the spec DID anticipate. Fix:
added `Stack`/`RegimeStack` to the SAME case group as `BoundedRegression` (`blend_linear`) — once
S3-1 deploys a stack as a per-alpha weight vector (the projection bridge), applying it is
IDENTICAL to applying any other linear combo; no new apply-side math. Documented at the call site.

**RED (`atx-engine/tests/combine/combine_method_enum_layout_pin_test.cpp`, new):** written FIRST
against the pre-S3-0 tree — compile failure, `no member named 'Stack' in ... CombineMethod` (+ 5
more missing `CombinerConfig` fields). Captured via the first `p8-build.ps1` run (12 compile errors).

**GREEN:** enum append + Err arms + cfg fields land; `combine_method_enum_layout_pin_test.cpp`'s
3 tests pass (`FrozenIndicesRuntimeCheck`, `CombinerConfigDefaultsAreInert`,
`FitRejectsStackAndRegimeStackAsStageDispatched`). Full regression sweep green (193 tests):
`atx-engine-combine-tests` + `atx-engine-learn-tests` (22 tests incl. `AlphaCombiner.*` byte-identity
for the 5 legacy methods, `RegimeCombine.SingleRegime_ByteIdenticalToAlphaCombiner`,
`Phase4Integration.Firewall_CombinerFutureRowsCorrupted_WeightsByteIdentical`) + the FULL
`atx-impl-tests` suite (193/193 — confirms the enum append does not silently break any existing
`atx-impl` call site; `stage_combine.cpp` has no exhaustive `CombineMethod` switch of its own today,
only the `method_from_string`/dispatch `if` chain S3-1 extends next).

## Unit checklist
- [x] S3-0  Ledger + `CombineMethod::Stack`/`RegimeStack` + `CombinerConfig` fields + enum pin
- [ ] S3-SEAM  S4-handoff participation unit fix (stage_combine.cpp:315,360)
- [ ] S3-1  `fit_stack` wiring (forward-return label + meta + stack->weight bridge)
- [ ] S3-2  admit-vs-fallback gate (mandatory)
- [ ] S3-3  PIT HMM regime posterior -> `RegimeStack`
- [ ] S3-4  `cleaned_alpha_cov` consumption behind `kind==Factor`
- [ ] S3-5  determinism battery (consolidated)

## Determinism contracts (both apply this sprint, per the p8 contract)
- **(A) Opt-in** (S3-1..S3-4, all four): inert default (`method` stays `ShrinkageMv`;
  `risk_model.kind` stays `Diagonal`) ⇒ byte-identical to the pre-S3 combine golden.
- **(B) Correctness fix — documented exception** (S3-SEAM): the fix changes the capacity KVS
  numbers because the old ones were wrong (S4-1's twin bug). No combine DIGEST moves (participation
  never enters `combo.bin`/the hash) — recorded when landed.

## Log
