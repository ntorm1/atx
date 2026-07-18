# AL preset ladder — research, mapping, and tier policy (WS-K / K1)

Status: **research + policy + measurement**. This note delivers the K1 deliverable of
the 2026-07-18 AL-Solve-Wall SOTA sprint: (1) the QuantLib `QdFpAmericanEngine`
preset definitions and how they map onto our `AlScheme`; (2) Healy's FP-A/FP-B
stability policy; (3) an accuracy×cost **ladder bench** with measured numbers; (4) a
**tier policy table** (marks / greeks / fit-de-Am / cache-sampling); (5) the audit of
what `pricing_.al_opts` the backtest cold path actually resolves to.

K1 changes **no production default**. It adds the ladder bench + this policy so
consumers (K2/K3, C3, L4) can adopt tiers under their own gates.

---

## 0. TL;DR (the decisions)

- **Our fast preset over-pays the fixed-point quadrature and under-pays the pricing
  quadrature.** QuantLib's fast scheme *decouples* the two: a cheap boundary-locating
  quadrature (`l=7`) with an accurate final pricing quadrature (`p=27`). Our
  `al_fast_opts()` conflates them (`n_quad_fp = n_quad_price = 16`).
- Measured on a 104-pt representative SPY-OPRA grid (provisional, shared host): the
  **QuantLib-fast-equivalent rung (`fp=8`, `price=32`, `nb=7`, 2 sweeps) is ~1.8×
  faster than our current fast at essentially equal accuracy (~1e-3)** — and it wins
  *despite* running on the un-specialized generic path. This is the K1 headline: the
  "≥2× from preset right-sizing" the sprint predicted is real.
- **Our ACCURATE preset (`nullopt` → 12/24/48) already dominates the
  QuantLib-accurate-equivalent** on this engine (cheaper *and* more accurate). No
  re-tiering of the high-accuracy tier is warranted.
- **FP-A/FP-B**: our solver is FP-B-class (the robust default). Healy shows FP-A is
  unstable for `q < r`; we do not use FP-A, so nothing to gate. Keep FP-B.
- **Audit**: the backtest cold path prices with `pricing_.al_opts`, which is baked at
  *fit* time. Under the production populate default (`FitPreset::Robust`) that resolves
  to `al_default_opts()` = `{12,24,8,1e-10}` → a **{nb=12, fp=24, price=24, 2 JN + 6
  FP, 1e-10}** scheme — NOT the true nullopt-ACCURATE (which is `price=48, 4 FP`).
  Marks whose economic tolerance the fast/`ql_fast` tier already meets are being priced
  on this ~4–8× more expensive scheme on every cache miss. The lever is C3 (which preset
  gets baked) + L4 (tier wiring); K1 quantifies the rungs.

---

## 1. QuantLib `QdFpAmericanEngine` preset definitions (primary source)

Source: QuantLib `ql/pricingengines/vanilla/qdfpamericanengine.cpp`
([lballabio/QuantLib](https://github.com/lballabio/QuantLib/blob/master/ql/pricingengines/vanilla/qdfpamericanengine.cpp);
annotated mirror
[rkapl123.github.io/QLAnnotatedSource](https://rkapl123.github.io/QLAnnotatedSource/d0/d74/qdfpamericanengine_8cpp_source.html)).
Algorithm: L. Andersen, M. Lake, D. Offengenden, "High-Performance American Option
Pricing", [SSRN 2547027](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027)
(2015).

The engine ships three iteration schemes:

| Preset | Constructor | l | m | n | p / ε |
|---|---|---|---|---|---|
| `fastScheme()` | `QdFpLegendreScheme(7, 2, 7, 27)` | 7 | 2 | 7 | p=27 |
| `accurateScheme()` | `QdFpLegendreTanhSinhScheme(25, 5, 13, 1e-8)` | 25 | 5 | 13 | ε=1e-8 (tanh-sinh) |
| `highPrecisionScheme()` | `QdFpTanhSinhIterationScheme(10, 30, 1e-10)` | — | 10 | 30 | ε=1e-10 (tanh-sinh) |

Constructor parameter semantics (`QdFpLegendreScheme(Size l, Size m, Size n, Size p)`):

- **l** — Gauss-Legendre order of the quadrature used *inside* the fixed-point
  iterations (the Kim integral that locates the early-exercise boundary).
- **m** — number of fixed-point iteration steps.
- **n** — number of Chebyshev interpolation nodes for the boundary.
- **p** — Gauss-Legendre order of the *final* quadrature that integrates the premium
  along the converged boundary to produce the price.

The tanh-sinh variants replace the order-`l`/`p` Gauss-Legendre integrals with adaptive
tanh-sinh (Gauss-Lobatto fallback) integration to tolerance `ε`.

**The load-bearing observation:** the fast scheme runs a *cheap* fixed-point
quadrature (`l=7`) but a *rich* final pricing quadrature (`p=27`). The fixed-point
integral only has to locate the boundary; the pricing integral sets the price accuracy
given a converged boundary. Because the fixed-point integral runs `l·n·m` times per
solve while the pricing integral runs once, decoupling `l` (small) from `p` (large) is
the entire trick: pay for accuracy only where it is cheap.

Performance envelope (ALO paper / HPC-QuantLib): at FD-grid accuracy the algorithm runs
~**100,000 prices/s/core (~10 µs/op)**, and 10–11 significant digits in <0.1 s. See
[High-Performance American Option Pricing (HPC-QuantLib)](https://hpcquantlib.wordpress.com/2022/10/09/high-performance-american-option-pricing/).

## 2. Mapping onto our `AlScheme`

Our internal solve scheme (`atx-vol/src/american_boundary.hpp`, `struct AlScheme`):

```
n_boundary    Chebyshev boundary collocation nodes           <-> QuantLib n
n_quad_fp     Gauss-Legendre order, fixed-point integral     <-> QuantLib l
n_quad_price  Gauss-Legendre order, premium/pricing integral <-> QuantLib p
n_iter_jn     Jacobi-Newton sweeps      }  total sweeps       <-> QuantLib m
n_iter_fp     fixed-point sweeps        }
tol           boundary-residual convergence tol              <-> QuantLib ε (loosely)
seed          cold boundary seed (BAW default; QD+ available)   (no QuantLib analogue)
```

Our GL tables exist for orders **{8, 16, 24, 32, 48, 64}** (`gl_find`, american.cpp),
so QuantLib's `l=7`/`p=27` map to the nearest available `8`/`32`.

The **public knob struct `AlOpts`** (`american.hpp`) exposes only *one* quadrature knob:

```
struct AlOpts { n_collocation; n_quadrature; max_newton_iter; tol; }
```

and `scheme_from_opts` (american.cpp) maps it with **`n_quad_price = n_quad_fp`** for any
engaged `AlOpts`. **So the public API cannot express `l ≠ p`** — the one axis QuantLib's
fast scheme is built on. The internal `AlScheme` *can* (separate fields); the A6 bench
seam `detail::andersen_lake_seeded(..., n_quad_price)` exposes the premium override, and
the ladder bench drives every decoupled rung through it. **Policy recommendation (for
K2/K3):** give `scheme_from_opts` / the internal preset table a decoupled premium order
so a production "fast+decoupled" rung is selectable without the bench seam. (K1 does not
change the public struct — pre-release clean-break budget, but out of K1 scope.)

Preset correspondence:

| QuantLib | (l,m,n,p/ε) | Our nearest `AlScheme` | Notes |
|---|---|---|---|
| fast | (7,2,7,27) | nb=7, fp=8, price=32, ~2 sweeps | our `al_fast_opts` is nb=7 **fp=16 price=16** — over-pays fp, under-pays price |
| accurate | (25,5,13,1e-8) | nb=13, fp=24, price=48, 5 sweeps | close to our nullopt ACCURATE (nb=12, fp=24, price=48, 6 sweeps) |
| highPrecision | (—,10,30,1e-10) | nb≈16–30, fp/price=64, ~10 sweeps, tol 1e-12 | ~ our ladder `reference` rung |

## 3. Healy FP-A vs FP-B policy (primary source)

Source: J. Healy, "Pricing American options under negative rates",
[arXiv 2109.15157](https://arxiv.org/abs/2109.15157).

- **FP-B** (price-continuity form, Healy eqns 20/23–24) is stable across the tested
  parameter range and is the recommended default: *"We prefer to focus only on the
  stable FP-B method."*
- **FP-A** (high-contact / δ-continuity form, eqns 21/25–26) **may be unstable for
  `q < r`** — oscillations near `t=0` that worsen with maturity and even at moderate
  rates (e.g. T=10, r=5%). The safe region for FP-A is therefore `q ≥ r` (equivalently
  `r − q ≤ 0`, i.e. the |r−q| policy: FP-A only when the drift `r−q` is not positive /
  is small).

**Our position:** our fixed-point iteration is the robust (FP-B-class) form and does not
switch to FP-A anywhere, so there is **no |r−q|-gated scheme switch to add**. If a future
task introduces an FP-A fast path for its convergence speed, it MUST gate to `q ≥ r`
(cite Healy §on FP-A instability) and fall back to FP-B otherwise. Recorded here so the
policy is not rediscovered. Note our engine independently classifies the negative-carry
double-continuation corner (`classify_regime`) as `NotImplemented`; that is a separate
concern from FP-A/FP-B stability.

---

## 4. The ladder bench (measured)

Bench: `atx-vol/bench/al_preset_ladder_bench.cpp` (target
`atx-vol-al-preset-ladder-bench`, rows `american/ladder/*`). Baseline JSON:
`bench/baselines/i7-1260p-clang18-avx2-al-preset-ladder.json`.

Grid: 104-pt representative SPY-OPRA strike/T/vol grid (S=600, r=0.043, q=0.013; dense
near-money moneyness ladder × ~1wk–1y maturities × a put-skewed smile). The bench
*harvests a real fitted SPY board* (2026-06-05 stress-close slice → `FitPreset::Fast`
session → `iv(K,T)`) when the OPRA parquet is present; in this source-only environment
the parquet was absent, so `grid_real=0` (representative fallback). Every rung is scored
against the richest in-repo AL scheme (`reference`: nb=16, fp=64, price=64).

**PROVISIONAL** (shared host; §3 measurement honesty). Several rows breach the 5% CV
gate this run (reference 7.6%, accurate 5.7%, fast 5.5%, fast_p32 5.4%, mid 9.2%; ql_fast
3.2%, ql_accurate 2.7% pass). The **relative A/B is citable** (ql_fast is fastest in
every one of 5 reps); the **absolute µs/op is provisional** until the WS-V V3
quiet-window re-capture.

| rung | nb | fp | price | sweeps | µs/op (prov.) | ×vs fast | max abs err | p99 abs err | spec. FP? |
|---|---|---|---|---|---|---|---|---|---|
| `reference` | 16 | 64 | 64 | 12 | 1352.2 | 0.03× | — (denom) | — | no |
| `accurate` (nullopt) | 12 | 24 | 48 | 6 | 205.6 | 0.23× | 3.9e-5 | 2.8e-5 | **yes** |
| `fast` (`al_fast_opts`) | 7 | 16 | 16 | 4 | 46.7 | 1.00× | 9.7e-4 | 7.8e-4 | **yes** |
| **`ql_fast`** (decoupled) | 7 | 8 | 32 | 2 | **25.8** | **1.81×** | 1.0e-3 | 7.1e-4 | no (7,8) |
| `ql_accurate` | 13 | 24 | 48 | 5 | 208.7 | 0.22× | 6.5e-5 | 5.2e-5 | no (13,24) |
| `fast_p32` (fast + rich premium) | 7 | 16 | 32 | 4 | 56.1 | 0.83× | 1.3e-4 | 9.4e-5 | yes FP / generic premium |
| `mid` | 9 | 16 | 24 | 6 | 110.5 | 0.42× | 3.0e-4 | 2.6e-4 | no (9,16) |

Errors are absolute price differences (grid S=600, so $-units) vs the `reference`
scheme; `med_abs_err` is ~0 because many grid points are near-intrinsic (exact).

**Readings:**

1. **`ql_fast` is the headline.** Cheap fixed-point quadrature (`fp=8`, half of fast's
   16) + only 2 sweeps (vs 4) drops the dominant `fp·nb·sweeps` work from 448 to 112
   (−75%), while the decoupled rich premium (`price=32`) holds accuracy at ~1e-3 —
   *statistically the same* as the current fast (9.7e-4). Net **~1.8× faster at equal
   accuracy**, and it pays the generic-path tax (below), so a specialized `(7,8)` would
   widen the gap. Validates the QuantLib decoupling thesis on our engine.
2. **`fast_p32` is the drop-in accuracy bump.** Same specialized `(7,16)` FP block as the
   current fast, premium raised 16→32: **~7× more accurate (1.3e-4)** for ~+20% time.
   Useful where marks/greeks need ~1e-4, not ~1e-3.
3. **Our ACCURATE already dominates QuantLib-accurate.** `accurate` (205.6 µs, 3.9e-5) is
   both cheaper-ish *and* more accurate than `ql_accurate` (208.7 µs, 6.5e-5). The
   high-accuracy tier needs no change. (`ql_accurate` also pays the generic tax; even
   specialized it would only match, not beat, our well-tuned nullopt preset.)
4. **`mid` is Pareto-dominated** by `fast_p32` (cheaper AND more accurate) — dropped from
   the recommendations.

**Specialization confound (an engine finding, flagged for K2/K3).** Only two
`(n_boundary, n_quad_fp)` pairs are hoisted (compile-time trip counts + geometry
precompute): `(7,16)` and `(12,24)` (`al_fp_specialized`, american.cpp). Every other rung
runs the *generic* scalar fixed-point path and pays a large tax — this is why
`ql_accurate` (13,24, generic) lands at the same µs as `accurate` (12,24, specialized)
despite doing fewer sweeps. **Consequence for the policy:** adopting a new production rung
(e.g. `ql_fast`'s `(7,8)`) should be paired with a one-line addition to
`al_fp_specialized` + its geometry sizing, or the generic tax eats part of the win.
`ql_fast` beats `fast` *anyway* — the algorithmic win survives the tax — but specializing
it converts the ~1.8× into the full headroom.

---

## 5. Tier policy table

Derived from §4, gated against the §3 economic gate:
`price abs err ≤ min(0.5·tick, 0.1·vega·1e-4)`, inside the quote half-spread, no new
butterfly/calendar/vertical arb. For SPY, `0.5·tick = 0.005`; for ATM `0.1·vega·1e-4 ≈
5e-4`. Every tier carries its **measured max abs err** from the ladder; no silent budget
cut.

| Consumer / tier | Recommended rung | Measured max abs err | Why (gate) |
|---|---|---|---|
| **marks-tier** (backtest/live marks, settlement, pnl marks) | **`ql_fast`** (fp=8, price=32, nb=7) | ~1.0e-3 | 1e-3 = 0.1¢ ≪ SPY penny half-spread (0.5¢+); comfortably inside the band; 1.8× cheaper than today's fast. Cache-served marks skip the solve entirely (see §6) — this is the **cache-miss / cold** mark cost. |
| **greeks-tier** (FD/analytic bundles: delta/gamma/vega/…) | **`fast_p32`** (fp=16, price=32, nb=7) | ~1.3e-4 | FD greeks divide price error by the bump; the richer premium (7× tighter price than fast) keeps differenced greeks in-band. `ql_fast` (1e-3) is too coarse for bumped vega/volga. K3's laned bundle should ride this rung. |
| **fit-de-Am-tier** (session de-Americanization / IV inversion) | **`fast`** (unchanged) or **`ql_fast`** | ~1e-3 | Inversion/cache sampling need only ~1e-4 *price* vs ~1e-2 surface RMSE (per `al_fast_opts` doc); already fast-tier. `ql_fast` is a safe 1.8× drop-in here (same accuracy class) once specialized. Keep tol matched (iv_tol 1e-5). |
| **cache-sampling-tier** (correction-cache build: `andersen_lake_call_slice`) | **`ql_fast`** | ~1e-3 | The cache is sampled once and re-served ~15–75× (RepresentativeFast, ~6.5 µs/serve); its build cost is pure `nb·fp·sweeps` work per sample → the `fp=8`/2-sweep rung is the biggest saver, and the served correction is blended/interpolated so ~1e-3 sample error is absorbed. |
| **reference / oracle** (parity gates, ladder denominator, golden checks) | **`accurate`** (nullopt) or `reference` | 3.9e-5 / ~0 | The correctness oracle stays. Never a hot-path tier. |

Notes:
- **Composition trap (§11.2):** each tier is individually in-band, but the composed
  pipeline (marks→pnl→greeks) must be re-checked against the accurate reference by
  K5/L5/C6 — not just per-stage. This table is per-stage evidence, not the composed gate.
- **No default changes here.** These are *recommendations*; K2/K3 (kernel), C3 (populate)
  and L4 (loop) adopt them under their own economic gates. `ql_fast` should be specialized
  (`al_fp_specialized` += `(7,8)`) before it becomes a production default.
- The vega term of the gate tightens on low-vega deep wings; ladder max errors are
  dominated by exactly those long-dated wing points, so the table uses `max` (worst-case),
  not median.

---

## 6. Audit — what `pricing_.al_opts` does the backtest cold path resolve to?

**Question (§2 finding 9 / K1):** the backtest prices marks through a `PricedSurface`;
which AL preset does its cold path actually use, and is it over-paying?

**The routing (traced, current lines):**

1. Backtest marks call `PricedSurface::fair_value(K,T,side, execution)` →
   `price_resolved` (`src/priced_surface.cpp:600-618`). Two layers:
   - **Cache layer:** if `execution == Configured` and a `query_accelerator_`
     (correction cache) is present and the blend is usable, the mark is served by
     `american_price_cached` (~6.5 µs, **no boundary solve**). This is the
     RepresentativeFast/CarryBank path and the dominant steady-state mark route when the
     cache covers the query.
   - **Cold fallback:** otherwise →
     `american_price(pricing_.S, …, pricing_.method, std::optional<AlOpts>{pricing_.al_opts})`
     (`priced_surface.cpp:616-617`). Engaged optional ⇒ `scheme_from_opts` maps the public
     knobs (NOT the nullopt ACCURATE path).
2. `pricing_.al_opts` is baked at **fit** time in `VolaSession::to_priced_surface`
   (`src/session.cpp:1305`): `pc.al_opts = in_.deam.al_opts.value_or(al_fast_opts())`.
3. `in_.deam.al_opts` is set by the fit preset (`apply_fit_preset`, `session.cpp:691-739`):
   - `Fast` / `Hft` → `al_fast_opts()` = `{7,16,4,1e-8}`.
   - `Accurate` / `Robust` → `al_default_opts()` = `{12,24,8,1e-10}`.
   - unset → `build()` auto-selects `al_fast_opts()` (`session.cpp:776-777`).
4. **The populate default is `FitPreset::Robust`** (`surface_db.hpp:115`,
   `SymbolFitConfig::preset`). So archived surfaces the backtest reads carry
   `pricing_.al_opts = al_default_opts() = {12,24,8,1e-10}`.

**The truth (the trap):** `{12,24,8,1e-10}` passed *engaged* through `scheme_from_opts`
does **NOT** equal the nullopt ACCURATE preset. Per the header note
(`american.hpp:56-64`), it resolves to:

```
engaged {12,24,8,1e-10}  ->  nb=12, fp=24, price=24, n_iter_jn=2, n_iter_fp=6, tol=1e-10
nullopt ACCURATE          ->  nb=12, fp=24, price=48, n_iter_jn=2, n_iter_fp=4, tol=1e-10
```

i.e. the Robust-populated cold mark path prices on **{12, 24, 24, 2+6, 1e-10}** — half the
premium quadrature (24 vs 48) but *more* fixed-point sweeps (6 vs 4) than the true
ACCURATE. Call it a "pseudo-accurate" scheme. On the ladder it sits near the `accurate`
rung (~200 µs/op) — **~4–8× the `ql_fast`/`fast` cost.**

**Answer:** Yes — the backtest **over-pays on every cold (cache-miss) mark**. It prices
marks on a ~200 µs pseudo-accurate scheme whose accuracy (~1e-5–1e-4) is far beyond what a
mark needs (the pxCLN 99.5% in-band metric is itself defined with `al_fast_opts` — see
`tests/support/opra_fixture.hpp:216`). A mark only has to land inside the quote
half-spread; `ql_fast` (~1e-3, 25.8 µs) and `fast` (46.7 µs) both clear that with room.

- **Where the over-pay bites:** cache *misses* and any strategy requiring
  `ColdReference` economics (`backtest.cpp:905-910,1369-1374`). Cache *hits*
  (RepresentativeFast) already skip the solve and are not over-paying.
- **The lever is not in K1's files.** `pricing_.al_opts` is baked at fit time, so the fix
  is **C3** (derive a Populate tier so the *right* preset is baked — `ql_fast`/`fast` for
  marks-grade surfaces) composed with **L4** (tier wiring so the cold fallback and the
  risk frame request the marks tier). K1's ladder is the evidence those tasks gate on.
- **If a surface must stay Robust-accurate for other reasons**, the backtest cold mark
  path could still be routed to a cheaper `al_opts` at *query* time (a PricedSurface
  marks-tier override), independent of the fit preset — a candidate seam for L4.

---

## 7. Sources

- QuantLib `QdFpAmericanEngine` source — https://github.com/lballabio/QuantLib/blob/master/ql/pricingengines/vanilla/qdfpamericanengine.cpp ; annotated: https://rkapl123.github.io/QLAnnotatedSource/d0/d74/qdfpamericanengine_8cpp_source.html
- Andersen, Lake, Offengenden, "High-Performance American Option Pricing", SSRN 2547027 — https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027
- HPC-QuantLib, "High Performance American Option Pricing" — https://hpcquantlib.wordpress.com/2022/10/09/high-performance-american-option-pricing/
- J. Healy, "Pricing American options under negative rates", arXiv 2109.15157 — https://arxiv.org/abs/2109.15157

In-code citations point back here at the point of use (`al_preset_ladder_bench.cpp`
header; `american.hpp` `al_fast_opts` / `AlOpts` notes).
