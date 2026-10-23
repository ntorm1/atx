# The primary Greek bundle on the (σ, q) boundary family (A0, r6)

Status: public, opt-in (`api/pricing/american.hpp`, the A0 block after `AlSigmaQFamily`;
implementation `src/pricing/american_sigma_q_family_greeks.cpp`). No default path, preset,
policy key or record calls it. Contract and formulas are on the declarations and at the top
of the TU; this note holds the definitions a caller needs, the measured accuracy and the
measured cost.

## What it serves

`family_greeks(fam[, rate], S, K, sigma, q, req)` returns price, Δ, Γ, vega, θ, rho and
phi of the `(S, K)` contract of the family's side at the contract's own `(σ, q)`, from ONE
premium pass over K1's fused stages. `family_greeks_many` is the structure-of-arrays form
for a chain on one family: per-lane `K`, `σ`, `q`, an `active` mask (a retired lane is
neither read nor written), per-lane statuses, supported masks and reasons. Every active
lane is the scalar entry's result bit for bit.

| Greek | What it is | Needs |
|---|---|---|
| Δ, Γ | ∂/∂S of the integrand at fixed boundary (exact: the internal put's boundary does not depend on its spot); calls by homogeneity | — |
| vega | K1's fused dP/dσ, the boundary's σ-motion included (bit for bit `sigma_q_family_detail::price_and_vega`'s vega on every served row) | — |
| θ | the continuation-region PDE identity θ = rV − (r − q)SΔ − ½σ²S²Γ, calendar dV/dt per unit of the geometry's T | — |
| phi | ∂P/∂q: frozen-boundary terms plus the boundary motion dy/dq through the tensor's q axis | a q axis (`n_q() > 1`) |
| rho | ∂P/∂r: frozen-boundary terms plus Σ ∂P/∂y · dY/dr | an `AlSigmaQFamilyRateBumps` built from this family |

The price field is the fused pass's price — `price()`'s map to rounding (K1: 2.2e-16 scaled),
not `price()`'s bits — and does not depend on the mask.

## What is held fixed (`AlFamilyGreekConvention`)

- `held_fixed`: as requested. `ContractSigma` (the contract's σ held) and `StickyStrike`
  (the surface's σ(K) at the contract's strike held — on a family priced at that σ these
  are the same numbers) are implemented. `StickyMoneyness` and `RefittedSurface` need
  dσ/dS of a fitted surface (A1 / H0) and are NOT implemented: every requested Greek is
  NaN with `ConventionUnsupported`, the price is still served.
- `model = SigmaQFamily`: derivatives of the family's OWN polynomial map, not of the
  converged Andersen-Lake model (deep review F11). The distance to the converged model is
  measured below.
- `clock = GeometryTime`: θ per unit of the geometry's T (whatever year fraction the
  caller prepared it on); rho / phi per unit of rate / yield. A roll through expiry, an
  earnings date or a dividend is an event, not θ.
- `surface_version`: the caller's tag, echoed (a family has none).

The convention is populated on every bundle, refusals included.

## Validity and reasons (`AlFamilyGreekReason`)

Every requested Greek carries a reason; `supported` holds the requested Greeks with a usable
number (`Served` or `Exercised`). An unsupported Greek is never `0.0` marked served.

| Reason | Value | When |
|---|---|---|
| `Served` | the derivative | continuation region, inside the σ and q boxes |
| `NotRequested` | 0.0 | not in the mask |
| `Exercised` | Δ = −1 put / +1 call, the rest 0.0 | the exercise regime: K1's rule (`FusedPriceVega::exercised`), read as is — the spot inside the current edge, or the served price within 1e-9 K of intrinsic |
| `Refused` | NaN | the row refuses (`status` ≠ Ok: σ / q outside the box, bad S / K, unprepared, re-prepared geometry) |
| `NoCarryBumps` | NaN | rho without a rate-bump set |
| `CarryBumpRefused` | NaN | rho with a set that is unprepared or was built from another family |
| `NoQAxis` | NaN | phi on a collapsed q box |
| `NearNodeDegenerate` | NaN | the transform s = √y at its zero at a premium node with a nonzero slope in that Greek's parameter (σ: vega, q: phi, r: rho) |
| `ConventionUnsupported` | NaN | `StickyMoneyness` / `RefittedSurface` requested |

Exercised rows serve rho = phi = 0 even without a rate-bump set: intrinsic does not move
with r or q. On those rows the bundle's vega is 0.0 (`Exercised`) where K1's fused slope
may be nonzero — K1's `price_and_vega` serves the slope of the map for an inversion; the
bundle serves the exercise Greeks for hedging.

**The near-boundary band.** A served row whose spot is within a quarter standard
deviation of the family's current exercise edge (|ln(S / S_edge)| < 0.25 σ√T) is in the
separately reported error class below. Its θ is the weakest Greek there: the PDE identity
multiplies the map's Γ error by ½σ²S² exactly where V → intrinsic and θ → 0.

## The rate-bump set (rho)

`AlSigmaQFamilyRateBumps::prepare(fam)` builds two families at r ± h
(h = `kAlSigmaFamilyCarryBump` = 1e-4) with the base's opts, boxes, nodes and axis and keeps
only the central difference dY/dr of their node tensors. There is no one-sided fallback:
any refusal of a bumped family is returned and rho stays `CarryBumpRefused`. Cost: two
family builds (cost table). Its own error is the bump's O(h²) truncation:
A(h) = f'(r) + P_y y''' h² / 6; against the Richardson reference R = (4 D(h/2) − D(h)) / 3
of the second families' prices the gap falls 3.99999x (median over 136 resolved rows) when
the set is rebuilt at h/2 — truncation and nothing else.

## Accuracy

All measured on the vol-rel build (AVX2 host: the families are AVX2-built unless stated).

**Against the family map's own differences** (`AlSigmaQFamilyGreeks.*`, fast; Richardson
ladders of the bundle's own price; every row held to gap ≤ the ladder's tolerance, relative
claims on resolved rows only; 5 families: SPY 23 d put / call, SPY 2.17 y put, a T = 1 put
on a 1 %-wide q box at n_q 6, a call at n_q 8):

| Greek | Rows | Resolved | Worst rel (resolved) | Worst gap / tol |
|---|---|---|---|---|
| Δ | 472 | 395 | 1.6e-11 | 0.054 |
| Γ | 472 | 0 (a second difference) | — | 0.062 |
| vega | 472 | 263 | 2.4e-11 | 0.066 |
| phi | 472 | 104 | 3.1e-11 | 0.065 |
| rho vs D(h) / vs R, outside the band | 456 | — | 2.9e-6 / 2.3e-6 | — |
| rho vs R, inside the band | 20 | — | 7.6e-5 | — |
| θ vs the PDE identity of the served fields | every row | — | ≤ 1e-13 | — |

The SPY q box is 8.9e-5 wide: a q step that fits it leaves the price's rounding at ~5e-8 of
phi, so no SPY phi row resolves 1e-9; those rows are held to gap ≤ tol only.

**Against the converged model** (`AlSigmaQFamilyGreeksModel.*`, slow, 60 s; Andersen-Lake
32 / 64 nodes, 16 sweeps, tol 1e-13; each Greek by a 4-level Richardson ladder — θ the
CALENDAR difference −∂P/∂T; the converged reference's uncertainty from the 24 / 48 rung;
the family's own build scheme, the geometry's Reference preset, as the third column;
104 rows over SPY 23 d put / call, a 55-day high-vol put / call at σ 0.7 / 1.2 and three
in-the-money sets walking to the edge; CSV `A0-20261023-greeks\logs\slow-*-model.csv`):

| Greek | Outside band: vs converged max [p99] | vs build scheme max | build scheme vs converged | Inside band (8 rows): vs converged max |
|---|---|---|---|---|
| Δ | 1.9e-5 [2.3e-6] | 1.2e-8 | 1.9e-5 | 2.6e-5 |
| Γ | 2.7e-5 [1.1e-5] | 5.0e-8 | 2.7e-5 | not differenced (one-sided) |
| vega | 6.9e-5 [2.1e-5] | 4.9e-8 | 6.9e-5 | 1.5e-3 (converged uncertainty 1.3e-3) |
| θ | 1.8e-4 [4.7e-5] | 6.8e-5 | 2.4e-4 | 1.38 (0.007 σ√T from the edge) |
| rho | 1.9e-4 [5.7e-5] | 2.3e-6 | 1.8e-4 | 2.2e-3 |
| phi | 1.7e-4 [5.5e-5] | 2.2e-7 | 1.7e-4 | 2.1e-3 |

Outside the band the family's distance to the converged model IS its build scheme's
(the worst row everywhere is the 2.17-year put): the interpolation adds 1e-8-class to Δ /
Γ / vega / phi and the bump's truncation 2e-6 to rho. θ (the PDE identity) sits 1.8e-4
from the converged calendar difference — closer than the build scheme's own calendar
difference (2.4e-4): that scheme's price is not converged in T. The L3g σ family's θ on
the same rows measures the same (1.8e-4 / 4.7e-5 p99): review-L3g m5 closed. Inside the
band θ reaches 1.38 relative (0.47 per year absolute on a 61 USD put) at 0.007 σ√T from the
edge, where θ → 0 and the identity amplifies Γ's error; the build scheme agrees with the
converged reference there (3.1e-3), so it is the family, not the reference.

**AVX2- vs scalar-built families** (same evaluator; the AVX2 node solve reorders the sweep
arithmetic, ≤ ~4e-13 of y): Δ 1.3e-14, S·Γ 2.4e-12, vega 2.3e-11, θ 7.9e-10, rho 7.0e-10,
phi 7.1e-9, price 4.2e-15 (pinned at these values).

**Overlap with the L3g σ family** (collapsed q box, scalar build — the same boundary, two
arithmetics): Δ 4.4e-16, S·Γ 7.8e-14, vega 2.5e-13, θ 2.2e-11, rho 3.5e-13 (pinned).

## Cost

vol-rel-avx2 bench rows `american/sqgreeks/spy23/nq4/ns12` (per contract: the SPY 23 d
chain's 317 rows on a put and a call family, n_q 4, n_σ 12, 5,072 timed queries) and
`american/sqgreeks_chain/{spy23,synthetic300}/{primary,all}` (one `family_greeks_many` call
per chain, single thread, 5 warm + 101 timed calls); `ATX_VOL_AL_SQGREEKS=1` +
`ATX_VOL_AL_SPY23=<csv>`. Two runs on a LOADED host (65 – 99 % CPU from other build lanes):
unqualified, a quiet-host pin is W4's. JSON: `A0-20261023-greeksenchfter-m3-{1,2}.json`.

| row | p50 µs | p99 µs |
|---|---|---|
| `price()` | 6.1 / 7.4 | — |
| K1 `price_and_vega` (fused) | 5.0 / 6.1 | — |
| bundle, primary mask (Δ Γ vega θ phi) | 5.5 / 6.8 | 9.1 / 12.5 |
| bundle, all (+ rho, with a rate-bump set) | 5.9 / 7.2 | 9.7 / 13.3 |
| family build (`prepare`, n_q 4 × n_σ 12) | 2,605 / 3,261 | — |
| rate-bump set (`AlSigmaQFamilyRateBumps::prepare`, two family builds) | 5,094 / 6,676 | — |

| chain (one call) | p50 ms | p99 ms | per lane p50 µs |
|---|---|---|---|
| SPY 23 d (317), primary | 1.68 / 1.96 | 1.76 / 2.00 | 5.3 / 6.2 |
| SPY 23 d (317), all | 1.77 / 2.10 | 1.89 / 3.30 | 5.6 / 6.6 |
| synthetic 300, primary | 1.72 / 2.05 | 1.90 / 3.94 | 5.7 / 6.8 |
| synthetic 300, all | 1.87 / 2.37 | 1.97 / 4.41 | 6.2 / 7.9 |

Targets (master plan P3): bundle p50 ≤ 20 µs — met (5.5 – 7.2 µs). 300-chain p50 / p99 ≤ 1 ms
(stretch) — NOT met: 1.7 – 2.4 ms p50. The chain entry is one scalar pass per lane
(≈ K1's fused price + vega plus 10 – 20 %); 1 ms needs a lane-batched (SIMD) pass or a second
thread, neither in this lane.

The review-L3g m3 change (reuse the integrand's Φ(−d−), Φ(−d+) in the spot / carry
accumulators, in both this pass and the L3g σ-family pass) moved the primary bundle from
7.6 / 8.3 to 5.5 / 6.8 µs p50 and the L3g hedge row from 11.6 / 10.0 to 8.0 / 8.9 µs, with
the `bits_fnv52` fold of every served field identical before and after (each A0 row and the
L3g `price` / `hedge` / `eight` / `all` rows).
