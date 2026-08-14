# Fundamental signal improvement loop 39: operating leverage

Status: preregistered; no Loop 39 return results inspected at registration time.

## Primary research and hypothesis

Novy-Marx (2011), *Operating Leverage*, shows that operating costs create
systematic operating leverage and reports higher average returns for firms with
higher operating leverage. The primary publication is:

- https://doi.org/10.1093/rof/rfq019

ATX hypothesis: higher annual `(COGS + SG&A) / total assets`, known at the
monthly decision date, predicts higher subsequent US common-equity returns.

## Frozen point-in-time construction

- Reuse the exact annual filing inputs, decision date, availability, and governed
  universe already recorded by `profitability_operating_profitability`.
- Operating costs equal reported COGS plus reported SG&A. When direct COGS is
  unavailable, `revenue - gross_profit` is the algebraically identical fallback.
- Total assets must be positive. COGS/resolved COGS and SG&A must be positive and
  finite. No absent expense is replaced with zero.
- Reject raw operating-cost/assets ratios outside `(0,10]` as unit/taxonomy
  discontinuities; this guard is fixed before returns.
- Cross section: monthly 1%/99% winsorization and sample z-score; minimum 20
  names. Higher operating leverage receives the higher score.

The pre-return coverage audit found 37,545 eligible observations across 453
securities and 174 monthly dates from 2012-03-30 through 2026-06-15, with
23--360 names per date.

## Frozen sequential evaluation

Stage 1 evaluates adjusted-price rank IC and HAC inference at 21, 63, 126, and
252 trading days. Stage 2 is allowed only when 252-day IC is positive and HAC
t-statistic is at least 2.0.

If Stage 1 passes, Stage 2 additionally requires a positive 252-day top-minus-
bottom quintile spread, at least 20 names on at least 36 monthly dates, and zero
PIT, key, value, or lineage violations. Only then may the costed Polars
`atx-factor` walk-forward run. Mega-alpha admission remains frozen at candidate
OOS Sharpe >= 0.50, deflated Sharpe probability >= 0.95, at least +0.05 Sharpe
improvement for an 80/20 blend, positive stressed blend Sharpe, participation in
every valid fold, and passing deployment/turnover gates.

## Production implementation and quality

Migration `0241` governs `risk_operating_leverage` as a direct dependency of the
governed annual operating-profitability factor. `atx_db.operating_leverage`
extracts exact filing values and source IDs from parent lineage; it uses reported
COGS on 34,496 rows and the algebraically identical revenue-minus-gross-profit
fallback on 3,012 rows. No missing expense is zero-imputed.

The live build produced 37,508 rows in 9.63 seconds across 452 securities and
174 monthly dates from 2012-03-30 through 2026-06-15. Breadth is 23--360. IDs
and natural keys are unique; raw values are within the frozen `(0,10]` range;
scores and JSON are valid; availability is PIT; and date-wise standardized
moments are exact to floating-point tolerance. The focused dual-path COGS test
passes.

## Stage 1 results and decision

Run id: `loop39-operating-leverage-screen`. Wall time: 6.57 seconds.

| Horizon | Rank IC | HAC t-stat | Sign consistency | Dates | Mean names |
|---:|---:|---:|---:|---:|---:|
| 21 | -0.00421 | -0.42 | 50.29% | 171 | 214.4 |
| 63 | 0.00088 | 0.06 | 54.44% | 169 | 212.4 |
| 126 | -0.00007 | -0.00 | 48.19% | 166 | 209.5 |
| 252 | 0.00641 | 0.27 | 50.62% | 160 | 203.4 |

Decision: **reject Loop 39 from the mega-alpha portfolio**. The one-year HAC
t-statistic is 0.27 versus the required 2.0 and shorter-horizon ICs oscillate
around zero. Stage 2 and `atx-factor` are prohibited; router v6 remains unchanged.
