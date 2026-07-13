# Merge report: codex/correctness-first-surface-v2 → main (2026-07-12)

Merge base: 726222a. Main side: e2e fitting/pricing/greeks sprint + follow-ups
(c7249a5..bc23d66, incl. b80a1eb C8 v_min de-saturation — confirmed an ancestor
of HEAD). Branch side: correctness-first surface pipeline (dual MarketMark/Risk
surfaces, independent admission oracle) + five-task reviewed fix pass
(c83c9fe..88fff30).

43 conflicted paths (not 46): 19 UU in atx-vol, 21 AA in atx-ui, plus
CMakeLists.txt and .superpowers/sdd/progress.md. This report covers every
non-trivial resolution and each semantic judgment call.

## Global structure of the two sides

Both sides independently rewrote large parts of the same facade from the same
base:

- **Main** canonicalized fitting preparation (`PreparedSlice` /
  `prepare_expiry`, 1a9d007), added transactional surface admission
  (`SurfaceBuildReport` / `FitAdmissionPolicy` / independent-invariant
  evaluator, 5abae0b + da718f7) and transactional expiry refits
  (`refit_expiry`, a73b5cd + 9b3c4c6), exact Greeks/price seams
  (priced_surface / american_batch / portfolio — these files merged cleanly),
  QP KKT certification (50956be), SpiderRock band stats + failed-calendar-check
  honesty (8bedc63), interval-loss capacity guard (727adde), and
  `parallel_for_dynamic` exception propagation (9458c29).
- **Branch** built the dual-surface pipeline: `SurfacePolicy` / `SurfaceState` /
  `FitQualityMode` / `SurfaceOutputs`, the independent risk oracle
  (`validate_risk_surface` + `decide_risk_surface_admission` +
  `ValidationDigest` with CarryGap = 1u<<11), certified price-to-IV inversions
  (`DeAmAuditDiagnostics` / `deam_inversion_certified`), robust multi-pair
  carry, fail-closed serving defaults, the parallel de-Am prepass certification
  capture (perf C1), copy-on-write local risk refits (`refit_risk_slice`), and
  persistence wiring (`SurfaceProvenance`).

Files exclusively owned by one side (surface_policy.*,
risk_surface_validation.*, fit_policy.*, curve_selector.*, priced_surface.*,
portfolio*, american_batch.*, corpus_board_fit.*, prepared_fitting.* [base],
deamer.*, surface_archive.*, surface_db.*) merged cleanly and are not discussed
further; both sides' features in them survive untouched.

## Trivial / mechanical resolutions

- **atx-ui/** (21 AA files): branch's tree taken wholesale. Main's atx-ui came
  from a WIP snapshot commit (3744e02); the branch imported the same workspace
  and evolved it (v2 quality modes, admitted-risk status line, `--quality`
  flag, `-DataPath` param). Verified by diff: branch = main's content + strict
  evolution.
- **CMakeLists.txt (root)**: branch's guarded `if(ATX_BUILD_UI)
  add_subdirectory(atx-ui)` over main's unconditional add. The branch's review
  pass deliberately kept the UI opt-in (documented deferral); the `ATX_BUILD_UI`
  option merged cleanly.
- **.superpowers/sdd/progress.md**: union — both sides' sprint records kept in
  order (SpiderRock sprint section + review-fix pass section).
- **atx-vol/bench/CMakeLists.txt**: union — main's `atx-vol-fitting-bench` and
  branch's `atx-vol-surface-v2-bench` both kept.
- **calib.hpp (ObsSet)**: union — main's `n_score_inversions` + branch's
  `deam_audit`.
- **curve_fit.hpp**: branch's include block (superset); `CurveSurfaceReport`
  union — main's `n_score_inversions` + branch's `n_carry_skipped` +
  `input_certification`.
- **pricer_fitter.hpp**: union of private members — main's `published_report_`
  / `last_attempt_report_` / `published_provenance_` + branch's
  `served_decision_`.
- **session.hpp**: union — main's `ParityDiagnosticState` enum + `parity_state`
  field; branch's `IncrementalRefitDiagnostics`, expanded `SessionDiagnostics`
  (carry/inversion certification counters incl. `n_price_bound_violations`),
  `SessionCarryDiagnostics`, `SessionSliceDiagnostics`.
- **dense_slice.hpp**: union — main's QP-certificate fields (`qp_iterations`,
  `qp_stationarity`, `qp_primal_violation`, `qp_complementarity`,
  `qp_dual_violation`) + `kMaxIntervalSlackRows`; branch's `effective_lambda` /
  `noise_scale` fields + `ConvexFitContext`. `fit_convex_slice` gets main's
  stricter documented contract + branch's `ConvexFitContext` parameter.
- **arb.cpp**: union of anonymous-namespace helpers — main's
  `butterfly_scan_slice` + branch's `shared_grid_gap` /
  `validate_pair_projection_inputs`.
- **tests: calib_test.cpp / dense_slice_test.cpp / pricer_fitter_test.cpp**:
  both sides' new tests kept (rule 5). No test deleted.

## Semantic judgment calls (rules 2/3/6)

### 1. calib.cpp `build_observations_european` — OTM shortcut audited (rule 2)

Base and main pushed shortcut rows into the fit set via an early-`continue`
with the RAW row (American mid retained). The branch deliberately replaced that
with flow-through: shortcut proposals get `sig = Ok(sigma_mkt)` and then pass
the same cold-reference audit as every other Andersen-Lake route
(certification-hole closure, 174bee2). **Branch supersedes the early-continue**
(the audit could otherwise be bypassed by any shortcut row — exactly the
vacuous-certificate hole). Main's independent scoring semantics
(`prepare_scoring` / `n_score_inversions` / `score_sigma_mkt`) graft onto the
flow-through unchanged: for an accepted shortcut row the score resolves to the
same value main computed. Branch's bare `++out.n_dropped` drops inside the
audit block were upgraded to main's `reject(ObsRejectionReason::
Deamericanization)` so provenance stays truthful for audit-dropped rows.

### 2. dense_slice.cpp QP — main's KKT certificate subsumes branch's I-4 fix (rule 1, "genuinely superseded")

Both sides hardened `qp_active_set` against the same defect (infeasible x0 /
silently-certified cap exit). Main's version is strictly stronger: scaled
start-feasibility check (`kQpStartTol`) fails closed before iterating, and
`qp_result` refuses to mark ANY exit converged unless the full KKT certificate
(stationarity + primal + complementarity + dual) passes — a cap-exit is always
rejected by callers (`!solved.converged → Internal`), feasible or not. The
branch's `worst_residual` start/cap checks are therefore subsumed and dropped
(with a comment preserving the I-4 finding). Branch's fit-side I-4 mitigation
(x0 box-clamp into `[intrinsic+eps, forward−eps]`) and its price-epsilon bound
rows, left-origin convexity row, `required_k` exact-node union and noise-aware
lambda are all kept; main's `cmax`/`span` fallback-start declarations were
reinstated (the merge had dropped them). Both diagnostics sets are populated on
`ConvexSliceFit` (qp_* + effective_lambda/noise_scale).

### 3. curve_fit.cpp — branch certification capture rehosted on main's PreparedSlice (rule 3)

Main replaced the ObsSet prepass with canonical `PreparedSlice::create` and
replaced the second cold de-Am parity pass (`build_parity_data`) with prepared
score columns (same keyed population as the fit rows — cf615f4's fix). The
branch's perf-C1 certification capture (cert-carry re-resolve with
`deam_cert_caches`, chain snapshots, `source_quote_lookup`,
`SliceInputCertification` rows, `n_carry_skipped` surfacing) was integrated
INTO that structure rather than reverting it:

- `PreparedSlice` gained a retained `deam_audit()` (populated from
  `ObsSet::deam_audit` by the Configured builder) so the branch's inversion
  certification flows through the canonical seam.
- `build_parity_data`/`ParityData` are gone (superseded); scoring uses
  `score_columns()`.
- Phase 2's cert block reads `prepared.fit_observations()` /
  `prepared.deam_audit()` instead of `pre.obs`.
- Main's try/catch exception seam around `run_deam_prepass` (9458c29) kept.

### 4. surface_parity.cpp / prepared_fitting.* — audit protocol + §5.2 counters rehosted (rules 2+3)

Main canonicalized `run_surface_parity` onto `prepare_expiry` (which hides
`resolve_chain_forward`), while the branch added, in the old structure: the
§8.1 fit-inversion audit inside `build_aligned_obs`
(`deam.audit_fit_inversions`), `n_carry_skipped` / `n_audit_starved` counting,
and per-slice `carry` diagnostics on the report. Resolution: main's structure
wins; the branch semantics were rehosted:

- `PreparedSliceInputs` gained `audit_fit_inversions` +
  `max_iv_residual_half_spreads` (threaded from `deam` by `prepare_expiry`) and
  two optional OUT-tallies (`out_legacy_fit_rows`, `out_legacy_audit_dropped`)
  written before the usable-floor check so an audit-starved thin slice is
  distinguishable from a sparse one even when `create()` fails.
- `prepare_legacy` implements the branch's exact audit protocol (cold
  re-reprice, accurate fallback, drop + count on persistent failure).
- `prepare_expiry` gained an optional `PrepareExpiryDiagnostics*` out-param
  (carry_failed / carry_available+carry / n_fit_rows / n_audit_dropped),
  populated on success AND failure.
- `run_surface_parity` classifies prepare failures via that diag
  (carry_failed → `++n_carry_skipped`; rows+audit_dropped ≥ floor →
  `++n_audit_starved`) and pushes `prep_diag.carry` ‖ context — reproducing the
  branch's report exactly on main's structure. No gate weakened.

### 5. pricer_fitter.cpp — the union facade (rules 2, 3, 4; the largest call)

Both sides rewrote the 301-line base into ~900-line files with different
serving models (main: single transactional surface + FitAdmissionPolicy;
branch: dual mark/risk + independent oracle). The merged file (~1626 lines)
uses the branch's dual-surface pipeline as the spine (rule 4) with main's
transactional machinery woven in (rule 3):

- **fit(chain, session_overlay)**: branch's dual pipeline + main's
  `session_overlay` (applied once to each purpose's fully-resolved inputs,
  never on ladder retries), main's duplicate-maturity input-validation gate
  (recorded via `last_attempt_report_`), and full `SurfaceBuildReport` attempt
  history over the risk pipeline (primary + both fallback ladders), published
  transactionally with the admitted risk surface + `FitSnapshotProvenance`.
- **Risk admission is the AND of both gates (rule 2 core call)**: the branch's
  independent oracle (`validate_risk_surface` + session-context merge +
  `decide_risk_surface_admission`) AND main's family-neutral
  `FitAdmissionPolicy` (`completed_attempt_report` → independent-invariant
  evaluator → `evaluate_surface_admission`). A policy-gate rejection is folded
  into the digest as `ValidationFailure::InvalidDomain` BEFORE the oracle
  decision so served health can never disagree with publication. With the
  default (WP12 mark-contract) policy this adds only the consumer-independent
  numerical floor; `risk_admission_policy()` remains the strict opt-in. A risk
  surface can never publish without the branch's oracle — da718f7's
  "marks serve by default" intent is honored on the mark path only.
- **Mark-only requests** (legacy Hft mapping via branch's
  `effective_request()`): synchronous, admission-gated (main's cfg_.admission —
  exactly main's single-surface Hft/LinearVariance behavior), transactional
  publish with report + provenance. Dual-request marks keep the branch's
  contract (async build, publish on success, LKG on failure).
- **refit_expiry** (main) operates on the default-purpose surface; when that is
  the RISK surface, publication additionally requires the branch oracle
  (validate + merge_session_failure_context + decide) — main's mark-grade
  admission alone can never republish a risk surface. Publishes a new
  generation. 9b3c4c6's `refresh_refit_diagnostics` parity_state recompute
  survives verbatim in session.cpp.
- **refit_risk_slice**, `effective_request`, fail-closed `surface()` /
  `fitted()`, purpose-aware `value_chain` overloads: branch verbatim (main's
  purpose-less value_chain body was byte-identical to the branch's overload
  body).
- **fallback_curve_rungs**: branch's risk-safe rung values (no LinearVariance
  rungs for parametric families) + main's SplineVol entry — this union had
  already auto-merged.
- Merge seam `merge_session_failure_context` (incl. PriceBounds count
  saturation) and `risk_validation_config` per-mode grids: branch verbatim.

### 6. session.cpp

- Small hunk: main's failed-calendar-check honesty fix (sentinel 1, never
  "clean" on a failed check — 8bedc63) + branch's `arb_check_price_bounds`
  self-check (oracle I-2). Both kept.
- Giant hunk: pure both-sides-added-methods union — main's `clone_for_refit` /
  `apply_prepared_essvi_refit` / `refresh_refit_diagnostics` + branch's
  `cached_refit_observations` (carry-coordinate invalidation).

### 7. corpus.cpp / corpus_board_fit.* (rule 3)

Main extracted `FitSlot`/`fit_board` into corpus_board_fit.{hpp,cpp} (T5) so
`populate_surface_db` shares the pipeline. The branch's provenance capture
(e5e2a8a: `provenance_from_health` off the purpose-matched bundle health,
before the stack-local fitter dies) was moved into the extracted
corpus_board_fit.cpp; `FitSlot` gained the `provenance` member. corpus.cpp took
main's extraction comment side; its provenance consumer (archive item write)
had auto-merged.

### 8. vol_curve.cpp

SVI case: main's Martini-Mingone butterfly serving gate (project + re-check +
refuse) runs first, then the branch's shared-k calendar pair projection, with
the branch's independent `validate_parametric_risk_shape` as the final
fail-closed backstop. b80a1eb's C8 v_min de-saturation + slice_ok admissibility
gate and f8699d9's SplineVol dispatch verified present (auto-merged regions).

### 9. multiname_pipeline_test.cpp pins (rule 6)

Main's pins = T16a put-cache run under main's serving; branch's pins = the V2
default-risk run. The merged production default IS the V2 pipeline, so the
branch's pins were taken, with a merge-repin comment noting that main's T16a
`andersen_lake_put_slice` boundary reuse can shift put-priced aggregates
~1e-10 relative against them. These are the 3 MultinamePipeline bit-pins in the
documented artifact-cache-flaky bucket on both sides; failures here are triaged
as known-pin, not chased (per instructions), and re-captured on the merged
binary only if systematic.

### 10. pricer_fitter — the §9 routing call: a legacy preset never implicitly requests Risk (rule 2)

This is the one place where the two sides' *contracts* — not just their code —
were in direct conflict, and the first resolution got it wrong. It is recorded
in full because it changed a public default.

**The collision.** Both sides shipped a serving default, and they disagree:

- **Main (da718f7, WP12)**: the default serves a **MARK**. Strict risk
  admission is an explicit opt-in (`risk_admission_policy()` /
  `admission.consumer == SurfaceConsumer::Risk`).
- **Branch (§9 compat seam)**: legacy presets "inherit mandatory independent
  risk admission" — `map_legacy_fit_preset` maps Fast/Robust/Accurate to
  `SurfacePurpose::Risk`, and `surface()` fails closed on an unadmitted risk
  surface rather than substituting the mark.

The merged `is_v2_request()` tried to satisfy both by *value*: a config whose v2
fields were untouched (`quality_mode == Balanced && outputs == MarketMarkAndRisk`)
and which touched no main-only knob was auto-routed into the branch's dual
bundle. But that value IS the shape of every legacy `PricerConfig`, so
`preset = Fast` → `map_legacy_fit_preset` → purpose Risk → `effective_request()`
returns outputs containing Risk → `surface()` resolves to `risk_surface_`, which
is null whenever the independent oracle does not admit. Main's mark-grade
consumers — `surface_db_populate` (`pricer_config_for_symbol`, preset Fast),
`corpus_board_fit`, `dispersion` (bare `PricerConfig`) — were silently promoted
to fail-closed **risk** requests. `populate_surface_db` fitted **zero** surfaces
(n_ok=0, n_failed=2 per date, no partitions written): a production breakage of
the surfdb backfill.

**Why the obvious fixes are wrong.** Making main's call sites request
`SurfaceOutputs::MarketMark` (option (b)) routes them into the branch's
*mark-only* path, which is a hard-coded `FitPreset::Hft` +
`VolCurveKind::LinearVariance` build — it has no curve selector, no fallback
ladder and no preset budget. That is not main's single-surface fit; it would
silently replace populate's selector-routed ConvexDense/eSSVI surfaces with an
unconstrained dense mark. And the routing could not be fixed by value at all,
because `config_for(Balanced)` in the branch's own qualification suite assigns
exactly the default values — it is indistinguishable from `PricerConfig{}`.

**Resolution (option (a), made detectable).** The v2 request became an explicit
opt-in *at the type level*: `PricerConfig::quality_mode` and
`PricerConfig::outputs` are now `std::optional`. Engaging **either** is the v2
signal.

- **Neither named** → main's legacy world: ONE surface, `preset`-budgeted,
  selector-routed, admitted by `cfg_.admission`, served from the market_mark
  slot, incrementally refit via `refit_expiry`. This is byte-for-byte main's
  pre-merge path, and it restores every main consumer at once.
- **Either named** → the branch's dual pipeline. An unnamed field still resolves
  through `map_legacy_fit_preset`, so the legacy preset keeps supplying its §9
  *work budget* (Fast → Latency, Accurate → Accuracy) and Hft keeps its
  never-an-implicit-risk-request carve-out. The preset supplies the budget of a
  v2 request; it does not create one.

**No gate weakened.** Any request that resolves to a Risk output is still built
and admitted only through the branch's independent oracle
(`validate_risk_surface` + `merge_session_failure_context` +
`decide_risk_surface_admission`) AND main's `FitAdmissionPolicy`, still refuses
to substitute the market mark from `surface()` / `fitted()` / purpose-less
`value_chain()`, and `risk_admission_policy()` still forces the strict consumer.
Every SurfaceV2FailClosed / SurfaceV2Qualification / RiskSurfaceAdmission test is
green — they name their quality mode explicitly (`config_for(...)`), so they keep
routing to v2 unchanged.

Two branch tests encoded the overturned half of the contract and were updated
(not deleted) with the reasoning inline:
`SurfacePolicy.PricerConfigDefaultsToBalancedDualOutputWithMandatoryAdmission` →
`...DefaultsToLegacyMarkAndOptsIntoDualOutput`, and
`SurfaceV2LegacyCompat.LegacyRiskPresetsRouteThroughTheSharedMappingTable`, which
now names the dual output explicitly, leaves `quality_mode` unnamed — pinning
exactly the mapping-table path it exists to pin — and additionally asserts the
bare legacy shape serves a mark. `OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard`
(rewritten by the branch around the dual pipeline) names the v2 request it is
written against.

### 11. pricer_fitter — `session_overlay` clobbered the risk contract (merge miswire, rule 2)

Found while verifying that the populate boards' risk rejections were *legitimate*
before accepting the routing fix. They were not: the same boards fit a **Healthy**
risk surface at all three quality modes when fitted directly, and rejected with
`InsufficientData` only when populate's `session_overlay` was passed.

Main's overlay seam (`fit(chain, session_overlay)`) and the branch's risk
pipeline met for the first time in this merge, and the overlay ran **last** — so
`apply_symbol_config` → `apply_fit_preset` overwrote the resolved risk family and
every mandatory risk budget that `apply_risk_policy()` had just set:
`calendar_repair` Project→None, `score_parity`, `enforce_calendar_floor`, the
pinned accurate Andersen-Lake reference, `require_carry_confidence`,
`audit_fit_inversions`, and the per-mode carry/observation floors. Every risk
build on an overlaid board collapsed.

It is also a **fail-open** hole, which is the reason it is fixed here rather than
left latent behind the routing change: the config-level equivalents
(`score_parity = false`, `enforce_calendar_floor = false`) are hard-rejected
above as an *"invalid correctness policy for requested risk surface"*, so the
overlay must not be able to smuggle them into a served risk surface. Fix: the
overlay still lands on the exact inputs the fit uses (main's contract — band_k,
al_override, caches, a pinned curve all reach the fit), and the mandatory risk
contract is then re-asserted on top of it, restoring the resolved risk family.

## Build & test evidence

- Build: `cmake --build --preset rel --target atx-vol-tests` — clean under
  clang-cl `/W4 /WX`. `atx-vol-surface-v2-bench` and `atx-vol-fitting-bench` also
  build clean (the `std::optional` v2 fields are assigned, never read, outside
  the fitter; atx-ui's `--quality` path and the bench both remain explicit v2
  requests).
- Suite: `build-rel/bin/atx-vol-tests.exe --gtest_brief=1` →
  **1326 ran, 1310 passed, 7 skipped, 9 failed**, and the 9 are exactly the
  known-allowed pre-existing set (below). Before the routing/overlay fixes the
  same binary was 1302 passed / 17 failed; the delta is precisely the 8
  regressions triaged below, with no test changing state in the other direction.
- The full build graph still stops at a **pre-existing, unrelated** break in
  `atx-engine/tests/core/phase4_integration_test.cpp:335` under `/WX`. Untouched.

## Failure triage

**Known-allowed (9) — pre-existing on BOTH sides, not chased, not rebaselined:**
`AndersenLakeRegime.PositiveRateGrid_BitIdenticalToPrechange`,
`Pin.EvalAndEvalGradBitIdentical`, `Pin.AmericanGreeksBundleBitIdentical`,
`Pin.EvalPartialsMatchesEvalGrad`,
`SpyBidAskRegression.ConvexDenseServedViaSessionInBand`,
`SpyBidAskRegression.AutoSelectPicksDenseForSpy`,
`MultinamePipeline.HeldLotWithoutSurfaceIsCountedNotHidden`,
`MultinamePipeline.DefaultPolicyFullBasketBitIdentical`,
`MultinamePipeline.DefaultPolicyStillBitIdentical`.

**Merge regressions (8) — all fixed, none rebaselined away:**

1. **`SurfaceDbPopulate.*` (7 tests: FitsAndStoresPartitionsPerDate,
   HonorsDisabledSymbol, SkipExistingResumes, FailedFitRecordedNotFatal,
   DateWithZeroSuccessfulFitsWritesNoPartition, StatsCsvShape,
   SymbolConfigOverlayReachesFit)** — every populate fit failed (n_ok=0,
   n_failed=2, zero partitions). **Two independent merge defects, both real, both
   fixed in the source:** the §9 routing inversion (§10) promoted populate to a
   fail-closed risk request, and the `session_overlay` miswire (§11) then made
   that risk build fail outright. Diagnosed, not assumed: a probe fitting the
   same four boards showed `risk_state=healthy reasons=0x0` at Latency, Balanced
   and Accuracy with no overlay, and `rejected reasons=0x400` (InsufficientData)
   with it — so the rejection was NOT a legitimate thin-board carry/certification
   failure. No populate expectation was changed.

2. **`CorpusBuildSession.SyntheticThirteenNameThreeDateBreadthScoreboard`**
   (admitted 11 vs 9, quarantined 4 vs 6, dropped_signal 0 vs 1) — **not a code
   defect: a bad auto-merge of the test itself.** Both sides edited this test and
   git took both hunks. Main added the config line
   `fit_template.admission = risk_admission_policy()` (WP12: pin the strict risk
   contract explicitly, *because* the default now serves marks) — a line the
   branch never had. The branch independently rebaselined the counts to 9/6/1
   under ITS default template, i.e. under the v2 risk pipeline with no admission
   pin. The two edits were never valid together. With the merged (main's) config
   line the test measures main's single-surface path, whose pins are 11/4/0 —
   exactly what the merged code produces. Main's pins restored, with the
   provenance/fallback assertions the test also carries left untouched and green.

3. **`Dispersion.BookBitIdenticalAfterVegaOnlyResolve`** — repinned, cause
   established first. The routing fix (§10) returned this board to main's
   single-surface mark path (it fits through a bare `PricerConfig`), which
   collapsed the failure from "a completely different surface" to a uniform
   ~1e-8 relative shift in the low ~30 bits of every leg field. That residual is
   the branch's **shared fitting-core** corrections, which main's path consumes
   too: `build_observations_european` now flows OTM-shortcut proposals through
   the same cold-reference inversion audit instead of retaining the raw American
   mid (§1), and `qp_active_set`/`fit_convex_slice` gained the feasible-x0 clamp,
   noise-aware lambda and left-origin convexity row (§2). Evidence that this is a
   fit shift and not a serving or resolve change: `leg.T` is bit-identical, the
   C1.7 contract under test (vega-only `resolve_leg` == the two-full-bundle
   `resolve_leg`) is untouched, and the book's semantic invariant still holds
   exactly — re-priced bucketed vega is bit-exactly −10000.0 on the index leg and
   1 ULP from +10000.0 on the names. Anchors re-captured on the merged binary via
   the test file's own `Dispersion.PrintBookHexAnchors_C1_7`, with the cause named
   inline.
