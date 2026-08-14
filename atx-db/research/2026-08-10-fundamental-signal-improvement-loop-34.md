# Fundamental signal improvement loop 34: enterprise yield

Status: preregistered; no return results inspected at registration time.

## Primary literature and hypothesis

Loughran and Wellman (2011), *New Evidence on the Relation between the
Enterprise Multiple and Average Stock Returns*, define the enterprise multiple
as `(equity value + debt + preferred stock - cash) / EBITDA` and report that low
enterprise-multiple firms subsequently outperform high-multiple firms. The
published article and DOI are:

- https://doi.org/10.1017/S0022109011000445
- https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/abs/new-evidence-on-the-relation-between-the-enterprise-multiple-and-average-stock-returns/5CD22A12A06AFCDC5233E477757FB659

ATX hypothesis: a high enterprise yield (`EBIT / enterprise_value`) predicts
higher subsequent US common-equity returns. This is the sign-equivalent inverse
of a low `EV / EBIT` multiple and is easier to winsorize than an unbounded ratio.

## Pre-analysis data decision

The production coverage audit was performed before any return analysis:

- `ev_to_ebit`: 18,870 2026 rows / 167 securities;
- `ev_to_sales`: 20,565 2026 rows / 183 securities;
- `ev_to_ebitda`: 173 2026 rows / 2 securities because the current normalized
  depreciation/amortization input is sparse.

Therefore:

1. Primary candidate: `enterprise_yield_ebit = operating_income_ttm / enterprise_value`.
2. Secondary candidate: `enterprise_yield_sales = revenue_ttm / enterprise_value`.
3. `EBITDA / EV` is coverage monitoring only and cannot be substituted after
   seeing returns.

## Point-in-time construction

- Source rows: `valuation_multiples`, production source
  `derived_valuation_multiples_v1`.
- Primary source formula: reciprocal/sign transform of `ev_to_ebit`; require
  positive finite enterprise value and positive finite operating income.
- Secondary source formula: reciprocal/sign transform of `ev_to_sales`; require
  positive finite enterprise value and positive finite revenue.
- A row is usable only when `available_at` is no later than the rebalance
  decision timestamp. No restated value may be backcast before availability.
- Join returns by immutable `security_id`, never ticker alone.
- Universe: point-in-time `us_common_equity_liquid_v1` membership where available.
- Monthly sampling: final eligible trading observation in each calendar month;
  at most one signal per security/month.
- Cross section: 1%/99% winsorization followed by date-wise z-score.
- Minimum breadth: 20 eligible names per date. Dates below the gate are omitted,
  not imputed.
- Existing `FAMA_FRENCH_12` rows are unavailable before 2026-08-09 and therefore
  cannot be used to neutralize this historical test without lookahead.

## Upstream research gates

Evaluate forward total returns at 21, 63, 126, and 252 trading days using:

- mean daily cross-sectional Spearman IC;
- Newey-West/HAC t-statistics with horizon-appropriate overlapping-return lags;
- equal-weight top-minus-bottom quintile spread;
- quintile monotonicity;
- row, security, and date coverage.

The primary candidate advances to `atx-factor` only if all are true:

1. 252-day IC is positive;
2. 252-day HAC t-statistic is at least 2.0;
3. 252-day top-minus-bottom spread is positive;
4. at least 20 eligible names exist on at least 36 monthly dates;
5. no PIT, key-uniqueness, non-finite-value, or lineage violation is present.

The secondary candidate is evaluated independently under the same gates and
cannot rescue a failed primary through post-hoc blending.

## Mega-alpha admission gates

If an upstream candidate passes, run the governed expanding-window walk-forward
test in `atx-factor` with transaction costs and the frozen mega-alpha v6 baseline.
Admission requires all of:

1. candidate OOS Sharpe >= 0.50;
2. deflated Sharpe ratio probability >= 0.95;
3. 80/20 candidate blend improves baseline Sharpe by >= 0.05;
4. stressed blend Sharpe remains positive;
5. candidate participates in every valid OOS fold;
6. gross deployment and turnover gates pass;
7. evidence digest and acceptance decision are persisted.

Failure at either stage is an explicit rejection, not a reason to alter this
protocol.

## Production implementation completed

- Replaced the market-cap refresh with a set-based DuckDB path. A read-only 2026
  benchmark fell from roughly 95 seconds/year on the prior pandas path to 1.91
  seconds for 83,675 rows; the live atomic load took 11.18 seconds including
  initialization, validation, and checkpointing.
- Restored seven already-cached SEC companyfacts concepts to
  `fundamental_fact_revisions`, including current/noncurrent debt, preferred
  stock, and minority interest.
- Replaced the enterprise-value pandas refresh with component-lineaged SQL using
  the governed debt hierarchy. The live warehouse now contains 584,114 unique EV
  rows from 2012-03-26 through 2026-06-15 across 258 securities, with no non-finite
  value or availability violation.
- Replaced valuation-multiple assembly and formula expansion with vector stages.
  The 2026 production partition contains 635,714 unique rows across 15 populated
  formulas and 728 securities. Required fields, finiteness, arithmetic,
  meaningfulness, uniqueness, and availability checks all report zero violations.
  `ev_to_assets` remains unpopulated because strict same-period EV/assets overlap
  is absent; no cross-period imputation was introduced.
- Migration `0236` governs the two Loop 34 factors and their dataset, metric, and
  universe dependencies.
- The production feature materialization contains 28,321 rows. EBIT/EV has
  15,629 rows, 193 securities, and 172 monthly dates; Sales/EV has 12,692 rows,
  216 securities, and 141 monthly dates. Factor IDs and natural keys are unique,
  all values are finite, and no `available_at` exceeds the decision date.

## Governed evaluation results

Run id: `loop34-enterprise-yield-eval`. Return target: adjusted prices. Quantiles:
five. No industry neutralization was applied because the available industry
classification cannot be backcast without lookahead.

### Primary: operating-income enterprise yield

| Horizon | Rank IC | HAC t-stat | Q5-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21 | 0.00076 | 0.06 | 0.116% | 55.29% | 0.10 |
| 63 | 0.00210 | 0.12 | 0.375% | 48.81% | 0.30 |
| 126 | 0.00755 | 0.35 | 1.526% | 50.30% | 0.30 |
| 252 | 0.01646 | 0.54 | 4.096% | 55.35% | 0.30 |

Mean breadth is 90.9 names across 172 dates (minimum 23, maximum 152). Top and
bottom quintile turnover are 27.80% and 28.16%; mean rank autocorrelation is
0.9708. The one-year sign and spread are positive, but the frozen inference gate
fails decisively: HAC t=0.54 versus the required 2.0.

### Secondary: revenue enterprise yield

| Horizon | Rank IC | HAC t-stat | Q5-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21 | -0.00167 | -0.11 | 0.406% | 54.68% | 0.00 |
| 63 | -0.02081 | -0.89 | 1.143% | 46.72% | 0.30 |
| 126 | -0.02613 | -0.68 | 3.329% | 54.48% | 0.40 |
| 252 | -0.03964 | -0.70 | 5.390% | 54.69% | 0.90 |

Mean breadth is 90.0 names across 141 dates. Despite positive pooled quintile
spreads, the date-wise rank IC is negative at every horizon and therefore fails
the preregistered sign gate.

## Mega-alpha decision

Decision: **reject both Loop 34 candidates from the mega-alpha portfolio**.

The primary fails the required one-year HAC threshold, and the secondary fails
the one-year IC sign as well as inference. Under the frozen protocol neither is
allowed to enter the costed `atx-factor` walk-forward stage; running that stage
after an upstream failure would be a post-hoc search. Router v6 and the
mega-alpha registry remain unchanged. The production EV, valuation, and feature
surfaces remain available for future research and downstream consumers.
