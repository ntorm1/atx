# Polars factor engine: loop-31 production validation

Date: 2026-08-10

## Scope and architecture

`atx-factor` is the standalone Polars research and portfolio-admission layer between governed
`atx-db` signals and the event-driven `atx-engine`. It consumes a canonical point-in-time long
panel, validates knowledge and label timestamps, builds deterministic continuous rank books, and
emits a fail-closed mega-alpha decision. It does not own warehouse migrations or execution.

The implementation is vectorized in Polars. Capped proportional allocation uses the exact sorted
water-fill solution `w_i = min(cap_i, lambda * |score_i|)` rather than iterative eager passes.
Dollar neutrality is preserved under capacity shortfalls by applying the same attainable budget to
both sides. Relative floating-point centering residue is canonicalized to zero, including odd-sized
rank cross-sections, and genuinely flat cross-sections produce flat books.

The default cost and capacity contract is:

- $100 million AUM, gross 1.0, 5% name cap, and at least 20 names;
- 0.25 bp one-way commission, 2 bp half spread, 10 bp square-root impact, and 50 bp annual borrow;
- 10% maximum ADV participation, with desired positions capped at one quarter of that ceiling to
  reserve capacity for a full signal flip and approximate 2x liquidity deterioration;
- doubled-cost stress, explicit realized participation and minimum-gross gates, and no hidden
  rescaling of an infeasible book.

Chronological evaluation uses an expanding 60-period training history, a one-period embargo, and
eight non-overlapping 12-period OOS folds. Admission requires candidate OOS Sharpe, a 90% deflated
Sharpe probability after 32 recorded trials, positive marginal blended Sharpe, positive stressed
Sharpe, turnover/drawdown/correlation limits, and capacity compliance. The decision artifact
includes every setting and fold, is content-digested, and is written atomically. Only accepted
decisions can enter the atomic mega-alpha registry.

Implementation choices follow the official [Polars streaming execution
model](https://docs.pola.rs/user-guide/concepts/streaming/) and [DuckDB Polars integration
contract](https://duckdb.org/docs/current/guides/python/polars). The multiple-testing gate follows
Bailey and Lopez de Prado's [Deflated Sharpe Ratio](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551).

## Live loop-31 evaluation

Candidate: `profitability_quarterly_operating_profitability_change_yoy`  
Baseline: `composite_operating_profitability_or_net_issuance` (production router v6)  
Artifact: `research/loop31-profitability-change-mega-alpha-decision.json`  
Evidence SHA-256: `9bf020c09af3b432b92c3ec6e9d79bedfc079f85390412905dde365cc7c0631e`

The live run evaluated 96 monthly OOS observations in eight disjoint folds. End-to-end runtime was
17.1 seconds; the exact allocator reduced the earlier iterative path's approximately 122 seconds by
about 7x.

| Portfolio | Sharpe | Annual return | Total return | Max drawdown | Annual cost | Turnover |
|---|---:|---:|---:|---:|---:|---:|
| Candidate | -0.429 | -2.202% | -16.590% | -27.888% | 0.422% | 0.239 |
| Router v6 | 0.629 | 2.138% | 18.809% | -9.004% | 0.332% | 0.118 |
| 80/20 blend | 0.449 | 1.631% | 14.088% | -8.388% | 0.351% | 0.147 |
| 80/20 blend, doubled costs | 0.357 | 1.276% | 10.882% | -8.620% | 0.701% | 0.147 |

Candidate DSR probability is 0.000224, correlation with the baseline is 0.260, and marginal blended
Sharpe is -0.180. Minimum gross deployment is 48.5% for the candidate and 83.8% for the blend;
maximum blended participation is 12.9%, above the 10% ceiling.

Decision: **reject the candidate from the mega-alpha**. It fails standalone Sharpe, DSR, marginal
Sharpe, participation, and gross-deployment gates. No registry mutation was made. The governed
factor remains available for monitoring and future conditional/nonlinear research.

## Verification

- Ruff passes for the package.
- Fourteen focused tests cover schema fail-closed behavior, deterministic and capacity-constrained
  allocation, costs, chronological fold isolation, mega-alpha admission/rejection, non-finite JSON
  normalization, and atomic registry behavior.
- The live decision was regenerated after the allocator refactor; the artifact timestamp and digest
  changed and all eight fold records are embedded in the result.
