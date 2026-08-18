# SpiderRock Mode A conventions — resolved map and residual floor

Resolved by the closed staged convention sweep at `e79f2b8d` over the aggregate
smoke+tune cohorts: 277,952 rows priced, 0 engine errors, 100% selection
coverage on every one of the eleven charter metrics.

This file records what the sweep RESOLVED. It is not a claim that Mode A is
accurate. Read the residual floor below before citing any number here.

## The resolved map

| Group | Key | Resolved value |
|---|---|---|
| Input model | `input_model` | `discrete_forward_pv__rate__sdiv_yield` |
| | `forward_formula` | `uprc_exp_rate_t_minus_ddiv` |
| | `rate_model` | `continuous_row_rate` |
| | `carry_model` | `sdiv_as_yield` |
| | `dividend_model` | `discrete_cash_forward` |
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

`production_conventions` is committed beside `conventions` and the two are
asserted equal. Without that, the receipt would record the map the sweep
resolved but never the map production actually prices with, and the two are
only compared while a sweep is running.

## Why the input model won

Smoke-cohort price MAE per candidate, in ticks (lower is better):

| Candidate | Smoke MAE | Tune MAE |
|---|---:|---:|
| `discrete_forward_pv__rate__sdiv_yield` | **93.65** | **375.51** |
| `discrete_forward_pv__rate_minus_sdiv__zero_carry` | 104.49 | 483.94 |
| `uprc_spot__rate__sdiv_yield` | 126.29 | not staged |
| `discrete_forward_net_carry__rate__sdiv_yield` | 138.09 | not staged |
| `discrete_forward_pv__rate_plus_sdiv__zero_carry` | 139.56 | not staged |
| `discrete_forward__zero_rate__zero_carry` | 332.23 | not staged |
| `discrete_forward__rate_minus_sdiv__zero_carry` | 772.03 | not staged |
| `discrete_forward__rate__sdiv_yield` | 783.65 | not staged |

The staged design is deliberate: only the two candidates that survive smoke are
repriced on tune, because a full tune pass costs ~360 s.

The decisive structural fact is that discounting the discrete dividend to
present value beats subtracting it from the forward undiscounted. Against the
prior map this moved price MAE 421.24 -> 376.06 ticks and volga 132.20 -> 0.52
relative. Volga was not a tuning gain; the prior map had the wrong scale.

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
| `mode_a_price_mae` | 376.06 ticks | 376.06 ticks | <= 1 tick |
| `mode_a_vol_mae` | 0 bp | 0 bp | <= 5 bp |
| `mode_a_delta_rel` | 0.0123 | 0.0133 | <= 0.01 |
| `mode_a_gamma_rel` | 0.0617 | 0.0788 | <= 0.01 |
| `mode_a_theta_rel` | 0.1295 | 12.684 | <= 0.01 |
| `mode_a_vega_rel` | 0.0815 | 0.2134 | <= 0.01 |
| `mode_a_rho_rel` | 0.1144 | 0.8489 | <= 0.01 |
| `mode_a_phi_rel` | 0.1188 | 1.1989 | <= 0.01 |
| `mode_a_volga_rel` | 0.1091 | 0.5228 | <= 0.01 |
| `mode_a_vanna_rel` | 0.0989 | 0.1573 | <= 0.01 |
| `mode_a_delta_decay_rel` | 0.1507 | 1.3189 | <= 0.01 |

**Not one greek meets the charter target.** Price MAE is 376x the target.

`mode_a_vol_mae = 0` is an IDENTITY, not an achievement: Mode A prices AT
`srVol`, so the vol it reports back is the vol it was handed. It becomes a real
measurement only in Mode B, which fits vol from raw NBBO. Citing 0 bp as
evidence of vol accuracy would be a category error.

Theta, rho, phi and delta-decay remain materially wrong under the standard
convention (12.7x, 0.85x, 1.20x, 1.32x). The gap between their symmetric and
standard columns is the signature of a scale or basis error that the symmetric
loss partly absorbs, not of a small numerical residual.

## Accepted regression

The bounded no-regression rule permits a symmetric metric to regress only while
`candidate <= baseline * 1.01`, and every permitted regression is published:

| Metric | Candidate | Baseline | Fraction of baseline |
|---|---:|---:|---:|
| `mode_a_vega_rel` | 0.081468501930500911 | 0.081233446188804986 | 0.002893583280335071 |

One regression, +0.29% of baseline, inside the 1% bound. The bound exists
because the fit is multi-objective over eleven targets sharing one map: no point
in the closed candidate grid strictly dominates every other on all eleven, so a
strict `candidate <= baseline` rule is unsatisfiable by anything the search can
reach. 1% is the charter's own Mode A greeks tolerance, so a permitted
regression cannot flip the verdict of a scorecard cell.

## Speed pin

Measured by `convention_speed_measure` on a quiet host, rel-avx2, 264,026 rows:

- baseline: **3469.47 rows/s**
- pin: **3122 rows/s** = `floor(baseline * 0.90)`

The pin is DERIVED, never copied. A pin equal to the baseline turns the
re-measurement into a coin flip on run-to-run noise, so the 10% margin is part
of the contract and the validator rejects any pin above `baseline * 0.95`.

The sweep's own `diagnostic_speed` (dev preset, 770 rows/s) is marked
`citable: false` and is not the pin.

## Oracle suspects

`oracle_suspect_candidates` is empty and `market_evidence_status` is
`not_evaluated_no_nbbo_gate`. No cell has yet been excluded from the ratchet on
market evidence, because no NBBO gate has run. The oracle is the north star,
not truth; that list stays honest by staying empty until evidence fills it.
