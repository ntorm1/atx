# Common Markov concentrated state grid

Implemented on 2026-10-16 from frozen base
`9717c4769fb93ff8e40e9159bcc6b741aa46bf7e` in leased pool-34, branch
`feat/vol-grid-20261016-concentrated`, RunId/HeartbeatId
`vol-grid-20261016-concentrated`. The independent heartbeat keeper acquisition
receipt was PID 16640, start `2026-10-16T12:15:55.0336771Z`.

## Contract and numerical design

`api/pricing/american_markov_grid.hpp` exposes
`make_american_markov_sinh_grid(spot, config)`. It returns owned states,
the initial spot index, adjacent spot gaps/minimum gap in units of spot, and
the maximum interior unit-volatility exit rate on the represented grid.

The transformation is `x/spot = 1 + a*sinh(u)`, with caller-selected positive
dimensionless concentration `a`, left coordinate `-asinh(1/a)` and right
coordinate `asinh((upper_multiple-1)/a)`. The `N-1` intervals are allocated
proportionally to these coordinate lengths, rounded to nearest, with at least
one interval on each side. Each side is uniform in its transformed coordinate.
Exact zero, initial spot and upper boundary are assigned explicitly. The output
has exactly the requested N nodes: no insertion, deletion or width floor.

The bounded factory accepts 3..4097 nodes subject to an explicit caller state
cap (default1025). The hard cap uses the compiler's existing constant.
Malformed inputs and resource configurations return InvalidArgument; exceeding
the caller count cap or losing finite strictly increasing representation or
finite spacing/rate diagnostics returns OutOfRange. Allocation is outside the
pricing hot path and may throw bad_alloc, with no caller state modified.

The rate diagnostic is `max_i (x_i/h_left)*(x_i/h_right)`. With constant sigma,
multiply by sigma squared to estimate the initial-grid rate per variance year.
It is a rounded diagnostic, not a certified upper bound. Compilation continues
to compute/check actual local-volatility and post-dividend rates and retains
all positive-transition, time-step, row and state budgets. The helper neither
changes the compiler's uniform65 default nor raises any resource limit.

All strikes and expiry prefixes use the same supplied grid and stock process.
The helper takes no strikes, quotes, fair prices or fair vols. A characteristic
`sigma*sqrt(variance_horizon)` is a possible explicit width, not an automatic
fit rule. A single width across very different expiries is a numerical tradeoff.
Narrow concentration increases CFL cost; a finite upper domain remains a tail
assumption. This factory does not certify continuous-American accuracy.

## Evidence

Implementation preceded tests, as the user requested; local TDD wording was
overridden. Graph discovery tools were absent, so source discovery used rg.

Temporary local CMake wiring added the source to atx-vol and the test to the
focused Markov executable. Both CMake files were restored before committing;
permanent library/test/umbrella/manifest integration belongs to the parent.

Completed checks through the absolute leased-tree wrapper:

- `-Preset vol-rel-avx2 -Jobs 2 build atx-vol-american-markov-tests`: passed.
- `-Preset vol-rel-avx2 -Jobs 2 -Ctest -R '^atx-vol-american-markov$'`:
  passed, 47 executed GoogleTests, 0 failed, including all 11 new grid cases.
- Direct new-suite receipt:
  `build-vol-rel-avx2/grid-tests.xml`: 11 run, 11 passed, no skips.
- `-Preset vol-hygiene -Jobs 2 check
  atx-vol/src/pricing/american_markov_grid.cpp
  atx-vol/tests/american_markov_grid_test.cpp`: passed with PCH off.
  A cold full owning-library hygiene build was intentionally stopped after
  102 of238 objects; the focused new translation units are the completed gate.
  No full Debug test run or repository-wide hygiene gate is claimed.

New checks cover exact count/spot/boundaries from3 to4097 nodes, asymmetric
domains, covariance from1e-150 to1e150 stock scales, invalid/nonfinite inputs,
extreme representability refusals, explicit compiler budget refusals,
negative rates/separate clocks, dated cash with local volatility and positive
martingale rows, stopped variance, limited liability and deterministic exercise.

A zero-rate/zero-yield near-expiry control uses S100, sigma.02,
variance time1/252, calendar time7/365. Both American sides then have a known
European limit. Five strikes99.85/99.95/100/100.05/100.15 are checked against the
independent Black formula, with width `.02*sqrt(1/252)`:

| Grid | Error in price units |
| --- | ---: |
| Existing uniform65, ATM only | 0.048992158330299504 |
| Sinh65, maximum over five strikes and both sides | 0.00027185645863225805 |
| Sinh257, maximum over five strikes and both sides | 0.000014398378136701306 |

This isolates grid resolution and is not the funded `short_low_vol` American
put fixture from the prior FD audit. Parent audit integration will measure
funded/cash/high-volatility American behavior and explicit refusals over all
existing fixtures. No vendor labels were read and no runtime-speed claim was
made. Finite-model martingale consistency does not establish continuous
American or tail accuracy.

## Primary research context

The sinh concentration family has established numerical precedent. Düring,
Fournié and Heuer, [section5.1 of their 2014 paper](https://arxiv.org/html/1404.5138v1#S5.SS1),
use a sinh zoom and discuss balancing central resolution against errors
elsewhere. Their scheme is a different high-order stochastic-volatility PDE
method; its convergence/stability claims are not claims for this finite Markov
operator. The present spot-centred, exactly budgeted two-sided construction is
chosen to preserve one reusable stock process, independent of option strike.

## Integration checklist

Register `src/pricing/american_markov_grid.cpp` with atx-vol,
`tests/american_markov_grid_test.cpp` in focused and monolithic targets, and
the public header in the parent-owned umbrella/manifest. Independently review
the exact lane SHA before integration. Keep explicit audit profile names for
concentration/node/domain choices and distinguish grid refusal from model
compilation refusal. Do not replace production defaults from one short-expiry
control. Further independent FD results, quote fitting, Greeks and performance
qualification remain separate work.
