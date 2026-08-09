# Fundamental signal improvement loop 10: SEC filing reaction and earnings confirmation

Date: 2026-08-09

## Research question

Does the market reaction immediately after a periodic SEC filing confirm standardized unexpected
earnings and improve the tactical earnings signal?

## Primary-source basis

- Livnat, Qi, and Wu find that market reactions around SEC filings are positively associated with
  the preceding earnings surprise and that the filing window contains unusually concentrated
  confirming information:
  <https://citeseerx.ist.psu.edu/document?doi=b225d3cba66b0a8d015ba9db994528c6b325d0aa&repid=rep1&type=pdf>.
- Li and Ramesh find significant reactions around quarterly periodic reports primarily when the
  filing is the first public earnings disclosure, while 10-K filings can contain incremental
  information beyond earnings releases:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1344826>.
- Soffer and Lys show that dissemination of predictable information progressively incorporates the
  implications of prior earnings news into investor expectations:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1911-3846.1999.tb00583.x>.

The research distinguishes preliminary earnings announcements from later SEC filings. The
warehouse must preserve that distinction.

## Timestamp audit and event definition

All 70,702 first-revision quarter-like diluted-EPS filing timestamps have the same 22:00 time, and
the timestamp date always equals the SEC `filed` date. This is a deterministic companyfacts
normalization, not original EDGAR acceptance time.

Consequences:

- an intraday announcement window cannot be reconstructed;
- the SEC filed date must not be called an earnings-announcement timestamp;
- the conservative measurable event is the first complete trading session strictly after the
  filed date;
- the factor is unavailable until that session closes.

The event return is the security's adjusted close-to-close return for that post-filing session,
less the cross-sectional median return on the same date. The post-session adjustment factor removes
split and dividend discontinuities. There are 33,284 distinct usable filing reactions across 1,113
securities from 2012-03-21 through 2026-06-12.

## Build

### Filing reaction

Added `earnings_sec_filing_reaction` with:

- explicit day-level SEC filing semantics and timestamp limitation in every row's lineage;
- first complete session after the filed date, bounded to seven calendar days;
- split/dividend-adjusted security return;
- same-date cross-sectional median market adjustment;
- reaction availability enforced before the monthly decision timestamp;
- 1% winsorization, cross-sectional z-scoring, deterministic IDs, and idempotent refresh;
- dependency on the exact SUE statement event and market return;
- governed migration `0203` and standalone CLI.

Live output: 98,485 rows, 1,113 securities, 176 dates, 2012-03-30 through 2026-06-15.

### Earnings confirmation

SUE and filing reaction have mean correlation 0.049 and mean absolute correlation 0.061. An
equal-weight intersection improved the joint horizon profile, so it was productionized as
`earnings_sue_filing_confirmation`:

- exact `(security_id, as_of_date)` intersection;
- later upstream `available_at` propagation;
- nested lineage for both inputs;
- equal weighting followed by cross-sectional z-scoring;
- governed migration `0204` and standalone CLI.

Live output: 98,485 rows, 1,113 securities, and 176 dates.

## Analysis

### Filing reaction alone

Run id: `loop10-filing-reaction-production`.

| Horizon | Mean rank IC | HAC t-stat | Mean names |
|---:|---:|---:|---:|
| 21d | 0.00786 | 1.81 | 511 |
| 63d | 0.01130 | 1.69 | 512 |
| 126d | 0.01835 | 2.61 | 507 |
| 252d | 0.00786 | 1.09 | 499 |

The strongest response appears at 126 days rather than immediately after filing, consistent with
a gradual confirmation channel but also potentially with medium-term price continuation.

### Earnings confirmation

Run id: `loop10-earnings-confirmation-production`.

| Horizon | Mean rank IC | HAC t-stat | Spread | Hit rate |
|---:|---:|---:|---:|---:|
| 21d | 0.01346 | 2.44 | 0.256% | 59.1% |
| 63d | 0.01640 | 2.37 | 0.587% | 61.5% |
| 126d | 0.01922 | 2.23 | 0.585% | 62.7% |
| 252d | 0.00801 | 0.67 | -0.717% | 50.6% |

The confirmation factor is tactical: top/bottom turnover is 48.0%/48.4%, rank autocorrelation is
0.767, and the 252-day endpoint spread is negative.

### Subperiod stability

| Period | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC |
|---|---:|---:|---:|
| 2012-2015 | 0.0204 / 3.15 | 0.0213 / 2.27 | 0.0127 / 1.26 |
| 2016-2020 | 0.0091 / 0.86 | 0.0077 / 0.52 | 0.0056 / 0.35 |
| 2021-2026 | 0.0128 / 1.35 | 0.0215 / 2.37 | 0.0384 / 2.93 |

The sign is positive for every tested period/horizon. As in Loop 09, 2016-2020 is weak, while
recent predictive power shifts toward 63-126 days.

## Router allocation tests

On 88,316 common keys, earnings confirmation and the production router have mean correlation 0.018
and mean absolute correlation 0.043.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Router | 0.02207 / 3.56 | 0.03609 / 4.31 | 0.04907 / 4.54 | 0.06375 / 5.72 |
| Earnings confirmation | 0.01381 / 2.51 | 0.01721 / 2.48 | 0.02001 / 2.34 | 0.00868 / 0.73 |
| 75% router / 25% confirmation | 0.02521 / 4.25 | 0.03788 / 4.85 | 0.05049 / 4.70 | 0.05366 / 3.79 |
| 50% router / 50% confirmation | 0.02232 / 3.90 | 0.03112 / 4.27 | 0.04202 / 4.23 | 0.03836 / 2.71 |

A full-breadth fallback overlay—25% confirmation where available and otherwise the router—retains
roughly 660 names and produces IC 0.02449/0.03673/0.05164/0.06326 with HAC
4.09/4.95/5.31/4.91. It improves 21-63 days but still reduces the 252-day IC and HAC versus the
unmodified router.

## Decision

Promote `earnings_sec_filing_reaction` as a queryable event factor and
`earnings_sue_filing_confirmation` as the preferred tactical earnings sleeve for 21-126-day
forecasts.

Do not replace the broad production router and do not promote a universal router/earnings blend.
The 25% overlay is approved only as a research candidate for horizon-aware 21-63-day forecasts;
its long-horizon degradation prevents an unconditional production allocation.

## Verification

- Filing-reaction targeted tests: 2 passed.
- Earnings-confirmation targeted tests: 2 passed.
- New files: Ruff and Python compilation clean.
- Schema `0204`, migration checksums, checkpoint, dependency, duplicate, and finiteness checks:
  passed.
- Full-suite execution was intentionally avoided.

## Next loop

Research earnings quality conditioned on cash realization: combine earnings surprise/confirmation
with cash-flow surprise or accrual direction. The goal is to distinguish sustainable earnings news
from accrual-driven news without imposing a long-horizon blend on the tactical sleeve.
