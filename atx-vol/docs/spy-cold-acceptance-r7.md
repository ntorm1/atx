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
validation with native sigma/q family inversion and batching. It also explicitly
enables `graded.terminal_floor_recovery`: the library enforces terminal floors and
widens only directly conflicting asks in its projection boxes, while scoring
against the original input quotes. Reports expose the number and largest dollar
amount of ask corrections, plus native repaired-price inversion counters.
Ask-correction counts belong to each repair attempt: a quote normalized in both
slice and joint projection can appear twice. Unique departures are counted from
the final raw-input comparison, never by summing repair counters.
It is a candidate,
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
count one, and `-SiblingBusyThresholdPct 5 -RequireIdleSibling`. The `qualify`
command inspects every native timing report and demands every observed
cold-calibration time below 1.000 s:

Qualification counts actual successful records and requires at least 100 distinct
trial IDs, report files, report hashes, and kernel PID/creation-time identities.
The launcher and native executable independently record each process identity.
Each CSV is rehashed, each native clock must equal its launcher summary, and each
native report must show exactly one initial repetition with an empty de-Am memo.
Native affinity, worker count and sibling load are checked directly; sibling
activity must be measured below 5%. Older manifests without process identities
cannot qualify. Summary counters or an asserted idle-sibling flag are insufficient.

```powershell
python atx-vol/bench/spy_acceptance.py qualify --input $inputCsv `
  --audit-dir $auditDir --cold-manifest 'C:/.../cold-100/manifest.json' `
  --build-cache build-vol-rel-avx2/CMakeCache.txt --out 'C:/.../acceptance.json'
```

The clock covers resident-snapshot normalization, fitting/repair and publication.
CSV loading and process startup are separately reported. If a serving record is
part of the selected API route, its construction must be included. An audit with
no attached record only verifies the direct fitted-slice API, never the record
or live-publication route. Attached records fail the continuous representation
gate until their own between-node shape/calendar proof is implemented; finite
actual-API probes still diagnose record-price disagreement.

The correctness oracle also reconstructs the common spot, market clocks/rate/cash,
per-strike normalized carry, and exercise-horizon bounds from frozen raw inputs.
Exported bounds cannot redefine the constraints used to judge the exported prices.
The current query export includes IV interval endpoints and volatility conventions;
the verifier requires well-formed actual metadata and exact CSV/API equality for
status, reason, volatility, both interval endpoints and convention on every eligible
row. Earlier audit files without those fields cannot satisfy this expanded gate.

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

## Executed combined baseline, before joint recovery integration

`acceptance/combined-before-joint-audit-v1/` contains a completed native audit,
receipt, predictions and `verification.json`. It used the combined C4/native
candidate plus bounded carry normalization, before the new joint solver and
terminal-floor recovery. Binary SHA256:
`bdcc6d103dcfb07d182f3c40a5540c47b2b18001d2c332e2c9ccf4e7040a71a6`.
Prediction SHA256:
`1475253faac77aeb50b47f235df9fe4f4bf72e144c4368933c89339598e42e94`.

This baseline **fails acceptance**: 27/33 slices and 10,362/12,866 rows served;
10,258 contained, or 98.9963328% containment among served rows. The six refused
expiries are 20742, 20745, 20746, 20777, 20805 and 20833, all calendar refusals.
Bounded carry normalization recovers 20875 and changes exactly one strike with
maximum price-change bound `1.3233227929e-7` USD.

The actual API oracle finds 48 terminal-floor violations: 20 nodes on 20741 and
15 on 20749 (max approximately `5.4e-5` USD), four nodes on 21533 and nine on
21568 (max $0.779517). The other independently checked shape, affine, calendar,
between-node/API consistency constraints pass on represented support. The
2,504 missing rows still fail completeness. These observations motivate enforcing
terminal lower bounds at **every** projected node, while widening boxes only for
the 15 direct input conflicts. The audit's approximately 144-second fit/reprice
clock includes expensive validation calls and is **not a performance result**.

Focused verification of the harness: 21 independent Python oracle tests pass;
native CLI checks cover the audit, normalization, existing common-spot behavior,
floor-policy parsing and fresh-process identities. The benchmark target builds
with `vol-rel`. Negative cases include
withheld-price leakage, between-node disagreement, missing union queries, false
identified price-only volatility, nonadjacent calendar crossings, discounted
terminal floors under nonzero carry, two workers, a multi-CPU process mask and
an unexpected expiry fanout. ThinLTO performance qualification is still pending.
Independent review found and reproduced a former qualification bug: one native
50-second trial could pass under a summary claiming 100 trials of 0.5 seconds.
The joined evidence checks above and adversarial regression now reject this case.

## Integrated full-surface correctness result

Root commit `2362f6aa` passes the independent actual-API correctness gate in
`acceptance/integrated-full-v1-audit/verification.json`: all 33 valid expiries and
12,866 eligible rows are served, with 12,851 inside the original boxes
(99.88341365%). The 15 departures are exactly the previously proved direct
terminal-floor conflicts. There are zero raw-model, shape, parity, calendar,
analytical between-node terminal-floor or CSV/API price failures.

The audit covers 6,433 price nodes and 62,494 actual side/strike queries, with no
attached serving record. All 12,866 actual node statuses, reasons and returned
volatilities also agree with CSV metadata (`metadata-verification.json`). There
are 12,183 identified volatilities and 683 price-only rows: 16 floor intervals,
610 low-vega cases and 57 unidentified intervals. CSV interval structure passes;
the current actual-query export does not include interval endpoints or volatility
conventions, so exact API/CSV equality for those fields is not asserted here.

Binary SHA256: `004993da5a846dce39f1d0c1177b767ea3cb9268459a36678b58548a46a8b9c3`.
Prediction SHA256: `2be0eb424dbd04e0a9173c7f5997d6e5b7c67e8c8d2435f54792af75878568fc`.
The prediction bytes match the unaudited development profile exactly. Its
21.217-second development cold clock is not a qualifying ThinLTO performance
result; the audit's approximately 60.3-second clock includes expensive query
verification. The sub-second goal remains unachieved.
