# Implied forward / carry (`sdiv`) estimator and quote-slice carry mode

Branch `feat/vol-implied-forward-20261016`, frozen base
`5e978fed9fe5db28c83a1fcf1d47fff0d7f829e1`, pool-35 lease run
`vol-20261016-fwd`. Implementation first, tests after (user request). Motivation:
hypothesis 7 of `C:/atx-cache/quote-benchmark/diagnosis-20261016/2026-10-16-common-model-error-diagnosis.md`
(quote-slice antisymmetric call-minus-put error 32.9% of squared paired error, 48.8% at
>365d; "forward too low").

## What was built

**`api/marketdata/implied_forward.hpp` / `src/marketdata/implied_forward.cpp`** — a
reusable per-expiry estimator. Input: one expiry's call/put quote pairs (strictly
increasing strikes), spot, clock, rate (on the pricer clock; DF = exp(-rate*years)) and an
optional reference yield. Output: forward F, total continuous carry
`implied_yield = rate - ln(F/S)/years` (the `sdiv` analogue), `prepaid_deficit = S - F*DF`
(PV of all dividends + borrow), `forward_offset = F - S*exp((rate-ref)*years)` (the
`uOff` analogue), the exercise-corrected bid/ask channel `[forward_lower, forward_upper]`,
ATM total vol, dispersion / leave-one-out / exercise-shift in ATM standard deviations of
moneyness, pair counts and up to 32 per-pair diagnostics. Typed status: `Ok`,
`InsufficientPairs`, `Dispersion`, `Contaminated`, `Invalid`; no exceptions for control
flow; deterministic (bitwise test); fixed-capacity buffers.

Method:

1. Two-sided pairs only (a missing bid is an upper bound, never a point quote).
   Provisional forward: parity forward `K + (C_mid - P_mid)/DF` at the pair with the
   smallest |C - P|, refined by the width-weighted median of the `min_pairs` nearest.
2. Strip: the `max_pairs` (8) pairs nearest the provisional forward in log-moneyness,
   kept within `band_sd` (0.75) ATM standard deviations (ATM unit from the straddle time
   value), never fewer than `min_pairs` (3). Weight = 1/(channel half-width)^2 x a
   proximity kernel, winsorized at 4x the median.
3. Robust location: weighted median -> weighted Huber IRLS (k = 1.345) with the scale
   floored at half the median channel half-width; outliers (beyond 3 scales AND outside
   their own channel) are dropped and the retained pairs re-located.
4. American exercise, honestly (the approximation): mid parity is biased by the
   difference of the two early-exercise premia (put EEP with r > 0 biases F low; call EEP
   with q > r biases it high). `AmericanAlignment` (default) runs a per-pair Newton fixed
   point at which the call and the put have the SAME American implied volatility under
   the library's continuous-carry Andersen-Lake model (q_eff bridge): invert the OTM leg at
   the q that reproduces x, reprice the ITM leg, step `x += s*(ITM_Am - ITM_mid)/DF`
   (Newton in the European limit; steps capped at 0.25 sd). This is parity on
   de-Americanized prices and is a MODEL-CONSISTENT carry (it absorbs any exercise
   structure a continuous-yield model cannot represent, as a vendor `sdiv` calibration
   does). `Parity` measures, and never applies, the EEP bias with one Newton step per pair
   and refuses (`Contaminated`) when it exceeds `max_exercise_shift_sd` (0.01 sd).
   Legs at their exercise boundary (mid <= intrinsic + 1e-4 + 1% intrinsic) are excluded
   (`ExerciseBound`); IV or repricing failures are `Unidentified`; non-convergence or a
   per-pair correction above 0.25 sd is `Contaminated`.
5. Refusals: fewer than `min_pairs` retained; more than 34% of evaluated pairs rejected
   as outliers (bimodal strip); dispersion > 0.05 sd; leave-one-out shift > 0.02 sd.

Allocation: the Parity stage allocates nothing; the American stage reuses the pricer's
thread-local scratch and allocated nothing in a warmed steady-state call (counting
allocator test). Error paths of the pricer may allocate (error text).

**Quote-slice wire-in (opt-in, default byte-identical).** `api/fitting/quote_slice_carry.hpp`
defines `QuoteSliceCarryMode {Supplied, ImpliedForward}`, `QuoteSliceCarryConfig` and the
per-expiry `QuoteSliceCarryResolution`. `src/fitting/quote_slice_carry.{hpp,cpp}` (internal)
pairs the slice's own quotes, estimates F on the pricer clock (variance years, funding rate
rescaled to preserve DF), restates F as the market's calendar continuous yield UNDER the
configured dividend model while keeping the supplied aggregate PV
(continuous: `r - ln(F/S)/T`; escrowed: `r - ln(F/(S-PV))/T`; funding:
`-ln((F e^{-rT} + PV)/S)/T`), drops per-contract yield overrides (one carry per expiry) and
runs the unchanged Supplied fit. Refusal, the DatedCash model (lattice forward), a
malformed market, or a slice refusal of the implied carry fall back to the Supplied fit,
bit-identically. Tool: policy keys `carry_mode` (`supplied` | `implied-forward` |
`implied-forward-parity`) and `implied_forward {...}` per profile; profile provenance emits
the carry fields only when non-default; the native report adds `carry_status_counts` and
`carry_resolutions` (per expiry: F, supplied F, implied calendar/total yields, parity F,
channel, dispersion/exercise shift, pair counts, status, reason) only when a profile opts in.

Call-site hooks in files owned by the concurrent admission lane (merge list):

- `include/atx/vol/api/fitting/quote_slice.hpp`: +1 include (`quote_slice_carry.hpp`),
  `QuoteSliceConfig::carry` (+3 lines), `QuoteSliceFit::carry_resolution` (+3 lines).
- `src/fitting/quote_slice.cpp`: +1 include (`fitting/quote_slice_carry.hpp`), +3 lines at
  the top of `fit_quote_slice` (dispatch when `config.carry.mode != Supplied`).
- `quote_fit_policy.cpp`: untouched. Admission, curve families, caps, defaults: untouched.

Umbrella `vol.hpp` untouched; the README tier table (the umbrella suite's pin) moves
Tier-B 32 -> 34 for the two new `api/` headers.

## Gates (implementation commit `a657f047`)

All builds via `C:\atx-wt\pool-35\scripts\atx-build.ps1`, `-Jobs 2`, focused targets only
(a monolithic `atx-vol-tests` build was started, then stopped on the coordinator's rule
before linking; it was never used as a gate). XML receipts in
`C:/atx-cache/quote-benchmark/implied-forward-20261016-gates/`.

| gate | preset | result |
|---|---|---|
| `atx-vol-implied-forward-tests` (new focused exe: 16 ImpliedForward + 6 QuoteSliceCarry + 2 ImpliedForwardAllocation) | vol-rel-avx2 | **24/24 pass**, 0 skips |
| `atx-vol-quote-slice-tests` (`QuoteSlice.*:AmericanPriceSlice.*:QuoteFitPolicy.*`) | vol-rel-avx2 | **49/49 pass** |
| anchored ctest `^atx-vol-(quote-slice\|implied-forward)$` | vol-rel-avx2 | **2/2 pass** |
| `atx-vol-umbrella-tests` | vol-rel-avx2 | 9/10; `NoFixturePathResolvedOutsideTheSharedResolver` fails **identically on the untouched base tree** (same 5 literals: chain_export_test.cpp:1138, oracle_bench_test.cpp:711/1399, chain_export.hpp:1330, oracle_bench_main.cpp:86; `umbrella-tests-base-tree.xml`). `TierCountsMatchTheReadmeTable` passes after the Tier-B pin edit |
| PCH-off `check` (vol-hygiene): implied_forward.cpp, quote_slice_carry.cpp, quote_slice.cpp, quote_fit_policy.cpp, quote_bench_main.cpp, the three test TUs (both owning exes) | vol-hygiene | clean, `/W4 /WX` |
| `git diff --check`, 100-column manual check | - | clean |

Test highlights: European parity recovers F to 1e-12 relative; AmericanAlignment recovers
the forward of an Andersen-Lake chain (T=2y, r=5%, q=1%, skewed smile) to 2e-6 relative
(accurate preset) / 5e-5 (default fast preset), including a q > r chain whose CALL carries
the premium; Parity refuses the 2y chain as `Contaminated` and accepts a 1-week chain;
sparse, dispersed, bimodal, exercise-bound and malformed inputs return the typed refusals;
a corrupted pair is rejected as an outlier; bitwise determinism; zero allocations in a
steady-state call of both stages; quote-slice refusal / DatedCash / malformed-market
fallbacks are bit-identical to the Supplied fit; a 150 bp wrong supplied carry is replaced
(yield within 5e-6, mid-repricing error below half the supplied fit's); the funding-PV
mapping keeps the cash PV and reproduces F.

## Benchmark (dev200, 1030 bucket, development cohort only)

Binaries: base `atx-vol-quote-bench-base-5e978fed.exe` (f23aa76f...), implementation
`atx-vol-quote-bench-a657f047.exe` (2b0a23b2...), in
`C:/atx-cache/quote-benchmark/implied-forward-20261016-bin/`. Official runs through
`tools/quote_benchmark.py run|score|compare`; tenor / antisymmetric tables from
`implied-forward-20261016-runs/scripts/compare_runs.py` (same scored-row mask as the
official scorer; reproduces 10,654 rows / MAE 0.150988 for the baseline).

**Parity (default carry).** `20261016-auto-funding-fwdparity-v1.csv` is byte-identical to
the base-commit binary's output (`c89d0739...`) and identical on all 11 shared columns of
all 72,349 lines to the stored `20261016-auto-funding-v2.csv`; that file's hash differs
only because the base schema has since added the `vol_status`/`model` columns. Score:
10,654 rows, MAE 0.1509883, bias -0.0289790 (equal to the stored score). Native report:
only timing fields and the `model` key differ.

**`carry_mode: implied-forward`** (`20261016-auto-impliedfwd-v1`, policy
`policy-impliedfwd-v1.json` = auto profiles + carry mode). Carry: 585 expiries applied,
397 insufficient_pairs, 89 contaminated, 67 dispersion fallbacks (applied/total by tenor:
<=30d 71/263, 30-90d 163/338, 90-365d 249/413, >365d 102/124). Served 10,694 -> 10,762;
fitted chains 132 -> 129; calendar rejections 151 -> 197. Own scored rows: MAE 0.1510 ->
**0.2733** (10,654 -> 10,724 rows).

Exact intersection (7,852 rows; a-only 2,802, b-only 2,872), supplied vs implied:

| tenor | rows | MAE supplied | MAE implied | bias supplied | bias implied | anti share supplied | anti share implied |
|---|---|---|---|---|---|---|---|
| ALL | 7,852 | 0.1761 | **0.2588** | -0.0211 | +0.0574 | 0.346 | 0.388 |
| <=30d | 1,302 | 0.0460 | 0.0611 | -0.0235 | -0.0107 | 0.005 | 0.250 |
| 30-90d | 776 | 0.1208 | 0.1702 | -0.0459 | -0.0177 | 0.032 | 0.315 |
| 90-365d | 3,348 | 0.1780 | 0.2686 | -0.0700 | -0.0036 | 0.136 | 0.395 |
| >365d | 2,426 | 0.2608 | 0.3798 | +0.0558 | +0.2022 | 0.492 | 0.390 |
| calls | 3,911 | 0.1610 | 0.3169 | -0.0675 | +0.2072 | | |
| puts | 3,941 | 0.1910 | 0.2012 | +0.0250 | -0.0912 | | |

Official compare: paired mean |err| reduction -0.0828; 36.6% of rows improved, 52.1%
worsened. Exploratory variants (same binary, receipts `final-*`): Parity method 0.1709 ->
0.3127 on 7,858 rows (90-365d 0.165 -> 0.484); tight strip (2..4 pairs, band 0.3 sd)
0.1738 -> 0.2620. Runtime: fit+reprice 13.3 s -> 27.6 s, chain p50 1.9 -> 10.0 ms
(diagnostic, not a quiet-host measurement).

**Why it loses, measured (diagnostics; labels used for evaluation only).**

1. *The estimator is right about SpiderRock's own forward.* Applying the same
   AmericanAlignment to the srPrc labels (bid = ask = srPrc, a diagnostic input never
   scored as a model; `diag-label-implied-forwards-native.json`) recovers the supplied
   (vendor `sdiv` + `ddiv_pv`) forward to median 0.03 / -0.06 / -0.33 / -0.19 bp on
   no-dividend chains (<=30d / 30-90d / 90-365d / >365d), +9.6 bp for dividend LEAPs.
2. *The supplied carry IS the label's carry*: median |supplied - label-implied| forward
   = 0.07 / 0.2 / 1.6 / 5.1 bp by tenor, versus 8 / 25 / 38 / 39 bp for the quote-only
   implied forward (similar for every variant). Market mids at the snapshot imply a
   different forward from SpiderRock's EMA-smoothed, channel-fitted calibration (ASML 1-6
   week expiries: +12-15 bp already in raw mid parity), so any quote-only forward moves
   the slice AWAY from the label. Vendor `sdiv` is a fitted output; using it is
   label-adjacent, which is why it scores well.
3. *Hypothesis 7's premise is refuted at the forward level.* Forcing each expiry onto the
   LABEL-implied forward (oracle shift of the supplied sdiv;
   `diag-label-oracle-shift-compare.json`) moves the 10,428-row intersection only
   0.1497 -> 0.1438, >365d 0.2365 -> 0.2182, anti share 0.34 -> 0.19. 93% of the >365d
   antisymmetric squared error is ASML (spot $1,712, mean anti -0.58, about 7.5 bp of
   forward); its label-implied LEAP forward is +7.5 to +12.8 bp above supplied (continuous
   vs dated dividend exercise structure), and the quote-only forward gets within -5..+8 bp
   there but is 12-15 bp off on ASML's short expiries. A perfect per-expiry forward is
   worth about 0.006 of MAE, not the 0.016 upper bound the diagnosis estimated by removing
   the whole antisymmetric component; the remainder is strike-structured (exercise and
   dividend modelling), not a forward level.
4. Dropping the vendor's strike-varying `sdiv` alone is neutral (flattened control:
   0.1702 -> 0.1704 on 9,013 rows).

**Verdict.** The estimator and the opt-in mode work as specified (synthetic recovery,
label self-consistency, bit-identical default), but on the SpiderRock-label benchmark the
honest quote-only carry is worse than the supplied vendor carry at every tenor and should
stay opt-in/off. Coverage +70 scored rows, at the cost of +46 calendar rejections. Nothing
here qualifies a default (one date, one bucket, development cohort).

## Unfinished / follow-ups

- Strike-varying carry (vendor `cpAdj` analogue: a 9-point sdiv(z) curve from per-pair
  alignment roots) is not implemented; finding 3 says the residual antisymmetric error is
  strike-structured, so that, or dated-dividend exercise, is the next lever, not the level.
- Cross-expiry smoothing of the implied carry (the vendor's ~30 s EMA, term-structure
  regularization) would cut the +46 calendar rejections; not attempted.
- The monolithic `atx-vol-tests` and full fast-label qualification were not run (lane
  rule); the two new test files are registered there for release qualification.

## Artifact SHA256

| artifact (under `C:/atx-cache/quote-benchmark/`) | sha256 |
|---|---|
| `implied-forward-20261016-bin/atx-vol-quote-bench-a657f047.exe` | 2b0a23b2489749352c8edb8b8f57903a93e429cbaa4bbab8da23f5c7869ef511 |
| `implied-forward-20261016-bin/atx-vol-quote-bench-base-5e978fed.exe` | f23aa76f709e351d3d54f19074f80f2d1881ebdb767778c272303ab912c12235 |
| `20261014-dev200/20261016-auto-funding-fwdparity-v1.csv` | c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c |
| `20261014-dev200/20261016-auto-funding-fwdparity-v1.csv.run.json` | 156df180c0b52d004908fe5c91e159fd7244d33f023dcca53aef5e340ab7cb0e |
| `20261014-dev200/20261016-auto-funding-fwdparity-v1-native.json` | cdeb0681faa0bdcc422a28aeb4913579211cc882bedcf0a59ae72adf0bd16f7d |
| `20261014-dev200/20261016-auto-funding-fwdparity-v1-score.json` | 823c7280065bfdc69415c5686983a57c2d292af8735543608dfdc2fc85f36aca |
| `20261014-dev200/20261016-auto-impliedfwd-v1.csv` | 43a1d17471f7c9839d7eb2019f7abe0c510347ede27b280af019209945042f76 |
| `20261014-dev200/20261016-auto-impliedfwd-v1.csv.run.json` | 961344f0f1faacf51d8a8306dda2a7893939e4303e56347d66e468de442ea943 |
| `20261014-dev200/20261016-auto-impliedfwd-v1-native.json` | e1ca2f7eba9479d44249557fc0063549e5bfc6df246cef17bcefbd37fb0cf3a9 |
| `20261014-dev200/20261016-auto-impliedfwd-v1-score.json` | 03f2d5d72d8f2f711d0a9b0f73514c346c26f214cc561af955132cb65396d05f |
| `20261014-dev200/20261016-auto-funding-vs-impliedfwd-v1-compare.json` | 1c9ce4eb6fc9a98884bc948838619d714980066a5495c5d879b6e06e11ff48a5 |
| `implied-forward-20261016-runs/policy-impliedfwd-v1.json` | 21ea4de7a9b4b80b1789f8bbcd8631bc72d76a3e1a4c5b00cf1850425e862cce |
| `implied-forward-20261016-runs/official-auto-funding-v2-vs-impliedfwd-v1-tenor.json` | ff4e52ef83f4332793c1fa4a4ae6c50b07bd29863b18b7e8c7d1eb6c68e9395f |
| `implied-forward-20261016-runs/diag-label-implied-forwards-native.json` | d4dc0d8d03da153dd593d2a39877b4d4d5b6402f555e56b4a8b72293f1205b4e |
| `implied-forward-20261016-runs/diag-label-oracle-shift-compare.json` | 7725f67ad54e3a91c74ac6f389cf19b8c9acfddc5591d149a106350fa3ab4e09 |
| `implied-forward-20261016-runs/diag-flat-sdiv-compare.json` | aaa9fb9b76019898b8b93b8feebdf6bb25a9b515440d30869f1b6db0c353b972 |
| `implied-forward-20261016-gates/implied-forward-tests.xml` | ee5f60a0309df3f2dbbaf1a681005c72ad4c24b3d1683fb8dff84ebfbcb1abe3 |
| `implied-forward-20261016-gates/quote-slice-tests.xml` | 46aa7230ddb0336a78c9760aaccad4d58a4f64ee874c0396582ac72616fedc15 |
| `implied-forward-20261016-gates/umbrella-tests.xml` | 984b03e4f8d45f9230016fa6f56adb6fe98eb337c4422a98a1007b4462f5929c |
| `implied-forward-20261016-gates/umbrella-tests-base-tree.xml` | 39260493801ee28067e8f63e773fdb656fa6ba5beabc1c10209a9337e8fbf6c3 |
