# atx-vol Full-Stack Competitiveness Sprint

**Date:** 2026-07-10 (rev. 2, 2026-07-11: kernel-coupled items split out to the 07-11
sprint so this sprint can run in parallel with the in-flight 07-09 sprint)

**Status:** implementation-ready plan for sub-agent-driven dev. Every claim about the
current code carries a `file:line` from a fresh four-stream audit (fitting,
analytics/risk, backtest/data/bench, plus the prior American-kernel sprint); every
external claim carries a primary URL. Claims that could not be sourced are in §16.

**Scope (rev 2, parallel-safe):** the whole library as a *product* — fitting/calibration
throughput, real-time scenario/stress risk, P&L attribution, and dispersion analytics.
This sprint runs **in parallel with** the American pricing/portfolio-throughput sprint
(`2026-07-09-...`, in progress at P2-2b at the time of this revision). Every item that
depended on, or overlapped with, that sprint's remaining packages (P2-2b–P2-5, P2-X,
P3, P4, P5, P6) has been moved to
`2026-07-11-atx-vol-kernel-coupled-integration-sprint.md` and keeps its package ID
there: baseline repair C0.1, backtest wiring C1.1–C1.3, analytic-AL default C1.4,
AAD + AoSoA C4, surrogate cache C5, correlation Greeks C6.2, legacy retirement C7.

**Parallel-execution rule:** this sprint must not edit files owned by in-flight 07-09
work — `american.cpp/.hpp`, `correction.*`, `backtest.cpp`, and the internals of
`portfolio_pricer.*` / `pricing_executor.*` — beyond strictly additive new entry
points (C3's `scenario_grid`, C1.7's vega-only eval).

**Northstar:** the fastest and most complete C++ analytics library for American listed
equity options — fitting, pricing, Greeks, backtesting, portfolio risk, dispersion —
faster than any existing implementation, at Vola-Dynamics-class surface quality.

**Locked scope decisions (this sprint):**
1. **AAD moved to 07-11.** The adjoint-Greeks package (§C4) is kernel-coupled — it
   productizes the 07-09 P2-X spike, rides the P3 AoSoA kernel, and needs the residual
   evaluator P2-2b is restructuring — so it lives in the 07-11 sprint.
2. **No third-party engine in the build.** Competitive claims are gated against our own
   checked-in baselines and *cited* against published anchors (§3). We still level the
   ISA playing field (§C0); baseline *repair* (C0.1) moved to 07-11 because its
   suspected root cause is removed by the backtest wiring that now follows 07-09 P5.
3. **Legacy-stack retirement moved to 07-11.** §C7 is gated on the surrogate re-home
   (C5.2), and §C5 duplicates 07-09 P4 — both live in the 07-11 sprint. This sprint
   still delivers C7's other prerequisite, C3.3 (attribution on the canonical stack).

---

## 1. Executive decision

### The through-line: the fast machinery already exists — it is stranded from the products

Four independent audits converged on **one structural finding**: atx-vol has already
*built* most of the fast paths it needs, then left them disconnected from the shipping
products. The dominant lever this sprint is **integration and productization**, not new
mathematics — with three genuinely new capabilities layered on top (scenario grid,
vectorized calibrator, AAD Greeks).

| Fast thing that exists | Where it is stranded | Evidence |
|---|---|---|
| AVX2 kernels (eSSVI, Greeks, IV, B76, PnL) | referenced **only** by `bench/` + `tests/`; no production path calls `simd::` | grep: `simd::` hits only `atx-vol/bench/*`, `atx-vol/tests/*`; `src/simd/*_avx2.cpp` |
| Persistent `PortfolioPricer` + `PortfolioWorkspace` + totals API (P1.4/P1.5) | `backtest.cpp` rebuilds `Portfolio`+`PortfolioPricer` **every step**, uses none of it | `src/backtest.cpp:69-99,116-176,640-663`; in-place API used only in `portfolio_pricer.cpp`/`pricing_executor.cpp` |
| `CorrectionCache` American surrogate (trivariate Chebyshev) | `PricedSurface` is "cache-free residue" by design → serialized surface reloads **cold** | `priced_surface.hpp:30,36-40`; `surface_archive.hpp:196-211` has no cache field |
| Analytic AL Greeks (5 solves vs 17 FD) | **off by default** on the at-scale path | `portfolio_pricer.hpp:420` (`analytic_greeks=false`); `priced_surface.hpp:114-120` |
| eSSVI `fit_core` warm-start (`warm`/`prior_strength`) | surface drivers call it with **no warm** → no cross-snapshot recalibration | `src/essvi_calib.cpp:646,919-920` |
| Batch AVX2 eSSVI evaluator (`essvi_backbone_w_batch`) | LM residual/Jacobian is a **scalar per-obs loop** | `simd/essvi_batch.hpp:34`; `src/essvi_calib.cpp:224-231` (no `_batch`/`simd`) |

The gap to a Vola-class product is therefore the composition of six productization gaps
and three capability gaps:

**Productization gaps (integration, low-risk, high-leverage):**
1. the backtest ignores the persistent pricer/workspace and reprices a fully **cold**
   book every step (`backtest.cpp`; cold cost `port/floor/greeks/u2688` = **750 µs/unique**)
   — **moved to 07-11 C1.B (overlaps 07-09 P5)**;
2. every serialized surface reloads cache-free, so all downstream Greeks are cold FD
   (`priced_surface.hpp:30`) — **moved to 07-11 C5 (overlaps 07-09 P4)**;
3. the AVX2 kernels are not wired into any production pricing or fitting path (the
   fitting side stays here as C1.5; the American-kernel side is 07-09 P3);
4. calibration parallelizes across names but **never across expiries** — a wide index
   fits one slice at a time (`src/essvi_calib.cpp:884`);
5. warm-start is not threaded through the surface calibration drivers;
6. Greeks at scale default to the cold 17-solve FD bundle, not analytic AL — **moved
   to 07-11 C1.4 (shifts the numbers 07-09 baselines gate against)**.

**Capability gaps (new product surface vs a Vola-class competitor):**
7. **no scenario/stress grid** — only a scalar, deprecated `scenario_pnl`
   (`portfolio_risk.cpp:161`, `SurfaceTwist` = `NotImplemented` at `:191`);
8. **no adjoint Greeks** — the only quantitative competitive metric anyone publishes
   (§3) — and no correlation Greeks for dispersion — **moved to 07-11 C4/C6.2**;
9. **dispersion is a single ATM implied-correlation number** — no term structure,
   ATM-straddle-only, with a 4× double-resolve waste
   (`dispersion.cpp:229,248,266,53,57`); the correlation-Greeks piece is 07-11 C6.2.

### Where "fast" has to land, and against what

There is **no published American-option Greeks throughput number** anywhere (confirmed in
the prior sprint §5.8 and re-confirmed here). We anchor on **price** and **calibration**
throughput, which *are* published, and derive Greeks from a cost model.

**External anchors (verified, §3):** ~**45,000 American prices/s** and ~**16,500 American
calibrations/s** on a single AMD Ryzen 9 5900 core (tastyhedge); ALO's stated ~**100,000
prices/s/CPU** (SSRN 2547027); QuantLib `QdFpAmericanEngine` ~**70–180k prices/s batch**
(academic, CPU unverified); Jaeckel European BS IV ~**2.8M/s** (European only — never a
headline for American). The only quantitative *competitive* claims in the whole market
are **AAD Greeks speedups** (Numerix "up to 1000× vs bumping"; MatLogica "price+all-Greeks
< price alone"), both vendor-marketing (§3).

**Targets (per core, pinned i7-1260P, sustained batch), gated by §9 accuracy:**

| Metric | Measured now | Ship target | Stretch |
|---|---:|---:|---:|
| Whole-surface cold fit | ~0.36 s (`roadmap:23`) | **≤120 ms** | ≤60 ms |
| Live single-slice warm refit | ~126 µs (`roadmap:205`) | **≤40 µs** | ≤20 µs |
| American calibrations/s (implied-vol inversion) | not benched in isolation | **≥40k/s** | 80k/s (beat the 16.5k anchor 2.4×) |
| Scenario grid (11×11 spot×vol, full book) | none | **≤1.2× one Greek solve** (Taylor grid) | exact-resolve on demand |
| Dispersion book build (N names) | ~4× redundant solves | **1× (no double-resolve)** | + term structure & basket recon (C6) |

Moved to the 07-11 sprint with their packages: portfolio full Greeks (≥25k uniques/s),
single-unique cold Greeks (≤150 µs), adjoint bundle (≤2.5× one price), backtest step
(≥5×), cached price/Greeks rates, correlation Greeks.

Every "measured now" number is provisional and re-measured under §C0 before it gates.

---

## 2. Non-negotiable invariants

1. Reference scalar output stays bit-for-bit identical where the API promises it (fits,
   prices, Greeks, P&L axes, NAV/accounting).
2. No global `/fp:fast`, no `-ffast-math`. `/arch:AVX2` only on dedicated ISA objects or
   an explicit opt-in perf preset — never silently library-wide on the default build.
3. **No-arbitrage is a gate, not a feature.** Every fitter output satisfies Martini-Mingone
   necessary-and-sufficient no-butterfly and Hendriks-Martini no-calendar conditions, or
   is explicitly rejected. No quantization of strikes/maturities/vols.
4. Deterministic totals across worker count and execution grouping, via fixed
   input-order reduction.
5. The scalar cold ALO oracle is never removed; surrogates and AAD are validated
   *against* it, never substituted silently.
6. Discrete-dividend and borrow handling correctness is a disqualifier if wrong — treat
   as a hard gate on every American path (§9.1).
7. AAD results are numerically validated against bump-and-revalue Greeks to the §9.2
   gates before any adjoint value is trusted *(owned by 07-11 C4; the gate travels
   with the package)*.

---

## 3. Competitive anchors and what "beat Vola" means (verified)

**Vola Dynamics** (https://voladynamics.com/): C++11 core + Python/Java/C# wrappers;
"market-maker-quality" real-time arbitrage-free surfaces (Bayesian + parametric C-curve
families, W-shaped earnings, wing extrapolation, per-quote error bars), a "probably the
fastest available" American pricer with accurate cash-dividend/large-borrow handling and
SSR-adjusted "smart delta", full cross-Greeks (delta/gamma/vega/vanna/volga/rho/rhoBorrow/
rhoDiv/theta/fugit, two thetas), scenario-based **and** greek-based P&L attribution
decomposed to ATF/skew/curvature, and VIX/variance/dividend modules. **No hard numeric
performance claim is published** — only "milliseconds", "whole US universe on one box in a
fraction of a second". Their algorithm and production surface model are **not** published.
Public methodology is confined to Klassen's no-arb SSVI/S3 conditions (SSRN 2725700) and
cash-dividend pricing (SSRN 2634051).

**Published throughput anchors (single core unless noted):**

| Metric | Number | Hardware | Source |
|---|---:|---|---|
| American prices/s | 45,000 | Ryzen 9 5900, 1 core | https://tastyhedge.com/blog/how-to-calibrate-american-options-really-fast/ |
| American calibrations/s | 16,500 (→ ~1.5M-option US market in ~45 s) | Ryzen 9 5900, 1 core | same |
| European BS IV/s | 2,800,000 | Ryzen 9 5900, 1 core | same (Jaeckel "Let's Be Rational") |
| American prices/s (algo ceiling) | "close to 100,000/s/CPU"; 10–11 digits in <0.1 s | unspecified | SSRN 2547027 |
| QuantLib QdFp prices/s | ~39k single / ~180k batch (fast); ~12.5k / ~71k (accurate) | unverified CPU | assoc. arXiv:2109.15157; hpcquantlib.wordpress.com |
| AAD Greeks | "up to 1000× vs bumping"; all 1st-order at ~3–4× one price | unspecified | numerix.com AD blog; Giles-Glasserman NA-05-15 |

**What "competitive in 2025-26" means** (buyers = market makers, vol-arb, prop): ms-scale
real-time surface refresh over the whole US universe on one box; arbitrage-free surfaces
in wings + earnings with uncertainty; full American cross-Greeks with correct discrete
dividends/borrow and SSR delta; **spot×vol×time×rate scenario/stress matrices**; greek +
scenario **P&L attribution** to ATF/skew/curvature; **dispersion/implied-correlation**
(now standardized by Cboe DSPX, launched Sep-2023); an embeddable C++ core with wrappers.

**Implications** (drive package priority): (i) own the American price *and calibration*
throughput numbers and publish them reproducibly with the CPU named — transparency is
itself a differentiator; (ii) batch/SIMD is where the wins are (QuantLib already shows
~4–5× batch-over-single); (iii) AAD Greeks at adjoint-factor < 4 is the one marketable
"beat" metric; (iv) calibration throughput (16.5k/s anchor, ~170× slower than European
IV) is the real live-surface bottleneck; (v) surface *quality* is the moat — fast-but-
fragile loses; (vi) scenario matrices + P&L attribution + dispersion are explicit product
modules we currently lack.

---

## 4. Verified implementation map (from the four-stream audit)

### 4.1 Fitting / calibration

Models and optimizers (`file:line`): raw SVI = quasi-explicit **Nelder-Mead**(m,σ) ×
bounded-linear-LSQ inner × IRLS-Huber (`src/svi_calib.cpp:671,316,229`); SVI-MM =
**LM** in price domain with Mingone-polytope projection, **analytic** Jacobian
(`:859,584,485,428`); eSSVI = **LM** damped Gauss-Newton on the Mingone unit cube,
**analytic** grad, Lee projection (`src/essvi_calib.cpp:646,240,177`); C8 = LM with
**central-FD** JW→raw Jacobian (`src/c8.cpp:203-232`, `src/c8_calib.cpp:324`); CStar =
block-coordinate LM, analytic (`src/cstar_calib.cpp:615,464`).

Throughput facts: **scalar residual/Jacobian** despite a built AVX2 kernel
(`src/essvi_calib.cpp:224-231` vs stranded `simd/essvi_batch.hpp:34`); **parallel across
names only** via `calibrate_pool` `std::jthread` (`src/calib_pool.cpp:289-315`), the
chain (expiry) loop **serial** (`src/essvi_calib.cpp:884`); C8 Jacobian does **10
`jw_to_raw` FD solves/obs**, re-evaluated 3× per LM step (`src/c8_calib.cpp:183,187,200`);
**6 `std::vector` allocations per slice** with no cross-slice reuse
(`src/essvi_calib.cpp:739-744`); redundant `essvi_backbone_w` between `residuals_and_jac`
and `cube_sse` (`:226,144`); SVI-MM **O(n²) q90 selection sort** each outer pass
(`src/svi_calib.cpp:1037-1045`). **De-Americanization is the true bottleneck**: the 0.36 s
cold whole-surface cost is per-strike Andersen-Lake IV inversion in
`build_observations_european` (`src/calib.cpp:336`) — and the low-level drivers +
`calibrate_pool` **skip de-Am entirely** (raw Black-76 `build_observations`,
`src/essvi_calib.cpp:898`), biasing American fits. Measured: cold whole-surface ~0.36 s;
XOM 18-slice ~534 ms; warm single-slice ~126 µs (`roadmap:23,205`).

No-arb: eSSVI butterfly-free **by construction** (Mingone cube through `essvi_phi_max`,
`src/vol_surface.cpp:163`) + optional Lee bisection; SVI-MM projects to admissible
polytope (`src/svi_calib.cpp:428`); C8/CStar Roper density projection post-fit
(`src/c8_calib.cpp:291`, `src/cstar_calib.cpp:582`); calendar = **post-fit projection**
(`src/arb.cpp:425,374`). Raw-SVI slices carry **no** by-construction butterfly guarantee
(`src/svi_calib.cpp:102`). `fit_metrics.hpp` (reduced χ², "min-edge" error bars) is
standalone, **not consumed** by any fitter's stopping rule (`fit_metrics.hpp:11`).

### 4.2 Portfolio analytics / risk / dispersion

**Two stacks.** Legacy European Black-76 stack (`portfolio_price.cpp`,
`portfolio_greeks.cpp`, `portfolio_risk.cpp`, `bulk.cpp`) is stamped **DEPRECATED — do not
extend** (`portfolio_risk.hpp:1-12`); canonical American SoA stack is
`portfolio_pricer.*` on `priced_surface.*`. They disagree on Greek convention (European vs
American) and agg-key layout. Full 8-Greek set (delta/gamma/vega/theta/rho/vanna/volga/
charm) in both; no 3rd-order anywhere.

Greeks-at-scale: dedup on exact bits of `(uid,K,T,side)` (`Portfolio::create`
`portfolio_pricer.cpp:65-102`); `PreparedPortfolio` aligned SoA groups
(`prepared_portfolio.hpp:131-180`); persistent pool fan-out
(`portfolio_pricer.cpp:255`); per-contract kernel is **cold `american_greeks_fd` ~17
solves** (`portfolio_pricer.hpp:47`), analytic AL **off by default** (`:420`). **No
surrogate Greek route on the canonical path** — the `CorrectionCache` surrogate exists
only in the deprecated stack (`bulk.cpp:200`, `portfolio_price.cpp:215-220`). The greek
bundle is carried **AoS** (`std::vector<AmericanGreeks>`, `portfolio_pricer.cpp:413`) even
though frames are SoA, blocking per-axis pruning + column SIMD; FirstOrder and SecondOrder
both compute the full bundle (`priced_surface.hpp:141-147`).

Scenario/stress: **no grid.** Only scalar `scenario_pnl` (`portfolio_risk.cpp:161`,
deprecated, `SurfaceTwist` unimplemented `:191`); canonical `pnl_explain` requires the
caller to build two `SurfaceSet`s — no bump helper, no spot ladder, no vol grid, no
cross-gamma matrix.

Dispersion (`dispersion.cpp`): O(n) closed-form implied correlation (`:205-212`),
vega-neutral straddle book (`:216-273`), robust missing-name policy + uid binding
(`:149-181,281`). Gaps: single ATM correlation number (no term structure, no skew/variance
dispersion, no per-name marginal correlation), **no correlation Greeks**, ATM-straddle-only
(`:41`), vega-neutral-only. Waste: `build_dispersion_book` resolves every leg **twice**
(`:229` then `:248/:266`) and each `resolve_leg` computes **two full 8-Greek bundles** just
to read vega (`:53,:57`) → ~4× the necessary solves.

### 4.3 Backtest / data / bench / build

Backtest: load-once invariant holds (`src/backtest.cpp:230-276,393,767`), but per step it
rebuilds `Portfolio::create` + `PortfolioPricer` + allocating `price()`/`pnl_explain()` in
both `book_greeks` (`:69-99`) and `compute_step` (`:116-176`), and the DeltaToZero hedge
overlay builds a **separate pricer per uid per step** (`:640-663`) → `2+U` pricer
constructions/step, none reused. O(n²) ledger scans (`shares_get/add/sum`, `in_before`,
`in_book`, `uid_of`: `:441-464,547-562,278-291`). The shipped P1.4/P1.5 persistent
infra is **never** referenced by `backtest.cpp`.

Serialization: `PricedSurface` is cache-free by design (`priced_surface.hpp:30,36-40`);
`ArchivePricingRecord` (`surface_archive.hpp:196-211`) has **no** correction-cache field →
every reloaded surface is cold; `map_all` reconstructs every surface per date (`:238`).
CRC-32C hardware-dispatched read (`surface_archive.hpp:30-39`). Panels are **not**
serialized (transient ingest only).

Bench: Google-Benchmark suite under `ATX_BUILD_BENCH` (`bench/CMakeLists.txt`) —
american/portfolio/simd/reloc targets. **No cached-vs-cold on the portfolio API**
(`portfolio_throughput_bench.cpp:27-32`), **no real-OPRA baseline**, **no head-to-head vs
any external engine** (QuantLib is an accuracy comment only, `american.hpp:44`,
`american_test.cpp:213`; absent from `vcpkg.json`). Only two baselines checked in
(`i7-1260p-clang18-sse2`, american + portfolio). **Credibility defect:**
`port/price/greeks/u2688/r1/t1` median = **23.1 s @ CV 40%**, which is **11×** its own
kernel floor (`port/floor/greeks/u2688` = 2.03 s) and 15.7× its own t2 — physically
impossible; `compare_baseline.py` marks CV>5% as `NOISY` and refuses to gate it, so the
highest-value scaling rows are **ungated and internally contradictory**.

Build/ISA: **no global `/arch:AVX2`, no `-march`, no LTO, no PGO** anywhere (root +
`atx-vol/CMakeLists.txt` + `CMakePresets.json`). AVX2 is per-object only on
`src/simd/*_avx2.cpp` (`atx-vol/CMakeLists.txt:91-94`) with CPUID dispatch; the scalar
hot paths (backtest, scatter, resolve, fit) compile at **SSE2** — the compiler never
vectorizes them. sccache + LLD + PCH are build-time only. `vcpkg.json`: arrow/zstd/
zlib-ng/openssl/gtest.

---

## 5. Research findings and implications (primary sources)

**5.1 Fitting SOTA.** Fastest accuracy-and-no-arb-guaranteed stack = Zeliade quasi-explicit
per-slice SVI (closed-form inner linear solve, 2-D outer box) — ZWP-0005
(https://www.zeliade.com/wp-content/uploads/whitepapers/zwp-0005-SVICalibration.pdf) —
gated by **Martini-Mingone necessary-and-sufficient** no-butterfly conditions (arXiv:2005.03340,
https://arxiv.org/abs/2005.03340), with **Mingone global arb-free eSSVI** for the flagship
surface fit (arXiv:2204.00312, https://arxiv.org/abs/2204.00312) and Hendriks-Martini
no-calendar conditions (SSRN 2971502). **No published SIMD/GPU vol calibrator exists** —
a vectorized batch calibrator over the quasi-explicit inner solve is unclaimed territory.
→ **§C2.**

**5.2 AAD Greeks.** Reverse-mode gives **all first-order sensitivities at < ~4× the primal
cost, independent of input count** — Giles-Glasserman NA-05-15
(https://people.maths.ox.ac.uk/~gilesm/files/NA-05-15.pdf); correlation Greeks at ≤4×
regardless of #names — Capriotti-Giles arXiv:1004.1855 (https://arxiv.org/abs/1004.1855);
2024 survey QF 24(9) (https://people.maths.ox.ac.uk/~gilesm/files/AAD_Review.pdf).
**Gate:** the ALO boundary is an implicit fixed point and the exercise decision is
non-smooth — differentiate the *converged* residual via the implicit function theorem, not
through the iteration; budget tape memory (checkpointing). → **§C4.**

**5.3 Surrogates.** Chebyshev parametric option pricing with rigorous error bounds
(arXiv:1505.04648, https://arxiv.org/abs/1505.04648); **Dynamic Chebyshev** prices American
options with **price + delta + gamma per backward step, model-dependent work precomputed
offline** (arXiv:1806.05579, https://arxiv.org/abs/1806.05579); low-rank tensor Chebyshev
for higher parameter dimension (DOI 10.1137/19M1244172). GP/NN surrogates enforce no-arb
only as *soft* penalties → not a trusted pricing source, use as a fast prior
(arXiv:1906.05065, arXiv:2406.11520). **When surrogates win:** high evaluation volume
against a *fixed* model (backtests, portfolio revaluation) where an offline build
amortizes. → **§C5.**

**5.4 American core.** ALO remains SOTA for BS-American (SSRN 2547027); QD+ seeding (Li
2010), double-boundary negative-rate case (Andersen-Lake 2021 Wilmott; Healy arXiv:2109.15157).
Already handled in the prior sprint's P0.5; keep as invariant. Nonlinear-stencil/GPU work
(arXiv:2303.02317) is adjacent, non-goal.

**5.5 Fast Φ/erfc for clean Greeks.** Cody rational (Math.Comp. 23, 1969) and Hart-5666
(West 2005) at ~1-ULP; SLEEF 1.0-ULP and 3.5-ULP tiers (arXiv:2001.09258, https://sleef.org/).
**Gate:** finite-difference and vega/gamma differencing amplify error — the AVX2 kernel
must use a **1-ULP-class** Φ; the 3.5-ULP fast tier is unsafe under FD Greeks. → **§C4.**

**5.6 Dispersion.** Cboe implied-correlation methodology
(https://cdn.cboe.com/resources/indices/documents/Implied_Correlation-WhitePaper-v1.0.5.pdf),
DSPX index; correlation Greeks are the Capriotti-Giles AAD case (5.2). Literature is
thinner/practitioner-oriented — an opportunity, since competitors treat it as research.
→ **§C6.**

---

## 6. Target architecture

```text
            OPRA / panel ingest ──► calibrate (C2) ──► PricedSurface (+ optional cache, C5)
                                        │                        │
                    [batch quasi-explicit + AVX2 + warm-start,   │  [carry-aware CorrectionCacheV2
                     intra-name expiry parallel, de-Am fast]     │   attached + serialized]
                                                                 ▼
                                              ┌──────── canonical American analytics core ────────┐
                                              │  PortfolioPricer (persistent) + PortfolioWorkspace │
                                              │  PreparedPortfolio SoA groups + pricing_executor   │
                                              └───────────────────────┬────────────────────────────┘
                     ┌──────────────────────┬──────────────────────────┼───────────────────────┬─────────────────────┐
                     ▼                      ▼                          ▼                       ▼                     ▼
            price/greeks (C1/C4)   scenario_grid (C3)          AAD Greeks (C4)         backtest advance (C1)   dispersion (C6)
            [AoSoA<4> AVX2,        [spot×vol×t×r matrix,       [IFT adjoints on ALO    [reuse pricer+ws,       [corr term struct,
             analytic-AL default,   Taylor grid ≈ free,        boundary; correlation   totals API, no per-      corr Greeks via AAD,
             per-axis pruning]      exact re-solve on demand]  Greeks; <4× primal]     step rebuild]           basket-surface recon]

     Deprecated European stack (portfolio_risk/price/greeks/bulk) ──► RETIRED (C7), callers migrated to the canonical core.
```

Reference (scalar, bit-stable, cold ALO) and Production (AVX2/AAD/surrogate) lanes remain
distinct; every persisted result records ISA, math mode, route, and cache fingerprint.

*Delivery split:* the `AAD Greeks (C4)` box, the `backtest advance (C1)` box, the
optional-cache attachment (C5), and the legacy-stack retirement (C7) deliver in the
**07-11** sprint. This sprint delivers calibrate (C2), `scenario_grid` (C3),
dispersion term structure / basket reconstruction (C6.1/C6.3/C6.4), and the additive
price/greeks integrations C1.5–C1.7.

---

## 7. Work packages

Package IDs are `C0…C7` to avoid collision with the American sprint's `P0…P6`.
Packages C4, C5, C7 and items C0.1, C1.1–C1.4, C6.2 have **moved to the 07-11 sprint**
and keep their IDs there; stubs below record the move.

### C0 — Benchmark credibility + ISA-fair build *(required first)*
**Est.** 2 engineer-days · **Risk** low · **Ship condition:** required before any later
number gates.

- **C0.1 — moved to 07-11.** The corrupt `port/price/greeks/u2688/*` rows' suspected
  root cause (per-call `Portfolio`/`Pricer` rebuild) is removed by the backtest wiring
  that now follows 07-09 P5; the re-take happens there. Until then, treat those rows
  as ungated and do not cite them.
- **C0.2 Fill the baseline gaps.** Add checked-in baselines for backtest (pre-change
  snapshot), simd, corpus/fit, and a **real-OPRA corpus** run (today the bench suite is
  100% synthetic; real OPRA is example-only, no baseline). The **cached-vs-cold
  portfolio** row lands with C5 in 07-11.
- **C0.3 Fitting-throughput first-class bench.** Promote fit timing out of `reloc-bench`
  into a `fitting_throughput_bench` with calibrations/s and warm-vs-cold, so §C2 has a
  gate. Report calibrations/s directly comparable to the 16.5k anchor.
- **C0.4 ISA-fair perf preset.** Add a `rel-avx2` preset (`/arch:AVX2` global **on that
  preset only**, default `rel` stays SSE2/portable) so the compiler can vectorize scalar
  hot paths and so any published ratio is not understated. ThinLTO/PGO evaluation stays
  with 07-09 P6 — do not duplicate it here. Never `/fp:fast`. Record ISA in every
  baseline filename (already the convention). Preset-only change (`CMakePresets.json`);
  do not touch the per-object ISA setup 07-09 P3.1 owns.
- **C0.5 Anchor doc.** A `bench/ANCHORS.md` capturing the §3 published numbers with URLs,
  CPU, and the "European-IV ≠ American-IV" caveat, so no future claim conflates them.

**Acceptance:** every **newly added** baseline has CV ≤5% single-core / ≤10% all-core
and is consistent with its kernel floor; `compare_baseline.py` gates all new rows (no
silent NOISY skips); the pre-existing corrupt portfolio rows are quarantined (marked
ungated) pending 07-11 C0.1; the real-OPRA corpus bench runs in CI-lite; the `rel-avx2`
preset builds warnings-clean and is measurably ISA-different from `rel`.

### C1 — Wire the stranded fast paths into production *(low-risk, highest leverage)*
**Est.** 2.25 engineer-days · **Risk** low–medium · **Expected gain:** ~3–4× eSSVI
inner loop; ~4× dispersion book build.

- **C1.1–C1.3 — moved to 07-11 (as C1.B).** Backtest wiring overlaps 07-09 P5 (same
  file `backtest.cpp`, same deliverable — P5.2's `advance()` subsumes C1.1's pricer
  reuse).
- **C1.4 — moved to 07-11.** Flipping `PriceOptions::analytic_greeks` shifts every
  portfolio-Greeks number the 07-09 baselines gate against, and 07-09 P2-5 rewires
  the analytic route the flag selects.
- **C1.5 Wire AVX2 into the eSSVI LM.** Replace the scalar `residuals_and_jac` loop
  (`src/essvi_calib.cpp:224-231`) with `essvi_backbone_w_batch` (`simd/essvi_batch.hpp:34`)
  + a new batched grad kernel; dedupe the `essvi_backbone_w` double-eval (`:226,144`);
  asymmetric-ρ falls back to scalar (kernel already validated to 1e-12).
- **C1.6 Thread warm-start through the surface drivers.** Give `calib_surface_impl` an
  optional prior `VolSurface*`; pass the matching prior slice as `warm` into `fit_core`
  (`src/essvi_calib.cpp:919`). Null path stays byte-identical.
- **C1.7 Kill the dispersion double-resolve.** Reuse the `DispersionLeg`s from the signal
  pass; add a vega-only single-axis eval (mirror `PricedSurface::delta`). ~4× on book build
  (`dispersion.cpp:229,248,266,53,57`).

**Acceptance:** Reference outputs bit-identical on all fit/price/PnL columns; eSSVI fit
byte-identical on the scalar fallback path and within 1e-12 on the AVX2 path; dispersion
book build shows ~4× fewer solves; §C0 baselines show the targeted multipliers;
sanitizers green.

### C2 — Fitting throughput to beat the 16.5k calib/s anchor
**Est.** 7 engineer-days · **Risk** medium · **Expected gain:** ≥3× whole-surface fit;
≥2.4× calibrations/s.

- **C2.1 Intra-name expiry parallelism + per-thread scratch arena.** Fan the chain (expiry)
  loop across the thread pool (slices are independent — butterfly per-slice, calendar is a
  cheap post-pass); fit into disjoint slots for determinism (mirror `calibrate_pool`); hoist
  the 6 per-slice vectors into a reused workspace (`src/essvi_calib.cpp:884,739-744`; mirror
  in svi/c8/cstar). Largest single lever on wide-index latency.
- **C2.2 Vectorized batch calibrator (the white-space win).** Batch the quasi-explicit
  SVI inner linear solve and the eSSVI residual/Jacobian across strikes/slices/names on
  AVX2 lanes (§5.1 — no published SIMD calibrator exists). Build on C1.5's kernels.
- **C2.3 De-Am fast path in the driver + fix the American bias.** Route the driver /
  `calibrate_pool` path through `build_observations_european` with the
  `AmericanCorrectionCaches` hot path (Black-76 + Chebyshev correction, already plumbed,
  `calib.hpp:302-314`) instead of the cold Andersen-Lake per-strike solve
  (`src/calib.cpp:336`); make de-Am the default for American names rather than raw
  `build_observations` (`src/essvi_calib.cpp:898`). Attacks the actual 0.36 s bottleneck
  **and** removes the silent American-premium bias in the pool path. Knob-tunable
  (`iv_tol`, `al_opts`); gate against SPY parity regression.
- **C2.4 Analytic C8 Jacobian.** Replace the 10-FD-solves/obs `jw_to_raw_jac`
  (`src/c8.cpp:203`) with the closed-form JW→raw derivative; compute the raw-SVI conversion
  once per LM step, not inside every `build_normal_eq` (`src/c8_calib.cpp:187`). Protect the
  recovery contract with an FD cross-check test.
- **C2.5 No-arb gate hardening.** Add the Martini-Mingone necessary-and-sufficient
  per-slice butterfly check (arXiv:2005.03340) as a validator on *every* fitter output,
  including raw-SVI which today has none (`src/svi_calib.cpp:102`). Feed `fit_metrics`
  (reduced χ², min-edge) into a model-selection / stopping signal (`fit_metrics.hpp:11`).

**Cross-cutting cleanups:** replace SVI-MM's O(n²) q90 selection sort with the 64-strided
helper already in `detail/robust.hpp:137` (`src/svi_calib.cpp:1037`); unify the two
divergent Huber losses.

**Acceptance:** whole-surface cold fit ≤120 ms and calibrations/s ≥40k on the pinned host;
every fitted slice passes Martini-Mingone + Hendriks-Martini; de-Am path removes the
American bias (SPY parity gate green); C8 analytic Jacobian matches FD to 1e-8; warm-started
refit converges in ≥40% fewer LM iters on the real corpus or the warm path is left off with
evidence.

### C3 — Scenario / stress grid engine + P&L attribution *(flagship Vola-parity gap)*
**Est.** 5 engineer-days · **Risk** low (Taylor grid) → medium (exact re-solve).

- **C3.1 `scenario_grid` on `PortfolioPricer`.** New API taking a base `SurfaceSet` + a grid
  of `(dS%, dσ, dr, dt)` → a PnL matrix, reusing dedup + `PreparedPortfolio` +
  `pricing_executor`. **First cut = Taylor grid:** the per-unique Greek bundle is computed
  once; reconstruct PnL across the grid analytically to 2nd order (same math as
  `scatter_pnl_rows`, `portfolio_pricer.cpp:753-762`), so grid cost ≈ one Greek solve.
- **C3.2 Exact re-solve for large bumps.** For |bump| beyond a measured Taylor-valid radius,
  re-solve via a no-refit vol-bump surface view (bump σ/spot on the resolved point without
  re-fitting the curve). Route large cells to exact, small cells to Taylor; record the route.
- **C3.3 Greek + scenario P&L attribution.** Decompose PnL into ATF / skew / curvature /
  rates / time / unexplained (the Vola vocabulary), on the canonical stack (subsuming and
  replacing the deprecated `project_compare` / `scenario_pnl`, `portfolio_risk.cpp:634,161`).
  This is a prerequisite for C7 (07-11).

**Acceptance:** full-book 11×11 spot×vol grid at ≤1.2× one Greek-solve cost (Taylor path);
Taylor vs exact agree to §9.3 within the declared radius; attribution axes are pure
(non-target axes exactly zero under a pure bump); deterministic across worker count.

### C4 — moved to 07-11 *(AAD Greeks-at-scale + AoSoA<4> AVX2 American primal)*

Kernel-coupled, so it moved: C4.1 is literally 07-09 P3.2 ("the deferred P3.2");
C4.2 productizes the 07-09 P2-X implicit-differentiation spike and needs the residual
evaluator P2-2b is restructuring right now; C4.4 overlaps P3.4's SoA-preferred Greeks
output. Full text, hard ship rule, and acceptance live in the 07-11 sprint unchanged.
The §5.2/§5.5 research findings below remain the evidence base and are referenced from
there.

### C5 — moved to 07-11 *(surrogate acceleration: carry-aware cache + Chebyshev reprice)*

C5.1/C5.4 near-verbatim duplicate 07-09 P4.1–P4.5 (carry-aware `CorrectionCacheV2`,
derivative tensors, archive attachment, `route=ColdFallback`, PDE-theta semantics,
independent cold+PDE scoring). The 07-11 version is defined as the delta/integration
over P4, plus the surrogate re-home (C5.2) and the optional Dynamic-Chebyshev backtest
reprice (C5.3). C5 still precedes C7 there.

### C6 — Dispersion completeness *(differentiator)*
**Est.** 3.5 engineer-days · **Risk** medium · C6.2 moved to 07-11 with C4.

- **C6.1 Correlation term structure.** Extend the single ATM implied-correlation number
  (`dispersion.cpp:205-212`) to a per-expiry correlation curve; add skew-/variance-based
  dispersion, not ATM-only.
- **C6.2 — moved to 07-11.** Correlation Greeks + basket risk ride the C4.3 AAD
  correlation-Greek path; they consume the C6.1 term structure delivered here.
- **C6.3 Implied-basket-surface reconstruction.** Build the theoretical index surface from
  constituents and compare against the traded index surface (the core dispersion signal).
- **C6.4 Sizing variants.** Add gamma-neutral and premium-neutral sizing beside the current
  vega-neutral straddle (`dispersion.cpp:41,244-273`); option-strip/variance replication for
  a true variance-dispersion trade.

**Acceptance:** correlation term structure matches a direct recompute on a control basket;
basket-surface reconstruction round-trips against the implied-correlation closed form; new
sizing variants preserve the documented neutrality to tolerance. (Correlation-Greek gates
travel with C6.2 to 07-11.)

### C7 — moved to 07-11 *(retire the deprecated European portfolio stack)*

Gated on the surrogate re-home (C5.2 → 07-09 P4) — the deprecated `bulk.cpp` cannot be
deleted while the only `CorrectionCache` Greek route lives there. This sprint still
delivers C7's other prerequisite, C3.3 (attribution on the canonical stack). The
read-only caller audit (C7.1) may start any time, including during this sprint.

---

## 8. Delivery sequence (4-week, sub-agent lanes)

Two lanes run in parallel: a **fitting/bench lane** and an **analytics lane**, with a
correctness owner gating every merge. Nothing below waits on 07-09.

| Week | Fitting/bench lane | Analytics lane |
|---|---|---|
| 1 | **C0** (C0.2/C0.3/C0.4/C0.5) credibility + ISA-fair preset + fitting bench | **C1.5** AVX2-into-LM; **C1.7** dispersion de-dupe |
| 2 | **C2.1** intra-name parallel + arena; **C2.3** de-Am fast path | **C1.6** warm-start; **C3.1** scenario grid (Taylor) |
| 3 | **C2.2** vectorized batch calibrator; **C2.4** analytic C8 Jac | **C3.2** exact re-solve; **C3.3** attribution |
| 4 | **C2.5** no-arb gate + fit-metrics feedback | **C6.1** corr term structure; **C6.3** basket recon; **C6.4** sizing |

**Handoffs to 07-11:** C0.1 baseline repair (after 07-09 P5 / C1.B); the cached-vs-cold
bench row (after C5); the C1.4 default flip (after the P2-5 verdict); C4/C5/C6.2/C7 as
mapped in the 07-11 sprint's §2. **If the 4-week box compresses to a hard 2 weeks, ship
C0 + C1.5–C1.7 + C2** — that wires the fitting fast paths, beats the calibration anchor,
and produces defensible numbers, with C3/C6 deferred.

---

## 9. Correctness and numerical acceptance gates

**9.1 Price / fit gates.** Three independent references: current scalar accurate ALO; a
Richardson-refined Crank-Nicolson PDE oracle; Leisen-Reimer. Reference mode preserves bit
contracts. Every fit slice passes Martini-Mingone necessary-and-sufficient no-butterfly +
Hendriks-Martini no-calendar. De-Am path preserves the SPY American-parity regression.
Discrete-dividend + borrow corners are on the corner grid. Fast/cached price ≤$0.001 (max
<$0.005) vs cold ALO inside the declared box; else cold fallback.

**9.2 Greek gates** (bump-and-revalue reference around the accurate pricer; AAD validated
against it): delta abs ≤2e-5; gamma abs ≤2e-5 or rel ≤2e-3 where |gamma|>1e-3; vega/rho/
theta contribution under canonical 1-day/1-vol-pt/1-bp shocks ≤$0.001/share; vanna/volga/
charm ≤$0.001/share; correlation Greeks within FD tolerance on the control basket; no
NaN/Inf except documented invalid lanes; exercise-boundary band reported separately with
scalar fallback where a derivative is non-smooth.

**9.3 Scenario / P&L attribution gates.** `unexplained = total − Σ(explained)` is
tautological and not an accuracy test. Require pure-axis bump tests (non-target axes exactly
zero), 2nd-order convergence (halving a pure move reduces the residual at the expected
order away from the boundary), Taylor-vs-exact agreement inside the declared radius,
whole-book Reference-vs-Production by axis, and NAV/accounting equality across worker count
and grouping.

**9.4 Determinism.** Reference scalar: cross-thread + archive bit identity where promised.
Production AVX2/AAD: bit identity for the same ISA/math-mode/build at all worker counts.
Cross-ISA: tolerance, not false bit-identity. Every persisted result records ISA, math mode,
route, cache fingerprint, compiler version, scheme.

---

## 10. Performance acceptance gates

A package ships only if all apply: (1) median target met on the synthetic matrix **and** a
real OPRA corpus; (2) CV within the C0 limit; (3) p99 latency does not regress >10% unless
the API is explicitly throughput-only; (4) fallback rate reported and ≤5% on the normal
corpus; (5) no new steady-state allocation in kernel / prepared portfolio / stateful backtest
paths; (6) cold-cache and warm-cache both measured where both matter; (7) all-core runs long
enough to expose frequency/thermal effects; (8) output bandwidth reported separately from
unique-contract compute; (9) full tests + warnings-as-errors + sanitizers + archive
compatibility pass; (10) a before/after JSON and a short decision note committed; (11)
published-anchor comparisons cite CPU + source and never conflate European with American IV.

---

## 11. Benchmark scenarios that must exist

| Scenario | Why |
|---|---|
| One SPY board full fit, cold + warm | calibration latency floor + warm speedup |
| Whole-index fit (SPX/SPY, 20–40 expiries) | intra-name expiry parallelism (C2.1) |
| Universe fit (N names) | "whole US universe on one box" bar (Vola qualitative) |
| Calibrations/s, American IV inversion | the 16.5k anchor; the live-surface bottleneck |
| Portfolio full Greeks, u2688 × {r1..r1000} × {t1..t8} | at-scale Greeks; corrupt-row re-take is 07-11 C0.1 |
| Adjoint bundle vs bump-and-revalue | AAD ship rule (07-11 C4) |
| Full-book 11×11 spot×vol scenario grid | C3 flagship, Taylor vs exact |
| Cached-vs-cold reprice on `PricedSurface` | 07-11 C5; the path nothing measures today |
| 250-date rolling backtest, fixed book | step-reuse gate is 07-11 C1.B; pre-change baseline here (C0.2) |
| Dispersion book build + correlation term structure | C6 (corr Greeks: 07-11 C6.2) |
| Real-OPRA corpus (not synthetic) | credibility of every headline number |
| Negative/near-zero r,q corners | double-boundary + discrete-div correctness |

Each portfolio/fit case reports **unique-contracts/s or calibrations/s alongside
positions/s** — a headline positions/s without its dedup ratio is rejected.

---

## 12. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Vectorized calibrator loses fit quality / no-arb | Martini-Mingone gate on every output; scalar fallback for asymmetric-ρ; bit-identical scalar path retained |
| De-Am fast path changes fitted params | knob-tunable tol; SPY parity regression gate; keep cold AL available |
| Global AVX2 breaks portability/bit-identity | `/arch:AVX2` only on the `rel-avx2` preset, never default; no `/fp:fast`; forced-scalar CI |
| Scenario Taylor grid inaccurate for large bumps | measured Taylor-valid radius; exact re-solve beyond it; route recorded |
| No external engine in the build → "faster" is only vs our baselines | cite published anchors with CPU + URL; reproducible baselines; be explicit it is anchor-relative, not head-to-head |
| Parallel-sprint merge conflicts with in-flight 07-09 work | parallel-execution rule (header): no edits to `american.*`, `correction.*`, `backtest.cpp`, `portfolio_pricer` internals; new entry points additive-only; rebase gate before merge |
| Additive `portfolio_pricer`/`priced_surface` entry points (C3.1, C1.7) collide with 07-09 P3.5/P5 edits | new TUs + new methods only; no signature changes to existing APIs; coordinate merge order with the 07-09 owner |

Risks for the moved packages (AAD subtlety, tape memory, cache self-consistency,
legacy-retirement regression, baseline noise) travel with them to the 07-11 sprint.

---

## 13. Explicit non-goals

- No third-party pricing engine (QuantLib/etc.) added to the build this sprint (decision 2);
  claims are anchor-relative and internally reproducible.
- GPU/CUDA (published American speedups are Monte-Carlo/binomial; no semi-analytic-ALO
  crossover exists).
- COS method (needs a characteristic function; not the natural tool for BS-American).
- Replacing ALO as the American reference, or changing fitted-curve semantics beyond the
  no-arb hardening.
- Global `-ffast-math` / `/fp:fast`; quantizing strikes/maturities/vols.
- NN/GP surrogates as a *trusted* pricing/Greeks source (soft no-arb only; allowed as a
  fast prior/initializer feeding the arb-free parametric fit).
- Serializing mutable warm-start state as part of the immutable surface.
- Treating more threads as a substitute for a faster per-core kernel.

---

## 14. Implementation task ledger

| ID | Deliverable | Est. | Proof |
|---|---|---:|---|
| C0.2 | Backtest/simd/fit/real-OPRA baselines | 0.75 d | checked-in JSON, gated |
| C0.3 | First-class fitting-throughput bench | 0.5 d | calibrations/s vs 16.5k anchor |
| C0.4 | `rel-avx2` ISA-fair preset | 0.5 d | ISA-diff build |
| C0.5 | `bench/ANCHORS.md` | 0.25 d | URLs + CPU + Euro-IV caveat |
| C1.5 | AVX2 eSSVI kernel into LM residual/Jac | 1.0 d | 1e-12 parity; ≥3× inner loop |
| C1.6 | Warm-start through surface drivers | 0.75 d | byte-identical null path; iter drop |
| C1.7 | Kill dispersion double-resolve + vega-only eval | 0.5 d | ~4× book build |
| C2.1 | Intra-name expiry parallel + scratch arena | 1.5 d | deterministic; near-linear in expiries |
| C2.2 | Vectorized batch quasi-explicit calibrator | 2.0 d | ≥2.4× calibrations/s |
| C2.3 | De-Am fast path in driver + fix bias | 1.5 d | SPY parity; bottleneck removed |
| C2.4 | Analytic C8 Jacobian | 1.0 d | 1e-8 vs FD; per-iter cost drop |
| C2.5 | Martini-Mingone gate + fit-metrics feedback | 1.0 d | every slice arb-free; selection signal |
| C3.1 | `scenario_grid` Taylor path | 1.5 d | ≤1.2× one Greek solve |
| C3.2 | Exact re-solve for large bumps | 1.5 d | Taylor-vs-exact within radius |
| C3.3 | Greek + scenario P&L attribution (ATF/skew/curv) | 2.0 d | pure-axis tests |
| C6.1 | Correlation term structure | 1.0 d | control-basket recompute |
| C6.3 | Implied-basket-surface reconstruction | 1.5 d | closed-form round-trip |
| C6.4 | Gamma/premium-neutral sizing | 1.0 d | neutrality tolerance |

Moved rows (C0.1, C1.1–C1.4, C4.1–C4.4, C5.1–C5.4, C6.2, C7.1–C7.3) live in the 07-11
sprint's ledger.

---

## 15. Definition of done

- [ ] every newly checked-in baseline is CV-clean, gated, and consistent with its kernel
      floor; a real-OPRA-corpus baseline exists; the corrupt portfolio rows are
      quarantined pending 07-11 C0.1; the ISA-fair `rel-avx2` preset builds clean;
- [ ] Reference mode is bit-for-bit unchanged and all §9 gates pass;
- [ ] the AVX2 eSSVI kernel drives the LM fit; warm-start is threaded through the surface
      drivers; whole-surface cold fit ≤120 ms and calibrations/s ≥40k;
- [ ] every fitted slice is Martini-Mingone + Hendriks-Martini arb-free; the de-Am path
      removes the silent American-IV bias; C8 uses an analytic Jacobian;
- [ ] a full-book spot×vol×time×rate scenario grid ships with greek + scenario P&L
      attribution (ATF/skew/curvature), on the canonical stack;
- [ ] dispersion has a correlation term structure, basket-surface reconstruction,
      multiple sizing variants, and no double-resolve;
- [ ] no file owned by in-flight 07-09 work was edited (parallel-execution rule), and
      every handoff item is tracked in the 07-11 sprint doc;
- [ ] every ship target in §1 (kept rows) is met on the pinned host or the increment is
      documented as killed with evidence; before/after JSON and accuracy report are
      committed;
- [ ] README performance claims distinguish scalar latency, batch throughput, cold/reference
      vs cached/production, unique-vs-position rates, and cite published anchors with CPU +
      URL (never conflating European with American IV).

At that point atx-vol fits faster (vectorized, intra-name-parallel, warm-started,
arb-free-gated), stresses a whole book in ~one Greek-solve (scenario grid + attribution),
computes real dispersion analytics without redundant solves, and backs every headline
with a credible, reproducible number against published anchors — while the kernel-coupled
work (AAD, surrogate cache, backtest wiring, legacy retirement) proceeds in the
07-09 → 07-11 lane without merge conflicts.

---

## 16. Unverified claims and open questions

- **No published American-Greeks throughput exists** (re-confirmed). Greeks targets are a
  cost model against price/calibration anchors.
- **QuantLib QdFp batch numbers (~70–180k/s)** could not be pinned to a CPU; treat as
  order-of-magnitude, not a hardware anchor (per decision 2 we do not build QuantLib anyway).
- **Vola Dynamics publishes no hard number**; the beatable bar is qualitative
  ("whole US universe, sub-second, one box"). Its algorithm/surface model are not public.
- **MatLogica / Numerix AAD speedups** are vendor-marketing (no CPU, no absolute
  throughput); use only to justify AAD *direction*, not a target multiple.
- **No published SIMD/GPU vol calibrator** was found — C2.2 is asserted as engineering
  white space, not a reproduced result.
- **SLEEF erf/erfc exact ULP tiers**, **West/Cody exact digit counts**, and **Li 2010 QD+
  primary URL** need a direct lookup before the Φ-kernel and seed choices are finalized
  (§5.5, §5.4).

**Open engineering questions:**
1. *(moved to 07-11 with C0.1/C1.B)* Is the 23 s baseline row a real pathology (per-call
   rebuild / oversubscription) or a measurement artifact?
2. What is the measured Taylor-valid bump radius for the scenario grid (sets the
   Taylor↔exact routing threshold in C3.2)?
3. What is the real per-expiry σ range and expiry count on the OPRA corpus (sets the
   intra-name parallel width and any calibrator lane count in C2)?
4. *(moved to 07-11 with C4)* Does the AAD bundle actually clear ≤2.5× price on the ALO
   boundary, or does the analytic-AL + AoSoA-FD route win?

