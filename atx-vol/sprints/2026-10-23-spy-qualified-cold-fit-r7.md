# SPY qualified cold fit, r7

Status: implementation in progress; no qualified sub-second result.

## Acceptance contract

One build and one policy must simultaneously deliver a fresh-process SPY cold fit on
one physical core in less than one second, at least 99.5% bid/ask containment, service
on every valid positive-time expiry, and no arbitrage in the representation clients
actually query. Do not combine timing, coverage and accuracy from separate policies.
The target is calibration; ready-to-quote construction and risk remain separate,
explicitly reported clocks. No warm-cache substitution, hidden parallelism, reference
labels as fitting inputs, skipped validation, or reclassification of solver failures
as invalid data.

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
