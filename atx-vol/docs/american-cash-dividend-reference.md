# American options on discrete cash dividends — an independent reference, and the 301-step default

Lane D0 (vol-sota master plan, Sprint 4 package D0; deep review F10), 2026-10-22.
Code: `src/pricing/american_discrete_div_reference.{hpp,cpp}` (library-private, selected by
no default). Tests: `tests/american_discrete_div_reference_test.cpp` (fast, ctest
`atx-vol-discrete-div-reference`) and `tests/american_discrete_div_reference_matrix_test.cpp`
(slow, ctest `atx-vol-discrete-div-reference-matrix`, suite in `ATX_VOL_SLOW_FILTER`).
Every table in sections 5-8 is pasted from what the slow suite printed in the run recorded in
section 10, and the anchors in section 3 are the fast suite's output; the prose quotes those
tables, and no number is computed by hand.

## 1. Why, and the answer

Every American discrete-cash-dividend number atx-vol serves comes from one family of lattices,
the spliced CRR at 301 steps (`kDiscreteDivDefaultSteps`). Until now it was checked only against
itself: its refined comparator is the same splice at 2401 steps. Deep review F10 asked for an
independent reference. This lane builds two: an event-aligned Crank–Nicolson PDE with an exact
LCP, and an event-aligned trinomial lattice. They share the model, the event schedule and three
small primitives (the payoff cell average, the cubic interpolation and the jump operator built
on them). They share no time discretisation, constraint treatment, grid or boundary treatment.
Each converges on its own refinement ladder, and the PDE reports time and space separately. On
a 50-case matrix covering the brief's list, the two methods agree on all 50 cases within their
own uncertainties.

**What certifies the 41 RESOLVED rows is the PDE's own convergence.** Its uncertainty is inside
the kernel's 1e-7 IV contract on those rows. The trinomial is a coarser cross-check, to ITS
uncertainty (1.7e-5 to 1.8e-3 USD on those rows). That is above the 1e-7 x vega threshold on all
41, and the raw gap between the two methods exceeds the threshold on 17 of them (section 5,
cross-check table). Five rows are UNRESOLVED only against that solver tolerance (low vega), 2 are
price-only, and 2 are a stated convention gap.

**The answer.** The 301-step default is adequate to about 6 bp of IV for chains with at most
one in-life ex-date that is not inside the first lattice step. It is NOT adequate for
multi-event spot-shift chains, which it overprices. On the matrix's quarterly 1-USD chains
(S 100) the error is -4.1 to -4.4 bp of IV per event at `T <= 1`, -3.2 at `T 2` and -2.8 at
`T 3` (-34 bp in total on the 12-event chain). On the SPY-shape LEAPS it is -1.0 bp per event
(-9 bp in total). The error decays only about first order in the step count: 2401 steps still
leaves -6 bp on the 12-event chain, at 23x the cost. A dividend inside the first step is the
exception the step count DOES fix: +17 to +20 bp at 301 and 601 steps, under 1 bp at 1201. The
recommendation is **event-adaptive**: keep 301 as the global default where it is adequate, and
handle the multi-event and first-step cases adaptively. A global raise of the step count is not
the fix for multi-event chains. The kernel's refined
comparator is not a reference either: its own error claim is violated on 20 of 48 rows, and on
the review-m1 contract (cash = 64 % of spot) it is off by 2.61 USD, where the 301 lattice is
within 5.4e-5. Section 9 has the memo; sections 5-8 have every number it cites.

## 2. The model and the conventions

**Process.** Between ex-dates the stock is geometric Brownian motion with drift `r - q` and
volatility `sigma`, `q` being the residual continuous yield on top of the cash schedule (the
lattice kernel's convention, `american.hpp`). At an ex-date the stock falls by the cash.
Events outside `(0, T]` are ignored; an ex-date within `kTauAtExpiryRelTol * T` of expiry is
applied to the payoff analytically. Same window as the kernel.

**Exercise rights (matched).** American exercise at any time; at an ex-date the holder may
exercise CUM-dividend (against the undiluted stock) or an instant later POST-dividend, exactly
the two-level test the kernel's splice applies (`american.hpp` detail 2); on an expiry ex-date
the cum level is the payoff's (an American call cannot be moved by cash landing on its own
expiry).

**Cash that would exceed the stock (`ddiv_ref::CashProcess`).** The treatment is a model
choice and is stated, never substituted:

| Convention | Rule at an ex-date | Who uses it |
|---|---|---|
| `SpotLiquidator` | `S+ = max(S- - D, 0)`: the firm pays what it is worth and the stock is absorbed at 0 (a put is then worth `K` now, or `K e^{-r tau}` held) | **The kernel**: `splice_dividend` floors the post level at 0 for every Spliced* scheme |
| `SpotSurvivor` | `S+ = S- - D` when `S- >= D`, else `S+ = S-` (the dividend is skipped) | Reference only, to MEASURE the treatment's weight (group I) |
| `Escrowed` | `X = S - escrow(t)` lognormal; exercise values `X + escrow(t)`, cum and post at the ex-date | The `Escrowed*` lattices. The stock never falls below the cash to come, so the question cannot arise after `t = 0`; `X0 <= 0` is `OutOfRange` |

**Declared on the kernel's results.** `api/pricing/american_discrete_div_kernel.hpp` now carries
`DiscreteDivConvention` (process, cum/post exercise, cash-excess treatment, ex-date grid rule,
spot-vs-escrowed volatility) and the constexpr map `discrete_div_convention(DiscreteDivScheme)`;
`DiscreteDivConverged` and `DiscreteDivIvResult` carry it as an appended field. Additive only: no
arithmetic changed (parity hashes in section 10). `american_discrete_div_price` (a bare `double`)
and `DiscreteDivGreeks` (in the frozen `american.hpp`) are covered by the map from the scheme
their caller passed.

## 3. The two reference methods

Both methods are written for this lane and share nothing with the kernel except the public
vocabulary types (`CashDividend`, `Side`, `ExerciseStyle`). With each other they share the event
schedule (`ddiv_ref::event_schedule`, the kernel's `(0, T]` window and expiry tolerance) and
three numerical primitives. Those are the payoff cell average (`cell_average`), the cubic
Lagrange interpolation (`interp_uniform_cubic`), and the jump operator built on them
(`apply_jump` / `jump_at`). A defect in any of the three would move both methods together, and
the cross-check could not see it. The fast suite therefore pins each one against an exact value:
cubics, closed-form payoffs, the escrowed Black-76 and the exact one-dividend European and
American-call quadratures (anchors below). They share no time discretisation, no constraint
treatment, no grid and no boundary treatment.

### A. Event-aligned Crank–Nicolson PDE (the primary reference)

| Choice | What | Why |
|---|---|---|
| State | `x = ln S` (spot conventions) or `x = ln X`, `X = S - escrow(t)` (Escrowed), on a uniform grid whose centre node is exactly `S0` (or `X0`) at every refinement level | The price and the grid Greeks are read at a node, so nested levels need no interpolation |
| Domain | `[min(ln S, ln K, ln max(S - sum D, 0.05 S)) - 8 sigma sqrt T, max(ln S, ln K) + 8 sigma sqrt T]`, Dirichlet far field: the discounted forward intrinsic of the in-the-money side (floored by immediate exercise for American), 0 on the other | Width 8 vs 11 standard deviations is checked by fast test `SpaceAndTimeAreRefinedIndependentlyAndShareOnlyTheCorner` |
| Space | Central differences; the operator is an M-matrix at every grid used | Second order, and the M-matrix property is what makes the LCP below well posed and Howard iteration finite |
| Time | Crank–Nicolson; after the payoff and after every jump the first two CN steps are replaced by four implicit-Euler half steps (Rannacher) | Smooths the non-smooth data without the CN oscillation, keeps second order |
| Time grid | Every interior ex-date is a grid time, bit-exactly (segments between events); each segment has `max(4, ceil(base L / T)) 2^level` steps, quadratically graded towards the segment's own start: `t = t_end - L (k / n)^2` | Uniform steps measured time order ~1.25 for the American constraint (the early-exercise boundary is `sqrt(tau)`-singular at every restart); graded steps measured 1.8-2.0. A minimum of 4 steps per segment: with 1, the 0.0013-year last segment of case B2p held the time axis to order 1.2 |
| Payoff | Cell-averaged over each node's control volume with the kink split exactly (4-point Gauss–Legendre per piece) | Removes the strike-placement oscillation of point sampling |
| American constraint | The discrete LCP `min(M V - b, V - g) = 0` solved each step by exact Howard policy iteration on the tridiagonal system, accepted at 64 ulps of each row's term scale (1024 ulps from the third iteration: the rounding ties an `r = q = 0` deep-ITM put produces). The first step of each segment seeds its policy from one unconstrained step | No penalty parameter and no SOR tolerance, so the constraint adds no error of its own. The tie rule and the seeding were both measured on case G4p (section 10, Diagnostics in the lane report): without them the finest grid cycled for its whole cap, or took 31 iterations |
| Jump (spot conventions) | At an ex-date the post-dividend curve is read at `ln(S - D)` by cubic Lagrange interpolation in `x`; a node whose control volume contains a kink of the jumped curve (the cum-exercise crossing, the `S = D` floor) is cell-averaged locally instead; below the grid the value is linear in `S` to its exact value at zero (`K` for a liquidated put, `K e^{-r tau}` held) | Point interpolation across the cum-exercise kink cost 2e-5 on the one-dividend American call; local averaging brought it to 4e-8 against the exact quadrature |
| Jump (Escrowed) | None in state space: `X` is continuous; the exercise value is `X + escrow(t)`, and both cum and post rights are tested at the ex-date | The escrowed model's defining property |

Space and time are refined **independently**: a space ladder `N_x = 1200, 2400, 4800, 9600` at the
finest time grid, and a time ladder (nominal base, 2x, 4x, 8x) at the finest space grid; the
corner is solved once and read by both. Each axis gets the rule below with theoretical order 2.

### B. Event-aligned trinomial lattice (a coarser cross-check)

A recombining trinomial tree in `ln S` (or `ln X`) with one spacing for the whole tree,
`dx = sigma sqrt(3 dt_max)`, and a per-segment step `dt` so that every ex-date is a time level;
branch probabilities matched to the first two moments of the log return over each segment's
step; Bermudan exercise at every level (cum and post at an ex-date). The tree is a widening fan
padded by `7 sigma sqrt T + ln(S / net)` on both sides, so the post-dividend level of every node
is inside the tree and no boundary condition is ever applied. The jump is the same operator as
the PDE's. Ladder: 1000, 2000, 4000, 8000, 16000 nominal steps; theoretical order 1.

It is a different discretisation family in time, constraint and grid: explicit and probabilistic,
with no LCP solver, no Rannacher, no grading and no far field. With method A it shares the
model, the event schedule and the three primitives named above. It is also much coarser. Its
uncertainty on the RESOLVED rows is 1.7e-5 to 1.8e-3 USD, 4.7 to 127 times the 1e-7 x vega
threshold (section 5, cross-check table), so it confirms the PDE only to that level.

### Uncertainty rule (per axis, then per method)

From the last three values of an axis, `d1 = v[n-2] - v[n-3]`, `d2 = v[n-1] - v[n-2]`,
`p = log2(d1 / d2)`. The axis is **asymptotic** iff `d1 / d2 > 1` and `p` lies in
`[p_th / 2, p_th + 1]`; then the Richardson correction is `d2 / (2^min(p, p_th) - 1)` and the
axis's uncertainty is `|correction|` (the full correction, not a fraction of it). Otherwise no
correction is applied and the uncertainty is `max(|d1|, |d2|)`. A method's value is its finest
value plus the corrections; its uncertainty is `u_x + u_t + 1e-12 max(|V|, 1)` (rounding floor).

**Cross-check.** The methods agree iff `|A - B| <= u_A + u_B`. If they agree the reference is A
with `u_A` (the tighter); if not, the reference is still A but its uncertainty is
`|A - B| + u_A + u_B` and the row is `UNRESOLVED (methods disagree)`. Because `u_B` is 10 to 400
times `u_A` on the RESOLVED rows, agreement is a weak test: it catches a gross error in either
method, not one at the 1e-7 IV level.

**Status of a row.** The kernel claims its implied vols to `DiscreteDivIvOpts::tol = 1e-7` (in
vol units). A reference resolves a row iff the methods agree and `u_ref <= 1e-7 x vega`
(`RESOLVED`); otherwise `UNRESOLVED (u > 1e-7 IV)`. Since `u_ref = u_A` whenever the methods
agree, **RESOLVED is certified by the PDE's self-convergence** (two independent refinement
axes, each inside its asymptotic window or bounded by its envelope). The trinomial's agreement
holds only to `u_B`. A row whose vega is below
`1e-4 max(P, 1)` has no identifiable IV (`PRICE-ONLY`; master plan 3.2) and is judged in price
only. Survivor rows are `CONVENTION GAP`: the kernel does not implement that convention, so the
kernel columns there measure the convention's weight, not an error.

### Validation anchors (fast suite, every run)

Printed by `atx-vol-discrete-div-reference` (fast ctest), run of section 10:

```text
[ddiv-ref] no-div put  K 100 T 1.00: pde 8.3131291086 +- 1.56e-05  AL 8.3131239539  |gap| 5.15e-06  (AL accurate preset 8.3131146288)
[ddiv-ref]   space 8.3123270597 8.3129209003 8.3130757554 8.3131154369 p_x 1.96 u_x 1.37e-05
[ddiv-ref]   time  8.3131085061 8.3131137633 8.3131156499 8.3131154369 p_t 0.00 u_t 1.89e-06
[ddiv-ref] no-div put  K 110 T 0.50: pde 13.5511219448 +- 1.21e-05  AL 13.5511182635  |gap| 3.68e-06  (AL accurate preset 13.5511112633)
[ddiv-ref]   space 13.5504762104 13.5509539965 13.5510782308 13.5511098155 p_x 1.98 u_x 1.08e-05
[ddiv-ref]   time  13.5510948564 13.5511044076 13.5511081410 13.5511098155 p_t 1.16 u_t 1.36e-06
[ddiv-ref] no-div call K 95 T 1.00: pde 9.3924638327 +- 1.45e-05  AL 9.3924615201  |gap| 2.31e-06  (AL accurate preset 9.3924544799)
[ddiv-ref]   space 9.3917584754 9.3922800796 9.3924174182 9.3924521088 p_x 1.99 u_x 1.17e-05
[ddiv-ref]   time  9.3924509894 9.3924490029 9.3924517817 9.3924521088 p_t 3.09 u_t 2.78e-06
[ddiv-ref] no-div put  K 90 T 0.25: pde 2.4890821291 +- 2.54e-06  AL 2.4890806733  |gap| 1.46e-06  (AL accurate preset 2.4890790628)
[ddiv-ref]   space 2.4889852601 2.4890522899 2.4890737006 2.4890795870 p_x 1.86 u_x 2.23e-06
[ddiv-ref]   time  2.4890636189 2.4890753528 2.4890787101 2.4890795870 p_t 1.94 u_t 3.10e-07
[ddiv-ref] no-div put  K 100 T 1.00: pde 9.9476448718 +- 1.75e-06  AL 9.9476449660  |gap| 9.42e-08  (AL accurate preset 9.9476449660)
[ddiv-ref]   space 9.9476021510 9.9476350786 9.9476432973 9.9476452933 p_x 2.04 u_x 6.65e-07
[ddiv-ref]   time  9.9476976542 9.9476589573 9.9476482103 9.9476452933 p_t 1.88 u_t 1.09e-06
[ddiv-ref] no-div put  K 120 T 3.00: pde 30.3680435810 +- 3.13e-05  AL 30.3680394395  |gap| 4.14e-06  (AL accurate preset 30.3680146303)
[ddiv-ref]   space 30.3663559134 30.3676139329 30.3679353966 30.3680163404 p_x 1.99 u_x 2.72e-05
[ddiv-ref]   time  30.3680478091 30.3680120681 30.3680161091 30.3680163404 p_t 4.13 u_t 4.04e-06
[ddiv-ref] euro-1div put  tau 0.400 D 3.0: pde 11.7175783524 +- 4.82e-06 exact 11.7175783546 |gap| 2.15e-09
[ddiv-ref] euro-1div call tau 0.400 D 3.0: pde 11.7088098582 +- 6.98e-06 exact 11.7088098612 |gap| 2.99e-09
[ddiv-ref] euro-1div put  tau 0.003 D 6.0: pde 10.6158295550 +- 4.92e-06 exact 10.6158295466 |gap| 8.41e-09
[ddiv-ref] euro-1div call tau 0.003 D 6.0: pde 7.7318528720 +- 6.91e-06 exact 7.7318528745 |gap| 2.49e-09
[ddiv-ref] euro-1div put  tau 0.500 D 20.0: pde 22.0398860115 +- 1.77e-06 exact 22.0398860119 |gap| 4.40e-10
[ddiv-ref] euro-1div call tau 0.500 D 20.0: pde 5.2930938638 +- 7.17e-06 exact 5.2930938650 |gap| 1.15e-09
[ddiv-ref] amer-call-1div: pde 11.6639138296 +- 2.84e-06 exact 11.6639138666 |gap| 3.70e-08
[ddiv-ref] amer-call-1div-itm: pde 31.4520350958 +- 5.37e-06 exact 31.4520351031 |gap| 7.28e-09
[ddiv-ref] survivor-vs-liquidator sigma 0.8: call 2.70216791 vs 2.67757246  put 3.82101120 vs 4.47503628
[ tie ] time level 0: 3 policy iterations max, 27 rounding-tie steps, worst accepted violation 8.732e-14 (393 ulps)
[ tie ] time level 3: 2 policy iterations max, 0 rounding-tie steps, worst accepted violation 1.416e-14 (64 ulps)
[ddiv-ref] cross-check put  spot-liquidator: pde 11.0743260466 +- 1.77e-05  lattice 11.0743177162 +- 3.17e-04 (p_N 1.18)
[ddiv-ref] cross-check put  escrowed: pde 10.7459000370 +- 1.62e-05  lattice 10.7455120615 +- 7.17e-04 (p_N 0.65)
[ddiv-ref] cross-check call spot-liquidator: pde 9.1899648768 +- 4.00e-06  lattice 9.1898739459 +- 8.80e-04 (p_N 1.07)
[ddiv-ref] cross-check call escrowed: pde 8.8510304672 +- 4.67e-06  lattice 8.8509515944 +- 8.72e-04 (p_N 1.09)
```

## 4. The matrix

50 cases, `tests/american_discrete_div_reference_matrix_test.cpp` `matrix()`. Defaults unless a
row says otherwise: `S 100`, `sigma 0.25`, `r 0.04`, `q 0`, American; puts at `K 100`, calls at
`K 95`; process `SpotLiquidator` (the kernel's). Ids end in `p` (put) or `c` (call).

| Group | Brief item | Rows |
|---|---|---|
| A | ex-date +-1 step, and the rounding boundary | `T 0.5`, one 2.00 at 0.25 = step 150.5 of the 301 lattice (round half to even: 150); A1 = -1 step, A2 = base, A3 = +1 step, A4 / A5 = step 150.49 / 150.51 |
| B | dividend on the first and the last step | `T 1`, one 3.00 at 0.7 of the first 301 step (B1), 0.4 of a step before expiry (B2), on expiry (B3) |
| C | 2 / 4 / 8 dividends | quarterly 1.00 from 0.10 at `T` 0.5 / 1 / 2 |
| D | LEAPS 2-3 y | D12: `T 3`, 12 quarterly 1.00. DSPY: the L3f worst contract's shape, `S 772.98`, `T 2.182`, `sigma 0.35`, `r 0.047`, nine quarterly 1.8311 from 0.15, `K` 700 (put) / 858 (call) |
| E | D/S of 1 %, 5 %, 20 % | one dividend of 1 / 5 / 20 at 0.5, `T 1`; plus E64, the review-m1 contract (dev200: `S 6.375`, `K 5`, `T 0.347`, `sigma 0.2`, `r 0.0468`, 4.10 cash = 64 % of spot, 5.9 days out) |
| F | exercise immediately before the jump | deep in the money just before a late ex-date: F1 call `K 70` / put `K 130`, 5.00 at 0.45, `T 0.5`; F2 call `K 80` / put `K 125`, 4.00 at 0.24, `T 0.25` |
| G | r = 0 | G1: one 3.00 at 0.5; G4: quarterly 1.00 from 0.10; `T 1`, `r 0` |
| H | escrowed convention | as G1 / G4 at `r 0.04` under `Escrowed`, against the kernel's `EscrowedLeisenReimer` |
| I | liquidator / survivor | `S 10`, `K 8`, `sigma 0.8`, `r 0.03`, one 6.00 at 0.25, `T 0.5`: IL = liquidator, IS = survivor |

Calls and puts in every group; `r > 0` everywhere except G.

**Kernel columns.** `P301` is `american_discrete_div_price` at `kDiscreteDivDefaultSteps` (301)
with `SplicedCrr` (H: `EscrowedLeisenReimer`). `d301 IV bp (inv)` is `(sigma_301 - sigma) x 1e4`,
where `sigma_301` is what `american_discrete_div_implied_vol` (301 steps, the same scheme,
`DiscreteDivTier::Accurate`, `tol 1e-7`) returns when handed the REFERENCE price. That is the
IV error a 301-step inversion makes on a fair price, exactly. Negative means the lattice overprices.
`refined cmp` is `american_discrete_div_converged_price` (`SplicedBbs` 2401 with Richardson against
1201), and `cmp claim` is its own `|fine - coarse|`. `holds` / `VIOLATED` compares that claim with
`|cmp - ref| - u_ref`. For H the refined column is `EscrowedLeisenReimer` at 2401 and carries no
claim. `dcmp bp (lin)` is `(cmp - ref) / vega`, and vega is a fixed-grid PDE pair at
`sigma +- 1e-3`.

**Liquidator / survivor, stated.** The kernel is the liquidator: `splice_dividend` floors the
post-dividend level at 0. Group I measures what the choice is worth when cash can exceed spot
(the survivor rows are `CONVENTION GAP`; their kernel columns measure the convention, not an
error). In the run: put 3.8210112 (survivor) vs 4.4750363 (liquidator), -0.654 USD; call
2.7021679 vs 2.6775725, +0.0246 USD. On a name where the stock can trade near its next
dividend, the convention is worth more than any lattice error in this document.

## 5. Comparison table

The run of section 10, pasted verbatim:

<!-- BEGIN D0 COMPARISON TABLE -->
| id | group | side | T | events | max D/S | reference (PDE) +- u | lattice ref +- u | agree | P301 | d301 USD | d301 IV bp (inv) | refined cmp | dcmp USD | dcmp bp (lin) | cmp claim | status |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A1p | A align -1step | P | 0.5 | 1 | 0.020 | 7.1736405 +- 1.4e-06 | 7.1736365 +- 2.9e-05 | yes | 7.1823329 | +8.69e-03 | -3.136 | 7.1743049 | +6.64e-04 | +0.240 | VIOLATED (6.4e-04) | RESOLVED |
| A1c | A align -1step | C | 0.5 | 1 | 0.020 | 9.6862934 +- 4.8e-07 | 9.6862927 +- 6.3e-05 | yes | 9.6856777 | -6.16e-04 | +0.247 | 9.6862920 | -1.42e-06 | -0.001 | holds (1.0e-03) | RESOLVED |
| A2p | A align base | P | 0.5 | 1 | 0.020 | 7.1735700 +- 1.4e-06 | 7.1735662 +- 2.9e-05 | yes | 7.1823329 | +8.76e-03 | -3.162 | 7.1742391 | +6.69e-04 | +0.242 | VIOLATED (6.5e-04) | RESOLVED |
| A2c | A align base | C | 0.5 | 1 | 0.020 | 9.6904775 +- 4.9e-07 | 9.6904767 +- 6.3e-05 | yes | 9.6856777 | -4.80e-03 | +1.929 | 9.6912957 | +8.18e-04 | +0.326 | VIOLATED (1.4e-04) | RESOLVED |
| A3p | A align +1step | P | 0.5 | 1 | 0.020 | 7.1734917 +- 1.4e-06 | 7.1734903 +- 2.7e-05 | yes | 7.1821699 | +8.68e-03 | -3.130 | 7.1741598 | +6.68e-04 | +0.242 | VIOLATED (6.5e-04) | RESOLVED |
| A3c | A align +1step | C | 0.5 | 1 | 0.020 | 9.6947023 +- 4.8e-07 | 9.6947020 +- 6.3e-05 | yes | 9.6947103 | +8.05e-06 | -0.004 | 9.6952178 | +5.16e-04 | +0.205 | VIOLATED (9.9e-05) | RESOLVED |
| A4p | A align 150.49 | P | 0.5 | 1 | 0.020 | 7.1735708 +- 1.4e-06 | 7.1735669 +- 2.9e-05 | yes | 7.1823329 | +8.76e-03 | -3.161 | 7.1742391 | +6.68e-04 | +0.242 | VIOLATED (6.5e-04) | RESOLVED |
| A4c | A align 150.49 | C | 0.5 | 1 | 0.020 | 9.6904354 +- 4.8e-07 | 9.6904346 +- 6.3e-05 | yes | 9.6856777 | -4.76e-03 | +1.912 | 9.6912957 | +8.60e-04 | +0.343 | VIOLATED (1.4e-04) | RESOLVED |
| A5p | A align 150.51 | P | 0.5 | 1 | 0.020 | 7.1735693 +- 1.4e-06 | 7.1735655 +- 2.9e-05 | yes | 7.1822158 | +8.65e-03 | -3.121 | 7.1742325 | +6.63e-04 | +0.240 | VIOLATED (6.4e-04) | RESOLVED |
| A5c | A align 150.51 | C | 0.5 | 1 | 0.020 | 9.6905195 +- 4.8e-07 | 9.6905187 +- 6.3e-05 | yes | 9.6918739 | +1.35e-03 | -0.549 | 9.6906820 | +1.62e-04 | +0.065 | holds (9.6e-04) | RESOLVED |
| B1p | B first step | P | 1 | 1 | 0.030 | 9.6402481 +- 1.6e-06 | 9.6402426 +- 2.6e-05 | yes | 9.5674193 | -7.28e-02 | +19.125 | 9.6412420 | +9.94e-04 | +0.264 | VIOLATED (6.7e-04) | RESOLVED |
| B1c | B first step | C | 1 | 1 | 0.030 | 12.5021530 +- 6.8e-07 | 12.5021529 +- 1.0e-04 | yes | 12.4406101 | -6.15e-02 | +16.989 | 12.5030255 | +8.72e-04 | +0.241 | VIOLATED (6.5e-04) | RESOLVED |
| B2p | B last step | P | 1 | 1 | 0.030 | 9.3875344 +- 6.9e-07 | 9.3875331 +- 6.2e-05 | yes | 9.3929711 | +5.44e-03 | -1.380 | 9.3874322 | -1.02e-04 | -0.026 | holds (9.0e-04) | RESOLVED |
| B2c | B last step | C | 1 | 1 | 0.030 | 14.4909890 +- 2.2e-07 | 14.4909891 +- 6.9e-05 | yes | 14.5040103 | +1.30e-02 | -3.698 | 14.4938766 | +2.89e-03 | +0.817 | holds (3.1e-03) | RESOLVED |
| B3p | B on expiry | P | 1 | 1 | 0.030 | 9.3852415 +- 3.0e-07 | 9.3852416 +- 5.6e-05 | yes | 9.3929711 | +7.73e-03 | -1.963 | 9.3852437 | +2.17e-06 | +0.001 | holds (2.8e-04) | RESOLVED |
| B3c | B on expiry | C | 1 | 1 | 0.030 | 14.4997542 +- 7.3e-07 | 14.4997539 +- 1.0e-04 | yes | 14.5040103 | +4.26e-03 | -1.208 | 14.4997335 | -2.07e-05 | -0.006 | holds (3.0e-04) | RESOLVED |
| C2p | C 2 events | P | 0.5 | 2 | 0.010 | 7.1221957 +- 1.9e-06 | 7.1221913 +- 3.5e-05 | yes | 7.1511559 | +2.90e-02 | -10.401 | 7.1263130 | +4.12e-03 | +1.485 | VIOLATED (1.1e-03) | RESOLVED |
| C2c | C 2 events | C | 0.5 | 2 | 0.010 | 9.6074845 +- 4.4e-07 | 9.6074830 +- 6.2e-05 | yes | 9.6254782 | +1.80e-02 | -7.090 | 9.6113499 | +3.87e-03 | +1.506 | VIOLATED (1.0e-03) | RESOLVED |
| C4p | C 4 events | P | 1 | 4 | 0.010 | 9.9514465 +- 1.9e-06 | 9.9514457 +- 4.4e-05 | yes | 10.0171207 | +6.57e-02 | -16.870 | 9.9424837 | -8.96e-03 | -2.312 | holds (1.2e-02) | RESOLVED |
| C4c | C 4 events | C | 1 | 4 | 0.010 | 12.2084392 +- 2.4e-07 | 12.2084347 +- 9.0e-05 | yes | 12.2691659 | +6.07e-02 | -16.553 | 12.2009770 | -7.46e-03 | -2.031 | holds (1.0e-02) | RESOLVED |
| C8p | C 8 events | P | 2 | 8 | 0.010 | 13.7238309 +- 1.8e-06 | 13.7238255 +- 6.4e-05 | yes | 13.8676976 | +1.44e-01 | -26.821 | 13.7161280 | -7.70e-03 | -1.442 | holds (2.6e-02) | RESOLVED |
| C8c | C 8 events | C | 2 | 8 | 0.010 | 15.7109046 +- 3.9e-07 | 15.7108903 +- 1.4e-04 | yes | 15.8392170 | +1.28e-01 | -24.829 | 15.7015891 | -9.32e-03 | -1.818 | holds (2.6e-02) | RESOLVED |
| D12p | D LEAPS 3y | P | 3 | 12 | 0.010 | 16.4113302 +- 4.4e-06 | 16.4113136 +- 8.8e-05 | yes | 16.6212556 | +2.10e-01 | -32.814 | 16.4109743 | -3.56e-04 | -0.056 | holds (3.8e-02) | RESOLVED |
| D12c | D LEAPS 3y | C | 3 | 12 | 0.010 | 18.2067337 +- 8.0e-07 | 18.2067040 +- 1.8e-04 | yes | 18.4163734 | +2.10e-01 | -34.017 | 18.2042431 | -2.49e-03 | -0.406 | holds (3.8e-02) | RESOLVED |
| DSPYp | D LEAPS SPY | P | 2.182 | 9 | 0.002 | 93.3185322 +- 3.3e-05 | 93.3183331 +- 5.8e-04 | yes | 93.6502010 | +3.32e-01 | -8.709 | 93.3727381 | +5.42e-02 | +1.417 | holds (5.5e-02) | RESOLVED |
| DSPYc | D LEAPS SPY | C | 2.182 | 9 | 0.002 | 149.6531807 +- 9.2e-06 | 149.6529728 +- 1.8e-03 | yes | 150.0738202 | +4.21e-01 | -9.599 | 149.7249693 | +7.18e-02 | +1.629 | VIOLATED (4.5e-02) | RESOLVED |
| E1p | E D/S 1% | P | 1 | 1 | 0.010 | 8.7370443 +- 1.6e-06 | 8.7370453 +- 2.3e-05 | yes | 8.7605229 | +2.35e-02 | -6.086 | 8.7349665 | -2.08e-03 | -0.540 | holds (3.3e-03) | RESOLVED |
| E1c | E D/S 1% | C | 1 | 1 | 0.010 | 13.8776912 +- 7.0e-07 | 13.8776903 +- 1.0e-04 | yes | 13.8953259 | +1.76e-02 | -4.933 | 13.8760259 | -1.67e-03 | -0.464 | holds (2.5e-03) | RESOLVED |
| E5p | E D/S 5% | P | 1 | 1 | 0.050 | 10.7557330 +- 1.8e-06 | 10.7557339 +- 1.8e-05 | yes | 10.7742669 | +1.85e-02 | -4.812 | 10.7557701 | +3.71e-05 | +0.010 | holds (1.8e-03) | RESOLVED |
| E5c | E D/S 5% | C | 1 | 1 | 0.050 | 12.1633005 +- 3.2e-07 | 12.1632984 +- 9.3e-05 | yes | 12.1692928 | +5.99e-03 | -1.744 | 12.1633523 | +5.19e-05 | +0.015 | holds (5.8e-04) | RESOLVED |
| E20p | E D/S 20% | P | 1 | 1 | 0.200 | 20.6308584 +- 1.1e-06 | 20.6308616 +- 1.7e-05 | yes | 20.6428902 | +1.20e-02 | -4.118 | 20.6308680 | +9.61e-06 | +0.003 | holds (1.2e-03) | RESOLVED |
| E20c | E D/S 20% | C | 1 | 1 | 0.200 | 10.8738575 +- 5.6e-07 | 10.8738559 +- 9.6e-05 | yes | 10.8703221 | -3.54e-03 | +1.340 | 10.8735831 | -2.74e-04 | -0.103 | holds (1.7e-03) | RESOLVED |
| E64p | E D/S 64% (review m1) | P | 0.3472 | 1 | 0.643 | 2.7180752 +- 2.5e-10 | 2.7180752 +- 5.3e-12 | yes | 2.7181288 | +5.37e-05 | n/a | 5.3288952 | +2.61e+00 | n/a | VIOLATED (1.0e+00) | PRICE-ONLY (IV not identified) |
| E64c | E D/S 64% (review m1) | C | 0.3472 | 1 | 0.643 | 1.3788048 +- 1.6e-10 | 1.3788048 +- 4.0e-12 | yes | 1.3787753 | -2.95e-05 | n/a | 1.6411698 | +2.62e-01 | n/a | VIOLATED (1.3e-01) | PRICE-ONLY (IV not identified) |
| F1c | F ITM pre-jump | C | 0.5 | 1 | 0.050 | 31.3109315 +- 4.0e-07 | 31.3109315 +- 4.0e-06 | yes | 31.3108848 | -4.67e-05 | +0.235 | 31.3110107 | +7.92e-05 | +0.428 | VIOLATED (3.2e-05) | UNRESOLVED (u > 1e-7 IV) |
| F1p | F ITM pre-jump | P | 0.5 | 1 | 0.050 | 33.0453867 +- 2.3e-06 | 33.0453847 +- 1.1e-05 | yes | 33.0456890 | +3.02e-04 | -0.346 | 33.0455322 | +1.45e-04 | +0.166 | VIOLATED (4.1e-05) | UNRESOLVED (u > 1e-7 IV) |
| F2c | F ITM pre-jump | C | 0.25 | 1 | 0.040 | 20.8846222 +- 3.9e-07 | 20.8846222 +- 5.9e-06 | yes | 20.8836668 | -9.55e-04 | +3.030 | 20.8845214 | -1.01e-04 | -0.353 | VIOLATED (6.0e-05) | UNRESOLVED (u > 1e-7 IV) |
| F2p | F ITM pre-jump | P | 0.25 | 1 | 0.040 | 27.8985665 +- 1.7e-06 | 27.8985663 +- 4.8e-06 | yes | 27.8977794 | -7.87e-04 | +2.280 | 27.8984803 | -8.62e-05 | -0.265 | VIOLATED (8.3e-05) | UNRESOLVED (u > 1e-7 IV) |
| G1p | G r=0 | P | 1 | 1 | 0.030 | 11.5210593 +- 1.9e-07 | 11.5210590 +- 7.5e-05 | yes | 11.5340066 | +1.29e-02 | -3.292 | 11.5214091 | +3.50e-04 | +0.089 | holds (1.2e-03) | RESOLVED |
| G1c | G r=0 | C | 1 | 1 | 0.030 | 11.2202666 +- 3.0e-07 | 11.2202654 +- 9.7e-05 | yes | 11.2259351 | +5.67e-03 | -1.586 | 11.2192944 | -9.72e-04 | -0.268 | holds (1.5e-03) | RESOLVED |
| G4p | G r=0 | P | 1 | 4 | 0.010 | 12.0663212 +- 2.0e-07 | 12.0663187 +- 7.9e-05 | yes | 12.1320966 | +6.58e-02 | -16.780 | 12.0575025 | -8.82e-03 | -2.259 | holds (1.1e-02) | RESOLVED |
| G4c | G r=0 | C | 1 | 4 | 0.010 | 10.6583395 +- 5.6e-07 | 10.6583347 +- 9.8e-05 | yes | 10.7190124 | +6.07e-02 | -16.386 | 10.6504730 | -7.87e-03 | -2.125 | holds (1.0e-02) | RESOLVED |
| H1p | H escrowed | P | 1 | 1 | 0.030 | 9.5541072 +- 1.7e-06 | 9.5541077 +- 2.2e-05 | yes | 9.5545551 | +4.48e-04 | -0.118 | 9.5541849 | +7.77e-05 | +0.020 | n/a | RESOLVED |
| H1c | H escrowed | C | 1 | 1 | 0.030 | 12.6540532 +- 4.6e-07 | 12.6540521 +- 9.6e-05 | yes | 12.6510635 | -2.99e-03 | +0.840 | 12.6536352 | -4.18e-04 | -0.118 | n/a | RESOLVED |
| H4p | H escrowed | P | 1 | 4 | 0.010 | 9.7640159 +- 2.0e-06 | 9.7640124 +- 4.5e-05 | yes | 9.7639586 | -5.73e-05 | +0.015 | 9.7640071 | -8.77e-06 | -0.002 | n/a | RESOLVED |
| H4c | H escrowed | C | 1 | 4 | 0.010 | 12.0273800 +- 2.8e-07 | 12.0273761 +- 9.1e-05 | yes | 12.0281620 | +7.82e-04 | -0.217 | 12.0275072 | +1.27e-04 | +0.035 | n/a | RESOLVED |
| ILp | I cash>spot spot-liquidator | P | 0.5 | 1 | 0.600 | 4.4750363 +- 4.5e-07 | 4.4750364 +- 4.2e-06 | yes | 4.4758247 | +7.88e-04 | -5.895 | 4.4752563 | +2.20e-04 | +1.621 | VIOLATED (6.8e-05) | UNRESOLVED (u > 1e-7 IV) |
| ILc | I cash>spot spot-liquidator | C | 0.5 | 1 | 0.600 | 2.6775725 +- 9.4e-08 | 2.6775723 +- 1.9e-05 | yes | 2.6771144 | -4.58e-04 | +3.130 | 2.6779053 | +3.33e-04 | +2.256 | holds (4.3e-04) | RESOLVED |
| ISp | I cash>spot spot-survivor | P | 0.5 | 1 | 0.600 | 3.8210112 +- 4.7e-07 | 3.8209721 +- 1.5e-04 | yes | 4.4758247 | +6.55e-01 | n/a | 4.4752563 | +6.54e-01 | n/a | n/a | CONVENTION GAP (kernel = liquidator) |
| ISc | I cash>spot spot-survivor | C | 0.5 | 1 | 0.600 | 2.7021679 +- 1.1e-07 | 2.7021723 +- 1.6e-05 | yes | 2.6771144 | -2.51e-02 | n/a | 2.6779053 | -2.43e-02 | n/a | n/a | CONVENTION GAP (kernel = liquidator) |
<!-- END D0 COMPARISON TABLE -->

**UNRESOLVED rows (5), all for the same reason: `u_ref > 1e-7 x vega`, with the methods
agreeing.** They are F1c, F1p, F2c, F2p (deep in the money before a late ex-date, vega 1.85-8.8)
and ILp (vega 1.36). Their reference uncertainty is 0.4-2.3e-6 USD, which is 1.4e-3 to 5.1e-3 IV
bp against the kernel's 1e-3 bp root contract. These rows cannot certify a root at the solver's
tolerance. They DO certify the kernel's discretisation error, because the 301 lattice misses by
0.23-5.9 bp, 110-2200 times `u_ref`. No row has methods that disagree (0 of 50).

**PRICE-ONLY (2): E64p, E64c.** The vega is ~0 (the put is pinned at `9.1 e^{-r tau} - S`
after the 4.10 cash: analytic 2.7180751733). The reference agrees with the analytic value to
all 10 printed decimals. The 301 lattice is within 5.4e-5 USD (put) and 2.9e-5 (call). The refined comparator
returns 5.3289 for the put, **+2.61 USD**, against a claimed 1.0. This confirms review m1: on a
cash-dominated contract the comparator, not the 301 lattice, is the broken number.

**What certifies RESOLVED: the cross-check table.** For every IV-identified kernel-model row it
lists the 1e-7 x vega threshold, and `u_A`, `u_B` and `|A - B|` as multiples of it, pasted from
the same run:

<!-- BEGIN D0 CROSS-CHECK TABLE -->
| id | status | thr = 1e-7 vega USD | u_A (PDE) / thr | u_B (lattice) / thr | abs(A - B) / thr |
|---|---|---|---|---|---|
| A1p | RESOLVED | 2.77e-06 | 0.523 | 10.5 | 1.44 |
| A1c | RESOLVED | 2.51e-06 | 0.192 | 25.3 | 0.30 |
| A2p | RESOLVED | 2.77e-06 | 0.509 | 10.6 | 1.37 |
| A2c | RESOLVED | 2.51e-06 | 0.194 | 25.2 | 0.31 |
| A3p | RESOLVED | 2.77e-06 | 0.511 | 9.8 | 0.49 |
| A3c | RESOLVED | 2.51e-06 | 0.192 | 25.0 | 0.10 |
| A4p | RESOLVED | 2.77e-06 | 0.522 | 10.5 | 1.42 |
| A4c | RESOLVED | 2.51e-06 | 0.192 | 25.2 | 0.31 |
| A5p | RESOLVED | 2.77e-06 | 0.510 | 10.5 | 1.37 |
| A5c | RESOLVED | 2.51e-06 | 0.191 | 25.2 | 0.32 |
| B1p | RESOLVED | 3.77e-06 | 0.437 | 7.0 | 1.47 |
| B1c | RESOLVED | 3.62e-06 | 0.189 | 28.0 | 0.03 |
| B2p | RESOLVED | 3.93e-06 | 0.174 | 15.8 | 0.34 |
| B2c | RESOLVED | 3.54e-06 | 0.063 | 19.6 | 0.03 |
| B3p | RESOLVED | 3.93e-06 | 0.077 | 14.1 | 0.01 |
| B3c | RESOLVED | 3.54e-06 | 0.205 | 29.1 | 0.11 |
| C2p | RESOLVED | 2.77e-06 | 0.674 | 12.5 | 1.58 |
| C2c | RESOLVED | 2.57e-06 | 0.173 | 24.0 | 0.58 |
| C4p | RESOLVED | 3.88e-06 | 0.489 | 11.2 | 0.21 |
| C4c | RESOLVED | 3.67e-06 | 0.065 | 24.5 | 1.23 |
| C8p | RESOLVED | 5.34e-06 | 0.339 | 12.0 | 1.00 |
| C8c | RESOLVED | 5.12e-06 | 0.077 | 26.9 | 2.78 |
| D12p | RESOLVED | 6.37e-06 | 0.693 | 13.8 | 2.60 |
| D12c | RESOLVED | 6.14e-06 | 0.131 | 30.0 | 4.84 |
| DSPYp | RESOLVED | 3.83e-05 | 0.850 | 15.2 | 5.20 |
| DSPYc | RESOLVED | 4.41e-05 | 0.208 | 40.9 | 4.72 |
| E1p | RESOLVED | 3.85e-06 | 0.427 | 6.0 | 0.24 |
| E1c | RESOLVED | 3.59e-06 | 0.196 | 28.9 | 0.23 |
| E5p | RESOLVED | 3.83e-06 | 0.473 | 4.7 | 0.23 |
| E5c | RESOLVED | 3.45e-06 | 0.094 | 27.1 | 0.60 |
| E20p | RESOLVED | 2.93e-06 | 0.367 | 5.9 | 1.10 |
| E20c | RESOLVED | 2.65e-06 | 0.210 | 36.1 | 0.60 |
| F1c | UNRESOLVED (u > 1e-7 IV) | 1.85e-07 | 2.144 | 21.6 | 0.08 |
| F1p | UNRESOLVED (u > 1e-7 IV) | 8.79e-07 | 2.587 | 12.7 | 2.29 |
| F2c | UNRESOLVED (u > 1e-7 IV) | 2.86e-07 | 1.375 | 20.5 | 0.29 |
| F2p | UNRESOLVED (u > 1e-7 IV) | 3.26e-07 | 5.104 | 14.8 | 0.36 |
| G1p | RESOLVED | 3.93e-06 | 0.049 | 19.1 | 0.06 |
| G1c | RESOLVED | 3.62e-06 | 0.082 | 26.8 | 0.33 |
| G4p | RESOLVED | 3.90e-06 | 0.051 | 20.2 | 0.63 |
| G4c | RESOLVED | 3.70e-06 | 0.152 | 26.4 | 1.29 |
| H1p | RESOLVED | 3.80e-06 | 0.457 | 5.7 | 0.12 |
| H1c | RESOLVED | 3.54e-06 | 0.129 | 27.0 | 0.31 |
| H4p | RESOLVED | 3.80e-06 | 0.517 | 12.0 | 0.90 |
| H4c | RESOLVED | 3.60e-06 | 0.077 | 25.2 | 1.08 |
| ILp | UNRESOLVED (u > 1e-7 IV) | 1.36e-07 | 3.304 | 30.6 | 0.79 |
| ILc | RESOLVED | 1.48e-07 | 0.635 | 126.5 | 1.06 |

RESOLVED rows 41: u_B above thr on 41, abs(A - B) above thr on 17.
<!-- END D0 CROSS-CHECK TABLE -->

`u_A / thr <= 1` is what makes a row RESOLVED. On all 41 RESOLVED rows `u_B / thr` is 4.7-127,
so the trinomial cannot resolve any of them at the contract. On 17 of the 41 the raw gap exceeds
the threshold; the largest are DSPYp 5.20, D12c 4.84, DSPYc 4.72, C8c 2.78 and D12p 2.60, which
is 37x `u_A` on D12c. Every gap is inside `u_A + u_B`. The kernel-error columns of section 5 are
measured against the PDE value and `u_A`. The smallest 301-step error on a RESOLVED row, A3c's
8.05e-6 USD, is 17x its `u_A`.

## 6. Convergence: time and space reported independently

For each row: the space sequence (`N_x` 1200 / 2400 / 4800 / 9600 at the finest time grid),
the time sequence (nominal base, 2x, 4x, 8x at the finest space grid), and the lattice sequence
(1000 ... 16000 steps). Each comes with its observed order `p`, the asymptotic flag, and the
axis uncertainty. Then `u_ref` in USD and IV bp, the reference vega, and the PDE's
policy-iteration diagnostics (worst Howard iterations in a step / steps accepted by the
rounding-tie rule / worst accepted LCP violation in ulps of the row scale). Verbatim:

<!-- BEGIN D0 CONVERGENCE TABLE -->
| id | PDE space (finest time; Nx 1200..9600) | PDE time (finest space; nominal base..8x base) | lattice (steps 1000..16000) | u_ref USD | u_ref IV bp | vega | PDE Howard max iter / tie steps / worst LCP ulps | wall s |
|---|---|---|---|---|---|---|---|---|
| A1p | 7.173578433 7.173624214 7.173636079 7.173639012 | p 2.02 | u 9.8e-07 | 7.173618496 7.173633557 7.173637816 7.173639012 | p 1.83 | u 4.7e-07 | 7.174058524 7.173841247 7.173742498 7.173692061 7.173665617 | p 0.93 | u 2.9e-05 | 1.4e-06 | 5.23e-04 | 27.66 | 14 / 0 / 0 | 1.6 |
| A1c | 9.686298124 9.686294776 9.686294070 9.686293902 | p 2.07 | u 5.6e-08 | 9.686320600 9.686300272 9.686295177 9.686293902 | p 2.00 | u 4.3e-07 | 9.687331887 9.686808341 9.686547586 9.686419473 9.686356066 | p 1.01 | u 6.3e-05 | 4.8e-07 | 1.92e-04 | 25.11 | 1 / 0 / 0 | 1.0 |
| A2p | 7.173508105 7.173553985 7.173565710 7.173568620 | p 2.01 | u 9.7e-07 | 7.173547643 7.173563085 7.173567441 7.173568620 | p 1.89 | u 4.4e-07 | 7.173987438 7.173772098 7.173671715 7.173621749 7.173595454 | p 0.93 | u 2.9e-05 | 1.4e-06 | 5.09e-04 | 27.66 | 14 / 0 / 0 | 2.6 |
| A2c | 9.690481914 9.690478800 9.690478122 9.690477965 | p 2.10 | u 5.3e-08 | 9.690505194 9.690484462 9.690479265 9.690477965 | p 2.00 | u 4.3e-07 | 9.691531929 9.690991651 9.690731251 9.690603332 9.690540010 | p 1.01 | u 6.3e-05 | 4.9e-07 | 1.94e-04 | 25.1 | 1 / 0 / 0 | 1.6 |
| A3p | 7.173429601 7.173475569 7.173487339 7.173490266 | p 2.01 | u 9.8e-07 | 7.173469362 7.173484704 7.173489082 7.173490266 | p 1.89 | u 4.4e-07 | 7.173907394 7.173695327 7.173592908 7.173543090 7.173517467 | p 0.96 | u 2.7e-05 | 1.4e-06 | 5.11e-04 | 27.66 | 14 / 0 / 0 | 1.7 |
| A3c | 9.694706398 9.694703540 9.694702895 9.694702745 | p 2.11 | u 5.0e-08 | 9.694729787 9.694709197 9.694704036 9.694702745 | p 2.00 | u 4.3e-07 | 9.695751999 9.695214573 9.694955003 9.694827551 9.694764782 | p 1.02 | u 6.3e-05 | 4.8e-07 | 1.92e-04 | 25.09 | 1 / 0 / 0 | 1.0 |
| A4p | 7.173508913 7.173554732 7.173566445 7.173569400 | p 1.99 | u 1.0e-06 | 7.173549231 7.173564007 7.173568230 7.173569400 | p 1.85 | u 4.5e-07 | 7.173986485 7.173771955 7.173672025 7.173622271 7.173596070 | p 0.93 | u 2.9e-05 | 1.4e-06 | 5.22e-04 | 27.66 | 14 / 0 / 0 | 2.8 |
| A4c | 9.690439869 9.690436752 9.690436074 9.690435917 | p 2.10 | u 5.3e-08 | 9.690462793 9.690442329 9.690437200 9.690435917 | p 2.00 | u 4.3e-07 | 9.691489062 9.690949187 9.690688999 9.690561185 9.690497916 | p 1.01 | u 6.3e-05 | 4.8e-07 | 1.92e-04 | 25.1 | 1 / 0 / 0 | 1.5 |
| A5p | 7.173507359 7.173553235 7.173564963 7.173567872 | p 2.01 | u 9.7e-07 | 7.173546605 7.173562268 7.173566680 7.173567872 | p 1.89 | u 4.4e-07 | 7.173985807 7.173770902 7.173670750 7.173620894 7.173594651 | p 0.93 | u 2.9e-05 | 1.4e-06 | 5.10e-04 | 27.66 | 14 / 0 / 0 | 2.8 |
| A5c | 9.690523953 9.690520840 9.690520163 9.690520006 | p 2.11 | u 5.2e-08 | 9.690546880 9.690526418 9.690521289 9.690520006 | p 2.00 | u 4.3e-07 | 9.691572928 9.691033180 9.690773039 9.690645249 9.690581992 | p 1.01 | u 6.3e-05 | 4.8e-07 | 1.91e-04 | 25.1 | 1 / 0 / 0 | 1.6 |
| B1p | 9.640156216 9.640223782 9.640241821 9.640246476 | p 1.95 | u 1.6e-06 | 9.640245298 9.640246214 9.640246414 9.640246476 | p 1.68 | u 2.8e-08 | 9.640952069 9.640522576 9.640358686 9.640295280 9.640268927 | p 1.27 | u 2.6e-05 | 1.6e-06 | 4.37e-04 | 37.69 | 6 / 0 / 0 | 3.2 |
| B1c | 12.502190159 12.502162405 12.502155465 12.502153729 | p 2.00 | u 5.8e-07 | 12.502160123 12.502155278 12.502154040 12.502153729 | p 1.99 | u 1.0e-07 | 12.503771537 12.502960943 12.502557221 12.502355124 12.502254054 | p 1.00 | u 1.0e-04 | 6.8e-07 | 1.89e-04 | 36.16 | 1 / 0 / 0 | 1.8 |
| B2p | 9.387524046 9.387530909 9.387533088 9.387533710 | p 1.81 | u 2.5e-07 | 9.387523939 9.387530239 9.387532841 9.387533710 | p 1.58 | u 4.4e-07 | 9.388594077 9.388036932 9.387789288 9.387657399 9.387595229 | p 1.09 | u 6.2e-05 | 6.9e-07 | 1.74e-04 | 39.34 | 24 / 0 / 0 | 2.3 |
| B2c | 14.490996769 14.490991021 14.490989593 14.490989240 | p 2.02 | u 1.2e-07 | 14.490995918 14.490990823 14.490989556 14.490989240 | p 2.00 | u 1.1e-07 | 14.492101723 14.491545122 14.491267449 14.491127650 14.491058387 | p 1.01 | u 6.9e-05 | 2.2e-07 | 6.32e-05 | 35.36 | 1 / 0 / 0 | 1.8 |
| B3p | 9.385229538 9.385238601 9.385240867 9.385241435 | p 2.00 | u 1.9e-07 | 9.385248648 9.385243153 9.385241779 9.385241435 | p 2.00 | u 1.1e-07 | 9.386130714 9.385686491 9.385463981 9.385352782 9.385297177 | p 1.00 | u 5.6e-05 | 3.0e-07 | 7.73e-05 | 39.34 | 22 / 0 / 0 | 2.0 |
| B3c | 14.499794284 14.499764322 14.499756828 14.499754953 | p 2.00 | u 6.3e-07 | 14.499761213 14.499756443 14.499755251 14.499754953 | p 2.00 | u 9.9e-08 | 14.501399780 14.500574726 14.500164969 14.499959701 14.499856921 | p 1.00 | u 1.0e-04 | 7.3e-07 | 2.05e-04 | 35.38 | 1 / 0 / 0 | 3.5 |
| C2p | 7.122139476 7.122180546 7.122191115 7.122193799 | p 1.98 | u 9.1e-07 | 7.122147724 7.122181813 7.122191239 7.122193799 | p 1.88 | u 9.5e-07 | 7.122724590 7.122448537 7.122320856 7.122258189 7.122225828 | p 0.95 | u 3.5e-05 | 1.9e-06 | 6.74e-04 | 27.73 | 34 / 0 / 0 | 2.0 |
| C2c | 9.607487522 9.607485406 9.607485058 9.607484992 | p 2.41 | u 2.2e-08 | 9.607511106 9.607491281 9.607486254 9.607484992 | p 1.99 | u 4.2e-07 | 9.608529452 9.607988566 9.607731911 9.607606149 9.607544599 | p 1.03 | u 6.2e-05 | 4.4e-07 | 1.73e-04 | 25.66 | 1 / 0 / 0 | 1.2 |
| C4p | 9.951372893 9.951426795 9.951441038 9.951444642 | p 1.98 | u 1.2e-06 | 9.951407756 9.951435391 9.951442729 9.951444642 | p 1.94 | u 6.8e-07 | 9.952275559 9.951822642 9.951622970 9.951532741 9.951489240 | p 1.05 | u 4.4e-05 | 1.9e-06 | 4.89e-04 | 38.77 | 25 / 0 / 0 | 4.1 |
| C4c | 12.208446492 12.208440491 12.208439606 12.208439459 | p 2.58 | u 4.9e-08 | 12.208451111 12.208442267 12.208440022 12.208439459 | p 1.99 | u 1.9e-07 | 12.210057667 12.209206370 12.208802778 12.208614980 12.208524842 | p 1.06 | u 9.0e-05 | 2.4e-07 | 6.48e-05 | 36.74 | 1 / 0 / 0 | 3.3 |
| C8p | 13.723746100 13.723807206 13.723824580 13.723829057 | p 1.96 | u 1.6e-06 | 13.723805950 13.723824079 13.723828290 13.723829057 | p 2.46 | u 2.6e-07 | 13.725198753 13.724437486 13.724098135 13.723953703 13.723889601 | p 1.17 | u 6.4e-05 | 1.8e-06 | 3.39e-04 | 53.43 | 18 / 0 / 0 | 5.1 |
| C8c | 15.710935548 15.710910516 15.710905891 15.710904967 | p 2.32 | u 3.1e-07 | 15.710910311 15.710906257 15.710905226 15.710904967 | p 1.99 | u 8.7e-08 | 15.713693354 15.712135962 15.711469709 15.711165557 15.711027931 | p 1.14 | u 1.4e-04 | 3.9e-07 | 7.71e-05 | 51.23 | 1 / 0 / 0 | 2.9 |
| D12p | 16.411244188 16.411305746 16.411323386 16.411328279 | p 1.85 | u 1.9e-06 | 16.411313058 16.411325568 16.411328108 16.411328279 | p 3.89 (non-asym) | u 2.5e-06 | 16.413295371 16.412152700 16.411684519 16.411488824 16.411401204 | p 1.16 | u 8.8e-05 | 4.4e-06 | 6.93e-04 | 63.72 | 16 / 0 / 0 | 8.2 |
| D12c | 18.206805089 18.206747545 18.206736777 18.206734543 | p 2.27 | u 7.4e-07 | 18.206738000 18.206735379 18.206734711 18.206734543 | p 1.99 | u 5.6e-08 | 18.210920807 18.208469048 18.207488926 18.207072510 18.206888270 | p 1.18 | u 1.8e-04 | 8.0e-07 | 1.31e-04 | 61.35 | 1 / 0 / 0 | 4.3 |
| DSPYp | 93.317459671 93.318244462 93.318446904 93.318499708 | p 1.94 | u 1.9e-05 | 93.317871085 93.318331900 93.318463368 93.318499708 | p 1.86 | u 1.4e-05 | 93.331955402 93.323710836 93.320719449 93.319493384 93.318913220 | p 1.08 | u 5.8e-04 | 3.3e-05 | 8.50e-04 | 382.7 | 15 / 0 / 0 | 6.2 |
| DSPYc | 149.653939241 149.653338950 149.653216580 149.653189585 | p 2.18 | u 9.0e-06 | 149.653162742 149.653186436 149.653189114 149.653189585 | p 2.51 | u 1.6e-07 | 149.691736112 149.669911925 149.660617640 149.656578081 149.654775417 | p 1.16 | u 1.8e-03 | 9.2e-06 | 2.08e-04 | 440.6 | 1 / 0 / 0 | 3.8 |
| E1p | 8.736947443 8.737019333 8.737038025 8.737042693 | p 2.00 | u 1.6e-06 | 8.737038161 8.737041444 8.737042438 8.737042693 | p 1.96 | u 8.8e-08 | 8.737410436 8.737243099 8.737144539 8.737091549 8.737068411 | p 1.20 | u 2.3e-05 | 1.6e-06 | 4.27e-04 | 38.51 | 21 / 0 / 0 | 4.1 |
| E1c | 13.877730283 13.877700911 13.877693664 13.877691864 | p 2.01 | u 6.0e-07 | 13.877698311 13.877693400 13.877692171 13.877691864 | p 2.00 | u 1.0e-07 | 13.879384975 13.878527322 13.878106614 13.877897770 13.877794047 | p 1.01 | u 1.0e-04 | 7.0e-07 | 1.96e-04 | 35.89 | 1 / 0 / 0 | 3.7 |
| E5p | 10.755629411 10.755706789 10.755726233 10.755731153 | p 1.98 | u 1.7e-06 | 10.755725524 10.755729673 10.755730809 10.755731153 | p 1.73 | u 1.5e-07 | 10.756015185 10.755894309 10.755814525 10.755770014 10.755751939 | p 1.30 | u 1.8e-05 | 1.8e-06 | 4.73e-04 | 38.34 | 8 / 0 / 0 | 3.3 |
| E5c | 12.163310947 12.163303023 12.163301207 12.163300786 | p 2.11 | u 1.4e-07 | 12.163312380 12.163303548 12.163301338 12.163300786 | p 2.00 | u 1.8e-07 | 12.164853504 12.164053294 12.163672090 12.163485003 12.163391695 | p 1.00 | u 9.3e-05 | 3.2e-07 | 9.42e-05 | 34.47 | 1 / 0 / 0 | 1.8 |
| E20p | 20.630802069 20.630844060 20.630854601 20.630857298 | p 1.97 | u 9.3e-07 | 20.630848013 20.630855016 20.630856850 20.630857298 | p 2.03 | u 1.5e-07 | 20.630888573 20.630900284 20.630880863 20.630863688 20.630861614 | p 3.05 (non-asym) | u 1.7e-05 | 1.1e-06 | 3.67e-04 | 29.35 | 8 / 0 / 0 | 4.1 |
| E20c | 10.873880296 10.873863215 10.873859045 10.873858027 | p 2.03 | u 3.4e-07 | 10.873871733 10.873861291 10.873858679 10.873858027 | p 2.00 | u 2.2e-07 | 10.875404943 10.874623199 10.874237770 10.874046985 10.873951515 | p 1.00 | u 9.6e-05 | 5.6e-07 | 2.10e-04 | 26.51 | 1 / 0 / 0 | 3.2 |
| E64p | 2.718075170 2.718075172 2.718075173 2.718075173 | p 2.00 | u 5.9e-11 | 2.718075176 2.718075173 2.718075173 2.718075173 | p 4.07 (non-asym) | u 1.9e-10 | 2.718075173 2.718075173 2.718075173 2.718075173 2.718075173 | p 0.00 (non-asym) | u 2.6e-12 | 2.5e-10 | n/a | 1.446e-09 | 7 / 0 / 0 | 1.4 |
| E64c | 1.378804801 1.378804798 1.378804798 1.378804798 | p 2.00 | u 5.9e-11 | 1.378804796 1.378804797 1.378804798 1.378804798 | p 4.03 (non-asym) | u 1.0e-10 | 1.378804798 1.378804797 1.378804797 1.378804797 1.378804797 | p 1.15 | u 2.6e-12 | 1.6e-10 | n/a | -1.43e-09 | 1 / 0 / 0 | 1.5 |
| F1c | 31.310952901 31.310936783 31.310932748 31.310931741 | p 2.00 | u 3.4e-07 | 31.310927990 31.310930832 31.310931559 31.310931741 | p 1.99 | u 6.1e-08 | 31.310996892 31.310964223 31.310947558 31.310939451 31.310935452 | p 1.02 | u 4.0e-06 | 4.0e-07 | 2.14e-03 | 1.851 | 1 / 0 / 0 | 1.1 |
| F1p | 33.045425838 33.045395084 33.045387578 33.045385696 | p 2.00 | u 6.3e-07 | 33.045295988 33.045363427 33.045381070 33.045385696 | p 1.93 | u 1.6e-06 | 33.045536305 33.045466705 33.045424350 33.045405738 33.045395862 | p 0.91 | u 1.1e-05 | 2.3e-06 | 2.59e-03 | 8.789 | 24 / 0 / 0 | 2.2 |
| F2c | 20.884643824 20.884627608 20.884623541 20.884622525 | p 2.00 | u 3.4e-07 | 20.884620890 20.884621883 20.884622385 20.884622525 | p 1.84 | u 5.4e-08 | 20.884716519 20.884668787 20.884645419 20.884633836 20.884628021 | p 0.99 | u 5.9e-06 | 3.9e-07 | 1.38e-03 | 2.857 | 1 / 0 / 0 | 1.3 |
| F2p | 27.898593704 27.898572318 27.898567006 27.898565685 | p 2.01 | u 4.4e-07 | 27.898506429 27.898550234 27.898562393 27.898565685 | p 1.88 | u 1.2e-06 | 27.898652408 27.898605931 27.898585618 27.898575971 27.898571160 | p 1.00 | u 4.8e-06 | 1.7e-06 | 5.10e-03 | 3.258 | 35 / 0 / 0 | 1.0 |
| G1p | 11.521054143 11.521057995 11.521059021 11.521059282 | p 1.98 | u 8.9e-08 | 11.521065899 11.521060857 11.521059597 11.521059282 | p 2.00 | u 1.0e-07 | 11.522268933 11.521661421 11.521359390 11.521208983 11.521134002 | p 1.00 | u 7.5e-05 | 1.9e-07 | 4.93e-05 | 39.3 | 3 / 74 / 742 | 1.6 |
| G1c | 11.220261559 11.220265217 11.220266362 11.220266666 | p 1.91 | u 1.1e-07 | 11.220278477 11.220269480 11.220267229 11.220266666 | p 2.00 | u 1.9e-07 | 11.221873124 11.221064853 11.220656573 11.220459429 11.220362418 | p 1.02 | u 9.7e-05 | 3.0e-07 | 8.20e-05 | 36.25 | 17 / 2239 / 1024 | 2.5 |
| G4p | 12.066318826 12.066320314 12.066320994 12.066321207 | p 1.68 | u 9.6e-08 | 12.066327478 12.066322724 12.066321511 12.066321207 | p 1.99 | u 1.0e-07 | 12.067692388 12.066975103 12.066639037 12.066476334 12.066397540 | p 1.05 | u 7.9e-05 | 2.0e-07 | 5.07e-05 | 39.04 | 3 / 27 / 393 | 2.3 |
| G4c | 10.658327342 10.658335863 10.658338681 10.658339470 | p 1.84 | u 3.1e-07 | 10.658355506 10.658343315 10.658340241 10.658339470 | p 2.00 | u 2.6e-07 | 10.660118680 10.659186268 10.658740021 10.658530090 10.658432413 | p 1.10 | u 9.8e-05 | 5.6e-07 | 1.52e-04 | 37.01 | 38 / 4061 / 1024 | 5.1 |
| H1p | 9.554007585 9.554081705 9.554100688 9.554105486 | p 1.98 | u 1.6e-06 | 9.554099131 9.554103757 9.554105145 9.554105486 | p 2.03 | u 1.1e-07 | 9.554469572 9.554294241 9.554201726 9.554150850 9.554129256 | p 1.24 | u 2.2e-05 | 1.7e-06 | 4.57e-04 | 37.97 | 8 / 0 / 0 | 3.8 |
| H1c | 12.654074931 12.654058625 12.654054657 12.654053682 | p 2.02 | u 3.3e-07 | 12.654061948 12.654055651 12.654054076 12.654053682 | p 2.00 | u 1.3e-07 | 12.655631104 12.654829682 12.654437264 12.654243648 12.654147889 | p 1.02 | u 9.6e-05 | 4.6e-07 | 1.29e-04 | 35.42 | 1 / 0 / 0 | 1.4 |
| H4p | 9.763943182 9.763996214 9.764010262 9.764013899 | p 1.95 | u 1.3e-06 | 9.763975862 9.764004303 9.764011918 9.764013899 | p 1.94 | u 7.0e-07 | 9.764827126 9.764409652 9.764195831 9.764103435 9.764057937 | p 1.02 | u 4.5e-05 | 2.0e-06 | 5.17e-04 | 38.03 | 25 / 0 / 0 | 2.7 |
| H4c | 12.027389231 12.027381832 12.027380532 12.027380264 | p 2.28 | u 8.9e-08 | 12.027391877 12.027383062 12.027380825 12.027380264 | p 1.99 | u 1.9e-07 | 12.029012272 12.028153585 12.027747493 12.027557790 12.027466941 | p 1.06 | u 9.1e-05 | 2.8e-07 | 7.71e-05 | 35.99 | 1 / 0 / 0 | 2.0 |
| ILp | 4.475012410 4.475030364 4.475034852 4.475035966 | p 2.01 | u 3.7e-07 | 4.475041229 4.475037170 4.475036198 4.475035966 | p 2.06 | u 7.7e-08 | 4.474959288 4.475001283 4.475019271 4.475028058 4.475032212 | p 1.08 | u 4.2e-06 | 4.5e-07 | 3.30e-03 | 1.358 | 15 / 0 / 0 | 2.2 |
| ILc | 2.677572569 2.677572548 2.677572555 2.677572557 | p 2.52 | u 4.3e-10 | 2.677578409 2.677573953 2.677572836 2.677572557 | p 2.00 | u 9.3e-08 | 2.677872404 2.677721672 2.677647094 2.677609622 2.677590965 | p 1.01 | u 1.9e-05 | 9.4e-08 | 6.35e-04 | 1.475 | 1 / 0 / 0 | 1.3 |
| ISp | 3.821062863 3.821005869 3.821009745 3.821010704 | p 2.01 | u 3.2e-07 | 3.821002149 3.821008601 3.821010273 3.821010704 | p 1.96 | u 1.5e-07 | 3.821318987 3.820890486 3.820851911 3.821000087 3.820972074 | p 0.00 (non-asym) | u 1.5e-04 | 4.7e-07 | n/a | -0.313 | 15 / 0 / 0 | 2.2 |
| ISc | 2.702162935 2.702167992 2.702168037 2.702168048 | p 2.02 | u 3.7e-09 | 2.702174851 2.702169671 2.702168373 2.702168048 | p 2.00 | u 1.1e-07 | 2.702431101 2.702318309 2.702250034 2.702204137 2.702188225 | p 1.53 | u 1.6e-05 | 1.1e-07 | 6.97e-04 | 1.607 | 1 / 0 / 0 | 1.0 |
<!-- END D0 CONVERGENCE TABLE -->

Reading it: the space axis is asymptotic on every row, at `p` 1.68-2.58. The time axis is
asymptotic at `p` 1.58-2.51 on all but three rows. D12p (3.89) falls outside the window and gets
the envelope, not the Richardson correction; E64p / E64c (4.07 / 4.03) are already at 1e-10.
The lattice runs near its theoretical order, 0.91-1.53. It is non-asymptotic only where it is
already at rounding (E64p) or oscillating (E20p, ISp). The rounding-tie rule fires only on the
`r = 0` rows, the case it was written for: G1p 74 steps, G1c 2239, G4p 27, G4c 4061, out of
~1.2e4 time steps per ladder at `T 1`. It never accepts above its 1024-ulp bound.

## 7. Summary by event count and tenor, and the step ladder

<!-- BEGIN D0 SUMMARY TABLE -->
| events | tenor | n | max abs d301 USD | max abs d301 IV bp (inv) | max abs dcmp USD | max abs dcmp bp | cmp claim violated | UNRESOLVED |
|---|---|---|---|---|---|---|---|---|
| 1 | <= 0.5 y | 18 | 8.76e-03 | 5.895 | 2.61e+00 | 2.256 | 15 | 5 |
| 1 | 0.5-1.5 y | 16 | 7.28e-02 | 19.125 | 2.89e-03 | 0.817 | 2 | 0 |
| 2-4 | <= 1.5 y | 8 | 6.58e-02 | 16.870 | 8.96e-03 | 2.312 | 2 | 0 |
| 5-8 | 1.5-2.5 y | 2 | 1.44e-01 | 26.821 | 9.32e-03 | 1.818 | 0 | 0 |
| 9-12 | > 2 y | 4 | 4.21e-01 | 34.017 | 7.18e-02 | 1.629 | 1 | 0 |
| all | all | 48 | 4.21e-01 | 34.017 | 2.61e+00 | 2.312 | 20 | 5 |
<!-- END D0 SUMMARY TABLE -->

(`cmp claim violated` counts rows where `|cmp - ref| > claim + u_ref`. The survivor rows are
excluded from every bucket.)

The step ladder below prices each IV-identified kernel-model row with its default scheme at 301,
601, 1201 and 2401 steps. It is linearised IV bp, the same sign as the table above, per row for
the multi-event chains and as a maximum per bucket. Then the per-price cost on the 12-event LEAPS
(best of 5 on this host, while two other lanes were running):

<!-- BEGIN D0 STEP LADDER TABLE -->
| id | events | T | scheme | IV bp at 301 | 601 | 1201 | 2401 |
|---|---|---|---|---|---|---|---|
| B1p | 1 | 1 | spliced | +19.325 | +20.289 | -0.180 | -0.220 |
| B1c | 1 | 1 | spliced | +17.019 | +19.245 | +0.601 | -0.234 |
| C2p | 2 | 0.5 | spliced | -10.445 | -4.581 | -0.973 | -1.226 |
| C2c | 2 | 0.5 | spliced | -7.012 | -2.825 | -1.030 | -1.246 |
| C4p | 4 | 1 | spliced | -16.940 | -9.231 | -3.945 | -0.816 |
| C4c | 4 | 1 | spliced | -16.529 | -7.564 | -3.015 | -0.853 |
| C8p | 8 | 2 | spliced | -26.928 | -15.741 | -8.560 | -3.557 |
| C8c | 8 | 2 | spliced | -25.044 | -15.699 | -7.783 | -3.053 |
| D12p | 12 | 3 | spliced | -32.946 | -20.717 | -12.089 | -6.018 |
| D12c | 12 | 3 | spliced | -34.171 | -19.910 | -12.242 | -5.914 |
| DSPYp | 9 | 2.182 | spliced | -8.668 | -6.220 | -4.290 | -2.840 |
| DSPYc | 9 | 2.182 | spliced | -9.547 | -3.919 | -2.747 | -2.875 |
| G4p | 4 | 1 | spliced | -16.848 | -9.176 | -3.909 | -0.826 |
| G4c | 4 | 1 | spliced | -16.392 | -7.589 | -3.098 | -0.788 |
| H4p | 4 | 1 | escrowed-lr | +0.015 | +0.014 | +0.005 | +0.002 |
| H4c | 4 | 1 | escrowed-lr | -0.217 | -0.190 | -0.081 | -0.035 |

| events | tenor | n (IV-identified) | max abs IV bp at 301 | 601 | 1201 | 2401 |
|---|---|---|---|---|---|---|
| 1 | <= 0.5 y | 16 | 5.807 | 7.036 | 3.533 | 1.005 |
| 1 | 0.5-1.5 y | 16 | 19.325 | 20.289 | 1.466 | 0.731 |
| 2-4 | <= 1.5 y | 8 | 16.940 | 9.231 | 3.945 | 1.246 |
| 5-8 | 1.5-2.5 y | 2 | 26.928 | 15.741 | 8.560 | 3.557 |
| 9-12 | > 2 y | 4 | 34.171 | 20.717 | 12.242 | 6.018 |
| all | all | 46 | 34.171 | 20.717 | 12.242 | 6.018 |

Per-price cost on D12p (SplicedCrr, best of 5, us): 301 38; 601 98; 1201 272; 2401 888; refined comparator 1356.
<!-- END D0 STEP LADDER TABLE -->

## 8. Derivative uncertainty (subset)

Delta and vega by bump and revalue on the reference. Every bumped price is a full PDE ladder
(800 / 200 per year, 4 levels) on a grid held fixed across the sigma bumps. There are three bump
sizes (2 %, 1 %, 0.5 % of `S`; 0.01, 0.005, 0.0025 in `sigma`) and a Richardson extrapolation
over them; `u` is that extrapolation's uncertainty plus the bumped prices' own uncertainty over
`2h`. The table groups six cases by event count and tenor: one event at `T` 0.5 / 1 (A2p,
E20p, F1c; B3p's cash lands on expiry), four at `T 1` (C4c), and twelve at `T 3` (D12p). The comparison is against the
kernel's `american_discrete_div_greek_bundle` (301 steps).

<!-- BEGIN D0 DERIVATIVE TABLE -->
| id | greek | bump study (h: value) | extrapolated +- u | grid (PDE finest) | lattice 301 | d301 |
|---|---|---|---|---|---|---|
| A2p | delta | 2: -0.48222531; 1: -0.48204307; 0.5: -0.48199735 | -0.48198203 +- 2.2e-05 | -0.48198204 | -0.48205866 | -7.66e-05 |
| A2p | vega | 0.01: 27.66013732; 0.005: 27.66038239; 0.0025: 27.66041960 | 27.66043200 +- 1.3e-03 | n/a | 27.70602799 | +4.56e-02 |
| B3p | delta | 2: -0.43395979; 1: -0.43382835; 0.5: -0.43379545 | -0.43378447 +- 1.2e-05 | -0.43378441 | -0.43388818 | -1.04e-04 |
| B3p | vega | 0.01: 39.34296820; 0.005: 39.34313136; 0.0025: 39.34317205 | 39.34318562 +- 2.9e-04 | n/a | 39.39163877 | +4.85e-02 |
| C4c | delta | 2: 0.63353691; 1: 0.63372267; 0.5: 0.63376916 | 0.63378469 +- 1.7e-05 | 0.63378474 | 0.63340172 | -3.83e-04 |
| C4c | vega | 0.01: 36.73856148; 0.005: 36.73984487; 0.0025: 36.74016568 | 36.74027262 +- 3.3e-04 | n/a | 36.70157143 | -3.87e-02 |
| D12p | delta | 2: -0.42161746; 1: -0.42154399; 0.5: -0.42152505 | -0.42151848 +- 1.6e-05 | -0.42151906 | -0.42121570 | +3.03e-04 |
| D12p | vega | 0.01: 63.71732038; 0.005: 63.71789261; 0.0025: 63.71797563 | 63.71800330 +- 1.8e-03 | n/a | 63.95150459 | +2.34e-01 |
| E20p | delta | 2: -0.73490860; 1: -0.73504588; 0.5: -0.73508010 | -0.73509151 +- 1.6e-05 | -0.73509134 | -0.73528752 | -1.96e-04 |
| E20p | vega | 0.01: 29.33428199; 0.005: 29.34376350; 0.0025: 29.34612518 | 29.34691240 +- 1.8e-03 | n/a | 29.32732144 | -1.96e-02 |
| F1c | delta | 2: 0.98951479; 1: 0.98963586; 0.5: 0.98966604 | 0.98967610 +- 1.2e-05 | 0.98967603 | 0.98977176 | +9.57e-05 |
| F1c | vega | 0.01: 1.85587154; 0.005: 1.85212472; 0.0025: 1.85118773 | 1.85087527 +- 6.7e-04 | n/a | 1.88186792 | +3.10e-02 |
<!-- END D0 DERIVATIVE TABLE -->

The PDE's own grid delta (central difference at the centre node, finest grid) matches the
bump-extrapolated delta to at most 5.8e-7 (D12p), within the reference's `u`. The 301 lattice's
delta is off by up to 3.8e-4 (C4c) and its vega by up to +0.234 (D12p, 0.37 % of vega). Like
its price, its greek error grows with the event count: vega error 0.02-0.05 at one event (A2p,
E20p, F1c), -0.039 at four (C4c), +0.234 at twelve (D12p).

## 9. Decision memo: is the 301-step default adequate?

**Recommendation: EVENT-ADAPTIVE, not RAISE.** Keep 301 steps for chains with at most one
in-life ex-date that is not inside the first lattice step. Do not treat a 301-step IV on a
multi-event spot-shift chain as accurate, and do not answer that by raising the global step
count: the multi-event error decays only about first order in N. A dividend inside the first
step is different: a larger N does fix it (item 2), so adaptive N is a measured option there.
Nothing is promoted by this lane.

The evidence, every number from the run of section 10 (IV bp by inversion unless marked):

1. **One event, `T <= 1`: adequate to about 6 bp.** A1p-A5p -3.12 to -3.16; E1p -6.09, E5p -4.81,
   E20p -4.12; E1c / E5c / E20c -4.93 / -1.74 / +1.34; G1p -3.29; B2 / B3 -1.2 to -3.7. With no
   in-life event (B3, cash on expiry) the 301 lattice is still -1.96 (put) / -1.21 (call): that
   is the CRR floor at this step count. The error does NOT grow with the cash's size (E1 to E20).
2. **The exception inside one event: a dividend in the first step.** B1p +19.12, B1c +16.99
   (inverted). The step ladder (section 7, linearised) prices B1 at 301 / 601 / 1201 / 2401 steps
   as +19.3 / +20.3 / -0.18 / -0.22 bp (put) and +17.0 / +19.2 / +0.60 / -0.23 bp (call). At 301
   and 601 steps the ex-date (0.0023 y) falls in the first or second step; at 1201 it is 2.8 steps
   in, and the error collapses to under 1 bp. So the first-step case IS fixed by more steps,
   provided N puts the ex-date a few steps in. An event-adaptive N for first-step chains is a
   measured option. The single-event 0.5-1.5 y bucket reaches 1.47 bp at 1201 steps.
3. **Alignment is a step function in the ex-date.** Moving the ex-date 0.02 of a step across the
   rounding boundary (A4c -> A5c) moves the 301 call by 2.46 bp (+1.91 -> -0.55). The reference
   moves 8.4e-5 USD (0.03 bp). The put rows A1/A2/A4 price identically on the lattice.
4. **Multi-event chains: the measured per-event error by tenor and shape.** Each figure is the
   total IV error (put / call, inverted) divided by the event count, averaged over put and call.
   - Quarterly 1-USD chains from 0.10 at S 100: C2 (`T 0.5`) -10.40 / -7.09, -4.4 per event;
     C4 (`T 1`) -16.87 / -16.55, -4.2; G4 (`T 1`, r = 0) -16.78 / -16.39, -4.1; C8 (`T 2`)
     -26.82 / -24.83, -3.2; D12 (`T 3`) -32.81 / -34.02, -2.8.
   - The SPY shape (9 events of 1.83 USD = 0.24 % of spot, `T 2.18`, sigma 0.35): DSPY
     -8.71 / -9.60, -1.0 per event. This is consistent with L3f's 9 bp p99.

   In USD the 1-USD chains' error is +0.009 to +0.018 per event. The bp figure falls with tenor
   because vega grows. Each total includes the no-event CRR floor at 301 steps, -1.2 to -2.0 bp
   (B3). **The scaling in the cash amount is NOT identified.** The single-event rows do not grow
   with the cash (E1 / E5 / E20 puts -6.1 / -4.8 / -4.1 bp at 1 / 5 / 20 % of spot), and no
   matrix row combines several events with large cash. So neither a per-cash-size rule nor a
   size-independent per-event rule is supported, and the SPY shape's smaller per-event figure
   cannot be attributed to its smaller cash, its sigma or its tenor.
5. **Raising N is the wrong lever (section 7 step ladder, linearised).** D12p goes
   -32.9 / -20.7 / -12.1 / -6.0 at 301 / 601 / 1201 / 2401 steps, C8p -26.9 / -15.7 / -8.6 / -3.6,
   and DSPYp -8.7 / -6.2 / -4.3 / -2.8: roughly first order in N. The cost on D12p is
   38 / 98 / 272 / 888 us, about N^1.5 (this run; the previous run measured 51 / 130 / 386 / 1290
   on a busier host, the same shape). 2401 steps is 23x the cost for a 5.5x smaller error, and
   it still leaves 6 bp on a 3-year quarterly chain.
6. **The escrowed lattice: 301 steps CAN carry four events.** `EscrowedLeisenReimer` at 301 on
   the same event schedules is within 0.84 bp of its own reference (H1c +0.84, H4c -0.22,
   H1p -0.12, H4p +0.015). It is a different model, so this is not a substitute. It also changes
   the tree family (Leisen-Reimer, not CRR) as well as the event treatment, so it does not
   isolate the cause. **That the multi-event error sits in the spot-shift splice is a
   hypothesis.** No ablation was run; the kernel's `SplicedBbs` / `SplicedBbsr` were not run at
   301 on C4 or D12.
7. **The refined comparator is not a reference.** Its claim `|fine - coarse|` is violated on
   20 of 48 kernel-model rows, its worst identified error is 2.31 bp (C4p), and on E64 it is off
   by 2.61 USD (section 5). It costs 1356 us on D12p, 36x the 301 lattice (this run).

**What "event-adaptive" should mean (for the follow-up lane, not done here).**
- (a) Gate: flag the 301 IV on multi-event spot-shift chains as biased low, by the per-event
  ranges of item 4 at the matching tenor, rather than serving it as accurate. Item 4 does not
  identify how that bias scales with the cash amount; add a multi-event large-cash row (for
  example 4 x 5 %) before sizing it for such chains.
- (b) First-step chains: choose N so the first ex-date sits a few steps in. 1201 steps did this
  for B1 (item 2).
- (c) Fix, a hypothesis to test first: the splice's interpolation. One reading of the
  size-independence of item 1 is that the per-event error comes from the splice's
  interpolation, not from the jump size. Run the `SplicedBbs` / `SplicedBbsr` 301 ablation on
  C4 and D12 before building on it.

Re-score any fix against this matrix: the slow suite prints every column above. A fix should be
judged on C4, C8, D12 and DSPY at 301 steps, with the A/B alignment rows as the regression set.

## 10. Reproduce, and the run this document records

From the repo root of a tree configured with `vol-rel`:

```powershell
powershell -NoProfile -File scripts\atx-build.ps1 build atx-vol-discrete-div-reference-tests -Preset vol-rel -Jobs 1
# fast suite (24 tests, ~8 s): the anchors in section 3
powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset vol-rel -R "^atx-vol-discrete-div-reference$"
# slow matrix (one gtest case, ~3 min on this host): sections 5-8
$env:ATX_VOL_DDIV_REF_OUT = '<a NEW, existing directory>'   # optional: tables.md + matrix.csv
build-vol-rel\bin\atx-vol-discrete-div-reference-tests.exe --gtest_filter=DiscreteDivReferenceMatrix.*
# or: powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset vol-rel -R "^atx-vol-discrete-div-reference-matrix$"
```

**The recorded run.** Commit `cc89bc89` (branch `feat/vol-D0-cashref-20261022`, review fix
round 1), 2026-10-22, wall 186.6 s, exit 0, `[  PASSED  ] 1 test`. The summary line was
`SUMMARY cases 50 unresolved 5 price_only 2 methods_disagree 0`.
- Evidence: `C:\atx-cache\quote-benchmark\D0-20261022-fix1\matrix-run-cc89bc89\` (log,
  `tables.md`, `matrix.csv`, binary). Its manifest is
  `C:\atx-cache\quote-benchmark\D0-20261022-fix1\sha256.txt`, in the PARENT directory.
- Earlier runs: `8da986d0` (`D0-20261022-matrix\run-8da986d0\`, 183.7 s) and `dc6a6ab4`
  (`run-dc6a6ab4\`, 173.8 s). Their manifest is `D0-20261022-matrix\sha256.txt`.
- All three printed byte-identical comparison, convergence (except the wall column), summary
  and derivative tables. The step ladder was added at 8da986d0. The cross-check table and the
  B1 ladder rows were added at cc89bc89, together with review fixes m1 and m3, which no matrix
  row reaches.

**Additive kernel change, parity.** The kernel diff is additive (section 2). The dividend parity
cohorts re-ran on the bench built from this branch at `dc6a6ab4` (`atx-vol-quote-bench`, sha256
`9289555daac528fad9472f7464d599f50f3f97845c2c4a2a0830e69cf0f23115`) and reproduced both control
hashes exactly: dev200 `abc75955...`, dev200b `747d3e6c...`
(`C:\atx-cache\quote-benchmark\D0-20261022-parity\`). The kernel and every TU the bench
calls are unchanged since; the reference TU is linked into the library but never called by the
bench.

**Andersen-Lake, observed and not adjudicated.** On five of the six no-dividend anchors the PDE
sits 1.5-5.2e-6 USD ABOVE Andersen-Lake at its high preset `{32, 64, 16, 1e-13}`, and on the
sixth 9.4e-8 below. AL's production accurate preset sits 0-2.5e-5 BELOW its high preset, so AL
converges from below. Every gap is inside the PDE's stated uncertainty, which is all the fast
test asserts.


## 11. D1 production: splice fix, routes, batch, Greeks, latency (lane D1, 2026-10-23)

This section is lane D1's. The D0 sections above are unchanged. They are the reference this
section is scored against. Every table here is pasted verbatim from one run of the production
suite's slow tests:
- binary `atx-vol-discrete-div-production-tests`, sha256 `c3793e5f...` (branch
  `feat/vol-D1-cash-production-20261022`, code at `0b69b219`);
- log `C:\atx-cache\quote-benchmark\D1-20261023-production\run5-final-slow.log`.

### 11.1 The splice defect, and what the production lattice changes

Section 9 item 6 left the cause of the multi-event error as a hypothesis. It is the splice's
linear interpolation. At an ex-date the continuation is re-read at each node's post-dividend
level. The historical splice reads it off the chord between the two bracketing nodes. The chord
of a convex function lies above it by (x - x_j)(x_{j+1} - x) V''/2, a positive bias of order
h^2 V'' = O(1/N) at every ex-date, and the events add up: +0.0165 USD per quarterly 1-USD event
at 301 steps (table 11.3, `hist301`).

The production rollback (`ddiv_detail::run_production_lattice`) uses the same tree, the same
probabilities and the same final step, with four changes:
1. **The cubic splice.** A four-point Lagrange on the same geometric nodes, O(h^4) on smooth
   data.
2. **Forward-preserving re-dating.** Cash rounded onto step t_k is scaled by
   exp(-(r - q)(tau - t_k)).
3. **Re-timed exercise at an ex-step.** Cum / post exercise is valued at the true ex-date.
   This removes the step function of section 9 item 3.
4. **Pre-steps.** The tree is rooted before valuation, so an ex-date in the first steps has the
   nodes a splice needs, and delta / gamma are read at t = 0.

With every correction off it is the historical rollback bit for bit (fast test
`AllCorrectionsOffIsTheHistoricalRollbackBitForBit`, 96 combinations).

### 11.2 The route rule (kernel header section 4; `discrete_div_lattice_route`)

A route is a pure function of (S, T, schedule, opts): no sigma, rate or strike. So a root find
and a batch group each see one discretisation. Every price, IV and Greek result carries the
`route` it used.

| Condition | Method, steps | Flag | Evidence (D0 rows, this section's re-score) |
|---|---|---|---|
| kill switch | historical SplicedCrr at base steps (301), call for call | `KillSwitch` | parity hashes `abc75955` / `747d3e6c` |
| Standard, <= 12 in-life events | CubicCrr at 301 | — | every RESOLVED row within 3 bp (worst B2c -2.865); C8 / D12 at -0.6..-2.0 bp against -24.8..-34.0 historical |
| Standard, > 12 events | CubicCrr at 25 x events, capped at 2401 | `StepsRaisedForEvents` (`StepsCapped`) | the kernel header's per-event rule. No D0 row exercises it (every D0 chain has at most 12 events), so it is not certified by this matrix |
| an ex-date in the first steps | pre-steps: the smallest even count (>= 2) that keeps the 6-sd band's post-dividend levels on the grid, evaluated at sigma 0.10; cap 1024 | `PreStepsForEarlyExDate` | B1p / B1c: +19.1 / +17.0 bp historical, -1.97 / -1.29 Standard, -0.110 / -0.078 Refined |
| an ex-date within half a step of t = 0 | spliced on the t = 0 layer (>= 4 pre-steps) | `ExDateOnStepZero` | fast route test |
| cash beyond the band | the pre-step cap, depth reported | `GridBottomExtrapolated` | E64 needs 334 / 688 / 940 pre-steps at 301 / 1201 / 2401: inside the cap, priced to 1e-12 USD (11.4). At the checkpoint's cap of 64 the Refined route missed E64p by +1.28 USD |
| Refined | BBS final step + Richardson pair (1201, 601) | — | every RESOLVED row within 1 bp (worst E20c -0.646) |
| Refined, an in-life ex-date in (T - 2T/601, T) | CubicCrr at 2401, no Richardson | `NearExpiryExDate` | B2c +1.885 bp (checkpoint re-score) under the pair, because the coarse lattice's BBS step spans the ex-date the fine lattice splices; +0.114 bp on this route. B2p -0.268 |

No global raise of 301: every D0 chain (at most 12 events) routes at 301 on Standard.

### 11.3 The splice-fix verdict: per-event drift on the 8- and 12-event chains

**Method.** Each chain is truncated to its first k events (k = 0, 1, 2, 4, 8, 12). Each
truncation is priced against a LIVE reference: D0's PDE ladder, 1200 / 300 per year, 4 levels.
The per-event drift is the endpoint (e(K) - e(0)) / K. Its reference uncertainty is
u_drift = (u(K) + u(0)) / K.

Per-truncation errors (USD):

| case | k | ref +- u | hist301 err | std err | refined err |
|---|---|---|---|---|---|
| C8p | 0 | 10.8797793614 +- 2.37e-06 | +8.444e-03 | +8.444e-03 | +6.857e-05 |
| C8p | 1 | 11.2812204067 +- 2.42e-06 | +2.830e-02 | +8.284e-03 | +7.203e-05 |
| C8p | 2 | 11.7095986995 +- 2.55e-06 | +4.868e-02 | +8.702e-03 | +4.156e-05 |
| C8p | 4 | 12.5428778953 +- 2.73e-06 | +8.421e-02 | +9.548e-03 | +1.035e-04 |
| C8p | 8 | 13.7238308662 +- 1.81e-06 | +1.439e-01 | +1.048e-02 | +7.975e-05 |
| C8c | 0 | 20.2071615268 +- 1.46e-06 | +4.330e-03 | +4.330e-03 | +3.501e-05 |
| C8c | 1 | 19.5128418128 +- 1.46e-06 | +1.945e-02 | +4.305e-03 | +3.524e-05 |
| C8c | 2 | 18.8511592401 +- 1.44e-06 | +3.528e-02 | +4.437e-03 | +3.428e-05 |
| C8c | 4 | 17.6275253285 +- 1.41e-06 | +6.739e-02 | +4.335e-03 | +2.884e-05 |
| C8c | 8 | 15.7109045721 +- 3.95e-07 | +1.283e-01 | +3.190e-03 | +5.150e-05 |
| D12p | 0 | 12.5480204737 +- 2.99e-06 | +8.311e-03 | +8.311e-03 | +9.252e-05 |
| D12p | 1 | 12.9291778776 +- 3.04e-06 | +3.035e-02 | +8.086e-03 | +9.902e-05 |
| D12p | 2 | 13.3344086809 +- 3.17e-06 | +5.340e-02 | +8.091e-03 | +1.052e-04 |
| D12p | 4 | 14.1543934975 +- 3.33e-06 | +9.573e-02 | +8.752e-03 | +8.781e-05 |
| D12p | 8 | 15.5257666713 +- 3.51e-06 | +1.564e-01 | +9.739e-03 | +3.268e-04 |
| D12p | 12 | 16.4113301577 +- 4.42e-06 | +2.099e-01 | +1.120e-02 | +4.431e-05 |
| D12c | 0 | 24.8599087191 +- 2.44e-06 | +1.095e-02 | +1.095e-02 | -6.560e-05 |
| D12c | 1 | 24.1417468431 +- 2.43e-06 | +2.640e-02 | +1.093e-02 | -6.398e-05 |
| D12c | 2 | 23.4509238309 +- 2.42e-06 | +4.232e-02 | +1.086e-02 | -6.319e-05 |
| D12c | 4 | 22.1527703196 +- 2.38e-06 | +7.515e-02 | +1.055e-02 | -6.423e-05 |
| D12c | 8 | 19.8932926729 +- 2.31e-06 | +1.433e-01 | +1.034e-02 | -7.607e-05 |
| D12c | 12 | 18.2067337416 +- 8.01e-07 | +2.096e-01 | +1.147e-02 | -6.521e-05 |

Summary (USD per event):

| case | events | u(K) | u_drift | hist301 per event | std per event | refined per event | within u_drift: std / refined |
|---|---|---|---|---|---|---|---|
| C8p | 8 | 1.81e-06 | 5.23e-07 | +1.693e-02 | +2.545e-04 | +1.397e-06 | NO / NO |
| C8c | 8 | 3.95e-07 | 2.32e-07 | +1.550e-02 | -1.424e-04 | +2.061e-06 | NO / NO |
| D12p | 12 | 4.42e-06 | 6.18e-07 | +1.680e-02 | +2.410e-04 | -4.018e-06 | NO / NO |
| D12c | 12 | 8.01e-07 | 2.70e-07 | +1.656e-02 | +4.279e-05 | +3.199e-08 | NO / yes |

**Verdict.**
- The linear-splice defect is gone. The historical +0.0155..+0.0169 USD per event falls by 98.5
  to 99.7 % on the Standard route and by 3.6 to 6 orders of magnitude on the Refined one.
- **The brief's bound, "per-event drift within the reference uncertainty", holds on one case of
  four.** It holds on D12c (Refined, 3.2e-8 against u_drift 2.7e-7). It misses on C8p, C8c and
  D12p (Refined +1.4e-6 / +2.1e-6 / -4.0e-6 against u_drift 2.3-6.2e-7, a factor of 3-9). The
  Standard route misses everywhere: 4.3e-5..2.5e-4 USD, under 0.05 bp per event.
- **Why the Refined misses are a floor, not a trend.** The Refined route's own errors scatter by
  about +-3e-5 USD between truncations (the k rows above; D12p at k = 8 is the outlier,
  +3.3e-4). Divided by K that scatter is 2.5-4e-6
  per event, the size of the misses. The least-squares slopes (+2.8e-6, +1.9e-6, +3.8e-6,
  -4.3e-7) are of the same size and flip sign against the endpoint on D12p and D12c. Every
  Refined truncation is within 3.3e-4 USD of its reference.

### 11.4 Re-score of D0's 50-row matrix

The kernel IV bp is `(sigma the route implies from the REFERENCE price - sigma) x 1e4`, through
`american_discrete_div_production_iv` at tol 1e-9. `hist301` is the historical 301 SplicedCrr
inverted the same way. It matches section 5's `d301 IV bp (inv)` to 0.002 bp (the inversion
tolerance is 1e-9 here and 1e-7 there). Every Standard route below is the route the rule picks;
`301+n` means n pre-steps.

| id | status (D0) | ref +- u | hist301 bp (inv) | std route | std USD | std bp (inv) | refined USD | refined bp (inv) |
|---|---|---|---|---|---|---|---|---|
| A1p | RESOLVED | 7.1736405 +- 1.4e-06 | -3.135 | cubic-crr 301+2 | +6.17e-03 | -2.230 | +1.27e-05 | -0.005 |
| A1c | RESOLVED | 9.6862934 +- 4.8e-07 | +0.247 | cubic-crr 301+2 | -3.47e-03 | +1.394 | +3.29e-04 | -0.131 |
| A2p | RESOLVED | 7.1735700 +- 1.4e-06 | -3.161 | cubic-crr 301+2 | +5.65e-03 | -2.041 | +1.88e-05 | -0.007 |
| A2c | RESOLVED | 9.6904775 +- 4.9e-07 | +1.929 | cubic-crr 301+2 | -6.01e-03 | +2.451 | -9.65e-04 | +0.387 |
| A3p | RESOLVED | 7.1734917 +- 1.4e-06 | -3.129 | cubic-crr 301+2 | +6.17e-03 | -2.227 | +2.29e-05 | -0.008 |
| A3c | RESOLVED | 9.6947023 +- 4.8e-07 | -0.003 | cubic-crr 301+2 | -2.85e-03 | +1.162 | -6.61e-04 | +0.265 |
| A4p | RESOLVED | 7.1735708 +- 1.4e-06 | -3.160 | cubic-crr 301+2 | +5.66e-03 | -2.043 | +2.37e-05 | -0.009 |
| A4c | RESOLVED | 9.6904354 +- 4.8e-07 | +1.912 | cubic-crr 301+2 | -5.99e-03 | +2.441 | -9.41e-04 | +0.377 |
| A5p | RESOLVED | 7.1735693 +- 1.4e-06 | -3.120 | cubic-crr 301+2 | +6.16e-03 | -2.227 | +1.10e-05 | -0.004 |
| A5c | RESOLVED | 9.6905195 +- 4.8e-07 | -0.548 | cubic-crr 301+2 | -1.47e-03 | +0.595 | +5.14e-04 | -0.205 |
| B1p | RESOLVED | 9.6402481 +- 1.6e-06 | +19.125 | cubic-crr 301+8 | +7.43e-03 | -1.970 | +4.13e-04 | -0.110 |
| B1c | RESOLVED | 12.5021530 +- 6.8e-07 | +16.989 | cubic-crr 301+8 | +4.65e-03 | -1.293 | +2.82e-04 | -0.078 |
| B2p | RESOLVED | 9.3875344 +- 6.9e-07 | -1.380 | cubic-crr 301+2 | +8.23e-03 | -2.090 | +1.06e-03 | -0.268 |
| B2c | RESOLVED | 14.4909890 +- 2.2e-07 | -3.698 | cubic-crr 301+2 | +1.01e-02 | -2.865 | -4.02e-04 | +0.114 |
| B3p | RESOLVED | 9.3852415 +- 3.0e-07 | -1.962 | cubic-crr 301+2 | +7.73e-03 | -1.962 | -4.51e-05 | +0.011 |
| B3c | RESOLVED | 14.4997542 +- 7.3e-07 | -1.209 | cubic-crr 301+2 | +4.26e-03 | -1.209 | -4.33e-07 | +0.000 |
| C2p | RESOLVED | 7.1221957 +- 1.9e-06 | -10.401 | cubic-crr 301+2 | +6.11e-03 | -2.203 | +3.92e-05 | -0.014 |
| C2c | RESOLVED | 9.6074845 +- 4.4e-07 | -7.089 | cubic-crr 301+2 | -3.14e-03 | +1.241 | +2.89e-04 | -0.112 |
| C4p | RESOLVED | 9.9514465 +- 1.9e-06 | -16.870 | cubic-crr 301+6 | +8.12e-03 | -2.091 | +4.39e-05 | -0.011 |
| C4c | RESOLVED | 12.2084392 +- 2.4e-07 | -16.552 | cubic-crr 301+6 | +5.43e-03 | -1.486 | -6.01e-04 | +0.164 |
| C8p | RESOLVED | 13.7238309 +- 1.8e-06 | -26.820 | cubic-crr 301+4 | +1.05e-02 | -1.960 | +7.97e-05 | -0.015 |
| C8c | RESOLVED | 15.7109046 +- 3.9e-07 | -24.828 | cubic-crr 301+4 | +3.19e-03 | -0.621 | +5.15e-05 | -0.010 |
| D12p | RESOLVED | 16.4113302 +- 4.4e-06 | -32.813 | cubic-crr 301+4 | +1.12e-02 | -1.757 | +4.43e-05 | -0.007 |
| D12c | RESOLVED | 18.2067337 +- 8.0e-07 | -34.016 | cubic-crr 301+4 | +1.15e-02 | -1.867 | -6.52e-05 | +0.011 |
| DSPYp | RESOLVED | 93.3185322 +- 3.3e-05 | -8.710 | cubic-crr 301+4 | +1.69e-02 | -0.444 | +1.61e-03 | -0.042 |
| DSPYc | RESOLVED | 149.6531807 +- 9.2e-06 | -9.598 | cubic-crr 301+4 | +5.67e-02 | -1.294 | -1.64e-03 | +0.037 |
| E1p | RESOLVED | 8.7370443 +- 1.6e-06 | -6.086 | cubic-crr 301+2 | +7.62e-03 | -1.977 | +6.60e-05 | -0.017 |
| E1c | RESOLVED | 13.8776912 +- 7.0e-07 | -4.932 | cubic-crr 301+2 | +4.17e-03 | -1.168 | -3.52e-06 | +0.001 |
| E5p | RESOLVED | 10.7557330 +- 1.8e-06 | -4.812 | cubic-crr 301+2 | +6.55e-03 | -1.706 | +6.15e-05 | -0.016 |
| E5c | RESOLVED | 12.1633005 +- 3.2e-07 | -1.744 | cubic-crr 301+2 | +2.66e-03 | -0.775 | -8.96e-04 | +0.257 |
| E20p | RESOLVED | 20.6308584 +- 1.1e-06 | -4.118 | cubic-crr 301+2 | +9.21e-04 | -0.314 | +7.58e-05 | -0.026 |
| E20c | RESOLVED | 10.8738575 +- 5.6e-07 | +1.339 | cubic-crr 301+2 | -8.30e-04 | +0.314 | +1.79e-03 | -0.646 |
| E64p | PRICE-ONLY (IV not identified) | 2.7180752 +- 2.5e-10 | +nan | cubic-crr 301+334 | -4.40e-13 | +nan | -1.11e-14 | +nan |
| E64c | PRICE-ONLY (IV not identified) | 1.3788048 +- 1.6e-10 | +nan | cubic-crr 301+334 | +7.15e-13 | +nan | +2.63e-13 | +nan |
| F1c | UNRESOLVED (u > 1e-7 IV) | 31.3109315 +- 4.0e-07 | +0.235 | cubic-crr 301+2 | -5.73e-04 | +2.827 | +1.77e-04 | -0.953 |
| F1p | UNRESOLVED (u > 1e-7 IV) | 33.0453867 +- 2.3e-06 | -0.346 | cubic-crr 301+2 | -1.34e-04 | +0.155 | +8.26e-06 | -0.009 |
| F2c | UNRESOLVED (u > 1e-7 IV) | 20.8846222 +- 3.9e-07 | +3.030 | cubic-crr 301+2 | -1.11e-03 | +3.507 | +2.73e-05 | -0.095 |
| F2p | UNRESOLVED (u > 1e-7 IV) | 27.8985665 +- 1.7e-06 | +2.280 | cubic-crr 301+2 | -1.40e-03 | +4.016 | +6.43e-06 | -0.020 |
| G1p | RESOLVED | 11.5210593 +- 1.9e-07 | -3.292 | cubic-crr 301+2 | +7.43e-03 | -1.890 | -7.58e-06 | +0.002 |
| G1c | RESOLVED | 11.2202666 +- 3.0e-07 | -1.586 | cubic-crr 301+2 | +9.78e-04 | -0.273 | +1.97e-03 | -0.550 |
| G4p | RESOLVED | 12.0663212 +- 2.0e-07 | -16.780 | cubic-crr 301+6 | +7.96e-03 | -2.038 | -2.21e-05 | +0.006 |
| G4c | RESOLVED | 10.6583395 +- 5.6e-07 | -16.386 | cubic-crr 301+6 | +7.11e-03 | -1.925 | +3.05e-04 | -0.083 |
| H1p | RESOLVED (not the D1 convention) | 9.5541072 +- 1.7e-06 | +nan | cubic-crr 301+2 | +1.65e-01 | +nan | +1.58e-01 | +nan |
| H1c | RESOLVED (not the D1 convention) | 12.6540532 +- 4.6e-07 | +nan | cubic-crr 301+2 | +1.50e-01 | +nan | +1.46e-01 | +nan |
| H4p | RESOLVED (not the D1 convention) | 9.7640159 +- 2.0e-06 | +nan | cubic-crr 301+6 | +1.96e-01 | +nan | +1.87e-01 | +nan |
| H4c | RESOLVED (not the D1 convention) | 12.0273800 +- 2.8e-07 | +nan | cubic-crr 301+6 | +1.86e-01 | +nan | +1.80e-01 | +nan |
| ILp | UNRESOLVED (u > 1e-7 IV) | 4.4750363 +- 4.5e-07 | -5.894 | cubic-crr 301+334 | -3.28e-04 | +2.471 | -5.82e-04 | +4.486 |
| ILc | RESOLVED | 2.6775725 +- 9.4e-08 | +3.129 | cubic-crr 301+334 | -3.29e-04 | +2.248 | -3.23e-06 | +0.022 |
| ISp | CONVENTION GAP (kernel = liquidator) (not the D1 convention) | 3.8210112 +- 4.7e-07 | +nan | cubic-crr 301+334 | +6.54e-01 | +nan | +6.53e-01 | +nan |
| ISc | CONVENTION GAP (kernel = liquidator) (not the D1 convention) | 2.7021679 +- 1.1e-07 | +nan | cubic-crr 301+334 | -2.49e-02 | +nan | -2.46e-02 | +nan |

Worst RESOLVED |IV bp|: **Standard -2.865 (B2c), Refined -0.646 (E20c).** The historical 301
lattice reaches -34.016 (D12c).

These two numbers are the routes' ACCURACY CLASS: Standard <= 3 bp and Refined <= 1 bp of IV
on D0's 41 RESOLVED rows, in one convention (spot-shift liquidator). They are the lane's
published class, not a brief bound. The plan's D1 objective is "converged model reference
within frozen price / root tolerances", and the brief's bar is D0's own u. That objective is
NOT met: the per-event drift misses u on 3 of 4 chains on Refined and on all 4 on Standard
(11.3). Refined C8 / D12 sit at 0.007-0.015 bp, above the 1e-7 IV root tolerance (review-D1,
ruling A(i)).

**Rows the claim does not cover, stated.**
- **UNRESOLVED (5): F1c, F1p, F2c, F2p, ILp.** The reference cannot certify a root at the 1e-7
  IV contract on these rows (section 5).
  - Standard: +2.83 / +0.16 / +3.51 / +4.02 / +2.47 bp.
  - Refined: -0.95 / -0.01 / -0.10 / -0.02 / **+4.49** bp.
  - ILp's Refined miss is DECIDABLE. The reference's u is 4.5e-7 USD (0.003 bp), so +4.49 bp
    is 1,300 times u. The Refined route is not reference-grade on a put whose spot loses 60 %
    to cash (sigma 0.8, 2.3 sd). The Standard route is closer there.
- **PRICE-ONLY (2): E64p, E64c.** The IV is not identified (vega ~1e-9).
  - Both routes price them to within 1e-12 USD of the reference (Standard -4.4e-13 / +7.2e-13,
    Refined -1.1e-14 / +2.6e-13).
  - The production inversion refuses a quote at or below the exercise value as price-only. A
    quote on the American plateau identifies no volatility.
- **CONVENTION GAP (2): ISp, ISc.** The survivor convention is not implemented; the kernel is
  the liquidator, as before. The gap is +0.654 / -0.025 USD.
- **Group H (escrowed).** H is a different model. The spot-shift routes sit 0.15-0.19 USD from
  it, which measures the convention, not an error.

### 11.5 The comparator refuses cash-dominated contracts (L3f review m1)

`american_discrete_div_converged_price` now returns OutOfRange for a contract whose spot,
after the cash paid up to an ex-date tau in (0, T], sits at or below S exp(-6 sigma sqrt(tau)),
or at or below zero.
- **Why.** Its lattices would otherwise read the spot's post-dividend continuation off the
  grid-bottom extrapolation, and `fine - coarse` stops being an uncertainty.
- **What trips it in D0's matrix.** E64 alone: 40 sd, +2.61 USD in section 5. B1 is at
  2.5 sd and IL at 2.3 sd; both stay served. The D0 slow test now expects the refusal on E64
  and success elsewhere.
- **The run.** `run6-d0-matrix.log`, the same binary set, re-ran section 5-8's matrix. Its
  E64 `refined cmp` column reads `nan`.

### 11.6 Sensitivities against bump-and-revalue on the reference

**What the bundle is** (`american_discrete_div_production_greeks`; the batch's `greeks()` is
bit-identical). Fix round 1 (review-D1 M1) changed delta, gamma and vega.
- **delta and gamma:** the t = 0 stencil. On the Refined route they take the BBS pair's
  Richardson, (n d_n - m d_m) / (n - m), as the price does.
- **vega:** the Richardson pair of central sigma bumps at h = 10 % and h / 2,
  (4 v(h / 2) - v(h)) / 3.
- **rho / phi:** central 1e-3 bumps.
- **dP/dD_i:** amount bumps.
- **theta / charm:** the fine lattice's two-step stencils.
- **the event-aware one-day roll.**

Each is computed on one route with fixed nodes. An unsupported greek is NaN, with its bit
and a typed reason (`DiscreteDivUnsupportedReason`), never 0.

**The vega rule.** Vega is UNSUPPORTED (reason `ExerciseDominated`) on an American call with
an in-life ex-date tau_i where both hold:
- early exercise just before tau_i can pay, D_i > K (1 - e^{-r (t_next - tau_i)});
- the cum-dividend dated forward there is at least 2 sd in the money,
  ln(F(tau_i-) / K) >= 2 sigma sqrt(tau_i).

On D0's 50 rows the rule covers F1c (z = 2.23), E64c (price-only anyway, z = 9.6) and no
RESOLVED row. The study below brackets it with D0 group F's other rows, where vega is served
and holds its claim:
- F2c, a call 1.90 sd in the money;
- F1p and F2p, the puts.

**What the reference is.** D0's PDE on a fixed common-mode grid (800 / 200 per year at
level 2), with central differences at h and h / 2 and one Richardson step. Delta and gamma
are the PDE's own centre-node stencil. `+- u` is the reference's own uncertainty for that
greek and row:
- delta and gamma: the PDE ladder's, (4/3) |x(level 3) - x(level 2)|, or D0 section 8's u
  where it is larger;
- the roll: the same ladder estimate;
- vega: the larger of the bump spread and D0 section 8's u;
- the rest: the bump spread.

The lattice columns are Standard at 301 / 601 / 1201 and Refined (the step study).
`refined err / u` and `bound` refer to the claim below. Run: `D1-20261023-fix1`
`run7-final-slow.log` (binary sha256 6555ad42..., code at 59cdf815). Its re-score and drift
tables reproduce sections 11.3-11.4 line for line (81 of 81 lines).

| id | greek | reference +- u | std 301 | std 601 | std 1201 | refined | err std 301 | err refined | refined err / u | bound |
|---|---|---|---|---|---|---|---|---|---|---|
| A2p | delta | -0.48198164 +- 2.2e-05 | -0.481666 | -0.481824 | -0.481902 | -0.481981 | +3.15e-04 | +4.78e-07 | 0.0 | 6.60e-05 |
| A2p | gamma | 0.023820959 +- 1.3e-08 | 0.0237588 | 0.0237898 | 0.0238053 | 0.0238208 | -6.22e-05 | -1.42e-07 | 10.7 | - |
| A2p | vega | 27.660532 +- 1.3e-03 | 27.6861 | 27.6743 | 27.6669 | 27.6596 | +2.56e-02 | -9.49e-04 | 0.7 | 2.77e-02 |
| A2p | rho | -20.794507 +- 6.0e-04 | -20.8212 | -20.8096 | -20.7995 | -20.7886 | -2.67e-02 | +5.86e-03 | 9.8 | 1.04e-01 |
| A2p | phi | 18.379389 +- 1.9e-04 | 18.4003 | 18.391 | 18.3829 | 18.3742 | +2.09e-02 | -5.21e-03 | 27.1 | - |
| A2p | theta | -5.2288344 +- 1.5e-04 | -5.23659 | -5.23289 | -5.23103 | -5.23197 | -7.76e-03 | -3.14e-03 | 21.3 | - |
| A2p | dP/dD1 | 0.51413842 +- 5.8e-08 | 0.513996 | 0.514012 | 0.514082 | 0.514152 | -1.43e-04 | +1.32e-05 | 227.0 | 2.00e-03 |
| A2p | roll 1d | -0.020813763 +- 2.2e-07 | -0.0207408 | -0.0207264 | -0.0207201 | -0.0207146 | +7.29e-05 | +9.91e-05 | 455.3 | 2.00e-03 |
| B3p | delta | -0.43378418 +- 1.2e-05 | -0.433409 | -0.433619 | -0.43369 | -0.433785 | +3.75e-04 | -3.29e-07 | 0.0 | 5.00e-05 |
| B3p | gamma | 0.015739895 +- 3.7e-09 | 0.0156984 | 0.0157298 | 0.0157295 | 0.01574 | -4.15e-05 | +6.04e-08 | 16.3 | - |
| B3p | vega | 39.343181 +- 2.9e-04 | 39.3902 | 39.4626 | 39.3645 | 39.3429 | +4.70e-02 | -2.53e-04 | 0.9 | 3.93e-02 |
| B3p | rho | -52.743096 +- 5.6e-03 | -52.7645 | -52.7485 | -52.7472 | -52.7413 | -2.14e-02 | +1.78e-03 | 0.3 | 2.64e-01 |
| B3p | phi | 43.364927 +- 1.8e-03 | 43.3781 | 43.3734 | 43.3679 | 43.3644 | +1.32e-02 | -4.86e-04 | 0.3 | - |
| B3p | theta | -2.8081719 +- 8.6e-07 | -2.81161 | -2.81342 | -2.80904 | -2.80976 | -3.44e-03 | -1.59e-03 | 1857.1 | - |
| B3p | dP/dD1 | 0.51202446 +- 7.2e-06 | 0.510932 | 0.498021 | 0.510958 | 0.512032 | -1.09e-03 | +7.11e-06 | 1.0 | 2.00e-03 |
| B3p | roll 1d | -0.011162424 +- 5.3e-09 | -0.0111824 | -0.0112415 | -0.0111724 | -0.0111623 | -2.00e-05 | +1.44e-07 | 27.0 | 2.00e-03 |
| C4c | delta | 0.63378496 +- 1.7e-05 | 0.633937 | 0.633897 | 0.633833 | 0.633775 | +1.52e-04 | -1.00e-05 | 0.6 | 5.10e-05 |
| C4c | gamma | 0.015581996 +- 1.4e-08 | 0.0155443 | 0.0155715 | 0.0155764 | 0.0155831 | -3.76e-05 | +1.15e-06 | 81.0 | - |
| C4c | vega | 36.740286 +- 1.3e-03 | 36.5606 | 36.7177 | 36.6816 | 36.714 | -1.80e-01 | -2.63e-02 | 20.5 | 3.67e-02 |
| C4c | rho | 43.960915 +- 3.7e-02 | 43.8179 | 43.9959 | 43.9048 | 43.8584 | -1.43e-01 | -1.02e-01 | 2.8 | 2.20e-01 |
| C4c | phi | -53.81144 +- 2.3e-01 | -53.6764 | -53.9041 | -53.8231 | -53.787 | +1.35e-01 | +2.44e-02 | 0.1 | - |
| C4c | theta | -6.9162622 +- 3.4e-05 | -6.92268 | -6.9224 | -6.91915 | -6.91825 | -6.42e-03 | -1.99e-03 | 58.2 | - |
| C4c | dP/dD1 | -0.6215135 +- 1.1e-07 | -0.621559 | -0.621572 | -0.621537 | -0.621506 | -4.52e-05 | +7.22e-06 | 68.7 | 2.00e-03 |
| C4c | roll 1d | -0.027467955 +- 1.1e-07 | -0.0270435 | -0.0280939 | -0.0271827 | -0.0261712 | +4.24e-04 | +1.30e-03 | 11655.2 | 2.00e-03 |
| D12p | delta | -0.42151882 +- 1.6e-05 | -0.420774 | -0.421142 | -0.421329 | -0.421516 | +7.45e-04 | +2.56e-06 | 0.2 | 5.00e-05 |
| D12p | gamma | 0.0096382139 +- 1.2e-09 | 0.00960907 | 0.00962359 | 0.00963094 | 0.00963822 | -2.91e-05 | +2.50e-09 | 2.0 | - |
| D12p | vega | 63.718271 +- 1.8e-03 | 63.7645 | 63.746 | 63.7328 | 63.7195 | +4.62e-02 | +1.23e-03 | 0.7 | 6.37e-02 |
| D12p | rho | -153.12924 +- 5.1e-01 | -152.891 | -152.935 | -152.955 | -152.973 | +2.38e-01 | +1.56e-01 | 0.3 | 7.66e-01 |
| D12p | phi | 109.71669 +- 5.7e-03 | 109.597 | 109.687 | 109.711 | 109.735 | -1.20e-01 | +1.85e-02 | 3.2 | - |
| D12p | theta | -0.6694102 +- 5.2e-06 | -0.671369 | -0.670397 | -0.669926 | -0.670302 | -1.96e-03 | -8.92e-04 | 171.4 | - |
| D12p | dP/dD1 | 0.4258465 +- 4.3e-08 | 0.42568 | 0.425756 | 0.425799 | 0.425841 | -1.67e-04 | -5.83e-06 | 134.3 | 2.00e-03 |
| D12p | roll 1d | -0.0026601588 +- 1.8e-07 | -0.00234302 | -0.00267015 | -0.00268672 | -0.00270401 | +3.17e-04 | -4.39e-05 | 248.1 | 2.00e-03 |
| E20p | delta | -0.73509053 +- 1.6e-05 | -0.73382 | -0.734453 | -0.734772 | -0.735089 | +1.27e-03 | +1.11e-06 | 0.1 | 5.00e-05 |
| E20p | gamma | 0.015537642 +- 1.2e-08 | 0.015524 | 0.0155308 | 0.0155342 | 0.0155375 | -1.36e-05 | -1.21e-07 | 9.9 | - |
| E20p | vega | 29.34689 +- 9.5e-03 | 29.3761 | 29.3608 | 29.3543 | 29.3478 | +2.92e-02 | +9.11e-04 | 0.1 | 2.93e-02 |
| E20p | rho | -55.211237 +- 1.3e-03 | -55.228 | -55.2588 | -55.2099 | -55.1615 | -1.67e-02 | +4.97e-02 | 38.6 | 2.76e-01 |
| E20p | phi | 43.863039 +- 6.1e-04 | 43.8769 | 43.9033 | 43.8605 | 43.818 | +1.39e-02 | -4.50e-02 | 73.4 | - |
| E20p | theta | -1.0898609 +- 5.1e-05 | -1.09335 | -1.09161 | -1.09075 | -1.09073 | -3.49e-03 | -8.68e-04 | 17.1 | - |
| E20p | dP/dD1 | 0.76596261 +- 8.5e-08 | 0.765814 | 0.765685 | 0.765881 | 0.766076 | -1.49e-04 | +1.13e-04 | 1324.5 | 2.00e-03 |
| E20p | roll 1d | -0.0043288103 +- 2.6e-07 | -0.00101996 | -0.0037761 | -0.00381307 | -0.00385106 | +3.31e-03 | +4.78e-04 | 1843.1 | 2.00e-03 |
| F1c | delta | 0.98967585 +- 1.2e-05 | 0.989583 | 0.989642 | 0.989647 | 0.989654 | -9.28e-05 | -2.23e-05 | 1.9 | 5.00e-05 |
| F1c | gamma | 0.0016319532 +- 1.1e-08 | 0.00163944 | 0.00163439 | 0.00163446 | 0.00163442 | +7.49e-06 | +2.47e-06 | 230.4 | - |
| F1c | vega | 1.8508761 +- 3.7e-03 | UNSUPPORTED (NaN) | UNSUPPORTED (NaN) | UNSUPPORTED (NaN) | UNSUPPORTED (NaN) | +nan | +nan | nan | 3.70e-03 |
| F1c | rho | 30.447139 +- 7.9e-06 | 30.4527 | 30.4497 | 30.4479 | 30.446 | +5.52e-03 | -1.10e-03 | 140.6 | 1.52e-01 |
| F1c | phi | -44.5371 +- 4.2e-06 | -44.5424 | -44.5395 | -44.5378 | -44.5361 | -5.28e-03 | +1.03e-03 | 244.0 | - |
| F1c | theta | -3.216217 +- 1.6e-05 | -3.21268 | -3.21412 | -3.21554 | -3.21552 | +3.53e-03 | +7.00e-04 | 43.2 | - |
| F1c | dP/dD1 | -0.00052793797 +- 8.7e-08 | -0.000711452 | -0.000363144 | -0.000443573 | -0.000520805 | -1.84e-04 | +7.13e-06 | 82.4 | 2.00e-03 |
| F1c | roll 1d | -0.012750274 +- 1.3e-08 | -0.0127151 | -0.0126339 | -0.0127574 | -0.0128755 | +3.51e-05 | -1.25e-04 | 9694.1 | 2.00e-03 |
| B1p | delta | -0.46930873 +- 3.6e-07 | -0.469002 | -0.46915 | -0.469231 | -0.469312 | +3.07e-04 | -3.72e-06 | 10.3 | 5.00e-05 |
| B1p | gamma | 0.018533504 +- 4.7e-07 | 0.0184896 | 0.0185119 | 0.0185227 | 0.0185334 | -4.39e-05 | -1.06e-07 | 0.2 | - |
| B1p | vega | 37.686068 +- 1.0e-03 | 37.725 | 37.7043 | 37.6956 | 37.6871 | +3.90e-02 | +1.06e-03 | 1.1 | 3.77e-02 |
| B1p | rho | -36.676882 +- 1.6e-03 | -36.7458 | -36.7137 | -36.6931 | -36.674 | -6.89e-02 | +2.85e-03 | 1.8 | 1.83e-01 |
| B1p | phi | 31.152915 +- 5.6e-04 | 31.2012 | 31.1796 | 31.164 | 31.1498 | +4.83e-02 | -3.14e-03 | 5.6 | - |
| B1p | theta | -3.5289351 +- 1.0e-05 | UNSUPPORTED (NaN) | UNSUPPORTED (NaN) | -3.52982 | -3.53058 | +nan | -1.65e-03 | 160.8 | - |
| B1p | dP/dD1 | 0.46953479 +- 1.0e-07 | 0.469542 | 0.469389 | 0.469524 | 0.469659 | +7.12e-06 | +1.25e-04 | 1213.4 | 2.00e-03 |
| B1p | roll 1d | -0.013553926 +- 4.8e-07 | -0.0142075 | -0.0187193 | -0.0138087 | -0.0139313 | -6.54e-04 | -3.77e-04 | 782.5 | 2.00e-03 |
| F1p | delta | -0.93618755 +- 3.9e-08 | -0.935546 | -0.935884 | -0.936045 | -0.936187 | +6.42e-04 | +6.38e-07 | 16.5 | 5.00e-05 |
| F1p | gamma | 0.007157082 +- 4.3e-08 | 0.00718742 | 0.00717128 | 0.00716372 | 0.00715716 | +3.03e-05 | +7.46e-08 | 1.7 | - |
| F1p | vega | 8.7891866 +- 1.9e-03 | 8.85158 | 8.77205 | 8.75268 | 8.78971 | +6.24e-02 | +5.28e-04 | 0.3 | 8.79e-03 |
| F1p | rho | -57.259334 +- 7.1e-05 | -57.2637 | -57.2624 | -57.2609 | -57.2612 | -4.34e-03 | -1.91e-03 | 27.0 | 2.86e-01 |
| F1p | phi | 42.375199 +- 4.7e-05 | 42.3796 | 42.3786 | 42.3769 | 42.3769 | +4.38e-03 | +1.72e-03 | 36.6 | - |
| F1p | theta | 2.8322674 +- 9.5e-04 | 2.83603 | 2.83338 | 2.83186 | 2.83196 | +3.76e-03 | -3.10e-04 | 0.3 | - |
| F1p | dP/dD1 | 0.93695207 +- 8.8e-08 | 0.936879 | 0.936956 | 0.936922 | 0.936874 | -7.33e-05 | -7.77e-05 | 880.3 | 2.00e-03 |
| F1p | roll 1d | 0.011275157 +- 1.7e-06 | 0.0114568 | 0.0110104 | 0.0114013 | 0.0113353 | +1.82e-04 | +6.02e-05 | 35.7 | 2.00e-03 |
| F2c | delta | 0.97508376 +- 5.7e-07 | 0.974961 | 0.974945 | 0.975013 | 0.975081 | -1.23e-04 | -3.25e-06 | 5.7 | 5.00e-05 |
| F2c | gamma | 0.0047581562 +- 2.2e-08 | 0.00476837 | 0.00477228 | 0.00476543 | 0.00475855 | +1.02e-05 | +3.94e-07 | 17.8 | - |
| F2c | vega | 2.8569332 +- 1.3e-03 | 2.85271 | 2.85312 | 2.85363 | 2.85485 | -4.22e-03 | -2.09e-03 | 1.6 | 3.82e-03 |
| F2c | rho | 18.389777 +- 5.1e-06 | 18.3954 | 18.3906 | 18.3901 | 18.3897 | +5.59e-03 | -8.90e-05 | 17.5 | 9.19e-02 |
| F2c | phi | -23.402088 +- 3.9e-06 | -23.4074 | -23.4029 | -23.4024 | -23.402 | -5.33e-03 | +8.30e-05 | 21.1 | - |
| F2c | theta | -4.5519886 +- 5.5e-05 | -4.54422 | -4.55055 | -4.55127 | -4.55126 | +7.76e-03 | +7.27e-04 | 13.2 | - |
| F2c | dP/dD1 | -0.00010234356 +- 6.0e-08 | -0.000154527 | -8.15106e-05 | -9.10082e-05 | -0.000100014 | -5.22e-05 | +2.33e-06 | 38.7 | 2.00e-03 |
| F2c | roll 1d | -0.018014468 +- 1.1e-07 | -0.0174608 | -0.0177416 | -0.0182153 | -0.0186878 | +5.54e-04 | -6.73e-04 | 5908.9 | 2.00e-03 |
| F2p | delta | -0.97146775 +- 4.9e-07 | -0.971307 | -0.9713 | -0.971382 | -0.971466 | +1.60e-04 | +1.41e-06 | 2.9 | 5.00e-05 |
| F2p | gamma | 0.0052364872 +- 1.1e-07 | 0.00524786 | 0.00525226 | 0.00524464 | 0.00523671 | +1.14e-05 | +2.20e-07 | 2.0 | - |
| F2p | vega | 3.258195 +- 2.2e-03 | 3.25822 | 3.25461 | 3.25069 | 3.25911 | +2.91e-05 | +9.14e-04 | 0.4 | 6.49e-03 |
| F2p | rho | -30.032094 +- 1.4e-05 | -30.0368 | -30.0328 | -30.0324 | -30.032 | -4.68e-03 | +1.02e-04 | 7.6 | 1.50e-01 |
| F2p | phi | 23.335878 +- 1.4e-05 | 23.341 | 23.3367 | 23.3362 | 23.3358 | +5.08e-03 | -1.06e-04 | 7.7 | - |
| F2p | theta | 3.3690242 +- 1.5e-03 | 3.38055 | 3.36949 | 3.36736 | 3.36802 | +1.15e-02 | -1.00e-03 | 0.7 | - |
| F2p | dP/dD1 | 0.96912463 +- 1.3e-07 | 0.96923 | 0.969145 | 0.969123 | 0.969104 | +1.05e-04 | -2.03e-05 | 153.5 | 2.00e-03 |
| F2p | roll 1d | 0.01345013 +- 3.1e-06 | 0.0139048 | 0.0134874 | 0.0134054 | 0.0134155 | +4.55e-04 | -3.46e-05 | 11.2 | 2.00e-03 |

**Claims and what the study finds.** The slow test asserts the Refined route against each
claim's bound, min(the checkpoint's bound, max(3 u, class)). A bound is therefore never
looser than before, and it is 3 u wherever the reference resolves the greek finer than the
class.

| greek | checkpoint bound | class | worst Refined error (row, error / u) | verdict |
|---|---|---|---|---|
| delta | 2e-4 | 5e-5 | 2.2e-05 (F1c, 1.9 u; bound 5.00e-05) | holds |
| vega | 0.2 % | 0.1 % | 0.073 % (F2c, 1.6 u; bound 3.82e-03) | holds |
| rho | 0.5 % | 0.5 % | 0.232 % (C4c, 2.8 u; bound 2.20e-01) | holds |
| dP/dD1 | 2e-3 | 2e-3 | 1.3e-04 (B1p, 1213.4 u; bound 2.00e-03) | holds |
| roll 1d | 2e-3 USD | 2e-3 USD | 1.3e-03 (C4c, 11655.2 u; bound 2.00e-03) | holds |

Vega's class is not 3 u for a measured reason. C4c's Refined vega converges to -0.08 % as the
bump goes to 0 (`diag-scratch-bumps.log`), which is 20-80 times D0's u. That is the
lattice's own vega error, and no runtime rule can single the row out. A pure 3 u bound
would refuse a well-defined greek. The same holds for rho, dP/dD and the roll: their errors
sit far above the reference's tiny u, and their classes are the checkpoint's, unchanged.
These are accuracy classes, not claims to be within the reference's uncertainty.

The slow ctest is green (`ctest-production-final.log`). Before the fix it was red on the
three checkpoint claims (`ctest-matrix-red.log`), and on the new bounds and rule
(`gtest-m1-matrix-greeks-red.log`).

**rho and phi: a bump-size defect, fixed.** The checkpoint's bundle used the historical
bundle's 1e-4 rate / yield bumps. On D12p that gave Refined rho -150.51 and phi 108.74 against
the reference's -153.13 / 109.72 (-1.7 % / -0.9 %).
- **Diagnosis.** r and q move no node, but they flip exercise decisions node by node, so the
  lattice price is rippled in r and q below ~1e-3.
- **Evidence.** At matched bump sizes the lattice tracks D0's PDE ladder.
  - On D12p: rho secant at 1e-3, -152.955 (CubicCrr 1201) against -152.965 +- 0.006 (ladder).
  - At 5e-3: -150.959 against -150.937.
  - Log: `diag-scratch-rho-phi.log`.
- **The scan.** A scan of 1e-4 / 1e-3 / 2e-3 / 5e-3 and a Richardson pair over the seven study
  rows (`diag-scratch-bumps.log`) put the worst Refined error at:

| bump | rho | phi |
|---|---|---|
| 1e-4 | 1.7e-2 | 8.9e-3 |
| 1e-3 | 2.3e-3 | 1.0e-3 |
| 2e-3 | 4.3e-3 | 4.3e-3 |
| 5e-3 | 1.4e-2 | 5.8e-3 |
| Richardson pair | 2.8e-3 | 2.0e-3 |

The production bundle now uses 1e-3 (`kDiscreteDivProductionRateBump` /
`kDiscreteDivProductionYieldBump`). The historical bundle's constants are untouched. The fast
test `RhoAndPhiAgainstTheReferenceOnTheTwelveEventLeaps` pins D12p at 0.5 %. It was red first.

**Theta and the roll.**
- **Theta.** Where the route's two-step stencil differences across an ex-date (an in-life
  ex-date on steps 0..2), theta and charm are UNSUPPORTED. On B1p they are NaN with the bit on
  the 301 / 601 routes. At 1201 the ex-date is 2.8 steps in and theta is served: 1201 gives
  -3.5298 and Refined -3.5306, against the reference's -3.5289.
- **Roll.** The one-day roll crosses B1p's ex-date as an event: the 3.00 comes off the stock
  and the contract re-routes. Refined -0.01393 against the reference's -0.01355 USD.

### 11.7 Batching, latency and coverage

`DiscreteDivBatch::prepare` builds each (S, T, r, q, schedule) group's routes and step maps
once. The price, implied_vol and greeks loops then allocate nothing (allocation test: 0 in all
three loops). They are bit-identical to the per-contract entries (fast test).

**The measurement.** `atx-vol-ddiv-latency` (tools) times each entry per contract on one
pinned P-core logical CPU, with B0's receipts. Its cold group-calibration clock charges
`prepare()` to `family_build_validation` and the inversions to `inverses`. The stage table
reconciles: |unattributed| <= 5e-7 s.

**The cohort result.** The dev200b divsched cohort is 398 chains and 22,009 contracts, Standard
route. p50 per contract:
- cold standalone price 24.1 us (17.9 at 0 in-life events, 98.2 above 12);
- prepared price 23.8 us;
- identified IV 114.2 us;
- Greek bundle 191.3 us;
- group setup 2.8 us.

IV coverage on the bid/ask mid:
- 16,076 identified;
- 1,719 price-only;
- 0 refused;
- 6 out of budget;
- 4,208 with no two-sided market.

All 22,009 are priced.

**Against the budgets.** Every P1 / P2 / P3 budget is a MISS, by 1.8x (cold price, 0 events) up
to 37x (bundle, > 12 events). The table by event count and tenor, the Refined route and the
budget verdicts are in `docs/hft-support-matrix.md`.

**Fix round 1 (run `D1-20261023-fix1\latency\f1\`).** The re-measurement is on the same
cohort and CPU, at twice the host load (system 40 % against 21 %). p50:
- cold price 26.9 us; prepared price 26.6 us; IV 123.0 us. These legs are unchanged code,
  so the +10 % is the load.
- bundle 264.4 us. That is +2 rollbacks for the vega pair on top of the load.
- dominating stage of the cold group clock: inverses 99.94 % (2.6377 s of 2.6393 s);
  family_build_validation 1.6 ms.

Coverage also changed. 938 mid quotes strictly below the exercise value are now
RefusedRange (inconsistent quotes), where they were price-only. The new counts:
- 16,076 identified (unchanged);
- 781 price-only;
- 938 refused (range);
- 6 out of budget.

### 11.8 Reproduce

```powershell
powershell -NoProfile -File scripts\atx-build.ps1 build atx-vol-discrete-div-production-tests atx-vol-ddiv-latency -Preset vol-rel -Jobs 1
# fast suite (27 tests, ~1 s)
powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset vol-rel -R "^atx-vol-discrete-div-production$"
# slow: re-score + drift (~100-150 s) and the Greek study (~16-22 s)
build-vol-rel\bin\atx-vol-discrete-div-production-tests.exe --gtest_filter=DiscreteDivProductionMatrix.*
# latency / coverage (inputs: D1-20261023-production\scripts\build_ddiv_latency_inputs.py)
build-vol-rel\bin\atx-vol-ddiv-latency.exe --input <csv> --output <json> --affinity <P-core CPU> --require-pcore
```

Evidence: `C:\atx-cache\quote-benchmark\D1-20261023-production\` (manifest `sha256.txt`).

### 11.9 Fix round 1 (review-D1, 2026-10-23)

The review of a203eb19 returned BLOCK with two majors.
- **M1, the Greek study red.** Fixed in 11.6:
  - Richardson delta / gamma on the Refined route;
  - the Richardson vega pair;
  - the exercise-dominated vega rule;
  - bounds re-derived from the reference's u.
- **M2, archive.** `fit_archive_records.cpp` refused every archived SplicedCubic fit
  (RecordInvalid). Its scheme range now ends at SplicedCubic, and a round-trip test covers
  every named scheme.

The minors:
- **Moved-from batch.** It is empty (RefusedGroup), not a null dereference.
- **Below-intrinsic quote.** RefusedRange, not price-only.
- **Pre-steps.** The count and `GridBottomExtrapolated` are documented as a
  sigma >= 0.10 statement.
- **A fast reference delta pin.** E20p and C4c against D0 section 8.
- **The reference's u.** The Greek table now carries it.
- **The B1 pin.** It is labelled the lane's accuracy class.
- **The next experiment.** It is named once, with an acceptance table, in the HFT row.
- **The cubic splice's zero floor: measured, and not applied.**
  - Against D0's 4-level PDE ladder, 32 near-expiry European cases (ex-date 0.6 to 5 steps
    of 301 before expiry; K 90 to 130; calls and puts): a floor at 0 raised the European error
    in 30 of 32 cases.
  - It also broke put-call parity: the C and P errors diverged by up to 1.2e-3 USD.
  - The reason: the cubic is linear in the values, so the unfloored lattice keeps parity
    exactly.
  - American rollbacks are floored by the exercise max anyway.
  - A fast test pins European parity (1e-9) and the Refined European price (1e-3 USD;
    measured 1.5e-4).
  - Logs: `diag-scratch-eu-nofloor.log`, `diag-scratch-eu-floor.log`.
