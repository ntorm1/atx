# L3g family-greeks — analytic Greeks, the exact Newton slope, chain and shock evaluation on `AlSigmaFamily` (2026-10-20)

Lane `L3g-family-greeks` (branch `feat/vol-L3g-family-greeks-20261020`, base L2h fix tip `6e94ba40`,
pool-36, RunId `vol-20261020r3-L3g`). Brief: `.superpowers/sdd/2026-10-18-vol-sota/lane-L3g-family-greeks-brief.md`;
research: `research-analytics-hft-report.md` §3.5 / §4. Every number below is from ONE run on the
shared host (unqualified; the load line is in the lane report). Nothing default changes: no caller
constructs a family or calls these entries (L2i adopts `family_price_and_vega` opt-in; L3h consumes the
chain / shock entries); the AL fingerprint `53c4df68`, the dev200 default parity `c89d0739` and the
`pricing/` + `simd/` name set are the three proofs (§6).

## 1. What landed

- **`AlSigmaFamily::State` moved to the private header `src/pricing/american_sigma_family.hpp`** (pure
  code motion with the file-local `cheb_node` / `bary_eval` / `axis_of` and the three constants; `prepare`
  / `price_at` / `admit` byte-for-byte in the TU). The Greeks TU reaches the family's private pointer
  through `sigma_family_detail::FamilyAccess`, the one friend `american.hpp` declares for it (the private
  NAME `AlSigmaFamily::State` cannot be spelled outside the class or a friend, so the nested-class
  accessor the brief imagined could not serve — §6 lists the one-line friend as the deviation).
- **NEW `src/pricing/american_family_greeks.cpp`** and the additive `american.hpp` block after
  `AlSigmaFamily`: `AlSigmaFamilyGreeksMask` (price-only / hedge = Δ Γ vega θ / eight / all = + charm),
  `AlSigmaFamilyGreeks` (price + 9 Greeks, typed status, `exercised` / `floored` / `boundary_term_included`),
  `family_greeks(fam, S, K, σ, mask)`, `family_price_and_vega(fam, S, K, σ)` (the exact Newton slope),
  `family_chain_greeks(put*, call*, S, K[], σ[], side[], mask, columns, status[])`,
  `family_shock_grid(fam, S, K, σ, spot_pct[], vol_bump[], mask, columns, status[])`, and the detached
  serve copy `AlSigmaFamilyDetached` / `family_detach(fam)` (the family without its ≈ 90 KB geometry).
- **One fused pass** over the p = 48 premium nodes computes the price (expression for expression the
  kernel's own accumulation — the price beside the Greeks and the detached copy's price are
  `AlSigmaFamily::price()` to the bit, Reference and Fast geometries, pinned) and every derivative:
  Δ / Γ / speed at the fixed (spot-independent) boundary; vega / vanna / volga = explicit σ-terms + the
  boundary term through dy_k/dσ (nodal Chebyshev differentiation matrices D⁽¹⁾ / D⁽²⁾ applied to the
  σ-series, interpolated by the same barycentric basis — exact for the polynomial, stable at and near a
  node); θ / charm from the continuation-region PDE identity; rho / phi ONLY through a carry set
  (resume r4, below). Calls through McDonald-Schroder + the homogeneity identities of the internal put.
- **Resume r4 (2026-10-21, PM ruling: the frozen-boundary rho is never served)**:
  `AlSigmaFamilyCarryBumps` (`prepare(base, axes)`) builds SECOND families at r ± h and q ± h
  (h = `kAlSigmaFamilyCarryBump` = 1e-4, the bundle's own bump; the base geometry's AlOpts, the base's
  side / anchor / box / nodes / axis) and keeps only dY_{k,s}/dr, dY_{k,s}/dq of their σ-series (A5:
  forward difference vs the base when the down bump leaves the American regime); `family_greeks(fam,
  carry, S, K, σ, mask)` adds the boundary term Σ ∂P/∂y_k · dy_k/dθ in the SAME fused pass (one more
  τ-barycentric accumulator, no extra quadrature). Without a carry set a requested rho / phi is NaN with
  `carry = NoCarryBumps`; a set built from another family (side, anchor, (T, r, q), box, nodes, axis,
  profile, two series sentinel rows) or lacking a requested axis gives `CarryBumpsMismatch`.
  `family_chain_greeks` takes optional `put_carry` / `call_carry` (trailing, defaulted); flags bit 3
  marks a refused rho / phi. All pinned bounds filled (2-figure round-ups of the measured worst), the
  wrong call exercise fixture fixed (call S 100 K 40 with q < r is a continuation row), one revert
  spot-check per Greek class on the SOURCE (§6).
- NEW `tests/american_family_greeks_test.cpp` (`AlSigmaFamilyGreeks.*`, in `atx-vol-pricing-slice`),
  additive bench rows in `bench/al_preset_ladder_bench.cpp` (env `ATX_VOL_AL_FAMILY_GREEKS=1`).

## 2. Design notes

- **The map being differentiated** (research §4.1, now Verified by the price identity): for the
  internal put, P = clamp(E + Π), Π = ½√T Σ_j w_j I_j, I_j = 2 z_j [r K e^{−r t_j} Φ(−d−) − q S e^{−q t_j}
  Φ(−d+)], d± = ln(S e^{−q t}/(B_j e^{−r t}))/v ± v/2, v = σ√t, B_j = xmax e^{−√y(T − t_j)}, y(u) the
  τ-barycentric interpolant of y_k(σ), y_k(σ) the σ-barycentric interpolant of the series. The fused pass
  accumulates `total += w_j · I_j` with the kernel's own expressions (`premium_integrand_put` /
  `premium_integrand_put_fast`, `al_boundary_at`, `al_put_premium_impl`, the clamp order of
  `al_put_price_from_boundary`) — the price beside the Greeks is `price()` to the bit on every contract
  of every set, Reference and Fast geometries (`PriceFieldIsTheFamilyPriceToTheBit`, §3).
- **Δ, Γ, speed**: ∂d±/∂S = 1/(S v) at fixed B — exact for the served map (the internal put's boundary is
  spot-independent). With a = rK e^{−rt}, b = qS e^{−qt}, u = 1/(Sv), φ± = φ(d±):
  ∂I/∂S ∝ −a φ− u − q e^{−qt} Φ(−d+) + q e^{−qt} φ+/v; ∂²I/∂S² ∝ a φ− d+/(S²v²) + q e^{−qt} φ+/(Sv) −
  q e^{−qt} d+ φ+/(Sv²); ∂³I/∂S³ in the TU. The European leg's closed forms (Δ, Γ, speed, vega, vanna,
  volga, ∂/∂r, ∂/∂q) use the same Φ / φ implementation as the kernel (atx::core on Reference, Cody on Fast).
- **vega, vanna, volga** = the explicit σ-terms (∂d+/∂σ = −d−/σ, ∂d−/∂σ = −d+/σ) + the boundary motion.
  With s_j = √y_j, B_j = xmax e^{−s_j}: B' = −B s', B'' = B (s'² − s''), s' = y'/(2s), s'' = y''/(2s) −
  y'²/(4s³); every boundary term carries ∂I/∂B ∝ 1/B, so B cancels and only s', s'' enter:
  vega_b ∝ −G s'/v, G = a φ− − b φ+ (≥ 0); volga_b ∝ 2 s'(1 − d+d−) G/(σv) + s'²[(a d− φ− − b d+ φ+)/v² −
  G/v] + G (s'² − s'')/v; vanna_b ∝ −(s'/v) ∂G/∂S. y_k'(σ), y_k''(σ): the nodal differentiation matrices
  D⁽¹⁾_{st} = (w_t/w_s)/(z_s − z_t), D⁽¹⁾_{ss} = −Σ_{t≠s} D⁽¹⁾_{st} (Berrut & Trefethen, SIAM Review 46
  (2004), eq. (9.4), negative-sum diagonal) and D⁽²⁾_{st} = 2 D⁽¹⁾_{st}(D⁽¹⁾_{ss} − 1/(z_s − z_t)),
  D⁽²⁾_{ss} = −Σ_{t≠s} D⁽²⁾_{st} (Welfert 1997 — the paper's printed D⁽²⁾ has a sign typo; the Welfert
  form is the one implementations use), applied to the series and then INTERPOLATED by the same
  barycentric basis b_s(z): y'(z) = Σ_s b_s(z) (D⁽¹⁾Y)_s is an identity for the degree-(n−1) polynomial
  (its derivative is reproduced by n nodes), exact and forward-stable, and at an exact node hit b = e_hit
  reduces it to the nodal derivative (the exact-node erratum, p. 510: the quotient-rule derivative of the
  barycentric formula is 0/0 at a node and loses O(1/|z − z_s|) digits near one — never used here;
  `GreeksAreContinuousThroughASigmaNode`). Tables depend on n_σ alone (process-wide, built once);
  per call: n_σ² + 2 n_b n_σ FMAs. Chain rule through z(σ): dz/dσ = 2/(u_hi − u_lo) · (1/σ on LogSigma,
  1 on Sigma), d²z/dσ² = −2/((u_hi − u_lo) σ²) on LogSigma.
- **θ, charm**: the continuation-region PDE identity θ = rV − (r − q) S Δ − ½σ²S²Γ, charm = ∂θ/∂S with
  the analytic speed — the `american_greeks_al` / `RecordGreeks` definition. The family has NO τ axis
  (one geometry per expiry), so an FD-in-T theta does not exist on it; the identity is exact in the
  continuation region given exact V, Δ, Γ (which these are, for the served map). θ is compared against
  the adjoint-free bundle's PDE θ in §4 (same identity, FD Δ / Γ).
- **rho, phi**: frozen-y part: ∂I/∂rate ∝ K e^{−rt}(1 − rt) Φ(−d−) − (t/v) G, ∂I/∂yield ∝
  (t/v) G − S e^{−qt}(1 − qt) Φ(−d+), plus the explicit xmax(K, rate, yield) = K min(1, rate/yield)
  rescale of B (∂ ln xmax/∂rate = 1/rate, ∂/∂yield = −1/yield when rate < yield — the SPY call
  orientation; 0 otherwise), plus the European leg. The frozen part ALONE is up to 2.42× off the
  re-solved rho (§4) — the boundary's own motion is first order for the budget-limited y*, not the
  envelope-small term the research assumed — so it is never served (PM ruling r4). The served rho / phi
  add the boundary term exactly as vega adds dy/dσ: ∂I/∂B · dB/dθ ∝ −G s'_θ/v, s'_θ = y'_θ/(2√y),
  y'_θ = Σ_k ℓ_k(u_j) Σ_s b_s(z) D^θ_{k,s} with D^θ = (Y(θ + h) − Y(θ − h))/(2h) from the carry set's
  bumped families (central; forward when the down bump leaves the regime). The carry set maps option
  axes to the internal put's: a put's rate is r and yield q, a call's rate is q and yield r (so call rho
  rides the internal YIELD term, call phi the RATE term). Cost: 2 family builds per axis per
  (expiry, side) (§5), then ≈ free per contract (one more accumulator in the same pass). Design choice vs
  the brute-force "second family price difference" (4 extra quadratures per contract): the same
  derivative to O(h²) (measured §4, the formula test), ≈ 20 µs cheaper per contract and without the
  difference-of-two-interpolants noise / 2h on wide boxes.
- **Calls**: the internal put's bundle at (S_int = K, K_int = S; rate = q, yield = r) mapped by
  homogeneity — exact for the served map (y strike-free; xmax, E and the clamps linear in K_int):
  Δ_call = (P − S_int P_S)/K_int, Γ_call = (S_int/K_int)² P_SS, speed_call = −x³ P_SSS − 3x² P_SS/K_int,
  vanna_call = (vega − S_int vanna_int)/K_int, rho_call = ∂P/∂yield, phi_call = ∂P/∂rate; vega, volga
  unchanged; θ / charm from the PDE in the option's own (S, r, q).
- **Regime flags** mirror `RecordGreeks`: when the internal spot is on the exercise side of the current
  edge (S_int ≤ B_now = xmax e^{−√y[n−1]}, an exact node hit), or the served price is intrinsic to the
  bundle's tolerance (≤ intr + 1e-9 K — the kernel's clamp, or the decomposition reproducing K − S to
  quadrature noise, which it does in and just above the exercise region), the exercise Greeks go with
  the unchanged price (Δ = −1 put / +1 call, everything else 0 — rho / phi 0 with a carry set,
  `exercised`); `floored` = that regime on the continuation side of the edge. The flags agree with the
  bundle's own intrinsic test along a 41-point spot ladder through the edge (41/41,
  `ExerciseRegionServesIntrinsicWithTheExerciseGreeks`).
- **Mask**: price-only IS `price()` (the same `price_at`); every other mask runs the fused pass and
  skips the accumulators (and the σ tables) it does not need; a requested field is bit-identical across
  masks (each accumulator's sequence is its own: `MaskSelectsWhatIsComputedAndRequestedFieldsAreMaskIndependent`).
- **Detach**: `AlSigmaFamilyDetached` owns the σ-series, the node grid and the workspace header (the
  Gauss-Legendre table pointers are process-wide, `n_quad_price`, `fast`): `state_bytes()` ≈ the family's
  5,776 + ~40 B. It serves price (through the fused pass in price-only mode — the bit-identity is what
  makes this legal), Greeks and the slope; no generation stamp (it cannot go stale); the family may be
  re-prepared or its geometry released after `family_detach`. Chain / shock entries on a detached copy are
  not provided (L3h can add them on the same kernel: `greeks_on_view` is the one shared body).
- **Threads / allocation**: every entry is const and stack-only (one AlBoundary ≈ 1.3 KB + ≈ 1 KB of
  scratch); no counters, no ledger bump (nothing default calls this TU). 8 threads × 4 passes bit-identical
  to the serial loop (`ConcurrentGreeksAreBitIdenticalToTheSerialLoop`).

## 3. Formula check — Richardson on the family's own map

`AlSigmaFamilyGreeks.GreeksMatchRichardsonOnTheFamilyMapWithinBound` (vol-rel, final source; the same
digits at `a4b34521`): relative gap of each analytic Greek vs a Richardson central difference of the
FAMILY's own price (spot step min(1e-3 S, 0.02 S σ √T), σ step 5e-4 / 2.5e-4; floors Δ / vanna 1e-3,
Γ 1e-6 S, vega / volga 1e-3 max(P, 1), charm 1e-2 max(P, 1)); continuation rows whose stencil stays
3 h off the exercise edge. Bound = the pinned 2-figure round-up.

| set (n) | Δ | Γ | vega | vanna | volga | charm |
| --- | --- | --- | --- | --- | --- | --- |
| SPY-like 23 d (229) | 2.31e-8 [2.4e-8] | 1.34e-7 [1.4e-7] | 1.83e-8 [1.9e-8] | 6.27e-7 [6.3e-7] | 4.31e-5 [4.4e-5] | 6.16e-5 [6.2e-5] |
| deep-carry (180) | 7.02e-8 [7.1e-8] | 3.07e-8 [3.1e-8] | 3.24e-10 [3.3e-10] | 4.20e-4 [4.3e-4] | 6.16e-9 [6.2e-9] | 1.35e-4 [1.4e-4] |
| profile grid [0.08, 0.7] (967) | 2.36e-8 [2.4e-8] | 1.21e-6 [1.3e-6] | 4.80e-7 [4.8e-7] | 4.92e-6 [5.0e-6] | 1.13e-4 [1.2e-4] | 5.96e-3 [6.0e-3] |

Worst contracts (printed by the test): SPY volga at the ATM call σ 0.306 (volga ≈ 0 under the 1e-3·P
floor — the reference's σ step is the limit); deep-carry vanna / charm at S = K = 100, σ 0.052, T 2-5
(1.3 % above the edge: the FD stencil's own truncation); grid charm at put K 140 T 0.5 σ 0.32 (the spot
difference of the served θ). Δ / vega are 1e-8-class, Γ 1e-7 (1e-6 on 7-day rows), second order 1e-6
to 1e-4 where the REFERENCE resolves it — the research's "1e-8 first order / 1e-6 second order"
expectation holds on the smooth region. The near-edge band (±3 h of the edge) is excluded: there the
served max(decomposition, intrinsic) flickers along a spot ladder and no FD reference exists (ledger
candidate 3 of the checkpoint).

The vega boundary term is NOT small: with it removed (revert spot-check, §6) the vega gap is 2.41
(SPY), 0.99 (deep-carry), 13.1 (grid) — the boundary's σ-motion is first order for the budget-limited
y*, the same finding as rho's 2.42 below.

`PriceAndVegaIsTheExactSlopeWithinTheVolgaBound` (the slope L2i adopts): |vega − family forward chord|
≤ h·|volga| + 1e-8 on every row; vega vs the family's central difference at σ ± h worst 2.36e-5
[2.4e-5]; vs the cold `al_price_pack` chord (boundary re-solved at σ and σ + h, de-biased by h/2·volga)
worst 2.36e-5 [2.4e-5] — both at call K 927.6 σ 0.154 (n = 211 / 231).

## 4. Error tables vs the Reference bundle (i) and the Richardson / second-family reference (ii)

`american/family_greeks_error/<set>/n<N>` (vol-rel-avx2 bench `e5ad3261…`, binary of `9c9f0e0a`;
`C:\atx-cache\quote-benchmark\L3g-20261021-resume\errors\family-greeks-errors.json`). Max / p99 relative
(|ref| ≥ floor: Δ / vanna 1e-3, Γ 1e-7 S, the rest 1e-3 max(P, 1)); EVERY priced continuation row,
near-edge rows INCLUDED (the tests exclude ±2.5e-3 S of the edge; maxima that sit on near-edge rows are
the kink, not the formula). (ii) = Richardson on the COLD kernel (Δ / Γ h = 1e-3 S and h/2, one-sided on
the continuation side near the edge; σ ± 2e-3 / 1e-3 re-solves) for Δ…volga; for rho / phi (ii) = the
second family's price difference (fam2).

SPY 23 d (317, 12 LogSigma nodes) — max [p99]:

| Greek | vs (ii) Richardson / fam2 | vs (i) bundle |
| --- | --- | --- |
| Δ | 8.3e-8 [6.6e-8] | 1.5e-3 [1.3e-3] |
| Γ | 4.6e-5 [2.5e-7] | 5.7e-4 [5.1e-4] |
| vega | 1.8e-6 [3.6e-7] | 1.3e-3 [5.3e-4] |
| θ | 8.0e-4 [1.6e-6] | 4.6e-3 [6.1e-4] |
| vanna | 2.2e-4 [1.6e-6] | 8.2e-3 [9.6e-4] |
| volga | 8.0e-5 [2.5e-6] | 1.7e-4 [1.4e-4] |
| rho (carry, served) | 1.5e-5 [6.1e-6] | 1.5e-5 [6.1e-6] |
| phi (carry, served) | 1.2e-5 [4.7e-6] | 1.25e-5 [4.7e-6] vs the cold q ± h difference |
| rho frozen-y (NOT served) | 6.3 [2.5] | 6.3 [2.5] |
| bundle rho vs fam2 / cold phi vs fam2 | 3.9e-7 / 4.6e-7 | — |

Other sets (12 nodes unless stated), max [p99] vs (ii) / served rho vs bundle:
- ledger grid (104): Δ 2.1e-6, Γ 3.6e-7, vega 6.1e-6, θ 4.0e-6, vanna 1.1e-5, volga 2.5e-5; rho 7.3e-7,
  phi 5.3e-7; frozen rho 0.12. vs the bundle every Greek 2.7e-3 – 8.2e-3 (the bundle's stencils).
- deep-carry corner (640): Δ 1.2e-7, Γ 1.4e-7, vega 7.5e-7, θ 3.5e-5, vanna 2.6e-3, volga 1.8e-4; rho
  1.4e-5, phi 1.1e-5; frozen rho 0.996 (p99 0.992: the frozen rho misses essentially ALL of rho there).
  vs the bundle θ 1.16 / vanna 1.93 (the bundle's FD Γ / mixed stencil straddling the edge 1.3 % below
  spot — reference-limited, LEDGER 2026-10-16).
- profile fastgrid [0.076, 0.735] (983 of 1,350; 225 refusals = q = 0 calls + σ outside the box):
  n12 Δ 2.3e-6 [4.8e-7], Γ 9.6e-4 [2.9e-6], vega 1.5e-3 [4.3e-5], vanna 9.9e-2 [1.2e-3], volga 3.9e-2
  [5.4e-4]; rho 2.0e-3 [4.9e-5], phi 2.0e-3 [4.1e-5] (bundle vs fam2 itself 2.1e-3 [1.6e-5]: the wide
  box's interpolation error); n16: Δ 3.5e-7, Γ 1.4e-3 [5.1e-7], vega 1.2e-3 [1.6e-5], volga 9.1e-2
  [4.2e-4]; rho 1.3e-3 [4.9e-5].
- spy23 off-node midpoints (311): p99 Δ 9.2e-8, Γ 4.7e-6, vega 6.7e-7, θ 2.8e-5, vanna 1.7e-6, volga
  4.7e-6; rho 7.9e-6 / phi 7.1e-6 p99 (max 1.5e-4 / 1.4e-4 on near-edge rows); maxima of Γ / θ / vega
  (1e-2 – 1.1) are the 6 near-edge rows next to the 6 exercised ones.

**rho / phi decision (PM ruling r4 implemented)**: the frozen-boundary rho is 0.12 – 6.3 relative off
(p99 2.5 on SPY) — never served. The carry set is Reference-class: 1.5e-5 max / 6e-6 p99 vs the
re-solved bundle on SPY 23 d, 1e-5 on the deep-carry corner, 7e-7 on the ledger grid; on the wide
fastgrid box both it and the brute-force second family sit 1e-3 from the bundle (the box's own
interpolation error). One unattributed residual: on SPY the carry rho is 1.5e-5 from the brute-force
second-family difference while that difference is 3.9e-7 from the bundle — both brute-force
differences straddle the near-edge kink the same way and the analytic boundary term does not (the
test, which excludes near-edge rows, measures 5.9e-6; NOT proven — the bench does not split near-edge
rows out); in absolute terms the gap is ≤ 3.5e-5 USD per unit rate (≤ 3.5e-9 USD per bp). Pinned:
`GreeksWithinMeasuredBoundOfTheReferenceBundleOnSpy23` rho 6.0e-6 / phi 5.3e-6 (measured 5.95e-6 /
5.25e-6, near-edge excluded) and `CarryRhoPhiAreTheDerivativeOfTheSecondFamiliesWithinBound` 3.7e-4 /
4.3e-4 (all sets incl. the wide box; the worst is the wide box's σ = 0.08 edge).

**Correction (A0 r6, 2026-10-23 — review-L3g m1).** The attribution above is withdrawn. The
review showed the carry-vs-brute-force rho residual is a BULK offset (p99_abs 3.24e-5 ≈ max_abs
3.49e-5 USD per unit rate), not a near-edge outlier. The discriminator it asked for was run
(A0 lane, pool-3, vol-rel): `AlSigmaFamilyCarryBumps::prepare`'s bump temporarily set to
h = 5e-5, `GreeksWithinMeasuredBoundOfTheReferenceBundleOnSpy23` rerun, the bump restored and
the test rerun at h = 1e-4 (logs `A0-20261023-greeks\logs\m1-discriminator-h{5e-5,1e-4}.log`):

| h | rho_carry worst rel | phi_carry worst rel | worst row |
| --- | --- | --- | --- |
| 1e-4 | 5.949e-6 | 5.252e-6 | put S 773, K 811.65, T 0.068 |
| 5e-5 | 1.996e-6 | 1.702e-6 | the same row |

The gap falls 2.98× (rho) / 3.09× (phi) when h halves. Fitting gap = c h² + e: c h² = 5.3e-6
(rho) / 4.7e-6 (phi) at h = 1e-4 and e ≈ 0.6e-6 — about 90 % of this test's residual is the
carry set's O(h²) bump truncation (the review's candidate: the carry route's truncation
P_y y''' h² / 6 differs from the brute-force difference's P''' h² / 6, so the two routes
need not agree beyond O(h²)), the rest a floor from the Reference bundle and the family's
interpolation. The bench's carry-vs-fam2 offset was not re-measured at h / 2. The (σ, q)
family's rate-bump set shows the clean case: against the Richardson reference of its second
families the gap falls 3.99999× (median over 136 rows) at h / 2
(`AlSigmaQFamilyGreeks.RhoIsTheBumpedFamiliesCentralDifferenceToItsTruncation`). Serving
impact unchanged (≤ 3.5e-9 USD per bp); the bump is not changed.

## 5. µs table (SPY 23 d, avx2, unqualified, one run)

`american/spy23_family_greeks/*`, `american/family_chain/*`, `american/family_shock/*` (bench
`e5ad3261…`, `us\family-greeks-us.json`). Host LOADED: load 37 % before the µs rows, 78 % after (five
build lanes live) — unqualified; the price-only row (7.6 µs p50) vs L2h's 4.7 µs quiet-host number says
this run is ≈ 1.6× slow.

| row | p50 µs | p99 µs | mean µs |
| --- | --- | --- | --- |
| price-only (= `price()`) | 7.6 | 12.8 | 7.7 |
| + Δ Γ vega θ (hedge) | 12.2 | 18.8 | 12.5 |
| + 8 Greeks (eight, rho / phi via carry) | 13.7 | 35.1 | 14.4 |
| all (+ charm) | 15.4 | 39.5 | 17.5 |
| `family_price_and_vega` | 19.1 | 43.1 | 20.1 |
| `price_and_chord` (two quadratures) | 21.4 | 36.6 | 21.6 |
| `american_greeks_al` accurate / Fast, same contracts | 1,125 / 515 | 1,638 / 892 | — |
| adjoint bundle (puts, 163) | 1,807 | 2,337 | — |

(The `price_and_vega` row ran after `all` as the load rose; its p50 above `hedge` is the host, not the
kernel — it computes a subset of hedge's accumulators. A quiet-host pin is W4's.) Carry set build:
7.4 ms mean per (S, T, r, q, side) family (4 bumped geometries + families; 148 families on SPY 23 d
because the funding-pv q is per strike); 8.3 ms in the `all` row. `AlSigmaFamilyCarryBumps::state_bytes()`
≈ 8.5 KB.

Chains (one single-thread `family_chain_greeks` call, ms): synthetic 300 strikes price 2.83, hedge
5.23, eight 6.82, all 7.04; SPY 23 d chain (317 rows) price 2.91, hedge 5.71, eight 6.82, all 5.65;
carry sets for both sides of one expiry 20 ms (outside the timed call). **Target ≤ 5 ms for 300 +
8 Greeks: 6.82 ms on this loaded host** (price-only 2.83 ms; at the 1.6× load factor ≈ 4.3 ms quiet —
inferential, a W4 pin decides). Shock 11 × 11 (± 10 % spot, ± 10 vol points, eight mask): 2.18 ms per
contract; 704 ms per synthetic 300-chain (1,001 of 36,300 cells refused SigmaOutsideBox). Per contract
vs today at p50 on the same host: ≈ 80× the accurate bundle, ≈ 130× the adjoint bundle.

## 6. Gates

Commands, exit codes and output are in the lane report (`lane-L3g-family-greeks-report.md`).
- `AlSigmaFamily.*` 11/11 with every `[AlSigmaFamily]` printout equal to the L2h-fix1 digits;
  `AlSigmaFamilyGreeks.*` 15/15 (vol-rel, source `78b5db21`; the bench commit `9c9f0e0a` touches no
  library or test source).
- Revert spot-checks on the SOURCE (inverse Edit; restored blob `f891dcb1` = HEAD's): Δ (sign of the
  premium spot term) → formula test fails (Δ 0.133 / 3.04 / 2.62); vega (boundary term removed) →
  formula + slope tests fail (vega 2.41 / 0.99 / 13.1; slope vs cold 2.41); volga (D⁽²⁾ term sign) →
  formula test fails (volga 5.45 / 1.99 / 43.9); rho (carry boundary term removed) → bundle test (rho
  2.42) and carry-vs-fam2 test (13.7) fail.
- AL fingerprint `53c4df68` MATCH (vol-rel-avx2, `9c9f0e0a`).
- gates-B (sibling suites, `^pricing/AlSigmaFamily`, chain-batch, name set cap 3, hygiene, dev200
  parity): see the lane report.

## 7. What L3h / L2i must assume

1. **The calls** (api/pricing/american.hpp, the block after `AlSigmaFamily`; all opt-in, nothing default
   constructs a family): `family_greeks(fam, S, K, σ, mask)` → `AlSigmaFamilyGreeks` {price, delta, gamma,
   vega, theta, rho, phi, vanna, volga, charm, status, carry, exercised, floored,
   boundary_term_included}; `family_greeks(fam, carry, S, K, σ, mask)` (rho / phi served);
   `AlSigmaFamilyCarryBumps::prepare(fam, Mask{kRho | kPhi})` once per (expiry, side) after the family;
   `family_price_and_vega(fam, S, K, σ)` → {price, vega, status}; `family_chain_greeks(put*, call*, S,
   K[], σ[], side[], mask, columns, status[], put_carry* = null, call_carry* = null)` → Status
   (InvalidArgument on a length mismatch, nothing written; per-row outcomes in status[]);
   `family_shock_grid(fam, S, K, σ, spot_pct[], vol_bump[], mask, columns, status[])` → Status, cell
   (i, j) at i · n_vol + j (rho / phi refused per cell); `family_detach(fam)` → `AlSigmaFamilyDetached`
   with `price` / `greeks` / `price_and_vega` (rho / phi refused).
2. **Refusal routing** = the family's: `NotPrepared` / `GeometryRePrepared` → rebuild or cold;
   `InvalidArgument` → the cold path's own refusal; `SigmaBelowGuard` (σ < 0.01) / `SigmaOutsideBox` →
   cold. On a refusal the price AND every requested Greek are NaN with the status (never NaN alone). The
   chain entry writes NaN into every stored column of a refused row and `NotPrepared` for a row whose
   side has no family; the shock grid refuses a cell whose σ leaves the box (`SigmaOutsideBox`) or whose
   shocked spot is ≤ 0 (`InvalidArgument`). The L1c non-monotone refusal stays in front of any
   family-based INVERSION exactly as L2h §7 item 4 says; the Greeks never see an inversion target.
3. **L2i's one-line opt-in**: in the Premium iterate replace `const AlSigmaFamilyChord c =
   fam.price_and_chord(S, K, x); slope = (c.price_sigma_bumped − c.price) / kAlPackSigmaBump;` by `const
   AlSigmaFamilyPriceVega pv = family_price_and_vega(fam, S, K, x); slope = pv.vega;` — the slope of the
   SAME map (boundary moving with σ through the interpolant), without the chord's h/2 · volga bias and
   without the kAlPackSigmaBump headroom at the box top (the box rule's `+ 2 · kAlPackSigmaBump` may
   go). NOT wired in this lane. Cost: one fused pass (§5) instead of two quadratures.
4. **Greeks' regime caveats**: θ / charm are the PDE identity (no τ axis); in the exercise region the
   exercise Greeks are served (flags above); rho / phi exist ONLY with a matching carry set (the frozen
   boundary is never served — PM ruling r4): no set → NaN + `carry = NoCarryBumps`; a set from another
   family or without the axis → NaN + `CarryBumpsMismatch`; the carry status is all-or-nothing over the
   requested rho / phi; a carry set is a value copy (it does not observe the family) and must be rebuilt
   when the family is (it refuses by identity otherwise). `boundary_term_included` is true when vega /
   vanna / volga were computed; a served rho / phi always carries its (r, q) boundary term.
5. **Column contract** (`AlSigmaFamilyGreekColumns`): a column is written iff its span is non-empty, and
   then its length must equal the row count; the mask decides what is COMPUTED, the spans what is STORED
   (an unrequested Greek with a span reads 0.0); `flags` bit 0 = exercised, bit 1 = floored, bit 2 =
   boundary_term_included, bit 3 = a requested rho / phi refused. Spans are caller-owned; the entries
   allocate nothing (the carry set's prepare() allocates its block and two bumped geometries).
6. **Shock axes**: spot (the homogeneity rescale, exact) and absolute vol bumps (inside the box);
   dt / dr / dq are not family axes — a time or rate shock needs another geometry (and family).
7. **Cost per call**: §5 — the µs line of the lane report; price-only = `price()`.
8. **detach() exists** (`family_detach`); ≈ 5.8 KB per (expiry, side) copy; the geometry (≈ 90 KB) can be
   released after it.
