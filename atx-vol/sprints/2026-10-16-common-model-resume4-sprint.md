# Resume 4: direction reset toward per-expiry fitting (October 16, 2026)

Root tree `C:/atx-wt/pool-33`, branch `feat/vol-resume4-20261016`, base
`5e978fed` (resume3 handoff). Lease RunId `vol-20261016-root4`.

## Evidence that set the direction

- Diagnosis (read-only, dev200 development cohort): the common Markov model's
  error is a systematic low price level caused by ATM volatility stuck under
  resource caps (class B) and by the 0.25 sparse prior (class A). The per-expiry
  quote-slice fitter is far more accurate where it serves, but static admission
  limits it to 16.7% of eligible rows. 59% of eligible rows are refused by caps
  that engineering can lift. See the ledger line of this date and
  `C:/atx-cache/quote-benchmark/diagnosis-20261016/`.
- Research (cited): SpiderRock labels are per-expiry channel-constrained spline
  smiles priced per contract with per-expiry sdiv and forward offset. See
  `C:/atx-cache/quote-benchmark/research-20261016/`.

## Lanes dispatched this resume

| Lane | Branch | Purpose |
| --- | --- | --- |
| bench-e0-queue | none (archived seed-free binary) | E0 byte parity, then frozen R104/R416 queue |
| time-knots-lane | `feat/vol-markov-time-knots2-20261016` | finish the time-knot fitter WIP, migrate `interval_groups()` consumers, gates |
| policy-experiments | `feat/vol-policy-exp-20261016` | JSON-only X1 seed+prior, X2 five knots, X3 per-interval, X4 caps, X5 combo; status-based hybrid composition tool |
| implied-forward-lane | `feat/vol-implied-forward-20261016` | reusable per-expiry implied forward / sdiv from put-call pairs; opt-in carry mode for the quote-slice path |
| slice-coverage-lane | `feat/vol-slice-coverage-20261016` | graded admission, cross-expiry term prior, channel-penalty objective, optional knot spline for the per-expiry fitter |

All results are development-cohort evidence; no default promotion, no holdout
labels opened, no main merge. Integration, gates and receipts are appended below
as lanes report.

## Lane result: bench-e0-queue (complete)

E0 byte parity passed. R104 removes every E0 fit failure and reaches the
cap-feasible ceiling (45,070 eligible prices, 70.49%); its three-way MAE is
0.383 against E0's 0.500 with containment 97.1% and p99 2.19. R416 adds cost
and boundary mass without shared-row gain. The remaining common-model gap to the
quote midpoint (0.359 all-eligible) is still a low level (bias -0.245) on the
sparse profile and in snapshots whose ATM knot is below the market. R104 is the
development base for the policy experiments; nothing is promoted. Receipts and
hashes: `C:/atx-cache/quote-benchmark/common-resolvent-controlled-20261016-v1/RESULTS-20261016.md`.

## Lane result: time-knots-lane (complete, under review)

`f4416a74` on `feat/vol-markov-time-knots2-20261016`: variance-time-knot
shape, `interval_groups()` consumers migrated, legacy eight-case parity
232/232 values, anchored suites fit 28, policy 16, seed 15, market 20, pricing
60, native CLI 122, audit 18, campaign 22, PCH-off 15 objects. Measured
midpoint-freezing bias follows x/sinh(x): -4.9% variance inside a steep coarse
cell, +8.3% straddling a knot. Independent exact-SHA review dispatched.

## Lane result: implied-forward-lane (complete, under review)

`a5d2018a` on `feat/vol-implied-forward-20261016`: reusable per-expiry implied
forward / sdiv estimator (Tier-B headers, allocation-free, typed refusals) and
an opt-in `carry_mode` for the quote-slice path; default path byte-identical.
Benchmark verdict: the supplied carry in the raw schema already sits within
0.07-5.1 bp of the label-implied forward by tenor, while any quote-only forward
misses by 8-39 bp, so the mode is WORSE on dev200 (0.176 -> 0.259 on 7,852
shared rows) and must stay off for this benchmark. Hypothesis 7 is refuted at
the forward level; the residual >365d call/put asymmetry is strike-varying
(ASML 93%), i.e. exercise/dividend modelling. The module is kept for live use
where no vendor carry exists.

## Standing direction (user, this resume)

- SpiderRock marks are the yardstick, not the architecture. Any curve family
  or surface parameterisation that matches or beats the labels on accuracy,
  robustness and performance is preferred over replicating the vendor spline.
  Family choice is decided by dev200 evidence under one shared objective.
- Build infrastructure must not burn lane time: no monolithic test link in
  lanes, focused test targets only, vol presets limited to the vol closure.
- Source and document edits are made with the editor tools, not scripts.
- Time-knot fitter `f4416a74` integrated at `fc5ffb84` after APPROVE.

## Integration log (root branch `feat/vol-resume4-20261016`)

| Merge | Lane SHA | Outcome |
| --- | --- | --- |
| `fc5ffb84` | time-knots `f4416a74` | APPROVE; 2 minor deferred |
| `3495635f` | policy-experiments `9c1005ac` | compose tool + note; X1 modest gain; status hybrid 75.7% at 0.292 |
| `d7f4b6b5` | implied-forward `a5d2018a` | APPROVE; mode stays off (supplied carry equals the label carry); 4 minor deferred |
| `da3a4179` | build-infra `5cecf923` | vol-only presets, `vol-rel`, lease configure fix, test slices; root adopted `vol-rel` |
| `ac01702c` | cohort builder `6703f85d` | dev200b built; ranking holds; spread-normalised MAE adopted |
| `dc064a1e` | tick-path `94208f57` | BLOCK at `dcb6e3f1` (out-of-box ladder certificate) -> fix -> APPROVE; chain refresh p99 ~106-181 us unqualified |
| `be85fd21` | markov-fit `88c77c5d` | APPROVE; LM ends evaluation-limit stalls; seed-as-prior 0.3519 -> 0.3391 on the R104 rows (post-hoc); MINOR-1 one-sided seed bound must be fixed before holdout |

| `b9374ae7` | dividend-schedule `a402d615` | APPROVE; payers 0.224->0.211 (dev200) and 0.295->0.232 (dev200b); ASML blocked by lattice strike kinks |
| `31cdfeac` (ff) | slice-coverage `03583b45` + merge | BLOCK -> fix -> APPROVE -> merge APPROVE; graded admission 78.7% coverage at own MAE 0.235 vs mid 0.333; knot spline best family |
| `ef5bc6c3` | review-cleanup `3465221c` | APPROVE (0 blocker/major, 2 minor, 3 nit); closes the markov-fit, implied-forward and time-knots minors; ledger tail conflict kept both sides; root gates fit 41/41, seed 25/25, policy 17/17, implied-forward 26/26, ctest 5/5 + 2/2, pytest 149/149, PCH-off 17 TUs; parity reruns skipped (reviewer proved c89d0739, 6b129f64, definitions). MINOR-1 closed at integration: m1/m3/m5/m6 seed results before `811282cf` and composites on m6 must be rerun before held-out use. Deferred: MINOR-2 untested carry fallback causes InvalidMarket/NoModelYield/SliceRefused and Parity Contaminated exclusion; NIT-1 few-ulp retry toward a bound; NIT-2 budget-blocked retry reports jacobian_failure not evaluation_limit; NIT-3 fallback:invalid keys also renamed (now in the integration ledger line), quote-slice report schema not bumped from 2 |
| `3fa27b00` | serve-path `03a5463b` | APPROVE (0 blocker/major, 3 minor, 4 nit); opt-in records serve mode + exercise-edge gate, opt-in nearest-ATM rebuild order (catch-up near-money 17 -> 6 / 59 -> 18 ticks), lockstep record IV 404-601 ns (unqualified), opt-in parallel record build; no conflicts; root gates tick-path 51/51, PricingExecutor 23/23, session/correction 78/78, PricedSurface 52/52, fitting/backtest slice failure+skip sets equal the reviewer's (48 / 9 names), wave-1a fit 41/41, seed 25/25, policy 17/17, implied-forward 26/26, ctest 5/5, pytest 149/149, PCH-off 14 TUs / 17 objects, bench self-check 0 failures, name coverage 1/1; parity reruns skipped (PM ruling). MINOR-1 accepted: the `e695a411` tick-path default changes (8-ulp certificate floor, `max_cert_rel` 5e-7, exercise Greeks at the floor, refused certification row -> record uncertified) are correctness fixes; lane sprint note Defaults corrected. Deferred: MINOR-2 untested refused-row path; MINOR-3 no in-tree test pins the box-end solve reuse; NIT-1 `build_workers` comment; NIT-2 build continues past the first error; NIT-3 `SessionServePolicy` unvalidated; NIT-4 trailer |
| `53f728c7` | smooth-div `4e263541` | BLOCK at `2ec1c2de` (MAJOR-1: the BBSR coarse-lattice IV bound had no regression test) -> fix (test only, `quote_slice.cpp` unchanged) -> APPROVE; opt-in dated-cash lattice schemes behind the `dividend_lattice` keys (spliced-bbs, spliced-bbsr, escrowed-bbs, escrowed-lr; default spliced CRR unchanged), dividend-schedule minors M1-M3 and N2-N4; escrowed-lr vs spliced payers intersection MAE 0.1969 -> 0.1470 (dev200) and 0.2093 -> 0.1862 (dev200b), dev200b nets -250 rows to the calendar check, escrowed vol is not spot vol, no promotion; no conflicts (5 files auto-merged, both change sets carried unchanged); root gates smoothing 28/28, discrete-div 88/88, dividend-schedule 21/21, quote-slice 84/84, lane ctest anchor 94/94, tick-path 51/51, PricingExecutor 23/23, session/correction 78/78, PricedSurface 52/52, wave-1a fit 41/41, seed 25/25, policy 17/17, implied-forward 26/26, ctest 5/5, pytest 151/151, fitting/backtest slice failure+skip sets equal wave 1b (48 / 9 names), PCH-off 13 TUs / 14 objects; parity reruns skipped (PM ruling). Deferred: MINOR-3 `quote_surface.cpp:406` one-liner unreachable from in-repo callers and untested; MINOR-4 N2 route-check reorder untested; no machine-readable `vol_convention` field (route text only); channel observation pass floors at `kIvMin` regardless of the lattice bound (`quote_slice.cpp` ~901, follow-up); NIT-1 Peizer-Pratt tail cancellation; NIT-2 only the null `lattice_scheme` case pinned in pytest; NIT-3 trailer |
| `37addcd0` | slice-repair `5e30330c` | BLOCK at `04203854` (MAJOR-1: the dominance gate refused in-market RIVN slices, no minimum outside count, point fallback before the calendar pass; MAJOR-2: calendar-stage channel repair, VolBudget and NotCertified untested) -> fix (`7d883fa1`: gate runs last and never refuses, `point_min_outside_quotes`, 5 tests each failing on revert; `5e30330c` docs) -> APPROVE (2 minor, 6 nit); opt-in channel repair + dominance gate: repair-dominance-v3 dev200 coverage 78.74% -> 84.89%, intersection MAE -1.49% on 49,112 rows, 4,626 new rows at $0.138, 0 rows lost to the gate, RIVN 4/4, CRWV Nov 20 1/41 outside; 1 conflict (`quote_slice.hpp` `QuoteSliceConfig` tail: smooth-div `discrete_div_scheme` and slice-repair `repair`, both kept), 6 shared files auto-merged, both sides' changed lines carried unchanged; root gates quote-slice 97/97, implied-forward 27/27, smoothing 28/28, discrete-div 88/88, dividend-schedule 21/21, fit 41/41, seed 25/25, policy 17/17, ctest smooth-div anchor 94/94, slice-repair anchor 5/5, tick-path 51/51, PricingExecutor 23/23, session/correction 78/78, PricedSurface 52/52, wave-1a 5/5, pytest 152/152, fitting/backtest slice failure+skip sets equal wave 1c (48 / 9 names), PCH-off 12 TUs / 16 objects, bench self-check 0 failures, name coverage 1/1; wave-1 parity all MATCH (below), keys-ON v3 cohort5 `97fad2f0` / dev200 `14d9dc6d` and escrowed-lr `2cfda793` / `5a9c1f5f` reproduce. Deferred: gate phases 2 (undo) and 3 (re-admission) untested; stale calendar-repair record on kept point fits; 6 nits; AAPL Nov 20 held by the 65 d gated point fit (carried to the knot-ladder lane); v1 MINOR-1 confidence-ordered eviction and MINOR-2 gate-off point escapes (PM) |
| `1050dc54` | composite `bf3acaae` | tool-only, APPROVE (0 blocker/major, 3 minor); opt-in `--restrict INDEX=ROWS.csv` for the status composer (default byte-identical: 2026-10-16 C1 `77d9cdde` / C2 `c3ec3786` re-made), 11-case pytest wired into `atx-vol-quote-benchmark-tools`; composite on both cohorts with the `24e01644` binary: C5 (escrowed-lr payers -> repair-v3 -> m6) dev200 92.42% / 0.2197 / 0.144 spreads, dev200b 98.63% / 0.2245 / 0.110; C1 -> C5 -4.91% / -6.28% on exact intersections, C4 -> C5 -3.50% / -4.34%, m6 vs X1 fallback a wash; m6 rerun after the one-sided seed fix 0.3390 -> 0.3363; no conflicts (merge tree = lane tree); root gates quote-bench build -Jobs 2 exit 0 (no work, binary `4829965a`), ctest quote-benchmark-tools 1/1, pytest quote-bench slice 152/152, composer pytest 11/11; parity reruns skipped (PM ruling: no `atx-vol/src` or `atx-vol/include` file changed). Deferred: F1 IDE `.mypy_cache` in policy-experiments-20261016 (manifest deliberately not re-baselined), F2 restricted-middle-input and `--restrict` + `--receipts` test gap, F3 routed receipts' `fallback_rows_from_input_0` counts input 0 only (use `fallback_rows`) |
| `49977fda` | batch-pricing `583e3a75` | BLOCK at `fe6a516f` (MAJOR-1: Lockstep IV accepted on the residual alone, dev200 row 1295 2.8e-3 vol off; MAJOR-2: PerContract unverified in the flat exercise interval) -> fix (`5da11835`: one refusal rule on both IV routes, zero-vol band screen + evaluated-chord Ok, cold-map verification of PerContract; 7 new tests fail on the fe6a516f lib) -> scoped re-review APPROVE (0 blocker/major, 2 minor, 1 nit); chain batch price / Greeks / IV API `api/pricing/batch_chain.hpp` (Tier-B 37 -> 38): default Scalar and PerContract routes bit-identical to the per-contract loop at every thread count, opt-in LanedAvx2 (2.0-2.5x single-thread, ~6x at 3 contexts, max dprice 1.1e-12) and Lockstep IV (vs true vol 1.6e-8 chain / 3.7e-8 dev200; PerContract 2.1e-9 / 3.8e-9), >= 3x single-thread NOT met (timings UNQUALIFIED); dev200 route status disagreements 0; nothing existing calls the API, no AL kernel or PricingExecutor file touched; no conflicts (`atx-vol/tests/CMakeLists.txt` auto-merged; the root changed no `atx-vol/src` or `atx-vol/include` file since the lane base `24e01644`); root gates build chain-batch / pricing-slice / umbrella / quote-bench -Jobs 3 exit 0, chain-batch 27/27, PricingExecutor 24/24, umbrella TierCountsMatchTheReadmeTable 1/1, PCH-off 4 TUs; parity reruns skipped (PM ruling: reviewer proved `c89d0739` / `4b273b08` on the lane binary). MINOR-1 closed at integration: lane note §4 IV rows labelled pre-fix (post-fix PerContract t1 0.73x the loop, UNQUALIFIED). Deferred: MINOR-2 PerContract polish NoConvergence exit untested, refuses 17 / 1,800 well-posed sweep targets (K=140 T=5 put sigma .2) that Lockstep solves; NIT-1 upward Lockstep slope probe never counted resolved; untested OutOfBand screen branch; synthetic near-zero-vol route disagreements 288 / 2,520 (dev200 0); `andersen_lake` non-monotone in sigma near the exercise edge and `american_implied_vol` long-dated deep-ITM accuracy (kernel, al-speed); kernel interface requests 1-5; post-fix timing bench not rerun |
| `7aa883ce` | surface-archive `6d2f6910` | BLOCK at `431b724f` (MAJOR-1: u32 overflow in `expected_curve_words`; MINOR-1: series segment-name path traversal; MINOR-2: bench parity checked before encoding; MINOR-3: bench raw handle and buffer) -> fix (`3a1f663b` / `45955ea2`: u64 word counts and size_t sums, `decode_curve` runs the full `curve_ok`, manifest counts ORed, writer-only segment names checked at manifest open / segment open / compaction, bench round trip from the published bytes, RAII handle, timings out of the checksummed record) -> scoped re-review APPROVE (0 blocker/major, 1 new minor, 2 nit; in-place revert evidence); ATXVFA opt-in fitted-surface archive (`api/storage/fit_archive*.hpp`, CLI `atx-vol-surface-archive`, bench `atx-vol-fit-archive-bench`, spec `docs/storage/ARCHIVE_FORMAT.md`): dev200 graded-v1 archive 12,140,032 B, byte-reproducible (`50cac121...`), decoded round trip 50,348/50,348 rows and 1,926/1,926 ExpiryRecords bit-exact vs `1348f63d`, ExpiryRecord lookup + price median 500 ns p99 1.3 us (UNQUALIFIED); no existing path reads or writes it; no conflicts (`atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt` auto-merged with the wave-3a lines; merged archive files byte-equal the lane); root gates build fit-archive / surface-archive / fit-archive-bench / chain-batch / pricing-slice -Jobs 3 exit 0, fit-archive 53/53, ctest atx-vol-fit-archive 1/1, chain-batch 27/27, PricingExecutor 24/24, PCH-off 9 TUs 0 diagnostics; parity reruns skipped (PM ruling: opt-in archive, reviewer proved `c89d0739` on the lane binary). Deferred: bench `unbuffered_read` aborts when the archive size is not a multiple of 512 (`--ladders off` dev200 exits 1; ledger note); NIT manifest counts not cross-checked against the opened segment; NIT untested `unbuffered_read` throw paths; first-review NIT-2 `intern_reason` growth, NIT-3 temp left on a failed rename; two storage-slice failures seen in the lane tree not confirmed on `24e01644` |
| `8ac4be4a` | deferred-tests `083ce77a` (vol-sota W0, L0c) | light review APPROVE (0 blocker/major, 1 minor sprint-note wording); tests + sprint note only (7 paths, +668): N2 route-window label, few-ulp Markov retry (min_sigma mirror), carry fallback causes, Parity exclusion, refused certification row; no conflicts (merge tree = PM prebase `8c260d19`); root gates build 5 targets -Jobs 3 exit 0, anchored ctest 66/66 (implied-forward, american-markov-fit, quote-benchmark-native, `tick-path:*`, `fitting/SessionServe*`), no header touched. Carried: refused-row record exercise-edge geometry (`expiry_record.cpp:293-301`, to L3b) |
| `3ca49eb0` | al-speed `e8472fe2` (vol-sota W0, L0b) | review v2 APPROVE `409ee7aa` (0 blocker/major, 4 minor, 4 nit) -> fix round 1 (dev200 kernel-input latency rows, adjoint Reference pin test, bound text) -> re-review APPROVE `e8472fe2` (1 nit, NIT-5 closed in the al-speed note at integration); opt-in `AlProfile::Fast` AL kernel tiers 1-2, carry memo keyed on the profile; A/B tier 2 vs tier 1 on the Reference `accurate` p50 NOT RESOLVED (+3.84 us mean vs A/A 4.30 us, unqualified); no conflicts (merge tree = prebase `f5ecee88`); root gates build pricing/simd/fitting slices + markov-fit/quote-bench/implied-forward/tick-path -Jobs 3 exit 0, `pricing/*` + `simd/*` ctest 901 tests with the failed/skipped name set = review v2's under `ATX_VOL_FIT_WORKERS=3` (19 names `52324ef8`; minus the env-only HelpingDispatcher skip = the lane's 18 `cff50826`), L0c set + `^fitting/DeAmer\.` 95/95, PCH-off 26 TUs 0 diagnostics, AL fingerprint `53c4df68` MATCH (vol-rel-avx2 bench of the final root) |
| `05a7da3e` | knot-ladder `085e7380` (vol-sota W0, L0a) | fresh review BLOCK `2e60186c` (MAJOR-1 five ladder containment checks untested, MAJOR-2 calendar key 3 untested, MINOR-1 note attribution) -> fix round 2 (6 single-purpose tests, each failing on its removal, 113 -> 119) -> re-review APPROVE `085e7380`; opt-in knot-first ladder (`graded.ladder`) and `price_only` key; 1 conflict `atx-vol/tools/compose_predictions_by_status.py` (root `--restrict` routing vs multi-value `--served-status`) resolved with the PM prebase blob `31f4ca80` (union; one-status receipt wording unchanged), merged tree `d484c8bc` = prebase `7cebbbd3`; root gates quote-slice 119/119, implied-forward 31/31, markov-fit 42/42, ctest 124/124 + `atx-vol-python` Skipped (77: no `_core` extension in pool trees), composer pytest 11/11, quote-bench CLI pytest 62/62, scorer 97/97, PCH-off 11 TUs 0 diagnostics; final vol-hygiene build (`atx-vol-fit-archive-tests`, 112 steps) 0 warning/error lines; full parity and keys-ON below |
| `5a6b1c6c` | L2d-scheduler `d2a1482b` (vol-sota W2) | review APPROVE under PM scope ruling (`expiry_schedule.hpp` ruled in scope; F2 to L2c, F3-F6 deferred); opt-in per-expiry fan-out of point / knot / B1 / B2 over `QuoteSurfaceConfig::fit_workers` (quote-bench: `ATX_VOL_FIT_WORKERS`, 0/unset = all cores), longest-first, deterministic slots; `QuoteSurfaceLadder.*` registered under the quote-slice ctest; no conflicts (tree = prebase-W3 `e4d7d80d`); root gates quote-slice 123/123, curve-fit + ParallelFor 40/40 (+ the ledgered pre-existing `CurveFitSliceFallback.LinearFallbackRecoversForcedSplineFailures`), PCH-off 10 TUs 0 diagnostics |
| `e8f9fab4` | L1-fit-profile `4525b1ad` (vol-sota W1) | review APPROVE (2 minor); opt-in `--timing-detail` per-chain fit timing (`counters::fit_timing`, slot-local per expiry), real per-chain percentiles; 2 conflicts resolved with the Edit tool: `quote_surface.cpp` (L2d fan-out kept, L1 `ChainScope` first line of each per-expiry task, `timing_` sized in the constructor; blob `1be1baab` = prebase-W3) and `quote_surface_test.cpp` (include union, `add6056e`); `QuoteSurfaceTiming.*` added to the quote-slice ctest (PM note); root gates quote-slice 128/128, native CLI pytest 162/162, PCH-off 4 TUs. Semantic conflict L1 x L2d: L1's CLI test asserted chain walls sum within the surface wall (serial only; flaky 2/5 at cap 3 under the fan-out) -> PM ruling fix (a) `4fd60d0e` pins `ATX_VOL_FIT_WORKERS=1` for that test (assertion unchanged; native ctest 3/3 with the env unset) |
| `83d3e4ea` | L1c-al-nonmonotone `cc6cff84` (vol-sota W1) | review APPROVE (1 minor latent, 1 nit); opt-in `NonMonotoneRegion` refusal (`IvOpts` / `ChainIvOptions::refuse_non_monotone`, default off); no conflicts; root gates chain-batch 31/31, AlFastProfile 6/6, AmericanIvNonMonotone 5/5, PCH-off 19 TUs 0 diagnostics |
| `c5358ec8` | L2a-vol-space `a045a0e6` (vol-sota W2) | review APPROVE (2 minor, 1 nit); opt-in per-quote de-Am IV cache (`graded.fit_space vol` + `deam_cache`, accurate kernel byte-identical to the price channel; `deam_profile fast` changes the CSV); 2 conflicts resolved as the union: `quote_slice.cpp` (`DeAmIvView *deam` parameter + L1's Deam `BucketScope`; blob `4c5eeb06` = prebase-W3) and the quote-slice ctest suite list (12 suites, 135 cases; `7baf281f`); root gates quote-slice 135/135, native CLI pytest 163/163, chain-batch 31/31, PCH-off 14 TUs 0 diagnostics |
| `8be778b5` | L1b-shape-cohorts `54e9488d` (vol-sota W1) | review APPROVE (2 minor latent); tool-only nine-class shape manifest `ad323ccb` + opt-in `score --class-manifest`; no conflicts; root gate `atx-vol-quote-benchmark-tools` 163 passed |
| `b92d9006` | L2b-al-geometry `9fe45298` (vol-sota W2) | review APPROVE (0 blocker/major); `AlExpiryGeometry` / `AlBoundaryHandle` / `al_price_pack` bit-identical to the fresh kernels, opt-in batched / Fast-screened certification overload (library API, no quote-bench key); no conflicts; root gates identity 11/11, certification 6/6 (`AmericanCertification.*` not yet in the quote-slice ctest filter: exe 141 / 13 suites vs ctest 135 / 12, L2c), combined anchored ctest 211/211, pricing/simd name set at cap 3 = `52324ef8` modulo the documented `PricingExecutor.NestedDispatch_ReachesAPoolWorkerAndWakesThePool` load flake (3/3 alone), PCH-off 24 TUs 0 diagnostics; full parity, keys-ON and G2 below |
| `1a0b2b1a` | L2c-rung-memo `3d5fe7fa` (vol-sota W2b) | review APPROVE (0 blocker/major, 3 minor); opt-in per-expiry rung memo (`graded.rung_memo`): the de-Am IV cache owned per expiry slot and shared by every rung / repair / ladder attempt across fan-out threads, candidate certification-grid memo, fan-out exceptions -> typed surface error, kBand published after the fill, band fills charged to the deam bucket; 3 conflicts resolved with the Edit tool as the union: `quote_surface.cpp` (root blob = prebase blob, so the union is the lane's text; resolved `16446b35` = the lane's), `quote_surface_test.cpp` (`474dba43` = the lane's), quote-slice ctest suite list (root wrapping + `AmericanCertification.* QuoteSurfaceRungMemo.*`, 14 suites, `aa9fa1b0`); root gates quote-slice 150/150, native CLI pytest 164/164, curve-fit set 40/40 (+ 2 real-board skips), PCH-off 16 TUs 0 diagnostics |
| `f6fc7452` | L2e-deam-inversion `9923ce4c` (vol-sota W2b) | review APPROVE (0 blocker/major, 3 minor m1-m3; checkpoint + `review-L2e-finish`); opt-in Premium American IV inversion in the de-Am cache (`graded.deam_inversion premium`: BAW / Black-76 seed leg, cold accurate confirm per iterate, bit-for-bit Search fallback, bounded budget extension, `--deam-audit`; roots <= 2.4e-10 vol from the accurate root, ruling (a)); 3 conflicts resolved with the Edit tool as the union: `deam_iv_cache.cpp` `DeAmIvView::band` (L2c's local-copy publish x L2e's mid-first seeding: the range test and the edge inversions read the local, and the slot's mid fields are carried into the local after `mid_iv(i)` so publish() keeps them; blob `3e7c28d2`), `quote_bench_policy.hpp` graded key list (`54c47803`), CLI strict-option cases (`baaf07d0`); root gates AmericanIvPremium + AlFastProfile 12/12, QuoteSliceVolSpace + DeAmIvCache 9/9, chain-batch 31/31, quote-slice 151/151, native CLI pytest 165/165, PCH-off 67 TUs 0 diagnostics |
| `27eba264` | L2f-al-fast-tier `4ba287c9` (vol-sota W2b) | review APPROVE (0 blocker/major, 3 minor); opt-in `AlProfile::Screen` + `al_screen_opts()` seed-grade kernel tier (7 nodes, l 16, 2 JN + 3 FP sweeps, p 48; pinned |dP| <= 7.8e-4 max(P,1), 4.6e-6 for T <= 0.1, |dIV| <= 0.48 bp; never served), `AlBoundaryHandle` generation guard (L2b m2), bench status-array fix; no conflicts (kernel blobs `849c3cb3` / `d8b692cb` / `b0a2b84b` = the lane's); root gates 42 focused pricing gtests, chain-batch 31/31, combined anchored ctest 223/223, pricing/simd name set at cap 3 = `52324ef8` modulo the `PricingExecutor.NestedDispatch_ReachesAPoolWorkerAndWakesThePool` load flake (3/3 alone), AL fingerprint `53c4df68` MATCH (vol-rel-avx2 of `27eba264`), PCH-off 19 TUs 0 diagnostics; full parity, keys-ON and G2 below |
| `e63fa7ba` | SPY1s-owner Phase 1 + M1 `c20e40bc` (vol-sota W2c-a; contains L2h `6e94ba40` via `7980a6d3`) | review-SPY1s-p1 BLOCK on MAJOR M1 only (PROMOTION CASE SUPPORTED) -> M1 fix `c20e40bc` (`deam_iv_key` carries `deam_confirm` / `confirm_k` / `confirm_kernel`) -> rereview APPROVE (0 blocker/major); Screen-seeded Premium inversion with bound-decided band edges, confirm policy (always / band / never) and confirm kernel (reference / fast), Fast certification grid, seed routing, the reason-split counters; L2h σ-family `AlSigmaFamily` (rereview APPROVE; opt-in, no fit caller yet); clean (merged tree = `c20e40bc`'s tree `97f29471`); gates: pricing 73/73 + 1 skip (AlSigmaFamily 11, AmericanIvPremium 9), quote-slice owner suites 26/26, chain-batch 31/31 |
| (no-op) | L2h-sigma-family `6e94ba40` (vol-sota W2c-a) | `git merge --no-ff 6e94ba40` -> "Already up to date" (an ancestor through the owner's branch) |
| `f4f48299` | L3e-xcal `78e9f653` (vol-sota W2c-a) | review APPROVE (0 blocker/major, 5 minor deferred); opt-in cross-source calendar / convexity certificate in `compose_predictions_by_status.py` (`off` byte-identical); clean; composer pytest 20/20 |
| `5fb4d3ec` | L3p-shootout `c22312c1` (vol-sota W2c-a) | measurement lane, sprint note only; clean |
| `212f5ad7` | L5u-universe `27538985` (vol-sota W2c-a) | tooling lane: label-free universe builder + test (the dataset is never scored); the test registered in `atx-vol-quote-benchmark-tools` (`tests/CMakeLists.txt` blob `553e0546`); tools pytest 176/176 (L5u 4) |
| `bbaf9aa1` | R3 default promotion (integrate-W2c-a) | PM ruling R3: SPY1s arm D = the graded default (`quote_bench_policy.hpp` blob `6071627b`), kill switch byte-identical, gate PASS on dev200 / dev200b / cohort5, hash set re-baselined (R3 table below); CLI pytest 168/168 (69 in `test_quote_bench_cli.py`; 3 fail on the pre-promotion binary) |

**Wave W0 of the vol-sota sprint (G0, 2026-10-18).** Final root `05a7da3e`, quote-bench sha256
`9ce6f6c6afc3aa51a5feded37c6473a0cd39e15574f2e0e3a47c5fc7db3d1a12`, `ATX_VOL_FIT_WORKERS=3`,
outputs `C:/atx-cache/quote-benchmark/integrate-W0-20261018-parity/`. The knot-ladder lane changed
one keys-off cell: an `invalid_volatility` row's `vol_status` now reads `unidentifiable` instead of
`not_evaluated`. PM ruling (2026-10-18): this one-column normalisation applies to every default-path
output through the quote-slice serve path, proven per output by the strict normaliser
(`knot-ladder-fix1-20261017-scripts/normalise_vol_status.py`: only `vol_status`, only
`invalid_volatility` `not_evaluated -> unidentifiable`, blank price and vol, 0 other cells, normalised
sha256 equal to the expected hash). **From now on the RAW hashes below are the expected hashes; the
strict normaliser is the bridge to the pre-ladder numbers.**

| Output | Raw sha256 (expected from now on) | Normalised | Rows normalised | Verdict |
| --- | --- | --- | --- | --- |
| dev200 default `--policy auto` | `c89d0739` | - | 0 | MATCH (exact) |
| dev200b default | `4b273b08` | - | 0 | MATCH (exact) |
| divsched dev200 / dev200b | `abc75955` / `747d3e6c` | - | 0 | MATCH (exact) |
| divsmooth dev200 / dev200b | `2cfda793` / `5a9c1f5f` | - | 0 | MATCH (exact) |
| R104 common dev200 | `6b129f64` | - | 0 | MATCH (exact) |
| graded-v1 dev200 | `fc277413` | `1348f63d` | 564 | MATCH (modulo ruled vol_status normalisation) |
| graded-v1 dev200b | `46ad6c26` | `4c70d2bc` | 429 | MATCH (modulo ruled vol_status normalisation) |
| graded-v1 cohort5 | `171fa2d2` | `7fa3c415` | 2 | MATCH (modulo ruled vol_status normalisation) |
| repair-dominance-v3 cohort5 | `a714e814` | `97fad2f0` | 30 | MATCH (modulo ruled vol_status normalisation) |
| repair-dominance-v3 dev200 | `b11971e7` | `14d9dc6d` | 718 | MATCH (modulo ruled vol_status normalisation) |
| repair-dominance-v3 dev200b | `b0451d61` | `81cc69e0` | 538 | MATCH (modulo ruled vol_status normalisation) |

Keys-ON (knot-ladder policies `knot-ladder-v2-point.json` / `-strict.json`, same binary):
all six byte-identical to knot-ladder bench-v2 / fix 1 (point / strict: cohort5 `232d2c0d` /
`21f4ee58`, dev200 `6ebd63c5` / `858d6512`, dev200b `d53d2a0d` / `c5ab9121`), MATCH. AL Fast-profile kernel fingerprint `53c4df68` MATCH (vol-rel-avx2 ladder bench of
`05a7da3e`). C6 = ladder-point -> m6 from existing CSVs (`composite-20261018-c6/RESULTS.md`, no new
fit): dev200 coverage 93.68% / MAE $0.2258 / 0.1436 spreads / containment 98.34% vs C5 92.42% /
0.2197 / 0.1443 / 98.13%; dev200b 98.44% / 0.2352 / 0.1110 / 98.95% vs C5 98.63% / 0.2245 / 0.1098 /
98.97%; exact-intersection C5 -> C6 +1.59% / +4.71% (the plan's 0.5% bar is not met; C4 -> C6
-2.12% / +0.17%, i.e. most of the gap is C5's escrowed-lr payer source; C5 -> C6d with that source
first -1.97% / +1.49%); 0 violations inside any single source, cross-source only (C6 18 + 1,755 /
37 + 2,391 convex + calendar vs C5 24 + 2,511 / 36 + 1,917). No default moved, no holdout touched,
nothing pushed.

**Wave W2 of the vol-sota sprint (2026-10-19, checkpoint at user stop).** Merged in this order: L2d
`5a6b1c6c`, L1 `e8f9fab4` (+ PM-ruled test pin `4fd60d0e`), L1c `83d3e4ea`, L2a `c5358ec8`, L1b
`8be778b5`, L2b `b92d9006`. Final root binary (`b92d9006`) quote-bench sha256
`1ed7ae44dfc39e6afdc9ecfb47bf442e3cb5de3282271760f0afb1d4df2a412d`, `ATX_VOL_FIT_WORKERS=3`,
outputs `C:/atx-cache/quote-benchmark/integrate-W2-20261019-parity/`. Full default parity 13/13
MATCH, every RAW hash exact (no normalisation needed): dev200 `c89d0739`, dev200b `4b273b08`,
divsched `abc75955` / `747d3e6c`, divsmooth `2cfda793` / `5a9c1f5f`, graded-v1 `fc277413` /
`46ad6c26` / `171fa2d2`, repair-dominance-v3 `a714e814` / `b11971e7` / `b0451d61`, R104 `6b129f64`.
Keys-ON knot-ladder set 6/6 byte-identical to W0 (`232d2c0d` / `21f4ee58` / `6ebd63c5` / `858d6512`
/ `d53d2a0d` / `c5ab9121`). AL fingerprint `53c4df68` MATCH (vol-rel-avx2 of `b92d9006`, covers L1c +
L2b). W2 keys-ON identities: cohort5 graded-v1 `171fa2d2` at `ATX_VOL_FIT_WORKERS` 1 and 0; ladder
point / strict cohort5 at 0 = `232d2c0d` / `21f4ee58`; SPY graded-v1 with the accurate de-Am IV cache
(`graded-v1-volspace.json`) `087e6a54` at 3 and at 0 workers; L1 `--timing-detail` OFF: native report
key sets equal W0 on 9 parity outputs. Name set `^(pricing|simd)/` at cap 3 = `52324ef8` (the
`PricingExecutor.NestedDispatch_ReachesAPoolWorkerAndWakesThePool` load flake excepted; 3/3 alone).
**G2 NOT measured** (user stop before the keys-OFF vs keys-ON cohort5 `--timing-detail` run and the SPY
wall at 1 / 0 workers): plan §1.1 "Today" column not updated. Unqualified side readings from the
identity runs only: SPY graded-v1 + accurate cache 25.7 s wall at 3 workers, 12.4 s at all cores (16
threads, load 23-36%). No default moved, no holdout touched, nothing pushed.

**Wave W2b of the vol-sota sprint (2026-10-20).** Merged in this order on `f4a5028e`: L2c `3d5fe7fa` at
`1a0b2b1a` (3 conflicts: `quote_surface.cpp` and `quote_surface_test.cpp` resolved to the lane's blobs `16446b35` /
`474dba43` because root = prebase there; ctest suite union `aa9fa1b0`), L2e `9923ce4c` at `f6fc7452` (3 conflicts:
`deam_iv_cache.cpp` `DeAmIvView::band` union `3e7c28d2` — L2c's local-copy publish x L2e's mid-first seeding, the
slot's mid fields carried into the local after `mid_iv(i)`; `quote_bench_policy.hpp` key list `54c47803`; CLI strict
cases `baaf07d0`), L2f `4ba287c9` at `27eba264` (clean). Final root binary (`27eba264`) quote-bench sha256
`e86e0b98d979dd03fa64795f8a47b0f58c80670fafdfecbc46937f90e88206b8`, `ATX_VOL_FIT_WORKERS=3`, outputs
`C:/atx-cache/quote-benchmark/integrate-W2b-20261020-parity/` and `.../integrate-W2b-20261020-g2/`. Full default
parity 13/13 MATCH, every RAW hash exact (the W2 set: dev200 `c89d0739`, dev200b `4b273b08`, divsched `abc75955` /
`747d3e6c`, divsmooth `2cfda793` / `5a9c1f5f`, graded-v1 `fc277413` / `46ad6c26` / `171fa2d2`, repair-dominance-v3
`a714e814` / `b11971e7` / `b0451d61`, R104 `6b129f64`). W0 keys-ON ladder set 6/6 = W0 = W2. AL fingerprint `53c4df68`
MATCH (vol-rel-avx2 of `27eba264`; covers L1c + L2b + L2f). W2 identities re-proven 6/6 (L2d at 1 / 0, ladder at 0,
L2a SPY `087e6a54` at 3 and 0). W2b identities: L2c rung memo ON = OFF by hash 4/4 (`171fa2d2` / `232d2c0d` /
`21f4ee58` / SPY `087e6a54`); L2e premium own hash set bit-identical to the lane's and both reviews' (SPY `2c8b113c`,
cohort5 `bb5df194`, dev200 `196387bb`), served / refused set identical to OFF (diff EMPTY, 0 status mismatches), max
|dprice| 2.0e-5 / 4.1e-5 / 2.8e-4 USD = Search's distance from the accurate root (Premium roots <= 2.44e-10 vol;
ruling (a)), COUNT 191,511 -> 75,153 (0.392) and the fallback reasons identical to the lane's (the `band()` union
preserved L2e exactly). Name set `^(pricing|simd)/` at cap 3 = `52324ef8` (929 tests; the PricingExecutor load flake
excepted, 3/3 alone). **G2 measured, ALL keys that exist at W2b** (`fit_space vol` + `deam_cache` accurate +
`rung_memo` + `deam_inversion premium` + `fit_workers`; unqualified, one run each, 16 CPUs, load 4-23 %): SPY graded-v1
fit+reprice **15.0 s single-thread** (wall 15.2; W0 87.8-91, W2 52.7) and **3.35 s all cores** (wall 3.6; W2 12.4),
chain p50 401 ms @1; the two levers: **75,153 de-Am AL boundary solves per SPY surface** (W0 191,511; L2e cuts them,
L2c keeps the count at 75,153 under the fan-out) at **~149 µs per solve** (the unchanged Reference kernel; Screen and
the family are not wired yet) plus 28,908 certification-grid prices (34,694); cohort5 graded-v1 82.5 -> 33.0 s @1 /
5.4 s @0 (p50 187 -> 151 ms, p99 3,935 -> 1,483), ladder point 137.4 -> 36.8 s @1 / 8.8 s @0, dev200 graded-v1 23.3 s
@0 (p50 36.6 ms, p99 795). Served sets identical OFF vs ALL (lost 0 / gained 0 on cohort5 graded, ladder, SPY),
containment 91.59 % / 98.51 % and MAE/spread 0.2102 / 0.0848 unchanged to 1e-7; the all-keys ladder cohort5 hash
`270f1218` is first recorded. Plan §1.1 "Today" column updated: every speed row NOT MET (SPY 15x / 22x short), byte-identity
at any `fit_workers` MET; **SPY single-thread = 15.0 s, all cores = 3.35 s: unusable against the ≤ 1 s bar.** The HFT
sub-table (research §7.2) added under §1.1; the research report copied to `atx-vol/research/`. No default moved, no
holdout touched, nothing pushed.

**R3 default promotion (integrate-W2c-a, 2026-10-21, PM ruling R3).** Every graded policy now defaults to the SPY1s
Phase 1 arm D key set (`tools/quote_bench_policy.hpp` `graded_default_fit_space()` + `kGradedDefaultRungMemo`):
`fit_space vol`, `deam_profile screen`, `deam_cache true`, `deam_inversion premium`, `confirm always` (`confirm_k` 10),
`confirm_kernel fast`, `grid_kernel fast`, `rung_memo true`. The library struct defaults and every static policy are
unchanged; the band gate (`confirm band`), ladder-strict and the box certificate stay opt-in. Kill switch:
`{"fit_space": "price", "grid_kernel": "reference", "rung_memo": false}` reproduces the pre-R3 graded path byte for
byte. Evidence gate (new default vs the W2b default CSVs; `C:/atx-cache/quote-benchmark/integrate-W2c-a-20261021-parity/score/`):
PASS on dev200 / dev200b / cohort5 — served sets and status counts identical, 0 served violations, containment −1 / −1 /
0 rows (one-sided, bid 0, float32 ask edge, |Δprice| 2.4e-8 / 8.4e-8 USD), MAE and MAE/spread within 1.2e-6 relative,
every served root ≤ 2.3e-10 vol from the accurate root where the old Search answers sat up to 1.75e-3. Expected hashes
from now on (exact commands: `quote_benchmark.py run --dataset <cohort> --binary <bin> --output <csv> --model quote-slice
--policy <policy> --dividends funding-pv -- --report <json>` at `ATX_VOL_FIT_WORKERS=3`, the integrate-W2b
`run_parity.ps1` rows; SPY: `atx-vol-quote-bench --input quotes-SPY.csv --model quote-slice --policy graded-v1.json
--dividends funding-pv --timing-detail`):

| Output | Expected sha256 after R3 | Before R3 (= the kill switch now) |
| --- | --- | --- |
| graded-v1 dev200 | `2bc7777df4448c7f1818d10571859074646d51e12cab4731471947cdd75738f6` | `fc277413` |
| graded-v1 dev200b | `ed34eef7b51d4735748e5534a651569ff69026e07c1655cbb7b2fa6dad1264f2` | `46ad6c26` |
| graded-v1 cohort5 | `45e491ea311b352ec107658809429ca2583b5bfbdf2215f9c224b86f7fbc1581` | `171fa2d2` |
| graded-v1 SPY (direct, `fit_workers` 1 and 0) | `feac8d2c41a63ff8687e5e991d4026eeecccdf6e88418edcc874c8fc2ca8425b` | `087e6a54` |
| repair-dominance-v3 cohort5 / dev200 / dev200b | `c6ea5360` / `81572d79` / `09c8e212` | `a714e814` / `b11971e7` / `b0451d61` |
| knot-ladder v2 point cohort5 / dev200 / dev200b | `1fab44fd` / `0657a828` / `1a274084` | `232d2c0d` / `6ebd63c5` / `d53d2a0d` |
| knot-ladder v2 strict cohort5 / dev200 / dev200b | `333826fb` / `025e69c5` / `93d6543b` | `21f4ee58` / `858d6512` / `c5ab9121` |
| unchanged (static): dev200 / dev200b auto, divsched, divsmooth, R104 | `c89d0739` / `4b273b08`, `abc75955` / `747d3e6c`, `2cfda793` / `5a9c1f5f`, `6b129f64` | same |

The graded-v1 rows equal the owner's arm D hashes (`SPY1s-20261020-phase1/runs.log`). The opt-in graded families move
with the defaults: repair-dominance-v3 and the ladders keep their served sets except ONE dev200 chain (rows 4769480-4769592,
4 strikes, a channel refit whose outside share goes 0 -> 0.2): ladder-strict refuses it (`outside_market`, −8 of 54,762
rows), ladder-point serves it calendar-repaired (2 rows each on it and on its calendar neighbour move, max 0.105 USD);
outside that chain containment loses 1-2 rows per dev cohort, each on a float32 ask edge with |Δprice| ≤ 8.4e-8 USD
(one of them two-sided: dev200b row 8340964 under both ladders, 2.6e-8); MAE / MAE-spread never higher by more than
1.1e-5 relative; 0 served violations
in every family; full tables in `score/rebaseline/`.

**Wave W2c-a of the vol-sota sprint (2026-10-21).** Merged on `be379ad4`, all clean: SPY1s-owner Phase 1 + M1
`c20e40bc` at `e63fa7ba` (contains L2h `6e94ba40`; the L2h merge is then "Already up to date"), L3e `78e9f653` at
`f4f48299`, L3p `c22312c1` at `5fb4d3ec`, L5u `27538985` at `212f5ad7` (+ its test registration); R3 promotion
`bbaf9aa1`. Merge-root binary (`212f5ad7`) quote-bench sha256 `f550f2b9...`, final (`bbaf9aa1`) `d6824606...`,
`ATX_VOL_FIT_WORKERS=3`, outputs `C:/atx-cache/quote-benchmark/integrate-W2c-a-20261021-parity/` and `.../-g2/`. On the
merge root: full default parity 13/13 RAW MATCH (the W2b set); keys-ON 29/29 (W0 ladder 6, W2 6, W2b 9, the owner's arm
hash set 8: SPY A `19d47b61` / B `5985dfe1` / B3 `3b72a083` / C `8f80be87` / D `feac8d2c`, arm D cohort5 / dev200 / dev200b
`45e491ea` / `2bc7777d` / `ed34eef7`, every arm serving the keys-OFF 1,410 SPY rows); L1 report key sets 9/9; AL fingerprint
`53c4df68` MATCH; name set at cap 3 `52324ef8` EQUAL; CLI 66/66 on HEAD's tests. On the final root: the R3 table above
(new default = arm D bit for bit, kill switch 5/5, static rows 7/7); combined anchored ctest 244/244 (quote-slice 154 /
14 suites, tools 176, native CLI 168, campaign 22). **G2 with ALL keys = the R3 default (unqualified, host 63-93 % loaded):
SPY graded-v1 fit+reprice 10.88 s single-thread (wall 11.06; W2b 15.0) and 2.24 s all cores (wall 2.44; W2b 3.35);
the two levers: 106,232 de-Am AL solves per SPY surface (W2b 75,153: arm D adds one Fast confirm per accepted root;
22,590 Screen edges + 50,308 Screen iterates + 28,551 Fast confirms + 2,734 ambiguous + 2,655 search) at ~81 us per solve
blended (W2b ~149) + 28,908 Fast grid prices at ~73 us; cohort5 14.9 s @1 / 4.5 s @0 (p50 63.6 ms, p99 738 ms @1),
dev200 graded 12.4 s @0 (p50 16.3 ms, p99 497 ms); the SPY dated-cash row (static, inferred-schedule) 8.07 s vs auto 6.06 s,
0 rows served either way.** Plan §1.1 updated: every speed row NOT MET, byte-identity at any `fit_workers` MET; **SPY
single-thread = 10.9 s, all cores = 2.24 s: unusable against the ≤ 1 s bar.** No holdout touched, nothing pushed; the only
default moved is R3's.

Pending: knot-first ladder (inherits the AAPL Nov 20 / SPY 23 d calendar slot holders from slice-repair).

## Resume 5 wave 1 complete

Wave 1 merged four lanes into `feat/vol-resume4-20261016` (base `7ca31c95`): review-cleanup `3465221c` at `ef5bc6c3` (1a), serve-path `03a5463b` at `3fa27b00` (1b), smooth-div `4e263541` at `53f728c7` (1c) and slice-repair `5e30330c` at `37addcd0` (1d). Parity ran once, on the final root binary `37addcd0` (quote-bench sha256 `4829965a...`, `ATX_VOL_FIT_WORKERS=3`, outputs in `C:/atx-cache/quote-benchmark/integrate-1d-20261017-parity/`), with every opt-in key off: dev200 default quote-slice `c89d0739`, graded-v1 `1348f63d`, R104 common `6b129f64`, dividend default `abc75955` and dev200b default `4b273b08` all MATCH, as do dev200b divsched `747d3e6c`, dev200b graded-v1 `4c70d2bc` and cohort5 graded-v1 `7fa3c415`. The lanes' opt-in outputs also reproduce on the merged binary: escrowed-lr `2cfda793` (dev200) / `5a9c1f5f` (dev200b) and repair-dominance-v3 `97fad2f0` (cohort5) / `14d9dc6d` (dev200). No default moved, no holdout touched, nothing pushed.

## W2c-c / r5 R0 integration (vol-sota master plan Sprint 0, 2026-10-22)

Wave W2c-b stopped at its merge root `7344eb37` with its gates unrecorded and W2c-c was never merged (deep review F01).
Lane R0 (run `vol-sota-r5-20261022`, pool-3) finished the W2c-b gates at that exact SHA, then merged the three remaining
approved opt-in lanes onto a new root branch `feat/vol-resume5-20261022` cut from `7344eb37`. No default promoted;
nothing pushed. The first four rows are the W2c-b merges already on `feat/vol-resume4-20261016`, recorded here because
W2c-b never made its docs commit.

| Merge | Lane SHA | Outcome |
| --- | --- | --- |
| `ef13997a` | L2h-b sigma-q family `c16f2bac` (W2c-b; brings L3g wip `a4b34521`) | review APPROVE_WITH_MINORS; clean; opt-in kernel, no caller |
| `15b93b2c` | L3g family Greeks `e567110c` (W2c-b) | review APPROVE_WITH_MINORS; 1 conflict `american.hpp`, union of the two marked sub-blocks (blob `27b39d53`) |
| `59029ade` | L3f cash-dividend kernel `7d0d8468` (W2c-b) | review APPROVE_WITH_MINORS; clean, opt-in; m5 moved-from `buffers()` guard deferred |
| `7344eb37` | L3r1 dense-chain certificate `09a3c837` (W2c-b) | review APPROVE_WITH_MINORS, opt-in (default `exact`); 2 conflicts, unions (`quote_bench_policy.hpp` `1187c46b`, CLI strict cases `7821abec`) |
| `1d5b324a` | L2h-c family build speed `af74ffe9` (r5 R0) | review APPROVE_WITH_MINORS (n1-n7); clean, kernel-only |
| `ad205211` | L2h-c review n1 (r5 R0) | comments only: `AlSigmaQFamilySelfCheck` / `self_check()` are a 12-point sampled estimate, not a bound |
| `b319975b` | L3q market distance `95506f61` (r5 R0) | review APPROVE_WITH_MINORS (m1-m8), opt-in (`graded.market_distance` off); 1 conflict, allow-list union (`82896af6`) |
| `342aaf2e` | L3a wing tail `4d71ec17` (r5 R0) | review APPROVE_WITH_MINORS (m1-m6), opt-in (`graded.ladder_wing_tail` off, ladder-strict not promoted); 3 conflicts, unions (allow-list `8029c120`, `QuoteSurfaceSlice` fields `767a4ef5`, test tail) + 1 semantic: L3a's test helper `wing_chain` redefined L3r1's same-signature helper, renamed `wing_tail_chain` (`41c641d2`) |

**W2c-b gates at `7344eb37`** (quote-bench `eac00b98...`, the W2c-b pool-1 build modulo the PE link stamp;
`ATX_VOL_FIT_WORKERS=3`; outputs `C:/atx-cache/quote-benchmark/R0-20261022-integrate/base/`): the 8 default hashes MATCH;
the W2c-a keys-ON set 29/29 equal to the W2c-a root `dad69ccb`; kill switch 5/5, R3 repair-dominance-v3 3/3, L3r1 box
h0.5 / h0 `a3859ea1` / `05f64ba6` through the kill switch (under the R3 defaults `c3292d38` / `cb6b3680`, first recorded);
AL fingerprint `53c4df68`; name set `52324ef8` equal (967 tests; the L3g-wip failures the L2h-b / L2h-c lanes saw are
gone); L3f's SPY dividend default-vs-converged table identical to the lane's; gtests pricing 98 (97 + the skip),
`AlSigmaFamily` 11, `AlSigmaQFamily` 9 + alloc, `AlSigmaFamilyGreeks` 15, chain-batch 31, L3f 17 / 88 / 28 / 21, L3r1 15;
CLI 71; combined anchored ctest 272/272 (quote-slice 166); PCH-off 25 TUs, 0 diagnostics.

**Final root `342aaf2e`** (quote-bench `9eae17dd...`, outputs `.../R0-20261022-integrate/final/`): the 8 default hashes
byte-identical before / after:

| Output | sha256 (before = after) |
| --- | --- |
| dev200 default `auto` | `c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` |
| dev200b default `auto` | `4b273b084231f8a3811c7d6505d42500355ae29c864a5c2e1c2b964a7aab70fb` |
| dev200 graded-v1 (R3) | `2bc7777df4448c7f1818d10571859074646d51e12cab4731471947cdd75738f6` |
| dev200b graded-v1 (R3) | `ed34eef7b51d4735748e5534a651569ff69026e07c1655cbb7b2fa6dad1264f2` |
| cohort5 graded-v1 (R3) | `45e491ea311b352ec107658809429ca2583b5bfbdf2215f9c224b86f7fbc1581` |
| SPY graded-v1 (R3) | `feac8d2c41a63ff8687e5e991d4026eeecccdf6e88418edcc874c8fc2ca8425b` |
| dev200 divsched | `abc75955cab8bef821c032ee2a45aa08979b0e92283daad66d3ca2d62bc3bcee` |
| dev200b divsched | `747d3e6cfcb1122629e8add039c2b1fbe9cba784c0032ccf840030fb9daab5d3` |

Keys-ON 29/29, kill switch 5/5, families 3/3 and the L3r1 rows (both arms) equal the base. The merged lanes' identities
(their hashes were recorded on pre-R3 bases, so they are reproduced through the R3 kill switch `{"fit_space": "price",
"grid_kernel": "reference", "rung_memo": false}`): L3q SPY box h0 + repair `2145268a` MATCH (3,992 served), L3a lever-1
cohort5 ladder-strict + floor-price-only `7b450b5b` MATCH (9,520 served). Explicit deltas under the R3 defaults (lane
policies unchanged; first recorded): L3q `ba413a85` (3,992 served), L3a `8b467e37` (9,523 served). L2h-c is kernel-only:
`AlSigmaQFamily` 15/15 with the lane's digits (AVX2 vs scalar build 5.6641e-15, node/anchor 1.3221e-15) + alloc; no
recorded hash serves through the q-family, so the AVX2 rounding-level move touches none. Fingerprint `53c4df68`; name set
`52324ef8` equal (973 tests); L3q 6 gtests + 2 CLI, L3a 5 gtests, quote-slice 177/177, CLI 73, combined anchored ctest
278/278 (native CLI 172, tools 176, campaign 22); PCH-off 33 TUs (every non-bench includer of a changed header except
`american.hpp`'s 143, where the owning q-family / sigma-family TUs stand in), 0 diagnostics. **SPY graded-v1 @1 on the final
root (one run, unqualified, load 12 -> 41 %): fit+reprice 7.61 s (wall 7.74), 106,232 de-Am solves at 56.8 us + 28,908
grid prices at 49.8 us. No G2 / G3 claim.** No default promoted, no holdout touched, nothing pushed.

## W2c-d / r5 R1 integration (vol-sota master plan, 2026-10-22)

Lane W2c-d (run `vol-sota-r5-20261022`, pool-33) merged the reviewed owner rebase into the r5 root
`feat/vol-resume5-20261022`. R1-B had cherry-picked the SPY1s owner work onto the root `9cfd3dd0` as
`feat/vol-owner-rebase-20261022 @ 288d0d03` (code tip `ce22bdbf`; review-R1B APPROVE, 0 blocker / 0 major / 4 minor).
No default promoted; nothing pushed.

| Merge | Lane SHA | Outcome |
| --- | --- | --- |
| `2fce51ce` | R1-B owner rebase `288d0d03`: Phase 2 (`cb7cead0` `c9d99835` `80376a8e` `69e238be` `2e824b48` -> `42e9bc7a` `86e44826` `bfa8c014` `7f9f86ec` `b88f59d0`), M1 `9baa6d76` -> `ecf1e6c3` (patch-id equal), family-native `e648da0c` -> `ce22bdbf` (patch-id equal outside the key-list union) | review APPROVE (4 minors); clean `--no-ff` merge; the WIP certificate `1f96fca6` and the checkpoint note `1d74d36c` are excluded (R1-A MJ4) |

**Tree identity.** `git rev-parse HEAD^{tree}` at `2fce51ce` = `git rev-parse 288d0d03^{tree}` = `f99efea0d5ad0d481e4f0fa248c16bbf600ae8b6`.
Every R1-B phase-B gate therefore applies to the merge byte for byte, and no hash was rerun:
- the 9 default hashes MATCH (the 8 of R0's table + the SPY kill switch `087e6a54`);
- AL fingerprint `53c4df68`;
- keys-ON native SPY `a3c0b01e` / cohort5 `60314cc1` / dev200 `e16d181e` / dev200b `0677e06a`, and famq-grid SPY `86ca3e10`;
- served sets: lost 0, gained 0; the suites and the PCH-off check pass.

The bisect `check` at `ecf1e6c3` was run by the reviewer (exit 0).

**Smoke** (vol-rel, `-Jobs 1`):
- `atx-vol-quote-bench` rebuilt cleanly (CMake re-run + 36 steps, 0 warnings). The exe equals R1-B's `7bf84c23` except
  the 2 PE-stamp bytes at offsets 129-130.
- ctest `^atx-vol-quote-slice`: 1/1, 183 gtests passed.

**What the merged arm means.** `graded.family_native` ON vs OFF serves identical rows but moves cohort5 / dev200 prices
by up to 1.118e-6 / 1.380e-6 USD. That is exactly vega x the fitted-vol move (|dsigma_fit| <= 2.8e-8; review-R1B
F-R1B-1 ruling), not a root error. The arm stays opt-in and non-promotable until all three hold:
- MJ1 (confirm slope) is fixed, by K3;
- MJ2 (typed native refusals) is fixed, by K2;
- a bracketed native root-error record exists (K3).

Keys-ON arms gate in IV units. Docs commit: the R1-B sprint note's B1 bisect and fitted-vol wording (review minors 1-2),
the W2c-b ledger candidates (L2h-b, L3g, L3r1, L3f m1), the R1 baseline lines and this section. Report:
`.superpowers/sdd/2026-10-18-vol-sota/lane-W2cd-integrate-r5-report.md`.

## W2c-e / r5 integration of B0, K1 and D0 (vol-sota master plan, 2026-10-22)

Lane W2c-e (run `vol-sota-r5-20261022`, pool-35) merged three approved lanes into the r5 root
`feat/vol-resume5-20261022`, `--no-ff`, in this order, on top of `9f05a40e`. No default promoted; nothing pushed.

| Merge | Lane SHA | Conflicts | Outcome |
| --- | --- | --- | --- |
| `97f2f8de` | B0 benchmark harness and clock contract `6150dee8` (review APPROVE, re-review 2: all addressed) | none | tree = the B0 tip's `9a13fc86` (B0 already held `9f05a40e`) |
| `90700ea4` | K1 fused sigma/q family price + vega `2301d18c` (review APPROVE, re-review 1: MJ1 / MJ2 addressed) | `atx-vol/CHANGELOG.md` | entry union: B0's entry, then K1's |
| `42dc2ebd` | D0 independent cash-dividend reference `72600798` (review APPROVE, re-review 1: M1 / M2 addressed) | `atx-vol/CHANGELOG.md` | entry union: B0 + K1 entries, then D0's; both CMake lists auto-merged |

**Diff proof.** For every file of every merge, the body of `git diff <merge>^1 <merge>` equals the lane's own change
(merge base to lane tip), and the body of `git diff <merge>^2 <merge>` equals the root's own change (merge base to
first parent). All 180 comparisons are EQUAL_SEQ: 13, 21 and 56 files, two sides each, for B0, K1 and D0
(D0's merge base is `7344eb37`).

**Per-merge suites** (vol-rel, `-Jobs 1`):
- after B0: bench-clocks gtest 11/11, quote-slice gtest 183/183, CLI pytest 181 (B0's 9 cases 9/9);
- after K1: ctest `^.*AlSigmaQFamily` 29/29, Greeks + sigma family + alloc 27/27;
- after D0: ctest discrete-div reference / kernel / smoothing / schedule 4/4 (reference fast gtest 24/24, discrete-div
  gtest 88/88).

**Gates on the final code `42dc2ebd`** (the docs commit after it changes `.md` files only):
- 9/9 default hashes byte-identical: R0's 8 full sha256 (dividend keys-OFF `abc75955` / `747d3e6c` among them) plus
  the SPY kill switch `087e6a54`. quote-bench `6125a08b`.
- AL fingerprint `53c4df68` on vol-rel-avx2.
- Name set at cap 3: 52324ef8's 19 names plus `pricing/PricingExecutor.NestedDispatch_ReachesAPoolWorkerAndWakesThePool`,
  the LEDGER L575 load flake. The executor and its test are untouched since `9f05a40e`; on this shared host it also
  failed 6 of 40 in-process repeats. New names in the run: 14 `AlSigmaQFamilyFused.*` (K1) and 4 `AmericanIvPremium.*`
  inherited from `9f05a40e` (R1-B). All 18 pass.
- Combined anchored ctest (the lanes' suites, `^atx-vol-quote-slice`, the sigma / sigma-q family and AmericanIvPremium
  pricing suites): 75/75. The CLI pytest (`test_quote_bench_cli.py` and `test_common_markov_bench_cli.py`) gives 181
  passed with Python312. The `atx-vol-quote-benchmark-native` ctest fails only on its configured venv's missing polars
  (L467).
- `^atx-vol-pricing` is only `atx-vol-pricing-forcescalar`, which runs the monolithic `atx-vol-tests`. Its ForceScalar
  filter was run directly on the pricing and simd slices instead: 94 + 1 passed, 4 skipped, 1 known failure
  (`AmericanIv.PricePolishDefaultOffIsBitForBitControl`, already in 52324ef8). ScalarLegEnv passed (not skipped).
- PCH-off (`vol-hygiene`): 22 TUs / 26 objects, 0 diagnostics. They cover every non-bench owner of the six headers
  changed since `9f05a40e`, including the four owners of `bench_clocks.hpp`.

**G1 on the root after W2c-e** (shared host; G1 evidence, no F1 claim). B0 harness, 10 fresh-process cold trials,
`ATX_VOL_FIT_WORKERS=1`, CPU 6 P core, `-RequirePCore -RequireIdleSibling`; cold_calibration_s:

| Workload | Policy | min / median / p95 / max (s) | SMT contended | Predictions | Manifest |
| --- | --- | --- | --- | --- | --- |
| section 9 SPY | `graded-v1-famq-grid-native.json` `9091b102` | 5.77 / 6.51 / 6.94 / 7.14 | 0/10 | `a3c0b01e` | `8fe510c8` |
| default graded-v1 SPY | `graded-v1.json` `d7f89d13` | 7.78 / 9.15 / 9.65 / 9.77 | 0/10 | `feac8d2c` | `794f1812` |

Each row is the first exit-0 run under a rule fixed before the retries (at most 3 attempts per policy). The earlier
attempts exited 4 on sibling contention and are kept in the evidence folder: famq 1/10 and 6/10, default 1/10.

**Routed, not fixed here:**
- B0's comparator found one decidable failure (cohort5 AAPL call K=10, excluded side, 6.8e-4 vol) and the
  131 SPY / 780 cohort5 IV-not-identified served rows. Both go to C0 / H0.
- D0's memo recommends event-adaptive dividend handling over a raised 301-step default; any change is a later lane's
  decision.

### Deferred minors (W2c-e: B0, K1, D0 reviews)
- B0 m4: `run_cold_trials.ps1` leaves `fast_math` unasserted on the CMakeCache fallback, and `source.sha` is the
  tree HEAD at run time rather than bound to the binary.
- B0 m8: `same_predictions` skips `policy_source`, `configured_curve` and `model`, although the doc says "every
  published field".
- B0 m9: affinity API failures throw without `GetLastError()`, and `DWORD_PTR{1} << cpu` is valid only on x64.
- B0 m5 / m7: documented in `benchmark-clocks.md` section 3 ("Known inclusions") but not changed in code.
  - m5: static-path fits under `--timing-detail` run inside a ChainRecorder (with `--deam-audit` this adds discarded
    Search inversions).
  - m7: `fit_other` and the normalization / publication stages include about 10 ms of diagnostic JSON bookkeeping.
- K1 m1: the 5b model-vega table is thin. It has 3 mid-q cases on the default ISA, the spy23 call row is vacuous
  (European-dominated), and the "converged" scheme's own uncertainty is not shown.
- K1 m3: arms without tests: the `AlProfile::Fast` premium pass, the Linear sigma-axis chain, and interior sigma-node
  near-hits. This matters once K2 or the owner runs families on `fit_.al_opts` with Fast.
- K1 m5: `interpolate_derivs` accumulates the q derivative even when `dy_dq == nullptr`, about a third of the
  contraction work on the one-shot price path.
- D0 m2: the Howard tie rule's 1024-ulp constant is empirical and not bounded in u. It binds at its cap on G1c / G4c.
  The comment "in practice 1-4" and the doc's "the constraint adds no error of its own" are stale.
- D0 m4: the fast-suite resolution pin `u < 1e-6 x BS vega` is a loosening of the first `u < 2e-5 USD` pin
  (anchor K120 T3 needs it). Describe it as one.
- D0 m5: the t -> 0 limit is checked for the European case only, with a hand-picked 1e-4 slack. The American limit is
  not pinned.
- D0 m6: the slow matrix ctest asserts only `rows.size() == 50`. Add `EXPECT_EQ(disagree, 0U)`, and optionally pin the
  UNRESOLVED / PRICE-ONLY id sets.
- D0 m7: the A base at step 150.5 rounds half-to-even to 150, so no row moves the kernel's ex-step down by one.

Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-W2ce-integrate-r5-report.md`. Evidence:
`C:\atx-cache\quote-benchmark\W2ce-20261022-integrate\`.

## W2c-f / r6 integration of C1, K2 and C0 (vol-sota master plan, 2026-10-23)

Lane W2c-f (run `vol-sota-r6-20261023`, pool-5) merged three approved lanes into the root
`feat/vol-resume5-20261022`, `--no-ff`, in this order, on top of `668ad80e`. K3 is not in this wave (W2c-g). No
default promoted; nothing pushed.

| Merge | Lane SHA | Conflicts | Outcome |
| --- | --- | --- | --- |
| `cea079fc` | C1 exercise region `715d4210` (review APPROVE at `1404ef80`; re-review of fix round 1: ALL ADDRESSED) | `atx-vol/CHANGELOG.md` | entry union: the root's entries, then C1's |
| `befa8fb8` | K2 batch inverse `e47fe501` (review APPROVE at `f0186be1`; re-review of fix round 1: ALL ADDRESSED) | `atx-vol/CHANGELOG.md` (two hunks; merge bases `6150dee8` and `2301d18c`) | entry union: the root's entries, then K2's three; K2's copy of K1's entry is byte-identical to the root's, so it appears once |
| `776df009` | C0 contracts `e22ff2c4` (review APPROVE at `c7679508`; re-review of fix round 1: ALL ADDRESSED) | none | CHANGELOG, `tests/CMakeLists.txt`, `tests/slices.cmake` and `tools/quote_bench_main.cpp` (K2's `--inverse-replay` beside C0's serve / snapshot hunks) auto-merged |

**Diff proof.** For every file of every merge, the body of `git diff <merge>^1 <merge>` equals the lane's own change
and the body of `git diff <merge>^2 <merge>` equals the root's own change since the merge base: 39, 35 and 70
comparisons, all EQUAL_SEQ. K2 has two merge bases; git's virtual base equals `90700ea4`'s tree on every file except
the CHANGELOG, so `90700ea4` is the proof base, and the CHANGELOG is proven by an entry-set check instead: the merged
file's `#`-heading blocks are exactly the first parent's plus the lane's new ones, with no block lost or duplicated
(OK at all three merges).

**Per-merge suites** (vol-rel, `-Jobs 1`, `ATX_VOL_FIT_WORKERS=3`):
- after C1: C1's anchored AL ctest 127/127; `AlMonotone` + `AmericanExerciseRegion` 16/16 on AVX2 and on
  ForceScalar (four-site grid 16000 points, `laned=1` / `laned=0`); the root's W2c-e regex 75/75;
- after K2: K2's anchored ctest (IvBatch, AmericanIv*, sigma-q family, alloc, quote-slice, bench-clocks) 117/117
  (the known `PricePolishDefaultOffIsBitForBitControl` excluded); C1's AL regex + the W2c-e regex 204/204;
- after C0: quote-slice, fit-archive, implied-forward, discrete-div-smoothing, iv-batch-alloc, bench-clocks 6/6;
  `atx-vol-umbrella` fails with its 3 known tests only.

**Gates on the final code `776df009`** (the docs commit after it changes `.md` files only):
- 9/9 default hashes byte-identical to the W2c-e table, `sha256sum -c` 9 OK, stderr files 0 bytes. quote-bench
  `83d3b40c`.
- AL fingerprint `53c4df68` on vol-rel-avx2 (library then bench targets, 73 s + 30 s).
- Name set at cap 3: identical to W2c-e's (52324ef8's 19 + the L575 NestedDispatch flake; executor code untouched).
  37 new names, all passing, 0 removed: C1 `AlMonotone.*` 6 and `AmericanExerciseRegion.*` 10, K2 `IvBatch.*` 19 and
  `AmericanIvPremium.*` 2.
- Combined anchored ctest (W2c-e regex + iv-batch-alloc, fit-archive, implied-forward, IvBatch, AlMonotone,
  AmericanExerciseRegion), native ctest excluded: 115/115. CLI pytest on the two files directly: 190 passed.
- ForceScalar filter on the pricing / simd slices: 106 + 1 passed, 4 skipped, 1 known failure
  (`AmericanIv.PricePolishDefaultOffIsBitForBitControl`); ScalarLegEnv passed.
- PCH-off (`vol-hygiene`): 58 TUs (every non-bench, non-python includer of the 15 changed headers, transitively
  through in-tree headers), exit 0, 0 first-party diagnostics.
- Keys-ON identities: C0's 10 rows equal its fix-round hashes (floor `374341cd` / `1aab494e` / `346211b9` /
  `feac8d2c`, one_spot `a9dec618` / `ed34eef7` / `34e3ed27` / `d7a48be7`, box-h0 `cb6b3680` / `34d5aab0`); K2's SPY
  `family_native` ON with `--inverse-replay avx2` `5abc6bfe` (29725 requests, 28356 converged + 296
  sigma_above_box) and OFF `86ca3e10`: lost 0, gained 0, max |dP| 4.0e-11. C1's region rule has no policy or CLI
  key (API-only until K4), so it has no keys-ON row.
- Python wheel: not built. `pip wheel --no-build-isolation --no-index` stops at configure on the version-mirror guard
  in `atx-vol/python/CMakeLists.txt` (`${atx_VERSION}` is empty in the python project's scope; LEDGER trap of this
  date). Routed to the PM; nothing installed.

### Deferred minors (W2c-f: C1, K2, C0 reviews)
- C1 m1: the `american_region.hpp` header's measured classes are the AlMonotone grid's only (no L1c band tops, no
  5.8e-7 edge class), and its ZeroVolLimit gap list omits the double-continuation regime.
- C1 m3: `american_region_test.cpp:900` accepts an error from the geometry classified route; it should assert a value
  that is not priced.
- C1 m4: lane 0 of every pack is the same anchor contract, so the four-site counts include repeats (about 12,020
  distinct points, 723 near-edge, 128 grid-lane mismatches).
- C1 m6: the plateau-edge class is measured for the accurate preset only; measure al_bulk_opts / al_fast_opts before
  any leg on those presets runs `Intrinsic`.
- C1 m7: the Greeks routes stay on KernelFloor and report a nonzero vega on plateau contracts; PM decision whether to
  classify them or publish plateau Greeks from the class.
- C1 concerns: K4 must not enable `Intrinsic` on any inversion leg before the `ScopedAloPricer` slot switch; `:414` /
  `:740` go through Tier-A `american_price`; `batch_chain_iv.cpp`, `batch_chain.cpp` and the AVX2 driver have no
  lane owner; K1's fused `premium_pass_t` is a fifth inline floor site. C1's Tier-B header moves the pre-existing red
  umbrella counts by one (44 / 103).
- K2 MJ1 (cross-route carry) and m8 (typed fit-level counters): K4.
- K2 m2 (batch-error and map-NaN typing holes), m5 (stream recorder robustness, bench only), m9 (function size and a
  hot-path allocation): deferred.
- K2 re-review notes: no mixed good / refused tile test for m4; the recheck band also trips deep-OTM lanes (perf only,
  unmeasured on the replay); `FastProfileTablesEvaluateOnTheScalarMap`'s Auto assertion is trivial on a non-AVX2
  host. `LaneAcceptance` stays a placeholder until K3's `RootAcceptance`; the stop/go was not re-run at `e47fe501`.
- C0 residual: the opt-in ladder wing tail types its rows with `floor_vol`, not `identify_served` (candidate for C2).
- C0: `query_quote_slice` ignores per-row market-distance gating (C2); no exercise-style / certified-domain support
  flag (H0 / A1); B0's `served_iv_rows` still counts Legacy floor rows.
- C0 re-review notes: the agreement test is partly self-referential (the M2 pins carry the independent values); the
  lane's evidence dir has no standalone `sha256sum -c` log (re-run here: 9 OK).

Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-W2cf-integrate-r6-report.md`. Evidence:
`C:\atx-cache\quote-benchmark\W2cf-20261023-integrate\`.

## W2c-g / r6 integration of K3 (vol-sota master plan, 2026-10-23)

Lane W2c-g (run `vol-sota-r6-20261023`, pool-5) merged the approved K3 lane into the root
`feat/vol-resume5-20261022`, `--no-ff`, on top of `914a28ef`, then fixed the atxvol wheel's version-mirror guard and
built the wheel once. No default promoted; nothing pushed.

| Commit | What | Conflicts | Outcome |
| --- | --- | --- | --- |
| `a005113a` | merge K3 error envelope `91d1f669` (review BLOCK at `02eab1ed`; fix round 1; re-review ALL ADDRESSED at `91d1f669`; merge base `668ad80e`) | `CHANGELOG.md`, `src/pricing/american_iv.cpp`, `src/pricing/american_iv_premium.hpp`, `tests/american_iv_test.cpp` | union, plus the semantic glue below |
| `d79adeed` | `build(vol-python)`: the version-mirror guard reads `atx_VERSION` from the root directory's scope | - | wheel configures; drift still fails |

**Conflict resolution.** CHANGELOG: C0's entry, then K3's (entry-set check OK: 131 blocks = the root's 130 + K3's
one). Includes: both lanes' lines in all three C++ files. `IvPremiumStats`: K2's `native_*` fields and K3's
`acceptance` / `native_partner_span`. The `qfamily_native` comment keeps K2's iterate description and K3's
acceptance description; each side's sentence the other supersedes is dropped. The native branch of
`premium_root` takes K3's `accept_family_native` (K2 had changed the same call's partner argument to `nullptr`).

**Semantic glue (the textual union does not compile).** K2's batch-seam rewrite removed `NativeRoot::partner`: the
lane is a bracketed Newton on the fused vega and has no secant partner. K3's `accept_family_native` read the
partner only for the diagnostic `IvPremiumStats::native_partner_span`, which is now 0 on the native route
(documented at the field). K3's `AmericanIvPremium.FamilyNativeAcceptanceDoesNotRestOnANonLocalSecant` forced a
far secant partner through the memory's family points, which K2's lane never reads, so its precondition
`EXPECT_GT(far_partner, checked / 2)` failed (0 vs 28) while every acceptance assertion passed (56 roots, max
bracketed error 7.81e-10); it is now `EXPECT_EQ(far_partner, 0u)`, the acceptance assertions unchanged. Both
glue hunks are in the merge commit and its message.

**Diff proof.** Against the K3 blob (`914a28ef..merge` vs `668ad80e..91d1f669`) and against the W2c-f root blob
(`91d1f669..merge` vs `668ad80e..914a28ef`): 63 per-file comparisons EQUAL_SEQ; the 6 others (the three glue files
on both sides) differ only by the listed lines: the glue, the one line both lanes edited (the native accept call),
the `<limits>` include both lanes added (once), and the two superseded comment sentences.

**Per-merge suites** (vol-rel, `-Jobs 1`, `ATX_VOL_FIT_WORKERS=3`): K3's pricing anchor + quote-slice 86/86; K2's
final-gate regex 121/121 (quote-slice counted in K3's run); C0's quote-slice, fit-archive, implied-forward,
discrete-div-smoothing, iv-batch-alloc, bench-clocks 6/6 (`atx-vol-umbrella`: its 3 known failures, counts 44 /
103 as at W2c-f).

**Gates on the final code `d79adeed`** (the docs commit changes `.md` files only):
- 9/9 default hashes byte-identical, `sha256sum -c` 9 OK, stderr files 0 bytes; quote-bench `f9b2a473`.
- AL fingerprint `53c4df68` on vol-rel-avx2 (library 117 s, then bench targets 30 s).
- Name set at cap 3: exactly R0's `52324ef8` (the L575 flake did not fire this run). K3's 15 new names all pass
  (`AmericanFamilyError.*` 9, `AmericanFamilyRootAudit.*` 1, `AmericanIvPremium.*` 5); 0 removed.
- Combined anchored ctest (W2c-f regex + `AmericanFamilyError`, `AmericanFamilyRootAudit`), native ctest excluded:
  130/130. CLI pytest on the two files directly: 190 passed.
- ForceScalar filter on the slices: 121 + 1 passed, 4 skipped, 1 known failure
  (`AmericanIv.PricePolishDefaultOffIsBitForBitControl`).
- PCH-off (`vol-hygiene`): 80 TUs over the 5 changed headers (`quote_slice.hpp`, `counters.hpp`,
  `deam_iv_cache.hpp`, `american_family_error.hpp`, `american_iv_premium.hpp`), exit 0, 0 diagnostics (30 bench
  TUs, 2 example TUs and 2 python binding TUs are not configured in vol-hygiene; the bindings compiled in the
  wheel build with no first-party warning).
- Keys-ON (SPY, workers 2): K3 native `famq-grid-native` = K2's lane + K3's acceptance `710115bb` (neither lane's
  own hash), served 1410, lost 0 / gained 0 against R1-B `a3c0b01e`, K3 `2debd48d` and K2 `5abc6bfe`, max |dvol|
  2.3e-12, max |dP| 2.7e-11 against K3; work vs K3: confirm solves 21,916 -> 21,913, skips 8,065 -> 8,197,
  fallbacks 528 -> 520 (K2's 520), family prices 160,287 -> 145,052. B-family `13f6be35` = K3's, work identical.
  K2's `--inverse-replay avx2` row equals the native row; its `bench.inverse_replay` block is field-for-field
  W2c-f's (13/13: 29725 requests, 28356 converged + 296 sigma_above_box, fit_native 26689 status-equal, max
  dsigma 1.99e-13); its keys-OFF row moves `86ca3e10` -> `6368997d` = K3's famq-grid row (the family grid now serves
  no price), lost 0. C0's 10 rows equal its fix-round hashes.
- K3 bracket audit (`--deam-audit`, per-root CSV): SPY native 27,437, SPY B-family 27,182, cohort5 39,362, dev200b
  59,737, dev200 89,654 typed roots; 0 violations of 1e-7, 0 unresolved on all five (negative-carry subsets
  included); LegacyBandGate / LegacyNever 0; every row lost 0 vs R1-B and vs K3.
- Python wheel: the guard fix configures (before: `... the root CMakeLists is .`; after: configure done; a
  temporary pyproject `1.1.1` stops with `declares 1.1.1, but ... is 1.1.0`, restored). One wheel build at `-j 1`
  (stated 30-minute exception): 1237 s, `atxvol-1.1.0-cp312-cp312-win_amd64.whl` sha256
  `0ad53ac8390ed825af9c2dbfaf562745670a8bf7d58c81590dc7501f86f5b2d4`. In a scratch venv (numpy, pytest, pandas,
  pyarrow 18): `test_quote_slice.py` 5/5, `test_atxvol.py` 9/9 (the wheel-metadata version assertion included);
  14 binding files 217 passed / 24 failed / 5 skipped (20 = `run_sp100_strangle_backtest.py`'s driver refusing an
  atxvol outside the tree, by design; 2 `test_report_from_archive` friction_regime refusals; 2
  `test_surface_db_build` carry_failed cells; neither code path is touched by W2c-f or W2c-g); `test_chain.py`
  cannot import beside pyarrow 18.

### Deferred minors (W2c-g: K3 review, fix round, integration)
- K3 mn-1 (the family certification grid serves 0 prices under the validated domain-wide bound; a per-point grid
  bound) and mn-4 (PriceOnly / Interval fall back to Search, which serves an identified IV; publishing them needs a
  validated vega lower bound and a fallback change): K4.
- K3 mn-2 (informational): the B-family arm spends far more confirm solves (SPY 185 -> 19,566) for its 1e-7
  statement.
- K3 for K4: the thread-local `FamilyEnvelopeScope` must become an `IvPremiumGate` field before any lane is evaluated
  off the inverting thread (a lane outside the scope silently confirms everything); `family_enclosure` is declared
  in `src/pricing/american_iv_premium.hpp` for its per-end test, keep it reachable through a batch seam; the
  family grid's rung-memo key carries `q_bound` bits (in-process memo only); the trivial box bound alone certifies
  enclosures on low-premium families.
- K3 re-review: the negative-carry domain-bound test's built-envelope arm only reaches 0.0033 of the bound (the box
  term was the defect and is pinned through the every-node-unverified arm).
- W2c-g integration: `NativeRoot`'s comment still calls the fused vega "the confirm's LOCAL slope" and
  `IvPremiumStats::native_status` says "Converged with the root confirmed as before"; since K3 the native root goes
  through `accept_family_native`, which uses the enclosure's chord, not that vega. `native_partner_span` is a constant
  0 on the native route and could be retired from the audit CSV.
- W2c-g wheel: the wheel does not bundle `arrow.dll` / `parquet.dll` (a delvewheel-style repair or static Arrow,
  as the python README lists); with this build's Arrow 24 the README's measured "import atxvol, then pyarrow, works"
  no longer holds (pyarrow 18 fails after atxvol); 4 binding tests fail on code these lanes did not touch.

Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-W2cg-integrate-r6-report.md`. Evidence:
`C:\atx-cache\quote-benchmark\W2cg-20261023-integrate\`.

## W2c-h / r6 integration of D1 and A0 (vol-sota master plan, 2026-10-23)

Lane W2c-h (run `vol-sota-r6-20261023`, pool-5) merged the approved D1 (cash-dividend production) and A0 (sigma/q
family Greeks) lanes into the root `feat/vol-resume5-20261022`, `--no-ff`, in that order, on top of `92cea7df`. No
default promoted; nothing pushed.

| Commit | What | Conflicts | Outcome |
| --- | --- | --- | --- |
| `5fba816b` | merge D1 cash-dividend production `bf1ed68e` (review BLOCK at `a203eb19`; fix round 1; re-review ALL ADDRESSED at `bf1ed68e`; merge base `668ad80e`; the lane's checkpoint commit `fc136391` stays under its root merge `185f679c`, PM ruling) | `CHANGELOG.md`, `tests/CMakeLists.txt` | union, no glue |
| `085240c5` | merge A0 sigma/q family Greeks `ed2d2e05` (review BLOCK at `684b2ed4`; fix round 1; re-review ALL ADDRESSED at `ed2d2e05`; merge base `668ad80e`) | `CHANGELOG.md`, `CMakeLists.txt`, `tests/CMakeLists.txt` | union, no glue |

**Conflict resolution.** CHANGELOG at both steps: the root's entries first, then the lane's (D1: its FIXED review
round and its NEW production-lattice entry; A0: its NEW Greek-bundle entry); the entry-set check is OK at both
commits (133 = 131 + 2, 134 = 133 + 1; nothing lost, nothing duplicated). `ATX_VOL_SLOW_FILTER`: the root's
`AmericanFamilyRootAudit.*`, then `DiscreteDivProductionMatrix.*`, then `AlSigmaQFamilyGreeksModel.*`.
`atx-vol/CMakeLists.txt`: K2's `american_iv_batch.cpp` / `american_iv_stream.cpp` block, then A0's
`american_sigma_q_family_greeks.cpp` block. Auto-merged: `american.hpp` (D1's `SplicedCubic` enumerator and A0's
Greeks block are disjoint), `fit_archive_records.cpp` (D1's scheme range beside C0's records), its test,
`quote_bench_policy.hpp`. The union compiled as is: no semantic glue (the W2c-g `NativeRoot::partner` code is
untouched by both lanes). Neither lane touches `quote_slice.cpp`, `deam_iv_cache*` or `american_iv.cpp` (K4's
files).

**Diff proof.** Per file, the merge's change against each parent equals the other side's own change since
`668ad80e`: D1 merge 79 EQUAL_SEQ, A0 merge 87 EQUAL_SEQ; the only residual (2 per merge) is the slow-filter
line in `tests/CMakeLists.txt`, which carries both sides' appends.

**Per-merge suites** (vol-rel, `-Jobs 1`, `ATX_VOL_FIT_WORKERS=3`). After D1: production fast 27/27, ddiv 88/88,
reference fast 24/24, fit-archive 57/57 (D1's 54 + C0's 3), anchored ddiv / fit-archive ctests 6/6, K3's pricing
anchor 85/85. After A0: A0's closure 60/60 (slow model suite included), alloc 3/3, K2's final-gate regex 121/121,
and D1's production 27, ddiv 88, reference 24, fit-archive 57 again.

**Gates on the final code `085240c5`** (the docs commit changes `.md` files only):
- 9/9 default hashes byte-identical (quote-bench `e8567a41`), `sha256sum -c` 9 OK with the log persisted, stderr
  files 0 bytes.
- AL fingerprint `53c4df68` on vol-rel-avx2 (bin byte-equal to W2c-g's).
- Name set at cap 3: exactly `52324ef8` (the L575 flake did not fire); A0's 15 new pricing names all pass, 0
  removed (D1's tests live in their own targets).
- Combined anchored ctest (W2c-g regex + `atx-vol-discrete-div-production` + `AlSigmaQFamilyGreeks`,
  `AlSigmaQFamilyGreeksModel`), native ctest excluded: 146/146; CLI pytest on the two files directly: 190 passed.
- D1 slow matrix ctest once (stated ~3 min): `atx-vol-discrete-div-production-matrix` Passed, 149.45 s.
- A0 on vol-rel-avx2: the five family suites 69/69, AVX2-vs-scalar numbers equal to the lane's (S.Gamma 4.85e-14,
  theta 3.68e-13).
- ForceScalar filter: 121 + 1 passed, 4 skipped, 1 known failure (`PricePolishDefaultOffIsBitForBitControl`).
  `atx-vol-umbrella`: its 3 known failures; counts 45 / 104 (+1 each: D1's new public header
  `american_discrete_div_batch.hpp`).
- PCH-off (`vol-hygiene`): 36 TUs (the full closure of the 6 private / D1 / A0 headers, plus `american.cpp` for
  `american.hpp` and the lanes' changed TUs), exit 0, 0 diagnostics; the Debug library was precompiled in bounded
  batches first (196 TUs, 0 diagnostics).
- Keys-ON (all MATCH): D1 dev200 `fc81aad3` / dev200b `c6016745` (served 12,092 / 20,662); K3 SPY native `710115bb`
  and B-family `13f6be35` = W2c-g; K2 replay ON `710115bb` and keys-OFF `6368997d` = W2c-g, replay block 13/13;
  C0 one_spot dev200 `a9dec618`, dev200b `ed34eef7`, cohort5 `34e3ed27`, SPY `d7a48be7`, SPY box-h0 `34d5aab0`.

**Gate wording.**
- G5 (D1, PM ruling): splice defect removed; Standard <= 3 bp / Refined <= 1 bp accuracy class on every RESOLVED D0
  row; per-event drift within D0's u NOT met; P1/P2/P3 cash budgets unmet.
- A0: P3 bundle <= 20 us MET; 300-chain 1 ms stretch MISSED.

### Deferred minors (W2c-h: D1 and A0 reviews, fix rounds, integration)
- D1 re-review: the European parity pin covers one ex-date placement (1.4 steps before expiry) and K 100 / 130;
  the 0.6-step ex-date and K 90 / 110 are not pinned (parity is structural, cheap insurance).
- D1: the bounded challenger (one prepared Chebyshev-in-sigma family with the predeclared acceptance table in
  `docs/hft-support-matrix.md`) was not run; every P1 / P2 / P3 cash-dividend latency budget is a MISS (cold 24.1,
  price 23.8, IV 114.2, bundle 191.3 us p50); inverses are 99.94 % of the cold clock.
- D1: the per-event drift misses D0's u on 3 of 4 chains (Refined) and 4 of 4 (Standard); ILp Refined +4.49 bp
  (UNRESOLVED row); the study's vega / rho / dP/dD / roll bounds are accuracy classes, not u-derived.
- D1 re-review nits: one 102-column line in `fit_archive_records_test.cpp` (copied from older 102-column lines);
  C4c vega reads -0.08 % (bump -> 0) in the doc and -0.072 % (served bump) in the report; the study's
  `exercise_dominated` lambda names F1c; the `atx-vol-fit-archive-bench` publish path itself was not exercised.
- A0: the L3g sigma-family bundle still serves the in-band theta with status Ok (review-L3g m5 in-band part OPEN for
  the L3g owner).
- A0 re-review: `docs/sigma-q-family-greeks.md:160` and `:164` still quote the pre-fix pins (S.Gamma 2.4e-12 / theta
  7.9e-10; 7.8e-14 / 2.2e-11) instead of 4.9e-14 / 3.7e-13 and 3.6e-16 / 7.3e-16; the "32 of 317 SPY 23 d rows in
  the band" count has no reproducible source; the m5 allocation-free refusal relies on SSO capacity; the 0.25 band
  is conservative beyond ~0.1 sigma sqrt T.
- A0: the 300-chain <= 1 ms stretch is missed (1.7-2.4 ms p50 on a loaded host); it needs a lane-batched SIMD pass
  or a second thread.

Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-W2ch-integrate-r6-report.md`. Evidence:
`C:\atx-cache\quote-benchmark\W2ch-20261023-integrate\`.

## W2c-i / r6 integration of C2 and H0-A, A0 / K3 seams on (vol-sota master plan, 2026-10-23)

Lane W2c-i (run `vol-sota-r6-20261023`, pool-5) merged the approved C2 (repair consistency / publication
validation) and H0-A (serving bundle phase A) lanes into the root `feat/vol-resume5-20261022`, `--no-ff`, in that
order, on top of `5728edc6`, then switched H0-A's A0 / K3 compile-time seams on. No default promoted; nothing pushed.

| Commit | What | Conflicts | Outcome |
| --- | --- | --- | --- |
| `bdea828b` | merge C2 repair consistency `15bbeba7` (review BLOCK at `ea3d67ab`, M1 + m1-m5; fix round 1; re-review ALL ADDRESSED at `15bbeba7`; merge base `e22ff2c4`) | none | clean auto-merge, no glue |
| `53692d40` | merge H0-A serving bundle phase A `f8c431f0` (review APPROVE, 15 minors; merge base `914a28ef`) | `CHANGELOG.md`, `CMakeLists.txt` | union, plus the semantic fix below |
| `2126029c` | `feat(vol)`: `ATX_VOL_SERVE_RECORD_A0` / `_K3` default to 1 | - | serve-record 28/28 |

**Conflict resolution.** C2 auto-merged everywhere (`quote_slice.cpp`, `quote_slice.hpp`, `CHANGELOG.md`,
`CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/slices.cmake`, `quote_bench_main.cpp`, `quote_bench_policy.hpp`);
its `quote_slice.cpp` hunks are its declared repair / validation / publication functions, none in a region K4's
branch touches. H0-A: CHANGELOG = the root's C2 entry, then H0-A's (entry-set check 136 = 135 + 1); the library
source list = C2's `quote_slice_publication.cpp`, then H0-A's comment and `slice_serve_record.cpp`.

**Semantic fix (the textual union does not compile).** K3 (W2c-g, after H0-A's base) removed
`deam_family_probe_worst` from `deam_iv_cache.{hpp,cpp}` and replaced it in the fitter by `deam_family_certify`.
`slice_serve_record.cpp` keeps its approved measured family bound through a file-local copy of the removed
function (body byte-equal to `914a28ef`'s), plus `#include <numbers>` and one include comment (+54 / -1).
Mirroring the fitter's `deam_family_certify` instead compiles but fails 7 of 26 serve-record tests: the
envelope-derived bound makes every ladder of the fixture ineligible. Flagged for review; the K3 bound in the record
is a phase-B / H1 decision.

**Diff proof.** C2 merge: 89 of 89 per-file diffs EQUAL_SEQ (16 vs the C2 blob, 73 vs the root blob). H0-A merge:
67 EQUAL_SEQ; the only residual is `slice_serve_record.cpp` on both sides, exactly the semantic fix.

**Seam flip.** Both lanes are in the root (K3 `91d1f669` since W2c-g, A0 `ed2d2e05` since W2c-h), so the seams
default to 1. The default policy's `policy_sha256` moves `87318ae7…cefe` -> `57902383…f3fc` (read with a scratch
probe test, never committed). Serve-record suite with both seams on: 28/28 = the 26 +
`FamilyTierGreeksAreA0sBundleToTheBit` (A0) + `FamilyRootsAreAcceptedByTheEnvelopesEnclosure` (K3); the two seams
were never built together before (review-H0A m11, closed). Only `slice_serve_record.cpp` and its test include the
seam header.

**Per-merge suites** (vol-rel, `-Jobs 1`, `ATX_VOL_FIT_WORKERS=3`). After C2: quote-slice 223/223 (C2's 221 + K3's
net 2), `QuoteSlicePublication.*` 28/28, fit-archive 57/57, bench-clocks 11/11, anchored ctests 3/3, CLI pytest
193. After H0-A: serve-record 26/26 (seams off), `^serve-record:|^tick-path:|^fitting/SessionServe|^atx-vol-quote-slice$`
90/90, `QuoteSlicePublication.*` 28/28.

**Gates on the final code `2126029c`** (the docs commit changes `.md` files only):
- 9/9 default hashes byte-identical (quote-bench `6ebdaf61`), `sha256sum -c` 9 OK with the log persisted, stderr
  files 0 bytes.
- AL fingerprint `53c4df68` on vol-rel-avx2 (bin byte-equal to W2c-h's; 2.9 s); the record bench
  `atx-vol-serve-record-bench` builds on vol-rel-avx2 with both seams on.
- Name set at cap 3: `52324ef8` plus the L575 flake, which fired this run; 1062 names, 0 added, 0 removed (C2's and
  H0-A's tests live in their own targets).
- Combined anchored ctest (W2c-h regex + `^serve-record:`), native ctest excluded: 174/174; CLI pytest on the two
  files directly: 193 passed (`test_quote_bench_cli.py` alone 94 = C2's count).
- Direct: quote-slice 223, `QuoteSlicePublication.*` 28, serve-record 28, fit-archive 57.
- ForceScalar filter: 121 passed, 4 skipped, 1 known failure (`PricePolishDefaultOffIsBitForBitControl`).
  `atx-vol-umbrella`: its 3 known failures; counts 46 / 105 (+1 each: H0-A's public header `slice_serve_record.hpp`).
- PCH-off (`vol-hygiene`): the full closure of the 7 changed headers, 35 TUs (the bench TU is not configured
  there), exit 0, 0 diagnostics.
- Keys-ON (all MATCH, each lane at its own worker count): C2 dev200b h0 + repair `2e31e087`, h0.5 + repair
  `c4612fc3`, + validate `228b1a94` / `c1691f1e` (served 57,926 / 57,997 / 57,388 / 57,459), SPY / cohort5 arms
  `ba413a85` / `4acd8575` / `d834b4d3` / `a90b5047` / `c3292d38` (all at `ATX_VOL_FIT_WORKERS=2`, as C2 ran them);
  D1 `fc81aad3` / `c6016745`; K3 `710115bb` / `13f6be35`; K2 `710115bb` / `6368997d`, replay block 13/13; C0
  one_spot `a9dec618` / `ed34eef7` / `34e3ed27` / `d7a48be7` / `34d5aab0`.

### Deferred minors (W2c-i: C2 and H0-A reviews, fix rounds, integration)
- C2 (P0): the keys-ON repair arms are not run-to-run reproducible at fan-out width >= 2 (2 SPY chains, <= 5.1e-13,
  seen on the C0 base binary too); root cause not located (per-thread DeAm IV memo / AloPricer state candidates).
- C2 (H1): the ATXVFA fit archive does not store `QuoteSliceFit::withheld_nodes`; a decoded fit re-serves every
  `MarketDistanceFailure` row's off-market value (needs a `WithheldNodes` section and a round-trip test).
- C2 (C3 / G4, review B(3)): `validate` on the default graded path refuses 440 / 498 in-market dev200 / dev200b
  rows (547 on dev200b box + repair + validate); the validator is right: the default path publishes deep-ITM call
  nodes at S - K below S - dfK (C1 of the non-dividend certificate, 141 violations on 34 chains, mostly sub-tick).
  C3 picks: regime floors in the repair rungs and joint projection, a sub-tick allowance, or refusal.
- C2 re-review: no committed surface test sees a revert of the channel / market Recheck gates (unit tests and the
  dev200b hashes pin them); the M1 test pins `publication_checks == 0` and must be revisited if another repair
  commits on that snapshot; `quote_surface.cpp:52` include order (nit).
- H0-A (H1 / phase B / Q0), the review's 15 minors: m1 `implied_vol` has no staleness check; m2 the stale gate fails
  open at `now_ns = 0`; m3 the Family-tier certificate between nodes at the fit spot is the family's, not the
  chord's; m4 `bytes()` under-counts geometries (>= 2.94 MB per SPY symbol, not 2.57); m5 `implied_vol` with
  `tier = Cold` still runs the family Newton and a Fast confirm; m6 ladder IV `Ok` with an ungated `bound_iv`; m7
  `build_serve_record_with` does not check the holder's T / r / AlOpts / anchor; m8 the public `SliceServeRecord`
  constructor accepts nullptr; m9 a third replica of `zero_vol_american`; m10 the AggregateFundingPV carry replica
  is not bit-pinned; m11 closed here (28/28 with both seams); m12 the full mixed 300-chain is not met (~0.3 ms
  estimate); m13 outside the probed band the certificates are one-point measurements; m14 the seam state is visible
  only through `policy_sha256`; m15 a duplicated differentiated Clenshaw recurrence and a family-relative ladder
  certificate.
- H0-A (Q0): K3's put envelope is box-bound on the fixture (34 unverified nodes, 0.04-0.5 USD), so no put root is
  accepted by enclosure; the tier-1 chain is 15x over the 300-chain target; every latency row is unqualified.
- W2c-i integration: the record keeps the pre-K3 sampled bound (file-local copy); adopting K3's
  `deam_family_certify` bound kills every fixture ladder, and a phase-B holder from the fitter
  (`detail::build_serve_record_with`) carries the K3 bound, not the record's. K4 (still on its own branch) edits
  `deam_iv_cache*`, `quote_bench_main.cpp` and `quote_bench_policy.hpp`, which C2 and the record now also touch.

Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-W2ci-integrate-r6-report.md`. Evidence:
`C:\atx-cache\quote-benchmark\W2ci-20261023-integrate\`.

## W2c-j / r6 integration of K4, the native fit route (vol-sota master plan, 2026-10-23)

Lane W2c-j (run `vol-sota-r6-20261023`, pool-5) merged the approved K4 lane (native fit route; review APPROVE with
deviations, a development result, no promotion) into the root `feat/vol-resume5-20261022`, `--no-ff`, on top of
`d2c7eb5f`. The route stays opt-in (`deam_native_route` / `graded.native_route` default off). No default promoted;
nothing pushed.

| Commit | What | Conflicts | Outcome |
| --- | --- | --- | --- |
| `a969fd20` | merge K4 native fit route `efb442bb` (review APPROVE with deviations, 9 minors; merge base `92cea7df`) | `CHANGELOG.md`, `tests/CMakeLists.txt`, `tools/quote_bench_policy.hpp` | union, no semantic glue |

**Conflict resolution.** CHANGELOG = the root's entries, then K4's `### NEW` (entry-set check 138 = 137 + 1).
`tests/CMakeLists.txt`: the quote-slice test sources, its suite filter and the `atx-vol-tests` source list carry
both C2's `quote_slice_publication_test.cpp` / `QuoteSlicePublication.*` and K4's `fit_native_route_test.cpp` /
`FitNativeRoute.*`. `quote_bench_policy.hpp` `load()`: the graded key allow-list carries C2's `"publication"` and
K4's `"native_route", "reference_budget"`; `load()` is the one function both lanes edited, both edits additive (a
key each plus a parse block in different places), so union per the per-function rule.

**Region discipline (`quote_slice.cpp`, auto-merged).** A brace-depth parse of the merge-base blob maps every hunk
to its enclosing function: K4 touches `label_unidentified_mid` (new), `admit_point_observations`,
`SliceDeAmModel::implied_vol_premium`, `SliceDeAmModel::edge_bounds` (new), the `VolSpaceCache` constructor,
`certify_and_serve` (signature + two body hunks), `validate_channel`, `fit_quote_slice`, `fit_quote_slice_channel`;
C2 (the root side since `92cea7df`) touches `to_string(QuoteSliceRepairOutcome)`, `evaluate_quote_slice`,
`validate_quote_slice_repair`, `calendar_clamp`, `valid_recheck` / `nondividend_certified` /
`detail::nondividend_discount` (new), `repair_quote_slice_calendar`, `_in_channel`, `leaves_a_quote` (new),
`repair_quote_slice_market`, `JointWork` / `prepare_joint` / `joint_box`, `repair_quote_slice_calendar_joint`,
`query_quote_slice`. Functions touched by both: 0. Same check: `quote_slice.hpp` (K4 two config / diagnostics
structs, C2 others) and `quote_bench_main.cpp` (K4 `timing_json` / `charge_fit_lap`, C2 `fit_graded`) share no
function.

**Diff proof.** 64 per-file diffs EQUAL_SEQ (16 vs the K4 blob, 52 vs the W2c-i root blob); the 4 DIFFER are
`tests/CMakeLists.txt` and `quote_bench_policy.hpp` on both sides, exactly the union lines above.

**Post-merge suites, both sides** (vol-rel, `-Jobs 1`, `ATX_VOL_FIT_WORKERS=3`). K4: quote-slice 235/235 (W2c-i's 223
+ `FitNativeRoute` 12), `FitNativeRoute.*` 12/12 (`[K4 fit] requests 195 typed 195 lanes 195 search 0 reference 9`)
in both owning executables; C2: `QuoteSlicePublication.*` 28/28, quote-bench CLI 94, both CLI files 193; H0-A:
serve-record 28/28 with the seams on, `^serve-record:|^tick-path:` 82/82; anchored `^atx-vol-(quote-slice|bench-clocks)$` 2/2.

**Gates on the final code `a969fd20`** (the docs commit changes `.md` files only):
- 9/9 default hashes byte-identical (quote-bench `08de794c`), `sha256sum -c` 9 OK with the log persisted, stderr
  files 0 bytes.
- AL fingerprint `53c4df68` on vol-rel-avx2 (bin byte-equal to W2c-i's).
- Name set at cap 3: `52324ef8` exactly (the L575 flake did not fire); 1062 names, 0 added, 0 removed (K4's 12
  tests live in `atx-vol-quote-slice-tests` and `misc/FitNativeRoute.*`).
- Combined anchored ctest (W2c-i regex + `^misc/FitNativeRoute\.`), native ctest excluded: 186/186; CLI pytest
  193 (`test_quote_bench_cli.py` 94).
- ForceScalar filter: 121 passed, 4 skipped, 1 known failure; `atx-vol-umbrella`: its 3 known failures (46 / 105,
  unchanged: K4 adds no public header).
- PCH-off (`vol-hygiene`): the closure of the 6 changed headers, 85 TUs (28 library, 11 tools, 46 tests; the 2 `examples/` TUs are not configured there, bench / python TUs excluded), 9 batches exit 0, 0 diagnostics.
- Keys-ON (all MATCH): K4's speed arm SPY `75ca0fc5` / cohort5 `93918f62` / dev200 `9571c2e0` / dev200b `d6c05740`
  (workers 2), served set vs the default lost 0 / gained 0 on all four (max |dP| 7.8e-8 / 2.2e-5 / 6.3e-2 /
  9.3e-3 USD, = K4's); K3 audit on the merged SPY route output 29,411 typed roots, 0 violations, 0 unresolved;
  C2 dev200b `2e31e087` / `c4612fc3` / `228b1a94` / `c1691f1e` and SPY / cohort5 `ba413a85` / `4acd8575` /
  `d834b4d3` / `a90b5047` / `c3292d38` (workers 2); D1 `fc81aad3` / `c6016745`; K3 `710115bb` / `13f6be35`; K2
  `710115bb` / `6368997d` (replay block 13/13); C0 one_spot `a9dec618` / `ed34eef7` / `34e3ed27` / `d7a48be7` /
  `34d5aab0`. C2's validation does not touch the route's output (the arm hashes are K4's own).

**G3 (development row, merged root).** SPY native-route speed arm (policy `1a488f48`), B0's `run_cold_trials.ps1`, 10 fresh processes,
`ATX_VOL_FIT_WORKERS=1`, CPU 6 (P core), sibling idle 10/10 (max 1.5 %), host load 19 % before / 2 % after
(review-C3 leased beside), source `a969fd20` clean, binary `08de794c`, predictions `75ca0fc5` in all 10:
min / median / p95 / max **3.742 / 3.931 / 4.263 / 4.309 s**, beside K4's 4.154 / 4.540 / 5.265 / 5.395 and
review-K4's 4.419 / 4.667 / 5.043 / 5.160. Stage medians: inverses 2,343 ms, final grid 565, endpoints 345,
reference exceptions 328, family build 207, curve / repair 105 (unattributed 2.1 ms). No regression: every stage
is 6-15 % under K4's on a quieter host; a development row, not a qualification.
G3 MISS: SPY cold calibration median 4.54 s development row vs the 1 s bar; dominating stage inverses 2.77 s =
22,823 confirm solves; §7 challenger K5 opened (and K4b for K4's declared batching deviations).

### Deferred minors (W2c-j: K4 review, integration)
- K4 review m1 (-> K4b): the reference route types `PriceOnlyLowVega` with an uncertified bracket end, so `band()`
  can publish the range ends 0.005 / 40 as band ends (dev200: 1 request); return NaN for uncertified ends and test
  the real `reference_root` exit.
- m2 (-> K4b): `take()` accepts a certified end without checking it stays inside the bracket; crossed sides would
  call `std::clamp(root, lo, hi)` with `hi < lo` (unreachable under K3's `eps_c`; audit 0 violations).
- m3: the PM vega hook (`accept_family_native(..., lane_vega)`, fires 0 times on SPY) and the seed-routing bypass
  for family requests (2,048 -> 324 seed-routed) have no fail-on-revert test.
- m4 (-> K5): the inverse-stage split "confirms ~2 s, lane ~0.7 s" is an estimate; the review's arithmetic gives
  confirms ~1.1-1.4 s, family side ~1.4-1.6 s; K5 adds an inverse sub-clock (lane / enclosure / confirm / leg).
- m5 (-> K4b): the BAW seed leg stays for every request whose side has no family (cohort5 22.6 %, dev200 77.2 %,
  dev200b 94.0 % of leg roots; SPY 250 of 546 reference requests are leg failures).
- m6 (-> K4b): final-grid repricing is per node, not batched (658 ms vs 120); grid error metadata is aggregate.
- m7 (-> H1 / Q0): typed outcomes are not mapped onto C0's `price_only_reason` / `iv_lower` / `iv_upper`; the row
  gets `UnidentifiableVolatility`.
- m8: family refusals are counted, not typed by reason; no box extension for the 296 SPY `SigmaAboveBox` requests.
- m9 (doc): "evaluations 6,118 = AL-by-reason reference 6,118" is a stats identity, not a ledger reconciliation
  (the 670-solve gap exists at the root too).
- Q0: the K3 root audit is region-blind; add C1's exercise class at the served sigma as an audit column.
- K4 report: hash rows at workers >= 2 can differ at <= 5e-13 USD (P0 fan-out nondeterminism); `families.attempted`
  changed type (string -> int) in the bench JSON on every policy.
- W2c-j integration: none beyond the three unions.

Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-W2cj-integrate-r6-report.md`. Evidence:
`C:\atx-cache\quote-benchmark\W2cj-20261023-integrate\`.
