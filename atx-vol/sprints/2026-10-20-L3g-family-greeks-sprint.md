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
  node); θ / charm from the continuation-region PDE identity; rho / phi frozen-boundary (frozen y, the
  explicit xmax(K, rate, yield) rescale differentiated; dy/dr, dy/dq NOT included — measured in §4).
  Calls through McDonald-Schroder + the homogeneity identities of the internal put.
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
- **rho, phi**: frozen-boundary (frozen y): ∂I/∂rate ∝ K e^{−rt}(1 − rt) Φ(−d−) − (t/v) G, ∂I/∂yield ∝
  (t/v) G − S e^{−qt}(1 − qt) Φ(−d+), plus the explicit xmax(K, rate, yield) = K min(1, rate/yield)
  rescale of B (∂ ln xmax/∂rate = 1/rate, ∂/∂yield = −1/yield when rate < yield — the SPY call
  orientation; 0 otherwise), plus the European leg. dy/dr, dy/dq — the boundary's own motion — is NOT
  included (no r / q axis). Both conventions (frozen y = served; frozen B = without the xmax term) are
  measured against a second family at r ± 1e-4 / q ± 1e-4 in §4; the PM rules on the policy.
- **Calls**: the internal put's bundle at (S_int = K, K_int = S; rate = q, yield = r) mapped by
  homogeneity — exact for the served map (y strike-free; xmax, E and the clamps linear in K_int):
  Δ_call = (P − S_int P_S)/K_int, Γ_call = (S_int/K_int)² P_SS, speed_call = −x³ P_SSS − 3x² P_SS/K_int,
  vanna_call = (vega − S_int vanna_int)/K_int, rho_call = ∂P/∂yield, phi_call = ∂P/∂rate; vega, volga
  unchanged; θ / charm from the PDE in the option's own (S, r, q).
- **Regime flags** mirror `RecordGreeks`: when the kernel's intrinsic clamp fires (`intr > euro + prem`)
  the served price is intrinsic and the exercise Greeks go with it (Δ = −1 put / +1 call, everything else
  0, `exercised`); `floored` = the clamp fired although S_int > B(τ = T) (the continuation side of the
  current edge: B_now = xmax e^{−√y[n−1]}, an exact node hit). The flags agree with the bundle's own clamp
  test along a spot ladder through the edge (`ExerciseRegionServesIntrinsicWithTheExerciseGreeks`).
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

(filled from the gtest run)

## 4. Error tables vs the Reference bundle (i) and the Richardson / second-family reference (ii)

(filled from `american/family_greeks_error/*`)

## 5. µs table (SPY 23 d, avx2, unqualified, one run)

(filled from `american/spy23_family_greeks/*`, `american/family_chain/*`, `american/family_shock/*`)

## 6. Gates

(filled)

## 7. What L3h / L2i must assume

1. **The calls** (api/pricing/american.hpp, the block after `AlSigmaFamily`; all opt-in, nothing default
   constructs a family): `family_greeks(fam, S, K, σ, mask)` → `AlSigmaFamilyGreeks` {price, delta, gamma,
   vega, theta, rho, phi, vanna, volga, charm, status, exercised, floored, boundary_term_included};
   `family_price_and_vega(fam, S, K, σ)` → {price, vega, status}; `family_chain_greeks(put*, call*, S,
   K[], σ[], side[], mask, columns, status[])` → Status (InvalidArgument on a length mismatch, nothing
   written; per-row outcomes in status[]); `family_shock_grid(fam, S, K, σ, spot_pct[], vol_bump[],
   mask, columns, status[])` → Status, cell (i, j) at i · n_vol + j; `family_detach(fam)` →
   `AlSigmaFamilyDetached` with `price` / `greeks` / `price_and_vega`.
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
   exercise Greeks are served (flags above); rho / phi are frozen-boundary — their boundary term is the
   measured gap of §4 (PM policy pending; if ruled insufficient, a second family at r ± h is two builds
   per (expiry, side) ≈ 2.5 ms); `boundary_term_included` is true for vega / vanna / volga only.
5. **Column contract** (`AlSigmaFamilyGreekColumns`): a column is written iff its span is non-empty, and
   then its length must equal the row count; the mask decides what is COMPUTED, the spans what is STORED
   (an unrequested Greek with a span reads 0.0); `flags` bit 0 = exercised, bit 1 = floored, bit 2 =
   boundary_term_included. Spans are caller-owned; the entries allocate nothing.
6. **Shock axes**: spot (the homogeneity rescale, exact) and absolute vol bumps (inside the box);
   dt / dr / dq are not family axes — a time or rate shock needs another geometry (and family).
7. **Cost per call**: §5 — the µs line of the lane report; price-only = `price()`.
8. **detach() exists** (`family_detach`); ≈ 5.8 KB per (expiry, side) copy; the geometry (≈ 90 KB) can be
   released after it.
