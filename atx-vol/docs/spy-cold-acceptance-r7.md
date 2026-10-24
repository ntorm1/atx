# SPY cold-fit acceptance and independent feasibility evidence

Updated: 2026-10-24 (America/New_York). Starting local main:
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
The subsequent `acceptance/eligibility-identification-v2.json` keeps exactly
that raw eligibility and adds an independent minimum of **12,183 identified
rows**, frozen from wave1. `bench/fixtures/spy-wave1-identification-v1.json`
records the 683 baseline price-only row IDs and source/input/report/prediction
hashes. Every other eligible row must remain identified. A new price-only result
fails the coverage gate even if another row becomes identified and preserves
the total count. A legitimate change of final prices/classification requires
explicit review of the baseline; it cannot silently weaken acceptance. This
baseline is acceptance evidence only and is never provided to fitting.

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
  --accuracy-contract raw-spread-price-v1 `
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
  --accuracy-contract raw-spread-price-v1 `
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

Every identified actual API node also receives three independent cold
Andersen-Lake forward evaluations at its reported volatility and exactly
`sigma - 1e-7`, `sigma + 1e-7`. The export includes evaluated sigmas, model,
effective carry and all kernel options. The verifier independently reconstructs
the carry and checks Reference-profile pricing. `cold-roots` retains the strict
price-straddle diagnostic for every identified node, including complete failure
contexts and raw row IDs. Roundoff-only or flat cases are reported separately and
do not satisfy the historical strict IV gate; its diagnostic price allowance is
`32 * epsilon * max(1, S, K, abs(target), abs(cold prices))`, never an enlarged
IV tolerance. Price-only rows are counted separately without invented IVs.
These calls occur only inside the explicitly nonqualifying audit run.

The user approved the economically meaningful price contract on 2026-10-24:
`abs(cold Reference price at published IV - actual API price)` must not exceed
`min($0.001/share, 0.01 * (original ask - original bid))`. Select it explicitly with
`--accuracy-contract raw-spread-price-v1`. Results use acceptance schema v2 and
embed the selected contract and constants. Omitting the flag preserves the
historical `strict-iv-v1` gate; prior reports are not silently reinterpreted.

The budget comes from each original eligible row, never normalized/repaired or
exporter-supplied boxes. Duplicate contracts must satisfy every original row,
equivalent to using the tightest budget. A locked quote has zero economic budget.
Only the separately reported `32*epsilon*max(1,S,K,abs(target),abs(cold center))`
roundoff allowance remains; no minimum spread or budget floor is introduced.
Crossed/nonfinite boxes are invalid raw inputs under the existing frozen rules;
the budget function rejects them. Genuine price-only rows remain served and make
no point-IV claim. All serving, containment, model, metadata and price-arbitrage
gates are unchanged. Missing original budgets or missing cold contracts fail.
The generic numerical oracle also requires at least one checked identified row;
an entirely price-only surface cannot obtain a vacuous accuracy pass. The SPY
promotion gate additionally enforces the frozen per-row identification coverage.

The original +/-1e-7 IV results are diagnostics under this version, including
local finite-difference vega, residual/vega and budget/vega estimates. Those local
ratios are not root-error bounds or certified uncertainty intervals. This price
contract does **not** establish Greek accuracy. `cold-prices` saves the complete
raw-row join, per-row budgets and these diagnostics:

```powershell
python atx-vol/bench/spy_acceptance.py cold-roots --input $inputCsv `
  --audit-dir $auditDir --out 'C:/.../cold-node-roots.json'
python atx-vol/bench/spy_acceptance.py cold-prices --input $inputCsv `
  --audit-dir $auditDir --out 'C:/.../cold-node-prices.json'
```

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

Root commit `2362f6aa` passes the then-current, narrower actual-API correctness gate in
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

## Wave1 full audit and authorized economic price contract

Source `47b2037572690d60ce87113c37340c75ac129913` was audited in
`acceptance/integrated-full-wave1-audit/`. The native binary SHA256 is
`ee32307bdd29ef4a302364ddba520d148e5c1a126209dc3e8ccdffd6f8fc01da`;
native report SHA256 is
`72fe8d747a724bf92817733fe308d6a4fc91b90796f27daecad21476c58de3e7`.
Prediction SHA256 `681c3b44e3edbd799b90b03ccfaad2564d98a580085ae4131093cf749e08a8cf`
matches the separate unaudited development profile byte for byte.

All 33 valid expiries and 12,866 eligible rows are served; 12,851 prices remain
inside the original quotes (99.88341365%). All actual API shape/parity/calendar,
analytical between-node floors, independent raw carry/bounds, and exact CSV/API
metadata checks pass. The expanded audit checks all 12,183 identified final node
volatilities and leaves 683 genuine price-only rows without invented point IVs.

The original strict result remains preserved in `verification.json` and
`cold-node-roots.json`: 11,157 strict straddles and **1,026 failures** of the
unchanged +/-1e-7 IV diagnostic (979 puts, 47 calls), with no roundoff-only cases.
This was an accuracy blocker under the earlier contract, not a successful strict
IV fit. The earlier `2362f6aa` binary did not export these cold prices; its narrow
API pass is not evidence that those IVs satisfied the strict target.

Re-evaluating the same immutable wave1 evidence under the explicitly authorized
`raw-spread-price-v1` contract yields **zero price-budget failures** across all
12,183 identified nodes. `cold-node-prices-v2.json` contains every raw row/budget,
input/report hash receipt. `verification-economic-v3.json` passes all correctness
checks including frozen identification coverage: no new price-only row IDs and
all 12,183 identified rows checked. No fit was
rerun and no price, quote box, IV, classification or denominator was changed.
No row needs the separate roundoff allowance. Maximum absolute residual is
**$0.0007096053/share**, using 70.9605% of that row's $0.001 budget: expiry 21568,
put strike 925, row 27423846, original bid/ask 150.5/154.5. Median residual is
`4.83169e-13`, p95 `6.44746e-7`, and p99 `9.43202e-5` USD. The cap binds on 6,327
rows; the raw spread fraction binds on the other 5,856. The smallest budget is
`9.9997520e-5` USD because the raw cent spread is represented in binary floats.

Low-vega diagnostics remain economically relevant. The largest local
`abs(residual/vega)` estimate is 0.001700705 IV units: expiry 21078, call strike 65,
row 26827828, cold price residual $0.0001873946 and local vega 0.1101864. This is
a local estimate, not an independently solved root error. Budget/vega exceeds
0.001 IV units on 1,068 identified nodes and 0.01 on 164; its maximum 8.25924 is
too large for a local linear approximation to establish meaningful IV uncertainty.
The reported point IVs must not be used to imply certified hedge Greeks.

The two representative worst-price/low-vega contracts, including their opposite
sides and all four original raw boxes, are frozen in
`bench/fixtures/spy-wave1-cold-price-cases-v1.json`. Their numerical regression
passes the economic contract, preserves two strict-IV failures, and fails when
an original box is made genuinely tighter. The 40-test independent oracle suite
also exercises explicit version selection, duplicate-row budgets, locked/crossed
spreads, nonfinite evidence and the unchanged timing protections. The native CLI
fixture checks both sides against independent Black-Scholes cold prices and
passes the versioned economic oracle with original input boxes.
Fresh peer review found that the first economic oracle could pass an entirely
price-only fixture because it checked no point IVs. The follow-up rejects zero
identified rows and freezes the SPY count and identities independently of the
candidate. Regression tests reject both mass demotion and a count-preserving
swap of an identified row for a formerly price-only row.

The separate development cold fit was **22.1222 seconds**, with 8.57% measured
SMT-sibling activity. It is neither a quiet latency qualification nor a subsecond
result. The expensive API/cold-reference audit is correctness evidence only.

## Economic candidate: complete API audit at 13.03-second development fit

Source `0a5b2bc1acfe0d0cc45caa0e9aa0a5925fb386ff` with explicit policy
`bench/policies/spy-c4-economic-candidate.json` passes all 12 independent
correctness gates in `acceptance/integrated-economic-v1-audit/verification-economic-v1.json`.
The policy adds only the approved `.001` dollar cap and `.01` raw-spread fraction
to each of the three prior candidate profiles. This is an opt-in candidate;
historical default controls are unchanged.

All 33 valid expiries and 12,866 eligible rows remain served, with 12,851 inside
the original boxes (99.88341365%). Every served price string matches wave1;
there are zero price changes. There are zero API shape/parity/calendar,
between-node terminal-floor, raw-model or CSV/API metadata failures. All 12,430
identified node IVs pass the original-row cold Reference price budget, maximum
error **$0.0007096053236/share** (70.9605% of the row's budget), with no reliance
on the separate roundoff allowance. Median/p95/p99 price residuals are
`5.04485e-13` / `1.33296e-5` / `0.0002830024` USD.

The frozen identification coverage passes with **zero demotions**. There are
12,430 identified rows and 436 price-only rows: 247 baseline price-only rows now
return identified values (196 formerly `low_vega`, 51 `unidentified_interval`).
The strict IV diagnostic still reports 1,271 failures and 11,159 strict straddles;
245 of the 247 newly identified values fail that tighter diagnostic. These are
acceptable under the explicitly approved price contract, and do not establish
strict IV or Greek accuracy. Maximum local `abs(residual/vega)` estimate is
0.001899584 IV units; it remains a local conditioning diagnostic.

The same frozen binary's separate one-core development profile was **13.0317839
seconds**, one fit worker on performance core 6 with 3.42385% measured sibling
activity. It is not a ThinLTO/100-trial qualification and remains far above the
subsecond goal. The audit's additional cold/reference queries are excluded from
that performance figure. `summary-v1.json` joins the audit hashes, comparison
counts and these results.

Exact evidence hashes:

- Binary: `b7b78bb058944f716d9fe5f47e7d3d797ba01649555696cc53508190ff996b74`.
- Policy: `007fc30921f0f87ea37dd3c59589bd5a00a8a6ea6cd8749560a35a2eb14b931a`.
- Native audit: `0fb3e3cac9a2d35ec0708e13f0601afac4771eafa758241554867fc0c666f4ff`.
- Audit/profile predictions, identical:
  `8581165c8410f73d4284ab5222650c8d9728f0eb998ad5f7adf3cbcd0b3706aa`.
- Verification: `409cf65eb5aad4e818d737bf4f4a18e134d82583b330e6e8f94047ca2ae099ce`.

The integrated validation receipts report 11 native CLI cases, 19 archive cases,
45 pricing cases and 322 quote-slice cases passing (two explicit opt-in tests
skipped). Source review covers policy/provenance, copied trial budget state,
catalog validation and explicit archive refusal/rollback for the new unpersisted
fields. These checks support this candidate; they are not a latency qualification.

## Seed candidate review: target-floor classification (2026-10-24)

The subsequent source `8b410e31` development profile took 4.8842 seconds and
served the same prices, but it is **not a valid promoted baseline**. Its economic
price residuals passed the historical twelve gates while eight formerly
floor-volatility rows acquired point IVs. A cold residual within the allowed
price budget and a positive local chord do not establish that the target itself
lies above the IV reporting floor.

The private seed checker now applies the deterministic ceiling and zero-volatility
exercise-value band used by Search, then requires the target to be strictly above
the cold reference price at `kIvMin = 0.005`. Regressions include actual SPY row
27410223, a zero-rate European target implying 0.00499 with an otherwise acceptable
0.00501 seed, and an out-of-range target. All failed before the fix and pass after
it. The audit exporter now records `floor_sigma` and `price_at_floor` in the exact
reported contract context; a closed-form European fixture verifies those fields.
The independent audit is being extended with a separate floor classification gate.

Evidence: `economic-v2-profile/floor-promotion-regressions.json`,
`economic-seed-floor-red.xml`, `economic-seed-floor-green.xml`,
`economic-seed-floor-quote-green.xml`, `native-floor-export-red.xml`, and
`native-floor-export-green.xml`. The corrected source passes nine pricing,
22 quote and one native CLI tests, plus owning PCH-off compilation. A complete
audit and a new timed run of the corrected source remain required; the previous
4.8842-second measurement cannot be reused for it.

## Target-IV floors: independent thirteenth gate

The monetary repricing budget does not authorize identifying a target that belongs
to the zero-volatility or minimum-volatility interval. Acceptance output schema
`atx-vol-spy-acceptance-v3` therefore adds
`identified_node_target_floor_semantics` as a separate thirteenth check. The
unchanged `raw-spread-price-v1` result remains visible independently. Previously
saved twelve-gate receipts are historical evidence and are not rewritten.

Under `target-iv-floor-v1`, every final Success node must satisfy all three rules:

- The target exceeds the zero-volatility American value plus
  `1e-9 * price_ceiling + 1e-12`. Python independently maximizes the discounted
  deterministic payoff over time zero, maturity, and the possible interior
  stationary point; the ceiling accounts for negative rates or carry.
- The target strictly exceeds the cold Reference price at sigma `0.005`.
  The native audit must supply finite same-context `cold_node.floor_sigma` and
  `cold_node.price_at_floor`; the floor must equal `0.005` exactly. Missing fields,
  unsupported models, changed contract/options or nonfinite evidence fail closed.
- The reported sigma itself exceeds `0.005`. Neither the economic price budget
  nor its roundoff allowance relaxes these identification rules.

PriceOnly remains a served-price classification without a point-IV claim. The
frozen no-demotion/coverage gate continues separately; a legitimate new floor
classification must be reviewed explicitly. Standalone `cold-floors` writes the
full observations and failure contexts as `atx-vol-cold-node-floor-audit-v1`.
`verify` and `qualify` always enforce this gate in the new output schema.

The `8b410e31` candidate's saved economic-v2 audit still passes the prior twelve
checks, including cold monetary accuracy, when evaluated by the new verifier.
It fails the floor gate: **eight of 12,858 Success nodes** independently violate
the zero-volatility target screen. That historical exporter also lacks sigma-floor
price evidence for all 12,858 nodes, so the new gate cannot pass even after the
eight violations are accounted for. Its eight other nodes are PriceOnly.
The new receipt is
`acceptance/integrated-economic-v2-audit/verification-economic-with-floor-v3.json`;
the old receipt is untouched. This result does not promote that candidate's
separate development latency.

The eight actual failures are frozen with original boxes, row IDs, exact native
contracts and source/artifact hashes in
`bench/fixtures/spy-economic-v2-floor-promotions-v1.json`. No historical floor
price was fabricated. A separate closed-form test uses `S=K=100`, `T=1`, zero
rate/yield: the price at sigma `0.00499` is within $0.001 of the price at seed
`0.00501`, yet must fail identification because the target is below the sigma
floor. A genuinely just-above-floor target passes. Additional regressions cover
interior exercise optima with both signs of rates, floor equality, invalid/missing
fields, changed context, all-PriceOnly evidence and independent monetary PASS
beside floor FAIL. All 47 Python oracle tests pass.

## Complete final-price candidate, 2026-10-24

Source `cfb1e4363ec6545b9e31a43e9238d59dfa6ac663` uses the explicit
`joint_feasible_mid` objective: intersect quote intervals with proven floors,
project once across the surface, then identify each final node once. A single
fresh-process development run on performance core 6, one worker and 0% measured
SMT sibling activity took **1.4140127 seconds**. This is a non-ThinLTO development
measurement, not a 100-trial or subsecond qualification.

The same binary's complete API audit serves **33/33 valid expiries and
12,866/12,866 eligible rows**, with **12,851 inside the original quotes
(99.88341365%)**. It reports **12,850 identified IVs and 16 PriceOnly rows**.
All identified nodes pass the approved monetary repricing budget and the new
independent target-floor gate. Maximum cold Reference residual is
**$0.0000314582211/share**, at most **3.14582211%** of the applicable raw budget.
All shape, parity, calendar, API-price and metadata checks pass. Strict 1e-7 IV
comparison has 30 diagnostic failures, separate from the approved price contract.

The frozen status-comparison gate remains explicitly **failed for two rows**:
27418697 and 27419115. Their previous targets and IVs were valid. The new targets
are respectively about $0.0000000889 and $0.0000000195 lower and cross the existing
zero-volatility classification band; cold Search correctly returns the reporting
floor. Both asks were among the fifteen proven floor conflicts. They are served
as truthful PriceOnly, not omitted or given artificial time value. An explicit,
pinned audit-only review contract is being implemented; the strict receipt and
original baseline remain unchanged. This candidate is not yet promoted.

The clock and audit share prediction SHA256
`892135920e749c697228fffce1e212b36c4385d47d3b6aff3bc0c02bb91ce676`.
Binary SHA256 is `188f0a8d6e4b70c568ffe62028a6fa22ded0b1b87e967e1b0586eaec3e84fc50`;
policy SHA256 is `ca9f258e5a7edcd059f0cb34293cda09603b36c05ffaefc3e82808ed5a373d28`.
See `acceptance/integrated-joint-feasible-v1-audit/summary-v1.json` and
`joint-feasible-v1-profile/manifest.json` under the r7 evidence root.

Measured remaining work, rather than estimates from the old partial-service arm:

| Work | Development time | Next implementation test |
|---|---:|---|
| Global joint calendar projection | 418.076 ms | Inspect sparse constraint/workspace/factorization costs; preserve objective and final feasibility |
| Family construction | 216.079 ms | Measure necessary domains and preparation reuse without warm-data assumptions |
| Native root acceptance | 164.589 ms | Avoid strict-IV work only where an independently bounded monetary candidate suffices |
| Paired bounded-family preparation | 165.071 ms | Test one bounded candidate evaluation with explicit floor/region proof |
| Fast candidate route | 238.224 ms | Improve candidate coverage and eliminate unnecessary inverse iterations |
| Residual checks after native acceptance | 67.465 ms | Retain tight-budget/cold fallback; 10,662 other checks already reuse endpoint proofs |

The global calendar bucket is charged to the first chain, not that expiry's
independent fitting cost. Certification totals 732.141 ms and contains the listed
inverse/check work; do not add that inclusive bucket again. There are exactly
12,866 uncached final identifications and 12,866 subsequent cache hits.

Integrated correctness receipts include 32 pricing, 29 quote/finalization,
67 serving and 47 oracle tests before the public mode; the public boundary then
passes nine CLI, 26 mode/final-price/budget and 17 archive cases. Two explicit
external-fixture unit tests are skipped; the full native API run above supplies
the complete SPY evidence. Owning PCH-off builds pass. Merge finding R1 is fixed
by owned/versioned withheld support; R3 unsafe widening is blocked by typed
archive refusal, with versioned persistence still to implement. R2 asynchronous
publication protection is in its separate reviewed development lane.


## Release candidate follow-up: 1.406 seconds and reviewed coverage

Source `45d9e19290a557761801624cee555e1e4ed50afe` has a fresh one-core,
one-worker ThinLTO measurement of **1.406041 s** native cold calibration and
**1.5786941 s** process wall time, including input/output. Measured SMT sibling
activity was 0%. This single trial remains above one second and is not the
100-process qualification. Its prediction bytes match the earlier 1.4140127 s
development result exactly.

The exact release binary also passes all thirteen correctness gates under
`raw-spread-price-v1` and the explicitly selected `reviewed-feasible-floor-v1`.
It serves all 33 valid expiries and 12,866 eligible rows, with 12,851 inside raw
quotes, 12,850 identified IVs and 16 PriceOnly rows. Maximum cold price residual
remains $0.0000314582211/share, 3.14582211% of its budget; no roundoff-only passes.
All target-floor and published shape/parity/calendar checks pass. The unchanged
strict status diagnostic still fails for exactly IDs 27418697 and 27419115.
[The pinned review](reviewed-identification-contract.md) independently verifies
those two changed targets and genuine floor classifications; it preserves the
original baseline, minimum count and strict failure, and never enters fitting.
Every other new PriceOnly transition still fails. All 57 Python oracle tests pass.

Exact joined evidence under `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`:

- `joint-feasible-v1-lto-profile/manifest.json`, SHA256
  `482b9587e7cbb031e94231a4274f1372ea9f33ee193955b1160e6e8a6df68430`;
- `acceptance/integrated-joint-feasible-lto-v1-audit/summary-v1.json`, joining
  the native report, reviewed verification, binary, policy and prediction hashes;
- release binary SHA256
  `072116ac1d1cef36b62fcca11aed0b1f04d2ac1f2a41b47b6256a1e280843ae2`;
- native audit SHA256
  `11ef55c3c4c53f36b1f221c4f22ccf7faf808ff32790f49226ca7245c82f94f1`;
- predictions SHA256
  `892135920e749c697228fffce1e212b36c4385d47d3b6aff3bc0c02bb91ce676`.

The historical native report field `served_rows` means IV-served rows (12,850).
The independent acceptance metric counts price-served rows including the 16
truthful PriceOnly contracts (12,866). Neither number silently drops refusals
from its denominator; see the report's explicit identifiability semantics.

R2 is now integrated at `3c63261e`: generation and owner-input revisions prevent
stale asynchronous workers and either phase of `serve_build` from overwriting
newer publications or resurrecting retired expiries. Follow-up coalescing keeps
the newest accepted intent even when the queue is full. Combined R1/R2 serving
verification passes all 77 cases, including zero-allocation reader tests;
`r2-integrated-serving.xml` is the root receipt. R3 remains a safe typed archive
refusal, not a claim of full versioned persistence support.

The [pinned public pricing comparison](public-american-comparison-20261024.md)
records 252 common-domain and 357 diagnostic contracts, inclusive cold/prepared
clocks and all failures. The internal reference is correlated; it establishes
neither independent general American accuracy nor a worldwide fastest claim.
Current implementation priorities remain measured sparse projection overhead,
monetary inverse acceptance with unchanged independent floor auditing, and
QuantLib comparison. Direct monetary proposals may deliberately return slightly
lower IVs within the approved raw budget; that experiment must not claim strict
1e-7 IV accuracy, global numerical monotonicity, or Greek accuracy.


## Economic inverse candidate: 1.120 seconds, complete service

The next source, `4b9d0e15411aa859818232f6cb7ad8c8c29fb9b1`, measures
**1.1200148 s** native cold calibration and **1.2711822 s** complete process
wall time on one fresh process, one worker and P-core 6 with its sibling idle
(0%). This is the development AVX2 build, not a ThinLTO or 100-trial qualification.
The exact measured binary passes all thirteen actual-API gates under the explicit
`raw-spread-price-v1` and `reviewed-feasible-floor-v1` contracts. Counts remain
33/33 expiries, 12,866/12,866 price-served, 12,851 in original raw boxes,
12,850 identified IVs and 16 PriceOnly rows. No target-floor failures or
roundoff-only price passes occur. Maximum cold Reference residual is
$0.0005012966094284366/share, 50.1296609428% of its original-quote budget.
The strict 1e-7 IV diagnostic is deliberately not the monetary acceptance gate.

One bounded family evaluation accepts 11,549 of 12,045 monetary proposals before
strict-IV enclosure work. These proposals preserve the empirical family and
monotonic-domain premises; the full independent cold price and floor audit remains
mandatory. Generic/default strict-IV semantics and the three-day family guard
are unchanged. The measured calendar projection costs 348.762 ms, family build
202.762 ms, and remaining inverse certification 523.551 ms (inclusive).
The 302 shortest-expiry Fast candidates still cost 131.681 ms. These buckets,
not the earlier partially served two-second arm, guide the remaining work.

Exact evidence under `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`:

- `economic-direct-v1-dev-profile/manifest.json`, SHA256
  `c2ae714a8fd91e7fe76c58a42c64b7e20640e3c00dde0892369efa008ea8eeda`;
- `acceptance/integrated-economic-direct-v1-audit/summary-v1.json`;
- binary SHA256 `bc0bdd6cd6632a030b3737bd770182750550dc751a555f9ea296be8a7198915f`;
- predictions SHA256 `0ef95f6506611a71eb9639bcdc19f3752376642b2cda78aa5f722e603071d490`;
- native audit SHA256 `f71d103e8d4872decf1b56d47dfc3bb343a3ab5c5cd4b95b62d63cd8c75e9d6b`.

The separate `spy-joint-feasible-economic-family-always-candidate.json` changes
only the existing adaptive family selection to `always`. Its complete API audit
also passes all thirteen gates; prediction comparison changes only 60 IVs on one
sparse expiry, with every price and status identical. Maximum residual and all
counts are unchanged. Its receipt is
`acceptance/integrated-economic-direct-family-always-v1-audit/receipt.json`,
policy SHA256 `781ddc09f7f26cfbfeb6c32edae1e3591b725a8f8d8d5b85d18eeda00b23f34a`.
This is an experimental policy pending a quiet timing comparison, not a default
promotion. It does not bypass the short-expiry family-domain guard.

The family instrumentation has now measured 3,264 tensor nodes (146.1602 ms),
549 cold validation probes (68.2893 ms), bounded probes (5.475 ms), envelope
construction (3.7783 ms) and geometry (0.9676 ms) in a separate unchanged-output
profile. These are diagnostic subspans from that run, not additional costs to
sum with the newer whole-fit totals. Batched cold probes and sparse-factor
workspace reuse remain experiments until complete correctness and measured gains
are established. No subsecond or world-leading claim follows from this result.


## Current implementation queue and independent pricing frontier (2026-10-24)

The existing family=always candidate has a second, valid quiet development trial:
1.1198819 s native and1.2759514 s process wall, sibling0%, predictions matching
its all13-gate audit. The sparse60-row expiry now costs7.795ms including7.143ms
family construction, versus26.165ms in the earlier inverse-candidate stage.
Whole-fit timings overlap; this is not a statistically established overall gain.
The first trial (1.4844s, sibling8.333%) fails the predefined idle gate and is
retained, not removed. See economic-direct-family-always-v{1,2}-dev-profile.
Both the1.120 development binary and1.406 release binary are now archived under
SHA256 in the r7 evidence root's binaries directory before subsequent rebuilds.

The pinned six-profile public comparison adds useful intermediate choices:
Screen15.546us and accurate Fast58.351us per cold put on the same252-contract
common domain, both252/252 within the synthetic0.001USD price budget. Across
357 diagnostic contracts each has350 passes and7 explicitly unsupported
q<r<0 double-boundary cases. Screen still has8 failures and39 unresolved cases
at0.0001USD on that broad set; the user-approved SPY spread-scaled budget remains
separate. Tight reference disagreements are not passes. See
C:/atx-wt/pool-3/atx-vol/docs/quantlib-american-frontier-20261024.md and its hashed
manifest. Both original and follow-up measurements are retained; the close
Reference-versus-QuantLib-fast ranking reverses across runs, so balanced-order,
multi-process comparisons are required before a durable ranking claim.

| Work package | Current evidence and next action | Promotion gate |
| --- | --- | --- |
| Complete cold fit | Early Fast residual proposals are source-reviewed; original full search remains fallback. Screen proposals are a separate experiment. | All13 actual-API gates on exact final binary, same raw price budgets, all33/12866 served, then quiet ThinLTO trials. |
| Calendar memory | Cached/packed factor prototype has exact factor/solve/output parity but no convincing speed gain; retain it as an unpromoted experiment. Compact stored constraint rows112?64bytes is being tested with original Eigen. | Exact objective/constraints/output parity, contradiction diagnostics, full fit latency. |
| Calendar termination | Full unchanged publication certificate first passes at sweep35 of36; only one iteration is potentially removable. | Keep current stopping rule; no semantic weakening for this small opportunity. |
| Family probes | Three quiet replay processes save only10?13ms with151 mandatory scalar retries on549 probes. | Deferred; original scalar validation remains in production. |
| Public pricing | Six-profile continuous-carry comparison measured with archived v2 executable, complete denominators and explicit reuse costs. | Independent method refinement, balanced process/order repeats, calls and difficult-regime expansion. |
| Independent reference | New offline exact-discrete-LCP reference passes5 prerequisite tests and peer review;35 selected cases retain six separate time/space/domain grids. | Resolve each price by declared empirical refinement plus independent checks; no rigorous error bound or blanket oracle label. |
| American analytics | Current live serving uses sigma/q family edge typing. Public sigma-only Greek bundle has documented near-edge theta errors; caller audit/reproducer active. | Explicit validity/refusal or validated fallback per public output, then same-model price/Greek corpus and qualified timing. |
| Cash dividends | Existing lattice routes remain distinct from continuous-carry and aggregate-forward fits. | Dated-event reference, independent jump/exercise convergence, then amortized family/batch optimization; no continuous-yield substitution in scoring. |
| Publication/archive | R1/R2 serving fixes integrated and77 tests pass. R3 rejects unsafe unpersisted record versions. | Full versioned archive persistence remains outstanding; typed refusal is a safe interim closure. |
| Final qualification | No subsecond qualification, default promotion or main merge yet. |100 distinct fresh-process one-core/one-worker successful trials, every cold fit<1s, frozen input/policy/binary, idle sibling, exact joined correctness receipt. |

The independent PDE pilot explicitly remains insufficient for the tighter public
reference disagreement: long-maturity80%-volatility cases have empirical
indicators around0.00036USD. Its two European control groups differ from closed
forms by at most3.82e-6 and4.19e-6USD. Finite double-boundary PDE results do not
create shipping double-boundary support. Greek and cash-dividend accuracy remain
separate requirements; a price residual cannot certify a hedge sensitivity.


## Release follow-up: Screen proposals and compact projection rows

Source `126447ef614ce5c1f7783eaa5973566eafa91a8d` passes all thirteen
actual-API gates with the family=always economic policy. Three fresh one-worker,
P-core6 ThinLTO processes measure native cold fits of
1.0288362, 1.0897724, 1.0305445 seconds;
median 1.0305445 s and maximum
1.0897724 s. Every SMT-sibling reading is0%.
Process wall medians are 1.1828621 s including
I/O and startup. All results remain above one second. This is not the required
100-process qualification. Development measured1.0338457s; all release and
development predictions are byte-identical.

Coverage remains33/33 expiries,12866/12866 rows,12851 inside original quotes,
12850 identified IVs and16 truthful PriceOnly rows. All price/shape/parity/
calendar/floor checks pass. Maximum cold Reference residual remains
0.0005012966094284366USD/share,50.1297% of its immutable raw-spread budget;
no roundoff-only passes. Strict identification still exposes only the two
explicitly reviewed changed-target floor transitions. Price and status bytes
are identical to the preceding family=always candidate;551 IVs change.

The quiet development profile records684 cold-checked Screen acceptances from
700 candidate attempts. Fast-only early-stop counters are zero; the16 remaining
refusals retain typed fallback. Calendar364.361ms, family213.195ms and
certification408.026ms explain the remaining cost; certification includes
candidate108.856ms, strict native62.054ms, price checks56.857ms and prepared
seed checks23.770ms. Historical `reimply_fast_*` fields are aggregate aliases;
new `reimply_candidate_*`, `reimply_screen_*` and Fast-only proposal subsets
make attribution explicit. All15 pricing and47 identification/final-price/native
route tests pass; two external fixtures are skipped in that unit run and the
full native API audit supplies their complete SPY evidence. Owning counter/source
exporter PCH-off checks pass.

Exact r7 evidence:

- `screen-proposal-compact-v1-lto-profile/manifest.json`, SHA256
  `197aedc99cf7e413992aac77d1bb0c8bb9a97af579c685e8028a5efb34c4e260`;
- `acceptance/integrated-screen-proposal-compact-lto-v1-audit/summary-v1.json`;
- release executable `78e64e2c7c69ce5f2125952304ff251e78b18349920ea0560bfc5c74c42a3e0b` archived under `binaries/`;
- native audit `ecc32477bc9da11f41254d75c67f225785152e98a8056b32964c56ed195c26d1`;
- predictions `04f13351165dfb48943aa19022976100c311ebcfbaae6271e24abed0534db9a3`.

Next work is measured family tensor preparation and attribution of the remaining
bounded-candidate refusals. The calendar ordering/cached-factor experiments did
not beat the existing solver and stay unpromoted. Public Greek validity repair
and independent reference refinement continue separately. No default promotion,
main merge, full Greek/cash qualification or worldwide speed claim is made.


## 2026-10-24 integration checkpoint: exact cold floor reuse and Greek findings

The geometry/carry release at numerical source `a0af95ab` passed all 13 actual-API
SPY gates, with the same prices, classifications and prediction hash as
`126447ef`. Three quiet ThinLTO fresh processes took **1.0180031, 1.0411881 and
1.0494917 seconds** (median **1.0411881**, P-core6, one fit worker, SMT sibling0%).
Those measurements do not establish an overall improvement over the preceding
1.0305445-second median; the isolated geometry saving must not be advertised as
a complete-fit gain. Every trial still exceeds one second. Evidence:
`geometry-carry-v1-lto-profile/manifest.json` (SHA256
`049dc088446792606246ae4dde6bf7c85713567d9dd01d1c3b7c781dda896462`),
archived binary `1eb11005515b0a0c3bc1f1101b7d703c8a0bb491745e3fa191c7d93658ce8878`.

The next reviewed numerical component, integrated as `42333054` from
`234b31fe`, accepts 496 previously refused bounded-family proposals only after
the original complete cold price-seed proof. Their reporting-floor boundaries
share 11 exact models; retained owned handles preserve cold price bits, with no
rescaling, relaxed floor criterion or new reference budget. Preparation and
consumption validate model/options/request/budget and family identity separately.
The integrated full SPY audit passes all13 gates: 33 valid slices,12,866/12,866
eligible prices,12,850 IVs plus16 truthful PriceOnly outputs,12,851 inside raw
quotes (99.88341365%). Maximum cold price residual is still
$0.0005012966094284366, with no roundoff-only pass. All fitted prices and statuses
are unchanged;496 IVs change. The new prediction SHA256 is
`e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`.

The archived development executable is
`8be86d832ad116f02de8160f25653991caec81528523c1423dcc682a495cd5d6`;
`acceptance/integrated-cold-floor-v1-audit/verification-reviewed-v3.json` joins
its receipt, raw input and unchanged Always-family policy. Integrated focused
checks pass21 pricing tests and29 fitting tests;5 external opt-in fixtures are
skipped in that unit invocation, while the full API audit checks the complete
SPY input. This audit is correctness evidence, not a quiet latency measurement.
Next: measure the merged release and investigate identical floor work in the
remaining ordinary seed/Screen candidate checks, retaining every cold proof.

The complete715-contract Greek diagnostic is now frozen and independently
reverified. It retains25,740 field requests and17,103 available estimates; no
Greek accuracy budget is inferred from the monetary price budget. Cold AL theta
is wrong near exercise (+8.79997 versus refined calendar -0.337363), and a deeply
exercised call returns +12.99997 instead of the refined zero. A crossing-only
patch misses the latter. Fixed-step FD also has a short-expiry theta discrepancy
of5.39967 and a gamma discrepancy of0.0182586. Reference instability and all
unsupported fields remain in the denominators. See
`docs/american-greek-full-corpus-20261024.{md,json}` and the10 passing hardened
runner tests, including228 independent European price checks.

The next analytics implementation uses solved exercise-boundary evidence and
field-specific stencils, preserving a valid price when a derivative is refused.
A shared additive requested/supported/reason schema has passed review, but
producer, consumer, batch and Python propagation must land together before
calling the migration complete. Adaptive derivative refinement follows
separately. For the independent PDE oracle, price convergence alone does not
qualify Greeks: [in 't Hout's numerical study](https://arxiv.org/abs/2401.13361)
finds problematic Delta/Gamma convergence for Crank-Nicolson even with Rannacher
smoothing and studies DIRK/Lobatto alternatives. That research motivates an
explicit derivative convergence experiment, not an automatic solver switch.

Cash work continues from the existing D0/D1 implementations and their actual
22,009-contract evidence. `docs/cash-reference-frontier-review-20261024.md`
defines current-model rescoring, exact dividend-event alignment, independent
comparison and shared rollback experiments. Richardson drift is an empirical
indicator, not a conservative error bound by definition.

The user authorized a local-main merge at this checkpoint. Main's newer
bindings/storage work was reconciled in `725e36a7`; changelog/ledger conflicts
retain both histories. The new complete-enum gate caught the omitted
`ProjectedPrice` Python enumerator, corrected and independently reviewed in
`a1ca7d14`. Combined storage/Python/build and post-merge SPY checks are in progress.
Local main has not yet been advanced at the time of this entry. Unrelated main
worktree changes must remain intact. Default policy promotion,100-process
subsecond qualification, full archive persistence and broad Greek/cash/frontier
qualification remain open.
