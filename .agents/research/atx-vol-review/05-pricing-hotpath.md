# atx-vol Pricing Hot Path Audit — Portfolio Pricing + Greeks + SIMD dispatch

Scope: the per-bar reprice+risk loop. `PortfolioPricer` (portfolio_pricer.{hpp,cpp}),
`PricedSurface::evaluate_batch`/`evaluate_resolved` (priced_surface.cpp), the prepared
substrate, the pricing executor, and how they dispatch (or fail to dispatch) into the
SIMD batch kernels. READ-ONLY audit; no code changed. Bias: performance + "code plumbed
but not wired in."

Severity legend: Critical (correctness/crash), High (large steady-state waste on the hot
loop), Medium (meaningful but bounded/opt-in), Low (cleanup/scaling-tail).

---

## TOP 5

1. **[High] FullGreeks risk loop is scalar per-contract; the laned AVX2 American-greeks
   kernel is compiled but never dispatched from the portfolio path.**
   `priced_surface.cpp:1009-1036` — when `want_greeks`, `evaluate_batch` falls out of its
   vectorized price-only arm into a per-entry scalar loop calling `evaluate_resolved →
   greeks_resolved → american_greeks_fd/al` one contract at a time.
   `american_greeks_batch` / `american_put_greeks_batch` / `american_put_greeks_batch_avx2`
   (american_batch.cpp:257, american_boundary_batch.cpp:161, american_greeks_avx2.cpp:56)
   have callers ONLY in tests/bench. Fix: route the FullGreeks group through
   `american_greeks_batch` (4-wide boundary solve/pack). Doc claims ~4× laned vs scalar
   full bundle (laned-greeks.md:55).

2. **[High] First-order / hedge greek tier ("L4") is NOT wired end-to-end — plumbed enums
   are ignored.** `PriceFieldMask` is binary {None,Marks,Greeks,FullGreeks}
   (portfolio_pricer.hpp:244-249) — no first-order bit. `EvalField::FirstOrder` vs
   `SecondOrder` collapse to the same full bundle in `evaluate_resolved`
   (priced_surface.cpp:794-810). The K4 solve-skip selectors
   `american_greeks_al(...,need_vega,need_rho,need_charm)` (american.hpp:415-425) are never
   forwarded — `greeks_resolved` calls it with defaults (all true) (priced_surface.cpp:638).
   A delta-only cadence therefore pays the full 5-solve (analytic) / 17-solve (FD) bundle:
   **~80–94% of greek solve work wasted.** See GREEK TIER STATUS below.

3. **[High] `EvalField::Delta`/`Vega` selective primitives + `american_delta` (1–2 solves)
   are complete at the surface layer but explicitly DEFERRED — no portfolio caller.**
   priced_surface.hpp:245-249 ("Wiring portfolio-level callers … is deferred to WP9").
   `evaluate_resolved` honors them (priced_surface.cpp:824-847) and `american_delta` costs
   1–2 boundary solves vs 17 for the FD bundle (american.hpp:427-437), but nothing in
   `PortfolioPricer` ever requests `EF::Delta`. A delta-hedge backtest cannot reach the
   cheap delta path.

4. **[Medium] Returning `price()` / `pnl_explain()` wrappers allocate a fresh workspace and
   rebuild the PreparedPortfolio every call.** portfolio_pricer.cpp:1181, 1670 construct a
   one-shot local `PortfolioWorkspace ws;`. A per-bar backtest on the convenience (returning)
   API silently loses ALL the T5/T6 cross-snapshot reuse (frame columns re-`resize`d +
   `PreparedPortfolio::create` re-run every bar). Only `price_into`/`price_totals` with a
   retained `ws` are allocation-free.

5. **[Medium] Adjoint FullGreeks route is scalar per-contract AND re-resolves per contract,
   discarding the batch's ladder reuse; and the P&L path drops `resolved_price_isa`.**
   portfolio_pricer.cpp:712-742 — after the IV-only `evaluate_batch`, the adjoint loop calls
   `surf->resolve(kcol[p],tcol[p])` (line 725) + `american_greeks_adjoint` (line 727) per
   contract, re-doing the T-bracket/forward interp the batch already computed. Separately the
   P&L solve hardcodes `simd::SimdIsa::Auto` (portfolio_pricer.cpp:1288,1298,1307), ignoring
   `opts.resolved_price_isa`. (Adjoint is opt-in with no production caller today, so effect
   is latent.)

---

## GREEK TIER STATUS (key deliverable)

### Marks-only / prices_only: **HONORED** — 2nd- and 1st-order work is genuinely skipped.
Proof chain:
- `price_into` derives `want_greeks = has_field(fields, PriceFieldMask::Greeks)`
  (portfolio_pricer.cpp:1012). `PriceOptions::prices_only` → `PriceFieldMask::Marks`
  (portfolio_pricer.cpp:1148-1149).
- `solve_uniques` sets the surface request:
  `fields = adjoint ? Iv : (want_greeks ? Iv|Price|FirstOrder|SecondOrder : Iv|Price)`
  (portfolio_pricer.cpp:623-626). Under Marks the greek scratch `b_greeks` is sized 0
  (line 637) and never populated.
- In `evaluate_batch`, a Marks (`Price`, not greeks) group on the cold route takes the
  vectorized `american_price_batch_resolved` arm (priced_surface.cpp:932-1008).
- Scatter/reduce never touch the 8 greek columns under Marks (portfolio_pricer.cpp:839-852,
  866-869). **Verdict: a Marks request pays no greek solve. Correct and efficient.**

### First-order-only (delta/hedge): **IGNORED / NOT WIRED.** Three independent gaps:
1. **API can't express it.** `PriceFieldMask` has no first-order/hedge bit
   (portfolio_pricer.hpp:244-249). A caller needing only delta must request `FullGreeks`.
2. **Surface FirstOrder/SecondOrder split is a no-op.** `evaluate_resolved` computes
   `want_greeks = has(FirstOrder) || has(SecondOrder)` and routes BOTH to the full bundle
   `greeks_resolved` (priced_surface.cpp:794-810); `greeks_resolved` →
   `american_greeks_fd`/`american_greeks_al` computes ALL 8 greeks
   (priced_surface.cpp:637-642). Requesting `EF::FirstOrder` alone still computes
   vanna/volga/charm.
3. **K4 selectors exist but are never forwarded.** `american_greeks_al` gained
   `need_vega/need_rho/need_charm` (default all true) that skip σ±/r± solves — proven by the
   `BoundarySolves` ledger: hedge{delta}=1, {delta,vega}=3, full=5 (laned-greeks.md:69-74,
   american.hpp:415-425). But `greeks_resolved` (priced_surface.cpp:638) calls it with the
   defaults — the `EvalField → need_*` map the seam doc says L4 must add (laned-greeks.md:86-101)
   does not exist. The seam doc's own CAVEAT (laned-greeks.md:94-96): "today
   `want_greeks = has(FirstOrder)∨has(SecondOrder)` … so a bare `EF::Delta` does NOT
   currently trigger the greeks path" — i.e. the last-mile wiring is unfinished.

The doc claims "K3+K4 SHIPPED (dark)" (laned-greeks.md:16) but the L4 loop-side wiring in
`portfolio_pricer.cpp`/`priced_surface.cpp` (the mask-narrowing frame + `EvalField→need_*`
map) is verifiably absent. The kernel primitives are ready; the portfolio never calls them
in reduced-tier form.

**Waste quantification.** For a delta-hedge-only cadence the portfolio computes the full
bundle: `american_greeks_fd` ≈ 17 boundary solves (american.hpp:428-429); `american_greeks_al`
full = 5; hedge{delta} (K4) = 1; `american_delta` = 1–2. So delta-only risk pays 5×
(analytic route the backtest uses) to 17× (default FD route) the necessary boundary solves —
~80–94% of greek solve cost is discardable if the tier were wired. Doc: hedge first-order
~6× the scalar full bundle throughput (laned-greeks.md:55).

### P&L base bundle: legitimately full.
`solve_pnl_uniques` requests `Iv|Price|FirstOrder|SecondOrder` (portfolio_pricer.cpp:1287)
because the Taylor decomposition consumes all 8 coefficients (delta..charm,
portfolio_pricer.cpp:1399-1407). Not a defect — but confirms there is no tiering anywhere.

---

## UNWIRED KERNELS / KNOBS (the "code not wired in" ledger)

| Item | Where defined | State | Only reached by |
|---|---|---|---|
| `american_greeks_batch` (SoA laned American greeks) | american_batch.cpp:257 | compiled, complete | tests/bench only — NO portfolio/surface caller |
| `american_put_greeks_batch` + `_avx2` (4-wide AL greeks) | american_boundary_batch.cpp:161, american_greeks_avx2.cpp:56 | compiled, parity-gated | `american_greeks_batch` (unused) + tests/bench |
| `EvalField::Delta` / `EvalField::Vega` + SoA delta/vega columns | priced_surface.hpp:242-249 | honored in `evaluate_resolved`/`evaluate_batch` | tests only — explicitly DEFERRED (WP9) |
| `EvalField::FirstOrder` vs `SecondOrder` distinction | priced_surface.hpp:240-241 | dead: both → full bundle (priced_surface.cpp:794-810) | — |
| `american_greeks_al(need_vega,need_rho,need_charm)` K4 selectors | american.hpp:424-425 | callable, ledger-proven | `american_greeks_batch` only; `greeks_resolved` passes defaults |
| `american_delta` cheap route (1–2 solves) | american.hpp:438-441 | used by strike-solver, NOT portfolio | strike-from-delta solver |
| `kShipAvx2Boundary=false` (marks AVX2 dark) | american_boundary_batch.cpp:74 | dark by default | portfolio only via `resolved_price_isa=ForceAvx2` (portfolio_pricer.cpp:781) |
| `kShipAvx2Greeks=false` (greeks AVX2 dark) | american_boundary_batch.cpp:129 | dark AND unwired | nothing on the portfolio path |
| `PriceOptions::resolved_price_isa` | portfolio_pricer.hpp:538-542 | affects Marks batch only; dropped entirely on the P&L path | portfolio_pricer.cpp:781, evaluate_batch |

Net: in the **default (Auto ISA) production config the portfolio hot path uses NO AVX2
kernel** for American marks or greeks. Marks AVX2 is dark-shipped (reachable only via
`ForceAvx2`); American-greeks AVX2 is both dark AND never dispatched. The AVX2 wins reported
in docs/simd_fastpath.md (1.6–1.95×) are Black-76 European kernels
(`black76_greeks_batch`/`greeks_batch_avx2`, used in fitting/IV), not the American portfolio
greeks path.

---

## PERFORMANCE FINDINGS (by severity)

### High

**H1. Scalar per-contract greeks on the risk hot loop.** (priced_surface.cpp:1009-1036)
The `evaluate_batch` greeks arm is a scalar `for (e in run)` calling `evaluate_resolved` →
`american_greeks_fd/al` per contract. The T-bracket/carry ladder is reused for the resolve,
but each greek bundle is an independent scalar Andersen-Lake solve set. The 4-wide laned
bundle that would amortize the boundary solve across a pack exists (american_greeks_avx2.cpp)
and is unreachable here. Impact: the per-bar risk fan-out is the single most expensive part of
a backtest and it runs one contract wide. Fix: dispatch the FullGreeks group through
`american_greeks_batch` with `kernel.isa` (and flip `kShipAvx2Greeks` after A/B).

**H2. First-order tier not wired** — see GREEK TIER STATUS. Fix: add a first-order
`PriceFieldMask` bit (or thread `GreekNeeds`), map it to `EF::Delta`/`EF::Delta|Vega`, extend
`want_greeks` gating, and add the `EvalField → need_vega/need_rho/need_charm` map in
`greeks_resolved`.

**H3. Cheap `american_delta` path deferred** — see TOP 5 #3. Fix: request `EF::Delta` for the
hedge cadence; the surface already routes it to `delta_resolved` (priced_surface.cpp:824-834).

### Medium

**M1. Convenience wrappers re-allocate + rebuild per call.** portfolio_pricer.cpp:1181, 1670.
A backtest calling `price()`/`pnl_explain()` (not the `_into` variants) rebuilds
`PreparedPortfolio` (portfolio_pricer.cpp:916; `PreparedPortfolio::create` does a
`stable_sort` over uniques + tile partition, prepared_portfolio.cpp:62-134) and re-resizes
14/19 frame columns every bar. Fix: document the footgun louder or retain a workspace inside
the pricer for the returning API.

**M2. Adjoint route scalar + redundant per-contract resolve.** portfolio_pricer.cpp:712-742.
Re-`resolve` per contract (line 725) duplicates the batch's forward/bracket work; the greek
call is scalar. Opt-in today (no production caller — laned-greeks.md:76-84), so latent.

**M3. P&L path ignores `resolved_price_isa`.** portfolio_pricer.cpp:1288, 1298, 1307 hardcode
`SimdIsa::Auto`; the shifted-price leg (`EF::Price`, line 1307) could ride the same marks AVX2
fast path the price path can, but is pinned. Inconsistent knob semantics.

**M4. Fat AoS `ContractPx` gathered per position in scatter/reduce.**
portfolio_pricer.cpp:389-395 — `ContractPx` is `fair_value + AmericanGreeks(9 doubles) + iv +
vega_slope + status` ≈ 13 doubles (~104 B). `scatter_rows`/`reduce_price_totals` gather
`px[pf.contract_ix(i)]` per position (portfolio_pricer.cpp:821, 873) — an indirect gather of a
full cache line per row even when only `pv`/`delta` are consumed downstream. For a book where
positions map to scattered uniques this is poor locality. Consider SoA per-column unique
results so a Marks reduce touches only `fair_value`+`status`. Bounded (dedup keeps uniques
small), hence Medium.

### Low

**L1. Legacy VolSurface paths are O(n²) and fully scalar.** portfolio_price.cpp,
portfolio_greeks.cpp, portfolio_risk.cpp are the old `portfolio.hpp` C-port (Black-76 +
correction cache), separate from `PortfolioPricer`. `portfolio_risk.cpp` linear-scans groups
in `bucket()` (line 510-518), `find_or_build_ctx` (line 349-357), and the aggregate `bucket`
(line 603) — O(groups²)/O(ctx²) for many underlyings. Only relevant if any live caller still
routes through these; the new surface path supersedes them.

**L2. `EvalField::FirstOrder`/`SecondOrder` dead distinction** — the two bits produce identical
work (priced_surface.cpp:794-810). Either honor `SecondOrder`-omitted (skip vanna/volga/charm
stencils) or collapse to one bit.

---

## CORRECTNESS SPOT-CHECKS (no defects found)

- Masked-lane / degenerate handling: `evaluate_batch` poisons `iv=NaN` on rejected lanes to
  stay bit-identical to per-contract `evaluate` (priced_surface.cpp:977-981); AVX2 marks pack
  patches non-finite/irregular lanes through scalar (american_batch.cpp:236-252). Sound.
- Determinism across thread counts: disjoint slot writes + serial fixed-order reduction
  (portfolio_pricer.cpp:856-896; pricing_executor.cpp:245-267 determinism proof). Sound.
- Adjoint vs FD: adjoint claims only genuine early-exercise puts, else falls back to
  `american_greeks_fd` (adjoint_greeks.cpp:473-486); mark == AL price. Sound.
- `greeks().price == fair_value()` invariant preserved on the cold FD path
  (priced_surface.hpp:29-41).

---

## SOTA FEATURE GAPS

- **First-order / hedge tier** (H2/H3) — the biggest single win; kernel-ready, loop-unwired.
- **Laned American greeks dispatch** (H1) + flip the two dark ship gates after A/B.
- **Incremental / dirty-leg repricing:** none. `ensure_prepared` rebuilds on any logical-book
  change (portfolio_pricer.cpp:907-925); there is base-risk reuse across price→P&L
  (portfolio_pricer.cpp:1540-1547) and FullGreek seed reuse (stage_full_greek_seeds), but no
  per-leg dirty tracking within a bar.
- **Cross-symbol batched greeks:** groups are per-(uid,side) (prepared_portfolio.cpp:100-134);
  the greek solve never packs across uids even though the AL boundary math is uid-agnostic once
  resolved. A cross-uid pack would fill 4-lane packs on thin books.
- **FMA/AVX-512:** kernels are AVX2 4-lane f64 only (CMakeLists.txt:118-133); no AVX-512 8-lane
  path.
- **P&L path** always full-bundle (no reduced-tier P&L), and drops the ISA knob (M3).
