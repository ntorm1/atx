# Seam: laned greeks bundle (K3) + first-order tier (K4)

Status: **published seam contract** (WS-K owns; WS-L wires L4 against it). This doc is
published EARLY, before K3/K4 land, because the loop workstream (L4 tier wiring) blocks on
the entry-point signatures + mask semantics (sprint §9.6). It states what EXISTS today,
what K3/K4 will ADD, and the exact contract L4 codes against so the two can proceed in
parallel behind a stable interface.

Owner TUs (kernel): `src/american_batch.cpp`, `src/priced_surface.cpp` (batch arms),
`src/detail/adjoint_greeks.{hpp,cpp}`, new laned stencil kernel, `american_batch.hpp`
(the `GreekFieldMask` + entry decls). Loop wires through `portfolio_pricer.{hpp,cpp}` — it
does NOT edit the kernel TUs; it consumes the entry points below.

---

## STATUS — K3 + K4 SHIPPED (dark), what L4 wires against

The seam below is now IMPLEMENTED (SHAs in `.superpowers/sdd/sw-solve-wall/reports/
kernel-stage2.md`); this section is the authoritative entry-point + mask contract; the
sections after it are the original design rationale.

- **Laned kernel:** `src/simd/american_greeks_avx2.cpp`
  `detail::american_put_greeks_batch_avx2(...)` — the analytic PUT bundle 4-wide (5
  boundary solves/pack: base, σ±, r±), parity-gated vs scalar `american_greeks_al`
  (economic gate; transcendental-level deviations, see report). Rides the shared K2
  solve/price primitives (`american_boundary_avx2_kernel.hpp`).
- **Dispatch (what L4 calls, kernel-owned):**
  `simd::american_put_greeks_batch(S,K,T,σ,r,q, n, opts, AmericanGreeks* out, isa,
  need_vega=true, need_rho=true, need_charm=true)` — ISA-selected (ForceScalar = the
  `american_greeks_al` oracle; ForceAvx2/Auto-when-shipped = laned + scalar patch for
  non-early-exercise / non-finite lanes). Returns `SimdRoute`.
- **SoA surface (kernel-owned):** `american_greeks_batch(in, GreekFieldMask fields, ...)`
  already routes analytic PUT lanes through the dispatch when `simd::avx2_greeks_selected`
  (Auto respects the dark `kShipAvx2Greeks` gate); CALL lanes stay on the scalar analytic
  fan. **L4 does not change this function — it sets `kernel.isa` and the mask.**
- **Ship gate:** `kShipAvx2Greeks` (false, dark) in `american_boundary_batch.cpp`, mirror
  of `kShipAvx2Boundary`. The PM flips it after a quiet-window A/B. L4 wires the tier; it
  does not flip the gate.

### K4 first-order tier — the mask IS the tier (no separate function)

`need_vega`/`need_rho`/`need_charm` skip whole boundary solves:

| L4 request (`GreekFieldMask`) | selectors | solves/pack | greeks returned |
|---|---|---|---|
| **hedge** `Delta` (+`Gamma`,`Theta`,`Price`) | vega=rho=charm=**false** | **1** (base) | delta, gamma, theta, price |
| **risk** `Delta\|Vega` (+`Price`) | vega=**true**, rho=charm=false | **3** (base, σ±) | + vega, volga, vanna |
| + `Rho` | rho=**true** | +2 (r±) | + rho |
| **pnl-explain** `All` | all true | **5** | full 8-greek bundle |

`american_greeks_batch` derives the selectors from `fields` (`need_vega = Vega∨Volga∨
Vanna`, `need_rho = Rho`, `need_charm = Charm`). Guarantee (test
`FirstOrderMaskBitMatchesFullBundle`): the columns a reduced request returns are
**bit-identical** to the full-bundle run — the skipped solves never fed them. Measured
(PROVISIONAL): hedge first-order ~3.4× the full laned bundle, ~6× the scalar full bundle.

**Vega self-consistency guard — resolved, no replacement needed in this path.** In the
analytic bundle `volga = (v_σ+ − 2v₀ + v_σ−)/h²` is formed from the SAME σ± boundary
solves as `vega`, so volga costs **zero extra solves** and there is no volga-only re-solve
to drop (unlike the adjoint route's 2 cold σ± volga re-solves, `adjoint_greeks.cpp:374-403`
— a separate optimization L4 avoids by not requesting Volga through the adjoint path). The
first-order tier omits vega AND volga together (drops the σ± solves entirely), so there is
no "vega without its volga cross-check" state to guard. A tangent-vs-Richardson vega
cross-check would only matter for a hypothetical "vega at fewer-than-σ± solves" tier, which
this design does not create.

---

## 0. What exists today (the substrate L4 already has)

- **`GreekFieldMask`** (`american_batch.hpp:215`) — per-greek granular selector:
  `Delta | Gamma | Vega | Theta | Rho | Vanna | Volga | Charm | Price`, plus `AllGreeks`,
  `All`, `None`, and `|`/`&`/`has_field`. This is the granular request mask K4 needs; it
  ALREADY has per-greek bits (the coarse `PriceFieldMask{Marks,Greeks,FullGreeks}` in
  `portfolio_pricer.hpp:244` is the loop-side mask that must be refined to map onto it).
- **`american_greeks_batch(in, fields, greeks, kernel, ws)`** (`american_batch.hpp:277`) —
  the batch greeks entry. Today it is **strictly scalar per lane**: it fans
  `american_greeks_al` / `american_greeks_fd` across `kernel.executor`, writes only the
  `fields` columns via `write_masked`, and stamps `ws.lane_route[i] = Scalar` ("scalar
  Greek stencil (honest)", american_batch.cpp:293). **K3 replaces the per-lane body with a
  laned kernel; the signature and the `fields`/`ws` contract are unchanged.**
- **`write_masked`** (american_batch.cpp:64) — writes only requested `GreekFieldMask`
  columns into the `GreeksBatchSoA`; unselected columns are never touched. K3/K4 keep this.
- **The marks substrate K3 rides on** — `simd::american_put_boundary_batch(..., opts, isa)`
  (the K2 4-lane pack): one laned boundary solve per 4-pack of genuine early-exercise
  lanes, scalar patch for the rest. K2 validated it at the ql_fast marks rung
  (`AlOpts{7,8,2,tol,32}`) within the economic gate (see kernel-stage1.md).

---

## 1. K3 laned greeks — the entry-point contract

**Signature (unchanged from today):**
```cpp
Status american_greeks_batch(const AmericanBatchInput& in, GreekFieldMask fields,
                             simd::GreeksBatchSoA& greeks, PricingKernel& kernel,
                             PricingWorkspace& ws);
```
K3 is an **internal** rewrite of the body — L4 does not change its call. What changes is the
per-lane scalar fan becomes a laned pack pipeline:

1. **One laned boundary solve per 4-pack** — reuse the K2 pack (SoA<4> transpose, 4-wide
   BAW seed, lockstep sweeps, active-mask lane freezing). The boundary is **spot-independent**
   (the AL boundary depends on K,T,r,q,σ, not S), so it is solved ONCE per pack.
2. **Spot-stencil greeks ride FREE per lane** — delta, gamma, speed, theta, charm come from
   laned spot-stencil re-prices against the SAME converged boundary (no re-solve). These are
   the free greeks.
3. **σ/rate greeks via laned BUMPED boundaries** — vega, rho, vanna need bumped-boundary
   solves; do them 4-wide, **warm-seeded from the base-lane boundary** (the laned analogue
   of the scalar `al_solve_put_boundary_warm` pattern). Only computed if `fields` selects
   them (see §2).
4. **volga per the K4 policy** (§3) — the two cold σ± re-solves are the expensive tail;
   K4 governs whether they run.
5. **Guard-fallback lanes patch to scalar** — a lane that hits a guard/degenerate/non-American
   corner (the ~16.5% guard-fallback rate, sprint §11.5) is patched via the exact scalar
   `american_greeks_{al,fd}` and stamped `ws.lane_route[i] = Scalar`; the other 3 lanes are
   NOT serialized (active-mask patch-out, the K2 idiom). L4 reads `ws.lane_route_view()` to
   see per-lane routing; `ws.lane_status_view()` for `Ok`/`Unsupported`.

**Parity contract (what L4 can rely on):**
- Laned greeks vs the scalar FD reference: **economic gate per sprint §3** (delta/gamma
  bit-identical where the scalar path already claims it; the differenced/bumped greeks
  within the documented FD-vs-laned tolerance). The scalar path stays the oracle and the
  fallback. Any epsilon is documented at the point of change + in the K5 report.
- **Public output order preserved** (`greeks` SoA column `[i]` pairs with input lane `i`),
  exactly as the marks batch preserves order.
- Determinism across worker counts preserved (per-lane disjoint writes; pack grouping is a
  pure function of the input, not of thread scheduling).

---

## 2. Mask semantics — the L4 request contract

L4 requests exactly the columns the caller consumes, via `GreekFieldMask`. The kernel
computes only the work those columns require (this is the whole point of the tier):

| Caller need | `GreekFieldMask` request | Kernel work (K3/K4) |
|---|---|---|
| **Hedge** (delta only) | `Delta` (+ `Price` if the mark is also needed) | boundary solve + spot stencil. **No σ/rate bumped solves.** The first-order path (§3). |
| **Risk / entry frictions** (delta + vega) | `Delta \| Vega` (+`Price`) | boundary + spot stencil + ONE laned σ-bumped boundary for vega. No volga/vanna. |
| **PnL explain** (full bundle) | `All` | the full laned bundle incl. vanna/volga (K4 volga policy applies). |

Rules the kernel guarantees:
- `has_field(fields, X)` false ⇒ column `X` is **never written** and its **work is skipped**
  (no bumped solve for an unrequested σ/rate greek). This is the cost lever: the hedge path
  stops paying for 8 greeks when it consumes 1.
- `None` ⇒ no-op success. `Price` alone ⇒ marks only (routes to the K2 marks batch).
- Requesting a spot-stencil greek (`Delta/Gamma/Theta/Charm`) is **free** given any solve —
  the boundary is already there. Requesting `Vega/Rho/Vanna` costs a laned bumped solve.
  Requesting `Volga` costs the K4-governed σ± re-solves.

L4's job on the loop side: refine the coarse `PriceFieldMask` (Marks/Greeks/FullGreeks) so
the execute risk frame requests a **first-order** subset (hedge → `Delta`, entry frictions →
`Delta|Vega`) while the recorded pnl-explain row keeps `All`. The mapping
`PriceFieldMask → GreekFieldMask` lives on the loop side (portfolio_pricer, loop-owned); the
kernel consumes only `GreekFieldMask`.

---

## 3. K4 first-order tier — vega-guard redesign

K4 builds the `first_order_only` path the reviewer priced at ~2–2.5× (sprint §4 K4):
- **Drop volga's 2 cold σ± re-solves + vanna's premiums** when `fields` does not select
  `Volga`/`Vanna` (`adjoint_greeks.cpp:374-403` is the current volga σ± cost).
- **Replace the vega self-consistency guard volga provided** — research options: a
  tangent-vs-Richardson cross-check, or a laned σ± at the fast preset (cheaper than the two
  cold accurate re-solves). The chosen guard is documented at the point of use.
- The entry point does not change: K4 is expressed entirely through `GreekFieldMask` (a
  request that omits `Volga`/`Vanna` IS the first-order request). L4 does not need a separate
  function — it narrows the mask.

**Stamp/tier composition note for L1:** L4 also needs adjoint/first-order **stamp support**
so the base-risk reuse (L1) composes (`portfolio_pricer.cpp:1074-1087` today has no
base-risk stamp on the adjoint route). That plumbing is loop-owned; the kernel guarantees
the laned greeks entry is a pure function of `(in, fields)` so a stamp keyed on the frame is
sound.

---

## 4. Stability guarantees for L4 (what will NOT change under you)

- `american_greeks_batch` signature + the `GreekFieldMask` bit layout are **frozen**; K3/K4
  add no required positional args (any new knob rides on `PricingKernel`, which L4 already
  passes).
- `write_masked` column-selection semantics are frozen (unselected columns untouched).
- `ws.lane_route_view()` / `ws.lane_status_view()` remain the per-lane routing/health seam.
- The scalar path remains the oracle + fallback; `SimdIsa::ForceScalar` always reproduces
  the bit-identical scalar bundle, so L4 can A/B laned-vs-scalar for its own parity gate.
- Ship gating: the laned greeks path, like the K2 marks batch, lands behind a ship flag that
  the PM flips after a quiet-window A/B — L4 wires the tier request; it does not own the flip.

---

## 5. Cross-references

- K2 marks substrate + ship discipline: `.superpowers/sdd/sw-solve-wall/reports/kernel-stage1.md`.
- Preset tiers the greeks bundle rides (`fast_p32` for FD greeks): `docs/al-preset-ladder.md` §5.
- Sprint tasks: K3 (laned bundle), K4 (first-order + vega guard), L4 (tier wiring) —
  `atx-vol/sprints/2026-07-18-atx-vol-solve-wall-sota-sprint.md` §4.
