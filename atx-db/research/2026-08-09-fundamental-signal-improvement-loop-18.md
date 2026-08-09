# Fundamental signal improvement loop 18: q-factor quarterly ROE

Date: 2026-08-09

## Research question

Does the q-factor model's timely quarterly return-on-equity signal add predictive information beyond
the production operating-profitability/net-issuance router?

## Primary-source basis

- Hou, Xue, and Zhang's q-factor model contains market, size, investment, and return-on-equity
  factors and explains a broad anomaly cross-section:
  <https://academic.oup.com/rfs/article-abstract/28/3/650/1574802>.
- The authors' current technical specification defines ROE as quarterly income before extraordinary
  items divided by one-quarter-lagged book equity and sorts on the latest available ROE at the
  beginning of each month:
  <https://global-q.org/uploads/1/2/2/6/122679606/factorstd_2025feb.pdf>.
- The authors' working paper emphasizes that the model prices portfolios formed on ROE, earnings
  surprise, distress, issuance, and investment:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2152674>.

The published direction, single-quarter frequency, lagged denominator, monthly refresh, and
cross-sectional winsorization were fixed before evaluation.

## Explicit implementation adaptations

The public SEC feed is not Compustat, so exact field identity is impossible:

- reported quarterly `net_income` replaces Compustat income before extraordinary items (`IBQ`);
- reported positive `stockholders_equity` replaces the full quarterly book-equity reconstruction
  using deferred taxes, investment tax credits, and preferred stock;
- financial firms are not excluded because historical point-in-time industry classifications are
  absent. Applying classifications landed only in 2026 would leak current labels backward.

These adaptations are declared in the factor definition and every row's lineage. They are not
silently presented as the exact Compustat recipe.

## Production build

Added `atx_db.quarterly_roe`, migration `0215`, a standalone build CLI, and focused tests. The
feature is `profitability_q_factor_roe`.

For every governed monthly universe decision, the loader selects the most recent visible net-income
duration spanning 70-115 days and a positive visible stockholders-equity instant 60-130 days before
the earnings period end. Earnings can be at most 200 days old. The implementation accepts U.S.
quarterly and annual filings plus foreign-issuer 6-K/20-F/40-F forms; annual filings are eligible only
when they contain a genuine single-quarter duration fact.

The full refresh materializes 92,245 rows across 890 securities and 175 dates from 2012-04-30 to
2026-06-15 in 23.4 seconds. Observed earnings durations are 82-112 days and denominator gaps are
63-122 days. No fact is available after its factor row. Maximum lineage is 1,618 bytes.

Raw ROE has extreme values when positive equity is close to zero (from -17.6 million to 173.5 in
the live sample). Raw observations remain auditable; scoring caps them at the predeclared 1st/99th
cross-sectional percentiles before z-scoring.

## Standalone analysis

Run id: `loop18-q-factor-quarterly-roe-production`. Deciles and split-adjusted forward returns are
used.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0216 | 2.28 | -0.861% | 47.4% | 0.30 |
| 63d | 0.0321 | 2.48 | -2.010% | 47.9% | 0.22 |
| 126d | 0.0403 | 2.31 | -3.745% | 43.4% | 0.15 |
| 252d | 0.0471 | 2.32 | -6.502% | 44.4% | 0.20 |

Rank IC is positive and statistically credible at every horizon. The literal high-minus-low trade is
not: the lowest-ROE decile has anomalously high pooled returns (23.58% at one year), while deciles
2-10 generally rise with profitability. This bottom-tail result is exactly where omitted delisting
returns can create a misleading payoff, so the standalone feature is a predictive rank signal but
not a validated decile long-short strategy.

Top/bottom-decile turnover is 29.1%/35.7%, mean rank autocorrelation is 0.907, and 174 rebalances
are available.

### Subperiod stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0174 | 0.0204 | 0.0161 | 0.0131 |
| 2016-2020 | 0.0122 | 0.0118 | 0.0149 | 0.0314 |
| 2021-2026 | 0.0336 | 0.0608 | 0.0856 | 0.0943 |
| 2023-2026 | 0.0260 | 0.0478 | 0.0709 | 0.0735 |

The signal is positive in every subperiod and materially stronger in the modern period. Recent
HAC t-stats are 2.15/3.66/4.33/4.85 for 2021-2026 and 1.31/2.05/2.40/2.57 for 2023-2026.

### Distinctiveness

Mean cross-sectional correlation is 0.347 with operating profitability, 0.412 with operating cash
flow to assets, 0.094 with cash operating profitability, 0.388 with QMJ profitability, 0.166 with
low net issuance, and 0.180 with Altman. Quarterly ROE is related to existing quality surfaces but
contains substantial independent information.

## Router promotion trial and rollback

A coverage-neutral candidate blended 25% quarterly ROE with 75% operating profitability only when
both were present. Primary-only and issuance-fallback rows were unchanged. This avoids selecting
the weaker ROE-only cohort and preserves all 114,684 exported router rows.

| Router | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| Production primary/fallback | 0.02219 | 0.03509 | 0.05171 | 0.07088 |
| 25% ROE blend | 0.02383 | 0.03772 | 0.05397 | 0.07252 |

The blend improves mean rank IC and is especially constructive in 2021-2026. It fails the trading
tail check:

| Router | 21d Q10-Q1 | 63d Q10-Q1 | 126d Q10-Q1 | 252d Q10-Q1 |
|---|---:|---:|---:|---:|
| Production primary/fallback | 0.357% | 1.259% | 2.981% | 8.262% |
| 25% ROE blend | 0.202% | 0.929% | 2.313% | 7.634% |

The blend also raises top/bottom turnover from 16.2%/19.2% to 18.7%/27.0%, lowers rank
autocorrelation from 0.971 to 0.957, and reduces HAC t-stats. In 2023-2026, its one-year spread is
only 1.39% versus 3.90% for the baseline.

Migration `0216` recorded the attempted promotion. Migration `0217` then restored the stronger
primary/fallback router, and the final source is `atx-db conditional OP/issuance router v3`. This
keeps the failed production trial auditable without leaving weaker live values in place. The final
router refresh takes 24.8 seconds, has compact lineage capped at 500 bytes, and reproduces the
validated 0.02219/0.03509/0.05171/0.07088 IC profile under run id
`loop18-router-v3-restored-production`.

## Decision

Promote `profitability_q_factor_roe` to the production-queryable feature layer as an independently
validated rank signal. Do not use its extreme-low decile as a short leg until delisting-safe return
targets exist. Do not alter the production router: the ROE overlay improves average rank ordering
but weakens the more tradable tails and recent one-year spread.

## Verification

- Two quarterly-ROE tests plus two router regression tests passed serially.
- New and changed files pass Ruff and Python compilation.
- Final schema `0217`; ROE has two direct metric dependencies and the restored router has its two
  original factor dependencies.
- Migration checksums, duplicate-key, finiteness, duration, period-gap, availability, route-count,
  lineage-size, and checkpoint checks passed.
- Full-suite execution was intentionally avoided.

## Next loop

Research change in quarterly ROE. The q5 literature uses year-over-year `dROE`, and the warehouse's
earlier annual diagnostics found improving profitability more robust than profitability levels.
Build an exact four-quarter change with announcement-time lineage and test whether it preserves the
modern rank signal without the extreme-low-level tail pathology.
