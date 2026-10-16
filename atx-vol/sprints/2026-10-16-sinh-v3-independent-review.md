# Independent review of variance-scaled wider grids

Read-only numerical/source review, with this note as the only repository edit.
Fresh lease pool-36, RunId/HeartbeatId `vol-audit-v3-review-20261016`, branch
`feat/vol-audit-v3-review-20261016`, frozen comparator base
`d66fbed66e9313b5b69c3e3f1f5bc3e330bd6a02`. Parent pool-33 was not edited.

## Artifacts and checks

Reviewed native input:
`C:/atx-cache/quote-benchmark/markov-audit-20261016-sinh-v3.json`, SHA256
`28d6aeabf3cc985b952ad62d8d4f35dca546d196ffae3757ce17da583a481739`.
Its `.run.json` records binary SHA256
`bd7267533daddca3f216cbe23394fb39a6ee406ab6e396e4669830e30797f8f4`
and source SHA256
`963189ec548eb90fd97ad3535da11232fb8f1a85cb2abd5b7f5007a68b90778c`.
The actual parent audit source matched that source hash at review.

Prior panel `markov-audit-20261016-sinh-v2.json`, SHA256
`b491940f5da502af80998dd8e86537e1a31e8a75a7e7a1ef15c0381bad61362e`.
All 165 shared cases have identical spot, full interval schedule, requested
strikes, concentration scale, model status/diagnostics, prices and exact-control
errors. Timing metadata was intentionally excluded from this equality check.

The v3 panel has 198 cases = 11 fixtures x 18 grids. Exactly 182 compiled and
priced; 16 explicitly refused on the unchanged expanded-transition limit.
Every successful case has exactly the complete set
`expiry indices x 36 unique requested strikes x 2 sides x 2 exercises`.
There are no duplicate, missing or extra produced contracts. Refused cases
contain no prices and retain their requested universe. The three added profiles
all compile/price 11/11 fixtures, each 2592/2592 requested contracts.

Independent Python calculation recomputed every no-cash European row using
`F=S*exp(R-Q)`, discount `exp(-R)`, and Black's normal-CDF formula with total
variance `sum(sigma_i^2 * delta_variance_time_i)`. Here R and Q independently
integrate the respective rates on calendar time. The recomputed maximum errors
agree with the native report within 2e-12. Each fully successful profile has
720 such European controls. Cash prefixes were not assigned a lognormal control.

Across the whole panel, scalar/batch max difference is 2.1316282072803006e-13,
max transition probability-sum error and relative conditional-mean error are
each 2.220446049250313e-16. These establish implementation agreement and finite
process invariants, not continuous-American accuracy.

Exact comparator d66fbed6 was run with the original 24 ATM references and explicit
two-record negative-rate refinement replacement. Output:
`C:/atx-cache/quote-benchmark/common-lane-sinh-v3-compare-20261016-v1.json`, SHA256
`5faca7ea58b6b782d6826da49a2d83beadd0236ada5523e210987d53b417143e`.
It retains all 46656 requested contract/grid rows; 24/24 reference keys match.
Reference provenance is embedded in the report. No QuantLib recomputation,
vendor labels, fitting or policy selection was performed in this review.

## Quantitative findings

All rows below have 11/11 successful models and 2592/2592 produced contracts.
Boundary mass shown is the high-vol fixture, which is also the maximum across
these fixtures. It is terminal mass at the absorbing upper model state, not a
rigorous option-error estimate or the unbounded process's terminal tail mass.

| Grid | Upper/spot | Worst European error, 720 controls | Upper-state mass | ATM reference MAE, same 24 | ATM max gap |
| --- | ---: | ---: | ---: | ---: | ---: |
| default65 | 4 | 0.531280798346 | 0.0392733394 | 0.163272839667 | 0.531240652704 |
| sinh65-variance | 4 | 0.088313518240 | 0.0392906336 | 0.002067433065 | 0.004816616844 |
| sinh129-variance | 4 | 0.088490959034 | 0.0392899121 | 0.000476122095 | 0.001079237017 |
| sinh129-variance-domain8 | 8 | 0.006131119816 | 0.0030917322 | 0.000596471911 | 0.001787385123 |
| sinh129-variance-domain16 | 16 | 0.010136435622 | 0.0001227658 | 0.000788781985 | 0.002045276345 |
| sinh65-variance-domain16 | 16 | 0.042080013312 | 0.0001213720 | 0.002984945434 | 0.006226571081 |

The 24 saved American ATM references cover martingale, short-low-vol, negative
rate, negative-rate-and-yield, dated cash, near cash and clock pause. They do
**not** cover funded, separate clocks, high vol or deterministic cash. Thus the
excellent 24-contract ATM statistics alone would miss the high-vol tail defect.
High-vol European controls supply the evidence above; its American put accuracy
remains independently unmeasured here.

Widening from 4 to 8 at 129 nodes reduces worst European error by about 14.4x.
Widening from 8 to 16 lowers upper-state mass another 25.2x but worsens worst
European error by about 1.65x. The wider fixed-node grid is coarser internally:
high-vol below-spot spacing changes from 1.9049% to 2.4653% to 2.8905% of spot
for domains 4, 8 and 16. Corresponding high-vol diffusion steps fall from 3113
to 1961 to 1359. The error comparison therefore changes boundary truncation,
spatial resolution and CFL-selected time resolution together.

At 4x the worst high-vol error is approximately -0.088491 at K=180; at 8x the
worst is +0.006131 at K=44.375; at 16x/129 it is +0.010136 at K=40. Opposite
error signs and changing locations caution against reading the domain-8 result
as a monotone-convergence certificate; cancellation is possible.

Per-fixture maximum European errors for the 129-node adaptive profiles:

| Fixture | Domain4 | Domain8 | Domain16 |
| --- | ---: | ---: | ---: |
| martingale | .001103114405 | .001173844453 | .001949624605 |
| funded | .000843476850 | .001081971202 | .001287283233 |
| separate clocks | .001083686822 | .001573067868 | .001314283877 |
| short low vol | .000028919394 | .000033281073 | .000008113770 |
| high vol | .088490959034 | .006131119816 | .010136435622 |
| negative rate | .001599854793 | .002117671116 | .002732453152 |
| negative rate and yield | .001737130592 | .002098177426 | .002000550008 |
| clock pause, all prefixes | .000949623721 | .000973691503 | .001586059191 |

Dated/near cash have no no-cash European controls. Their domain-8 ATM American
max gaps versus saved FD references are .001787385123 and .001355498230.
The deterministic-cash maximum against the native continuous deterministic
control remains 1.5631940186722204e-13 for all three 129-node domains.

## Recommended next experiments

1. **Separate domain from interior resolution.** Preserve the existing inner
   state knots through 4x and append a small, documented sequence of increasing
   tail knots to 8x/16x using the existing explicit-grid interface. Zero and exact
   spot remain represented. Compare with matched-inner-spacing wider sinh grids.
   A nested extension is an experiment, not a promised improvement: retain the
   same positive-transition construction and all resource refusals. This should
   reveal whether the domain-8 result benefits from cancellation while avoiding
   automatic coarsening of all near-money states.
2. **Separate time discretization on the same grid.** For high-vol and negative
   rate, hold state locations fixed and reduce max variance/calendar steps enough
   to actually change the CFL-dominated step count. Merely changing 1/104 to
   1/208 may have no effect when CFL already chooses thousands of steps. Report
   realized steps and error changes, including American exercise separately.
3. **Add a small targeted independent American panel.** High-vol puts at K40,
   K100 and K180 are currently absent from saved references; these distinguish
   the interior/tail and exercise effects exposed above. For that fixture's
   nondividend, positive-rate calls, analytic European values already provide
   the no-early-exercise control. Reuse saved reference infrastructure/checkpoints
   and require separate observed-stability results instead of rerunning the full
   historical reference panel.
4. **Keep cash/clock-pause stress tests in every change.** Near-cash currently
   consumes the most storage/work: 728205 rows/5644 diffusion steps at 129/domain4,
   640098/4961 at domain8 and 571212/4427 at domain16. Blindly increasing nodes to
   restore domain-4 interior spacing can exceed the unchanged million-row budget.
   The storage optimization may reduce physical memory without justifying looser
   work/numerical gates. Do not trade that case away for smaller aggregate error.

Domain8/129 is the best current worst-European-error compromise in this panel,
not a production policy recommendation. Domain16's lower boundary mass alone
does not qualify its prices. Use explicit whole-panel coverage, matched contract
errors, tail diagnostics and realized resources together. All timings remain
diagnostic on a concurrent development host; no speed claim is supported.
