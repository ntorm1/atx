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
- [x] S3-SEAM  S4-handoff participation unit fix (stage_combine.cpp:315,360)
- [x] S3-1  `fit_stack` wiring (forward-return label + meta + stack->weight bridge)
- [x] S3-2  admit-vs-fallback gate (mandatory)
- [x] S3-3  PIT HMM regime posterior -> `RegimeStack`
- [x] S3-4  `cleaned_alpha_cov` consumption behind `kind==Factor`
- [x] S3-5  determinism battery (consolidated)

## Determinism contracts (both apply this sprint, per the p8 contract)
- **(A) Opt-in** (S3-1..S3-4, all four): inert default (`method` stays `ShrinkageMv`;
  `risk_model.kind` stays `Diagonal`) ⇒ byte-identical to the pre-S3 combine golden.
- **(B) Correctness fix — documented exception** (S3-SEAM): the fix changes the capacity KVS
  numbers because the old ones were wrong (S4-1's twin bug). No combine DIGEST moves (participation
  never enters `combo.bin`/the hash) — recorded when landed.

## Log

S3-SEAM: complete [CORRECTNESS — the S4-1 handoff]. Fixed the identical participation
unit bug at its two S3-owned `stage_combine.cpp` sites (S4-1's ledger recorded these as an
open Sprint-3 dependency, confirmed still present at kickoff at the SAME lines the S4-1
ledger cited):
- `alpha_capacity_aum`: `part_per_aum = abs_w / (price * adv)` → `abs_w / adv` (drop `/price`).
  `price` is still read (gates out unpriced names) but no longer enters the ratio. Header
  comment above the loop corrected (`C = ... (|w_i|/ADV_i)^delta`, price term removed).
- `alpha_max_participation`: `part = (target_aum * abs_w / price) / adv` →
  `(target_aum * abs_w) / adv`. Doc comment above the function corrected.

**Contract-B correctness fix** (Sprint-4-progress.md's taxonomy): the corrected numbers
differ from today's (participation was inflated by a factor of `price`), but participation
feeds ONLY the capacity kvs telemetry (`capacity_alpha_aum`/`capacity_min_alpha_aum`/
`capacity_max_participation`, `stage_combine.cpp:737-768`ish) — never `combo.bin`/the hashed
panel digest — so the combine DIGEST is byte-identical before/after; only the capacity kvs
VALUES change.

**RED (`atx-impl/tests/stage_combine_participation_test.cpp`, new):** the S4-1
price-invariance method — two reversal-fixture panels (S6-1's `make_reversal_panel`,
proven to give `alpha_capacity_aum` a genuine positive edge, i.e. it reaches the
`part_per_aum` arithmetic rather than short-circuiting to 0/+inf) differing ONLY by a
uniform 8x price rescale (`close *= 8`, `volume /= 8` — an EXACT power-of-two rescale
chosen so returns/positions/PnL/dollar-ADV are floating-point IDENTICAL between the two
panels and only the raw share price differs). Pre-fix: `CapacityAumIsPriceInvariantForEqualNotional`
RED — `cap1=81600943927.20` (scale=1) vs `cap2=652807551417.58` (scale=8), ratio exactly 8.0
(fails the 1e-6 relative tolerance). `MaxParticipationIsPriceInvariantForEqualNotional` RED —
`part1=0.006252` vs `part2=0.000782`, ratio exactly 8.0 (fails).

**GREEN:** both price-invariance tests pass post-fix (capacity/participation now equal to
float-noise tolerance across the 8x price rescale — the buggy price-dependence is gone).
A third test, `CapacityKvsKeysAreEmittedButNotFoldedIntoDigest`, confirms a capacity-floor-on
run and a capacity-floor-off run of the SAME fixture produce the IDENTICAL combine digest
(the structural half of "no golden re-baseline needed" — participation never enters
`combo.bin`). Full regression sweep green: `StageCombineParticipation` (3/3, new),
`AtxImplCombine.*` (30/30, incl. `S61_RealizedEdgeCapacityNonZeroForPositivePnlAlpha`/
`S61_CapacityOffPathByteIdentical`/`CapacityRunIsDeterministic`/
`CapacityActivatesAndIsNonDegenerate` — none pins an exact price-dependent capacity NUMBER as
a frozen expectation, only sign/determinism/activation, so **no golden re-baseline needed**),
full `atx-impl-tests` suite (157/157 matched by the `AtxImpl` filter + the 30 `AtxImplCombine`
above + the new suite — 1 pre-existing environment-gated skip, `AtxImplDiscover.
W6_RediscoverLowVolCapacityAlpha`, unrelated to this fix).

S3-1: complete. `atx-impl/src/stage_combine.cpp` wires `CombineMethod::Stack` end-to-end;
`atx-impl/src/stage_combine.hpp` (new) declares the S3 direct-call overloads
`run_combine(cfg, combiner_cfg)` / `run_combine(cfg, combiner_cfg, risk_cfg)`, mirroring the
p8-S1-2 `run_optimize(cfg, risk_cfg)` seam exactly (RunConfig is untouched — config.hpp/.cpp are
Sprint-5-owned; the direct-call overload is how S3 threads `CombinerConfig`'s new stacking/regime
knobs + `RiskModelConfig` without a new RunConfig field). `method_from_string` gains `"stack"`/
`"regime-stack"` string arms (Sprint 5 wires the actual `--method` CLI values later — these arms
just make the string reachable at all). A new `method_to_string` reverse-map replaces the old
`cfg.method.empty() ? "shrinkage-mv" : cfg.method` label derivation (both the weights-sidecar
"method=" line and the kvs "method" field) — a direct-call test can set `combiner_cfg.method`
without ever populating `cfg.method`'s raw string, so the label must come from the RESOLVED enum;
verified byte-identical for the 5 legacy strings via the full regression sweep below.

**PIT design decision (load-bearing, not in the spec's literal pseudocode):**
`meta_features_from_pool` has NO fit-window parameter — it always emits over the WHOLE pool it is
given. To give Stack the SAME `[fit_begin, fit_end)` firewall `AlphaCombiner::fit` enforces, the
meta is built from a NEW `windowed_pool(pool, fit_begin, fit_end)` helper (a pure re-slice of the
already-PIT-correct pool's pnl/positions into a fresh `AlphaStore` re-indexed at 0 == fit_begin —
no recomputation) and a new `build_forward_returns_window(...)` helper that reads panel `close`
ONLY at window-local dates (`d+h >= fit_end` -> NaN, never `d >= fit_end` at all). Without this,
the stack's meta would span the WHOLE panel (including the report-stage OOS region past
`fit_end`), a genuine look-ahead the linear path's own firewall does not have. This seam is not in
the sprint doc's own wiring pseudocode (which passes `pool`/`streams` directly) — recorded here as
a necessary elaboration, pinned by `ForwardReturnLabelIsPitCausal` below.

**Stack -> weight bridge (documented choice, per the sprint's explicit either/or):** ships the
CONSERVATIVE order-fixed least-squares projection (new `weights_from_stack_projection`), NOT
`sc.pos_flat` directly. Rationale: every downstream consumer of `combo.weights` (the combined
mega-alpha blend, `--conviction`, `--kelly-fraction`, `--corr-penalty`/`--capacity-floor`, the
breadth telemetry) is built around "one weight per pool alpha"; shipping a raw position stream
would silently bypass ALL of them for Stack/RegimeStack — a much larger behavior change than a
wiring sprint should introduce. The projection solves the normal equations `G w = rhs` (`G` = the
pool's own per-alpha position Gram matrix over the fit window, `rhs` = each alpha's inner product
with the stack's position stream) via `solve_spd` on `G + ridge_lambda*I` (reusing `cfg.ridge_lambda`,
already an existing BoundedRegression knob — no new estimator math), then the SAME
`renorm_abs_sum` (Σ|w|=1) every other method's `Combination` carries. Pinned by
`TwiceRunByteIdenticalComboAndVerdictHash`.

**Telemetry:** `stack_verdict_hash`/`stack_admitted`/`stack_oos_dsr_nonlinear`/
`stack_oos_dsr_linear`/`stack_oos_ic_nonlinear`/`stack_oos_ic_linear` kvs, present ONLY on the
Stack/RegimeStack path (absent otherwise — every legacy method's kvs set is unperturbed). The
full telemetry set (not just `verdict_hash`) landed in this commit rather than S3-2's, since it is
a trivial addition once `stack_verdict` is in scope — a documented sequencing deviation; S3-2's
own commit is the GATE (`if (v.admitted) ... else fallback`) + its two decisive fixtures.

**RED:** `stage_combine_stack_test.cpp` (new) and `stack_meta_from_positions_test.cpp` (new,
`atx-engine/tests/learn/`) were written and run against the just-landed S3-0 tree (Stack already
routed to `combiner.fit()`'s new Err arm, since the branch didn't exist yet) — compiling only
after `stage_combine.hpp`/the dispatch branch were added. Two real numeric REDs surfaced on first
run against the FRESH wiring: (1) `ProducesWellFormedWeightsAndStableVerdictHash`'s `Σ|w|`
assertion at `EXPECT_NEAR(gross, 1.0, 1e-6)` failed by `1.4e-6` — the weights SIDECAR round-trips
through `operator<<`'s default (6-sig-fig) text precision before the test re-parses it via
`std::stod`, so the in-memory `renorm_abs_sum` bit-exactness does not survive the text round-trip;
loosened to `1e-4` (a test-fixture tolerance fix, not a production bug). (2) `ForwardReturnLabelIsPitCausal`
originally compared the STAGE's whole-panel `digest` — this genuinely FAILED
(`16988449166131861428` vs `17962650443509673934`) because step 9 applies the fitted weights to
EVERY panel date including the OOS region past `fit_end`, and the alpha DSL's OWN causal window
legitimately produces different positions there once those prices are perturbed — expected
behavior, not a leak, but the WRONG invariant for a PIT guard on the FIT step. Fixed by comparing
only the weights-sidecar's `w[a]=value` lines (the fitted output) instead of the whole-panel
digest — the narrower, correct PIT claim.

**GREEN:** all 3 `StageCombineStack` tests pass + `StackMetaFromPositions` (1/1). Full regression
sweep green (207 tests): `AtxImplCombine`/`StageCombineParticipation` (30, unaffected — the 5
legacy methods still resolve through the unchanged `combiner.fit()` else-branch),
`AlphaCombiner`/`CombineMethodEnumLayout`/`RegimeCombine`/`Phase4Integration` (22),
`StageCombineStack` (3, new), `StackMetaFromPositions` (1, new), plus the broader `AtxImpl*` suite
(171). No golden re-baseline needed (Stack/RegimeStack have zero call sites in any pre-existing
test).

S3-2: complete [MANDATORY]. Refactored the S3-1 branch's inline body into a standalone
`atx::impl::fit_stack_combo(pool, close_all, ni, fit_begin, fit_end, combiner_cfg, regime)`
(declared in `stage_combine.hpp`, defined in `stage_combine.cpp`) so the decisive admit/reject
fixtures can drive it directly against a HAND-BUILT `combine::AlphaStore` pool — bypassing the DSL/
VM entirely, the same technique `ensemble_test.cpp` uses for `fit_stack` itself — independent of
whether a real alpha DSL expression happens to produce interaction/linear structure. Wrapped the
S3-1 body in the honest-gate: `if (verdict.admitted) { ship the stack's projected weights } else {
combine::AlphaCombiner{} /* default cfg: method==ShrinkageMv */ .fit(pool, fit_begin, fit_end) }` —
the fallback deliberately uses a FRESH default `CombinerConfig`, never `combiner_cfg` (whose
`.method` is Stack/RegimeStack and would hit `AlphaCombiner::fit`'s S3-0 Err arm).

**RED (`atx-impl/tests/stage_combine_stack_gate_test.cpp`, new):** `build_gate_fixture` mirrors
`ensemble_test.cpp`'s `linearly_combinable_meta`/`nonlinear_interaction_meta` fixtures (identical
Lcg, identical `Y = 0.6·c0+0.4·c1+0.05·noise` / `Y = sign(c0)·sign(c1)+0.10·noise` label functions,
identical 48-date/14-instrument/4-feature/seed shape) but encodes the label into a SYNTHETIC
close-price series (`close[d+1] = close[d]·(1+Y)`, horizon=1) instead of a hand-built
`FeatureMatrix`, so `fit_stack_combo`'s REAL wiring (`windowed_pool` +
`build_forward_returns_window` + the gate + the projection) is exercised end-to-end, not merely
`fit_stack` in isolation. First run of `RejectsOnLinearFixtureAndFallsBackByteIdenticalToShrinkageMv`
was a genuine RED: `fit_stack_combo` itself returned
`Err("solve_spd: matrix is not positive-definite")` — the fixture's per-alpha PnL stream (needed
only by the FALLBACK's `AlphaCombiner::fit`, which reads PnL, never positions) was left all-zero
(`AlphaStore` is position/label-agnostic; PnL is irrelevant to the stack path proper), so
Ledoit-Wolf shrinkage had nothing to shrink — a degenerate all-zero sample covariance stays exactly
zero regardless of shrinkage intensity, and Cholesky fails. Fixed by giving each fixture alpha a
small-amplitude, non-degenerate PnL proxy (`0.001 * <its own instrument-0 column value>` per date)
— irrelevant to the stack's own admit/reject math, but enough variance for ShrinkageMv's Cholesky
to succeed.

**GREEN:** all 3 new tests pass. `AdmitsOnInteractionFixture`: `v.admitted==true`,
`oos_ic_nonlinear > oos_ic_linear`, `oos_dsr_nonlinear > 0`, and the shipped combo is a well-formed
`Σ|w|=1` projection (not the fallback). `RejectsOnLinearFixtureAndFallsBackByteIdenticalToShrinkageMv`:
`v.admitted==false`, `reason==RejectFitness`, and the shipped combo is `EXPECT_EQ` (bit-exact) to a
SEPARATE plain `AlphaCombiner{}.fit(pool, 0, n_periods)` call on the identical pool/window — the
fallback fires and matches `--method shrinkage-mv` exactly, not an approximation.
`StackCpcvConfigIsThreadedFromCombinerConfig` (the wiring half of "stack_gate_purges_cpcv" — CPCV's
own purge/embargo CORRECTNESS is `eval::cpcv_folds`'s frozen-math contract, already covered by
`atx-engine/tests/eval/eval_cpcv_test.cpp` and exercised for real by every existing `fit_stack`
call in `ensemble_test.cpp`; what S3 additionally owns is that `CombinerConfig.stack_cpcv_embargo`
actually reaches `StackingCfg.cpcv` rather than being silently ignored): `embargo=0.0` vs
`embargo=0.9` on the SAME interaction fixture produce DIFFERENT `verdict_hash`es, proving the
config genuinely threads through. Full regression sweep green (210 tests, incl. all three new
`StageCombineStackGate` tests + the untouched S3-0/S3-1/S3-SEAM suites).

S3-3: complete. Extended `fit_stack_combo`'s signature from a caller-supplied `const learn::Hmm*`
to a `bool with_regime` flag (Stack=false, RegimeStack=true) — see the reasoning below for why a
pre-fit external Hmm parameter was the WRONG shape for this unit.

**Architecture finding (binding, deviates from the sprint doc's literal wiring pseudocode):**
`learn::fit_stack`'s regime-conditional arm (`ensemble_detail::fit_regime_nonlinear`) does NOT take
the caller's regime observable as an input — it RE-DERIVES `ensemble_detail::regime_observable(meta)`
internally (the cross-sectional mean of the meta's LAST feature column) and calls
`ensemble_detail::date_regimes(*regime, obs)` on THAT. This means the `Hmm*` passed to `fit_stack`
MUST have been fit on that exact same series, or the composition is statistically meaningless
(a fitted emission model applied to an unrelated series). The sprint doc's wiring pseudocode
("fit baum_welch on the regime observable — the panel's cross-sectional regime marker series, or
the loaded macro series when present") would have produced an INCOMPATIBLE Hmm for the nonlinear
arm. Resolution: `fit_stack_combo` builds `meta` first (S3-1's own windowed_pool +
build_forward_returns_window + meta_features_from_pool), THEN — only when `with_regime` — derives
`obs = learn::ensemble_detail::regime_observable(meta)` and fits `stage_regime.hpp`'s
`fit_regime_hmm(obs, hcfg)` (hcfg.n_states = `combiner_cfg.regime_n_states`, seed =
`combiner_cfg.stack_master_seed`) on THAT. The SAME `(hmm, obs)` pair then serves: (a) the
nonlinear arm (`fit_stack(meta, &hmm, scfg)` — self-consistent by construction), and (b) the
NEW regime-conditional LINEAR fallback (below).

**`stage_regime.cpp` extension (append-only, `run_regime` completely untouched):** added
`atx::impl::fit_regime_hmm(obs, cfg) -> Hmm` (new `stage_regime.hpp`), a thin, directly-testable
wrapper around `learn::baum_welch`, deliberately OBSERVABLE-AGNOSTIC — the header's doc block
records exactly why RegimeStack's specific observable is not a free parameter (see above) and
notes a future sprint could reuse this SAME function on a loaded-macro-series observable without
touching this file again.

**RegimeStack's fallback is regime-conditional linear, not flat linear (a new design decision, not
in the spec's literal text):** on non-admit, Stack falls back to the flat `AlphaCombiner{}.fit`
(S3-2, unchanged); RegimeStack instead falls back to `combine::fit_regime_combiner` +
`RegimeCombiner::blend`: `learn::ensemble_detail::date_regimes(hmm, obs)` gives WINDOW-LOCAL PIT-
argmax labels (length == the fit window), placed into a GLOBAL length-`pool.n_periods()` vector at
offset `fit_begin` (positions outside the window are never read — `fit_regime_combiner`'s own
contract); the SHIP-TIME blend posterior is `regime_posterior_at(hmm, obs, wlen-1)` — "as of" the
last in-window date, mirroring every other method's `[fit_begin, fit_end)` convention. Rationale:
"RegimeStack = stack + regime" per the sprint's architecture note — the regime overlay should
condition BOTH arms, not just the ones that admit; a pool whose optimal combination differs by
regime should track that even when the nonlinear base does not clear the S3-2 gate.

**The single-state fallback guard composes from three ALREADY-frozen guarantees, not a special
case coded in S3-3:** (1) `fit_stack`'s `fit_regime_nonlinear` with `n_states==1` puts every row in
ONE partition, so its unioned `oos_score_series`/`trial_count` equal `fit_flat_nonlinear`'s exactly
— same verdict. (2) `stack_to_candidate`'s `deploy_nonlinear` NEVER reads regime at all (documented
in `ensemble.hpp`) — admit-path weights are bit-identical regardless. (3)
`combine::fit_regime_combiner`+`RegimeCombiner::blend` with `n_regimes==1` is `regime_combiner.hpp`'s
OWN documented byte-identical reduction to `AlphaCombiner{}.fit`. Composing (1)+(2)+(3): RegimeStack
with `regime_n_states==1` is byte-identical to the corresponding Stack call on EITHER the admit or
the fallback branch — verified empirically, not merely argued (see GREEN below).

**RED->GREEN:** all four new atx-impl tests (`stage_combine_regime_test.cpp`) and the engine-level
`regime_stack_wire_test.cpp` passed on the FIRST run (the theoretical composition argument above held
exactly) — no numeric surprises this unit. `SingleStateByteIdenticalOnAdmitPath` /
`...OnFallbackPath`: `fit_stack_combo(..., with_regime=false)` vs `(..., with_regime=true, regime_n_states=1)`
on the SAME interaction / linear pool produce `EXPECT_EQ` bit-exact weights + identical verdict on
BOTH the admit and the fallback path. `TwiceRunByteIdentical`: a genuine 2-state RegimeStack fit is
twice-run bit-exact (verdict_hash + weights). `regime_posterior_pit_guard`: ALREADY COVERED by the
pre-existing, unchanged `Hmm.Posterior_FitOnTrailing_Causal_TruncationInvariant`
(`atx-engine/tests/learn/hmm_test.cpp:233`) and `RegimeCombine.PosteriorPath_TruncationInvariant`
(`combine_regime_combiner_test.cpp:208`) — both frozen, both cited rather than duplicated.

**`regime_combine_partitions_by_posterior` (new `atx-engine/tests/combine/regime_stack_wire_test.cpp`,
the "regime win, measured" deliverable):** a genuinely NEW composition
`combine_regime_combiner_test.cpp` did not cover — a REAL `baum_welch` fit (not hand-supplied
labels) on a 2-regime marker series (120 dates, 20-period runs, TRAIN=[0,80) HMM-fit + fit_regime_combiner,
TEST=[80,120) one run of each regime) scored PER-DATE via `regime_posterior_at` + `RegimeCombiner::blend`
against a single flat `AlphaCombiner` fit on the identical data. **Measured:** OOS mean return
regime=0.005 vs flat=0.0025 (exactly 2x — the flat 50/50-ish blend dilutes each regime's true
winner by roughly half); OOS Sharpe regime=5.0 vs flat=2.499. The regime-conditional book beats the
flat fit on both metrics, as required.

Full regression sweep green (219 tests): `StageCombineRegime` (3, new), `RegimeStackWire` (1, new),
plus every S3-0..S3-2/S3-SEAM suite unchanged.

**Deferred (documented, out of critical path):** genuine `seq==parallel` proof for the regime
partition / OOF walk (each fold/partition IS independent by construction — `fit_regime_nonlinear`
loops regimes independently accumulating into one series; `cpcv_folds` folds are independent — but
an actual DetPool-driven parallel-vs-sequential run was not built for this unit; folded into S3-5's
determinism battery, which is the sprint's designated home for this proof.

S3-4: complete. `run_combine`'s three-arg overload (`stage_combine.hpp`, already declared with the
S3-4 doc block in place from the S3-1/S3-3 header work) threads `risk::RiskModelConfig` through two
call sites, both gated on `risk_cfg.kind == risk::RiskModelKind::Factor` (default `Diagonal`,
inert):

  1. **Step 8a dispatch (the shipped weight fit):** a NEW parallel function
     `fit_shrinkage_mv_cleaned_cov(pool, fit_begin, fit_end)` (declared in `stage_combine.hpp`,
     defined in `stage_combine.cpp`), reached only when `cm == ShrinkageMv && risk_cfg.kind ==
     Factor`. It reuses the EXACT same public inputs `combine::detail::fit_shrinkage_mv` itself
     reads — `combine::detail::window_means` + `combine::detail::complete_case_centered` over the
     SAME `[fit_begin, fit_end)` window — but swaps the covariance source for
     `atx::engine::data::cleaned_alpha_cov(centered)` (the S1-shipped LW constant-correlation
     shrink + Marchenko-Pastur eigen-clip + strict-PD floor pipeline) in place of the raw
     complete-case MLE `mle_covariance`, then solves `Σ̂w = μ` via the same `solve_spd` +
     `renorm_abs_sum` (Σ|w|=1) convention every method uses.
  2. **Step 12 breadth instrumentation:** the effective-breadth covariance is now `(risk_cfg.kind
     == Factor) ? data::cleaned_alpha_cov(centered) : combine::detail::mle_covariance(centered,
     na)` — so the telemetry stays coherent with whichever covariance actually shipped the weights,
     rather than always reporting the raw-MLE breadth even when the Factor-cleaned covariance was
     what was actually solved against.

**Deliberately NOT a `combiner.hpp` edit (ownership boundary + anti-double-shrink rationale):**
`combine/combiner.hpp` is append-only under this sprint's ownership boundaries, and
`AlphaCombiner::fit`'s internal `fit_shrinkage_mv`/Ledoit-Wolf pipeline is frozen-ish, reused
verbatim by every other method's covariance need. Editing it in place to swap in
`cleaned_alpha_cov` would either (a) double-shrink — LW-to-scaled-identity THEN MP-eigen-clipping
the SAME matrix, an uncontrolled, undocumented estimator composition never validated together — or
(b) require a new conditional branch inside frozen combiner internals, an ownership violation. A
separate, parallel function in `stage_combine.cpp` (S3's own file) keeps the two covariance
pipelines cleanly disjoint: `risk_cfg.kind==Diagonal` (default) reaches `AlphaCombiner::fit`
untouched; `risk_cfg.kind==Factor` reaches `fit_shrinkage_mv_cleaned_cov` instead, never both.

**`factor_model_artifact.hpp` untouched (read-only accessor, per the hard constraint):** only
`data::cleaned_alpha_cov(const MatX&) -> MatX` is called, exactly as S1 shipped it (a pure,
non-fallible free function of the column-demeaned `centered` matrix — NOT a
`FactorModelArtifact::cleaned_alpha_cov(fit_begin,fit_end)` method, correcting the sprint doc's
pseudocode, as already noted at kickoff).

**Determinism classes, evidence (`atx-impl/tests/stage_combine_cleaned_cov_test.cpp`, suite
`StageCombineCleanedCov`):**

  * **(a) off-path byte-identity** — `DiagonalKindByteIdenticalToLegacyPath`: a real DSL/panel
    fixture (mirrors `stage_combine_stack_test.cpp`'s technique) run through the pre-S3-4 zero-arg
    `run_combine(cfg)` legacy entry point vs the new three-arg overload called with an EXPLICIT
    `risk::RiskModelConfig{}` (`kind==Diagonal`, the enum's own default): `combo.bin` digest AND the
    full weights-sidecar BYTES are identical between the two calls.
  * **(b) on-path RED->GREEN** — `FactorKindWiringIsLiveAndReportsMeasuredDiversification`: a
    hand-built `combine::AlphaStore` pool reproducing `risk_cleaned_alpha_cov_test.cpp`'s OWN N~T
    "spurious large eigenvalue" fixture (T=20, N=18: a common sinusoid + small idiosyncratic noise
    + one large near-independent outlier column — the noise-dominated regime the S1 accessor was
    built to tame) proves `fit_shrinkage_mv_cleaned_cov`'s weights genuinely differ from
    `combine::AlphaCombiner{}.fit`'s (the `kind==Diagonal` path) on the SAME pool/window — a
    no-op wire would leave them identical. **Measured** (printed via the test's own
    `std::cout`): `max|w|_diagonal=0.164248` vs `max|w|_factor=0.142554` — the cleaned-covariance
    ShrinkageMv fit is MORE diversified (lower max weight) than the plain LW-shrunk-to-identity
    fit on this fixture, the same direction `risk_cleaned_alpha_cov_test.cpp`'s own
    `ShrinksSpuriousEigenvalueAndDiversifies` proves against the raw (unshrunk) sample covariance —
    here demonstrated against the ALTERNATIVE a caller actually gets by not opting in (the
    already-LW-shrunk `Diagonal` path), a strictly harder bar. `EXPECT_LT` passed on the FIRST run
    (no fixture retuning needed).
  * **(c) twice-run** — `TwiceRunByteIdentical`: `fit_shrinkage_mv_cleaned_cov` called twice on the
    identical pool/window produces bit-exact weights and `fit_begin`/`fit_end`.
  * **(d) seq==parallel** — N/A for this unit, documented rather than vacuously tested:
    `cleaned_alpha_cov` introduces no new parallel dimension (it is a single fixed-order pure
    function of `centered`, already proven deterministic by S1's own
    `CleanedAlphaCov.PureAndDeterministic`, `risk_cleaned_alpha_cov_test.cpp`), and the weight solve
    (`solve_spd` + `renorm_abs_sum`) is the SAME already-frozen sequential helper every other method
    uses. Folded into S3-5's determinism battery for a final blanket re-confirmation alongside the
    other four opt-in methods, per that unit's stated scope.

Full regression sweep green (225 tests): `StageCombineCleanedCov` (3, new) + `CleanedAlphaCov` (3,
S1's own, now in-scope by suite-name regex) + every S3-0..S3-3/S3-SEAM suite unchanged.

S3-5: complete. No new engine code (per the sprint doc's own scope for this unit) — tests +
this ledger row, consolidating the whole sprint's determinism contract. New suite
`CombineDeterminismBattery` (`atx-impl/tests/stage_combine_determinism_battery_test.cpp`) covers
the two accept-criteria bullets NOT already discharged by an earlier unit's own RED->GREEN test;
the remaining three bullets are RE-CONFIRMED (not re-implemented) via this unit's regression sweep.

**New this unit:**

  * **`combine_default_byte_identical`** —
    `DefaultByteIdenticalAcrossAllFiveLegacyMethodsAndDefaultString`: loops over all six
    `--method` strings the CLI accepts (`""`, `shrinkage-mv`, `equal`, `rank`, `ic`, `bounded`) on
    the SAME DSL/panel fixture, comparing the pre-S3 zero-arg `run_combine(cfg)` legacy entry point
    against the new three-arg overload called with explicit default `CombinerConfig`/
    `RiskModelConfig{}` (`kind==Diagonal`): `combo.bin` digest AND the full weights-sidecar bytes
    are identical for EVERY method, on the FIRST run. This is the sprint-wide generalization of
    S3-4's own `DiagonalKindByteIdenticalToLegacyPath` (which covered only `shrinkage-mv`) to all
    five legacy methods + the empty-string default, proving the S3-0 enum append and the S3-4 new
    dispatch branch left every pre-existing method byte-for-byte untouched.
  * **`stack_seq_eq_parallel` / `regime_seq_eq_parallel`** — `StackSeqEqParallel` /
    `RegimeStackSeqEqParallel`: four independently-seeded hand-built pools (the "each
    fold/partition is independent" premise), each fit via `fit_stack_combo`, once in a plain
    sequential loop and once dispatched across `atx::engine::parallel::DetPool::parallel_for`
    (4 workers). Per-index `verdict_hash`, `admitted`, and `combo.weights` are bit-exact between
    the two execution substrates, for BOTH `with_regime=false` (Stack) and `with_regime=true`
    (RegimeStack, `regime_n_states=2`) — both passed on the FIRST run. **Scope note (an honest
    reading of the spec's literal wording, recorded rather than glossed over):** `learn::fit_stack`
    (frozen, `ensemble.cpp`) contains NO internal `DetPool`/thread dispatch of its own — its CPCV
    fold loop and (for RegimeStack) its per-regime partition loop are both single-threaded
    sequential reductions over a fixed-order collection, so there is no INTERNAL parallel
    execution substrate inside the frozen stacking math to compare against a sequential one. What
    IS genuinely testable, and what these two tests prove, is the practically load-bearing claim:
    dispatching multiple INDEPENDENT `fit_stack_combo` invocations concurrently via the engine's
    own `DetPool` (each call sharing nothing but const-ref inputs — no global RNG, no static/shared
    mutable state) cannot cross-contaminate results — the same guarantee a future sprint's
    fold-level-parallel `fit_stack` would need to uphold, verified here at the call-site level the
    engine actually exposes today.

**Re-confirmed (already proven by an earlier unit's own RED->GREEN test; re-run here, not
re-implemented):**

  * `stack_twice_run` — `StageCombineStack.TwiceRunByteIdenticalComboAndVerdictHash` (S3-1).
  * `regime_stack_twice_run` — `StageCombineRegime.TwiceRunByteIdentical` (S3-3).
  * `combine_method_enum_layout_pin` — `CombineMethodEnumLayoutPin` (S3-0,
    `combine_method_enum_layout_pin_test.cpp`): `Stack==5`, `RegimeStack==6`, `sizeof==1` still
    hold.
  * `FactoryOos.MineIntoOffPathDigestUnchanged` (factory/S4-owned, read-only here) — the
    combine-adjacent off-path golden this sprint must not perturb — re-run individually and
    confirmed green (`atx-engine/tests/factory/factory_oos_test.cpp`).

Full regression sweep green (264 tests, expanded regex adding `CombineDeterminismBattery` +
`FactoryOos`): every suite from S3-0 through S3-4 unchanged, plus the 3 new
`CombineDeterminismBattery` tests and the full `FactoryOos` suite (39 tests) all passing.
