# SPY centrality correction: retained negative result

The default-OFF experiment at `9965771e` does not justify promotion. Reusing a
factor for extra centrality directions reduces SPY's factor count, but adds too
many solves. The full fitted surface also fails the unchanged exact evidence
pins for two previously reviewed floor targets. Production main remains
`a037b1e4`; no threshold, fixture or default was changed to admit this result.

## Complete workload

Both fresh projection processes use the identical normalized SPY fixture,
33 expiries, 12,866 variables and 54,104 constraints. Every slice, exact box,
affine carry bound and all-pair calendar certificate passes. The ordinary build
reproduces the historical complete projected-price fingerprint exactly.

| Projection work | Original | Centrality |
|---|---:|---:|
| Numeric factorizations | 35 | 29 |
| Direction solves | 70 | 124 |
| Outer sweeps | 36 | 30 |
| Extra correction attempts/solves | 0 | 66 |
| Accepted corrections | 0 | 51 |
| Nonimproving corrections | 0 | 15 |
| Final primal residual | 8.47577e-11 | 3.49802e-11 |
| Optional workspace reservation | 0 | 2,040,256 bytes |

There are no correction allocation, input, solve, nonfinite or target refusals.
All additional work is retained. Development component observations are
822.001 ms original and 1005.952 ms centrality, including setup and certificates;
centrality's nested correction time is 266.766 ms. These runs are not ThinLTO
quiet-host latency evidence. Their work counts show the mechanism: six saved
factorizations are exchanged for 54 additional direction solves and correction
vector passes. No speedup or subsecond claim follows.

## Full fit and actual-API audit

One complete audit retains all predictions and all thirteen gates. Twelve pass:
all 33 valid expiries and 12,866 eligible rows are served, 12,851 lie inside their
original boxes (99.8834136484%), and the published API has no shape, parity or
calendar violation. Counts remain 12,850 identified IVs and 16 truthful PriceOnly
rows. The maximum cold price residual is $0.0005012966093147497/share; every
identified row passes `min($0.001/share, 1% of original spread)`. Prediction and
metadata queries agree with the actual API.

The identification-review gate remains **false**. There are no additional
PriceOnly IDs or classification changes relative to the qualified ordinary
surface. However, the two original reviewed transitions have new target prices:

| Row ID | Frozen reviewed target | Actual target | Difference, USD/share |
|---|---:|---:|---:|
| 27418697 | 655.9104556897006 | 655.9104556851681 | -4.53247e-9 |
| 27419115 | 558.0053902327610 | 558.0053902342734 | +1.51238e-9 |

Those shifts exceed the existing exact-evidence roundoff rule, although they
are economically tiny. The verifier correctly refuses `actual_query_or_target_mismatch`.
No pin is broadened and no new cold-proof fixture is manufactured to turn this
unsuccessful performance experiment green. The prediction hash is
`c740728e40ead4221cafc7340d612aee673520d323c71eeb420c516e7256989c`.
Audit-inclusive wall time is not a cold-fit latency measurement.

## Validation and next direction

The component has fifteen passing actual-solver/dispatch controls, 361 passing
enabled quote tests with six explicit skips, five owning PCH-off translation
units, and fifteen passing ordinary-build projection controls. Fresh independent
reviews approved the exact implementation before either full-SPY experiment.
The contrived finite-matrix overflow regression exposed a separate Newton-system
defect; its finite-Hessian refusal is useful independently of the unsuccessful
centrality strategy. No SPY mispricing caused by that overflow was observed.

The next experiment should reduce factorizations without adding dozens of
triangular solves. [Gondzio and Sobral](https://arxiv.org/abs/1806.09871) study
inverse secant updates across interior-point iterations. Their approach motivates
a separately declared, one-step factor-reuse experiment; it supplies no
convergence or speed guarantee for ATX's feasibility-only problem. The current
centrality result remains negative and will not be combined silently with it.

Evidence under `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/centrality-research-v1/`:
`whole-qp-component-receipt.json` pins 10 sources and 56 artifacts (SHA256
`a48519bfb2fbc57438a84fbcdff930e92c34838d7d68c764b579663cb8301d2b`).
`whole-qp-outcome-receipt.json` pins 39 subsequent artifacts, including both
complete projection outcomes, ordinary-build controls and the failed full audit
(SHA256 `962fdfba2d0c28dd7a84edaf4f5888feba90cfa0119f9c6ac86d808cfc4d6e83`).
The original failed audit, source, executables and floor diagnostic are immutable.
