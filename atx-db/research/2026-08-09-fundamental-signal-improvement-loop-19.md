# Fundamental signal improvement loop 19: four-quarter change in ROE

Date: 2026-08-09

## Research question

Does the q5 model's year-over-year change in quarterly return on equity retain the useful modern
component of quarterly ROE while avoiding its problematic low-level tail, and can it improve the
production operating-profitability/net-issuance router?

## Primary-source basis

- The global-q authors' current factor specification defines `dROE` as current ROE minus ROE four
  quarters earlier, uses the latest public earnings announcement at each monthly sort, and
  winsorizes cross-sections at the 1st/99th percentiles:
  <https://global-q.org/uploads/1/2/2/6/122679606/factorstd_2025feb.pdf>.
- Hou, Mo, Xue, and Zhang's augmented q-factor model forecasts expected growth using current Tobin's
  q, operating cash flow, and change in ROE; the expected-growth factor earns a reported 0.84% per
  month with a 10.27 t-stat in their sample:
  <https://academic.oup.com/rof/article-abstract/25/1/1/5727769>.
- The authors' maintained testing-portfolio definitions explicitly list `dRoe1` as the
  four-quarter change in ROE, confirming the signal horizon:
  <https://global-q.org/testingportfolios.html>.
- The corresponding working paper provides the longer construction and robustness record:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3525435>.

The four-quarter difference, higher-is-better direction, complete-case policy, monthly decision
surface, and 1%/99% winsorization were fixed before evaluation.

## Production build

Added `atx_db.delta_roe`, migration `0218`, a standalone build CLI, and two focused tests. The
feature is `profitability_q_factor_delta_roe`, sourced as
`atx-db PIT four-quarter change in ROE v1`.

Each current governed qROE decision is matched to the same security's closest visible prior qROE
whose underlying earnings period ended 300-430 days earlier. Both decisions must already be visible
at the current monthly close. The calculation is exactly:

`quarterly_roe_t - quarterly_roe_t_minus_4`

No missing values are imputed and no weights or return-fitted parameters are learned. The upstream
qROE feature already records the SEC facts, periods, and announcement-time availability; compact
delta lineage references both governed qROE decisions and their period ends.

The pre-build audit found 80,433 eligible matches, or 87.2% of all qROE decisions, with zero timing,
duplicate, or null-period violations. Observed fiscal gaps are 336-396 days with a 365-day median.
After the predeclared 20-name daily breadth floor, the refresh materializes 80,422 rows across 856
securities and 166 dates from 2013-01-31 to 2026-06-15 in 12.7 seconds. Retained dates have at least
22 names, and maximum lineage is 960 bytes.

Raw differences range from -533.47 to 17.6 million because qROE can explode when reported positive
equity approaches zero. Raw observations remain auditable; scores use the predeclared 1st/99th
cross-sectional caps before z-scoring. Persisted values contain no duplicate or non-finite rows.

## Standalone analysis

Run id: `loop19-four-quarter-delta-roe-production`. Deciles and split-adjusted forward returns are
used.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0106 | 1.65 | 0.041% | 51.9% | 0.15 |
| 63d | 0.0038 | 0.44 | -0.293% | 51.9% | 0.07 |
| 126d | -0.0010 | -0.09 | 0.166% | 54.1% | 0.13 |
| 252d | -0.0126 | -1.05 | -1.509% | 50.3% | -0.10 |

The signal is weakly useful over one month, has no reliable intermediate-horizon edge, and reverses
at one year in the full sample. Unlike qROE level, it does not have a persistent high-minus-low tail
payoff. Top/bottom-decile turnover is 40.0%/40.7%, mean rank autocorrelation is 0.817, and 165
rebalances are available. This is materially less stable and more expensive to trade than the
production router.

### Subperiod stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2013-2015 | 0.0283 | 0.0092 | -0.0120 | -0.0348 |
| 2016-2020 | -0.0003 | -0.0050 | -0.0140 | -0.0295 |
| 2021-2026 | 0.0112 | 0.0093 | 0.0194 | 0.0221 |
| 2023-2026 | 0.0219 | 0.0171 | 0.0275 | 0.0190 |

The 21-day result is strong in 2013-2015 (HAC 3.03), disappears in 2016-2020, and returns after
2021. The modern 126/252-day HAC t-stats are 2.10/2.78 for 2021-2026 and 2.18/2.19 for 2023-2026.
This regime dependence is useful research evidence, but it is not a stable unconditional premium.

### Distinctiveness

Mean cross-sectional correlation is 0.419 with its qROE parent, 0.048 with operating cash flow to
assets, 0.034 with operating profitability, 0.023 with the production router, 0.018 with asset
growth, 0.017 with low net issuance, -0.036 with Piotroski, and -0.006 with continuous financial
strength. The feature is economically distinct from the production router despite sharing qROE's
underlying facts.

## Router overlay trial

Coverage-neutral research panels blend 5%, 10%, or 20% delta ROE into the production router when
delta ROE is present and otherwise retain the baseline. No production rows or definitions are
changed.

| Router | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Production baseline | 0.02219 | 0.03509 | 0.05171 | 0.07088 |
| 5% delta ROE | 0.02186 | 0.03453 | 0.05059 | 0.06916 |
| 10% delta ROE | 0.02202 | 0.03422 | 0.04987 | 0.06763 |
| 20% delta ROE | 0.02172 | 0.03289 | 0.04723 | 0.06434 |

Every weight reduces full-history IC and HAC strength. Full-history Q10-Q1 spreads fall from
0.357%/1.259%/2.981%/8.262% for the baseline to 0.262%/0.799%/2.277%/7.473% at 10%. The same
10% overlay raises top/bottom turnover from 16.2%/19.2% to 18.0%/28.1% and lowers rank
autocorrelation from 0.971 to 0.958.

The modern-period improvement is too narrow to override that evidence. From 2023 onward, the 10%
blend raises 21-day IC from 0.0295 to 0.0318, but reduces one-year IC from 0.0803 to 0.0776 and
turns the 126-day spread from +1.419% to -0.041%. The production router remains unchanged.

## Decision

Keep `profitability_q_factor_delta_roe` as a production-queryable experimental feature for
short-horizon and regime-conditioned research. Do not promote it into the broad production router:
its full-history decay, unstable regimes, weak tails, and turnover costs outweigh its recent
one-month improvement.

The result also argues against treating delta ROE as a standalone q5 expected-growth proxy. The
published model combines it with Tobin's q and operating cash flow; that composite is the appropriate
next hypothesis.

## Verification

- The pure computation test and isolated migration-governance test passed.
- New and changed files pass Ruff and Python compilation.
- Live schema is `0218`; migration checksums verify and the factor has one direct governed-factor
  dependency.
- Duplicate-key, finiteness, fiscal-gap, availability, minimum-breadth, normalization, lineage-size,
  and coverage checks passed.
- Full-suite execution was intentionally avoided.

## Next loop

Research and implement a point-in-time q5 expected-growth composite. First audit whether the
warehouse can construct Tobin's q without current-market-value leakage, combine it with operating
cash flow and delta ROE using a predeclared, non-return-fitted specification, and test whether the
three-signal composite stabilizes delta ROE's regimes and tails.
