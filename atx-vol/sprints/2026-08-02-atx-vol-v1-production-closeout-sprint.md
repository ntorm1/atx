# atx-vol v1 Production Closeout Sprint (Sprint B)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Fresh
> implementer per task, task review after each, aggregated + final whole-branch reviews are tasks
> of this plan.

**Goal:** Ship atx-vol v1.0.0 from post-merge main: close correctness debt first, land the
remaining perf items second, close two bounded feature-completeness gaps third, then gate and tag.

Rethought 2026-08-02 after `feat/vol-v1-release` merged to main (merge `3d2ed52`, grpc/protobuf
manifest removal `4a05ebd`). Supersedes the pre-merge first draft of this file (`fa65b92`).
Priority order is binding — both for scheduling and for cut decisions if the sprint must shrink:

**1. Correctness   2. Performance   3. Feature completeness**

North star: the VolaDynamics profile
(`archive/research/vol/VolaDynamics_WilmottProfile_Jan2020_text.txt`) — market-maker-quality
American equity option pricing, fitting, and analytics: curve families that fit real skew shapes
(negative ATM curvature, W-smiles) with butterfly arb impossible by construction; fast bias-free
fitting with liquidity transfer and temporal filtering; PCP/borrow implying; cash-dividend-correct
fast American pricing; spot-vol dynamics driving smart Greeks and scenarios; vol derivatives
consistent with vanillas; clean C++/Python APIs.

## North-star scorecard (survey of main @ `4a05ebd`, 2026-08-02)

| Pillar | Status | Evidence (verified by read, not grep) |
|---|---|---|
| Curve families incl. neg-ATM-curvature / W | **At pillar** | raw-SVI, eSSVI, C8 (`c8.hpp:12-19` — `kappa<0` admits negative ATM curvature eSSVI cannot), CStar/C16M (`cstar_calib.hpp:94` W-shape), ConvexDense QP (`dense_slice.hpp`), S3; per-board selector (`curve_selector.hpp`) |
| No-arbitrage | **At pillar, one wiring hole** | Fit-time: butterfly no-arb is a KKT constraint of the ConvexDense QP (`dense_slice.hpp:13-24`). Post-fit: `arb.hpp` butterfly/calendar checks + projections; independent admission oracle (`detail/risk_surface_validation.hpp`). Hole: Task 1.1 |
| Fitting robustness | **Near pillar** | vega²/spread² weights, bid-ask awareness, IRLS-Huber, warm-start + `prior_strength` shrinkage (`calib.hpp`), observation-level `vol_error_bar` (`fit_metrics.hpp:21,68`). Missing: fitted-parameter covariance/error bars → roadmap #2 |
| American pricing | **At pillar** | Andersen-Lake spectral collocation + BAW + CN PDE oracle; hand-coded adjoint AAD, 8 greeks from one taped solve (`detail/adjoint_greeks.hpp`); hybrid discrete-cash dividends (`dividend.hpp`, Klassen 2017); explicit `ExerciseRegime` negative-rate map (`american.hpp:697-730`), strict-negative-rate double-continuation refuses rather than misprices → roadmap #3 |
| Borrow / PCP implying | **At pillar** | `rates_curve.hpp` borrow curve + `HtbDetector` (:247); `imply_borrow_european_pcp` (`dividend.hpp:181`); fixed-point American-PCP borrow in deamer (`deamer.hpp:123`) |
| Spot-vol dynamics | **PARTIAL** | Sticky blend omega in `adjusted_greeks.hpp`; post-hoc sticky diagnostics (`analytics.hpp:299-321`); `vol_beta` only inside dispersion shock model (`strategy_pipeline.hpp:188-193`); `scenario_grid.hpp:124-141` is sticky-strike-only. No SSR-style forward parametrization → roadmap #1 |
| Greeks | **At pillar** | 8 greeks incl. vanna/volga/charm, scalar + AVX2 batch, portfolio aggregation |
| Vol derivatives | **At pillar** | var/vol/capped swaps + mid-life RV dispatch (`derivatives.hpp`), priced off the same fitted `PricedSurface`/`SurfaceSet` as vanillas; `deriv_book.hpp` companion to portfolio_pricer. VIX log-strip is analytics-only → roadmap #4 |
| Scenario / risk analytics | **At pillar (v1 scope)** | `scenario_grid`, `pnl_attribution`, `historical_projection` (ES quantiles), tearsheet + report tools |
| Python API | **PARTIAL** | pybind11 module covers pricing/surfaces/backtest/strategy DSL; correctness holes + no vectorized American → Tasks 1.3, 5.1 |
| Persistence + market data | **At pillar (v1 scope)** | ATXVSA2 archive (CRC-32C, concurrent-read), `surface_db` manifest + partitions, Databento OPRA parquet ingestion (panel + hive v2), backfill tooling |

Verdict: fundamentals are at the north star. What stands between HEAD and a tag is (a) the
correctness debt in Tasks 0-1, (b) perf items 6.3-6.7, (c) two bounded feature gaps (Task 5), and
(d) genuine post-v1 north-star work this sprint seeds but does not build (roadmap section).

## Inherited state (updated post-merge)

- Parent sprint `2026-07-26-atx-vol-production-v1-release-sprint.md`: Sprints 1-5 CLOSED
  (`a4567b0`, `bb6d6a4`, `a02bd5f`, `fcfa3eb`, `2175c39`), S6-T29 accepted (`8e6f27a`), sprint cut
  after T29, branch merged to main. Ledger + briefs live untracked under
  `.superpowers/sdd/2026-07-26-atx-vol-production-v1-release-sprint/` in the `C:\atx-wt\pool-3`
  worktree.
- **T29 6.2 is superseded**: main's merge brought a real `RunConfig::prefetch_depth` field
  (`backtest.hpp:835`, default 1, arity pin now 17) with a pipelined incremental loader and a
  bit-identity-across-depths test (`backtest_test.cpp:3040`, sweeps {1,2,3,4,8,12,30}). The T29
  `kPrefetchLookahead` constant is gone. Default-depth tuning on the merged loader is Task 4.8.
- T29 6.1 stands as an evidenced negative result: whole-mapping `Mapping::prefetch()` measured
  slower warm (17/24 pairs); zero callers by design.
- API frozen: version single-sourced from `project(VERSION)`; Tier-A umbrella = **57** entries
  (`vol_umbrella_test.cpp:58-116`, `deriv_book.hpp` joined); RunConfig arity pin = **17**;
  packaging smoke consumer `scripts/atx-vol-test-package.ps1` is a standing gate.
- Test population: 2834 registered under `-L atx_vol` on main. Branch-side post-merge matrix at
  `922420e` was ZERO failures / 12 gated skips; the first full main-side matrix was in flight when
  this plan was rethought — its result is Task 0 input. 148 `GTEST_SKIP` sites (see Task 1.6 for
  the corrected gate inventory).
- **Pre-merge NAV anchors are stale by construction** (rel `123243.11724603444`, rel-avx2
  `123243.11724602008` at `8e6f27a`): main's SPX-Wilmott/European-semantics and derivatives work
  may legitimately move NAV. Task 0 re-pins.
- **v1.0.0 tag NOT created.** Task 7 owns it.

## Task 0 — Post-merge re-baseline + merge-decision audit (correctness; mandatory first)

Everything later cites this baseline.

1. Full matrix, dev preset (`-L atx_vol` and the `-L`-less monorepo run): record registered /
   executed / failed / skipped and the named failure list. Baseline to beat: 0 failures. Any
   failure is a merge regression — fix before proceeding.
2. NAV legs on `rel` and `rel-avx2` (parent `task-s4-gate-report.md` legs 3-4 recipes verbatim;
   corpus `C:\atx-data\spy-dispersion\runs\parity-full`, 4 sha256 pins). If NAV moved vs the
   pre-merge anchors: explain WHY (expected from main's European-semantics/Wilmott/derivatives
   work vs merge error). Explained + intended → re-pin anchors + section digests, record the
   ruling. Unexplained → merge defect, stop.
3. Packaging smoke consumer re-run.
4. **Merge-decision audit** — three semantic decisions were made inline at merge time by the
   controller and have had no subagent review. Adversarial review each:
   - `benign_flat_corner` AL-sweep exemption (`american.cpp:1347-1348` and `:1423-1424`): the
     r==0 && q<0 corner is exempted from the all-frozen NotConverged refusal. Verify against the
     FD oracle and `NegRateDomainMap.ZeroRateNegativeYield_IsSingleBoundaryAmerican`; confirm the
     exemption cannot mask a genuine all-frozen non-convergence in any other regime.
   - SSE2 golden fingerprint re-pin `718570745730299145ULL` (`prepared_portfolio_test.cpp`):
     confirm provenance — merged tree reproduces BRANCH pricing bit-for-bit, so the branch pin is
     the correct one, and the avx2-side pins/digests are self-consistent.
   - RunConfig 16→17 resolution + `AlOpts` designated-init remap: arity pin comment truthful,
     field order matches main's intent.

## Task 1 — Correctness closeout

Each item ends in a fix or an explicit recorded ruling. Nothing here defers to the gate.

1.1 **`with_no_arb_check` (BLOCKER)** — `projection.cpp:423-424`: the parameter is accepted and
    discarded (`(void)with_no_arb_check; // PORT NOTE: dense no-arb sweep deferred.`). A frozen
    v1 API silently ignoring a no-arb request is a correctness lie. Wire it (the machinery
    exists: `arb.hpp` butterfly/calendar checks + `risk_surface_validation` oracle — this is
    plumbing, not research) OR reject non-default values with `NotImplemented` at validation.
    Ship whichever, but the parameter must not be a silent no-op at tag time.

1.2 **Tier-A `derivatives.hpp` API coherence** — `:126` includes `detail/legacy_surface.hpp`
    because the extern templates `deriv_greeks<EssviSurface>` / `deriv_greeks<SviSurface>`
    (`:635-640`) instantiate on surfaces the S4-T21 demotion moved OUT of the supported API. A
    frozen Tier-A header now hard-depends on `detail/` types. Decide and execute one of:
    re-target the extern templates at the supported calibration-grade surfaces; or add Tier-A
    wrappers and move the legacy instantiations behind `detail/`; or document the legacy pair as
    supported-for-derivatives (least preferred — un-demotes them). Umbrella test + arity/API
    review gates the choice.

1.3 **Python binding correctness trio** (from
    `docs/reviews/2026-07-21-pipeline-sota-review/review-python.md`):
    - exceptions lose the structured `ErrorCode` (string-only; add a `.code` attribute) — :12;
    - `implied_vol_batch` discards the whole batch on one bad lane → per-lane NaN + code — :16;
    - `AloPricer.price` releases the GIL around a mutating call (cross-thread data race) — :22;
      fix or enforce/document a single-thread contract at the binding layer.

1.4 **SurfaceDbAdmin zero-spot ruling** — `cb7fe2e` (SE-P1-1) made `PricedSurfaceView` reject
    S<=0 while `SurfaceDbAdmin.VerifyDbFlagsNonFiniteAtmProbe` asserts a zero-spot record still
    maps. Behavioral decision: reject-and-reflag vs map-with-quarantine. Rule it, fix test or
    code to match.

1.5 **Standing dispositions** (parent ledger; each gets a ruling here, not at the gate):
    deep-OTM put parity gap; RunDir identity backlog; T17-F3 verify wire-in zero CI coverage; v1
    framing-block condition; no committed avx2 reference BYTES (digests only) — confirm or fix;
    `earnings_repro*` move candidate; ~2027-01 dated cliff for VolTime earnings-repro snapshots
    (documented per S5-T28 — verify the doc landed).

1.6 **Skip-set disposition (corrected inventory)** — 148 `GTEST_SKIP` sites. The old "~19 MB
    RunArchive e2e fixture" premise was WRONG — no such gate exists in-repo. Actual classes:
    (a) host-capability AVX2 gates (~27: `american_batch_test.cpp` 16, `simd_isa_override_test.cpp`
    10+1 env) — fine as-is, document; (b) `ATX_T7_DEFINITIONS_TSV` env gate
    (`listed_definitions_cache_test.cpp`, 10); (c) cached SPY OPRA parquet fixture presence
    (`backtest_real_test.cpp:244,338`, `spy_real_test.cpp:65,150`,
    `spy_archive_roundtrip_test.cpp:70`). For (b)/(c): provision (fetch/generate script or CI
    job) or accept + document the permanent skip set. The v1 claim "matrix green" must state
    what green covers.

## Task 2 — Thin-LTO on rel presets (perf 6.3 / parent S6-T30)

Brief already written: parent workspace `task-s6t30-brief.md`. Enable
`CMAKE_INTERPROCEDURAL_OPTIMIZATION` (thin-LTO under clang-cl 18) on `rel`/`rel-avx2` ONLY (dev
untouched). Centerpiece: 2×2 bit-parity table {rel, rel-avx2} × {LTO off, on} against Task-0
anchors. If LTO moves ANY bit: STOP, controller decides policy (re-pin vs disable contraction vs
skip LTO) — never silently re-pin. Bench evidence paired/interleaved only. Report build/link-time
delta. Full matrix under the LTO rel build, diffed against the non-LTO rel matrix run FIRST. No
source changes — an LTO-exposed ODR/link issue is a finding (interacts with S5-T25 tagged inline
namespaces), not an inline fix. A partial preset edit for this task was reverted pre-merge; start
clean from the brief.

## Task 3 — AVX2 pack utilization + dynamic partition (perf 6.4 + 6.5 / parent S6-T31)

Line refs re-verified on main 2026-08-02:

- 6.4 pack fill: greek packs are flushed per-(uid, side, raw-T-bits) run — `laned_greek_run.hpp`
  function now spans `:132-252` (`flush_put` :187-200, `flush_call` :201-211, scatter :213-249);
  call site `priced_surface_view.cpp:1038-1064` (route gate :1053, call :1056). Accumulate packs
  across T-runs before flushing; kernel unchanged. Gates: existing pack-composition-invariance
  and thread-count bit-identity tests. Any golden/fingerprint movement is a STOP + ruling.
- 6.5 dynamic partition: the static contiguous `run_ranges` partition now lives at TWO sites —
  `portfolio_pricer.cpp:990-999` (scalar/Auto, boundary walk :950-999) and `:1886-1897` (scalar
  fallback) — and the merge ADDED an AVX2 tile `run_blocks` path (`:972-984`, `:1873-1878`).
  The parent's "~40% parallel-region loss" figure predates `run_blocks`: **measure the imbalance
  on merged code FIRST**; if the tail imbalance no longer reproduces, record a negative result
  and stop. `run_dynamic` is determinism-safe here (disjoint slot writes); bit-identity across
  thread counts is the gate.
- Paired benches on the solve chain for both items; NAV unchanged vs Task-0 anchors.

## Task 4 — Allocation/medium batch + prefetch-depth default (perf 6.6 + 6.7 / parent S6-T32)

Line refs re-verified on main 2026-08-02. Items that measure as non-wins are recorded as negative
results, not shipped (T29 precedent).

1. `StepMarkMemo` clear/reinsert per step → dense generation-stamped vectors. The class moved to
   its own header: `step_mark_memo.hpp:39-126` (`populate_from_marks` :49-71; `entries_.clear()`
   :50). Instances at `backtest.cpp:2248,2525`.
2. `current_identity` opens an `ifstream` per load/prefetch on sealed archives:
   `snapshot_cache.cpp:103-119` (open :104; call sites :285, :335). Cache per mapping.
3. `resolve_universe_uids` recomputation: now FOUR call sites — `dispersion_strategy.cpp:237`,
   `:370`, `:424`, `:434` (merge added two). Memoize per step; scope grew, so re-measure value.
4. `uid_of` linear scan → `lower_bound` over sorted `syms_`: `backtest.cpp:2065-2078`.
5. SVI LM scalar Black-76 loop → batch: `svi_calib.cpp:607-634` (scalar loop :618-632,
   `black76_price` :626; `svi_total_w_batch` already vectorized :616).
6. Serial `reduce_price_totals` / `reduce_pnl_totals` duplicating kernel work:
   `portfolio_pricer.cpp:1056-1058`, `:2032-2033`.
7. 6.7 batch: `BacktestResult` reserve sweep; O(n²) uid dedup; `cache_key` lexically_normal
   allocs; `kGreekChunk` 128→32 experiment.
8. **Prefetch-depth default** (closes T29's superseded remeasure): `RunConfig::prefetch_depth`
   (`backtest.hpp:835`, default 1) on the merged pipelined loader. Paired-measure depth 1 vs 2
   vs 4 on the 135-session corpus, cold and warm; ship the best default. Output is bit-identical
   at any depth (`Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead`,
   `backtest_test.cpp:3040`), so this is pure throughput policy.

## Task 5 — Feature completeness (bounded, north-star aligned)

5.1 **Python batch parity for American + B76** — `review-python.md:40`: "No vectorized path for
    anything American." The AVX2 SoA kernels exist and are tested in C++
    (`american_batch.hpp` AllGreeks, `american_iv.hpp`, `black76_greeks_batch`,
    `black76_value_and_vega_batch`); bind them as numpy-vectorized entry points. Binding-only —
    no kernel changes. Per-lane error surfacing consistent with Task 1.3. Cut rule: if gate
    schedule demands, 5.1 may drop to post-v1 by controller ruling.

5.2 **Feature-claims audit (truthful v1 surface)** — README/CHANGELOG/doc pass, no code: the
    strict-negative-rate double-continuation refusal is documented where users hit it
    (`american.hpp:697-730` regime map); `scenario_grid` sticky-strike-only limitation stated
    (`scenario_grid.hpp:124-141` already says it — verify README doesn't overclaim); no claim of
    fitted-parameter error bars anywhere; VIX log-strip described as analytics, not a product
    module; README perf figures re-measured (paired) or removed. 5.2 may NOT be cut —
    truthfulness precedes the tag.

## Task 6 — Sprint aggregated review + fix rounds

READ-ONLY aggregated reviewers over Tasks 0-5 (named checks; parent T29 checks 1-8 join the set).
Fix rounds capped at 5, one combined fix dispatch + scoped re-review per round; minors deferred
to the final wave unless elevated with rationale.

## Task 7 — Release gate + v1.0.0 tag (parent S6-T33)

Brief `task-s6t33-brief.md` in the parent workspace, amended by this plan. Legs:
- Full matrix: `rel`, `rel-avx2`, forcescalar leg, adversarial archive suite, python suite
  (including any Task-5.1 bindings).
- Bench suite vs pre-sprint-1 baseline; publish before/after for the 135-session backtest cold +
  warm (paired method).
- NAV determinism both ISAs vs Task-0 anchors.
- Assert the standing-dispositions list is EMPTY (Task 1 closed all of them; anything surfaced
  since gets ruled here or blocks).
- CHANGELOG + README final pass (consistent with 5.2).
- **Tag v1.0.0** (controller, after PASS).

## Task 8 — Final whole-branch review

Most capable model, ONE fix wave, then superpowers:finishing-a-development-branch. Triage input:
the deferred-minors roster in the parent ledger and STATUS file (S1-S5 minors, T23 F-1/F-2/F-4,
T28 F-3 tier-count contract test, B-M-4 stderr-promise block, M-5/M-6 validate()/signal-dedup
notes).

## Post-v1 north-star roadmap (seeded here, built later — priority order)

1. **SSR spot-vol dynamics**: one vol-sensitivity parameter describing surface motion under spot
   moves, unifying the three fragments that exist today — `adjusted_greeks.hpp` sticky-blend
   omega, `strategy_pipeline.hpp:188-193` dispersion `vol_beta`, and the post-hoc sticky
   diagnostics (`analytics.hpp:299-321`, which become calibration inputs). Deliverables: smart
   Greeks (skew delta / skew gamma) and a `scenario_grid` smile-roll mode lifting the
   sticky-strike-only limitation. This is the largest remaining VolaDynamics pillar.
2. **Fitted-parameter error bars**: covariance/confidence on calibrated params from the LM/QP
   solvers (observation-level `vol_error_bar` exists; parameter-level does not) — enables the
   Bayesian liquid→illiquid information transfer the north star describes.
3. **Strict-negative-rate double-continuation American** (today `ExerciseRegime` refuses).
4. **VIX-style product module** atop the existing log-strip analytics.
5. Roster carryover: python step_observer/cancel bindings; per-(uid,expiry) risk slice;
   ATX_VOL_CORPUS_DATE_BATCH; `Mapping::prefetch_range` (atx-tsdb); quote_rejects reader-or-gate;
   tools/research link isolation; per-date drain shape in populate cancellation; record_signals
   dedup; exe-name alias; ATX_VOL_PROFILE rename.

## Measurement policy (binding, all tasks)

Sequential before/after on the dev box is NOT a valid instrument: the same binary measured 674 vs
1096 ms medians hours apart, and a sequential table showed the OPPOSITE sign of two paired
experiments (T29). All bench evidence must be paired/interleaved A/B within one session, ≥10
pairs, reporting per-pair deltas, win-counts, and medians. Keep both binaries on disk
simultaneously so pairs alternate binaries, not rebuilds. Bench configure:
`scripts\atx-build.ps1 configure -Preset rel -Bench` (flag form, never `--preset`).

## Environment cautions (every dispatch)

- `Set-Location <worktree>; ` prefix; ctest alternation requires
  `powershell -File scripts\atx-build.ps1 -Ctest -R '...'`.
- Never PowerShell Get-Content/Set-Content or `>` on sources (mojibake incident); Read/Edit tools
  only. Byte-preserving native output via bash or `cmd /c "... > f 2>&1"`.
- The IDE Grep tool has returned fabricated hit-lists in this repo; load-bearing enumeration via
  `git grep` / Select-String + Read verification.
- RTK hook filters plain `git diff`/`git log`; use `rtk proxy git ...` for raw output.
- Shared dep cache `C:/atx-cache/deps` is cross-worktree-mutable: dep-not-found ⇒ regenerate (dev
  configure + build) before diagnosing code.
- No `git stash`, no `git checkout <sha> -- <paths>`, no `git rm -r`, no `git add -A`/`-f`;
  explicit-path staging; never clang-format; 100-col; stale-exe link failure = retry once;
  `git commit -F` for long messages.
- Sprint reports/briefs stay untracked under `.superpowers/`; agents never commit them.

## Out of scope (unchanged from parent, minus items the merge delivered)

SpiderRock Parquet decoder (`data.cpp:558`), C8/CStar/Wing integration into `calibrate_pool`,
SplineVol warm refit, the 6 deferred calibration research knobs. (The prefetch-lookahead config
field left this list — the merge delivered it; Task 4.8 tunes the default.)

## Estimate

Task 0 ≈ 0.5-1 day; Task 1 ≈ 1.5-2 days; Tasks 2-4 ≈ 3 days; Task 5 ≈ 1 day; reviews + gate
(Tasks 6-8) ≈ 2 days. Total ≈ 8-9 days.
