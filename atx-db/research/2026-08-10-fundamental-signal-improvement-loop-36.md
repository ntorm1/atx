# Fundamental signal improvement loop 36: operating-cash-flow enterprise yield

Status: preregistered; no Loop 36 return results inspected at registration time.

## Research basis and hypothesis

Lakonishok, Shleifer, and Vishny (1994), *Contrarian Investment,
Extrapolation, and Risk*, study cash-flow-to-price among value strategies.
Loughran and Wellman (2011) show that a low enterprise multiple predicts higher
average returns. Primary publication links:

- https://doi.org/10.1111/j.1540-6261.1994.tb04772.x
- https://doi.org/10.1017/S0022109011000445

ATX preregisters `TTM operating cash flow / enterprise value`: a cash-flow yield
whose denominator prices both equity and net debt. This exact synthesis is an
ATX inference, not a formula attributed to either paper. Higher values are
hypothesized to predict higher subsequent US common-equity returns.

## Frozen construction

- Numerator: the exact positive TTM operating-cash-flow value and source ID in
  governed `profitability_operating_cash_flow_to_assets` lineage.
- Denominator: positive component-lineaged enterprise value on the exact same
  monthly decision date.
- Both inputs must be visible by that decision close. Maximum fundamental age is
  550 days; missing, stale, nonpositive, non-finite, or unlineaged inputs are
  omitted and never imputed.
- Universe: the PIT `us_common_equity_liquid_v1` membership inherited from the
  parent factor, joined by immutable `security_id`.
- Cross section: 1%/99% winsorization, sample z-score by date, minimum 20 names.

## Frozen sequential gates

Stage 1 uses the new bounded `--screen-only` evaluator at 21, 63, 126, and 252
trading days. The candidate proceeds only if 252-day IC is positive and its HAC
t-statistic is at least 2.0. This early gate is operational, not discretionary.

Only after Stage 1 passes may Stage 2 compute quintile spreads, monotonicity,
turnover, and breadth. Stage 2 additionally requires a positive 252-day top-
minus-bottom spread, at least 20 names on at least 36 dates, and zero PIT/key/
value/lineage violations.

Only after both stages pass may `atx-factor` run. Mega-alpha admission remains
frozen at OOS Sharpe >= 0.50, deflated Sharpe probability >= 0.95, at least
+0.05 Sharpe improvement for an 80/20 blend, positive stressed blend Sharpe,
participation in every valid fold, and passing deployment/turnover gates.

## Production implementation and quality

Migration `0238` governs `valuation_operating_cash_flow_enterprise_yield` and
records direct dependencies on enterprise value, the parent cash-flow factor,
and the governed universe. The shared lineaged-enterprise-yield SQL path now
handles both annual gross profit and TTM operating cash flow without duplicating
the materializer. It extracts the exact parent value/period/source ID, performs
same-date EV joining, validity and age gates, winsorization, sample z-scoring,
JSON lineage, hashing, and insertion inside DuckDB.

The live partition contains 18,155 unique rows across 209 securities and 172
monthly dates from 2012-04-30 through 2026-06-15. Breadth is 25--155 names per
date. There are zero duplicate IDs/natural keys, nonpositive or non-finite raw
values, non-finite scores, future-availability rows, or invalid JSON rows.
Per-date standardized means are within 1.35e-15 of zero and sample standard
deviations within 5.56e-16 of one.

The evaluator now exposes a tested `--screen-only` mode that persists IC/HAC and
decay, then returns before quantiles, turnover, correlation, and breadth. This
implements the frozen sequential protocol and avoids unnecessary portfolio work
for failed candidates.

## Stage 1 results and decision

Run id: `loop36-operating-cash-flow-enterprise-yield-screen`.

| Horizon | Rank IC | HAC t-stat | Sign consistency | Dates | Mean names |
|---:|---:|---:|---:|---:|---:|
| 21 | 0.00284 | 0.20 | 49.41% | 170 | 104.8 |
| 63 | -0.00862 | -0.39 | 55.95% | 168 | 103.8 |
| 126 | -0.00088 | -0.03 | 52.73% | 165 | 102.5 |
| 252 | 0.01139 | 0.27 | 49.06% | 159 | 99.6 |

Decision: **reject Loop 36 from the mega-alpha portfolio**. The one-year IC has
the expected sign, but HAC t=0.27 is far below the frozen 2.0 gate, while the
63- and 126-day ICs are negative. Stage 2 and `atx-factor` are therefore
prohibited; router v6 remains unchanged.
