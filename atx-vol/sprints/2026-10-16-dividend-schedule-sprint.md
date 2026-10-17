# Dated dividend schedule inference and the lattice route for payers

Branch `feat/vol-dividend-schedule-20261016`, frozen base `951c0414`, rebased onto the
root head `9fc3934b` (tick-path merge) before the final gates. Pool-3 lease, run
`vol-20261016-div`. Implementation commit `607391ec` (pre-rebase `51c81153`; the
rebased binary reproduces every benchmark output byte for byte). Implementation
first, tests after (user request).

Motivation: dividend payers are the worst stratum for every model on both cohorts
(cohort-20261016 RESULTS); the parquet has no ex-date column but `ddivPv = sum D_i
exp(-rate_T days_i/365)` dates the first event to an integer day for 98.5% of payers;
93% of the >365d call-put antisymmetric squared error is ASML (implied-forward sprint);
research recommendation 5 (price dated cash exactly when an ex-date precedes expiry).

## What was built

**`api/marketdata/dividend_schedule.hpp` / `src/marketdata/dividend_schedule.cpp`**
(Tier-B, outside the umbrella). Input: one chain's expiries `{expiry_day, years, rate,
dividend_sum, dividend_pv}` plus the valuation day number. Output: a fixed-capacity
`DividendScheduleResult` (≤ 256 events of `{amount, days, ex_day, increment_expiry_day,
increment_events, kind}`), typed status `Ok / NoDividends / Ambiguous / Inconsistent /
Invalid` with a reason enum, resolved-through day, unit amount, cadence, and repricing
residuals. Allocation-free (counting-allocator test), deterministic, `noexcept`.

Four passes:

1. **Peel.** Sort/merge expiries (identical duplicates merge, conflicting ones are
   `Invalid`). Each sum increment above 5e-5 is dated as ONE event at its PV-implied
   time `t = -ln(rest/inc) * 365 / rate_e`, where `rest` is the expiry's PV net of the
   earlier increments at that expiry's own rate. The time must lie in the window
   `[previous expiry day + 1, expiry day]` (an ex-date on the previous expiry date
   belongs to it) within 1 day, else `Inconsistent(ExDateOutsideBracket)`; a decreasing
   sum, a non-positive residual PV, or |rate| < 1e-6 are `Inconsistent` too. An
   increment is exactly dated when |t - round(t)| <= 0.05 day.
2. **Count.** Unit = median amount of the exactly dated increments; cadence = median
   spacing of two exactly dated single-unit increments that are ADJACENT (only then are
   they consecutive dividends). n = round(inc / unit), capped by how many events fit in
   the window at half the cadence (7 days when unknown) and by 64. The cap keeps a
   special dividend from being read as a run of regular ones.
3. **Place — the declared split rule.** An increment with n >= 2 is split into n EQUAL
   amounts at EQUALLY SPACED times `t_j = t_1 + (j-1) h`, h = cadence (else window/n),
   with `t_1` solved in closed form so the n events reproduce the increment's PV at that
   expiry's rate exactly; when the events would leave the window, h is shrunk by a
   64-step bisection that keeps feasibility as its invariant. Times stay fractional
   (PV-exact). Every split event is flagged `SumSplit`; a single event at a non-integer
   day is `NonIntegerDay`; either makes the schedule `Ambiguous`. Increments are placed
   in ladder order against the FINAL earlier events, so the identity holds at every
   incrementing expiry.
4. **Verify.** Reprice every expiry's sum and PV; count residuals above 1e-5 relative;
   a residual above 1e-3 (and 1e-4 in cash) makes the ladder `Inconsistent` from that
   expiry on. `Inconsistent` keeps the consistent prefix (`covers(expiry_day)`).

Helpers: `dividend_schedule_expiry_events` maps the events in `(valuation, expiry]`
onto THAT expiry's carry clock, `tau = years * days / (expiry_day - valuation_day)`
(constant clock-speed assumption, the one quote_slice already makes between its
clocks; an ex-date on the expiry date lands at exactly T); `dividend_schedule_instants`
emits `DividendEvent` instants on the UTC day grid for the shared route.

**Wire-in (opt-in, default byte-identical).** Policy key per quote-bench profile:
`"dividends": "aggregate" | "inferred-schedule" | "inferred-schedule-exact"`
(tool-side table keyed by profile name; no library profile type changed). For a
snapshot whose resolved profile opts in, the ladder of its eligible chains (median
`ddiv` per chain, which the binary now reads; median `ddiv_pv`, `rate`,
`years_calendar`) is inferred once; valuation day = UTC day of `snapshot_ns`. Per chain:
`discrete_div_route(instants, expiry, valuation, T, r, DiscreteDivPolicy::Lattice)`
decides; if it applies (an inferred ex-date in the life) the chain is fitted with
`QuoteSliceDividendModel::DatedCash` (V&N lattice, 301 steps, `dividend_pv = 0`,
per-strike `sdiv` kept as the residual yield) for IV inversion AND repricing;
otherwise the historical aggregate fit runs unchanged. `-exact` additionally requires
every event in the life to be `Exact`. Two lattice outcomes fall back to the aggregate
fit, recorded per chain as `escrow_fallback` with the reason: an input refusal (`Err`)
and a refused price certification (`ArbitrageFailure`, see below). The native report
gains a `dividend_schedule` block only when a profile opts in: conventions, status /
reason / event-kind / route counts, lattice vs escrow chain latency, every schedule
(events with ISO ex-dates and kinds) and every chain route (events and ambiguous events
in life, route PV, outside-window count, aggregate vs used forward and the shift in bp,
fit status, chain microseconds). The prediction CSV schema is unchanged; routes join on
`chain_id`. Files touched: the new header/source/tests, `tools/quote_bench_{main.cpp,
policy.hpp,data.hpp}`, CMake lists, `tests/slices.cmake`, README Tier-B 36 → 37.
`src/fitting/quote_slice.cpp` and `quote_slice.hpp`: **untouched**.

## The certification trap (measured; why the fallback exists)

A strike-by-strike binomial price is piecewise LINEAR in strike between terminal nodes.
Pricing each strike at its own fitted vol moves the nodes per strike, leaving concave
kinks of ~1e-5 in the deep wing (dev200 ASML 2027-04-16, dense profile, put slopes
0.003422 / 0.003715 / 0.003711 over 10-wide strikes) that `AmericanPriceSlice`'s fixed
`1e-8 + 1e-9 max(S,K)` tolerance refuses (convexity or vertical-spread slope). It is
structural: the same chain is refused at 301, 601, 1201 and 2401 steps (the violation
only moves node). Andersen-Lake is smooth in strike, so the aggregate path never shows
it. Without the fallback (smoke run, not committed): 63 lattice chains ArbitrageFailure
on dev200, 18 of them previously served. The certification is mandatory and was not
relaxed; the next certified model serves those chains. Unlocking them needs a
strike-smooth dated-dividend engine (control-variate lattice, smoothed terminal step, or
PDE with jump conditions) — see the ASML diagnostic for what is at stake.

## Gates (rebased `607391ec`, preset vol-rel, -Jobs 2-3, focused targets only)

| gate | result |
|---|---|
| `atx-vol-dividend-schedule-tests` (new: 15 DividendSchedule + 1 DividendSchedulePricing + 2 DividendScheduleAllocation via `tests/support/alloc_counter`) | **18/18 pass** |
| `atx-vol-quote-slice-tests` (`QuoteSlice.*:AmericanPriceSlice.*:QuoteFitPolicy.*`) | **49/49 pass** |
| anchored ctest `^atx-vol-(dividend-schedule\|quote-slice)$` | **2/2 pass** |
| `atx-vol-umbrella-tests` | 9/10: `TierCountsMatchTheReadmeTable` passes at Tier-B 37; `NoFixturePathResolvedOutsideTheSharedResolver` fails on the same 5 pre-existing literals as the base (chain_export_test.cpp:1138, oracle_bench_test.cpp:711/1399, chain_export.hpp:1330, oracle_bench_main.cpp:86) |
| PCH-off `check` (vol-hygiene): dividend_schedule.cpp, quote_bench_main.cpp, quote_bench_common{,_policy,_seed}.cpp, both test TUs | 7/7 clean, `/W4 /WX` |
| `git diff --check`, 100-column manual check | clean |
| default parity (runner, rebased binary) | dev200 `c89d0739…` = stored control (and all 11 shared columns of all 72,349 lines of `20261016-auto-funding-v2.csv`); dev200b `4b273b08…` = stored `dev200b-qs-auto.csv` |

Pricing example test: a deep-ITM American call (S 100, K 80, 30 days) with 3.00 cash
moves monotonically up as the ex-date moves later in the life, sits only ~0.10 below the
dividend-free value (exercise just before the ex-date, >= S - K e^{-r tau}) where the
European loses ~3, and equals the dividend-free price EXACTLY when the ex-date is on or
after expiry; the put gains from cash inside the life only. The monolithic
`atx-vol-tests` and the `-L atx_vol_fast` label were not run (lane rule).

## Schedule inference statistics (label-free; 15:00 ET snapshot)

| | dev200 | dev200b |
|---|---|---|
| snapshot ladders | 191 | 197 |
| status ok / ambiguous / no_dividends / inconsistent / invalid | 62 / 37 / 92 / 0 / 0 | 72 / 38 / 87 / 0 / 0 |
| payer ladders, events | 99, 397 | 110, 391 |
| events exact / sum_split / non_integer_day | 216 / 180 / 1 | 234 / 153 / 4 |
| increments exact / split / non-integer | 216 / 59 / 1 | 234 / 48 / 4 |
| first ex-date exactly dated; second | 97/99; 68/76 | 108/110; 78/84 |
| PV repricing residual median / p95 / max (relative); > 1e-5 | 3.2e-8 / 6.9e-8 / 3.0e-6; 0 | 3.0e-8 / 5.5e-8 / 9.0e-7; 0 |
| chain routes lattice / escrow / escrow_fallback | 315 / 760 / 63 | 378 / 776 / 20 |
| contracts lattice / escrow / escrow_fallback | 16,164 / 35,936 / 11,126 | 19,045 / 39,658 / 2,964 |
| lattice chains with a non-exact event in life | 60 | 56 |
| lattice vs aggregate forward, median / p95 / max abs shift | 0.05 / 3.0 / 68 bp | 0.07 / 3.3 / 187 bp |

The large forward shifts are very-high-`sdiv` names (PDBC 57%, BABO 31%, AMDY): the
lattice accrues the residual yield on escrowed cash, `sum D e^{(r-q)(T-tau)}` (the
library's documented model and the vendor convention measured 2026-08-22), the funding
approximation does not. Those chains were not served (insufficient quotes).

## Benchmark (development split, 15:00 bucket; labels only through the scorer)

Runs through `tools/quote_benchmark.py run|score|compare`, binary
`atx-vol-quote-bench-51c81153.exe` (rebased `607391ec` byte-identical outputs),
`--dividends funding-pv`, policy `auto` (parity) vs `policy-divsched-v1.json` (auto
profiles + `"dividends": "inferred-schedule"`). Tables from
`scripts/analyze_divsched.py`, which loads both runs through the scorer's own
`_load_evaluation` (same masks) and reproduces the official compare's intersection
and MAE exactly. Spread units = |error| / max(ask - bid, 0.01). Anti = share of paired
squared error in (e_call - e_put)/2 at the same chain and strike.

**Coverage.** dev200 served 10,694 → 12,016, scored 10,654 → 11,931 (a-only 278,
b-only 1,555); calendar rejections 151 → 137. dev200b served 19,484 → 20,796, scored
19,440 → 20,723 (a-only 1,556, b-only 2,839); calendar rejections 187 → 165; payer rows
scored 6,810 → 6,510. Every coverage change is a cross-expiry American calendar-check
transition (dev200: 20 chains calendar → ok, 5 ok → calendar; dev200b: 35 → ok, 12 →
calendar) plus lattice chains leaving an aggregate-path arbitrage_failure (2 on dev200,
1 on dev200b); no chain changes for any other reason (`transitions-divsched-v1.json`).

**dev200, exact intersection (10,376 rows)**

| slice | n | MAE $ a → b | MAE/spread a → b | bias a → b | anti a → b |
|---|---|---|---|---|---|
| ALL | 10,376 | 0.1489 → **0.1428** | 0.0765 → 0.0746 | -0.0262 → -0.0247 | 0.336 → 0.337 |
| payers | 4,786 | 0.2243 → **0.2111** | 0.0624 → 0.0581 | -0.0124 → -0.0091 | 0.385 → 0.389 |
| non-payers | 5,590 | 0.0844 = | 0.0886 = | = | = |
| <=30d (no payers) | 2,537 | 0.0658 = | 0.0967 = | | |
| 30-90d all / payers | 1,387 / 364 | 0.1143 → 0.1101 / 0.1593 → 0.1432 | 0.1161 → 0.1141 / 0.1234 → 0.1158 | | |
| 90-365d all / payers | 3,708 / 2,400 | 0.1539 → 0.1481 / 0.1849 → 0.1760 | 0.0683 → 0.0664 / 0.0666 → 0.0637 | | 0.136 → 0.141 |
| >365d all / payers | 2,744 / 2,022 | 0.2366 → 0.2236 / 0.2827 → 0.2650 | 0.0490 → 0.0451 / 0.0463 → 0.0411 | | 0.488 → 0.483 |
| payer calls / puts | 2,393 each | 0.1977 → 0.1857 / 0.2509 → 0.2366 | 0.0555 → 0.0514 / 0.0692 → 0.0648 | -0.087 → -0.075 / +0.062 → +0.057 | |
| route lattice | 2,282 | 0.1394 → **0.1118** | 0.0661 → 0.0571 | -0.040 → -0.033 | 0.093 → **0.027** |
| ... all events exact | 1,630 | 0.1444 → 0.1276 | 0.0774 → 0.0713 | | 0.030 → 0.019 |
| ... with split / non-integer events | 652 | 0.1271 → 0.0722 | 0.0377 → 0.0216 | | 0.434 → 0.119 |
| route escrow_fallback (certification) | 2,504 | 0.3016 = | 0.0590 = | | 0.460 = |
| ASML (all ex-date chains fell back) | 1,288 | 0.4479 = | 0.0430 = | | 0.487 = |

**dev200b, exact intersection (17,884 rows)**

| slice | n | MAE $ a → b | MAE/spread a → b | bias a → b | anti a → b |
|---|---|---|---|---|---|
| ALL | 17,884 | 0.2383 → **0.2194** | 0.0867 → 0.0817 | -0.0475 → -0.0425 | 0.136 → 0.092 |
| payers | 5,338 | 0.2950 → **0.2318** | 0.0918 → 0.0750 | -0.0647 → -0.0479 | 0.279 → 0.211 |
| non-payers | 12,546 | 0.2141 = | 0.0846 = | = | = |
| <=30d (no payers) | 3,544 | 0.1590 = | 0.0990 = | | |
| 30-90d all / payers | 2,580 / 454 | 0.1717 → 0.1695 / 0.3674 → 0.3548 | 0.0910 → 0.0907 / 0.1073 → 0.1051 | | |
| 90-365d all / payers | 7,130 / 3,028 | 0.2035 → 0.1811 / 0.1957 → 0.1429 | 0.0903 → 0.0815 / 0.0913 → 0.0707 | | 0.132 → 0.068 |
| >365d all / payers | 4,630 / 1,856 | 0.3897 → 0.3525 / 0.4395 → 0.3468 | 0.0695 → 0.0638 / 0.0888 → 0.0747 | | 0.175 → 0.126 |
| payer calls / puts | 2,669 each | 0.3361 → 0.2633 / 0.2540 → 0.2003 | 0.1014 → 0.0816 / 0.0822 → 0.0684 | -0.198 → -0.111 / +0.068 → +0.016 | |
| route lattice | 4,570 | 0.2714 → **0.1976** | 0.0887 → 0.0691 | -0.056 → -0.037 | 0.187 → **0.044** |
| ... all events exact | 3,582 | 0.2262 → 0.1800 | 0.0845 → 0.0699 | | 0.176 → 0.081 |
| ... with split / non-integer events | 988 | 0.4356 → 0.2613 | 0.1038 → 0.0660 | | 0.196 → 0.007 |
| route escrow_fallback | 768 | 0.4355 = | 0.1104 = | | 0.582 = |

Official runner compare (paired, intersection): dev200 13.0% of rows improved, 9.0%
worsened (payers 28.2% / 19.5%), vol MAE 481.8 → 479.3 bp; dev200b 14.4% / 11.1%
(payers 48.4% / 37.2%), vol MAE 513.5 → 510.6 bp.

**Ablation `inferred-schedule-exact`** (lattice only where every event in the life is
exactly dated): dev200 payers 0.2250 → 0.2192 (spread 0.0623 → 0.0603) on 4,656 rows;
dev200b payers 0.2950 → 0.2641 (0.0918 → 0.0820) on 5,338. Roughly half the gain comes
from chains whose life holds split events, so the declared split rule earns its place.

**ASML, the >365d antisymmetric error.** On the parity run ASML holds 94% of the >365d
antisymmetric squared error (mean (e_c - e_p)/2 = -0.583, matching the implied-forward
sprint's -0.58). The official route does NOT move it: all 13 ASML chains with an ex-date
in life fail certification (staircase above) and fall back. DIAGNOSTIC, not servable
(`diag/`, scratch harness reproduces the bench's lattice prices bit for bit on CVX's 64
served lattice rows): replacing only ASML's routed contracts with the uncertified
lattice prices at the lattice fit's own IVs, scored through the scorer loader against
the parity run on the same 10,654 rows: ASML MAE 0.4479 → **0.2314** (spread units
0.0430 → 0.0292), ASML anti share 0.487 → **0.010**, ASML >365d mean anti -0.583 →
-0.030, all-rows >365d anti SS 140.4 → 10.0 (share 0.488 → 0.112), payers 0.2245 →
0.1694. The dated-dividend exercise structure is the ASML defect; the binding constraint
is now the lattice's strike smoothness under certification, not the model.

**Runtime (unqualified: shared host, vol-rel without LTO, diagnostic wall time).**
Lattice-routed chains cost 1.79 s (dev200, 315 chains) and 2.45 s (dev200b, 378) in
total: chain p50 2.2 / 2.8 ms, p95 25 / 24 ms, versus escrow p50 1.4 ms (escrow_fallback
chains pay a refused lattice fit plus the aggregate refit, booked as escrow). Whole
fit+reprice 15.7 s vs 14.5 s (dev200) and 11.3 s vs 17.7 s (dev200b, noise). Most routed
chains are refused early (insufficient quotes) before any lattice IV work.

## Verdict

The schedule inference is clean on both cohorts (no inconsistent ladder, worst PV
residual 3e-6) and the lattice route improves the dividend-payer stratum on every tenor
bucket where payers exist, in dollars and in spread units, on both cohorts: payers -5.9%
(dev200) and -21.4% (dev200b), lattice-priced rows -20% / -27%, with coverage up and
non-payer prices bit-identical. It stays opt-in: one trading date, development split
only, and the certification fallback hides the biggest single opportunity (ASML).

## Unfinished / follow-ups

- Strike-smooth dated-dividend pricing (control variate against the dividend-free
  lattice, Broadie-Detemple smoothed final step, or a jump-condition PDE) so chains like
  ASML certify; the diagnostic sizes the prize. Not attempted (pricing engine and
  `quote_slice.cpp` belong to other lanes).
- Default promotion needs the validation split / a second date; no holdout touched.
- Ex-date intraday timing is day-granular (ex at the start of its day, expiry at the
  start of its date on the UTC grid); a session-time convention could matter for
  ex-dates one or two days out.
- High-`sdiv` names: the lattice's q-accrual on cash and the funding approximation
  disagree by up to 187 bp of forward; which one the vendor's per-strike sdiv is
  calibrated against is unmeasured beyond the 2026-08-22 SPY evidence.
- Monolithic `atx-vol-tests`, `-L atx_vol_fast`, vol-dev assertion builds, CHANGELOG:
  not done (lane rule / no 2026-10-16 lane touched the CHANGELOG).

## Artifacts (SHA256; full list in `C:/atx-cache/quote-benchmark/dividend-20261016-runs/SHA256SUMS.txt`)

| artifact (under `C:/atx-cache/quote-benchmark/`) | sha256 |
|---|---|
| `dividend-20261016-bin/atx-vol-quote-bench-51c81153.exe` | ac64d8ceadf97ac464cb06702b43d4cd2afaea1f5a20ec80b083c6a60167f7cd |
| `dividend-20261016-bin/atx-vol-quote-bench-607391ec.exe` (rebased) | ec3f229bbe29e518b121f614e656c7eff039e0ba090a545764bf01ccfd23ccc5 |
| `dividend-20261016-bin/quote_benchmark_51c81153.py` (runner, same bytes at 607391ec) | a51e2031afa48ffeb6fba9b2580b9245b8adb9158467343934dd64093c67a9f4 |
| `dividend-20261016-runs/policy-divsched-v1.json` | 46ee9efc1aa4968683c66766109dcf2baa6209fa2744f2ff0efb107782a3e92d |
| `dividend-20261016-runs/policy-divsched-exact-v1.json` | 57756b609140f6d1c24e92b4f71954e3bf2b166d002d60fef07f473c9c0cd91d |
| `dividend-20261016-runs/dev200/20261016-auto-funding-divparity-v1.csv` | c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c |
| `dividend-20261016-runs/dev200/20261016-auto-funding-divparity-v1.csv.run.json` | a9d33a3741f1c06afd102d71678df4cd1dce86da5a72d5c32301c75f62dc8576 |
| `dividend-20261016-runs/dev200/20261016-auto-divsched-v1.csv` | abc75955cab8bef821c032ee2a45aa08979b0e92283daad66d3ca2d62bc3bcee |
| `dividend-20261016-runs/dev200/20261016-auto-divsched-v1.csv.run.json` | fa0d5efe846793d12200aba09ec2a94146c98fcb820ebb19c019ff35d804c4a3 |
| `dividend-20261016-runs/dev200/20261016-auto-divsched-v1-native.json` | b7ae36ae60dbe6a996e78292c0249531a5b839a6081e76d03d43a0c321f660a8 |
| `dividend-20261016-runs/dev200/20261016-auto-divsched-v1-score.json` | aa764652b86873c1189b1973c3e342ccf57251cdd901930b4d637f961ec01b0c |
| `dividend-20261016-runs/dev200/20261016-divparity-vs-divsched-v1-compare.json` | 918e98254d1150f27d4d15bde90cef02332af908f890675e3ce6deabb6c9f982 |
| `dividend-20261016-runs/dev200/analysis-divparity-vs-divsched-v1.json` | 765a4beeb323b17e5ad29d541bc75e6a1c153ca67dcdf7659cc33f522c0f7787 |
| `dividend-20261016-runs/dev200b/20261016b-auto-funding-divparity-v1.csv` | 4b273b084231f8a3811c7d6505d42500355ae29c864a5c2e1c2b964a7aab70fb |
| `dividend-20261016-runs/dev200b/20261016b-auto-funding-divparity-v1.csv.run.json` | dc06fd9d5276d4b4fd8ed22e4fe7ddf66b8f74f3ab5afc82d6ec620743c70963 |
| `dividend-20261016-runs/dev200b/20261016b-auto-divsched-v1.csv` | 747d3e6cfcb1122629e8add039c2b1fbe9cba784c0032ccf840030fb9daab5d3 |
| `dividend-20261016-runs/dev200b/20261016b-auto-divsched-v1.csv.run.json` | 16bd52ca4e2d9588d58ed58f30ca86a64f7cd06fb82646bba7f7ac517d2e454d |
| `dividend-20261016-runs/dev200b/20261016b-auto-divsched-v1-native.json` | 59eb35bebb99e2c4a66f7d1ab546cdc7423c8f42714402be4b4869b1d1aa93f2 |
| `dividend-20261016-runs/dev200b/20261016b-auto-divsched-v1-score.json` | 66d90288bdf668d9de0a95aec3e837cc743103c5931d3e2e51cd93559a85c961 |
| `dividend-20261016-runs/dev200b/20261016b-divparity-vs-divsched-v1-compare.json` | 1431e672f757dccfa7df295ddf47c951203ea1d426dd640cdedbe31140ac3cd8 |
| `dividend-20261016-runs/dev200b/analysis-divparity-vs-divsched-v1.json` | 4c8bb1eb6a626e1a225438bbb3136c7b2415fc6902e908978128b761e8cf7125 |
| `dividend-20261016-runs/dev200/20261016-divparity-vs-divsched-exact-v1-compare.json` | b0387f2585b7f04334f5ce6ca85d06c704b0b1cb68ac911eb4b76de34bf0d2a0 |
| `dividend-20261016-runs/dev200b/20261016b-divparity-vs-divsched-exact-v1-compare.json` | 91468e9ebca95b948c3bc8bd29e63af12797df87f5bc8a18a8ce5d443f193ea7 |
| `dividend-20261016-runs/diag/20261016-asml-uncertified-lattice-diag.csv` (DIAGNOSTIC) | 18cec250dec60048a6682ef903b75eb5b11c863533c3bba34b616365210e81fb |
| `dividend-20261016-runs/diag/20261016-asml-uncertified-lattice-diag.csv.run.json` | a5be974d650a4ee03d9d239f5b90932d702342af55810e8c627f53d8b15cc018 |
| `dividend-20261016-runs/diag/analysis-asml-uncertified-diag.json` | 5933a8b3f34951bf969fd3195f29f3ec594b3ebd31cab874fbcc4c8a3861fac9 |
| `dividend-20261016-gates/dividend-schedule-tests-607391ec.xml` | ac684513707cbe80939a57d57ee09302273613baadc73a5f769de3e4c74f1256 |
| `dividend-20261016-gates/quote-slice-tests-607391ec.xml` | 2db8b14c0e38f433b72d772cb828ec149d62bd8a7af8af65659ec79ede789f0c |
| `dividend-20261016-gates/umbrella-tests-607391ec.xml` | 434a4ece53f5e2e73f6656249343cdab2dc9318b327fd7bef44e7e7effe4cc55 |
