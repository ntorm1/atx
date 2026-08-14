# Fundamental signal improvement loop 50: strict net-payout triage

Status: rejected from existing governed evidence; no new backtest run.

## Research and candidate

Boudoukh, Michaely, Richardson, and Roberts define net payout yield as common
dividends plus repurchases less equity issuance, divided by market equity, and
find information beyond dividend yield in the cross-section of expected returns
(https://doi.org/10.1111/j.1540-6261.2007.01226.x).

ATX already has the governed PIT feature `financing_net_payout_yield`, with a
strict same-filing complete-case policy. Missing XBRL payout components are not
silently zero-imputed. The feature layer, dependency graph, standalone CLI, and
targeted tests therefore already satisfy the production implementation need.

## Evidence correction and decision

The apparently strong stored result belonged to an equal-weight router/payout
prototype on a narrow common cohort. It was not standalone net-payout evidence,
and the router itself dominated that blend at every horizon.

The actual standalone surface has only 3,845 rows, 92 securities, and 141 dates.
Its 21/63/126/252-day ICs are positive, but HAC t-statistics are only
0.98/0.89/1.17/1.18 and mean breadth is roughly 23 names. It therefore fails the
same HAC >=2 upstream rule used by the current admission protocol.

Decision: **REJECTED AT EVIDENCE TRIAGE**. No redundant $50 million backtest was
run, no imputation policy was weakened, and no mega-alpha registry change was
permitted or made. The strict feature remains queryable; broader payout coverage
requires governed statement-completeness evidence or an additional source.
