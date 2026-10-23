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
