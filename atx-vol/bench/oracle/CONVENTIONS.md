# SpiderRock Mode A conventions — resolved map and residual floor

Resolved by the closed staged convention sweep at `54024add` over the aggregate
smoke+tune cohorts: 277,952 rows priced, 0 engine errors, 100% selection
coverage on every one of the eleven charter metrics. The dividend-schedule
pre-pass saw 30 snapshot groups and refused none.

This file records what the sweep RESOLVED. It is not a claim that Mode A is
accurate. Read the residual floor below before citing any number here.

## The resolved map

| Group | Key | Resolved value |
|---|---|---|
| Input model | `input_model` | `discrete_dividend_tree__rate__sdiv_yield` |
| | `forward_formula` | `none` |
| | `rate_model` | `continuous_row_rate` |
| | `carry_model` | `sdiv_as_yield` |
| | `dividend_model` | `discrete_cash_schedule` |
| Exercise | `exercise_style` | `european_cash_settled_index` |
| Time decay | `time_decay_method` | `analytic_derivative` |
| Clock | `day_count` | `BUS_252` |
| | `dte_banding_day_count` | `ACT_365F` |
| Price | `price_scale` / `price_sign` | `per_share` / `positive` |
| Vol | `vol_scale` | `decimal_identity` |
| Delta | `delta_scale` / `delta_sign` | `per_unit` / `positive` |
| Gamma | `gamma_scale` / `gamma_sign` | `per_unit` / `positive` |
| Theta | `theta_basis` / `theta_sign` | `per_day` / `positive` |
| Vega | `vega_scale` / `vega_sign` | `per_point` / `positive` |
| Rho | `rho_scale` / `rho_sign` | `per_point` / `positive` |
| Phi | `phi_scale` / `phi_sign` | `per_point` / `positive` |
| Volga | `volga_source` / `volga_scale` / `volga_sign` | `volga` / `per_point_squared` / `positive` |
| Vanna | `vanna_source` / `vanna_scale` / `vanna_sign` | `vanna` / `per_point` / `positive` |
| Delta decay | `delta_decay_basis` / `delta_decay_day_count` / `delta_decay_sign` | `per_day` / `BUS_252` / `positive` |

The map now carries all 33 keys: the exercise-style axis (85797d0f) and the
time-decay axis (635f8bd8) are explicit, never defaulted. `secant_252` is
plumbed but the selector cannot yet falsify it — both decay arms tie on every
key and `analytic_derivative` wins on the field-by-field identity tie-break,
not on evidence (see the ledger entry at 635f8bd8).

`production_conventions` is committed beside `conventions` and the two are
asserted equal. Without that, the receipt would record the map the sweep
resolved but never the map production actually prices with, and the two are
only compared while a sweep is running.

## Why the input model won

Smoke-cohort price MAE per input model, in ticks (lower is better; each model
fans into 6 tied stage-1 arms across the two non-price axes):

| Input model | Smoke MAE | Tune-sample MAE (best arm) |
|---|---:|---:|
| `discrete_dividend_tree__rate__sdiv_yield` | **4.59** | **8.97** |
| `discrete_forward_pv__rate__sdiv_yield` | 93.65 | 39.76 |
| `discrete_forward_pv__rate_minus_sdiv__zero_carry` | 104.49 | not staged |
| `uprc_spot__rate__sdiv_yield` | 126.29 | not staged |
| `discrete_forward_net_carry__rate__sdiv_yield` | 138.09 | not staged |
| `discrete_forward_pv__rate_plus_sdiv__zero_carry` | 139.56 | not staged |
| `discrete_forward__zero_rate__zero_carry` | 332.23 | not staged |
| `discrete_forward__rate_minus_sdiv__zero_carry` | 772.03 | not staged |
| `discrete_forward__rate__sdiv_yield` | 783.65 | not staged |

The staged design is deliberate: only the top-two input models' full tied fans
(12 finalists of the 54-candidate grid) are repriced on the 33,004-row tune
sample.

The decisive structural fact is that pricing on the discrete-dividend lattice
with the reconstructed cash schedule (spot = `uPrc`, nothing escrowed,
`rate = row.rate`, `q = row.sdiv` kept) beats every escrowed/forward-adjusted
Black arm. Exercise style resolved on evidence, not tie-break: the tree's
`american_all` arm scored 344.72 ticks on the tune sample against 8.97 for the
European-rule arms. Against the prior committed map this moved pooled price MAE
40.54 -> 8.66 ticks (4.7x) and every greek except volga improved.

## Residual floor — read this before citing anything above

Two error conventions are committed side by side and they must never be
unified:

- **symmetric** `|m-o| / max(|m|,|o|,floor)` — the loss the scale selection
  minimises, and therefore the RATCHET BASELINE and the no-regression
  criterion. It is bounded and has no smallest-scale gradient.
- **standard relative** `|m-o| / max(|o|,floor)` — committed unchanged only so
  the floor stays directly comparable to the charter's "greeks within 1% rel"
  target. It is never the gated criterion.

| Metric | Symmetric | Standard rel | Charter target |
|---|---:|---:|---|
| `mode_a_price_mae` | 8.6633 ticks | 8.6633 ticks | <= 1 tick |
| `mode_a_vol_mae` | 0 bp | 0 bp | <= 5 bp |
| `mode_a_delta_rel` | 0.0022 | 0.0022 | <= 0.01 |
| `mode_a_gamma_rel` | 0.0179 | 0.0318 | <= 0.01 |
| `mode_a_theta_rel` | 0.0639 | 7.3116 | <= 0.01 |
| `mode_a_vega_rel` | 0.0240 | 0.0739 | <= 0.01 |
| `mode_a_rho_rel` | 0.0217 | 0.6926 | <= 0.01 |
| `mode_a_phi_rel` | 0.0245 | 0.9976 | <= 0.01 |
| `mode_a_volga_rel` | 0.0831 | 0.1875 | <= 0.01 |
| `mode_a_vanna_rel` | 0.0277 | 0.0341 | <= 0.01 |
| `mode_a_delta_decay_rel` | 0.0903 | 0.8085 | <= 0.01 |

**Delta now meets the charter target; nothing else does.** Price MAE is 8.7x
the target — down from 376x at the previous floor. This regeneration RESETS the
ratchet: these values replace the escrow-era floors as the numbers a later
iteration must not be worse than.

`mode_a_vol_mae = 0` is an IDENTITY, not an achievement: Mode A prices AT
`srVol`, so the vol it reports back is the vol it was handed. It becomes a real
measurement only in Mode B, which fits vol from raw NBBO. Citing 0 bp as
evidence of vol accuracy would be a category error.

Theta, rho, phi and delta-decay remain materially wrong under the standard
convention (7.3x, 0.69x, 1.00x, 0.81x). The gap between their symmetric and
standard columns is the signature of a scale or basis error that the symmetric
loss partly absorbs, not of a small numerical residual. Volga is the one greek
the tree did not win (see the lattice-volga ledger entries at e9e6a306).

## Accepted regression

The bounded no-regression rule permits a symmetric metric to regress only while
`candidate <= baseline * 1.01`, and every permitted regression is published.
This sweep regressed on NOTHING: every symmetric metric is at or below its
baseline, and `accepted_regressions` is committed empty. The bound itself
stands unchanged — the fit is multi-objective over eleven targets sharing one
map, and 1% is the charter's own Mode A greeks tolerance.

## Speed pin

Measured by `convention_speed_measure` on a quiet host, rel-avx2, 264,026 rows:

- baseline: **9958.75 rows/s**
- pin: **8962 rows/s** = `floor(baseline * 0.90)`

The pin is DERIVED, never copied. A pin equal to the baseline turns the
re-measurement into a coin flip on run-to-run noise, so the 10% margin is part
of the contract and the validator rejects any pin above `baseline * 0.95`.

The sweep's own `diagnostic_speed` (rel-avx2 full attribution, 2646.7 rows/s
over 105.0 s) is marked `citable: false` and is not the pin.

## Oracle suspects

`oracle_suspect_candidates` is empty and `market_evidence_status` is
`not_evaluated_no_nbbo_gate`. No cell has yet been excluded from the ratchet on
market evidence, because no NBBO gate has run. The oracle is the north star,
not truth; that list stays honest by staying empty until evidence fills it.
