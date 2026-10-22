# Fused price + analytic vega on the (σ, q) boundary family (K1, r5)

Status: internal (`src/pricing/american_sigma_q_family.hpp`, namespace
`atx::vol::sigma_q_family_detail`). No public header changes, and no default policy
routes through it. `AlSigmaQFamily::price` (`price_on`) is still the default evaluation
and is unchanged.

## What it computes

The family stores the transformed Andersen–Lake boundary `y = (ln(B / xmax))²` on a
tensor of Chebyshev nodes: τ (the boundary grid) × q (`n_q`) × σ (`n_sigma`, a log-σ or
σ axis). A price at `(S, K, σ, q)` has five parts:

1. tensor barycentric interpolation of `y` in (σ, q) at each τ node;
2. τ barycentric interpolation at each premium node `t_i`;
3. `B_i = xmax · exp(−√y_i)`;
4. the premium quadrature `Π = ½√T Σ_i w_i 2 z_i (a_i Φ(−d₋) − b_i Φ(−d₊))`, with
   `a_i = r' K' e^{−r' t_i}`, `b_i = q' S' e^{−q' t_i}`, and the log-form
   `d₊ = (ln(S'/xmax) + √y_i + (r' − q') t_i) / v_i + v_i / 2`, where `v_i = σ √t_i`;
5. `P = clamp(E + Π)`, using the `al_put_price_from_boundary` clamp order.

Calls use the internal put coordinates `(S', K', r', q') = (K, S, q, r)`.

`price_and_vega` returns the price and `dP/dσ` of **this map**, the family's own
finite polynomial. That includes the σ-motion of the interpolated boundary. It is not
the converged model's vega (deep review F11). The two are validated separately below.

## The derivative

- **Boundary partials.** `dy/dσ` and `dy/dq` are the interpolant's own derivatives.
  - In σ, the basis derivative is `c_t = Σ_s b_s D1[s][t]`, using the L3g nodal
    differentiation matrix (`sigma_family_detail::diff_tables`, now shared from
    `american_family_greeks.cpp`). There is no quotient rule, so there is no near-node
    cancellation and no 0/0 at an exact node hit: the basis there is `e_hit`, and `c`
    is that row of D1.
  - Chain factors: `dz/dσ = 2 / (u_hi − u_lo) · (1/σ on the log axis)`, and
    `dz/dq = q_scale`.
  - The τ step reuses the `al_cheb_eval` loop on the same weights for `y` and `y'`.
    An exact τ hit returns the node values.
- **The quadrature accumulator.** It runs in the same pass as the price:
  `wz2_i · ((a φ(d₋) d₊ − b φ(d₊) d₋) / σ − (a φ(d₋) − b φ(d₊)) · s'_i / v_i)`, with
  `s_i = √y_i` and `s'_i = y'_i / (2 s_i)`. The first term is the explicit σ-dependence
  (`∂d±/∂σ = −d∓/σ`). The second is the boundary motion (`∂d±/∂s = 1/v`). The total is
  `vega = E_vega + ½√T · Σ` (0 when the premium floors at 0). The European leg is
  `black76_value_and_vega`; the Fast arm is `black76_put_fast` term for term.
- **Exercise.**
  - When the clamp serves intrinsic, the map is flat in σ: `vega = 0`, `exercised = true`.
  - `exercised` also follows the σ-family rule: within `1e-9 K` of intrinsic, or
    `S' ≤ xmax · now_factor`.
  - Otherwise the vega is the slope of the served map, even right next to exercise.
- **`NearNodeDegenerate`.** When some premium node has `|y_i| ≤ 64 ε · max|y|` with
  `y'_i ≠ 0`, `s'` is unbounded: this is the zero of the √ transform. The price is still
  served and the vega is NaN. With `y'_i = 0` the row is not degenerate.

## Stages (plain arrays, no hidden state — K2 lanes these)

| Stage | Depends on | Output |
|---|---|---|
| `State::lay_premium_nodes` | T and the premium order | `PremiumNodes`: t, √t, wz2, zc, kind |
| `carry_at` | side, r, q | `QCarry`: r', q', the `e^{−r't}` / `e^{−q't}` rows |
| `State::boundary_at_nodes` + `finish_node_boundary` | σ, q | `NodeBoundary`: s, s', v, skip, now factor, degenerate flag |
| `premium_pass(pn, c, nb, S', K', fast, want_vega)` | strike | `{price, vega, status, exercised}` |

The only strike-dependent work is the last stage. `price_vega_strikes(f, S, K[], σ, q,
out_price[], out_vega[])` runs stages 1–3 once and the last stage per strike. It is
bit-identical to the per-strike `price_and_vega` calls. Statuses are `Ok`,
`OutsideBox`, `NearNodeDegenerate` and `Unsupported` (`fused_status_of` maps the
family's `AlSigmaQFamilyStatus`). Nothing in the evaluation path allocates
(`AlSigmaQFamilyAlloc.FusedPriceVegaPathAllocatesNothing`).

## Time-grid precontraction (deep review §5.3)

- **`build_premium_table`** makes a self-contained snapshot. It folds the τ
  interpolation into the node tensor once:
  `h[i, j, s] = Σ_k ℓ_k(zc_i) Y[k, j, s]` for every premium node, plus the current-edge
  row. This allocates `(n_prem + 1) · n_q · n_sigma` doubles. A process-unique stamp
  rejects tiles of another build.
- **`contract_q`** contracts the table at one quote q into a fixed-size `QTile` (no
  allocation).
- **`price_vega_on(table, tile, S, K, σ)`** then costs two `n_sigma`-dots per premium
  node plus the premium pass.

**Decision.** A second tile mode was prototyped as the control. It kept the
q-contracted τ-node series and interpolated in τ per call. On vol-rel-avx2 it measured
4.7 µs against 4.0 µs for the precontracted tile on spy23 and spyall (12, 4), 4.2 against
3.6 on spylong (12, 4), and 4.5 against 4.0 on spyall (16, 4). Across all six rows it was
12.5–17.5% slower (the precontracted tile is 11–15% faster), outside the 10% keep band,
so it was **deleted**, together with the table's τ-grid and series copies. Evidence:
`C:\atx-cache\quote-benchmark\K1-20261022-fused\bench\sqfused-1.json` (`con_us_p50`).

## Equivalence and validation (vol-rel, AVX2 host; scalar and AVX2 builds)

- **Fused vs `price_on`.** The SPY probe grid is 300 strikes × 5 σ × 3 q × put/call ×
  `n_sigma` {12, 16}, 18000 rows, 2160 of them exercised. Max |ΔP| = 2.84e-14, and
  max |ΔP| / max(P, 1) = 2.2e-16. The table path agrees with `price_on` and with the
  fused vega to 1.8e-14 scaled. This is within rounding, so no opt-in hash set is needed.
- **Boundary partials** were checked against an adaptive 4th-order Richardson ladder of
  the family's own interpolant, including exact node hits and an ulp off a node. Worst
  relative error on resolved rows: `dy/dσ` 2.1e-11, `dy/dq` 2.1e-11.
- **Family-map vega** was checked against the same ladder on the family's own price map.
  Tolerance is `4 · err + gain · rounding` from the ladder's observed convergence. Rows
  are "resolved" when `tol ≤ 1e-9 |ref|`. The cases cover σ 0.05 / 0.3 / 1.5, small /
  mid / large boxes, calls and puts, a 16-node long expiry, and wide q boxes at the q
  edges.

  | Region (each build) | n | Resolved | Worst rel | Worst gap/tol |
  |---|---|---|---|---|
  | Smooth | 930 | 516 / 511 | 2.38e-11 | 0.061 |
  | Exact σ-node hits | 636 | 226 / 221 | 2.94e-11 | 0.042 |
  | Within one σ-node spacing of exercise (reported) | 274 | 9 / 8 | 8.8e-12 / 3.7e-12 | 0.066 |

  181 clamped rows serve vega exactly 0.
- **Model vega (F11, reported, not pass/fail).** The reference is converged
  Andersen–Lake (32 / 64 nodes, tol 1e-13), with a 4-level Richardson in σ.

  | Case | Fused vs converged | Fused vs the Reference scheme | Reference scheme vs converged |
  |---|---|---|---|
  | spy23 mid put | 3.4e-7 | 2.9e-10 | 2.9e-7 |
  | spy23 mid call | 2.1e-11 | 2.1e-11 | 0 |
  | spylong put | 6.9e-5 | 5.5e-10 | 6.9e-5 |

  The model gap is the Reference quadrature scheme's own distance from convergence, not
  the family's.

## Cost table (vol-rel-avx2, `american/sqfused/*`, `ATX_VOL_AL_SQFUSED=1`)

| Row | spy23 (12, 4) | spy23 (16, 4) | spyall (12, 4), 54 families |
|---|---|---|---|
| `price_on` (default) µs | 6.2 | 6.5 | 6.2 |
| Two-price chord µs | 12.4 | 13.0 | 12.4 |
| Fused price only µs | 4.2 | 4.3 | 4.2 |
| Fused price + vega, one shot µs | 5.0 | 5.4 | 5.0 |
| Price + vega from a table tile µs | 3.5 | 3.9 | 3.5 |
| Strike vector µs / strike | 3.0 | 3.3 | 2.8 |
| Family build µs | 2404 | 3305 | 2383 (125 ms total) |
| Table µs / tile µs | 5.4 / 1.6 | 6.8 / 1.6 | 5.2 / 1.5 |
| Scalar `andersen_lake` price / price + slope µs | 135.5 / 274.6 | 146.1 / 286.1 | 142.9 / 282.9 |
| Break-even evaluations (one shot / table) | 9.3 / 9.7 | 12.2 / 12.6 | 8.5 / 8.8 |
| Bytes: family / table / tile | 6472 / 21456 / 9384 | 8008 / 27728 / 9384 | 6472 / 21456 / 9384 |

Break-even is the number of evaluations per family at which build + N fused calls beat
N scalar price + vega calls. The scalar price + vega is two cold solves, at σ and at
σ + `kAlPackSigmaBump`. These are per-call p50s on a shared host; they are budget
evidence for K2 / K4, not an F1 claim.

Two observations, recorded and not acted on:

- **Is family setup excessive?** A family pays for itself after about 9–12 price + vega
  evaluations.
- **Is an inverse table worthwhile?** The precontracted table plus one tile costs about
  7 µs and saves 1.5 µs per evaluation at a fixed q, so it pays after about 5 evaluations
  per quote q.

The fused price-only stages are also about 1.5× faster than `price_on` while matching it
to 2.2e-16. Whether the fused path becomes the default evaluation is K4's call.
