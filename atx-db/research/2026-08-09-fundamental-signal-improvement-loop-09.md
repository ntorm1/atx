# Fundamental signal improvement loop 09: standardized unexpected earnings

Date: 2026-08-09

## Research question

Can a filing-time-safe standardized unexpected earnings signal add a short-horizon earnings-news
sleeve without weakening the broad profitability/issuance router?

## Primary-source basis

- Bernard and Thomas document post-earnings-announcement drift and evaluate delayed price response
  against risk explanations: <https://www.jstor.org/stable/2491062>.
- Their follow-up reports an implementable strategy based on the anomaly and notes that part of the
  drift is delayed until subsequent quarterly announcements:
  <https://deepblue.lib.umich.edu/bitstream/handle/2027.42/28288/0000041.pdf>.
- Livnat and Mendenhall compare time-series and analyst-forecast earnings surprises and find that
  the drift is larger for analyst surprises, while Compustat restatement policy does not explain the
  difference:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1475-679X.2006.00196.x>.
- Ball and Bartov show that the market incorporates the signs but underestimates the magnitude of
  serial correlation in seasonally differenced quarterly earnings:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=55467>.

Without a licensed point-in-time analyst-estimate history, the production feature uses the
time-series definition: current quarterly diluted EPS less the first-filed value for the same
quarter one year earlier, standardized by prior seasonal changes.

## Data audit and revision policy

| Metric | Quarter-like points | Securities | Same-season pairs |
|---|---:|---:|---:|
| Diluted EPS | 178,834 | 1,353 | 64,171 |
| Net income | 191,074 | 1,381 | 64,506 |

SEC comparative facts recur in later filings, so filtering `is_latest_revision` would inject future
restatements into historical signals. The feature instead selects the earliest available
`revision_sequence=1` point per security and period for both current and prior-year EPS.

There are 58,490 diluted-EPS seasonal changes across 1,339 securities with at least four prior
seasonal changes and nonzero historical volatility. The signal uses up to 20 prior changes,
excluding the current surprise from its denominator.

## Build

Added `earnings_standardized_unexpected_eps` with:

- initial SEC filing only; later comparative revisions are excluded;
- same-security prior-year matching within a 350–380-day tolerance;
- surprise standardization using 4–20 preceding seasonal EPS changes;
- no current-observation leakage into historical volatility;
- monthly decision points with 150-day signal freshness;
- split-aware market capitalization and liquidity gates;
- 1% two-sided winsorization, cross-sectional z-scoring, deterministic IDs, and full lineage;
- governed definition and dependency in migration `0202`;
- standalone CLI and targeted formula/governance tests.

Live build `loop9-sue-build` produced:

| Metric | Result |
|---|---:|
| Factor rows | 102,152 |
| Securities | 1,113 |
| Rebalance dates | 176 |
| Date range | 2012-03-30 to 2026-06-15 |
| Duplicate keys | 0 |
| Non-finite values | 0 |

## Production evaluation

Run id: `loop9-sue-production`.

| Horizon | Mean rank IC | HAC t-stat | Mean names | Spread | Hit rate |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.01264 | 1.95 | 528 | 0.263% | 56.7% |
| 63d | 0.01450 | 1.69 | 528 | 0.870% | 62.1% |
| 126d | 0.01233 | 1.10 | 524 | 0.727% | 60.2% |
| 252d | 0.00924 | 0.61 | 515 | 1.397% | 56.3% |

The expected horizon shape is present: IC peaks over 21–63 days and decays thereafter. The factor
is more tactical than the router: top/bottom turnover is 43.3%/44.9%, and mean rank
autocorrelation is 0.815.

## Subperiod stability

| Period | 21d IC / HAC | 63d IC / HAC |
|---|---:|---:|
| 2012-2015 | 0.0172 / 2.41 | 0.0089 / 0.92 |
| 2016-2020 | 0.0085 / 0.66 | 0.0082 / 0.44 |
| 2021-2026 | 0.0135 / 1.20 | 0.0249 / 2.26 |

Both horizons retain a positive sign in every period. Strength migrates from 21 days in the early
sample to 63 days recently; the middle period is weak.

## Equal-cohort incremental test

The router/SUE common cohort has 91,124 keys, 880 securities, and 175 dates. Mean correlation is
0.019 and mean absolute correlation is 0.039.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Production router | 0.02248 / 3.61 | 0.03593 / 4.30 | 0.05008 / 4.69 | 0.06475 / 5.69 |
| SUE | 0.01283 / 1.98 | 0.01460 / 1.70 | 0.01246 / 1.12 | 0.00924 / 0.61 |
| 75% router / 25% SUE | 0.02436 / 3.77 | 0.03483 / 4.05 | 0.04559 / 3.93 | 0.05427 / 3.61 |
| 50% router / 50% SUE | 0.02228 / 3.37 | 0.02935 / 3.46 | 0.03832 / 3.38 | 0.04301 / 2.73 |

A 25% SUE sleeve improves 21-day IC but weakens every longer horizon. The 50% blend is inferior to
the router everywhere.

## Decision

Promote `earnings_standardized_unexpected_eps` as a standalone tactical earnings-news factor for
roughly one- to three-month horizons. Do not alter the broad production router and do not publish a
single unconditional router/SUE blend.

A horizon-aware model may use a small SUE sleeve for 21-day forecasts, but that allocation must be
declared as a forecast-horizon policy rather than embedded in a supposedly horizon-agnostic factor.
The next research improvement should add announcement timing or point-in-time analyst estimates,
which the primary research suggests can strengthen PEAD.

## Verification

- Targeted SUE tests: 2 passed.
- SUE feature files: Ruff and Python compilation clean.
- Schema `0202`, migration checksums, checkpoint, duplicates, and finiteness: passed.
- Full-suite execution was intentionally avoided.

## Next loop

Research SEC filing-event timing and market reaction as an announcement-surprise proxy. Candidate:
combine SUE with the close-to-close or overnight announcement reaction to distinguish genuinely new
earnings information from seasonally unusual accounting values, while keeping all event timestamps
point-in-time safe.
