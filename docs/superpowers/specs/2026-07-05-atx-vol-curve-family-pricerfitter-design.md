# atx-vol — configurable curve family + self-contained PricerFitter + CurveSelector

**Date:** 2026-07-05
**Goal:** make the vol-curve family and `PricerFitter` entirely self-contained and
configurable so the library fits *any* underlying (index or single-name) with *any*
curve config, and generates a parameterized surface. Convex-QP Dense becomes one
curve type in a uniform family. If no config is supplied, a `CurveSelector` searches
for the best curve type + config for that board. "We have the depth (99.5% SPY);
now lock it in and add breadth."

## 1. Problem — what exists vs. what the goal needs

Firsthand code map (three exploration passes + direct reads):

- **The 99.5% Convex-QP dense fit is not servable through `PricerFitter`.**
  `PricerFitter::fit` → `VolaSession::build` → `run_surface_parity`, which is
  **hardwired to eSSVI**. `fit_convex_slice` (the thing that reaches 99.5%) is only
  ever called *manually in bench/example code* (`spy_bidask_bench`, `spy_oos_check`,
  `spy_dec_curve`). The production facade cannot produce a convex surface.
- **No curve polymorphism.** Every curve type is a standalone concrete struct
  (`EssviParams`, `SviParams`, `ConvexSliceFit`, …) with free-function evaluators;
  IV access is inconsistent (member `iv` only on `ConvexSliceFit`/`CStar`; the rest
  expose total-variance `w` only). Four surface containers hand-duplicate the same
  linear-in-total-variance time interpolation.
- **`PricerConfig` only selects a `FitPreset`.** No curve-type choice, no auto-search.
- **Rates are a flat scalar `double r` end-to-end.** `YieldCurve`/`CurveSet` exist
  but are dead relative to the fit path. There is no `MarketEnv` aggregate; spot/rate/
  timestamp travel as loose scalars across `OptionChain` + `SessionInputs`.
- **Boilerplate:** the real-OPRA load→install→session block is copy-pasted in 5+
  tests/examples.

Data on disk (real, same 2026-06-05T19:55Z snapshot, r=0.043):
`spy_opra_cbbo1m_…parquet` (index, dense, penny-wide) and
`xom_opra_cbbo1m_…parquet` (single-name, sparse, wider). One snapshot minute each —
no real open/close/pre-earnings slices (raw `.dbn.zst` present but no decoder in
atx-vol).

## 2. Research — the selection criterion (why SPY ≠ XOM)

Single-name boards are sparse and wide-spread; a dense near-interpolating fit
**overfits penny/quote noise** and generalizes worse than a parsimonious 3-parameter
eSSVI. Index boards (SPY) are penny-dense; the arb-free dense fit wins. The
principled, symbol-agnostic way to choose is **out-of-sample generalization**, not
in-sample error (which always favors more DoF). We already built the honest metric:
`spy_oos_check`'s leave-every-other-strike-out held-out price-in-band. That becomes
the `CurveSelector` objective, with a **parsimony tie-break** (fewer DoF wins ties).
On SPY this picks Convex-QP; on XOM it naturally prefers the parametric backbone.

## 3. Design

### 3.1 Uniform curve family — `include/atx/vol/vol_curve.hpp` (new)

A polymorphic per-slice curve. Virtual dispatch lives only at the fit/query-slice
layer, where cost is dominated by the American re-pricing (~6 µs/option); 2 vcalls
per query are free. The arithmetic evaluators (`essvi_total_w`, …) stay non-virtual.

```cpp
enum class VolCurveKind : std::uint8_t { ConvexDense = 0, Essvi = 1, Svi = 2 };

class IVolCurve {                     // one fitted expiry slice
 public:
  virtual ~IVolCurve() = default;
  [[nodiscard]] virtual double w(double k_log) const noexcept = 0;   // total variance
  [[nodiscard]] virtual double iv(double k_log) const noexcept;      // default sqrt(w/T)
  [[nodiscard]] virtual VolCurveKind kind() const noexcept = 0;
  [[nodiscard]] virtual std::size_t dof() const noexcept = 0;        // selector parsimony
  [[nodiscard]] double T() const noexcept; double F() const noexcept; double df() const noexcept;
 protected: double T_{}, F_{}, df_{};
};
```

Concrete adapters (thin, own their params): `ConvexDenseCurve` (wraps
`ConvexSliceFit`; `w = iv(k)²·T`), `EssviCurve` (wraps `EssviParams`;
`w = essvi_total_w`), `SviCurve` (wraps `SviParams`; `w = svi_total_w`).

`CurveSurface` — one ascending-T stack of `unique_ptr<IVolCurve>` with a **single**
linear-in-total-variance time interpolation + the Sprint-26 no-extrapolation guards
(the logic duplicated 4× today), answering `w(k,T)`/`iv(k,T)`.

Per-kind config + a tagged bundle:
```cpp
struct CurveConfig {
  VolCurveKind kind{VolCurveKind::ConvexDense};
  ConvexFitOpts convex{};       // reuse existing
  CalibOpts     parametric{};   // eSSVI/SVI share CalibOpts
};
```

Uniform per-slice fit dispatch (all kinds fit from de-Americanized European obs):
```cpp
Result<std::unique_ptr<IVolCurve>> fit_slice_curve(
    const CurveConfig&, std::span<const FitObs> obs_eu,
    double F, double T, double df);
```

### 3.2 Curve-agnostic surface fit + session override

`fit_curve_surface(under, SurfaceParityInputs, CurveConfig)` walks chains ascending-T,
reusing the **existing** de-Am front half (`resolve_chain_forward`) + q_eff bridge,
builds `build_observations_european`, calls `fit_slice_curve`, assembles a
`CurveSurface`, and scores re-Americanized parity per expiry (same metric as
`run_surface_parity`). Returns `{CurveSurface, ctx[], parity[]}`.

**Session wiring (low-risk override).** `SessionInputs` gains `CurveConfig curve{}`.
`VolaSession::build` dispatches:
- `Essvi` (**default**) → *exactly today's* `run_surface_parity` path (VolSurface).
  Byte-identical → all 584 tests + eSSVI depth preserved.
- `ConvexDense`/`Svi` → `fit_curve_surface`; result held in a new
  `std::optional<CurveSurface> curve_override_`.

`iv(K,T)` reads `curve_override_` when present, else `surface_` (today's path).
`fair_value`/`greeks`/ladders call `iv()` internally, so they follow the override
automatically and re-Americanize on the same interpolated carry. `refit_slice`
remains eSSVI-only (documented). This adds the convex path without disturbing the
optimized eSSVI concrete path.

### 3.3 MarketEnv — `include/atx/vol/market_env.hpp` (new)

```cpp
struct MarketEnv {
  double spot{0.0};
  std::int64_t now_ns{0};
  double flat_rate{0.043};        // fallback cont.-comp. rate
  YieldCurve yield{};             // optional rate curve; empty ⇒ flat_rate
  DividendSchedule dividends{};   // discrete cash divs
  [[nodiscard]] double rate_at(double T) const noexcept;  // yield.size()? yield.zero(T) : flat_rate
};
```

`OptionChain` gains a `from_frame(frame, MarketEnv)` overload (the scalar
`from_frame(frame, r, spot)` stays, delegating with a flat env) and stores the env.
`PricerFitter::fit` threads `MarketEnv` into the fit driver, querying `rate_at(T)`
per expiry (so a term structure of rates is honored where it matters — the carry;
flat env is bit-identical to today). Deep per-node rate integration is out of scope.

### 3.4 CurveSelector — `include/atx/vol/curve_selector.hpp` (new)

```cpp
struct SelectorConfig {
  std::vector<VolCurveKind> candidates{VolCurveKind::ConvexDense, VolCurveKind::Essvi};
  unsigned oos_max_expiries{8};   // subsample expiries for speed
};
struct CandidateScore { VolCurveKind kind; double oos_in_band; double oos_vw; std::size_t dof, n; };
struct SelectorResult { CurveConfig chosen; std::vector<CandidateScore> scores; };

Result<SelectorResult> select_curve(const Underlying&, const SurfaceParityInputs&,
                                    const SelectorConfig& = {});
```

Metric: leave-every-other-strike-out held-out **vega²-weighted price-in-band**,
averaged over a subsample of liquid expiries (the `spy_oos_check` logic, factored
into a reusable scorer). Highest OOS wins; ties (within ~0.3 pt) break toward fewer
DoF. `PricerConfig.curve` is `std::optional<CurveConfig>` — `nullopt` ⇒ run the
selector; set ⇒ use it directly.

### 3.5 Self-contained PricerFitter API (target)

```cpp
PricerConfig cfg;                       // .curve = nullopt ⇒ auto-select
OptionChain chain = OptionChain::from_frame(frame, env);   // env carries spot/ts/rate/divs
PricerFitter fitter{cfg};
fitter.fit(chain);                      // selects (if needed) + fits + owns surface
auto v = fitter.value_chain(chain, OutputField::All);
```

### 3.6 Boilerplate kill + tests

- `tests/support/opra_fixture.hpp` (new): `load_opra(symbol) → {panel, Universe, const Underlying&, MarketEnv}` with the multi-path probe + `GTEST_SKIP` when data is absent; plus a `price_in_band(...)` scorer lifted from `spy_bidask_bench`. Removes ~25 lines/site.
- `tests/spy_bidask_regression_test.cpp` (new): fit SPY through `PricerFitter` (convex config AND auto-select) and `ASSERT_GE` the clean price-in-band at **99.5%**. Skips cleanly without the parquet.

### 3.7 Breadth validation — `examples/vol_breadth_bench.cpp` (new)

- **Real:** SPY (index) + XOM (single-name), same snapshot, fit with **no config** →
  report which curve the selector auto-picked per symbol and the OOS in-band. Headline:
  one library call adapts to both.
- **Synthetic regimes** (extend `spy_fixture.hpp`): calm / high-vol / wide-open-spread /
  tight-mid-day / elevated-front (pre-earnings). Assert the fitter+selector stay
  arb-free and in-band across regimes. Labeled synthetic (no real open/close data on disk).

## 4. Files

New: `include/atx/vol/vol_curve.hpp`, `src/vol_curve.cpp`,
`include/atx/vol/market_env.hpp` (+ tiny `src/market_env.cpp` if needed),
`include/atx/vol/curve_selector.hpp`, `src/curve_selector.cpp`,
`tests/support/opra_fixture.hpp`, `tests/spy_bidask_regression_test.cpp`,
`examples/vol_breadth_bench.cpp`.
Modify: `session.hpp/.cpp` (CurveConfig + override), `pricer_fitter.hpp/.cpp`
(optional curve + selector + MarketEnv), `chain.hpp/.cpp` (MarketEnv),
`spy_fixture.hpp` (regime specs), `CMakeLists.txt` (+ sources, test, example).

## 5. Risk / invariants

- Default (eSSVI) fit path is untouched → 584 tests + eSSVI parity byte-identical.
- Convex path added as an override + a new curve-agnostic driver; the 99.5% SPY
  number is protected by a regression test.
- Virtual dispatch only per-slice-per-query; the American re-pricing dominates, so no
  measurable perf regression on `value_chain` (verified in Phase 7).

## 6. Results (measured)

**Depth — SPY held.** Convex-dense fit through `VolaSession`/`PricerFitter` and
served by `fair_value`: **pxCLN 99.49%** (4314/4336 clean quotes), pxALL 99.57% —
the 99.5% headline, now produced by the production facade rather than bench code.
Regression test `spy_bidask_regression_test` gates it (skips without the parquet).

**Served accuracy fix.** The 99.5% headline is defined on a COLD re-Americanization.
The library's fast cached hot-path pricer bakes its early-exercise correction at a
single representative carry, so serving an arbitrary model IV through it was only
**60%** penny-in-band on carry-distant expiries (a pre-existing limitation the new
depth exposed). `served_cache` now serves the high-accuracy override surface COLD
(→ **99.49%** served, == the fit accuracy) while the eSSVI default keeps the fast
cached path byte-identical. Band-IV inversions stay cached.

**Breadth — one call, right curve per board** (`vol_breadth_bench`, real OPRA):

| board | strikes | auto-selected | why (out-of-sample) | served pxCLN |
|-------|---------|---------------|---------------------|--------------|
| SPY (index, dense)      | 7278 | **ConvexDense** | 99.70% vs eSSVI 6.56% | 99.49% |
| XOM (single-name, sparse) | 695 | **eSSVI**       | 95.89% vs Convex 73.97% (dense over-fits) | 99.59% |

The CurveSelector's leave-every-other-strike-out metric picks the dense curve on
the penny board and the parsimonious backbone on the sparse one — the bias-variance
tradeoff the research predicted, with no per-symbol config.

**Breadth — market regimes** (`breadth_regime_test`, synthetic, labeled): calm /
stressed selloff (vol ×2.6) / wide-open (3% spreads) / pre-earnings (front-loaded
vol) all fit and serve 82–100% price-in-band with the auto-selecting fitter.

**No regression.** Full suite **588/588** (584 original + regression + breadth-regime;
the eSSVI default path is byte-identical, so the pre-existing tests are unchanged).

**Cost.** The convex dense fit's per-strike de-Americanization MUST be cold
Andersen-Lake: a near-interpolating fit propagates any de-Am bias straight into the
served IV, so routing it through the cached-de-Am hot path (fine for the coarse
eSSVI backbone) collapsed penny accuracy 99.5% → 57%. So the max-accuracy SPY fit is
de-Am-bound at ~14 s one-time for the full 7278-strike board (XOM ~1 s); the eSSVI
`Fast` path (~0.2 s) remains for latency-critical use, and value_chain band-IV
inversions stay on the fast cached path. Fit-once-serve-many amortizes it.

## 7. Deferred

C8/CStar as selectable kinds (evaluators partial/unported); full per-node rate-curve
integration into the carry (env accepts a `YieldCurve` and lowers to the
representative term rate; the market-implied per-expiry q_eff already absorbs the
dominant term structure); a real databento `.dbn` decoder for multi-time breadth
(synthetic regimes used instead — no real intraday slices on disk); folding the
default eSSVI path through `CurveSurface` (kept on its fast concrete container to
guarantee byte-identity); making the cached hot-path pricer carry-accurate (per-
expiry correction caches) so `value_chain` ModelPrice can stay both fast and
penny-accurate for the dense surface (today it serves cold-accurate).
