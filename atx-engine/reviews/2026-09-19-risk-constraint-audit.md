# Risk and execution audit, 2026-09-19

Scope: portfolio construction, risk constraint dispatch, and backtest execution.
Read-only research preceded the bounded routing change below. No live execution
capability or competitive investment performance is inferred from these fixes.

## Research and review

- [MOSEK benchmark-relative optimization](https://docs.mosek.com/portfolio-cookbook/benchmarkrel.html)
  defines tracking error from the covariance of active weights and models its
  limit explicitly. This supplies an independent measurement for the tracking
  regression, rather than merely checking which implementation was called.
- [MOSEK estimation error](https://docs.mosek.com/portfolio-cookbook/estimationerror.html)
  describes the sensitivity of portfolio weights to estimation error and robust
  optimization approaches. The robust regression below checks the independently
  derived minimizer of a two-name objective.
- [SEC market-access controls FAQ](https://www.sec.gov/files/faq-15c-5-risk-management-controls-bd.htm)
  motivates a future order-control boundary. This is design context, not a claim
  that the current research simulator meets market-access requirements.

## Implemented: standalone constraints bypassed the constrained solver

`PortfolioOptimizer::is_minimal_constraint_set` omitted `track` and `robust`.
Either descriptor by itself therefore ran the fast gross/net/position solver,
discarding the requested cone and bypassing descriptor validation.

`MultiHorizonOptimizer::is_minimal_constraint_set` had a separate, older list
which also omitted participation, ownership, and sector descriptors.

The single-period classifier now recognizes every existing extra descriptor.
The multi-horizon driver delegates to that classifier. Its `run` boundary rejects
participation or ownership caps with `InvalidArgument`, because that driver has
no source of per-period `CapacityRef` data. Silently materializing an empty
reference would still ignore those caps. Single-period capacity support remains
available through `PortfolioOptimizer::ref`.

Focused regression: `tests/risk/risk_constraint_dispatch_test.cpp`, owned by
`atx-engine-risk-tests`, filter `RiskConstraintDispatch.*` (15 tests).

The two-name fixture has exposures `(1,-1)`, factor variance `0.1`, specific
variances `1`, alpha `(0.5,-0.5)`, and risk aversion `0.5`. For neutral weights
`(a,-a)`, variance is `2.4*a^2`. A tracking budget of `0.1` must bind at `0.1`
(the former fast path yields `sqrt(0.6)`, about `0.774597`). With robust radius
`0.3`, the objective is `1.2*a^2 - a + 0.6*abs(a)`, whose minimizer is `a=1/6`
(the former fast path yields `a=1/2`). The tests check these independent values,
sector limits, both horizon forecast modes, invalid standalone cones, and
missing capacity reference rejection.

Adversarial review found three interactions exposed by the corrected routing:
augmented solves discarded the turnover objective cost; multi-horizon augmented
solves also discarded the calibrated gross-capacity ceiling; and a partial
post-solve trade could violate the very constraints the target satisfied.

The QP now uses its existing absolute-turnover epigraph variables for the exact
objective `kappa*sum(abs(w-w_prev))`, independently of any hard turnover budget.
The positive penalty occupies the corresponding augmented linear coefficients,
so ADMM, polish acceptance, and certificates use the same complete objective.
The sparse solver validates nonnegative finite penalties and finite, correctly
sized reference weights. Zero penalties retain the old assembly. The frozen
dense reference solver explicitly rejects this new objective rather than silently
ignoring it. This is the standard proportional-cost modeling approach described
in [MOSEK transaction costs](https://docs.mosek.com/portfolio-cookbook/transaction.html).

Both portfolio and multi-horizon drivers carry their configured turnover costs
into that objective. Multi-horizon materialization also clips gross leverage by
the calibrated capacity when enabled, preserving the opt-out. With nonbinding
tracking and `kappa=0.2`, the analytic two-name optimum is `(0.25,-0.25)` from
flat, versus `(5/12,-5/12)` when ignoring cost. A `0.05` gross capacity caps the
book at `(0.025,-0.025)`. Separate regressions exercise existing positions, a
simultaneous hard turnover budget and robust cone, and objective evaluation.

Nonminimal multi-horizon constraint sets now require `trade_rate=1`; partial
execution returns `InvalidArgument`. Blending toward a feasible target is only
safe if the previous book satisfies the current constraints, which is not
guaranteed for a nonzero tracking benchmark or changing covariance/limits.
The regression checks rejection with benchmark `(0.4,-0.4)`, tracking budget
`0.05`, and partial rate `0.5` in both forecast modes. Minimal unconstrained
partial trading remains supported.

Final acceptance: the coordinator built the final implementation and reported
all 15 `RiskConstraintDispatch.*` regressions passing. The combined risk, data,
and core gate selected 246 tests: 241 passed, zero failed, and five real-data
fixtures were skipped, in 14.31 seconds of CTest wall time. That gate includes
115 risk tests; the separately stopped dense-oracle battery remains incomplete.
Evidence: `build-equity/iteration1-final.xml` and `build-equity/iteration1-final.log`.
The coordinator subsequently corrected only the `qp_augment.hpp` explanatory
auxiliary-variable count comment; no code semantics changed after validation.

## Prioritized next increments

The execution agent implemented the initial outstanding-order finding in this
iteration: `BacktestLoop::rebalance` now replaces remaining target orders instead
of appending duplicate intent, preserving latency for unchanged orders. The
coordinator reported all 54 targeted execution/core tests passing. See the
execution audit for that change's detailed contract and measurements.

1. **Mark positions after execution impact.** `BacktestLoop::on_time_slice`
   marks the portfolio before settlement. Settlement shifts `Market` marks via
   permanent impact, but `settle_at` never refreshes the portfolio marks. The
   equity curve and subsequent rebalance use the old marks until the next bar.
   Refresh valuation after settlement, including same-bar mode. Measure the
   difference between sampled equity and cash plus positions valued at final
   market marks; it must be zero within floating-point tolerance. Target:
   `atx-engine-core-tests`, `BacktestLoop`.
2. **Complete the restored dense-oracle measurement.** The coordinator registered
   six legacy risk files directly under `tests/` into the risk target. During the
   initial routing gate, 103 tests passed; the dense-oracle battery
   `RiskQpAugment.MatchesDenseOracleAcrossBattery` was manually stopped after more
   than five minutes. That battery is incomplete, not a demonstrated regression.
   These initial results predate the turnover/capacity follow-up and do not verify
   its final state. Target: `atx-engine-risk-tests`.

Remaining limits: multi-horizon per-name capacity reference plumbing is not
implemented; augmented partial execution is explicitly unsupported; the legacy
dense-oracle battery still needs a complete measured run.
