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
