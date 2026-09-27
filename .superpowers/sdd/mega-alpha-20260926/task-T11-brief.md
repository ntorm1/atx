# Task T11 — TRAIN-only admission screen `v3-admit-v1` (extension of the T9 fitter)

Declared 2026-09-27 by the root parent BEFORE any library-v3 measurement. Owner: the T9 lane
(`C:/atx-wt/pool-3`), same tool `atx-impl/tools/fit_composition_weights.py` and its fixtures, after T9's
review. Every admission decision is a trial; the tool must emit the full decision table so root can
ledger it.

## Screen (exact, TRAIN 2020-2022 only; never reads validation/2025+)

Per candidate k, from the same per-candidate neutralized daily factor returns `f_k` and standalone
turnover `tau_k` that T9 computes (price-risk-v1 neutralized, gross-1, daily full rebalance):

1. **Orientation:** sign `s_k` = sign of mean `f_k` over 2020-01-01..2021-12-31 (FIT window). If the
   runner's TRAIN orientation artifact disagrees with `s_k`, report it; the screen uses `s_k`.
2. **Turnover:** `tau_k <= 0.70` (over all TRAIN). If above: status `reject_turnover` (the library
   owner may add a smoothed variant in a new library version; never silently dropped).
3. **Stability:** oriented annualized Sharpe of `s_k f_k` > 0 on FIT (2020-21) AND oriented mean of
   `s_k f_k` > 0 on HOLD (2022-01-01..2022-12-31). Else `reject_unstable`.
4. **Redundancy:** process survivors in descending FIT Sharpe order; admit k unless
   |corr(f_k, f_j)| > 0.70 on FIT days for an already-admitted j; else `reject_redundant(j)`.
   Correlation on days where both are finite; require >= 250 common days, else treat as uncorrelated
   and note it.
5. Candidates with fewer than 250 finite FIT days: `reject_insufficient`.

Output: `admission.json` (schema `atx.dsl-admission/v1`: screen id `v3-admit-v1`, inputs + shas, per
candidate: id, s_k, tau_k, FIT Sharpe, HOLD mean and Sharpe, max |rho| and against whom, status) +
`admission.csv`. Deterministic bytes, exclusive output.

The weight fit (`mv-shrink-0.9-nonneg-v1`) then runs over admitted candidates only (weight 0 for the
rest in the pinned-weights JSON), fit on full TRAIN, with `sum_k w_k tau_k` reported as before.

## Fixture

Synthetic: one candidate per rejection class + two admitted, with a correlated pair resolved by FIT
Sharpe; bit-stable output.
