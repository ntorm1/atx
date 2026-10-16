# Common-model liquidity policy

Implemented from frozen base `f376b6062c0754dbae0ae2d77b8cc159e9088768` in
leased pool-34, branch `feat/vol-markov-policy-20261016`, RunId/HeartbeatId
`vol-markov-policy-20261016`. Keeper acquisition: PID 21612, start
`2026-10-16T12:25:51.0360499Z`. The approved grid dependency 807c4036 was
cherry-picked locally as ab57c434; parent already owns grid integration and
should integrate only the later policy commit.

`AmericanMarkovPolicyCatalog` resolves common-process fitting settings from
raw contract liquidity, with exact case-sensitive symbol overrides. It has
its own model, sinh-grid and fitting configurations; no eSSVI/slice-curve
settings are used as common-model parameters. Definitions borrow fit knot,
initial-sigma and prior-sigma spans only through catalog creation. The catalog
deep-copies all three into stable storage. Resolved profiles are immutable
shared aliases whose ownership includes every referenced vector. Copies/moves
of catalogs and resolved results remain valid after source definitions are
destroyed. Copying a profile/config alone borrows its spans and requires retaining
the resolved owner, documented in the public header.

The existing quote policy's exact raw routing predicate was extracted as
`classify_quote_fit_liquidity(features, regimes)`. Both catalogs use it and
the existing `quote_fit_liquidity_features` extraction. This keeps conservative
duplicate-contract consolidation, near-money spread/bidless rules and
symbol-independent classification in one implementation. No threshold or
valid-input routing behavior was intentionally changed. The new public helper
validates routing features and thresholds. Locked quotes remain excluded from
liquidity evidence by that existing extractor, although the common fitter can
accept them as price intervals.

## Explicit uncalibrated starting profiles

| Profile | Local-vol structure | Prior / strike / time weights | Grid count / width |
| --- | --- | --- | --- |
| sparse | one shared sigma | .10 / .10 / .10 | 65 / .20 |
| ordinary | three shared knots(-.25,0,.25) | .01 / .05 / .05 | 129 / .10 |
| dense | five knots(-.5,-.2,0,.2,.5) per active variance interval | .001 / .01 / .02 | 129 / .10 |

All widths are dimensionless fractions of spot; all domains end at 4 times spot.
Sparse defaults use max 8 parameters, 128 evaluations, 24 sweeps; ordinary uses
max 32 parameters, 256 evaluations, 32 sweeps; dense uses max 64 parameters,
512 evaluations,48 sweeps and 8192 quote capacity. These are starting settings,
not market-optimized or accuracy-certified values. In sparse/shared structures,
some roughness terms are zero because there are no adjacent knots/time groups.
Caller definitions can replace every setting. No numerical gate is disabled.

The resolved result exposes its full numeric configuration, raw features,
selection source, actual time-group count and actual parameter count for
provenance. The adapter's market/dividend tolerance configuration remains
separate explicit per-run input; it is not hidden in a policy profile.

## Validation and bounds

Catalog creation validates each profile's finite/ranged numerical settings,
compiler hard caps, fitter hard caps, vector layouts, grid representability at
unit stock scale, and incompatible grid/model/batch/storage capacities. It does
not promise that every market/sigma trial can compile within those budgets.
The actual grid factory and fitter retain final numerical authority.

Catalogs are capped at 64 profiles, 4096 overrides and 128 bytes per name/symbol.
Empty names, whitespace/control characters, duplicate names/keys and unknown
references are refused. Raw resolution is capped at 4096 snapshots and 65536
total raw rows before feature allocations. The actual common schedule is
validated before counting variance groups. Calendar endpoints increase;
variance is nondecreasing. Every raw expiry must match calendar and variance
endpoints; snapshots must share one exact spot. Callers should use the adapter's
accepted common spot/clocks when constructing these metadata, while keeping
the underlying raw contract/bid/ask rows.

Shared parameters use one group. PerVarianceInterval uses one group per
strictly positive variance increment; paused intervals add none. All-paused
variance, max-parameter excess and initial/prior vectors inconsistent with
actual groups are explicit refusals. Profiles, knots and schedules are never
silently reduced to fit a budget. Overrides have no bypass. A single quoted
interval can be fitted under the sparse prior; there is no arbitrary four-strike
minimum inherited from a separate smile model. No usable quotes still fail in
the fitter. Positive quoted variance clocks remain required by the inherited
raw extractor.

## Validation receipts

Production implementation preceded tests, honoring the user's no-TDD request.
Shared classifier extraction was coordinated with the parent and native lane.
Graph MCP discovery tools remained absent; source discovery used rg.

Temporary local CMake registration added the grid/policy sources and policy
test to existing focused targets. Both CMake files were restored before the
policy commit; permanent source/test/umbrella/manifest wiring belongs to parent.

- Release/AVX2 build of atx-vol-american-markov-tests and atx-vol-quote-slice-tests
  passed through the absolute pool-34 wrapper. An initial test-TU warning for a
  copied range-loop string was corrected to const-reference before final gates.
- Anchored ctest regex `^(atx-vol-american-markov|atx-vol-quote-slice)$`: 2/2 passed,
  containing 48 + 49 = 97 executed GoogleTests, no failures/skips.
- Direct `AmericanMarkovPolicy.*`: 12/12 passed, receipt
  `build-vol-rel-avx2/policy-tests.xml`.
- Direct `QuoteFitPolicy.*`: 10/10 passed, receipt
  `build-vol-rel-avx2/quote-policy-tests.xml`.
- PCH-off focused checks passed for policy source/test and existing
  quote_fit_policy source/test (both existing test object registrations).
  No full Debug runtime, full-repository hygiene, or speed qualification claimed.

New tests exercise old/new routing parity, full numeric provenance, exact
overrides, raw-feature retention, deep ownership after destruction/copy/move,
paused-variance parameter layout, explicit overflow refusal, schedule-dependent
initial vectors, incompatible spots/clocks, duplicate expiries, malformed
catalog/numeric/resource inputs, raw count caps, moved-from refusal, shared
classifier validation, and one actual sparse quote-interval fit preserving the
common model's martingale diagnostic.

No vendor labels were opened. No empirical default tuning or universal grid
accuracy claim was made. Parent's independent grid audit continues separately;
finite-model consistency, continuous-American error, quote-fit quality and
resource refusal rates remain distinct metrics.
