# SPY qualified cold fit, r7

Status: implementation in progress; no qualified sub-second result.

Current integration checkpoint, 2026-10-24: local `C:/atx` main contains
`96cd56db`, with all pre-existing files preserved. New Greek/cash components are
integrating in pool-3; later pending-merge statements are historical.

The last ordinary-backend control passing its sibling gate takes **1.3226988, 1.4265373 and
1.4471122 seconds; median 1.4265373**. All three fresh processes use one worker
on P-core 6 and pass the sibling-activity gate. Archived binary `6afb2de0` includes
the finalizer floor cache and passes all 13 actual-API checks: 33/33 expiries,
12,866/12,866 eligible rows, 99.88341365% raw containment, 12,850 identified IVs
and 16 truthful PriceOnly results. Maximum cold Reference residual remains
$0.000501296609/share under `min($0.001, 1% of original spread)`.

The full double block-factor prototype also passes all 13 correctness gates.
Its two timing sets are 1.1301671/1.3739241/1.1918033 seconds and
1.6014335/1.2003743/1.1195960 seconds, respectively. The first set has one
sibling-activity failure and the second has three; neither qualifies a speedup.
All runs and denominators remain recorded. No candidate run is below one second.

The earlier 1.3944879-second ordinary-backend median, historical main median
1.0989175 and earlier 1.03-second sets remain evidence of their own runs.
They are not replacements for the latest clock or proof of a source regression.
Subsecond and 100-process release qualification remain open.
Whole-host qualification correction: that ordinary control recorded 27.4-28.7%
system activity, so passing its sibling gate did not establish a quiet host.
A newer six-run paired original/block experiment retained all results and failed
the declared system-load gate on every run (18.6-38.2%); three also failed the
sibling gate. Its medians, 1.2948967/1.2080157 seconds, cannot qualify a speedup.
The independent Windows aggregate-accounting cross-check agrees with the native
measurement. See `qp-paired-clock-v1/` and the block-factor research report.

Finalizer floor reuse removes 265 boundary solves with identical predictions.
FP32 replay is rejected; the full double block-factor prototype passes every API
correctness gate but remains unqualified for timing and production deployment. Boundary Greek admission passes the complete715
rerun with all prices unchanged; portfolio, derivative-book, backtest and Python
availability fixes are reviewed and integrated. Full available Python: 801 pass,
245 environment skips, one existing strict expected failure. Native surface and
served-session consumers and short-time derivative accuracy remain open. Cash
guards, exact50-case manifest and default-off full PDE batch driver are integrated;
all150 original outcomes are bit-identical. Three-grid accuracy evidence retains
43/44 pass plus1uncertain at finest; inclusive batch timing remains unrun.
See [current exact-binary evidence](../docs/spy-cold-acceptance-r7.md).

## Acceptance contract

One build and one policy must simultaneously deliver a fresh-process SPY cold fit on
one physical core in less than one second, at least 99.5% bid/ask containment, service
on every valid positive-time expiry, and no arbitrage in the representation clients
actually query. Do not combine timing, coverage and accuracy from separate policies.
The target is calibration; ready-to-quote construction and risk remain separate,
explicitly reported clocks. No warm-cache substitution, hidden parallelism, reference
labels as fitting inputs, skipped validation, or reclassification of solver failures
as invalid data.

**User-approved accuracy revision, 2026-10-24:** the calibration acceptance gate
now bounds the cold Reference repricing error of each reported point IV by
`min(0.001 USD/share, 0.01 * (raw ask - raw bid))`. The user explicitly selected
this budget to allow performance improvements while keeping errors economically
small. Use the tightest budget across duplicate rows at the same node/side;
do not substitute a spread floor, repaired boxes or widened quotes. Zero-width
quotes allow no economic error budget. Only a separately reported floating-point
roundoff allowance is permitted. Preserve and report raw inputs and the budget
in the acceptance evidence.

Absolute IV error of 1e-7 is now a diagnostic, not a promotion requirement.
Retain the failed strict audit below as historical evidence. Report low-vega
conditioning and IV uncertainty rather than implying that a small price residual
guarantees a precise IV. Greek accuracy needs separate tests; this revision does
not establish accurate risk or relax price-surface arbitrage, 99.5% containment,
all-expiry service, one-core execution or the sub-second timing requirement.
Production acceleration is explicitly opt-in with zero/off defaults and must
check its candidate against this price budget, retaining a reference fallback.

Re-scoring the frozen wave-1 actual-API export under this approved contract
passes all 12,183 identified rows, with zero economic-budget or roundoff-only
failures. The maximum residual is $0.0007096053236921307/share and maximum
budget utilization is 70.96053237%. All 683 typed PriceOnly rows remain served;
none were reclassified for this result. The existing all-33-expiry, 12,866-row,
99.88341365% containment and price-constraint checks still pass. Evidence:
`acceptance/integrated-full-wave1-audit/verification-economic-v1.json` and
`cold-node-prices-v1.json`. This changes the approved correctness criterion,
not the measured runtime: the 22.1222-second development profile remains
unqualified and far above the target. Its 1,026 strict-IV misses remain visible.

Frozen starting point: local main `a53c48c4a3c404fe63e24f07f1d574a2a015ed38`.
Integration worktree: `C:/atx-wt/pool-3`, branch
`feat/vol-spy-qualified-r7-20261023`. Input:
`C:/atx-cache/quote-benchmark/fit-timing-20261017-cohort5/input/quotes-SPY.csv`, SHA256
`b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e`.
Run evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023`.

## What the old clocks measure

| Arm | Cold wall | Service | Interpretation |
|---|---|---|---|
| K4b native batch | 2.473 s median, 10 quiet trials | 4/33 expiries, 1,410/13,168 raw rows | Low coverage; not the requested fit |
| C4 in-box, h0 | 63.503 s median, 53.310 s best, 10 development trials | 26/33 expiries, 10,068/12,866 eligible rows | Better containment, still incomplete |

The C4 policy lacks the K4b family/native/batch flags. Its final-grid stage median is
42.582 s, while the in-box projection itself is approximately 0.2 s across 18
expiries. Every moved price is scalar-inverted during publication and again during
query alignment, then may be inverted again in calendar passes and validation.
These integration costs, rather than the small projection alone, explain much of
the gap. Stage medians do not add to an actual trial total.

The prior C4 narrative rounded containment to 99.50%. Replaying its saved CSV gives
10,026 contained / 10,068 served = 99.5828367%. The new gate remains at least 99.5%
on the fixed valid denominator; dropping the difficult rows cannot improve it.

## Independent validity and feasibility

The raw corpus has 34 expiry groups; epoch-day 20740 has zero calendar time. The
other 33 contain 12,866 rows. An independent exact-box LP over all 33, including
20742, 20896 and 20875, is feasible at the common snapshot spot 773.0 for the
existing finite-domain shape/parity/calendar certificate. The audit uses both
sides and all-pair calendar constraints at union knots. The initial 12,867-variable,
307,954-constraint witness had a 4.66e-10 USD residual, but used overly loose cash
bounds. Repeating it with the fitter's effective-carry bounds remains feasible
for that existing certificate; the first witness is superseded, not qualification.

Adding the stronger necessary European terminal price floors makes the full
exact-box problem infeasible: 15 raw call asks (4 in expiry 21533 and 11 in 21568)
are below the model's floor, by as much as 1.0099945 USD. The target allows at most
64 of 12,866 valid rows outside their quotes. The corrected v3 witness raises only
those 15 individually contradictory asks to the model floor plus a fixed numerical
allowance, leaving all bids and every other ask unchanged. It attains exactly 15
raw-box departures, or 99.88341365% containment, with a 4.66e-10 USD maximum
constraint residual. These 15 departures are necessary by the individual floor
bounds and sufficient by the witness under these constraints. The largest is
0.2524986256 full spreads above ask, within the existing one-spread distance gate.
Continue serving those contracts at valid prices and report every raw-box departure.
Do not drop them from the denominator or silently widen an exact-box policy. The
earlier all-33 witness does not prove full financial no-arbitrage.

There are no zero-width bid/ask boxes in these SPY rows. In expiry 20742, calls at
K800 (ask), K801 (bid), and K803 (ask) all equal 0.00999999977648; shape constraints
force a flat segment and maximum uniform interior slack zero. This is a feasible
boundary, not proof of bad data. The former 31-expiry positive-slack claim is not
the full coverage denominator. An LP that lets slack go negative permits widened
boxes and cannot establish exact-box infeasibility from that negative solution.
The independent oracle is a feasibility witness, not the production fitter or
evidence of pricing-model accuracy outside the tested representation.

Expiry 20875 has two same-strike supplied carries differing by 4.656612873e-10.
Exact equality rejects its entire 294-row expiry. The yield-only forward difference
at the common spot is approximately 1.3435e-7 USD. Recovery must use a generic,
explicit input-normalization budget and preserve raw quote boxes and provenance;
there is no expiry-specific exception.

## Work packages and integration order

| Owner | Work | Required evidence |
|---|---|---|
| acceptance / pool-33 | Frozen validity manifest, independent price/query oracle, native audit export, combined policy | Adversarial verifier tests, all 33 included, all-pair union-grid checks, audits explicitly excluded from timing |
| repair_iv / pool-4 | Identify changed prices once; retain qualified native inverse context; batch changed nodes | Counter-based duplicate-work regression, query/row agreement, typed floor behavior, stale-model refusal, scalar fallback and archive behavior |
| joint_calendar / pool-5 | Coupled sparse QP for exact boxes, both sides, shape, parity and calendars, including boundary solutions | Synthetic feasible/infeasible/boundary cases; full primal residual checks; no solver exhaustion called infeasible |
| integrator / pool-3 | Bounded carry normalization, schedule joint repair once, integrate and profile; bounded-family kernel only where the correct candidate requires it | No material carry acceptance, deterministic normalization, one final identification per changed value, matched-source measurement |

1. Freeze the independent contract before optimizing. Review new source commits at
   their exact SHA before integration. Preserve default policy behavior.
2. Remove duplicate scalar identification while preserving PriceOnly versus Legacy
   semantics. A first regression measures 120 versus 60 Newton iterations and 78
   versus 39 Andersen-Lake solves; the optimized path must match one query per
   changed contract and do no inversions on an unchanged follow-up pass.
3. Resolve all 33 with a coupled boundary-capable projection. Avoid fitting,
   publishing, re-inverting and then throwing away the same trial repeatedly.
   Only declare infeasibility with a valid certificate; otherwise return explicit
   non-convergence and retain the failure in the coverage gate.
4. Reuse immutable family/native inversion state for repaired nodes and queries.
   Keep qualified price-error/root acceptance contracts, fallback at unsupported
   domains, and model-identity validation. Do not assume a local vega proves a
   one-point root enclosure.
5. Run the actual-query audit on the combined candidate. Test monotonicity,
   convexity, end slopes, affine call-put bounds, quote containment, and calendars
   at every relevant knot and between-node probes. A piecewise-linear certificate
   is checked at the union knots, including every overlapping expiry pair.
6. Profile this same correct candidate. Batch bounded-family prices, reduce
   reference evaluations and share interpolation work only where the profile
   shows remaining cost; never count early refusal as a successful optimization.
7. Build `vol-rel-avx2`, stop competing builds, pin a physical P-core with its
   sibling idle, set one fit worker, and launch fresh processes. Record source,
   binary/input/policy hashes, affinity and trial distribution. Development
   correctness builds use `vol-rel` and are not latency evidence.

## Mathematical design references

The coupled quote problem is a convex quadratic projection under affine
inequalities. Primal feasibility, optimization convergence and infeasibility are
different claims. The implementation uses the residual/KKT framework described
in Boyd and Vandenberghe, [Convex Optimization](https://web.stanford.edu/~boyd/cvxbook/bv_cvxbook.pdf),
chapters 4, 5 and 11. The independent feasibility witness does not require the
chosen optimization algorithm to have a strictly feasible starting point.

For constant effective carry, a proposed normalization bound follows by coupling
the two geometric-Brownian stock paths and applying the one-Lipschitz call/put
payoff at each bounded stopping time. For effective yields q_low <= q_high,
the absolute American price difference is at most

`S * exp(max(-q_low, 0) * T) * (1 - exp(-(q_high - q_low) * T))`.

This is our analytic derivation, independent of the numerical American solver.
Use the model's effective carry after the calendar/variance-clock and aggregate
PV conversion. It does not establish a bound for the dated-cash lattice; that
path must refuse conflicting carries. The canonical midpoint lies inside the
extrema, so the full-range bound also bounds each input-to-canonical change.

## Completion receipt still required

- Carry normalization: `c1d942d4`, reviewed by acceptance. Quote-slice ctest passed;
  all 3 new cases pass with PCH disabled. Owning PCH-off targets compile cleanly.
  Umbrella has 3 existing failures: README Tier-B count 38 versus 50 and fixture
  path-audit findings in unchanged files. All failure-source files and README are
  byte-identical to the frozen base. Seven other umbrella cases passed. This is
  not a claim that the full release suite is green.
- Duplicate identification: `c1c7f535`, reviewed by integrator; 60 focused tests pass.
- Coupled solver core: `abc95093`, reviewed by integrator. Native projection of
  the corrected v3 fixture certifies all 33 slices/12,866 prices in 40 iterations,
  exactly 15 departures; development test wall 471 ms includes JSON and repeated
  certification and is NOT a full fit or a qualified performance measurement.
- Native repair inversion and published-node reuse: `ee94ee72` + `16c7102e`,
  reviewed by integrator and acceptance, integrated as `4ddd9abf` + `d0bde8ad`.
  The final stack includes `b5e1378b` (integrated as `23065059`) to invalidate a
  published identification when its public PriceOnly bit changes. All 66 focused
  author tests pass; integrator ran 86 tests on the preceding integrated stack
  plus the new sticky-mask regression after rebuilding. A cached Search root is
  not an adequate accuracy oracle for the cold kernel: one comparison differed by
  1.44e-7, while direct cold-kernel endpoint prices bracketed the native root.
  Use tight bisection of the declared cold forward map for that accuracy test.
- Global repair scheduling: `a3895c53`, reviewed by calendar. Attempt one coupled
  projection of every served expiry before the existing calendar retry ladder.
  A regression proves feasible crossings are repaired without expiry refits;
  27 focused tests pass. Refused projections retain the existing fallback.
- Acceptance review caught a false timing pass: one native 50-second trial could
  be presented as 100 half-second trials by a contradictory launcher summary.
  `cd88e5c6`, independently reviewed by repair-IV and integrated as `afe8fc23`,
  now requires actual trial count, native clocks, process creation identities,
  affinity and prediction evidence to agree. All 20 verifier regressions and
  3 native CLI tests pass in the integrator tree. The latter invoke this tree's
  explicit executable with `pytest --noconftest`; no Python extension is used.
  A bare pytest run correctly rejected the host's editable extension from a
  different checkout. This was a benchmark verifier defect, not an observed
  production fit speed.

No release or main integration is claimed here. Append the exact reviewed source
commits, tests, full SPY oracle result, before/after stage and solve counters,
quiet one-core timings, and outstanding general-product limitations. A goal pass
requires all four acceptance conditions in one candidate; a speed improvement
alone does not close this sprint.

## Complete-fit baseline at 2362f6aa

Both development and ThinLTO actual-API audits pass on the frozen SPY input:
33/33 valid expiries, 12,866/12,866 eligible rows served, 12,851 inside the original
boxes (99.88341365%). The 15 departures are exactly the independently established
terminal-floor conflicts. The audit checks 6,433 price nodes and 62,494 actual
queries, with zero shape, parity, calendar, raw-model or between-node terminal
floor violations. This certifies the declared finite-domain PWL price surface;
it is not a proof of a full continuous-time arbitrage-free American option model.

The exact release binary is
`876a4cf8bbd6ce57b31039e36574051a93853f81e634c476020dcd6b8334eafa`.
Its predictions match the development audit byte for byte:
`2be0eb424dbd04e0a9173c7f5997d6e5b7c67e8c8d2435f54792af75878568fc`.
Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`;
the release audit is `acceptance/integrated-full-v1-lto-audit/verification.json`.

Three fresh-process `vol-rel-avx2` trials, pinned to P-core CPU 6 with one fit
worker, measured 22.3678407, 22.9317113 and 20.7058787 seconds. SMT sibling busy
fractions were 13.2774%, 4.5455% and 0.5963%; the first exceeds the 5% limit, so
the three-run set fails the quiet-run gate. The manifest is
`integrated-v1-lto-3/manifest.json`. These establish an approximately 21-23 second
complete-fit baseline, not a qualified latency distribution or a sub-second pass.
The independent actual-query audit is intentionally outside timed calibration.

For comparison, the old 2.4727-second median served only 4/33 expiries; the old
53.310-second best C4 trial served 26/33. Neither is a complete-fit baseline.
The launcher report's legacy `served_rows=12183` counts identified-vol rows only:
the independent acceptance denominator includes the additional 683 explicitly
typed PriceOnly rows. PriceOnly counts are 16 floor, 610 low-vega and 57
unidentified-interval rows, with no fabricated point volatility.

The development profile (`integrated-v1-profile/trial-001.json`) attributes
9.645 seconds to per-slice certification and 8.775 seconds to global calendar
processing, predominantly repaired-price identification. It records 21,066
native repair attempts, 20,818 accepted roots, 248 native fallbacks and 48,341
Andersen-Lake price calls. Its 12,805,296 premium evaluations are quadrature
evaluations, not that many complete American prices. This profile guides work;
its non-LTO timings are not performance qualification.

The next implementation wave is therefore:

1. Batch repaired-price lane preparation and acceptance, retaining every native
   bracket/error test and typed fallback, and serialize separate timing counters.
2. Fuse bounded-family price/envelope evaluation and expose a reusable batch
   interface, with stale-envelope identity checks and scalar retry on uncertain
   decisions. Measure the actual reduction before claiming SIMD gains.
3. Prepare bid/ask inverse lanes only when a channel solve requests a band.
   The baseline performs 31,874 prepared lanes despite zero channel solves;
   midpoint results and the original carry used to seed edges must be preserved.
4. Evaluate Fast confirms under their existing explicit price-error bound and
   avoid identifying prices that an imminent global projection will replace.
   These remain hypotheses until implemented, independently reviewed and audited.

Every optimization must retain all 33 slices, the immutable quote denominator,
the containment threshold and the actual-query constraints. The remaining
performance gap is over twentyfold; no measured result yet supports a claim
that these changes alone will close it. All 100 required quiet cold trials must
pass on one reviewed candidate before the goal can be marked complete.

## Wave 1: complete prices, failed cold-IV accuracy

The combined candidate `47b2037572690d60ce87113c37340c75ac129913` includes
repaired-node batches, lazy bid/ask preparation, configured Fast confirms,
fused bounded-family prices and separate fallback stage counters. Its development
binary SHA256 is `ee32307bdd29ef4a302364ddba520d148e5c1a126209dc3e8ccdffd6f8fc01da`.
The integrated suite passes 310 quote-slice tests and 40 targeted pricing tests;
two explicitly opt-in fixture/probe tests and one child-process entry are not
part of those passing counts. These tests do not replace the full-input audit.

The expanded actual-query audit **fails the accuracy gate**: of 12,183 reported
point IVs, 11,157 strictly straddle their published price under cold Andersen-Lake
evaluations at sigma +/- 1e-7, and 1,026 do not. There are no roundoff-only cases.
The maximum center price residual is $0.0007096053236921307. All 33 expiries,
12,866 eligible rows, 99.88341365% containment, price-shape/parity/calendar checks,
and exact API/CSV status/reason/interval/convention checks still pass. The 683
explicit PriceOnly rows are counted separately; converting failures to PriceOnly
solely to pass this audit is not an accepted fix.

Evidence is `acceptance/integrated-full-wave1-audit/verification.json` and
`cold-node-roots.json` under the same evidence root. Predictions SHA256 is
`681c3b44e3edbd799b90b03ccfaad2564d98a580085ae4131093cf749e08a8cf`.
Compared with the complete baseline, only `predicted_vol` differs (8,160 rows);
published prices and all other CSV fields are unchanged. The old audit did not
perform these cold-IV checks, so its price-surface PASS is not an IV accuracy PASS.

One development profile took 22.1222 seconds with 8.57% sibling activity and
therefore fails timing qualification. It identifies the dominant work:

| Stage | Calls | Elapsed seconds | Boundary solves |
|---|---:|---:|---:|
| Typed Search fallback | 5,274 | 13.5995 | 26,172 |
| Repeated plain solve after NonMonotone refusal | 1,253 | 4.6228 | 4,262 |
| Native acceptance/confirm | 21,066 | 0.3073 | 4,014 |
| Plateau checks, including 51 edge searches | 2,546 | 0.4405 | 2,546 |

Native batch lane preparation (0.0589 seconds) and bounded-family preparation
(0.4209 seconds) are separate from native acceptance. Initial endpoint work is
0.4964 seconds. Guard misses are 1,086 without a matching context, 472 outside
the retained q-family coverage and 3,468 outside the native price-domain gate.
The last count refers to the near-floor/nonmonotone price window, not a sigma
interpolation range that can simply be widened. The first chain's recorder
includes the whole joint calendar operation; it is not an expiry-local profile.

The next gate is accurate cold-map identification, with regressions from the
exported failures. Reuse the already-computed plain candidate to remove the
duplicate NonMonotone solve without changing outcomes. Then repair the cold-root
contract and optimize certified fallback work; `scalar_answer_certified` currently
permits a 1e-4 Newton correction within its band, while the acceptance target is
1e-7. Warm-boundary Search drift must also be checked outside that band. No speed
claim closes this correctness blocker, and no full accurate-fit latency has yet
been established.

## Economic candidate audit and next measured bottleneck

The user-approved `raw-spread-price-v1` contract supersedes strict +/-1e-7 IV
straddling as the promotion gate; the historical strict-IV failures above remain
diagnostics. Frozen source `0a5b2bc1` passes all 12 independent correctness gates:
33/33 valid expiries, 12,866/12,866 eligible rows served, 12,851 inside original
quotes (99.88341365%), 12,430 identified IVs and 436 explicit PriceOnly rows.
No previously identified row was demoted; 247 were recovered. Prices are unchanged
from wave1. All identified IVs meet the original-row monetary budget, with maximum
cold Reference error $0.0007096053236/share. Strict-IV failures number 1,271 and
are not hidden; Greek accuracy is still unqualified.

The same binary took 13.0317839 seconds in one fresh development run on P-core 6,
one fit worker, 3.42385% measured sibling activity. This is not a ThinLTO/100-trial
qualification. Exact source/binary/policy/prediction hashes and test receipts are
in `docs/spy-cold-acceptance-r7.md`; evidence is `economic-v1-profile/` and
`acceptance/integrated-economic-v1-audit/` under the r7 evidence root.

| Repaired-price stage | Calls | Seconds | Disposition |
|---|---:|---:|---|
| Fast Search candidate plus cold check | 4,912 | 6.1814 | 4,189 accepted; still iterative |
| Historical typed Search fallback | 723 | 3.2821 | Remaining hard/flat cases |
| Native acceptance | 21,066 | 0.2816 | 20,818 native roots |
| Native monetary-budget verification | 20,818 | 0.4115 | 855 required cold prices |
| Plateau classification | 1,998 | 0.2907 | Includes 51 edge searches |
| Batched family preparation | 122 batches | 0.3765 | Separate from native acceptance |

Initial endpoint work is 0.2065 seconds; family build/validation is 0.2789 seconds.
The 25,730 uncached reconstruction requests include intermediate slice and final
joint projection. Global calendar work is charged to the first recorder, so its
large time is not the cost of that expiry alone.

Execution order now:

1. Project original paired quote prices jointly before IV fitting. The reviewed
   private draft already projects all 33/12,866 rows with exactly 15 unavoidable
   terminal-floor departures, in 40/100 bounded iterations and no American IV or
   price calls. It does not yet publish a successful fit.
2. Classify each final unique node side once, build the genuine final OTM IV curve,
   retain exact query identities, and atomically publish only after all final checks.
   Any unsupported/failed attempt falls back for the whole requested surface with
   an explicit reason. Raw quotes remain unadmitted midpoint observations.
3. Replace expensive candidate Search where a batched family seed passes the same
   cold price/exercise/slope checks. This is a separate monetary-budget route; do
   not widen the existing strict native root certificate by assertion.
4. Repeat the complete economic/API/no-demotion audit and profile the resulting
   candidate. Qualify 100 quiet fresh one-core ThinLTO runs only after the measured
   complete fit is below one second. None of these steps is a promised speedup.

## Expanded long-running mandate, 2026-10-24

The user explicitly requested continued autonomous subagent development and web
research toward the fastest American equity-options library, including fundamental
algorithm changes when local tuning stalls. The subsecond complete-SPY gate remains
the first execution milestone; satisfying it does not finish general library work.
No further preference question is needed for routine implementation choices.

The broader performance claim must be grounded in reproducible comparisons with
pinned public implementations under matching models, error budgets and hardware.
Private HFT implementations cannot be exhaustively ranked from public evidence.
Keep separate results for cold isolated contracts, amortized batches and family
construction, whole-surface calibration, live update/serving, and price-plus-Greek
bundles. Include difficult exercise regions, short maturities, signed carry and
dated cash dividends, with typed unsupported regimes instead of silent substitution.
Cold setup, fallback costs, failed attempts, allocation and tail latency belong in
their respective measured scopes. Optimizing only accepted fast-path requests is
not an acceptable overall performance claim.

The master sprint plan retains the general pricing/analytics and publication-race
work packages. New primary-source research and comparator recipes will be tracked
alongside exact-source correctness and performance receipts. A faster prototype
that lowers frozen identification or economic correctness remains a failed
promotion candidate until its algorithm or objective is fixed.

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
[The pinned review](../docs/reviewed-identification-contract.md) independently verifies
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

The [pinned public pricing comparison](../docs/public-american-comparison-20261024.md)
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


## Qualification update: validity and rejected experiments (2026-10-24)

The latest measured complete-fit release remains 1.0305445 seconds median,
1.0288362?1.0897724 seconds across three quiet fresh processes; it is not yet
subsecond-qualified. The approved price contract remains min($0.001/share,
1% of the original raw spread), with unchanged full coverage and arbitrage gates.

Public sigma-only Greek validity is repaired in integrated source a23f50cb:
near-edge gamma/theta/charm now carry typed refusals, while prices remain unchanged.
Root integration passes all19 focused Greek tests, the direct/detached/chain/shock
zero-allocation test, and owning PCH-off compilation. The frozen715-contract
continuous-carry derivative corpus and separate model-versus-served-map runner
are the next accuracy gate. The price budget does not define a Greek tolerance.

Measured exclusions narrow the next performance work. Alternative QP orderings
increase symbolic work; cached factorization has no reliable latency gain. Changing
the tensor builder from AVX2 Reference to scalar Fast also has no reliable gain.
All496 rejected economic family proposals have intervals wider than the allowed
budget; moving the candidate or retrying the scalar bound recovers none. All496
are nevertheless served by strict native inversion, with466 additional cold
price-budget checks. The700 Screen attempts are a separate later population.
The current work targets repeated invariant geometry and duplicate cold proof
work, preserving the original proof and fallback on every refusal.

Independent continuous-carry PDE refinement on14 selected difficult puts now
agrees with QuantLib highPrecision within $0.000001947. Empirical refinement
indicators still reach $0.0000252; those indicators are not rigorous bounds and
all previously unresolved reference labels remain unchanged. See the hashed
cross-method comparison in independent-pde-long-vol-refine-v2/.

Cash-dividend work starts from the existing D0/D1 implementation, not a blank
reference project: exact-event PDE plus trinomial reference, corrected cubic
splice, event-aware routes and prepared price/IV/Greek batches already exist.
The existing22,009-contract D1 report records all price outputs and16,076 IVs,
781 genuine price-only rows,938 inconsistent-range refusals,6 budget failures,
and4,208 without two-sided markets. Historical load-contended timings are not a
new release qualification. Next gates are rescoring under explicit monetary
price tolerances, preserving event/model conventions and reference uncertainty,
then comparing amortized rollback/family or PDE methods at equal accuracy.
See atx-vol/docs/american-cash-dividend-reference.md section11 and the HFT support
matrix. Continuous-yield substitution cannot qualify dated-cash output.

Production default promotion, main merge,100-process cold-fit qualification,
full archive persistence and broad price/Greek/cash frontier qualification remain
open. Every rejected experiment is retained with source and evidence hashes.


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


## Local-main merge completed and current release clocks

The authorized local-main merge completed at `96cd56db9ad034ee6868e3e5a8babb81ac8cb82c`.
Main's bindings/storage history is retained through `725e36a7`; the only source
integration fix is the independently reviewed Python `ProjectedPrice` enum in
`a1ca7d14`. Storage3/3 gates pass; Python passes787 tests with245 environment
skips and1 existing strict xfail. The first Python run found two stale CRLF
fixture copies, restored to exact committed bytes; no golden or test source
was changed. Before/after manifests prove identical SHA256 values for all52,031
pre-existing modified/untracked files totaling2,120,057,056 bytes. Root CMake,
editor settings and untracked work remain intact. Development continues in pool3.

The combined development and ThinLTO executables pass all13 actual-API gates,
with identical predictions `e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`.
Their archived hashes are respectively
`5c121cd1ed9408fa3814f5a785d7e22092b2d751714b168b989411fd9669e5c7` and
`9e3f384cafc622539c5e58694634cc9be45094947d16d1af65cca61a3b855fc1`.
Audits: `acceptance/main-merge-{v1,lto-v1}-audit/verification-reviewed-v3.json`.

Three fresh-process ThinLTO one-worker/Pcore6 fits took **1.1647743,1.0251590,
1.0989175s**, median **1.0989175s**; all exceeded one second. SMT sibling shares
were2.609%,0%,0%, within the5% gate. Manifest
`main-merge-v1-lto-profile/manifest.json` SHA256
`88be793e4f3b711dcff33cccbbb3e904bf57118f1b4da13ce1696feafe167c81`.
A current-host rerun of archived baseline `1eb11005...` took1.4172-1.5288s with
23.7-28.1% system activity. A subsequent interleaved comparison recorded
baseline1.1479/candidate1.2117s, then stopped with exit4 on the next baseline's
5.747% SMT activity (34.6% system). These controls are retained under
`main-merge-baseline-rerun-v1` and `main-floor-interleaved-v1-*`; they establish
neither a speedup nor an algorithmic regression. Historical baseline source is
`a0af95ab`; source-root is intentionally absent in the archived-binary reruns.

Attribution is consistent with the removed work: the best current trial replaces
roughly120ms of native confirmation plus later price checks with65.392ms of
cold seed proof. Calendar381.625ms and family226.121ms were higher than the
preceding low-load measurements; these stage clocks are not an isolated A/B
proof. The next reviewed component `a180260f`, integrated as`c721422d`, shares
exact floors across remaining ordinary candidates and removes265 more boundary
solves with unchanged predicted bytes in its component replay. Integrated
verification and qualified latency remain pending. The latency target and
complete correctness contract are unchanged;100-process qualification remains
open, and no worldwide ranking or default-policy promotion is asserted.
