# AR6 adjusted-return consumer correction

Status: implemented and focused validation passed. Independent review found no
Critical issues; its one Important finding is fixed with passing regressions.
No live warehouse or source archive was read.

## Changed behavior

The five assigned consumers use positive finite canonical `adjusted_close`
directly. They no longer reconstruct return prices from `split_factor`, assume
unknown factors equal one, or apply a second cumulative adjustment. The
standalone `signal_eval.compute_forward_returns` raw-price helper and the frozen
legacy compatibility definitions are unchanged.

- `equity_price_metrics` uses adjusted endpoint prices for returns, momentum,
  volatility and its other return-based metrics. Adjusted open is raw open times
  the same row's adjusted-close/raw-close ratio. Dollar turnover remains raw close
  times raw volume. Missing adjustment evidence remains missing, including the
  return immediately after a missing observation (`pct_change(fill_method=None)`).
  Its shared row clock includes prior price and market-proxy inputs. Explicit
  `symbols=()` means no rows; only `symbols=None` requests the entire scope.
- The warehouse forward-target adapter and same-day leakage probe in `signal_eval`
  use adjusted prices. Whole-row revision selection precedes validity filtering;
  invalid latest prices cannot resurrect older valid prices. Invalid observations
  remain in the observation sequence, so they cannot shorten the requested
  horizon. The existing per-security observed-bar horizon convention is retained;
  the separately governed survivorship-safe panel supplies global market-session
  and terminal-event semantics. An empty formation panel still returns no rows.
- Filing reaction uses adjusted-close daily returns and carries both endpoint,
  filing and cross-sectional market input clocks. It selects the first
  post-filing session before testing return validity. Missing evidence no longer
  moves an event to a later valid session. Its lineage names the adjusted-price
  basis rather than representing an unknown split field as an adjustment factor.
- Fundamental momentum and twin momentum use adjusted endpoint ratios for their
  existing 12-1 price control. Invalid observations remain in the session grid,
  and the latest reference session is selected before invalid endpoints are
  excluded. An earlier valid reference cannot replace a missing latest control.
  The existing parent-factor scope, factor IDs and cross-sectional policies remain.

No migration, registration, activation, scheduler, retirement compatibility or
public function signature was changed. The custom feature implementer may keep
using existing `signal_eval` statistical helpers without an API adaptation.

The independent source review found one Important clock defect in the inherited
cohort transforms: rank/regression/standardization could consume another member's
late input while an output retained its earlier individual clock. All three
factor compute paths now carry the maximum admitted cohort clock per decision
date before those transformations. No ranking, regression or scoring formula
changed. Three small pure-frame regressions join the original focused package;
this Important finding requires the implementer's fix report, not a second review.

## Validation

One controller-authorized, guarded run passed all 42 selected cases with no
failures or reruns. The selection was the complete new
`tests/test_adjusted_return_consumers.py` and existing
`tests/test_equity_price_metrics.py`, the raw-helper and corrected warehouse
forward-target cases in `test_signal_eval.py`, the corrected loader and new cohort
clock regression in each momentum file, and the existing pure event-semantics
case plus new cohort clock regression in `test_filing_reaction.py`. It ran with
`-n 0 -q` under `run_memory_guarded.py --job-gb 2`. The guard exited zero;
`adjusted-return-consumers-test-memory.json` records a native process-tree peak
of 0.9327659607 GiB. The updated schema-cache bootstrap accounted for most elapsed
time. There was no concurrent production operation.

The new small-fixture test file covers flat
economic split returns, a cash distribution, ordinary returns, missing/zero/
negative/nonfinite adjusted values, no double adjustment, raw turnover, revision
selection, missing-endpoint horizon preservation, first event-session selection,
empty scopes and prior/market input clocks. Its DuckDB fixtures use one thread
and 128 MB, without warehouse initialization. Four existing source-contract
fixtures now provide canonical adjusted-close evidence explicitly.

Ruff found seven pre-existing findings on unchanged lines: I001/B905/RUF046 in
`equity_price_metrics.py`, and I001/E402 in `test_signal_eval.py`. The other owned
source files and new tests are clean. `git diff --check` passed. No full suite or
live rebuild has run.

## Remaining production limits

This price-column correction is not a memory rewrite or a source certification.
The exact existing unbounded paths remain:

- `equity_price_metrics.load_price_inputs(..., symbols=None)` calls `.df()` for
  the complete bar history, and `compute_equity_price_metrics` makes full pandas
  copies and grouped/merged frames. Do not launch the legacy full-universe job
  without a separately bounded writer or an explicit bounded scope. A per-name
  rewrite alone would change market-proxy and percentile-rank policies; an SQL
  or staged two-pass rewrite must preserve those policies.
- `signal_eval._derive_forward_returns_from_prices(..., panel=None)` returns
  every bar/horizon result as a pandas frame. Supplying formation keys limits
  final output and security scope, but SQL still computes each selected
  security's history. `_derive_same_day_returns_from_prices` also returns all
  contemporaneous returns to pandas for the existing leakage probe.
- Filing reaction computes its median market return in SQL over the bar history;
  momentum filters price histories to parent-factor securities in SQL. Each
  loader still returns its complete selected monthly/event output to pandas.
  Their full-universe output sizes and peak memory have not been measured here.

Historical listing/identity, source vintage and economic adjustment exceptions
remain unresolved source prerequisites. Separate split/share/EPS readers still
need independent split-only event evidence: `derived_compatibility` share and
issuance outputs; earnings/revenue surprise, earnings acceleration and seasonality;
and inferred `corporate_actions`/`adjustment_factors`. The bounded next action is
to measure usable split-event evidence and mark unsupported share-adjusted
outputs ineligible for certification. Dividend-inclusive return factors and
adjusted/raw price ratios are not substitutes for that evidence.
