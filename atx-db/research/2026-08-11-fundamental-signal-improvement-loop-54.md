# Fundamental signal improvement loop 54: production replacement challenge

Status: preregistered engine extension and operating-profitability replacement
test; no Loop 54 replacement result inspected at registration time.

## Research and decision problem

Harvey, Liu, and Zhu show that the factor zoo demands materially higher
significance hurdles under multiple testing
(https://doi.org/10.1093/rfs/hhv059). Bailey and Lopez de Prado's deflated
Sharpe ratio explicitly corrects strategy evidence for selection bias,
non-normality, and repeated trials
(https://doi.org/10.2139/ssrn.2460551).

Loop 53 found that `profitability_operating_profitability` has higher standalone
OOS Sharpe than router v6 but correctly fails additive admission because the two
are highly correlated. Correlation is evidence against a second sleeve, not
against replacing an inferior implementation. The production engine therefore
needs a separate replacement decision rather than weakening the additive gate.

## Frozen engine contract

Add a Polars-native `evaluate_replacement` API and `evaluate-replacement` CLI.
It must independently construct and chronologically evaluate challenger and
incumbent on their common formation dates, preserve each factor's own asset
coverage, apply identical costs and constraints, run a doubled-cost challenger
stress, emit an immutable checksummed decision, and never mutate production.

Replacement is accepted only if all gates pass:

- challenger OOS Sharpe >=0.50;
- challenger deflated-Sharpe probability >=0.95 with 32 declared trials;
- challenger-minus-incumbent Sharpe >=0.05;
- doubled-cost challenger Sharpe >0;
- challenger turnover <=0.70 and absolute drawdown <=0.25;
- challenger minimum gross deployment >=0.95;
- challenger maximum ADV participation <=0.10;
- at least three valid expanding 60/12/12 folds with one-period embargo.

No correlation gate applies because replacement, unlike addition, does not add
duplicated exposure. Additive `AcceptanceGate` behavior remains unchanged.

## Frozen production challenge

Challenger: `profitability_operating_profitability`.
Incumbent: `composite_operating_profitability_or_net_issuance` (router v6).
AUM is $50 million from Loop 45, with the existing 21-day horizon, gross 1.0,
5% name cap, minimum 20 names, 0.25 bps commission, 2 bps half spread, 10 bps
impact, 50 bps borrow, and 10% participation ceiling.

The immutable target is
`C:\atx\atx-factor\research\loop54-operating-profitability-replacement.json`.

## Implementation and bounded result

Implemented `ReplacementGate`, `ReplacementDecision`, and
`evaluate_replacement` in the separate Polars engine, plus the compact
`evaluate-replacement --summary-only` operator path. The additive gate was not
changed. Ten focused replacement, additive-decision, and CLI tests pass, and
Ruff passes on every touched source and test file.

The production challenge was capped at 45 seconds. It exceeded the cap before
serializing the target artifact, and the terminated worker left no artifact or
warehouse mutation. It was not rerun. This is an operational SLA failure, not
a fabricated backtest result.

The immediately preceding immutable Loop 53 run already contains the exact
independently built challenger and incumbent results at the same $50 million
cost, portfolio, horizon, walk-forward, and 32-trial settings. Its governed
evidence hash is
`aa62233e2897a1409cb7f9999e8ed01736e215159177477f163800d33cdd0139`:

- challenger OOS Sharpe 0.6652 versus incumbent 0.5352, improvement +0.1300;
- challenger deflated-Sharpe probability 0.4659 versus the frozen 0.95 floor;
- nine folds, turnover 0.1012, drawdown -0.0812, minimum gross deployment
  0.9877, and maximum participation 0.0939.

## Decision

**REJECT `profitability_operating_profitability` as the router-v6 replacement.**

The challenger clears raw Sharpe, improvement, turnover, drawdown, deployment,
participation, and fold-count gates, but its 0.4659 deflated-Sharpe probability
is far below 0.95. That necessary failure makes acceptance impossible even
without the timed-out doubled-cost diagnostic. Production remains unchanged.

The next engine iteration should make replacement evaluation sequential: stop
and serialize a checksummed rejection as soon as an absolute challenger gate
fails, avoiding unnecessary incumbent and stress passes while preserving every
acceptance requirement.
Even if accepted, a separate governed atx-db migration and verification would
be required before the production router changes.
