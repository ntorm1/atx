# Fundamental signal improvement loop 47: q-factor ROE triage

Status: rejected from existing governed evidence; no new backtest run.

## Research and candidate

Hou, Xue, and Zhang's q-factor model uses quarterly ROE as a proxy for expected
profitability and finds that investment and profitability summarize many return
anomalies (https://doi.org/10.1093/rfs/hhu068).

The ATX candidate was the already governed
`profitability_q_factor_roe`. It is point-in-time safe and production-queryable,
so no additional feature build was needed.

## Rapid evidence decision

Stored Loop 18 evidence shows 21-day IC 0.0216 and HAC 2.28, but a **negative
0.861% quintile spread** and only 47.4% spread hit rate. The mean rank relation
therefore does not survive in the extreme portfolio that the production engine
would trade.

Decision: **REJECTED AT EVIDENCE TRIAGE**. A fresh evaluation and costed run
would violate the speed policy without changing eligibility: the candidate
already fails the same positive-spread upstream gate frozen in Loops 40-46. No
mega-alpha registry change was permitted or made.
