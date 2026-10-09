# atx-vol Kernel-Coupled Integration Sprint (AAD, Surrogate Cache, Backtest Wiring, Legacy Retirement)

**Date:** 2026-07-11

**Status:** planned; execution gated on 2026-07-09 sprint progress per §2. Not started.

**Provenance:** split out of `2026-07-10-atx-vol-full-stack-competitiveness-sprint.md`
(rev 2) so that the 07-10 sprint can run in parallel with the in-flight
`2026-07-09-american-pricing-portfolio-throughput-sprint.md` (executing P2-2b at split
time). Every package here either **depends on** a 07-09 deliverable or **overlaps** it
(same files, same deliverable, or it shifts the numbers 07-09 gates against). Package
IDs retain their 07-10 numbering (`C0.1`, `C1.x`, `C4`–`C7`) for traceability.

**Scope:** AAD/adjoint Greeks + AoSoA AVX2 integration (C4), surrogate-cache
productization on the canonical path (C5), backtest wiring onto the persistent/stateful
infrastructure (C1.B = old C1.1–C1.3), the analytic-AL Greeks default flip (C1.4),
portfolio-baseline repair (C0.1), correlation Greeks (C6.2), and retirement of the
deprecated European portfolio stack (C7).

**Northstar:** unchanged from 07-10 — the fastest and most complete C++ analytics
library for American listed equity options, at Vola-Dynamics-class surface quality.

---

## 1. Why these items were split out (the conflict map)

07-09 remaining ledger at split time (executing P2-2b): P2-2b boundary hoist (in
flight; the QD+ seed spike concluded **kill — keep BAW**), P2-3 warm state, P2-5 σ-axis
Chebyshev boundary interpolant, P2-X implicit-differentiation spike, P3 AVX2 AoSoA
kernel (P3.1–P3.5), P4 carry-aware `CorrectionCacheV2` (P4.1–P4.5), P5 stateful
portfolio/backtest (P5.1–P5.4), P6 LTO/PGO.

| 07-10 item | 07-09 counterpart | Conflict |
|---|---|---|
| C0.1 baseline repair | P0-3 baselines; P5 | suspected root cause (per-call `Portfolio`/`Pricer` rebuild) is removed by the backtest wiring; re-take after, not before |
| C1.1–C1.3 backtest wiring | **P5.1–P5.4** | same file (`backtest.cpp`), same deliverable — P5.2's `advance()` subsumes C1.1's pricer reuse |
| C1.4 analytic-AL default flip | P2-2 (done), P2-5 (pending) | flipping the default shifts every portfolio-Greeks number 07-09's baselines gate against; P2-5 rewires the analytic route the flag selects |
| C4.1 AoSoA<4> AVX2 primal | **P3.2** | identical deliverable (07-10 text: "the deferred P3.2"); P3.1/P3.3 supply its ISA dispatch and Φ bakeoff |
| C4.2 IFT adjoints on the ALO boundary | **P2-X** | C4.2 productizes the P2-X spike; needs the residual evaluator that P2-2b is currently restructuring |
| C4.3/C4.4 adjoint bundle, column-major Greeks | P3.4 | P3.4 already makes SoA the preferred Greeks output — one layout, not two |
| C5.1/C5.4 carry-aware cache + independent scoring | **P4.1–P4.5** | near-verbatim duplicate (carry-aware build, archive attach, `route=ColdFallback`, PDE-theta semantics, independent cold+PDE scoring) |
| C6.2 correlation Greeks | — | depends on C4.3 |
| C7 legacy retirement | — | gated on C5.2 (surrogate re-home), which is gated on the P4/C5 cache |

**Dedup rule:** where a 07-10 package duplicated a 07-09 package, the version here is
the *delta/integration* on top of the 07-09 deliverable — never a second parallel
build. If 07-09 kills or descopes a counterpart, the item here reverts to its full
original 07-10 definition (recorded per package below).

---

## 2. Dependency gates

| Package | Starts after (07-09) | Also needs (07-10) |
|---|---|---|
| C7.1 caller audit | none (read-only, may start any time) | — |
| C1.B backtest wiring | P5 landed (else execute original C1.1–C1.3 verbatim) | — |
| C0.1 baseline repair | C1.B done (root cause removed) | C0.2 baselines |
| C1.4 analytic-AL default | P2-5 shipped or killed; C0.1 re-take done | — |
| C4.1 primal integration | P3.1–P3.3 | — |
| C4.2 IFT adjoints | P2-2b landed (residual evaluator stable); P2-X spike verdict | — |
| C4.3/C4.4 bundle + layout | C4.1, C4.2; P3.4 | — |
| C5 cache delta + re-home | P4 | — |
| C6.2 correlation Greeks | C4.3 | C6.1 term structure |
| C7.2/C7.3 retirement | C5.2 | C3.3 attribution |

---

## 3. Targets (moved from 07-10 §1; same pinned i7-1260P host, gated by the 07-10 §9 accuracy gates)

| Metric | Measured at split | Ship target | Stretch |
|---|---:|---:|---:|
| Portfolio full Greeks (u2688, t8) | 6,469 uniques/s | **≥25k uniques/s** | 60k/s |
| Portfolio full Greeks, single unique (cold) | 750 µs | **≤150 µs** (analytic+AoSoA) | ≤60 µs |
| Adjoint full-Greeks bundle cost | n/a (none) | **≤2.5× one price** | ≤1.5× (Giles-Glasserman bound is ~4×) |
| Backtest step (fixed 2,688-unique book) | rebuild+cold every step | **≥5× current** | ≥15× |
| Cached price (carry-aware, on `PricedSurface`) | unavailable | 500k contracts/s/core | 1M/s |
| Cached full Greeks | unavailable | 125k/s/core | 250k/s |
| Dispersion correlation Greeks | none | FD parity via AAD, ≤4× cost regardless of #names | — |

---

## 4. Work packages

### C0.1 — Repair the corrupt portfolio baseline rows
**Est.** 0.5 d · **Risk** low · **Gate:** after C1.B (removes the suspected root cause).

Regenerate `port/price/greeks/u2688/*` on a quiesced host; the t1 row must be ≤ its
own kernel floor × (measured overhead), not 11× it. The audit's leading hypothesis for
the 23.1 s @ CV 40% row (11× its own kernel floor, 15.7× its own t2) is the per-call
`Portfolio`/`Pricer` rebuild in the convenience `price()` wrapper — which C1.B/P5
removes. Re-take after that lands; if the pathology survives, root-cause it
(allocation storm / thread oversubscription) before gating anything on those rows.
(`portfolio_throughput_bench.cpp`; baseline JSON.)

**Acceptance:** t1 ≤ floor×overhead; CV ≤5%; `compare_baseline.py` gates the row (no
silent NOISY skip); the cached-vs-cold portfolio row (07-10 C0.2's deferred item) is
added once C5.1 exists.

### C1.B — Backtest onto the persistent/stateful path (old C1.1–C1.3)
**Est.** 2.5 d · **Risk** low–medium · **Gate:** 07-09 P5 landed (else execute the
original 07-10 C1.1–C1.3 text verbatim).

07-09 P5 delivers stable contract identity (P5.1), carried-forward step state + an
`advance()` API (P5.2), axis-state fusion (P5.3), and deterministic invalidation
(P5.4). This package is the *consumer-side* integration:

- **C1.B.1 (old C1.1)** — the backtest builds one `PortfolioPricer` +
  `PortfolioWorkspace` per book-identity cohort and reprices via
  `price_totals`/`pnl_totals` — or, where P5 shipped, steps via `advance()` and
  asserts the target-mark → next-base-mark bit-equality directly. The backtest
  consumes only `.total`, so drop per-row frames entirely. Key the workspace by
  cohort/book fingerprint to respect the ABA contract
  (`portfolio_pricer.hpp:296-310`). Removes `2+U` `Portfolio`/`Pricer` builds + frame
  allocations per step (`backtest.cpp:69-99,116-176,343-414,682-829`).
- **C1.B.2 (old C1.2)** — hoist the hedge overlay: price the whole book's deltas once
  via the reused pricer and index per-uid, or use `PricedSurface::delta` (~1–2 solves
  vs 17); turns `U` pricer-builds/step into 0 (`backtest.cpp:640-663`).
- **C1.B.3 (old C1.3)** — de-quadratic the ledgers: replace the O(n²) linear-scan
  ledgers (`shares_*`, `in_before/in_book`, `uid_of`, `add_uid`) with hash-indexed
  lookups (`backtest.cpp:441-464,547-562,278-291,626-633`).

**Acceptance:** Reference outputs bit-identical on all fit/price/PnL/NAV columns;
backtest steady state does **no** `Portfolio`/`Pricer` rebuild and no frame
allocation; ≥5× step throughput on the fixed 2,688-unique book; ledger ops scale
sub-quadratically; sanitizers green.

### C1.4 — Analytic-AL Greeks default at scale
**Est.** 0.5 d · **Risk** low · **Gate:** 07-09 P2-5 shipped or killed (it rewires the
analytic route the flag selects), and C0.1 re-taken so the flip does not invalidate
in-flight baseline comparisons.

Flip `PriceOptions::analytic_greeks` on for the greeks-at-scale path
(delta/gamma/vega/rho/vanna/volga bit-identical, only theta/charm differ vs FD,
documented at `portfolio_pricer.hpp:420`); ~3× on the bundle.

**Acceptance:** bit-identical δ/γ/vega/ρ/vanna/volga vs the FD route; theta/charm
within the 07-10 §9.2 gates; baseline JSON re-taken and gated after the flip.

### C4 — AAD Greeks-at-scale + AoSoA AVX2 integration *(the headline)*
**Est.** 7 d (was 10 d in 07-10 — C4.1's kernel is delivered by 07-09 P3) ·
**Risk** high · **Expected gain:** full-Greeks bundle at ≤2.5× one price; 2.5–3×/core
AVX2 on the primal.

- **C4.1 AoSoA<4> AVX2 American primal — integration only.** The kernel itself is
  07-09 **P3.2** (with P3.1 ISA dispatch and the P3.3 Φ/vector-math bakeoff). This
  item is the productization left over: wire the packed kernel into `priced_surface` /
  `PortfolioPricer` group execution, confirm the **1-ULP** Φ tier under FD
  differencing (07-10 §5.5: never the 3.5-ULP tier under differencing), per-lane
  scalar fallback policy on divergence/degenerate lanes, and the forced-scalar CI leg.
  *Reversion:* if 07-09 descopes P3, execute the full original 07-10 C4.1 build here.
- **C4.2 Implicit-function-theorem adjoints on the ALO boundary.** Productize the
  07-09 P2-X spike (its ship/kill report is an input, not a substitute). The exercise
  boundary is a converged fixed point `R(y;θ)=0`; differentiate the *converged point*
  (`∂y/∂θ = −(∂R/∂y)⁻¹ ∂R/∂θ`), never the iteration (07-10 §5.2). Expose a pure
  residual evaluator — coordinate with the P2-2b hoist, which restructures exactly
  this code; form `J=∂R/∂y` and `R_θ`; dense pivoted LU (n ≤ 12); propagate through
  the premium quadrature. This is the correct, memory-bounded alternative to taping
  the whole solver.
- **C4.3 Portfolio adjoint bundle + correlation Greeks.** Assemble the full
  first-order Greek bundle (and, where stable, 2nd-order vanna/volga) via one reverse
  sweep per unique contract at < ~4× the primal (Giles-Glasserman bound); extend to
  **all pairwise correlation sensitivities** for dispersion at ≤4× regardless of
  #names (Capriotti-Giles). Checkpoint the tape for the backward induction to bound
  memory.
- **C4.4 Per-axis pruning + column-major bundle.** Honor the `EvalField` split so a
  first-order-only or vega-only consumer skips 2nd-order stencils
  (`priced_surface.hpp:141-147`); store Greeks column-major to unblock SIMD
  reductions and marks-vs-Greeks masking (half-done via `PriceFieldMask`,
  `portfolio_pricer.hpp:189-210`). Align with 07-09 P3.4's SoA-preferred Greeks
  output — one layout, not two.

**Ship rule (hard, unchanged from 07-10):** AAD ships in Production only if it
(a) matches bump-and-revalue Greeks to the 07-10 §9.2 gates on the full OPRA + corner
grid, **and** (b) costs ≤2.5 price-equivalents for the full bundle. Otherwise AAD is
retained as an experiment and the analytic-AL + AoSoA-FD route ships. **A
frozen-boundary or tape-through-iteration approximation is never accepted.** The
scalar cold oracle remains.

**Acceptance:** scalar Reference bit-identical; AVX2 primal max price error ≤1e-8 USD
normal domain / ≤1e-3 USD stress, larger lanes fall back; AAD Greeks within §9.2 of
bump-revalue; AAD bundle ≤2.5× price; correlation Greeks validated against FD on a
small basket; no illegal instruction on forced-scalar CI; no scalar `erfc` inside the
packed loop (optimization-record + disassembly proof).

### C5 — Surrogate acceleration (carry-aware cache + Chebyshev reprice) *(gated)*
**Est.** 4 d (was 7 d in 07-10 — C5.1/C5.4 largely delivered by 07-09 P4) ·
**Risk** high · **Must precede C7** (re-homes the surrogate).

- **C5.1 Carry-aware `CorrectionCacheV2` on `PricedSurface` + archive — delta over
  P4.** 07-09 P4.1–P4.4 builds the carry-aware cache, the derivative tensors, the
  `PricedSurface` route with `ColdFallback`, and the archive attachment. This item is
  the residual verification: the archive payload covers
  version/dims/bounds/side/r-carry fingerprint/scheme/coefficient CRC/source-surface
  fingerprint (`surface_archive.hpp:196-211`); bounded-LRU lazy load; async post-fit
  production; PDE theta/charm + explicit r-derivative semantics (never the cache's
  total T-derivative as theta). *Reversion:* if P4 descopes any of this, build it
  here per the original 07-10 C5.1 text.
- **C5.2 Re-home the deprecated-stack surrogate.** The `CorrectionCache` Greek route
  used at `bulk.cpp:200` must become reachable from the canonical `PricedSurface`
  path before C7 deletes `bulk.cpp`. This is the dependency that orders C5 → C7.
- **C5.3 (optional, measured) Dynamic-Chebyshev American reprice for backtests.** For
  the fixed-model backtest reprice (07-10 §5.3), offline-precompute generalized
  moments then online backward induction emitting price+delta+gamma per step. Ship
  only if it beats the C5.1-cached ALO route on the 250-date backtest at the 07-10 §9
  gates.
- **C5.4 Self-consistency ≠ accuracy.** Delivered as 07-09 P4.5; confirm every enabled
  cache cell is scored independently against cold Reference **and** the
  PDE/Leisen-Reimer oracle by price/vega/moneyness/maturity/carry — never only
  percent-within-bid/ask.

**Acceptance:** 07-10 §9 gates pass for every enabled cache cell; out-of-gate cells
fall back cold with `route` recorded; cached value ≥500k contracts/s/core and cached
full Greeks ≥125k/s; archive round-trip preserves cache fingerprint + Reference
surface; the backtest reprice is no longer cold (measured drop from the 750 µs/unique
floor); the bench suite gains the cached-vs-cold portfolio row.

### C6.2 — Correlation Greeks + basket risk
**Est.** 1.5 d · **Risk** medium · **Gate:** C4.3; consumes the correlation term
structure delivered by 07-10 C6.1.

Expose dρ sensitivity ("correlation vega"), per-name marginal correlation
contribution/beta, and basket cross-underlying Greeks via the C4.3 AAD
correlation-Greek path.

**Acceptance:** correlation Greeks within the 07-10 §9.2 gates of FD on the control
basket; the dispersion book gains correlation-vega columns without a second resolve
pass.

### C7 — Retire the deprecated European portfolio stack
**Est.** 4 d · **Risk** medium · **Depends on C5.2 (surrogate re-homed) + 07-10 C3
(scenario grid + attribution shipped).**

- **C7.1 Caller audit.** Enumerate every caller of `portfolio_price.cpp`,
  `portfolio_greeks.cpp`, `portfolio_risk.cpp`, `bulk.cpp` (and `scenario_pnl`,
  `project_compare`). Anything still needing European Black-76 portfolio Greeks gets
  an explicit canonical-stack equivalent or a documented removal. *(Read-only — may
  start any time, including during 07-10.)*
- **C7.2 Migrate + delete.** Move required capability to the canonical stack
  (scenario → 07-10 C3 `scenario_grid`; attribution → C3.3; surrogate Greeks → C5.2),
  then delete the deprecated TUs and their tests, or convert the tests to
  canonical-stack coverage.
- **C7.3 Unify conventions.** One Greek convention (American) and one agg-key layout
  across the codebase; remove the European/American divergence
  (`portfolio_price.cpp:45` vs `portfolio_risk.cpp:132`).

**Acceptance:** the deprecated TUs are gone (or reduced to thin compatibility shims
with a removal date); no capability regression vs the pre-retirement caller audit;
full suite + warnings-as-errors + sanitizers green; a single documented Greek
convention.

---

## 5. Delivery sequence

Ordered by gate availability, not calendar weeks — start each package as its 07-09
gate clears:

| # | Package | Gate |
|---:|---|---|
| 1 | C7.1 caller audit | none (read-only) |
| 2 | C1.B backtest wiring | 07-09 P5 |
| 3 | C0.1 baseline repair | C1.B |
| 4 | C1.4 analytic-AL default | P2-5 verdict + C0.1 |
| 5 | C4.1 primal integration | P3.1–P3.3 |
| 6 | C4.2 IFT adjoints | P2-2b landed + P2-X verdict |
| 7 | C4.3/C4.4 bundle + layout | C4.1, C4.2, P3.4 |
| 8 | C5 cache delta + re-home | P4 |
| 9 | C6.2 correlation Greeks | C4.3 + 07-10 C6.1 |
| 10 | C7.2/C7.3 retirement | C5.2 + 07-10 C3.3 |

Total ≈ 20 engineer-days plus gating slack. If 07-09 stalls, C7.1 and any reverted
packages (per the §1 dedup rule) are the only immediately actionable items.

---

## 6. Risks and mitigations (moved from 07-10 §12 with their packages)

| Risk | Mitigation |
|---|---|
| AAD on the implicit ALO boundary is subtle / non-smooth at exercise | IFT adjoints on the *converged* residual (07-10 §5.2), boundary-band scalar fallback, hard ship rule (C4), FD validation gate |
| AAD tape memory blows up on backward induction | checkpointing; bound and report tape bytes; the boundary uses IFT not full taping |
| Retiring the legacy stack drops a used capability | caller audit first (C7.1); re-home surrogate (C5.2) before deletion; scenario/attribution already migrated by 07-10 C3 |
| Baseline still noisy after C0.1 | root-cause the 23 s pathology (likely per-call rebuild, removed by C1.B) before re-taking; quiesced host; gate all rows |
| Surrogate cache self-consistent but cold-inaccurate | independent cold + PDE oracle scoring (C5.4); never score only in-band |
| 07-09 descopes/kills a counterpart package (P2-X, P3, P4, P5) | each package records its reversion: execute the original 07-10 full text here |
| Double-build drift between sprints | dedup rule (§1): this sprint never rebuilds a 07-09 deliverable — it integrates or extends it |

---

## 7. Correctness and performance gates

The 07-10 sprint's §9 (price / Greek / scenario / determinism) and §10 (performance)
gates apply unchanged to every package here. In particular, 07-10 §2 invariant 7 (AAD
numerically validated against bump-and-revalue Greeks before any adjoint value is
trusted) is owned by this sprint's C4.

---

## 8. Implementation task ledger

| ID | Deliverable | Est. | Proof |
|---|---|---:|---|
| C0.1 | Repair corrupt portfolio baseline rows | 0.5 d | t1 ≤ floor×overhead, CV ≤5% |
| C1.B.1 | Backtest reuses pricer+workspace+totals / `advance()` | 1.5 d | zero per-step rebuild; ≥5× step |
| C1.B.2 | Hoist hedge overlay | 0.5 d | 0 pricer-builds/step |
| C1.B.3 | Hash-index the O(n²) ledgers | 0.5 d | scaling test |
| C1.4 | Analytic-AL Greeks default at scale | 0.5 d | bit-identical δ/γ/vega/ρ/vanna/volga |
| C4.1 | AoSoA<4> AVX2 primal integration (kernel = P3.2) | 1.5 d | scalar parity grid; no packed erfc |
| C4.2 | IFT adjoints on the ALO boundary | 3.0 d | Greek parity vs bump-revalue |
| C4.3 | Portfolio adjoint bundle + correlation Greeks | 2.5 d | ≤2.5× price; corr-Greek FD parity |
| C4.4 | Per-axis pruning + column-major bundle (align P3.4) | 1.0 d | first-order-only skips 2nd-order |
| C5.1 | Carry-aware cache delta over P4 | 1.0 d | route/cold parity; round-trip |
| C5.2 | Re-home deprecated-stack surrogate | 1.0 d | canonical-path cache Greeks |
| C5.3 | Dynamic-Chebyshev backtest reprice (optional) | 2.0 d | beats cached ALO or dropped |
| C5.4 | Confirm independent cold + PDE cache scoring (P4.5) | 0.5 d | error report by axis |
| C6.2 | Correlation Greeks + basket risk | 1.5 d | FD parity |
| C7.1 | Caller audit of the deprecated stack | 0.75 d | caller map + decisions |
| C7.2 | Migrate + delete deprecated TUs | 2.0 d | no regression; suite green |
| C7.3 | Unify Greek convention + agg-key | 1.25 d | single documented convention |

---

## 9. Definition of done

- [ ] the backtest performs no per-step `Portfolio`/`Pricer` rebuild and no frame
      allocation in steady state; ledgers are non-quadratic; ≥5× step throughput;
- [ ] the corrupt portfolio baseline rows are re-taken, CV-clean, and gated;
- [ ] analytic-AL Greeks are the at-scale default, with baselines re-taken after the
      flip;
- [ ] the AAD Greek path ships (≤2.5× one price, 07-10 §9.2 accuracy) **or** is killed
      with evidence and the analytic-AL + AoSoA-FD route ships;
- [ ] correlation Greeks exist for dispersion (FD parity on the control basket);
- [ ] `PricedSurface` serves a validated carry-aware cache with cold fallback;
      reloaded surfaces and backtests are no longer cold; the cached-vs-cold bench row
      exists;
- [ ] the deprecated European portfolio stack is retired (or reduced to dated shims)
      with no capability regression and a single documented Greek convention;
- [ ] every target in §3 is met on the pinned host or the increment is documented as
      killed with evidence; before/after JSON, accuracy report, and
      cache-amortization report committed.
