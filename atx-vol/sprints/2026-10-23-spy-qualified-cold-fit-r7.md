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
- Native repair inversion: `ee94ee72` review in progress. A cached Search root is
  not an adequate accuracy oracle for the cold kernel: one comparison differed by
  1.44e-7, while direct cold-kernel endpoint prices bracketed the native root.
  Use tight bisection of the declared cold forward map for that accuracy test.

No release or main integration is claimed here. Append the exact reviewed source
commits, tests, full SPY oracle result, before/after stage and solve counters,
quiet one-core timings, and outstanding general-product limitations. A goal pass
requires all four acceptance conditions in one candidate; a speed improvement
alone does not close this sprint.
