# atx-vol served-surface no-arb integrity — design

Date: 2026-07-07
Status: approved-scope, pending spec review
Predecessor: `2026-07-05-arb-constrained-dense-surface-design.md` (reserved calendar
no-arb + full session integration as "later phases"). This spec is those phases,
plus wing extrapolation and a band-aware objective.

## 1. Problem

The default liquid-board volatility surface (`VolCurveKind::ConvexDense`, and the
`Svi` override) is served to the whole downstream stack — pricing, backtest,
attribution — as a `CurveSurface` (an ascending-T stack of `IVolCurve` slices).
That surface has **no calendar-arbitrage guarantee**:

- `src/session.cpp:277-281` hardcodes `cdiag.calendar_arb_free = false` and
  `cdiag.n_calendar_viol_pre = 0` on the curve-override path, with the honest
  comment "Calendar no-arb across slices is not (yet) checked for the dense/SVI
  override." Every SPY production surface is served with calendar arb **unverified
  and unenforced**.
- The only calendar validator, `arb_check_calendar` (`include/atx/vol/arb.hpp:113`),
  accepts a `VolSurface` (eSSVI parametrization) — not the `CurveSurface` the dense
  path produces. So the dense surface was never *checkable*, hence the useless
  hardcoded `false`.

Butterfly (per-slice risk-neutral density positivity) *is* arb-free by construction:
the dense QP fits call prices under hard convexity/monotone/positivity inequalities
(`src/dense_slice.cpp:305-327`). Calendar is the missing half of static no-arb.

Two adjacent integrity gaps on the same surface:

- **Wings.** `ConvexSliceFit::call_price` flat-clamps the call price outside the
  node strike range (`src/dense_slice.cpp:142-151`). A flat call-price tail means
  total variance flattens rather than growing linearly in |k|, so far-wing IV
  degenerates and there is **no wing no-arb control** beyond the quoted strikes.
- **Objective.** The dense QP minimizes squared distance to the **mid** call price
  (`src/dense_slice.cpp:302`). A bid/ask-band-aware ("interval") loss exists in the
  vocabulary (`CalibLossKind::Interval`, `include/atx/vol/calib.hpp`) but is not
  threaded into the dense objective; fitting the mid instead of the tradeable band
  manufactures spurious edge.

## 2. Goals / non-goals

**Goals.**
1. The served ConvexDense/SVI `CurveSurface` is **statically arbitrage-free**:
   butterfly (already) **and** calendar, verified and enforced.
2. `session.cpp` reports a *true* `calendar_arb_free` / `n_calendar_viol_pre`.
3. Dense-curve wings carry an asymptotic-linear-total-variance (Lee) tail with a
   no-arb slope cap — finite, controlled far-wing IV.
4. The dense fit can target the bid/ask **band** (interval loss), not only the mid.
5. **Zero fit-quality regression where no arbitrage exists.** Enforcement is slack
   on clean boards → bit-identical served surface; it only acts where the
   independent fit would have crossed.

**Non-goals (explicitly deferred).**
- Joint/global term-structure calibration (shared ρ/θ(T)) — this stays a
  sequential per-slice fit with a calendar coupling, not a global objective.
- Bringing C8/CStar into the served family.
- eSSVI path changes — it already carries calendar repair (`run_surface_parity`);
  untouched.

## 3. Background: the representations

- A **slice** is `IVolCurve` with `w(k_log)` (total variance σ²T at
  log-moneyness k=ln(K/F_slice)) and `iv(k_log)`. `ConvexDenseCurve` owns a
  `ConvexSliceFit` (node strikes `u`, fitted European call prices `C`, forward `F`,
  discount `df`, year-fraction `T`).
- A **surface** is `CurveSurface`: ascending-T `slices_`, with a single
  linear-in-total-variance time interpolation (`src/vol_curve.cpp`).
- **Calendar no-arb** (standard): at each fixed log-moneyness k, total variance is
  non-decreasing in maturity — `w_i(k) ≥ w_{i-1}(k)` for `T_i > T_{i-1}`.
- **Butterfly ⇔ call-price convexity** in K, already a hard QP constraint.

## 4. Design

Four pieces. ①②③ are the no-arb core; ④ is the objective upgrade.

### ① Calendar checker on `CurveSurface`

New read-only validator (mirrors `arb_check_calendar` for `VolSurface`):

```cpp
// arb.hpp (or curve_fit.hpp — same namespace)
[[nodiscard]] Result<std::vector<ArbViolation>>
arb_check_calendar(const CurveSurface& s, double k_min, double k_max,
                   std::uint32_t n_grid);
```

Sample `n_grid` equispaced k in `[k_min,k_max]`; for each consecutive slice pair
`(i-1, i)` record a `Calendar` `ArbViolation` wherever
`w_{i-1}(k) - w_i(k) > tol` (a small absolute total-variance tolerance absorbing FP
/ interpolation noise). Empty vector ⇒ calendar-arb-free. No-op for `< 2` slices.

Wire it into `session.cpp` on the override path: replace the hardcoded
`calendar_arb_free=false` / `n_calendar_viol_pre=0` with the measured result over
the fitted `CurveSurface` on the board's own k-range. This alone restores honest
reporting — independent of whether enforcement (②) is on.

### ② Calendar enforcement by construction

**②a — Augment the QP to `Gx ≥ h`.** `qp_active_set` (`src/dense_slice.cpp:40`) is
homogeneous (`Gx ≥ 0`); the feasibility/ratio test at `:106-120` assumes RHS 0.
Add a `const VecX& h` parameter (default zero) and change exactly three things:
- feasibility residual: `gix = G.row(i).dot(x) - h(i)`;
- ratio test: `ai = -(G.row(i).dot(x) - h(i)) / gip`;
- the strictly-feasible start must satisfy `Gx0 > h` (see ②c).

The KKT step is unchanged: the working-set equality keeps `G_W p = 0` (stay on
active constraints where `G_W x = h_W`), so the bottom RHS block stays 0. `h = 0`
recovers today's solver **bit-for-bit**. This same augmentation also unlocks the
deferred `bound_slope_below` non-homogeneous bound (`∂C/∂K ≥ −df`,
`dense_slice.cpp:328-330`) — implement that row too while the form is open (low
cost, closes a documented TODO).

**②b — Per-node calendar floor in `fit_convex_slice`.** Extend `ConvexFitOpts`
with an optional calendar floor supplied by the driver:

```cpp
struct ConvexFitOpts {
  ...
  // Optional per-node lower bound on the fitted call price (calendar no-arb).
  // Empty => unconstrained (today's behavior). When present, length == node count
  // after gridding; entry j is C_floor at node strike u(j).
  std::span<const double> calendar_floor{};   // or pass via a new overload
};
```

For node strike `K_j` on this slice `(F_i, df_i, T_i)`, the floor is the Black-76
call price at the **previous** slice's total variance evaluated at the same
log-moneyness:

```
k_j       = ln(K_j / F_i)
w_prev    = prev_curve.w(k_j)                       // previous slice's total variance at k_j
sigma_j   = sqrt(w_prev / T_i)
C_floor_j = black76_call(F_i, K_j, sigma_j, T_i, df_i)
```

Because Black-76 call price is strictly increasing in total variance (vega > 0),
`C_i(K_j) ≥ C_floor_j  ⇔  w_i(k_j) ≥ w_prev(k_j)` — the calendar constraint at the
nodes, exactly. Add rows `g_j ≥ C_floor_j` (diagonal, one per node) to `G/h`.

The floor is enforced **at the node strikes**; between nodes both `C_i` and the
floor curve are convex, and the fine-grid checker (①) catches any residual
between-node crossing. The node grid is dense near the money (where crossings are
most likely); if the checker still reports a residual violation the driver may
refine (raise `node_cap`) — expected to be rare.

**②c — Sequential driver.** In `fit_curve_surface` (`src/curve_fit.cpp`), fit
slices in ascending-T order (already the iteration order). Maintain the running
`CurveSurface`; before fitting slice `i > 0`, compute `C_floor` from
`slice_{i-1}.w(·)` and pass it to `fit_slice_curve` → `fit_convex_slice`. The
strictly-feasible start (②a) is initialized from the floor itself plus a strict
positive margin: `C_floor` is a convex, non-increasing, non-negative call curve
(it is a valid arb-free smile repriced at `T_i`), so it already satisfies the
homogeneous cone; the margin strictifies the calendar bound. Fall back to a
Phase-1 feasibility solve only if FP noise makes the naive start infeasible.

Where the independent fit already satisfies the floor (no calendar arb — the common
case on a well-behaved board), every calendar row is slack at the optimum and the
QP returns the **same** node prices as today: bit-identical. Where it would cross,
the fit is lifted to the minimal calendar-arb-free boundary.

Applies to `ConvexDense` and `Svi` override kinds; eSSVI keeps `run_surface_parity`.
Cost: term-structure fit is now sequential across the board's ~10–20 expiries (was
independent). Per-board strike work is unchanged; cross-board / cross-date
parallelism in the corpus build is untouched.

### ③ Lee wing extrapolation

Replace `ConvexSliceFit::call_price`'s flat clamp (`dense_slice.cpp:146-151`) with
a **linear-in-log-moneyness total-variance** tail beyond the outer nodes:

- At each end compute the edge slope of total variance in k from the two outermost
  nodes: `p = dw/dk` at `k_edge`.
- Extrapolate `w(k) = w(k_edge) + p_clamped · (k - k_edge)` for k beyond the edge,
  with `p_clamped ∈ [0, p_cap]` (right wing) / `[−p_cap, 0]` (left wing).
- `p_cap` is the Roper/Lee large-strike butterfly bound for a linear total-variance
  tail (the SVI wing condition analogue). The exact constant is fixed against
  `arb_check_butterfly` sampled into the tail — the acceptance test is that the
  extrapolated region carries **zero** butterfly violations.

Implemented at query time on the fit (map extrapolated `w` back through the
Black-76 relation for `call_price`, or override `iv`/`w` directly). No refit, no
change to node prices. The surface `w(k,T)` interpolation and the parity scoring
region (quoted strikes) are unchanged; only queries outside the quoted range move.

### ④ Band-aware (interval) objective

Give the dense QP an exact interval loss via slack variables — still a convex QP:

For each observation node with band `[c_bid_j, c_ask_j]`, introduce non-negative
slacks `s⁺_j, s⁻_j` and constraints
```
C_j - c_ask_j ≤ s⁺_j ,   c_bid_j - C_j ≤ s⁻_j ,   s⁺_j, s⁻_j ≥ 0
```
minimizing `Σ_j w_j (s⁺_j² + s⁻_j²)` (plus the same third-difference roughness).
Residual is exactly zero inside the band, quadratic outside — the true interval
loss. This enlarges the QP by ≤ 2·(#obs) variables; acceptable given `node_cap`-
bounded problem sizes.

Gate behind `CurveConfig` / `ConvexFitOpts` (`loss = Mid | Interval`), **default
`Mid`** so production fits stay bit-identical until we flip it deliberately. Then
measure `mean_frac_within_bidask` (the OOS in-band metric that already drives the
selector) with `Interval` vs `Mid` on the SPY corpus; flip the default to
`Interval` iff it improves in-band without degrading OOS. The flip decision and its
numbers are recorded in the follow-up commit message.

## 5. Test plan (the gate)

New / extended tests (self-contained where possible; real-data gated behind the
existing `spy_real`/corpus fixtures):

1. **Calendar checker unit test** — construct a synthetic 2–3 slice `CurveSurface`
   with a hand-planted calendar crossing; assert the checker locates it, and that a
   monotone stack reports zero.
2. **Augmented-QP equivalence** — `qp_active_set` with `h = 0` returns bit-identical
   results to the pre-change solver on the existing dense-fit fixtures (regression
   guard on the surgical change).
3. **Calendar-arb-free-by-construction** — fit real SPY corpus boards through
   `fit_curve_surface` with enforcement on; assert `arb_check_calendar(CurveSurface)`
   returns **0** violations (baseline: measure the pre-enforcement count with ① and
   record it — this quantifies the hole).
4. **Butterfly still clean** — existing per-slice butterfly gate stays at 0
   violations post-enforcement.
5. **No-regression on clean boards** — on boards with no pre-existing calendar
   violation, enforced node prices equal unenforced (slack ⇒ identical), and
   `mean_frac_within_bidask` does not degrade beyond a tight tolerance corpus-wide.
6. **Wing no-arb** — `arb_check_butterfly` sampled into the extrapolated tail
   returns 0; `w(k)` is monotone non-decreasing in |k| in the tail; IV is finite far
   past the wings.
7. **Interval objective** — a fit with `loss=Interval` puts the fitted price inside
   `[bid,ask]` for a synthetic in-band-feasible board (residual 0), and reduces to
   `Mid` behavior on a degenerate zero-width band.
8. **Full suite green** — all 676 existing atx-vol tests still pass.
9. **End-to-end** — rebuild the SPY YTD corpus and re-run `spy_strangle_backtest` on
   the refreshed manifest; report the new `calendar_arb_free=true`, the corpus build
   time (must not materially regress), and a backtest sanity number.

## 6. Risks & mitigations

- **Touching the QP core.** Additive augmentation (`h=0` ≡ today, guarded by test 2);
  enforcement provably slack on non-violating boards (test 5); existing arb-free +
  in-band gates catch regressions.
- **Between-node calendar residual.** Enforced at nodes; fine-grid checker (①) is the
  backstop; node grid is money-clustered where crossings live; refine `node_cap` on
  the rare residual.
- **Sequential fit latency.** ~10–20 expiries/board, per-board; corpus parallelism
  across dates preserved; measured in test 9.
- **Wing cap constant.** Pinned empirically against `arb_check_butterfly` (test 6),
  not guessed.
- **Interval default flip.** Kept behind a flag, default `Mid`; flip only on measured
  in-band improvement.

## 7. Rollout / sequencing

1. ① checker + `session.cpp` honest reporting + baseline measurement of the hole.
2. ②a augmented QP (+ `h=0` equivalence test) and the `bound_slope_below` row.
3. ②b/②c calendar floor + sequential driver + by-construction gate.
4. ③ Lee wings + wing gate.
5. ④ interval objective (flag, default Mid) + in-band measurement.
6. End-to-end corpus rebuild + backtest sanity; commit per piece with the numbers.
