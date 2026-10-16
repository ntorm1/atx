# Quote-slice coverage sprint (2026-10-16)

Lane `slice-coverage-lane`, branch `feat/vol-slice-coverage-20261016`, frozen base
`5e978fed9fe5db28c83a1fcf1d47fff0d7f829e1`, rebased onto the resume-4 root
`da3a4179` (implied-forward + time-knot + build-infra lanes), pool lease `pool-36`
(run id `vol-20261016-slice`). Goal: serve far more of the dev200 eligible population with
the per-expiry quote-slice model without losing its accuracy, moving toward the
SpiderRock label construction (smooth per-expiry curve inside the bid/ask channel,
ATM-scaled, borrowing from neighbouring expiries when quotes are sparse).

Inputs read: `diagnosis-20261016/2026-10-16-common-model-error-diagnosis.md`,
`research-20261016/2026-10-16-sota-american-options-research.md`, the quote-slice /
policy / certification sources and the ledger. srPrc/srVol are never model inputs;
labels are read only by the scoring scripts after predictions exist.

## 1. Census: why 53,248 eligible rows are not served (baseline)

Baseline: `20261014-dev200/20261016-auto-funding-v2.csv` (automatic policy,
funding-PV, 63,942 eligible rows, 10,694 served). Label-free census script:
`C:/atx-cache/quote-benchmark/slice-coverage-20261016-scripts/census.py`
(+ `census2.py`); tables in `slice-coverage-20261016-census/`.

| refusal (row status) | rows | snapshots | expiries | mechanism |
|---|---|---|---|---|
| insufficient_quotes | 31,224 | 186 | 811 | fewer than `min_quotes` distinct admitted point strikes (dense 12 / ordinary 6 / sparse 4) |
| calendar_arbitrage | 12,484 | 24 | 151 | a certified slice crosses another expiry of the same snapshot; BOTH slices of every crossing pair are refused |
| arbitrage_failure | 8,824 | 8 | 44 | final American price slice fails certification: 42 call/put convexity above the global lower envelope, 2 vertical-spread slope |
| unsupported_deliverable | 716 | 12 | 31 | OCC numeric-suffix adjusted roots (legitimate refusal; deliverable data absent) |
| **total unserved** | **53,248** | | | |

By resolved profile (rows / snapshots / expiries):

| profile | ok | insufficient | calendar | arbitrage |
|---|---|---|---|---|
| dense | 3,336 / 4 / 16 | 3,146 / 3 / 12 | 3,060 / 4 / 14 | 8,192 / 4 / 38 |
| ordinary | 7,050 / 31 / 105 | 18,212 / 87 / 377 | 8,806 / 18 / 119 | 632 / 4 / 6 |
| sparse | 308 / 9 / 11 | 9,866 / 96 / 422 | 618 / 2 / 18 | - |

Insufficient expiries by distinct admitted point strikes (rows / expiries):

| profile | 0 | 1-2 | 3 | 4-5 | 6-11 | median two-sided strikes |
|---|---|---|---|---|---|---|
| dense (min 12) | 648 / 2 | 796 / 2 | 410 / 1 | 652 / 2 | 640 / 5 | 64-205 |
| ordinary (min 6) | 11,662 / 249 | 3,588 / 72 | 1,470 / 25 | 1,492 / 31 | - | 20-24 |
| sparse (min 4) | 8,012 / 348 | 1,456 / 61 | 398 / 13 | - | - | 9-13 |

Quote status of rows inside insufficient expiries: excluded_side (ITM leg under the
OTM side policy) 15,346; bidless 10,028; wide_spread_to_mid 3,350; wide_spread_vol
2,000; admitted 496. Inspection of zero-admission chains (EXPE 3d, JPSE 155d, GPTY
3d) shows the pattern: every OTM leg is bidless or >80% wide, while the ITM legs are
two-sided but excluded by the side policy. The information exists, but only as
bid/ask *intervals*, not as reliable mid targets.

Neighbour availability (a pass-1 point fit in the same snapshot, `t9`):

| status | has point-fitted neighbour | rows | expiries | snapshots |
|---|---|---|---|---|
| insufficient_quotes | no | 21,342 | 659 | 144 |
| insufficient_quotes | yes | 9,882 | 152 | 42 |
| calendar_arbitrage | yes | 12,484 | 151 | 24 |
| arbitrage_failure | yes | 8,824 | 44 | 8 |

So two thirds of the insufficient rows have no point-fitted neighbour: a term prior
built only from grade-A slices cannot reach them; the snapshot's own channel fits
must supply the cross-expiry shape (two-stage prior below).

## 2. Design (written before coding)

All new behaviour is opt-in. With no new policy keys, `fit_quote_slice` and the
bench produce byte-identical predictions (verified by a parity run).

### 2.1 `fitting/slice_channel.*` - channel-constrained knot spline (pure math)

Input per contract: log-moneyness `k`, a volatility channel `[sigma_lo, sigma_hi]`
obtained by inverting the contract's bid and ask with the SAME American model and
carry the point path uses for the mid (bidless: `sigma_lo = 0`; ask above the
maximum-volatility price: `sigma_hi = +inf`), and an optional mid IV. Model:
`sigma(z) = sum_j B_j(z) v_j`, natural cubic cardinal basis on fixed knots of the
SpiderRock 29-point LogStd grid, `z = k / (sigma_ref sqrt(T))`, flat beyond the
outer active knot (bounded wings). Objective (convex, piecewise quadratic):

```
  mid_weight     * sum_i a_i ((sigma(z_i) - sigma_mid_i) / h_i)^2      spread-normalised mid anchor
+ channel_weight * sum_i ((sigma_lo_i - sigma(z_i))_+^2 + (sigma(z_i) - sigma_hi_i)_+^2) / h_i^2
+ prior_weight_eff * sum_j ((v_j - v_prior_j) / (prior_scale * v_prior_j))^2   term/default prior
+ roughness      * sum_j (second difference of v / sigma_ref)^2
```

`h_i` = channel half-width in vol, floored. Solved by a bounded semismooth
active-set Newton iteration with backtracking (normal equations <= 29x29, fixed
max-size Eigen storage, no heap inside the iteration). Output: `SplineVolParams`
(served by the existing `SplineVolCurve`, `mult_cap` bounded) plus diagnostics
(iterations, channel violations, max violation in half-widths). This follows the
vendor objective (fit inside the channel, report error against the mid) and the
SANOS / research recommendation (steep outside-quote penalty + small
spread-normalised mid term).

### 2.2 `fitting/term_prior.*` - cross-expiry regularity

Nodes `(variance_years, atm_vol, multiples on a fixed z grid, confidence)` from
fitted slices. `create` sorts, validates and makes ATM total variance
non-decreasing in variance time by weighted pool-adjacent-violators. `at(T)`
interpolates ATM total variance linearly in variance time (flat-vol
extrapolation), multiples linearly in T (flat beyond the ends), and returns a
confidence that decays with log-time distance to the nearest node. Leave-one-out
query excludes a slice's own node. Allocation: `create` O(nodes x grid); `at`
allocation-free (fixed arrays). Effective prior strength for a slice is
`term_prior_weight * confidence * n0 / (n0 + n_points)`, i.e. it decays with the
slice's own two-sided quote count.

### 2.3 Graded admission (`fitting/quote_surface.*`)

Per snapshot (one underlier/root/valuation instant):

1. Pass A: every expiry through the unchanged point fitter (`fit_quote_slice`).
   Certified fits are grade-A candidates.
2. Pass B1: every non-A expiry (insufficient quotes, curve failure, arbitrage
   failure) through the channel fitter with the prior from grade-A nodes (or the
   data-only default shape when there are none).
3. Pass B2: a leave-one-out term prior from A nodes plus B1 nodes (B1 down-weighted)
   refits each non-A expiry. Certification is the unchanged `AmericanPriceSlice`.
4. Calendar: instead of refusing both slices of every crossing pair, accept slices
   greedily in confidence order (grade, then admitted point count, then channel
   count) and refuse only a slice that crosses an already accepted one. Every
   accepted pair is still checked by `check_american_calendar`.
5. Grade: A = point fit, certified, calendar-consistent; B = channel fit with at
   least `grade_b_min_points` own mid points; C = channel fit dominated by bounds
   and prior. Refusals remain explicit: unsupported deliverable, no valid quote,
   channel fit numeric failure, certification failure, calendar crossing.

### 2.4 Optional price repair inside the channel (if time permits)

For convexity failures (dense names), replace each side's American prices by their
greatest convex minorant only when every moved node stays within
`max_repair_half_spreads` of its own quote half-spread, then re-run the unchanged
certificate. Never relaxes the certificate.

### 2.5 Policy keys (bench JSON, all opt-in)

Top level: `"admission": "static" | "graded"` (default static) and an optional
`"graded"` object: `channel_weight`, `mid_weight`, `term_prior_weight`,
`prior_scale`, `prior_decay`, `roughness`, `min_half_width_vol`, `n0`,
`grade_b_min_points`, `min_channel_quotes`. Profile `"curve": "knot-spline"`
selects the channel knot spline for the point pass as well (run c). Graded output
adds a trailing `grade` column (A/B/C/empty); default output is unchanged.

Scope exclusions: carry/borrow and `deamer.cpp` are owned by the implied-forward
lane and are not touched; no cap is raised.

## 3. What was built (commit `ecf140dc`, rebased as `aabe436f`), and how it departs from the design

| piece | file(s) | notes |
|---|---|---|
| channel knot spline | `src/fitting/slice_channel.{hpp,cpp}` | internal header; SpiderRock 29-knot LogStd grid, active knots = observed z range +/-1 plus the nine central knots; cardinal natural-spline basis reproduces `SplineVolCurve` serving exactly; semismooth active-set Newton with backtracking on the exact objective, <=50 solves per pass, two passes (re-standardized at the fitted ATM vol); fixed-capacity 29x29 Eigen storage, heap only for O(obs x knots) basis scratch |
| term prior | `src/fitting/term_prior.{hpp,cpp}` | internal header; `create` O(nodes), `at` allocation-free with LOO exclusion and PAV recomputed per query; <=128 nodes |
| channel slice | `quote_slice.cpp` `fit_quote_slice_channel`, `QuoteSliceFitMethod::ChannelSpline` | `fit_quote_slice` split into prepare/admit/certify stages with unchanged arithmetic; channel = American IV of bid and ask at the contract's own carry (same AL forward map as the mid); bounded-move convexity repair (`repair_half_spreads`; the move from the fitted price is bounded, containment in [bid, ask] is not guaranteed) |
| calendar repair | `repair_quote_slice_calendar` | clamp between accepted earlier/later slices, convex minorant, <=3 rounds, move <= `calendar_repair_half_spreads` of the node's own half-spread, unchanged certificate, re-checked against every accepted slice |
| graded surface | `src/fitting/quote_surface.cpp` `fit_quote_surface` | declared in the existing public `quote_slice.hpp` (no new public header, no Tier-B churn) |
| bench | `quote_bench_{main,policy,data}` | `"admission"`, `"graded"`, profile `"channel"`, `"curve": "knot-spline"`; `grade` column and `graded_stats` (per-slice diagnostics) only in graded mode |

Departures from section 2, each made before the official runs:

1. **Channel penalty unit.** The design normalized hinge violations by each
   quote's own half-width. A unit test (`UpperBoundsAloneCapAPriorThatSitsAboveThem`)
   showed that this makes wide and bidless channels soft (a prior sat 0.017 vol
   above a pure ask bound). The channel term is now measured in the fixed unit
   `min_half_width_vol` (0.0025), so it is equally steep for every channel; the
   mid anchor stays spread-normalised. Smoke data: certificate failures of
   channel fits fell 340 -> 65 expiries after this change plus the repair.
2. **Convexity repair implemented** (2.4) after measuring the required move on a
   no-repair smoke: median 0.09, p75 0.28, p90 0.57 half-spreads; 301/340
   failing expiries were within 0.5. Policy value 0.5 (half of the half-spread).
3. **Calendar repair added.** Greedy acceptance alone left 196-271 refused
   expiries with crossing gaps median $0.05; a clamp-and-minorant repair that
   moves each node at most 0.5 of its own half-spread now runs before the
   retry. A repaired grade-A slice is demoted to B (its served prices are no
   longer the point fit's).
4. Grade A is defined by evidence (own admitted point strikes >= profile
   `min_quotes`, served without prior), so it is method-independent (run c).
   Corrected in the review fix round (item 7): A also requires that no repair
   moved a served price, and under a knot-spline/family profile A is that
   profile's prior-free channel fit, not a point fit.
5. **Curve family is a choice, not the vendor's spline** (direction received
   mid-sprint). `QuoteSliceChannelConfig::family` (unset = native knot spline)
   solves the SAME channel objective and term prior with eSSVI, SVI, C8,
   convex-dense or the point spline through `fit_slice_channel_family`
   (`slice_channel.hpp`): bounded active-set reweighting, each step a weighted
   `fit_slice_curve` call on (mid anchor merged with any violated bound) +
   (prior as point targets on the knots the spline would use), active set
   recomputed from the fitted curve, lowest exact data+prior objective kept,
   <= `family_iterations` (12) refits. Weights keep the objective's ratios in
   total variance, scaled so a unit mid anchor at the half-width floor carries
   the point path's `max_weight` cap (an active bound weighs 100x the cap).
   cstar is not a `VolCurveKind` behind `fit_slice_curve`, so it is not a
   candidate; LinearVariance (exact interpolation) is rejected. Commits
   `caf464d1` (solver, policy key `channel.family`) and `fd1d7594` (per-slice
   `iterations`/`solve_seconds` diagnostics).
6. **Rebase composition.** On `da3a4179` the implied-forward carry dispatch runs
   first in `fit_quote_slice` (before the method dispatch, so a resolved-carry
   channel fit works prior-free); `fit_quote_slice_channel` and
   `fit_quote_surface` require supplied carry and the bench rejects
   `admission: graded` with a `carry_mode` (composing the term prior with
   resolved carry is not done). Channel suites are owned by
   `atx-vol-quote-slice-tests` (`tests/slices.cmake`, CLAUDE.md table).
7. **Review fix round** (independent review of `360281b2`: BLOCK on MAJOR-1;
   fixes in `11318231`).
   - MAJOR-1: grade A was given to any served pass-A fit meeting the evidence
     minimum, including convexity-repaired slices and, under knot-spline or
     family profiles, channel fits, contradicting the header ("unchanged point
     fitter, no prior"). Grade A is now redefined consistently in
     `quote_slice.hpp`, the bench `graded_semantics` text and
     `docs/quote_benchmark.md`: own admitted point evidence >= `min_quotes`,
     served by the profile's own pass-A method (point fit, or the prior-free
     channel fit of a knot-spline/family profile) with no term prior and no
     served price moved by a repair. A pass-A slice whose served prices a
     convexity or calendar repair moved is graded B. Only the label changes:
     tiering, prior-node roles and acceptance order are untouched, so the
     served set and every price are unchanged (section 7 reruns).
   - MINOR: `QuoteSliceFit::moved_nodes` flags repaired nodes and
     `evaluate_quote_slice` re-implies their volatility from the served price.
   - MINOR: "in-channel" / "inside the quotes" wording replaced by
     bounded-move (the bound is N x the node's own half-spread from its fitted
     price, not containment in [bid, ask]).
   - MINOR: `validate_quote_slice_channel` (now public; requires
     `channel_weight > 0` and `mult_ceil > mult_floor`) runs once in
     `fit_quote_surface` and for every profile in the policy catalog.
   - MINOR: a snapshot above `kQuoteSurfaceMaxExpiries` falls back to the
     static per-expiry path (grade A/B/C by the pass-A rule, static calendar
     rule), reported as `graded_stats.static_fallback`; it no longer marks
     every row `invalid_market`.
   - NITs taken: `channel_weight > 0`; `n_butterfly_viol` documented as not
     computed for channel fits. Not taken: keeping only the best family curve,
     de-duplicating prior nodes after leave-one-out (would change graded-v1
     prices), splitting the long functions.

## 4. Approximations and known limits

- The channel uses American IVs at the supplied per-contract carry
  (funding-PV aggregate dividends, vendor `sdiv` column as input yield); carry
  error therefore shows up as call/put channel disagreement. Carry is owned by
  the implied-forward lane and was not touched.
- Index-based second differences (P-spline style, as in `fit_spline_vol_slice`)
  over the unevenly spaced SR knots; flat wings beyond the outer active knot.
- Default shape without neighbours is flat in multiples (the vendor uses a
  market-average default curve; not implemented).
- The term prior interpolates shape multiples linearly in variance time and
  holds them flat outside; earnings-variance splitting is not modelled.
- Calendar repair is greedy in confidence order (A, then B, then C by evidence),
  not a joint LP; a refused slice is never used to repair others.
- Both repairs move each node at most 0.5 of its own quoted half-spread from
  its previous (fitted) price; that bounds the move but does not keep a price
  inside [bid, ask]. They never relax `AmericanPriceSlice::create`,
  `check_american_calendar` or `check_nondividend_american`.
- Runtime: graded dev200 ~4.5 min versus ~16 s static (two to three American IV
  inversions per contract, up to three channel fits per non-A expiry). Not a
  speed claim; channel observations could be cached across B1/B2/retry.
- Family solve: each family keeps its own robust loss, filters, weight floors
  and serving gates (e.g. the SVI wing floor, the pinned point-spline butterfly
  gate), so what it minimises is the channel objective as seen through those;
  roughness is the family's own; the prior grid is standardized once at the
  seed (the knot spline re-standardizes once). Bound targets reach 100x the
  point weight cap.

## 5. Gates (focused targets only; the monolithic `atx-vol-tests` was never built)

At `ecf140dc` (pre-rebase, `vol-rel-avx2`, -Jobs 2): 6/6 changed TUs compile
under `/W4 /WX`; focused gtest 76/76 (49 pre-existing + 27 new); anchored
`^atx-vol-quote-slice$` 1/1; `^atx-vol-quote-benchmark-(native|tools)$` 2/2
(CLI file 45/45); PCH-off 12/12.

On the rebased branch (`fd1d7594` code; `vol-rel` per the build-lane rules,
-Jobs 3; the `caf464d1` state was also checked on `vol-rel-avx2`):
- `check` of slice_channel, quote_slice, quote_surface: clean; builds of
  `atx-vol-quote-slice-tests` and `atx-vol-quote-bench` clean (`/W4 /WX`).
- Focused gtest **82/82 pass** (49 pre-existing + 33 new: TermPrior 7,
  SliceChannel 8, SliceChannelFamily 4, QuoteSliceChannel 5, QuoteSurface 8,
  QuoteFitPolicy 1).
- Anchored ctest `-Preset vol-rel -R ^atx-vol-quote-slice$`: 1/1 pass (filter
  now includes `SliceChannelFamily.*`).
- Anchored ctest `-Preset vol-rel -R ^atx-vol-quote-benchmark-(native|tools)$`:
  2/2 pass; CLI file **46/46** (4 graded/knot/family cases plus a graded+carry
  rejection). TRAP: the fresh `vol-rel` configure took `C:/atx/atx-db/.venv`
  as Python (first on PATH, no polars), so both tests failed at import until
  reconfigured with `-DPython3_EXECUTABLE=C:/Program Files/Python312/python.exe`.
- PCH-off (`vol-hygiene`) `check` of the owning TUs of every changed header
  (quote_slice, slice_channel, quote_surface, term_prior, quote_fit_policy,
  quote_slice_carry, quote_bench_main and five test TUs): **12/12 pass**.

Review fix round (`11318231`, pool-34, `vol-rel`, -Jobs 3): `check` of
quote_slice, quote_surface, quote_fit_policy clean; both focused targets build
clean; focused gtest **83/83** (new `KnotSplineProfileGradesAOnlyWithOwnEvidenceAndUnmovedPrices`,
moved-node re-implication checks in it and in the calendar-repair test,
shared channel-config Err, catalog channel validation); anchored ctest
`^atx-vol-quote-(slice|benchmark-tools|benchmark-native|benchmark-campaign)$`
**4/4**; CLI file **47/47** (new static-fallback case with 129 expiries);
PCH-off 12/12; default parity `c89d0739` (`20261016-slice-parity-11318231.csv`).

## 6. Default-mode parity (byte identity)

Base binary (`5e978fed`, linked before any edit, SHA-256 `4772ff92...`) and
committed binary (`ecf140dc`, SHA-256 `7a80f8e5...`) run with
`--policy auto --dividends funding-pv` on dev200 `quotes.csv`:

| artifact | predictions SHA-256 |
|---|---|
| `20261016-slice-parity-base.csv` (base binary) | `c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` |
| `20261016-slice-parity-ecf140dc.csv` (this branch) | `c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` |
| `20261016-slice-parity-dc072f96.csv` (rebased on `d7f4b6b5` + family solver, binary `95fe3f04...`) | `c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` |
| `20261016-slice-parity-fd1d7594.csv` (rebased on `da3a4179`, `vol-rel` binary `d5fdda1b...`) | `c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` |

At `fd1d7594` the native report also differs from the base report only in the
five timing fields, and 191/191 policy decisions are equal.

Byte-identical predictions; native reports identical apart from timing fields;
191/191 policy decisions equal. Versus `20261016-auto-funding-v2.csv` (SHA-256
`b9ed9567...`) the only difference is the two trailing columns (`vol_status`,
`model`) that the base binary at `5e978fed` already writes and the older v2
binary did not: the first 11 columns are identical on all 72,348 rows. Verdict:
**parity holds**; the v2 byte difference predates this branch.

## 7. Benchmarks: graded admission (dev200, funding-PV, 63,942 eligible rows)

Runner `tools/quote_benchmark.py run` (receipts `<csv>.run.json` beside each
CSV in `C:/atx-cache/quote-benchmark/20261014-dev200/`); scorer
`slice-coverage-20261016-scripts/score_graded.py` (labels only for errors and
|z| buckets); scores in `slice-coverage-20261016-score/`. Policies in
`slice-coverage-20261016-policies/`: `graded-v1.json` (SHA-256 `d7f89d13...`,
point A + knot-spline B/C, both repairs at 0.5 half-spreads) and
`knotspline-v1.json` (`579e7720...`, same with every expiry on the knot spline).

| artifact | predictions SHA-256 |
|---|---|
| baseline `20261016-auto-funding-v2.csv` | `b9ed9567b27c082c8fbd996b656aca8114b24a71c85f9ebcebf0bfd2692f71f3` |
| (b) `20261016-slice-graded-v1.csv` | `d817c2256e8c1edcb6b56d8c2d009e9604d83fa1d3cfdb0e1ca1aff9a8658252` |
| (c) `20261016-slice-knotspline-v1.csv` | `172984b72f621dae928993e51f47acb11b383e0a8f3134b680a96be6f460729f` |

Both were produced at `ecf140dc`; rerun at `dc072f96` (rebased) the CSVs are
byte-identical (`20261016-slice-graded-v1-dc072f96.csv`, `...-family-knot-v1.csv`).

**Review fix round reruns** (binary `11318231`, SHA-256 `d934c43d...`):
`20261016-slice-graded-v1-11318231.csv` is byte-identical to graded-v1
(`d817c225...`); `20261016-slice-knotspline-v1-11318231.csv` (SHA-256
`b3fd97640a04d7a5514bd34d82a40d7d9602a7be6a65ae57924b0ab6fe5cec4d`) has the
same 72,348 statuses, prices and vols as knotspline-v1; only 10,654 rows move
A -> B (pass-A knot slices whose prices the convexity repair moved). The
tables below use the corrected grades.

**Coverage (rows).**

| run | served | A | B | C | arbitrage | calendar | insufficient | unsupported |
|---|---|---|---|---|---|---|---|---|
| baseline | 10,694 (16.7%) | 10,694 | - | - | 8,824 | 12,484 | 31,224 | 716 |
| graded-v1 | **50,912 (79.6%)** | 16,334 | 33,304 | 1,274 | 5,320 | 6,904 | 90 | 716 |
| knotspline-v1 (corrected grades) | 49,860 (78.0%) | 6,778 | 41,840 | 1,242 | 8,656 | 4,620 | 90 | 716 |
| knotspline-v1 (as first reported) | 49,860 | 17,432 | 31,186 | 1,242 | | | | |

**Exact intersection with the baseline (rows both price).** graded-v1: all
10,694 raw baseline rows are served at identical price and vol strings (10,654
of them carry labels: MAE 0.1510 both, bias -0.0290, containment 0.9828, core
vol 93.2 bp); 0 baseline rows lost. knotspline-v1: 9,392 labelled rows, MAE
0.1653 -> **0.1620**, bias -0.0302 -> -0.0436, containment 0.9869 ->
**0.9974**, core vol 94.6 -> 89.6 bp; 1,300 raw baseline rows lost (1,026
arbitrage, 274 calendar; 1,262 of them labelled), where the knot slice failed
the certificate and the point fit passed.

**Own population by grade** (price MAE vs label | mid MAE on the same rows |
bias | containment in [bid, ask] | core-vol MAE bp, |z|<=1.5 and >7 d):

| run / grade | rows | MAE | mid MAE | bias | contain | core vol |
|---|---|---|---|---|---|---|
| graded-v1 ALL | 50,395 | 0.2352 | 0.3329 | -0.141 | 0.988 | 744 |
| graded-v1 A | 16,256 | 0.1628 | 0.2706 | -0.059 | 0.975 | 90 |
| graded-v1 B | 32,948 | 0.2733 | 0.3630 | -0.183 | 0.994 | 987 |
| graded-v1 C | 1,191 | 0.1693 | 0.3505 | -0.097 | 0.992 | 5,880 |
| knotspline-v1 ALL | 49,386 | 0.2321 | 0.3382 | -0.140 | 0.997 | 746 |
| knotspline-v1 A (corrected) | 6,772 | 0.2245 | 0.3388 | -0.061 | 0.998 | 47 |
| knotspline-v1 B (corrected) | 41,452 | 0.2349 | 0.3376 | -0.154 | 0.998 | 834 |
| knotspline-v1 C | 1,162 | 0.1734 | 0.3577 | -0.100 | 0.991 | 5,878 |

Every grade beats the midpoint on its own rows. Newly served rows (graded-v1,
39,741): MAE 0.2578 vs mid 0.3469, containment 0.990. Core-vol MAE of B/C is
dominated by low-vega short-dated and wing rows; price MAE is the primary
measure. The B bias (-0.18) traces to rows where the label sits above the mid:
ITM two-sided legs (mid bias -0.24, a carry question for the implied-forward
lane), wide ETFs (QMOM, RXL) and bidless wings; nothing was tuned to labels.

**Strata, graded-v1** (rows | MAE | bias | mid MAE | containment):

| tenor | rows | MAE | bias | mid MAE | contain |
|---|---|---|---|---|---|
| <=7 d | 6,052 | 0.108 | -0.086 | 0.323 | 0.990 |
| 7-30 d | 5,089 | 0.203 | -0.152 | 0.369 | 0.989 |
| 30-90 d | 13,851 | 0.253 | -0.173 | 0.322 | 0.987 |
| 90-365 d | 17,837 | 0.268 | -0.180 | 0.312 | 0.986 |
| >365 d | 7,566 | 0.249 | -0.028 | 0.387 | 0.994 |

| abs z (label vol) | rows | MAE | bias | mid MAE | contain |
|---|---|---|---|---|---|
| <=0.5 | 11,440 | 0.312 | -0.121 | 0.361 | 0.991 |
| 0.5-1.5 | 21,124 | 0.277 | -0.181 | 0.325 | 0.991 |
| 1.5-2.5 | 11,770 | 0.179 | -0.143 | 0.306 | 0.984 |
| >2.5 | 6,061 | 0.053 | -0.034 | 0.361 | 0.982 |

## 8. Curve-family comparison under one channel objective

All six runs use `knotspline-v1` settings (graded admission, every expiry on the
channel objective, same term prior, both repairs at 0.5) and differ only in
`channel.family`. Binary `dc072f96` (SHA-256 `95fe3f04...`), run concurrently;
scorer `family_compare.py` -> `slice-coverage-20261016-score/family-compare.json`
(SHA-256 `70e149324d1d3365...`) and `family-compare-strata.csv`.

| family | policy SHA-256 | predictions SHA-256 |
|---|---|---|
| knot spline (native) | `579e7720...` | `172984b72f621dae...` (= knotspline-v1) |
| eSSVI | `df82af2de419d42c...` | `0e9f68db0d7d63cb...` |
| SVI | `b1bae5299423038f...` | `de8637e7dea13fe6...` |
| C8 | `fb61ddbd1075724b...` | `8825995ed1407dae...` |
| convex-dense | `11cbcccc353aaf8f...` | `7502b7cc3e4bb3c4...` |
| point spline (spline-vol) | `e5efa5ce60d52c6e...` | `26b5693e4162c4f6...` |

**Coverage and arbitrage.** Served slices are certified by construction (zero
served price-arbitrage); the table counts refusals and channel violations of
served slices (fitted vol outside its bid/ask vol channel, before pricing).
The A / B / C split below predates the review fix round: in these runs A is
each family's prior-free pass-A channel fit and includes convexity-repaired
slices (the fix relabels those B, as in section 7). The family runs were not
rerun; the fix does not change any served row or price, so the coverage and
accuracy comparisons stand.

| family | served rows | A / B / C | arb-refused rows (chains) | calendar-refused rows (chains) | fit failures rows | served slices with channel violations (violations / channels) | convexity-repaired slices |
|---|---|---|---|---|---|---|---|
| knot | 49,860 | 17,432 / 31,186 / 1,242 | 8,656 (98) | 4,620 (77) | 0 | 57 (121 / 49,386) | 451 |
| eSSVI | 55,844 | 21,662 / 32,916 / 1,266 | 916 (45) | 6,376 (98) | 0 | 204 (2,642 / 55,309) | 5 |
| SVI | 55,374 | 21,152 / 32,944 / 1,278 | 818 (41) | 6,736 (98) | 208 | 166 (960 / 54,831) | 5 |
| C8 | **56,206** | 20,384 / 34,488 / 1,334 | 864 (43) | 5,992 (96) | 74 | 331 (3,608 / 55,680) | 6 |
| convex-dense | 49,596 | 16,428 / 31,934 / 1,234 | 914 (36) | 8,738 (151) | 3,888 | 273 (969 / 49,176) | 166 |
| spline-vol | 8,520 | 3,706 / 4,280 / 534 | 478 (27) | 518 (17) | 53,620 | 18 (41 / 8,330) | 0 |

The parametric families pass the American price certificate far more often
(arbitrage refusals 0.8-0.9k rows vs 8.7k), which is where their extra coverage
comes from. The point spline's pinned butterfly gate refuses 877 of 1,138
expiries' targets, so it is not a viable channel solver as configured.

**Accuracy on common rows.** 40,972 rows served by all five main families (8,296
of them also by the baseline). Price MAE vs label | bias | containment |
core-vol MAE bp; the midpoint's MAE on the same rows is 0.3506 (0.2750 on the
8,296).

| family | MAE (40,972) | bias | contain | core vol | MAE (8,296) | contain (8,296) |
|---|---|---|---|---|---|---|
| **knot** | **0.2467** | -0.148 | **0.998** | **793** | **0.1535** | **1.000** |
| eSSVI | 0.3127 | -0.165 | 0.978 | 837 | 0.2250 | 0.984 |
| SVI | 0.3114 | -0.214 | 0.991 | 828 | 0.4138 | 0.980 |
| C8 | 0.3643 | -0.167 | 0.957 | 886 | 0.3267 | 0.951 |
| convex-dense | 0.3018 | -0.182 | 0.984 | 891 | 0.2236 | 0.981 |
| graded-v1 (point A + knot B/C) | 0.2570 (39,882) | -0.155 | 0.994 | 809 | 0.1600 | 0.989 |
| baseline quote-slice | - | - | - | - | 0.1600 | 0.989 |

On the 8,296 rows the knot spline is also better than the baseline point fit
(0.1535 vs 0.1600). By stratum (8,296 rows, `family-compare-strata.csv`; no
<=7 d rows) the knot spline has the lowest MAE of all families in every tenor
and |z| bucket; against the baseline point fit it wins 7-30 d (0.099 vs 0.133),
30-90 d (0.093 vs 0.117), 90-365 d (0.134 vs 0.155) and every |z| > 0.5 bucket,
and loses >365 d (0.229 vs 0.197) and |z| <= 0.5 (0.200 vs 0.184). SVI fails
badly beyond a year (MAE 1.01, bias -0.85).

**What the extra coverage is worth.** Rows the knot spline does not serve, as
served by each family: eSSVI 7,829 rows MAE 0.408 (containment 0.854), SVI
7,667 rows 0.290 (0.934), C8 8,722 rows 0.347 (0.860), convex-dense 5,273 rows
0.335 (0.952); graded-v1's point fits add 4,017 rows at 0.098 (0.932). The
parametric families buy coverage with quotes they cannot fit inside the
channel.

**Per-expiry time.** Separate sequential reruns with the perf-pin binary
(`vol-rel-avx2`, `fd1d7594`, SHA-256 `c89f414a...`), `20261016-slice-timing-<family>-fd1d7594.*`;
every rerun CSV is byte-identical to its concurrent run above. `solve` is the
channel solve alone for the served/last fit of each of the 1,131 expiries with
channels (`solve_us`); `expiry` is the amortised whole-surface time per expiry
(IV inversion of bid/ask/mid, fits, certification, repairs, retries). Shared
host (other lanes active), one run each: diagnostic, not a speed claim.

| family | solve p50 us | solve p95 us | solve mean us | Newton its / refits p50 (p95) | expiry p50 ms | expiry p95 ms | run s |
|---|---|---|---|---|---|---|---|
| knot | **75** | 728 | 227 | 2 (20) Newton | 244 | 971 | 365 |
| eSSVI | 102 | 762 | 242 | 1 (6) refits | 189 | 788 | 274 |
| SVI | 522 | 3,522 | 1,012 | 1 (3) | 191 | 814 | 281 |
| C8 | 418 | 12,817 | 2,303 | 1 (12) | 186 | 804 | 289 |
| convex-dense | 657 | 3,936 | 1,196 | 1 (3) | 180 | 671 | 263 |
| spline-vol | 153 | 562 | 211 | 0 (1) | 198 | 1,099 | 362 |

The solve is a small fraction of per-expiry time for every family; the knot
runs are slower end to end because their certificate failures trigger the
convexity/calendar repairs and stronger-prior retries (451 repaired slices).

## 9. Recommendation (by evidence, everything stays opt-in)

- **Channel solver: keep the native knot spline.** Under the identical
  objective and prior it is the most accurate family on common rows (overall
  and in every tenor and |z| bucket), keeps 99.8% of prices inside the quotes
  and has the fewest channel violations (121 vs 960-3,608); its solve is the
  cheapest at the median (75 us vs 102-657 us), although its end-to-end
  expiry time is higher because more of its slices need repairs.
  eSSVI, SVI and C8 add 5-6k rows only by serving fits that leave the channel
  (containment 0.85-0.93 on those rows); convex-dense serves 264 fewer rows
  than the knot spline (0.952 containment on the rows it adds); the point
  spline is refused by its own butterfly gate. Not chosen for vendor
  similarity: the spline wins on these measurements.
- **Admission: graded-v1 (point fit A, knot spline B/C)** is the best default
  candidate today: most coverage (79.6% vs 16.7%), no baseline row lost and the
  baseline's exact prices on the intersection. All-knot (knotspline-v1) is more
  accurate on the rows both serve but loses 1,300 raw baseline rows (1,262
  labelled) to certificate failures. The evidence points to a ladder (knot spline first, point fit when
  the knot slice fails the certificate) - not yet built (section 10).
- Defaults stay unchanged (`admission: static`, no family) until a held-out
  validation split confirms these dev200 results.

## 10. Unfinished / next

1. Knot-first ladder with point-fit fallback on certificate failure (projected
   from section 8: keeps the knot's accuracy and recovers the 1,300 rows), then
   rerun on dev200 and the validation split.
2. Remaining refusals in graded-v1: 5,320 arbitrage rows (mostly the
   vertical-spread and non-dividend checks under strike-varying supplied
   carry) and 6,904 calendar rows; both depend on the implied-forward carry,
   which this lane did not touch. Composing graded admission with
   `carry_mode` (term prior in resolved-carry coordinates) is not implemented.
3. B-grade negative bias (-0.18), mostly ITM legs where label > mid: re-measure
   once implied-forward carry is composed.
4. Vendor-style market-average default shape (we use flat multiples), earnings
   variance in the term prior, joint (LP) calendar repair.
5. Runtime: cache channel observations across B1/B2/retry; graded dev200 takes
   ~5-9 min vs 16 s static, dominated by American IV inversions, not the solve.
6. Family solver: families keep their own robust losses and gates; a family
   whose own regularisation is aligned with the channel objective (or a
   C8/eSSVI fitter accepting bound constraints directly) was not attempted.
