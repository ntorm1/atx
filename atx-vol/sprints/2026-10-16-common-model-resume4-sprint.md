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

Pending: knot-first ladder (inherits the AAPL Nov 20 / SPY 23 d calendar slot holders from slice-repair).

## Resume 5 wave 1 complete

Wave 1 merged four lanes into `feat/vol-resume4-20261016` (base `7ca31c95`): review-cleanup `3465221c` at `ef5bc6c3` (1a), serve-path `03a5463b` at `3fa27b00` (1b), smooth-div `4e263541` at `53f728c7` (1c) and slice-repair `5e30330c` at `37addcd0` (1d). Parity ran once, on the final root binary `37addcd0` (quote-bench sha256 `4829965a...`, `ATX_VOL_FIT_WORKERS=3`, outputs in `C:/atx-cache/quote-benchmark/integrate-1d-20261017-parity/`), with every opt-in key off: dev200 default quote-slice `c89d0739`, graded-v1 `1348f63d`, R104 common `6b129f64`, dividend default `abc75955` and dev200b default `4b273b08` all MATCH, as do dev200b divsched `747d3e6c`, dev200b graded-v1 `4c70d2bc` and cohort5 graded-v1 `7fa3c415`. The lanes' opt-in outputs also reproduce on the merged binary: escrowed-lr `2cfda793` (dev200) / `5a9c1f5f` (dev200b) and repair-dominance-v3 `97fad2f0` (cohort5) / `14d9dc6d` (dev200). No default moved, no holdout touched, nothing pushed.
