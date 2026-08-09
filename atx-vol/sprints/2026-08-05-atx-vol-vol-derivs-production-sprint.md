# atx-vol Vol-Derivatives Production Sprint (fitting → PV → greeks)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Fresh
> implementer per task, task review after each, per-phase aggregate reviews and a final
> whole-branch review are tasks of this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take the vol-derivatives stack (var/vol/capped swaps + the surface fitting that feeds
it) from "v1.0.0 at-pillar" to production-complete: close every verified correctness defect,
collapse the greeks-path cost by an order of magnitude, and ship the product/risk features a
dispersion desk actually runs (gamma/corridor/forward variance, variance options, skew vega,
surface dynamics, deriv P&L explain).

**Architecture:** The pricing layer is a pure function of a fitted surface (`iv(k_log,T)`) +
curve set: DDKZ log-strike Simpson strip (`derivatives.cpp`), Carr-Lee vol strike, lognormal RV
distribution model for capped/mid-life kinds, FD bump greeks with center-scheme pinning. The
fitting layer (eSSVI/ConvexDense per-slice + calendar projection + independent admission oracle)
serves `PricedSurface`. This sprint changes both ends and the contract between them (certified
band, wing policy, overlay views).

**Tech stack:** C++20, GTest (`-L atx_vol`), AVX2 SIMD kernels (`src/simd/`), pybind11 (bindings
untouched except where noted).

**Provenance:** Findings from a 4-track deep review, 2026-08-05: PV path (PV-*), greeks path
(GK-*), fitting chain (FIT-*), literature (LIT-*). Findings inventory + references at the bottom
of this file. Every task cites its findings.

## Global constraints (binding, every task)

1. Priority order **1. Correctness 2. Performance 3. Features** — binding for scheduling and for
   cut decisions if the sprint must shrink.
2. v1.x API freeze: Tier-A headers are frozen. All public changes here are ADDITIVE (new enum
   values, appended struct fields, new entry points). Every struct-field append updates its arity
   pin test. Target version: **1.1.0**.
3. The batch delta solver's half-tolerance (0.5×tolerance internal accept vs full-tolerance
   oracle gate) is load-bearing — never relax it (Task-9 ruling, 2026-08-05).
4. Numbers a caller already depends on: any change that moves a mark (C-1, C-2, C-3, C-5, F-1)
   documents the move in `CHANGELOG.md` with magnitude and migration note. Defaults stay
   behavior-compatible unless the old default is a verified bug (C-1 is; the flag flip is the
   fix).
5. Bench evidence: paired/interleaved A/B, ≥10 pairs, per-pair deltas + win-counts + medians —
   sequential before/after on the dev box is NOT valid (measurement policy,
   `2026-08-02-atx-vol-v1-production-closeout-sprint.md`). Quiet host for citable numbers.
6. Environment cautions from the closeout sprint apply verbatim (no PowerShell
   Get-/Set-Content on sources, Read/Edit tools only; `rtk proxy git ...` for raw git; explicit-
   path staging; no clang-format; 100-col).
7. Tests first where a defect is being fixed: write the failing oracle test, watch it fail,
   then fix. A test that pins the current wrong value (e.g. `AgedDispatch.DiscreteCorrection_
   OneOverN`) is replaced, not appended to.

---

## Review verdict (context for implementers)

**Core math verified sound, symbol-by-symbol:** DDKZ change of variables and df placement
(derivatives.cpp:914-941), forward split point, composite Simpson weights + 4m+1 tier defaults +
Richardson half-grid, Carr-Lee constant (call convention absorbs the straddle factor 2),
lognormal engine identities (E[√W]=√m·e^{−s²/8}; auto-calibration s²=−8·ln(K_vol/√K_var)),
capped closed forms and split-domain quadrature, aged blend at residual maturity, sticky-strike
bump sign (exact composition on the PricedSurface path), vanna/charm stencils, grid/ξ pinning
(measured noise floor ~2e-7), eSSVI φ ceiling = both Gatheral-Jacquier butterfly bounds exactly.

**What is broken or missing** is the subject of this sprint: one wrong formula behind a
non-default flag (C-1), an unresolved short-tenor regime (C-2), a quadrature-validity invariant
that holds today only by accident of defaults (C-3), a fail-silent engine selector and NaN
handling (C-4), a documented-but-biased Carr-Lee approximation feeding ξ (C-5), a certified-band
lie on Latency surfaces (C-6), two QP availability hazards (C-7), a fitting driver whose header
promises a projection it doesn't run (C-8), a theta that omits the largest deterministic daily
P&L term (C-10), a greeks path doing ~14 strip repricings where ~2 read passes suffice (P-1..P-4),
and the absent product/risk surface a dispersion desk needs (F-1..F-8).

---

## Phase 0 — Baseline + shared fixtures

### Task 0: Baseline matrix + skewed-surface reference fixture

**Files:**
- Create: `tests/deriv_fixtures.hpp` (header-only fixture builders)
- Modify: none

**Why:** Nearly every task below needs (a) a green baseline, (b) a REALISTIC skewed surface with
known analytic structure — today's deriv tests are overwhelmingly flat-surface (PV review, "Test
Coverage Assessment"), which is exactly why C-1/C-2 survived to v1.

**Steps:**
- [ ] Full matrix, dev preset (`-L atx_vol`): record registered/executed/failed/skipped. Baseline
      to beat: 0 failures. Any pre-existing failure: stop, report, do not proceed on red.
- [ ] Build `tests/deriv_fixtures.hpp`:
  - `make_flat_surface(sigma)` — existing pattern, centralized.
  - `make_skew_surface(atm_vol, skew_slope, convexity)` — eSSVI-parameterized slice set with
    ρ≈−0.7-equivalent skew at pillars {1M, 3M, 6M, 1Y}; parameters chosen inside the
    Gatheral-Jacquier butterfly bounds (θφ(1+|ρ|)<4, θφ²(1+|ρ|)≤4) and calendar-ordered.
  - `make_curves(spot, r, q)` — CurveSet with nontrivial r−q (needed by C-1's oracle).
  - A seeded MC harness `mc_realized_variance(model_params, n_paths, n_steps, seed)` producing
    discrete-monitoring realized-variance samples under Black-Scholes (exact normal increments,
    no discretization error) — the independent oracle for C-1 and F-5. Seed pinned; determinism
    asserted.
- [ ] Pin fixture values: one test asserting `make_skew_surface(0.20, -0.10, 0.03).iv(0, 0.25)`
      etc. to 1e-12 so downstream oracle drift is impossible.
- [ ] Commit.

**Acceptance:** matrix green; fixture pin test green; MC harness reproduces flat-BS
E[RV] = σ² + (T/n)(r−q−σ²/2)² within 3 MC standard errors at n_paths=2e5.

---

## Phase 1 — Correctness (binding order; nothing later starts until C-1..C-4 land)

### Task C-1: Rebuild the discrete-monitoring correction (verified wrong formula)

**Findings:** PV-1 (P1), PV-8 (P3), LIT-3.

**Files:**
- Modify: `src/derivatives.cpp:216-219, 376-379, 610-613, 740-743` (the four
  `Diffusion1OverN` application sites), `include/atx/vol/derivatives.hpp` (doc block for
  `DerivDiscreteCorrection`)
- Test: `tests/derivatives_test.cpp` (replace `AgedDispatch.DiscreteCorrection_OneOverN`)

**The defect, verified:** the code multiplies K_var by `(1 + 1/n_total)`. The Broadie-Jain (2008)
/ Bühler diffusion correction is ADDITIVE: with per-step drift μ = r − q − σ²/2,
`E[r_i²] = σ²Δ + μ²Δ²` ⇒ discrete fair variance = σ² + (T/n)·μ². The code's multiplicative form
adds σ²/n ≈ 1.6 var pts at (n=252, σ=20%) where the true term is ~0.016 var pts — wrong
functional form, ~100× overstated, and it divides by `n_total` mid-life where the correction
applies to the future leg's `n_remaining` fixings. The existing test pins the wrong factor.

**Spec:**
- Correction applied to the FUTURE leg only:
  `K_var_future += (T_resid / n_remaining) * (r_bar - q_bar - 0.5 * K_var_future)^2`
  where `r_bar`, `q_bar` are the continuously-compounded zero rate and carry at the residual
  tenor read from the same CurveSet the strip already resolves F and df from
  (`r_bar - q_bar = ln(F/S)/T_resid` — F and S are already in hand; no new plumbing), and
  `n_remaining = n_total - n_done` (unaged: n_total).
- ξ auto-calibration order (PV-8): resolve ξ against the UNCORRECTED strip mean, then apply the
  correction to the mean fed to the distribution model — "reproduces Carr-Lee exactly" must
  survive the mode. One ordering, all four sites.
- Doc block: name it what it is (Broadie-Jain diffusion drift term), state the residual O(1/n)
  jump term is NOT covered (LIT-3: jumps need FullMc — reserved), state magnitude ~fraction of a
  var pt daily-monitored.

**Steps:**
- [ ] Write the failing oracle test first:
      `DiscreteCorrection.MatchesBSExactDiscreteFairStrike` — flat surface σ=0.20,
      r=0.06, q=0.01 (μ = 0.03, correction = (1/252)·9e-4 = 3.571e-6 = 0.0357 var pts at T=1,
      n=252). Oracle #1 (analytic, exact): σ² + (T/n)μ². Oracle #2 (independent): Task-0 MC
      harness, 3-SE band. Assert both. Run: fails against the multiplicative code.
- [ ] Write `DiscreteCorrection.MidLifeUsesRemainingFixings` — aged contract n_done=126,
      n_total=252: correction divisor must be 126, not 252. Fails today.
- [ ] Write `DiscreteCorrection.XiCalibratedPreCorrection` — with the mode on, assert
      `vol_of_vol_used` equals the value resolved with the mode off (1e-14). Fails today.
- [ ] Implement per spec at all four sites (one shared helper in the anon namespace:
      `double discrete_monitoring_addend(double kvar_fut, double T_resid, uint32_t n_remaining,
      double r_minus_q)`).
- [ ] Delete the old pin test. Run the three new tests + full derivatives group. Green.
- [ ] CHANGELOG entry: `Diffusion1OverN` semantics corrected; anyone who enabled it was marking
      ~1.6 var pts rich at index parameters; new numbers are smaller by ~two orders.
- [ ] Commit.

**Acceptance:** three new tests green; no other test moves (mode is non-default); CHANGELOG
entry present.

### Task C-2: Short-tenor grid under-resolution + dead `LowT` flag

**Findings:** PV-2 (P1, verified numerically: Fast tier, T=1/252, σ=0.20 → K_var +6.06%),
PV-3 (P2: `DerivFlags::LowT` declared, zero writers).

**Files:**
- Modify: `include/atx/vol/detail/strip_grid.hpp` (node-count resolution), `src/derivatives.cpp`
  (:839-887 span/node resolution; LowT raise site)
- Test: `tests/derivatives_test.cpp`

**Spec:**
- The adaptive logic today only WIDENS the span (`adaptive_half_width` is a max) and rescales
  node count with span. Add the mirror rule: after span resolution, enforce
  `dk <= sigma_atm * sqrt(T) / 4` by raising the node count (preserving the 4m+1 invariant:
  round UP to the next 4m+1). Caller-pinned `strip_nodes` is never overridden (pin semantics are
  load-bearing for greeks) — a pinned grid that violates the floor raises `LowT` instead.
- Raise `DerivFlags::LowT` whenever (a) the floor had to engage, or (b) a pinned grid leaves
  `dk > sigma_atm*sqrt(T)/4`. Doc the flag's real meaning in the header.

**Steps:**
- [ ] Failing test `StripResolution.OneDayTenorAccurateAtAllTiers`: flat σ=0.20, T=1/252, truth
      K_var = 0.04 exactly; assert relative error < 1e-3 at Fast/Standard/High/Audit. Fast fails
      today at +6.06e-2.
- [ ] Failing test `StripResolution.LowTFlagFires`: pinned 97-node grid at T=1/252 carries
      `LowT`; default (post-fix) Fast grid at T=1/252 also carries `LowT` iff the floor engaged;
      T=0.25 Standard carries no `LowT`.
- [ ] Implement; verify greeks grid-pinning interaction: `deriv_greeks` pins the CENTER's
      resolved grid — with the floor, the center resolves the denser grid and the pin propagates
      it; assert `HighVolRegimeGridPinKeepsSecondOrderSane` still green.
- [ ] Richardson still populated (node counts stay 4m+1). Assert on the 1-day quote.
- [ ] CHANGELOG: Fast-tier short-tenor marks move (down ~6% at 1DTE/20-vol); Standard moves
      ≤4bp. Commit.

**Acceptance:** both tests green; full deriv group green; documented mark move.

### Task C-3: Simpson kink discipline — make the k=0 invariant structural, split at clamp edges

**Findings:** LIT-10 (the sharpest literature item), PV-6 interaction. Today the OTM integrand's
C¹ kink at k=0 (slope jumps by df·F by put-call parity) lands on a PANEL BOUNDARY only because
every default grid is symmetric with 4m+1 nodes — an accident of defaults, unasserted. Any
asymmetric caller pin (`k_min_log = -0.31, k_max_log = 0.29`) silently degrades composite
Simpson from O(h⁴) to O(h²) AND invalidates the Richardson /15 estimate (which then typically
UNDERSTATES true error). The wing clamp adds two more C¹ kinks at ±wing_clamp_k that sit
mid-panel today whenever the clamp binds.

**Files:**
- Modify: `src/derivatives.cpp` (:893-952 quadrature loop), `include/atx/vol/detail/strip_grid.hpp`
- Test: `tests/derivatives_test.cpp`

**Spec:**
- Split the composite integration at every interior kink: integrate puts on [k_lo, 0] and calls
  on [0, k_hi] as separate composite Simpsons; when the clamp binds inside the span, split
  additionally at ±wing_clamp_k. Each sub-interval gets its own (odd) node count from the global
  budget, proportional to length, preserving total ≈ resolved budget. k=0 and the band edges are
  now panel boundaries BY CONSTRUCTION for any grid, symmetric or not.
- Richardson: estimate per sub-interval (each smooth), sum the estimates. Populate
  `integration_error_est` whenever every sub-interval count is 4m+1 (arrange the split to keep
  this true for default budgets); NaN otherwise, as today.
- `strip_k_lo_used/strip_k_hi_used/strip_nodes_used` semantics unchanged (total span + total
  node count) so greeks pinning and archived quotes keep meaning.

**Steps:**
- [ ] Failing test `StripQuadrature.AsymmetricPinMatchesSymmetricReference`: flat σ=0.20,
      T=0.25; symmetric default quote as reference; pinned asymmetric grid (k_min=-0.31,
      k_max=+0.29, 101 nodes). Post-split both agree with truth 0.04 to <1e-9; pre-split the
      asymmetric one misses by O(h²) (measure and record the pre-fix delta in the test comment).
- [ ] Failing test `StripQuadrature.RichardsonBoundsTrueErrorOnSkew`: skewed fixture, Audit
      reference as truth proxy; assert `integration_error_est` at Standard bounds
      |K_std − K_audit| within 10× both directions — the estimate must be an estimate, not noise.
- [ ] Test `StripQuadrature.ClampEdgeSplit`: skewed fixture with binding clamp; assert value
      change from splitting < 1e-6 rel on default grids (regression guard) and error estimate
      finite.
- [ ] Implement split loop; keep the single-pass structure (one iv() read per node, weights per
      sub-interval).
- [ ] Full deriv group + greeks group (pinning interplay). CHANGELOG: default-grid values move
      < 1e-6 rel; asymmetric-pin values corrected. Commit.

**Acceptance:** three tests green; default-grid regression < 1e-6 rel everywhere in the matrix.

### Task C-4: Fail-loud dispatch — engine×kind matrix + interior bad-node accounting

**Findings:** PV-5 (P2: `VarSwap` + `engine=VolCarrLee` silently prices the strip; `VolSwap` +
`StripLogContract` silently prices Carr-Lee), PV-4 (P2: interior non-finite/≤0 iv contributes 0
silently; only boundary nodes flag).

**Files:**
- Modify: `src/derivatives.cpp` (:1079-1093 dispatch; :917-931 node loop),
  `include/atx/vol/derivatives.hpp` (DerivFlags addition)
- Test: `tests/derivatives_test.cpp`

**Spec:**
- Validate the full kind×engine matrix at dispatch: VarSwap accepts {Auto, StripLogContract};
  VolSwap accepts {Auto, VolCarrLee (unaged only, as today), RvDistributionProxy}; capped kinds
  accept {Auto, RvDistributionProxy}. Everything else: InvalidArgument. (This tightens two
  silent paths into errors — CHANGELOG note; no default-config behavior changes.)
- Interior bad nodes: count them; new appended flag `DerivFlags::InteriorBadNodes = 1u << 13`;
  if count > max(2, nodes/100) return Internal (a surface with a mid-grid hole is not priceable,
  it is broken). Boundary behavior unchanged.

**Steps:**
- [ ] Failing tests: `Dispatch.EngineKindMatrixEnforced` (both silent combos now
      InvalidArgument); `Strip.InteriorNaNFlagged` (surface adapter fixture with one NaN node →
      flag; with 5% NaN nodes → Internal).
- [ ] Implement. Full group. CHANGELOG (two error-contract tightenings). Commit.

**Acceptance:** tests green; arity/flag doc updated; no default-path value moves.

### Task C-5: Carr-Lee convexity refinement (close the ξ bias chain)

**Findings:** LIT-4 (P1-class model bias): the implemented `K_vol ≈ √(2π/T)·C_ATMF/(F·df)` is the
naive approximation Carr-Lee explicitly decline to endorse — under equity skew (ρ≈−0.7) it is
biased LOW by >40 vol bp at 6M (Heston BCC calibration, their §6.5). Because ξ auto-calibration
inverts `E[√W] = K_vol^{CL}`, understated K_vol ⇒ understated ξ ⇒ underpriced cap options and
mid-life convexity — the bias propagates through every distribution-model product.

**Files:**
- Modify: `src/derivatives.cpp` (:288-289 Carr-Lee site; :332-344 ξ resolver),
  `include/atx/vol/derivatives.hpp` (`DerivConfig` appended knob)
- Test: `tests/derivatives_test.cpp`, `tests/deriv_distribution_test.cpp`

**Spec:**
- Appended config knob `enum class CarrLeeForm : uint8_t { Naive = 0, Refined = 1 }` as
  `DerivConfig::carr_lee_form` (default Naive for 1.1 behavior-compatibility; flip to Refined at
  2.0 — CHANGELOG states the plan).
- Refined form: the Carr-Lee Remark 6.4/6.5 refinement using the strip's own K_var — both
  numbers already exist in the quote path:
  `K_vol_refined = IV_0 * (1 + (VAR_0 - IV_0^2) / (8 + 2*IV_0^2))`-family per rrvd §6
  (IV_0 = naive K_vol, VAR_0 = strip K_var). **Implementer: re-derive the exact refinement
  expression from the paper (https://math.uchicago.edu/~rl/rrvd.pdf, Remarks 6.3-6.5) before
  coding — the review transcribed it at summary fidelity; the paper is the spec.** The
  refinement must satisfy the two exact properties the tests below pin.
- ξ resolver honors the configured form (refined K_vol ⇒ larger ξ ⇒ richer caps).

**Steps:**
- [ ] Failing tests pinning the two exact properties:
      `CarrLee.RefinementVanishesOnFlat` — flat surface: K_var = K_vol² exactly ⇒ refined ==
      naive to 1e-12.
      `CarrLee.RefinementOrderedUnderSkew` — skewed fixture: naive < refined < √K_var (the
      refinement recovers part of the convexity gap, never overshoots the Jensen bound).
- [ ] `Distribution.XiRespondsToForm` — refined form yields strictly larger `vol_of_vol_used`
      and strictly larger capped-var-swap cap_option_value on the skewed fixture.
- [ ] Implement; document the bias magnitude (LIT-4 numbers) in the header block.
- [ ] Commit.

**Acceptance:** three tests green; default path unmoved; knob documented with the 2.0 default
flip plan.

### Task C-6: Certified-band provenance → wing clamp (stop trusting a band nobody certified)

**Findings:** FIT-C7 (P2): strip trusts |k| ≤ 0.5 via a static_assert against the DEFAULT
`RiskSurfaceValidationConfig{}.k_max` only; Latency-mode fits certify ±0.35
(pricer_fitter.cpp:1594-1598) — a Latency surface priced with default DerivConfig reads
[0.35, 0.5] uncertified, precisely what the clamp exists to prevent.

**Files:**
- Modify: `include/atx/vol/fit_policy.hpp` (persist certified band beside
  `invariant_grid_k_min/k_max`, :185-186 — fields exist, wire them), `src/surface_policy.cpp`,
  `src/derivatives.cpp` (`resolve_wing_clamp` consumes surface-carried band on the
  PricedSurface/SurfaceRef paths), `include/atx/vol/detail/strip_grid.hpp` (doc)
- Test: `tests/derivatives_test.cpp` or `tests/surface_policy_test.cpp` (wherever Latency-mode
  fixtures already exist — locate with `git grep -n "Latency" tests/`)

**Spec:** `wing_clamp_k == 0` (the default) resolves to the surface's OWN certified half-band
when the surface carries one, else `kCertifiedWingHalfBand`. Explicit >0 and <0 semantics
unchanged. Templated legacy-surface overloads (no provenance) keep the constant.

**Steps:**
- [ ] Failing test: Latency-certified fixture → default-config quote's effective clamp = 0.35
      (observable via `WingClamped` firing at a span that only exceeds 0.35, plus a new
      diagnostic assert path — cheapest: expose `resolved_wing_clamp` on the quote as an
      appended field with arity-pin update).
- [ ] Wire provenance; implement resolution; Balanced-mode surfaces resolve 0.5 exactly as
      today (regression assert).
- [ ] CHANGELOG (Latency-surface marks move; that is the fix). Commit.

**Acceptance:** test green; Balanced-path values bit-identical; arity pins updated.

### Task C-7: QP ratio-test clamp + anti-cycling (availability, fail-closed today)

**Findings:** FIT-C3 (P2): `ai = -gix/gip` with `gix` a few ulp negative (admitted within
`kQpStartTol=1e-12`) and `gip` just under the `-1e-14` cutoff yields large NEGATIVE α — steps
backward along the descent direction; KKT certificate fails closed → slice lost → board fails
under `fail_board_on_hard_slice_error`. FIT-C4 (P3): no anti-cycling tie-break; degenerate
vertices can burn `max_iter=200` → same board loss. Note: a Bland's-rule latch was deliberately
dropped from the 2026-08-05 QP fix commit (kept-minimal ruling); this task is its scoped
reintroduction with tests.

**Files:**
- Modify: `src/dense_slice.cpp` (:245-261 ratio test; working-set add/drop sites)
- Test: `tests/dense_slice_test.cpp` (locate exact name: `git grep -ln "fit_convex_slice" tests/`)

**Spec:**
- Ratio test: `const double ai = std::max(0.0, -gix / gip);` — α=0 correctly adds the blocking
  row and forces a working-set change; never step backward.
- Anti-cycling: lowest-index tie-break on blocking rows within 1e-14·scale of the minimal α, and
  lowest-index choice among equal most-negative multipliers on drop. Deterministic, no state.
- Iteration budget unchanged (200).

**Steps:**
- [ ] Failing/characterization tests: (a) unit-level ratio-test clamp — constructed 2-var board
      where the unclamped path steps backward (assert objective non-increase per iteration
      post-fix); (b) degenerate-vertex board (duplicate a calendar-floor row as an intrinsic
      bound row) converges < 200 iterations with identical certified solution.
- [ ] Implement both; run the existing QP divergence regression
      (`DenseSlice.*QP*`/degenerate-board certification suite) + full fitting group.
- [ ] Commit.

**Acceptance:** new tests green; existing QP suite green; no fitted-value drift on the standard
fixtures (bit-compare fit outputs on the SPY fixture test if one exists in the group).

### Task C-8: eSSVI alternate-driver no-arb honesty

**Findings:** FIT-C1 (P2): `essvi_calib.hpp:130-137/185` promises post-fit calendar projection;
implementation gates it behind `essvi_alt_driver_theta_project` default FALSE, and stamps
`out_diag->n_butterfly_viol = 0` unconditionally; `CalibOpts::validate_no_arb` is dead on this
path. FIT-C2 (P3): sequential driver's θ floor is ATM-only (wings can still cross). FIT-C5
(P2): HingeQuad wing residual served unprojected and butterfly-checked only on fixed
[-0.6, 0.6] while the residual reshapes out to quoted |k|≈1.2 (SVI branch got the FT-C2 fix;
Essvi branch didn't).

**Files:**
- Modify: `src/essvi_calib.cpp` (:1245 gate, :1267 diag stamp, :656-703 residual), 
  `include/atx/vol/essvi_calib.hpp` (contract), `src/vol_curve.cpp` (:517 Essvi-branch
  validation — use the quoted-range scan `validate_served_shape_over_quotes` as the SVI branch
  does at :547-550)
- Test: `tests/essvi_calib_test.cpp` (or the file `git grep -ln "essvi_calib_surface" tests/`
  names)

**Spec:**
- Honor `CalibOpts::validate_no_arb` on the eSSVI driver: when set, run `arb_check_calendar` +
  butterfly check post-fit into `FitDiag` (real counts, not stamped zeros) and bail per the knob's
  documented contract.
- Fix the header contract to describe the gate (or flip the default ON for the driver —
  implementer decides with the reviewer; either way header, code, and diag must agree).
- Essvi residual branch: validate over quoted range ± 0.5 whenever `resid_scale > 0` (mirror
  FT-C2). Roper projection port for HingeQuad stays OUT of scope (documented deferral, PORT NOTE
  updated) — the validation catches what the projection would have prevented, fail-closed.

**Steps:**
- [ ] Failing tests: (a) construct a 2-expiry board with a θ inversion — driver with
      `validate_no_arb=true` reports nonzero calendar violations / bails; (b) diag stamped-zero
      regression: butterfly count reflects an injected butterfly-violating slice; (c) HingeQuad
      residual fixture quoted to |k|=1.2 with a wing violation beyond 0.6 → caught.
- [ ] Implement; run fitting group. Commit.

**Acceptance:** header contract == code behavior; three tests green; canonical-path
(PricerFitter) outputs untouched (bit-compare on fixture).

### Task C-9: Small-bore correctness closeout (bundle)

**Findings:** PV-9, GK-C7, GK-C8, GK-C9b, FIT-C10, FIT-C8, FIT-C11, PV-7.

**Files:**
- Modify: `src/derivatives.cpp`, `src/deriv_book.cpp`, `include/atx/vol/calib.hpp`,
  `include/atx/vol/vol_curve.hpp` + `include/atx/vol/priced_surface.hpp` (doc), 
  `src/dense_slice.cpp:320-322`
- Test: alongside each

One reviewable commit per item; a task-reviewer gates the set:

- [ ] **Negative-T rho sign (PV-9):** fully-aged analytic branch clamps `T = max(T, 0.0)` before
      `rho = -T*PV` (derivatives.cpp:1323-1326). Test: expired lot (maturity_t = -0.01) → rho 0.
- [ ] **`vol_abs` bump validation (GK-C7):** in `bumps_valid`, reject (InvalidArgument) when
      `vol_abs >= sigma_atm(center)` read once at k=0 — the cheap sufficient guard against the
      silent zeroed-node corruption of v_dn. Test: vol_abs=0.15 on a 0.10-vol surface errors.
- [ ] **Front-pillar rolled carry pillar (GK-C8):** port `carry_from_ref`'s 2-pillar rolled-
      forward fix (derivatives.cpp:1683-1693) into `carry_from` (:1602-1610): when
      `T == ts.front()`, append a pillar at T−dt so the theta roll doesn't dive into
      clamped-forward territory. Test: mirror of `BridgeThetaMatchesPricedSurfaceReference` for
      the E6 path, front-pillar contract; header caveat (:716-723) deleted — the caveat is now
      fixed, not documented.
- [ ] **Book NaN-poisoning per column (GK-C9b):** `accumulate` (deriv_book.cpp:158-177) keeps
      per-column valid-lane counts; totals NaN only if a lane was NaN, but new appended fields
      `n_theta_excluded` etc. record how many lanes were excluded per column, so one near-expiry
      swap no longer silently blinds the desk theta with no count. Arity pin update. Test: book
      of 3, one theta-NaN lane → count 1, theta total NaN (semantics unchanged), count visible.
- [ ] **`noise_sigma` doc (FIT-C10):** calib.hpp:331 — "σ-equivalent of the FULL spread / vega"
      (comment was wrong by 2×; consumers are consistent; doc-only).
- [ ] **Tenor-extrapolation docs (FIT-C8):** vol_curve.hpp:339-342 + priced_surface.hpp:205-206
      rewritten to describe actual behavior (flat-w long end ⇒ zero forward variance; flat-vol
      short end), plus a warning naming the two containers that DO return NaN. Policy enum
      deferred to roadmap (out of scope here).
- [ ] **Flat right tail exponent floor (FIT-C11):** dense_slice.cpp:320-322 floor the wing
      exponent at 1e-6 so a zero fitted edge slope decays instead of flat-clamping. Test: pinned
      degenerate board's far-call price at 10·K_max is < its value at K_max.
- [ ] **Blend unit note (PV-7):** derivatives.hpp DerivContract doc: obs-count weights assume
      n_fut ≈ 252·T_resid; contract-staging mismatch is caller responsibility (doc-only).

**Acceptance:** each item's test green; reviewer confirms no scope creep.

### Task C-10: Carry theta — price the fixing roll, not just the calendar roll

**Findings:** GK-C2 (P1 convention hazard): reported theta rolls T holding `rv_spec` fixed —
omits the implied→realized rollover, the largest deterministic daily P&L term on any unexpired
swap (≈ −df·N·K_var/n_total per day on a fair-struck var swap; ~−635/day on the standard test
contract while reported theta ≈ 0). No output anywhere in the stack carries it.

**Files:**
- Modify: `include/atx/vol/derivatives.hpp` (`DerivGreeks` appended fields), `src/derivatives.cpp`
- Test: `tests/deriv_greeks_test.cpp`

**Spec:**
- Appended outputs (arity pin updated):
  - `double theta_carry = kQuietNaN;` — one-sided: reprice at (T − dt_1d, rv_spec WITH one
    additional fixing realized AT THE IMPLIED level: n_done+1, Σr² += K_var_future·(T_total/
    n_total) in raw sum units consistent with the tracker's annualization), minus center, per
    year. On a fair-struck swap this ≈ pure discounting drift (fair stays fair) — the diagnostic
    that separates convention from carry.
  - `double theta_zero_fixing = kQuietNaN;` — same with a zero return fixing (r_i = 0): the
    deterministic "nothing happens overnight" mark change, the number a desk calls carry.
  - Existing `theta` untouched (frozen semantics).
- Fully-aged: both = theta (analytic branch). `maturity_t <= dt`: both NaN like theta today.
- dt_1d = `DerivGreekBumps::time_years` (same knob).

**Steps:**
- [ ] Failing test `CarryTheta.FairSwapCarryIsDiscountingOnly`: flat surface, fair-struck unaged
      var swap → |theta_carry − r·PV| small (quantify: < 5% of df·N·K_var/n_total) while
      `theta_zero_fixing ≈ −df·N·K_var/n_total·252` per year within 5%.
- [ ] Failing test `CarryTheta.SumIdentity`: theta_zero_fixing − theta_carry ≈
      +df·N·K_var_future·252/n_total·w-scale (the implied fixing's worth) within tolerance —
      pins the two variants against each other AND the blend arithmetic.
- [ ] Implement (two extra repricings, only when second_order or a new
      `DerivGreekBumps::carry_theta = true` — default true; document cost).
- [ ] Commit.

**Acceptance:** tests green; arity pins updated; header documents which number to put in a
daily P&L predict (theta_zero_fixing).

### Task C-R: Phase-1 aggregate review

- [ ] Dispatch a fresh reviewer over the whole Phase-1 diff: verify each C-task against its
      finding, hunt regressions in flags/quote-field semantics (this phase appended 3+ fields
      and 2 flags), confirm CHANGELOG completeness, confirm every replaced test's oracle is
      independent (not self-referential). Findings → fix tasks before Phase 2.

---

## Phase 2 — Performance

Instrument first, then cut. Baseline instrument: `bench/analytics_bench.cpp` has the deriv bench
scaffolding (`git grep -n "var_swap" bench/`); add a greeks-path microbench there in P-1 before
touching the hot path. All numbers per the measurement policy.

### Task P-1: Strip-view resolve hoisting (carry/bracket once per strip, not per node)

**Findings:** PV-P1 (P1), FIT-P2, GK-P (rolled-T cache miss).

**Files:**
- Modify: `src/derivatives.cpp` (:1561-1577 `PricedSurfaceStripView`, :1723-1729
  `SurfaceRefStripView`), `src/priced_surface.cpp` (use `resolve_with_carry_and_bracket`,
  :594-613 — exists, unused here)
- Bench: `bench/analytics_bench.cpp` (add `BM_DerivGreeks_Standard_PricedSurface`)

**Spec:** the strip's T is constant: resolve forward interp + pillar bracket ONCE at view
construction (and once more for the rolled T−dt views), then per-node work is curve-eval at k
only — no `exp(x)` → K → `log(K/F)` round trip, no per-node `interp_forward`/`bracket`. Applies
to base and rolled tenors (GK's finding: the 3 rolled repricings currently take the linear
pillar scan per node).

**Steps:**
- [ ] Add the microbench; record paired baseline.
- [ ] Implement hoisted views. Bit-identity gate: quotes and all 8 greeks bit-identical on the
      full deriv test matrix (this is pure factoring — any numeric drift is a bug).
- [ ] Paired A/B: expect ≥2× on PricedSurface-path greeks. Record.
- [ ] Commit.

**Acceptance:** bit-identical outputs; measured ≥1.5× greeks-path improvement or a written
explanation of why the ceiling is lower.

### Task P-2: Kill wasted repricings (analytic rho, diagnostic-strip skip, swap_leg second_order)

**Findings:** GK-C3 (rho ≡ −T·PV exactly — df cancels in the strip integrand, Carr-Lee, and the
pinned-ξ model; the FD rate bump recomputes a full strip to recover an identity), GK-P (unaged
VolSwap greeks pay ~14 diagnostic strips whose output nobody reads), GK-P3 (swap_leg calls
greeks with second_order=true, consumes vega only).

**Files:**
- Modify: `src/derivatives.cpp` (greeks table :1213-1254; vol-swap diagnostic :489-496),
  `src/swap_leg.cpp` (:133-134, :259-261)
- Test: `tests/deriv_greeks_test.cpp`

**Spec:**
- rho: computed analytically as −T·PV_center (matches the fully-aged branch); the rate-bump
  evaluation is deleted from the table. Header documents WHY it is exact (df-invariance of every
  fair-strike path with forwards held fixed) and that forward-channel rate risk is deliberately
  not in rho (unchanged semantics, now honest AND free). −1 repricing.
- Diagnostic strip: internal flag on bumped evaluations (plumbed through the existing
  `pin_center_scheme` config path — e.g. a private bit the bump table sets) skips the
  best-effort convexity diagnostic inside `price_vol_swap`'s Carr-Lee branch. Center quote
  unchanged (diagnostics intact). −14 strips on unaged vol-swap greeks.
- swap_leg: `second_order=false` at both call sites (+ carry_theta=false when C-10's knob
  exists). −6 repricings per step.

**Steps:**
- [ ] Test first: `Rho.AnalyticMatchesFD` — pin the current FD rho against −T·PV on the skewed
      fixture across kinds/ages to 1e-6·|PV| BEFORE deleting the bump (proves the identity on
      real code, not on paper); then delete the bump and re-run.
- [ ] Bit-identity on all other greeks after the diagnostic-strip skip (they read only PV).
- [ ] swap_leg: existing swap_leg_test matrix green (vega unchanged, second-order fields unused
      there — assert the test file doesn't read vanna/charm first: `git grep -n "vanna\|charm"
      tests/swap_leg_test.cpp`).
- [ ] Paired A/B on the greeks bench + one backtest populate leg. Commit.

**Acceptance:** rho identity test green; measured repricing count Standard unaged VarSwap
greeks: 14 → 13 evaluations (the r+ bump deleted), unaged VolSwap strips halved; swap_leg step
cost down ≥25%.

### Task P-3: Batch the strip reads (SIMD + read-vector dedup across bumps)

**Findings:** GK-P2 (14 read passes where 6 distinct read vectors exist — grid pinned, so spot
bumps shift k by a constant and vol bumps add a constant), PV-P4 / FIT positives (AVX2 kernels
`essvi_backbone_w_batch`, `black76_batch`, `black76_greeks_batch_soa` exist and the strip loop
is scalar).

**Files:**
- Modify: `src/derivatives.cpp` (strip loop :893-952 gains a batched path; greeks bump table
  gains a read-vector cache), `include/atx/vol/simd/black76_batch.hpp` (consume)
- Bench: extend P-1's bench

**Spec:**
- Strip loop: gather node vols into a contiguous buffer via one batched surface read where the
  surface type supports it (PricedSurface: add a `iv_batch(span<const double> k, T,
  span<double> out)` that walks the bracketed slices once — the ladder-reuse path
  priced_surface.hpp:367-389 is the template); then one `black76_batch` pass for the OTM prices;
  scalar fallback preserved for arbitrary SurfaceT.
- Greeks: cache the 6 distinct read vectors ({x}, {x+ks₊}, {x+ks₋}) × {T, T−dt}; σ± and r-free
  bumps reuse cached vectors with the constant vol offset applied at use site. Numerically
  IDENTICAL by construction (same reads, same adds).
- Determinism: batch kernels must produce bit-identical results to scalar on the same inputs
  per the repo's existing batch-parity test conventions (see simd tests) — gate on the existing
  parity harness pattern.

**Steps:**
- [ ] Bit-identity tests: batched strip vs scalar strip on flat + skew fixtures, all tiers
      (1e-0 ulp — exact); greeks with read cache vs without — exact.
- [ ] Implement `iv_batch` + batched loop + cache. Paired A/B. Expected: ≥2× further on greeks
      path (multiplies with P-1), ≥1.5× single-quote Audit tier.
- [ ] Commit.

**Acceptance:** bit-identity green; measured gains recorded; scalar fallback covered by the same
tests.

### Task P-4: Analytic strip greeks fast path (opt-in, FD-parity-gated)

**Findings:** GK-P analytic (K_var is a LINEAR functional of B76 prices with fixed weights — the
quadrature weights are the adjoint; vega/delta/gamma/vanna have closed forms given σ(k), σ′(k),
σ″(k); ~25× fewer evaluations and no h² cancellation noise), LIT-8 (parallel vega = only first
bucket of what a desk runs — analytic path is the enabler for F-7's cheap extra buckets).

**Files:**
- Create: `src/deriv_analytic_greeks.cpp` (+ internal header `src/deriv_analytic_greeks.hpp`)
- Modify: `include/atx/vol/derivatives.hpp` (`DerivGreekBumps::method` appended:
  `enum class DerivGreekMethod : uint8_t { FiniteDifference = 0, AnalyticStrip = 1 }`),
  `src/derivatives.cpp` (dispatch)
- Test: `tests/deriv_greeks_test.cpp`

**Spec:**
- Scope: `VarSwap` only (uncapped, any age — the future leg is the strip, the accrued leg is
  constant), FD remains the path for vol/capped kinds (their model layer isn't linear). Auto
  falls back to FD outside scope.
- Formulas (per-node, weights w_i, spacing Δx, all at the pinned grid):
  `vega_Kvar = (2/T)(Δx/3) Σ w_i · b76_vega(F, K_i, σ_i, T) / (df·K_i)` (parallel);
  `delta_Kvar = (1/S)(2/T)(Δx/3) Σ w_i · b76_vega_i · σ′(x_i) / (df·K_i)` (sticky-strike:
  B76 homogeneity ⇒ spot acts only through the smile re-read; σ′ via the same batched read at
  x±δ or the surface's analytic slope where available);
  gamma/vanna/volga from σ′, σ″ (second read vector) — derive and document each in the header
  with the homogeneity argument; theta stays FD (one rolled strip — genuinely new information);
  rho analytic per P-2.
- PV/greeks scale to contract via the existing df·N·w_future factors.

**Steps:**
- [ ] Parity tests FIRST (they define done): analytic vs FD on flat + skew fixtures, unaged +
      mid-aged VarSwap: |Δ| ≤ max(1e-8·scale, 5·FD-noise-floor) per greek, where the FD noise
      floor is the measured bound from `HighVolRegimeGridPinKeepsSecondOrderSane` (~2.2e-7).
      Gamma/volga/vanna FINALLY get a non-flat oracle this way — the analytic form IS the
      oracle and the FD path cross-checks it (two independent constructions agreeing).
- [ ] Implement; wire dispatch; default remains FD (flip evaluated at 2.0).
- [ ] Paired A/B: expect ~10× VarSwap greeks vs post-P-3 FD.
- [ ] Commit.

**Acceptance:** parity suite green across tiers; measured gain recorded; method knob documented.

### Task P-5: ConvexDense serve-path cost (bisection early-exit + price-space calendar scan)

**Findings:** FIT-P1 (P2: fixed 64-iteration bisection, no early exit — up to ~260k B76 calls
per Audit strip on a ConvexDense surface; calendar scan pays the same tax through inversion).

**Files:**
- Modify: `src/dense_slice.cpp` (:370-378), `src/vol_curve.cpp` (:451-478 calendar scan)
- Test: existing dense_slice/vol_curve groups (bit-tolerance, see below)

**Spec:**
- Bisection: break when `hi − lo < 1e-12 · max(1.0, hi)` (preserves documented near-machine
  bracket); expected ~⅓ of iterations at healthy vega.
- Calendar floor scan: compare in PRICE space (fitted node price vs `black76_price(w_prev)`) —
  the floor is enforced in price space anyway; skips inversion entirely on the scan.

**Steps:**
- [ ] Characterization: record served iv() on a pinned ConvexDense fixture at 1e-12 tolerance
      pre-change; post-change assert agreement ≤ 1e-11 (early exit can move the last ulp — state
      the tolerance, don't pretend bit-identity).
- [ ] Implement both; run fitting + admission groups; paired A/B on a ConvexDense-routed strip
      quote. Commit.

**Acceptance:** ≤1e-11 iv drift; measured ≥2× on ConvexDense strip quotes; calendar-scan
outcomes identical on the fixture matrix (same floors chosen).

### Task P-6: Book-level shared-strip memo

**Findings:** GK-P book (L var swaps on same (uid, T, cfg) cost L×14 strips; PV and every greek
are affine in a shared per-(uid,T) block).

**Files:**
- Modify: `src/deriv_book.cpp` (:188-209)
- Test: `tests/deriv_book_test.cpp`

**Spec:** memo keyed (uid, T-bits, cfg-relevant-bits, kind-class) caching the per-tenor strip
block {K_var, strip grid, and — when P-4 is on — the analytic greek block}; per-contract row =
affine transform (df·N·w_future, strike offset, age weights). Pattern: `pnl_attribution`'s pivot
groups (src/pnl_attribution.cpp:113-127). Serial loop stays (documented v1 choice); memo makes
it O(distinct tenors) instead of O(rows).

**Steps:**
- [ ] Test: book with 10 rows over 2 distinct (uid,T) → assert (via a counter hook or the
      existing instrumentation_abi counters if one fits) strip evaluations = 2×(greeks-path
      count), not 10×; totals bit-identical to the unmemoized loop.
- [ ] Implement; commit.

**Acceptance:** bit-identical totals; eval-count assertion green.

### Task P-R: Phase-2 aggregate review + citable numbers

- [ ] Fresh reviewer over the phase diff (bit-identity claims re-verified by spot repro).
- [ ] Quiet-host paired measurement of the headline chain: Standard-tier VarSwap greeks on the
      skew fixture — v1.0.0 vs post-P-6, both presets. Target: ≥5× FD path, ≥25× with P-4
      analytic. Record in this file under Outcomes.

#### Outcomes

Measured on both Release presets (i7-1260P, clang-cl 18, ThinLTO), interleaved paired A/B with
arms round-robined within each round; each round's value is the best-of-N from that round's
registered 5 repetitions. Full protocol and per-round data: `task-P-R-review.md` §3.1.

**Headline, `deriv/greeks/standard_priced_surface`, `572bc55` (pre-Phase-2) vs `7943a4b`
(post-P-6):**

| preset | FD path | analytic (+P-4) |
|---|---|---|
| `rel-avx2` (Release + AVX2/FMA) | **1.924×** (ratio CV 4.8%, 16/16 wins; all three arm CVs 3.8–4.6%, inside the repo's 5% trustworthiness bar) | **3.120×** (ratio CV 5.9%) |
| `rel` (Release, SSE2) | **1.916×** (ratio CV 6.9%) | **3.063×** (ratio CV 8.1%) |

The two presets — independently built, independently measured, different ISA — agree within
0.5% (FD) and 2% (analytic). **Release does not rescue the Debug figures**: P-4 measures
1.57–1.60× on Release against 1.65× on Debug, and P-3's greeks contribution stays in the same
1.1–1.2× band as its Debug 1.09× (`task-P-R-review.md` §3.2).

**Targets ≥5× (FD path) and ≥25× (with P-4 analytic) were both MISSED** — by **2.6×** on the
FD path and **8.0×** on the analytic path. **Ruling: plan defect, not an implementation
shortfall** (`task-P-R-review.md` §5.1, on the same footing as the P-5 ≥2× ruling). `≥25×`
traces to P-4's own Findings stating an *evaluation-count* ratio ("~25× fewer evaluations"),
which the sprint's acceptance line then carried across as a *wall-clock* target — a unit
error — and the evaluation-count claim does not survive P-4's own scope either (actual
reduction ≈2.6×, not ~25×). `≥5×` was never itself a derived figure; it was assembled from
three per-task aspirations, two of which (P-3, P-5) were already individually ruled brief
defects or missed before this review. The delivered, Release-measured, CV-clean Phase-2
outcome is **1.92× FD / 3.12× analytic**, and that is the figure to cite downstream — not the
target.

**`572bc55` is a strictly smaller-workload denominator than a literal `v1.0.0` comparison would
be**, so the targets are missed under the literal spec too, not only against the corrected
baseline: Phase 1's C-10 turned `carry_theta` on by default (~+21% cost to the same call) and
grew `DerivGreeks`' arity, so a `v1.0.0` binary does strictly less work per call than `572bc55`
does — the literal "v1.0.0 vs post-P-6" multiple would be *smaller* than 1.924× / 3.120×, not
larger (`task-P-R-review.md` M-2).

Correctness was preserved exactly throughout — end-to-end bit-identity verified by bijection on
both Release presets over a 1344-leg portfolio frame (`task-P-R-review.md` §2) — which was the
sprint's stated priority-1.

---

## Phase 3 — Features (product + risk surface a dispersion desk runs)

### Task F-1: Lee-consistent wing extrapolation mode for the strip

**Findings:** FIT-F1 (P1 feature-gap), PV-6, LIT-6. Flat-clamp beyond the certified band is
Jiang-Tian-standard and defensible, but on equity skew it systematically understates K_var
exactly when σ_atm√T ≳ 0.083 (band edge inside ~2σ). The fitting side already serves
Lee-compliant wings (eSSVI φ ceiling ⇒ slope ≤ 2; ConvexDense power-law tails); the strip
refuses to trust them.

**Files:**
- Modify: `include/atx/vol/derivatives.hpp` (`DerivConfig::wing_mode` appended:
  `enum class StripWingMode : uint8_t { FlatClamp = 0, LeeSlopeExtrapolation = 1, Raw = 2 }`;
  Raw == today's `wing_clamp_k < 0`), `src/derivatives.cpp` (read path :906-922),
  `include/atx/vol/detail/strip_grid.hpp`
- Test: `tests/derivatives_test.cpp`

**Spec:**
- `LeeSlopeExtrapolation`: beyond the certified band, serve total variance
  `w(k) = w(k_band) + β±·(|k| − k_band)` with β± = the fitted slice's OWN total-variance slope
  at the band edge, clamped to [0, 2−ε] per Lee's moment bound (ε = 1e-3); convert to vol at the
  node's T. Continuous AND C¹-matched at the band edge where the surface's own slope is served
  (kills the C-3 band-edge kink in this mode).
- Default stays FlatClamp for 1.1 (mark stability was a deliberate desk ruling — sp100 XOM);
  the mode makes the level-fidelity trade PER-CALLER instead of global.
- `WingClamped` flag: fires in FlatClamp as today; new appended `WingExtrapolated = 1u << 14`
  fires in LeeSlope mode when nodes beyond the band contributed.

**Steps:**
- [ ] Test `WingMode.OrderingUnderSkew`: skew fixture, 6M tenor: K_var(FlatClamp) <
      K_var(LeeSlope) ≤ K_var(Raw) (raw eSSVI wings are linear-in-|k| ⇒ LeeSlope ≈ Raw here;
      assert the gap < 5% of the FlatClamp→Raw gap).
- [ ] Test `WingMode.FlatSurfaceInvariant`: flat surface — all three modes agree to 1e-12.
- [ ] Test `WingMode.SlopeClampBinds`: fixture with band-edge slope > 2 (constructible via
      HingeQuad residual fixture from C-8) → LeeSlope value uses slope 2−ε, flag fires.
- [ ] Implement; document bias direction + magnitude guidance (LIT-6: JT 2007 −198/+79 bp
      framing) in the header. Commit.

**Acceptance:** three tests green; default path bit-identical; C-3's split logic handles the
mode (band edge still a panel boundary in FlatClamp; no split needed at the edge in LeeSlope —
assert via the error estimate).

### Task F-2: Gamma swaps

**Findings:** PV-F1 (highest value per effort for dispersion), LIT-7 (Lee's weighted-variance
framework: weight w(y)=y/Y₀ ⇒ λ_yy = 2/(Y₀K); vanilla decomposition ∫ 2/(Y₀K)·Van dK).

**Files:**
- Modify: `include/atx/vol/derivatives.hpp` (`DerivKind::GammaSwap = 5`), `src/derivatives.cpp`
  (strip variant + dispatch), `include/atx/vol/swap_leg.hpp`/`src/swap_leg.cpp` (leg support)
- Test: `tests/derivatives_test.cpp`

**Spec:**
- Fair strike (undiscounted claim, log-strike form): with K = F·e^x, dK = K dx,
  `K_gamma = (2/(T·S0)) · (1/df) · (Δx/3) Σ w_i · OTM_i` — the 1/K weight cancels against the
  Jacobian; integrand is just the OTM price. Same grid/span/clamp/kink machinery as the var
  strip (share the resolved-grid path; the C-3 split applies unchanged).
- Realized leg: `RV_gamma = (annualization/n) Σ (S_i/S_0) · r_i²` — extend `RealizedTracker`
  with a gamma accumulator (appended field + observe path; spec fields appended, arity pins).
- Exactness caveat documented: single-expiry claim replication is exact under zero carry; under
  r−q ≠ 0 the exact hedge needs a continuum of expiries (Lee EQF). Ship the standard
  single-expiry form; document the O((r−q)T) approximation; oracle test quantifies it.
- Aged blend, PV, FD greeks: identical dispatch structure to VarSwap (the linear-in-variance
  blend applies to the gamma-weighted variance identically).

**Steps:**
- [ ] Oracle tests first: `GammaSwap.FlatZeroCarryExact` — flat σ, r=q=0: K_gamma = σ² to 1e-6
      rel (exact: E[(S_t/S0)] = 1); `GammaSwap.SkewOrdering` — negative skew: K_gamma < K_var
      (price weighting downweights the rich put wing — standard fact); `GammaSwap.MCOracle` —
      Task-0 MC harness extended with the S_i/S_0 weight, BS with drift: 3-SE agreement.
- [ ] Implement strip variant + tracker + dispatch + greeks (FD path free; P-4 analytic scope
      note: gamma-swap analytic greeks deferred, FD is correct).
- [ ] swap_leg + deriv_book plumbing (kind passthrough; book test row).
- [ ] Commit.

**Acceptance:** three oracles green; engine×kind matrix (C-4) extended and tested; CHANGELOG.

### Task F-3: Corridor variance swaps

**Findings:** PV-F3, LIT-7 (weight = 1{K∈C}/K²).

**Files:**
- Modify: `include/atx/vol/derivatives.hpp` (`DerivKind::CorridorVarSwap = 6`; contract fields
  `corridor_lo`, `corridor_hi` — absolute strikes, 0 = unbounded side), `src/derivatives.cpp`,
  `src/swap_leg.cpp`
- Test: `tests/derivatives_test.cpp`

**Spec:**
- Strip restricted to [ln(corridor_lo/F), ln(corridor_hi/F)] ∩ resolved span; corridor edges
  become composite-Simpson split points (C-3 machinery — edges are panel boundaries, no O(h²)
  contamination).
- Realized leg: fixings count only when S_i (convention: previous close, document it) lies in
  the corridor; conditional variant (divide by in-corridor count) exposed as a quote field, not
  a separate kind (both numbers from one accrual).
- Tracker: appended in-corridor count + in-corridor Σr² (arity pins).

**Steps:**
- [ ] Oracle tests: `Corridor.FullCorridorIdentity` — corridor spanning the whole grid
      reproduces K_var to 1e-12 (same nodes, same weights); `Corridor.SubCorridorOrdering` —
      down-corridor (puts) on skew fixture > up-corridor at equal width (skew makes downside
      variance rich); `Corridor.EdgeSplitAccuracy` — corridor edge mid-grid: Standard vs Audit
      agreement < 1e-6 rel (the split is doing its job).
- [ ] Implement; dispatch matrix + tests; commit.

**Acceptance:** oracles green; realized-leg convention documented in header.

### Task F-4: Forward-start variance entry point + calendar diagnostic

**Findings:** PV-F4, FIT-F2 (analytics_primitives derives forward variance from ATM-only total
variance — a second inconsistent convention; no negative-forward-variance detection anywhere),
LIT-7 (total-variance additivity).

**Files:**
- Modify: `include/atx/vol/derivatives.hpp` + `src/derivatives.cpp`
- Test: `tests/derivatives_test.cpp`

**Spec:**
- New entry: `Result<DerivQuote> forward_var_fair_strike(const PricedSurface&, double T1,
  double T2, const DerivConfig&)` (+ templated/SurfaceRef siblings):
  `K_fwd = (K_var(T2)·T2 − K_var(T1)·T1) / (T2 − T1)`, both strips priced under ONE shared
  config resolution: same wing mode, same clamp band, same width_sigmas, node budgets resolved
  per-tenor (grids differ; that's correct — the shared POLICY is what makes the difference
  meaningful).
- Negative forward variance ⇒ `ErrorCode::Internal` with a new appended flag
  `CalendarInconsistent = 1u << 15` on the (error-path) diagnostic — a calendar-arb surface must
  fail loud here, this is the strip-level detector the fit-side lattice check can't be (FIT-F3:
  fit-side tolerance 1e-7 w ≈ 1bp² variance is the accuracy floor; document it on the entry).
- Quote fields: fair_strike carries K_fwd; `accrued_component_dec`/`future_component_dec`
  repurposed as K_var(T1)/K_var(T2) is WRONG — do NOT overload; append `leg_T1_var_dec`,
  `leg_T2_var_dec` (arity pins).

**Steps:**
- [ ] Oracles: `ForwardVar.FlatSurfaceExact` — flat σ: K_fwd = σ² to 1e-10 any (T1,T2);
      `ForwardVar.TermStructureExact` — two-pillar surface with known w(T) linear-in-T between
      pillars: K_fwd matches the constructed forward variance analytically; 
      `ForwardVar.NegativeForwardFailsLoud` — constructed θ-inverted VolSurface (C-8 fixture) →
      Internal + flag.
- [ ] Implement; deprecation note on the ATM-only forward-vol helper in analytics_primitives
      (doc pointer to the new entry; no removal in 1.x).
- [ ] Commit.

**Acceptance:** oracles green; one convention documented as canonical.

### Task F-5: Options on realized variance (var calls/puts)

**Findings:** PV-F5 (~90% built: `lognormal_call(m, s, k)` with calibrated ξ IS a variance call
under the house model), LIT-5 (lognormal tail thinness caveat — document, don't block).

**Files:**
- Modify: `include/atx/vol/derivatives.hpp` (`DerivKind::VarianceCall = 7, VariancePut = 8`;
  `strike_dec` is the option strike; `cap_dec` unused/rejected), `src/derivatives.cpp`
- Test: `tests/deriv_distribution_test.cpp`

**Spec:**
- PV = df·N·E[(V − K)⁺] (call) / df·N·E[(K − V)⁺] (put), V = a + b·W the same blended variance
  the capped kinds price; W lognormal (mean = corrected strip mean, log-sd = ξ√T_resid); closed
  forms via `lognormal_call` + put-call parity. Aged/pinned exits mirror the capped-var
  structure (a ≥ K ⇒ call pins to a − K deterministic, etc.).
- Engine: Auto/RvDistributionProxy only (C-4 matrix extended).
- Header: model-risk note per LIT-5 (RV right tail super-lognormal ⇒ house model underprices
  OTM var calls; RvDistributionAffine/McQe remain the reserved escape).

**Steps:**
- [ ] Oracles: `VarOption.PutCallParity` — E[(W−K)⁺] − E[(K−W)⁺] = m − K to 1e-12 (both closed
      forms); `VarOption.CappedSwapIdentity` — K_capped_var = K_var − call(C)·(scale): assert
      the new call at strike C reproduces the capped-var-swap's `cap_option_value_dec` to 1e-12
      (same model, same code path — this is the wiring test); `VarOption.MCOracle` — MC harness
      RV distribution + payoff, 3-SE (model-vs-MC on the LOGNORMAL's own terms: simulate W
      lognormal, not BS paths — tests the pricer, not the model).
- [ ] Implement; greeks come free through `deriv_price` dispatch (FD).
- [ ] Commit.

**Acceptance:** three oracles green; matrix + arity updates.

### Task F-6: Single-name dividend adjustment in the realized leg

**Findings:** PV feature list + LIT-9 (ISDA/MCA convention: single-name returns computed off
dividend-adjusted prior close — JPM example ln(94/95), NOT ln(94/100); index legs unadjusted;
`include_dividend_adjustment` reserved since the C port).

**Files:**
- Modify: `include/atx/vol/derivatives.hpp` (`RealizedTracker::observe_dated` overload:
  `Status observe_dated(int64_t ts_ns, double spot, double ex_div_cash)`), `src/derivatives.cpp`
  (tracker impl)
- Test: `tests/derivatives_test.cpp`

**Spec:**
- `r_i = ln(S_i / (S_{i−1} − D_i))` when `include_dividend_adjustment` is set and
  `ex_div_cash > 0`; the 2-arg overload forwards with D = 0. Reject D ≥ prev_spot
  (InvalidArgument). The spec flag stops being "reserved; unused" — header updated.
- Backtest wiring OUT of scope (the fixing driver passes 0 until the corporate-actions feed is
  plumbed — separate sprint; document).

**Steps:**
- [ ] Oracle: `Tracker.DividendAdjustedReturn` — prior 100, div 5, close 94: r = ln(94/95) =
      −0.0105361 (hand value in test); unadjusted tracker gives ln(94/100); accrual math checks
      through `rv_done_dec` at annualization 252.
- [ ] Idempotency: replayed ts with div still AlreadyExists, no mutation.
- [ ] Implement; commit.

**Acceptance:** oracles green; flag no longer documented as unused.

### Task F-7: Skew + convexity + term-bucket vega

**Findings:** GK-G1/G2 (parallel-only vega; skew is the var swap's defining risk; ~20-line view
addition), LIT-8 (SSVI-parameter scenario set: level θ, skew ρ, convexity φ, term twist).

**Files:**
- Modify: `src/derivatives.cpp` (new views beside `VolShiftView` :1118-1126; greeks table),
  `include/atx/vol/derivatives.hpp` (`DerivGreeks` appended: `skew_vega`, `convexity_vega`;
  `DerivGreekBumps` appended: `skew_abs = 1e-3` (vol per unit k), `convexity_abs = 1e-3`,
  `smile_greeks = false` — off by default, 4 extra repricings)
- Test: `tests/deriv_greeks_test.cpp`

**Spec:**
- `SkewShiftView`: `iv(k) + s·k`, floored at 1e-4 (composes under the same pinned grid);
  `ConvexShiftView`: `iv(k) + c·k²`, same floor. Central differences ⇒ skew_vega = dPV/ds per
  1.00 of slope, convexity_vega = dPV/dc.
- Term-bucket vega: deferred to the book layer (a single-tenor deriv has one bucket; the
  book-level ladder = per-tenor sum of parallel vegas the memo (P-6) already isolates — wire a
  `vega_by_tenor` map on `DerivPriceFrame` totals, appended).
- Sign doc: state the k-convention (k = ln(K/F); s < 0 steepens equity skew) and the expected
  sign on a long var swap (puts richer ⇒ K_var up ⇒ long-var PV up under s < 0 ⇒ skew_vega < 0
  per +1.00 slope on the standard skew fixture — assert it).

**Steps:**
- [ ] Tests: `SmileGreeks.SkewSignOnSkewFixture` (sign + magnitude vs a 2-sided independent
      full-repricing FD at a different bump size — bump-size independence like the vega test);
      `SmileGreeks.FlatSurfaceConvexityPositive` (convexity bump raises both wings ⇒ K_var up ⇒
      convexity_vega > 0 long var); `SmileGreeks.OffByDefaultCostsNothing` (repricing-count
      assertion).
- [ ] Implement; book plumbing for `vega_by_tenor`; commit.

**Acceptance:** tests green; arity pins; header sign conventions.

### Task F-8: Surface dynamics — overlay views, scenario route, deriv P&L explain

**Findings:** FIT-F4 (no surface-dynamics support at the surface layer; sticky-strike exists
only as a private view inside derivatives.cpp; PricedSurface immutable with no overlay API),
GK-G4 (scenario_grid is option-Position-only), GK-G5 + GK-C2 (no vol-deriv P&L attribution —
jointly with carry theta the largest explain hole), LIT-8 (Derman regimes; parallel/skew/term
minimum scenario set). This is the sprint's seed of roadmap #1 (SSR dynamics).

**Files:**
- Create: `include/atx/vol/surface_overlay.hpp` + `src/surface_overlay.cpp` (Tier-B),
  `src/deriv_pnl.cpp` + `include/atx/vol/deriv_pnl.hpp` (Tier-B)
- Modify: `include/atx/vol/scenario_grid.hpp` + `src/…` (deriv route), `src/backtest.cpp`
  (:1054-1058 swap lane emits the explain)
- Test: `tests/deriv_pnl_test.cpp` (new), `tests/scenario_test.cpp` extension

**Spec (three deliverables, one task because they share the overlay type; reviewer may split):**
1. `SurfaceOverlay`: a lightweight view over any `iv(k,T)` source with fields
   `{vol_shift, skew_shift, convexity_shift, k_shift, term_scale}` composing exactly like the
   deriv-local views (which it REPLACES — derivatives.cpp's private RespotView/VolShiftView
   re-implemented on top, bit-identity gate). `StickyMode { StickyStrike, StickyMoneyness }`
   resolves how a spot move maps to k_shift (sticky-strike: k_shift = −ln(1+h) as today;
   sticky-moneyness: k_shift = 0). Public, so scenario/theta engines can express smile dynamics
   without refitting.
2. Scenario route: `scenario_grid` gains a deriv leg — Taylor reconstruction from `DerivGreeks`
   (delta/gamma/vega/volga/vanna + skew_vega when present) per spot×vol cell, same cell
   conventions as the option book; full-repricing mode behind the same knob the option grid
   uses.
3. `DerivPnlExplain`: two-date decomposition for a swap position:
   `ΔPV = carry (theta_zero_fixing·dt) + realized-vs-implied ((r²_actual − K_var_fut/n)·leg
   weight) + vol level (vega·Δσ_atm) + skew (skew_vega·Δslope) + discount roll + residual`,
   with the IDENTITY test sum-of-components − ΔPV = residual and |residual| bounded on
   controlled fixtures. Backtest swap lane emits it beside `swap_pnl` (additive columns).

**Steps:**
- [ ] Overlay + bit-identity: greeks matrix identical after the private-view replacement.
- [ ] Sticky-mode test: StickyMoneyness spot bump on skew fixture leaves ATM vol read unchanged
      (definition) while StickyStrike re-reads up the skew — assert both deltas and their sign
      difference on the skew fixture.
- [ ] Scenario: 3×3 grid Taylor vs full-reprice agreement within Taylor's own O(h³) on small
      cells (quantified tolerance from the bump sizes).
- [ ] Explain identity test: two-date flat-world fixture where every component is hand-derivable
      (zero return day: carry = theta_zero_fixing·dt exactly, others 0, residual < 1e-8·|PV|);
      skew-move fixture (only skew changes: skew term dominates, level term 0).
- [ ] Backtest columns + one populate smoke. Commit per deliverable.

**Acceptance:** bit-identity on greeks; identity tests green; backtest emits explain columns.

### Task F-9 (stretch): CBOE discrete-strike replication + marking enforcement

**Findings:** PV-F2 (no listed-strike sum ⇒ no basis between parametric mark and CBOE-style
settlement), PV "declared, unenforced" (`CboeVarianceFuture` marking is a silent no-op — a
caller hedging with listed variance gets OTC numbers), LIT-1 (CBOE methodology: trapezoid-like
midpoint sum in strike space, ΔK_i = (K_{i+1}−K_{i−1})/2, double-zero-bid truncation,
−(1/T)(F/K₀−1)² Taylor term).

**Files:**
- Create: `src/cboe_strip.cpp` + `include/atx/vol/cboe_strip.hpp` (Tier-B)
- Modify: `src/derivatives.cpp` (marking read: `CboeVarianceFuture` ⇒ NotImplemented until this
  lands, then routes), `tests/derivatives_test.cpp`

**Spec:** `cboe_var_strike(span<const Quote>, F, df, T)` implementing the white-paper sum
verbatim (midpoint ΔK, K₀ = first strike below F, Taylor term, zero-bid truncation rule);
`deriv_price` on a `CboeVarianceFuture`-marked contract requires a quote board (new optional
arg) or fails NotImplemented LOUD (closing the silent path either way — the fail-loud half is
NON-stretch and lands with C-4 if this task is cut: one line in the dispatch matrix).
Basis diagnostic: parametric strip vs CBOE sum on the same board, exposed for research.

**Acceptance:** white-paper example reproduction test (the 2019 VIX white paper worked example,
hand-transcribed constants); marking no longer silent either way.

### Task F-R: Phase-3 aggregate review + final whole-branch review + gate

- [ ] Phase-3 aggregate review (fresh reviewer, findings → fixes).
- [ ] Final whole-branch review vs this plan: every task's acceptance re-verified from the diff;
      findings inventory cross-checked — each P0/P1/P2 finding either fixed, feature-closed, or
      explicitly ruled + documented.
- [ ] Full matrix both presets; NAV legs re-run: derivatives changes CAN move backtest NAV
      (C-1/C-2/C-3 mark moves are expected and documented) — explained moves re-pin anchors with
      the ruling recorded, unexplained moves stop the sprint.
- [ ] Version 1.1.0: umbrella/tier counts re-derived, arity pins swept (this sprint appended
      fields to DerivQuote, DerivGreeks, DerivGreekBumps, DerivConfig, DerivContract,
      RealizedVarianceSpec, DerivFlags ×3, DerivKind ×4 — every pin test named in the diff),
      CHANGELOG complete, packaging smoke, tag.

---

## Out of scope (explicit, so nobody scope-creeps into them)

- `RvDistributionAffine` / `McQe` engines and `FullMc` discrete correction (reserved; the
  fail-loud contract already exists and stays).
- Bergomi forward-variance-curve dynamics as a model (F-8's overlay + F-4's forward strikes are
  the data layer a future ξ_t^T model consumes; the model itself is roadmap).
- SSR full unification (roadmap #1) — F-8 seeds it; the calibration of sticky/vol-beta params
  from `analytics.hpp` diagnostics is its own sprint.
- ξ term structure (single flat ξ per tenor stands; documented thinness in C-5's header note).
- Roper projection port for HingeQuad (C-8 validates instead, fail-closed).
- Corporate-actions feed plumbing for F-6 (tracker API lands; backtest wiring later).
- Python bindings for the new kinds/entries (binding sprint follows the C++ freeze).
- Tenor-extrapolation policy enum (C-9 fixes the docs; behavior change is 2.0 material).

## Estimates

Phase 0: 0.5 day. Phase 1: C-1..C-4 ≈ 2.5 days, C-5..C-10 ≈ 3 days, review 0.5. Phase 2: ≈ 3.5
days including benches (P-4 is the long pole at ~1.5). Phase 3: F-1..F-6 ≈ 4 days, F-7 ≈ 1,
F-8 ≈ 2.5, F-9 stretch ≈ 1.5, reviews + gate ≈ 1.5. Total ≈ 19 days committed + 1.5 stretch.
Cut order if the sprint must shrink (reverse priority): F-9 → F-7's convexity leg → P-6 → P-5 →
F-8's scenario route (keep overlay + explain) → F-3. Correctness tasks are never cut.

---

## Appendix A — Findings inventory (2026-08-05 review)

Severity: P0 blocker / P1 major / P2 minor / P3 nit. Status ⇒ task.

### PV path (derivatives.cpp + strip + rv_lognormal + swap_leg)

| ID | Sev | Finding | Where | Task |
|---|---|---|---|---|
| PV-1 | P1 | `Diffusion1OverN` multiplicative (1+1/n), literature says additive (T/n)(r−q−σ²/2)²; ~100× overstated; wrong divisor mid-life; test pins the bug | derivatives.cpp:216-219 +3 sites | C-1 |
| PV-2 | P1 | Short-tenor under-resolution: Fast tier +6.06% at T=1/252, σ=0.20 (verified numerically); adaptive logic only widens | derivatives.cpp:839-887, strip_grid.hpp:63-71 | C-2 |
| PV-3 | P2 | `DerivFlags::LowT` declared, zero writers | derivatives.hpp:190 | C-2 |
| PV-4 | P2 | Interior bad iv nodes silently zeroed, no flag | derivatives.cpp:917-931 | C-4 |
| PV-5 | P2 | Engine selector partially enforced; two silent kind×engine mismatches | derivatives.cpp:1079-1093 | C-4 |
| PV-6 | P2 | Adaptive widening × fixed ±0.5 clamp ⇒ flat tails dominate long tenors; global default not per-underlier | derivatives.cpp:856-887, 906-921 | C-6, F-1 |
| PV-7 | P3 | Obs-count blend vs calendar annualization unchecked | derivatives.cpp:153-170 | C-9 |
| PV-8 | P3 | ξ calibrated against corrected mean but uncorrected Carr-Lee under Diffusion1OverN | derivatives.cpp:373-382 | C-1 |
| PV-9 | P3 | Expired lot: negative T flips fully-aged rho sign | derivatives.cpp:1323-1326 | C-9 |
| PV-P1 | P1 | Per-node carry/bracket re-resolution on E6/SurfaceRef strip paths | derivatives.cpp:1561-1577 | P-1 |
| PV-P2 | P2 | Diagnostic strip inside every bumped vol-swap repricing | derivatives.cpp:489-496 | P-2 |
| PV-P4 | P2 | Strip loop scalar; essvi/black76 AVX2 kernels unused | derivatives.cpp:917-938 | P-3 |

### Greeks path

| ID | Sev | Finding | Where | Task |
|---|---|---|---|---|
| GK-C2 | P1 | Theta omits implied→realized rollover; no carry output anywhere | derivatives.cpp:1226-1228 | C-10 |
| GK-C3 | P2 | rho ≡ −T·PV exactly (df-invariance); FD rate bump is a wasted strip | derivatives.cpp:1150-1177 | P-2 |
| GK-C6 | P2 | Pinned-ξ vega excludes recalibration drift (documented; size unknown) | derivatives.cpp:1281-1295 | C-5 note |
| GK-C7 | P3 | `vol_abs` ≥ surface vol silently corrupts v_dn | derivatives.cpp:1256-1260 | C-9 |
| GK-C8 | P3 | Front-pillar theta rolls into clamped forward (E6 path; bridge already fixed) | derivatives.cpp:1602-1610 | C-9 |
| GK-C9 | P3 | Book: notional sign unvalidated; one NaN lane poisons column totals silently | deriv_book.cpp:102,158-177 | C-9 |
| GK-P | P2 | 14 repricings/call; 6 distinct read vectors; ~3.6k iv reads Standard | derivatives.cpp:1213-1254 | P-2, P-3 |
| GK-P2 | P2 | Analytic strip greeks available (weights = adjoint); ~25× | — | P-4 |
| GK-P3 | P3 | swap_leg pays second_order for vega-only consumption | swap_leg.cpp:133,259 | P-2 |
| GK-P4 | P3 | Rolled-T forward cache miss | derivatives.cpp:1570-1576 | P-1 |
| GK-G1 | P2 | No term-structure vega | — | F-7 |
| GK-G2 | P2 | No skew/convexity greeks | — | F-7 |
| GK-G4 | P2 | scenario_grid has no deriv route | scenario_grid.hpp | F-8 |
| GK-G5 | P1 | No vol-deriv P&L attribution; backtest emits undecomposed swap_pnl | backtest.cpp:1054-1058 | F-8 |

### Fitting chain

| ID | Sev | Finding | Where | Task |
|---|---|---|---|---|
| FIT-C1 | P2 | eSSVI alt-driver: promised calendar projection default-off; diag stamps zeros; validate_no_arb dead | essvi_calib.cpp:1245,1267 | C-8 |
| FIT-C2 | P3 | Sequential driver θ floor is ATM-only; wings can cross | essvi_calib.cpp:1076-1079 | C-8 |
| FIT-C3 | P2 | QP ratio test admits large negative α (ulp-negative gix / near-cutoff gip) | dense_slice.cpp:245-261 | C-7 |
| FIT-C4 | P3 | No QP anti-cycling tie-break | dense_slice.cpp | C-7 |
| FIT-C5 | P2 | HingeQuad residual unprojected + validated on too-narrow band (SVI got FT-C2 fix; Essvi didn't) | essvi_calib.cpp:656-703, vol_curve.cpp:517 | C-8 |
| FIT-C7 | P2 | Latency certifies ±0.35; strip trusts ±0.5; static_assert checks default config only | pricer_fitter.cpp:1594-1598, derivatives.cpp:139-141 | C-6 |
| FIT-C8 | P3 | Tenor extrapolation contradicts docs; three containers, two policies; flat-w long end = zero forward variance | vol_curve.cpp:330-341 | C-9 |
| FIT-C9 | P3 | Between-pillar butterfly not certified (linear-in-w blend) | vol_curve.cpp:342-346 | doc'd, roadmap |
| FIT-C10 | P3 | noise_sigma doc wrong by 2× | calib.hpp:331 | C-9 |
| FIT-C11 | P3 | Zero edge slope ⇒ flat (non-decaying) right tail | dense_slice.cpp:320-322 | C-9 |
| FIT-P1 | P2 | ConvexDense iv: fixed 64-iter bisection; ~260k B76 calls/Audit strip | dense_slice.cpp:370-378 | P-5 |
| FIT-F1 | P1 | Flat-vol wings beyond ±0.5 dominate exactly the tenors that matter; fit side already Lee-compliant | strip_grid.hpp:57 + fit families | F-1 |
| FIT-F2 | P2 | No forward-variance primitive; ATM-only second convention in analytics_primitives | analytics_primitives.cpp:255-256 | F-4 |
| FIT-F4 | P2 | No overlay/sticky-mode API; scenario+theta engines can't express smile dynamics | — | F-8 |

### Literature (references → conformance)

| ID | Topic | Key result for this sprint | Task |
|---|---|---|---|
| LIT-1 | DDKZ 1999; CBOE VIX methodology; Jiang-Tian 2005/2007 | Fitted-smile Simpson beats VIX trapezoid; flat extrapolation = JT practice; CBOE sum spec for basis | F-9 |
| LIT-2 | Carr-Wu 2009; Broadie-Jain 2008; Du-Kapadia | Jump bias (1/3T)E[Σx³], understates K_var on single names; corridor sidesteps tails | doc notes |
| LIT-3 | Broadie-Jain 2008; Zhu-Lian 2011; Bergomi ch.5 | Correct discrete correction additive O(1/n) drift²; daily ≈ 3-4 vol bp with jumps, ≪1 bp without | C-1 |
| LIT-4 | Carr-Lee 2008 rrvd; Friz-Gatheral 2005; Brockhaus-Long | Naive K_vol biased low >40bp at 6M under skew; refinement via strip's own K_var nearly free; E[√W]=√m·e^{−s²/8} exact | C-5 |
| LIT-5 | Lee EQF; JPM 2006; Sepp 2008 | Cap conventions conform; lognormal RV tail too thin ⇒ caps underpriced on names | F-5 note |
| LIT-6 | Gatheral-Jacquier 2014; Lee 2004; Benaim-Friz | φ ceiling = both butterfly bounds (verified in code); Lee slope ≤ 2 wing extrapolation; truncation understates K_var | F-1 |
| LIT-7 | Lee EQF corridor/weighted; Bühler 2006 | λ_yy = 2w/y²: gamma 1/(Y₀K), corridor 1{C}/K²; total-variance additivity for forward | F-2/3/4 |
| LIT-8 | Derman 1999; Daglish-Hull-Suo 2007; Bergomi ch.7-8 | Regime-conditional delta; minimum scenario set = level/skew/convexity/term; variance vega ladder = forward-variance buckets | F-7/8 |
| LIT-9 | ISDA 2002 defs + 2007 MCAs; JPM | Divisor N not N−1; 252; single-name div adjustment (ln(94/95) example); disruption = dropped fixing | F-6 |
| LIT-10 | Davis-Rabinowitz; Trefethen | Interior C¹ kink ⇒ composite Simpson O(h²) + Richardson invalid unless kink on panel boundary; k=0 alignment is accidental today | C-3 |

Primary sources: Carr-Lee https://math.uchicago.edu/~rl/rrvd.pdf · Gatheral-Jacquier
https://arxiv.org/pdf/1204.0646 · Broadie-Jain
http://www.columbia.edu/~mnb2/broadie/Assets/variance_swaps_jumps_200903.pdf · Lee EQF
http://math.uchicago.edu/~rogerlee/EQF_weightedvarianceswap.pdf (+ corridor variant) · JPM
Variance Swaps primer (Allen-Einchcomb-Granger 2006) · CBOE VIX methodology PDF · Lee 2004
moment formula http://math.uchicago.edu/~rogerlee/moment.pdf · Jiang-Tian SSRN 880459 ·
Zhu-Lian SSRN 1721897 · Derman Regimes of Volatility http://pricing.online.fr/docs/regimes.pdf
