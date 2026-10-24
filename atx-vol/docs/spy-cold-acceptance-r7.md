# SPY cold-fit acceptance and independent feasibility evidence

Date: 2026-10-23 (America/New_York). Starting local main:
`a53c48c4a3c404fe63e24f07f1d574a2a015ed38`. Acceptance branch:
`feat/vol-r7-acceptance-20261023`, pool-33. This document records a development
oracle and reproducible gates, **not an achieved subsecond fit**.

## Frozen workload and denominator

Input: `C:/atx-cache/quote-benchmark/fit-timing-20261017-cohort5/input/quotes-SPY.csv`.
SHA256: `b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e`.
There are 13,168 rows in **34** raw expiry groups. Expiry 20740 has 302 rows with
`years_calendar=0` and upstream `invalid_time`. The other **33 expiries and 12,866
rows remain required**. A fit failure, nonconvergence, low vega, wide spread,
bidless wing, or contradictory quote does not remove a row from this denominator.
All 6,433 positive-time strike pairs are present; no quote has equal bid and ask.

The frozen gate requires every eligible row to have a finite served price, all
33 slices fully served, at least 99.5% of those prices within original bid/ask,
and the final API representation to satisfy the stated arbitrage constraints.
At most 64 of 12,866 served prices can be outside original quotes. Genuine
price-only results count as served; claiming an identified volatility with a
price-only result fails. Fitting receives no vendor price or volatility labels.

Raw eligibility is saved in `acceptance/eligibility-v1.json` under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.

## C4 comparison on that exact input

Saved C4 file:
`C:/atx-cache/quote-benchmark/C4-20261023-inbox/runs-56d68e36/spy/spy-C4-inbox-h0.csv`.
SHA256: `c9d14f1493a5a1edd720d1992c975cb301e824370007af0dcbf7dd01a79cf8a2`.
It serves 10,068 rows in 26 slices and contains 10,026 of those prices:
**99.5828367% containment among served rows**, 78.2527592% eligible coverage.
The unrounded count differs from the historical rounded 99.50% dashboard.
Comparison receipt: `acceptance/c4-comparator-v1.json`.

## What is actually feasible

The offline SciPy/HiGHS LP has 12,867 variables and 307,954 constraints, including
205,290 calendar constraints at all pairwise overlapping union knots. It uses
the same snapshot spot, 773.0; calendar and variance clocks are kept distinct.
Quotes enter only as raw bid/ask boxes. Every call/put is constrained by intrinsic
and upper bounds, vertical spreads, convexity, affine cross-side bounds, and
calendar monotonicity. `t >= 0` is mandatory when maximizing least box slack:
negative slack would widen quotes and is not an exact-box feasibility witness.

Funding-PV terminal prepaid is `S*exp(-q*T_calendar)-PV`. The actual certificate
uses **effective** carry including aggregate cash, so its prepaid bounds are
`min(S,min(terminal_prepaid))` and `max(S,max(terminal_prepaid))`. Earlier archived
LPs used raw continuous-yield extrema and subtracted PV afterwards; those bounds
are looser for cash-dividend expiries. Initial r7 `*-v1` LP artifacts repeated
that simplification. They are superseded by the effective-carry artifacts below.

With actual effective-carry envelopes, the generic certificate constraints are
feasible inside all quotes for all 33 slices (`feasibility-joint-effective-carry-
noextra-v2.json`). **That does not establish all necessary pricing bounds.**
Adding the strike-specific terminal European lower bounds
`C >= max(terminal_prepaid-K*df,0)` and
`P >= max(K*df-terminal_prepaid,0)` reveals **15 calls whose ask is below that
lower bound**: four in expiry 21533 and eleven in 21568. The largest unavoidable
departure is $1.0099945023 (expiry 21568, strike 130, original bid/ask 642/646).
These rows remain in the serving and containment denominators.

Raise only those mathematically contradictory asks to their terminal floor plus
the fixed numerical allowance. Keep all original bids and every other ask exact.
The resulting all-33 joint problem is feasible, with exactly **15** prices outside
the original quote boxes: **12,851/12,866 = 99.88341365% containment**. Maximum
constraint residual is `4.6566127343e-10` USD. Every unavoidable departure is less
than one spread: max excess above ask / full spread is 0.2524986256; max distance
from midpoint / full spread is 0.7524986256.

This proves the minimum number of quote departures for the finite constraint
system is 15: direct individual lower bounds require at least 15, and the joint
witness attains 15. It is an **offline feasibility witness**, never a production
fit or fitting input, and establishes no latency result. Native fitting must
independently recover a valid surface and pass the API audit.

Authoritative receipts:

- `acceptance/terminal-floor-conflicts-v1.json`: all 15 row IDs and exact gaps.
- `acceptance/feasibility-joint-terminal-floors-elastic-v3.json`: joint witness
  summary, original-quote departures, and typed floor-only box adjustments.
- `acceptance/raw-boxes-terminal-floor-recovery-v3.json`: label-free diagnostic
  fixture, with effective bounds, strike-specific terminal prepaids/discounts,
  original boxes, and only the 15 floor-conflicting asks adjusted.

The zero-interior case in expiry 20742 is **not a locked quote** and is not invalid
data. Calls at strikes 800/801/803 have ask/bid/ask equal to
0.009999999776482582. Convexity forces a flat one-cent plateau. A method requiring
a strictly interior feasible point can fail on this valid boundary solution.

Expiry 20875 is also valid. At strike 784 its call/put carry differs by
`4.656612873e-10` per year; the corresponding shared-spot forward difference is
approximately `1.3435064e-7` USD. The existing exact-carry equality check refuses
the whole expiry. Explicit `snapshot.carry_price_tolerance=1e-6` invokes the
library's bounded, deterministic carry coalescing after common-spot rebasing.
Material conflicts still refuse. Normalization reports its count and bound,
changes no quote box, and is inside the cold normalization clock.

## Executable acceptance

`bench/spy_acceptance.py` freezes eligibility, analyzes feasibility, compares
predictions, runs an actual API audit, and verifies acceptance. Python 3.12.2,
NumPy 1.26.4, SciPy 1.12.0 were used for LP evidence; no dependencies were changed.
Only feasibility needs NumPy/SciPy; the acceptance verifier uses the standard
library. Artifacts are created exclusively; existing files are never overwritten.

The candidate policy `bench/policies/spy-c4-native-candidate.json` combines C4's
one-spot, floor-price-only, box h0, inbox repair, market-distance repair, publication
validation with native sigma/q family inversion and batching. It is a candidate,
not a qualified or default-promoted policy. No host-dependent wall budget is used.

```powershell
$inputCsv = 'C:/atx-cache/quote-benchmark/fit-timing-20261017-cohort5/input/quotes-SPY.csv'
$candidate = 'atx-vol/bench/policies/spy-c4-native-candidate.json'
$auditDir = 'C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/candidate-audit-NEW'
python atx-vol/bench/spy_acceptance.py run-audit --input $inputCsv `
  --binary build-vol-rel/bin/atx-vol-quote-bench.exe --policy $candidate --out $auditDir
python atx-vol/bench/spy_acceptance.py verify --input $inputCsv `
  --audit-dir $auditDir --out "$auditDir/verification.json"
```

`--acceptance-audit` exports final certified nodes, bounds, support masks, actual
`served_quote_slice_price` and `query_quote_slice` values/statuses at every node,
quarter/midpoints and every expiry union knot. It also exports declared market
carry. The independent checker reconstructs global convex envelopes, all-pair
vertical bounds, affine bounds, and all-pair calendar constraints. It checks the
terminal lower bound continuously: on a union segment prepaid is `c*K^a` minus
constant cash, so endpoints and the single possible derivative root cover any
interior extremum. Missing audit data, withheld-price leakage, API/CSV disagreement,
or missing expected queries fail closed. No extrapolated domain is certified.

Audit calls are expensive and deliberately contaminate that run's clocks; the
native report says `timing_qualifying=false`. A performance pass requires a
**separate** fresh-process run without `--acceptance-audit`, with identical binary,
input, policy, and prediction hashes. Use `bench/run_cold_trials.ps1` on a clean
`vol-rel-avx2` build, 100 fresh trials, affinity to one performance core, fit worker
count one, and `-RequireIdleSibling`. The `qualify` command inspects every native
timing report and demands every observed cold-calibration time below 1.000 s:

```powershell
python atx-vol/bench/spy_acceptance.py qualify --input $inputCsv `
  --audit-dir $auditDir --cold-manifest 'C:/.../cold-100/manifest.json' `
  --build-cache build-vol-rel-avx2/CMakeCache.txt --out 'C:/.../acceptance.json'
```

The clock covers resident-snapshot normalization, fitting/repair and publication.
CSV loading and process startup are separately reported. If a serving record is
part of the selected API route, its construction must be included. An audit with
no attached record only verifies the direct fitted-slice API, never the record
or live-publication route.

## Limits and primary reference

These gates establish the explicitly checked static constraints on the actual
published representation. They do not establish a general continuous-time
martingale model fitting every American price, validate an unknown cash-dividend
schedule, or certify all possible exotic/dynamic strategies. In particular,
American shape and calendar monotonicity alone do not imply every stronger
American/European consistency condition. Cox and Hoeggerl derive additional
conditions for co-terminal European and American puts on a **non-dividend** asset;
that theorem must not be presented as a sufficiency theorem for the present
dividend-paying SPY surface. [Primary paper](https://arxiv.org/pdf/1301.5467).

The root must still verify the real candidate reaches the original full objective.
Neither the feasible LP nor the native projection microbenchmark is that result.
