# Curve / admission matrix (lane C3, 2026-10-23)

One measured matrix of every fitting policy over the nine historical shape classes, the opt-in
shared call floor (`serve.call_floor: discounted_intrinsic`), the one-sided-quote admission
contract and a deterministic per-class selection. Nothing here is promoted: every key is opt-in,
the keys-OFF default path is byte-identical (9/9 default hashes). The measured numbers, hashes
and PASS/MISS verdicts live in the lane report
(`.superpowers/sdd/2026-10-18-vol-sota/lane-C3-curve-admission-r6-report.md`) and its evidence
directory (`C:/atx-cache/quote-benchmark/C3-20261023-admission/`).

## The matrix harness (`tools/shape_matrix.py`)

```
python -E -P -B atx-vol/tools/shape_matrix.py run    --spec matrix.json --output DIR
python -E -P -B atx-vol/tools/shape_matrix.py table  --output DIR
python -E -P -B atx-vol/tools/shape_matrix.py select --table DIR/table.json [--table ...] \
       --output winners.json [--containment 0.985] [--validated-only]
```

The spec (`schema_version` 1) names the native binary, the class manifest
(`shape-classes-20261014/manifest.json`, built by L1b from quote features only), the split
(only `development` is accepted), workers (1..8), the dividend mode, `max_cell_seconds`
(<= 600), named cohorts, named policies and the cells to run. Each cell is
`quote_benchmark.py run` (native fit, `--report native --timing-detail`) followed by
`quote_benchmark.py score --class-manifest`. The scorer is imported, never edited, so each score
keeps the scorer's own `evaluator_sha256`; SpiderRock marks are opened only by the scorer.

Per cell and per class the table carries: all-row / eligible-price / identified-IV coverage,
price-only rows, MAE (USD and per half spread), containment, IV median and p95 distance (bp),
served shape violations (monotone / convex / calendar) and duplicate disagreements, the refusal
taxonomy (status counts), per-expiry completeness buckets (complete / >= 0.9 / >= 0.5 / > 0 /
none), the served-EXPIRY completeness (fitted expiries, expiries serving at least one eligible
row) with the reason of every unserved slice, and the per-expiry fit cost (p50 / p95 of the B0
per-slice clock `graded_stats.slices[].timing.wall_us`, not wall time), plus the sha256 of the
prediction file. The unserved-slice reason comes from the native report's per-slice entry:
`arbitrage:<class>` (the certificate's refusal: vertical_spread, convexity,
nondividend_upper_bound, legendre, parity, other), `calendar`, `insufficient_quotes`,
`validator_rollback+<reason>` (C2's publication validation rolled a repair back first),
`rows_refused:<status>` (a served slice whose eligible rows were all refused) or
`not_fitted:<status>` (a chain the fitter never listed).
Every finished cell is appended to `cells.jsonl` and the tables are rewritten (checkpointing);
an existing prediction is re-verified by its receipt hash and never overwritten.

## Selection

`select` is deterministic per cohort x class: a policy qualifies with zero served shape
violations (and zero duplicate disagreements) and containment >= the bar (default 98.5 %, the
plan's C2 row); among qualifying policies the highest eligible price coverage wins, ties go to
the lower MAE/spread, then the lexicographically smaller policy name. A class with no
qualifying policy is reported as `none`. `--validated-only` admits only policies whose file sets
`graded.publication = validate` (C2's validation as the admission bar). It also reports the best
single policy (the one that
qualifies on the most classes). The selection is tool-side: the graded quote-slice path picks its
family per profile from the policy, and `curve_selector.cpp` serves the pricer-fitter path, so a
per-expiry route inside one surface fit needs a profile-routing hook in the surface fitter
(outside this lane's files).

## Opt-in shared call floor (`QuoteSliceCallFloor::DiscountedIntrinsic`)

The defect (C2 review B(3)): a later slice whose own carry puts its deep call at S - K (a
dividend payer, q > r) caps a short NON-dividend slice through the bounded calendar repair,
so the short slice serves its deep-ITM calls at S - K, below the no-arbitrage bound S - K * df.

The fix is a maturity-consistent floor shared by every expiry of one surface:

- `quote_surface_call_floor_lines(expiries)` lists one line {T, S, exp(-r T)} per expiry in the
  non-dividend regime `check_nondividend_american` certifies (rate >= 0, no continuous yield, no
  dividend PV, no calendar dividends, no quote yield override).
- Each expiry is held to the lines of expiries at or before it:
  floor(K) = max(0, max over T' <= T of S' - K * df'). An American call satisfies
  C(T) >= C(T') >= S' - K * df' for every non-dividend T' <= T, so the floor is a valid lower
  bound on every expiry, dividend payers included, and is non-decreasing in maturity.
- `certify_and_serve` lifts every call node of the CERTIFIED slice (after its own repairs) that
  sits more than the certificate allowance below the floor onto it, re-certifies with the
  slice's bounds, and marks the lifted nodes moved (their rows' volatilities are re-implied:
  `ReimpliedRepaired`). A lift the certificate cannot carry is refused whole
  (`diagnostics.call_floor_refused`), never half applied.
- Because every expiry is floored, the bounded calendar repair's floors and caps stay at or
  above the floor, so a repaired non-dividend slice keeps its bound and passes the
  `validate` publication check that refuses it under Legacy.

Bench: `serve.call_floor: legacy | discounted_intrinsic` (graded admission only; the native report
adds per-slice `call_floor` {lines, lifted_nodes, refused} and run counters only when on).
Library: `QuoteSliceConfig::call_floor` {mode, lines}; Legacy with lines is refused.

Known interactions (measured in the lane report): the key is inert where a surface has no
non-dividend expiry (cohort5, SPY included) and active in the combined arm on dev200 / dev200b; a
lifted later expiry can change
which of two crossing expiries the greedy calendar pass keeps (a calendar-pass exchange), because
the bounded calendar repair clamps calls without moving the put (`AmericanPriceSlice` refuses the
clamped grid on its affine call-minus-put bound).

## One-sided quotes and snapshot-only admission

- A zero-bid quote is never a point observation: the point fit marks it `BidlessQuote` and the
  channel uses it only as an upper bound [0, ask]; no midpoint is ever formed from a zero bid.
  `QuoteSliceAdmission` tests fail if either rule is reverted.
- A bid-only quote (bid > 0, ask <= 0) does not occur in cohort5 / dev200 / dev200b (0 rows);
  the adapter's `invalid_option_quote` rows there are no-market sentinels (ask <= 0, bid -99),
  outside the frozen eligible population. `QuoteSliceQuote` needs a finite positive ask, so the
  library cannot express a lower-bound-only observation (a bid-only inequality needs an API
  extension).
- A surface fit depends only on its own snapshot (no cross-snapshot state):
  `ASurfaceFitDependsOnlyOnItsOwnSnapshot` fits one snapshot, then a different one, then the
  first again (per-thread de-Am memo and rung memo on) and compares statuses and prices bit for
  bit.

## Events and 0DTE

Every 0DTE row of the cohorts is outside the eligible population (cohort5: 398 `invalid_time`;
dev200: 376 `invalid_time` + 250 `unsupported_exercise`; dev200b has no 0DTE row), so 0DTE has no
fitted row to score; the `short_dated` class (DTE <= 3) is the
nearest measured proxy. The inputs carry no earnings calendar, so earnings-variance rows are not
measurable in this matrix.
