# atx-vol → SOTA composable American-equity analytics + real-data proof

**Goal.** Second pass on `atx-vol` after the Vola-parity skeleton (see
`2026-07-04-atx-vol-vola-parity-design.md`). Two deliverables:

1. **Composable interface** — tie de-Am → fit → surface → price/greeks/arb/
   diagnostics into one clean, stateful `VolaSession` handle, the way Vola gives
   you a queryable surface object rather than a bag of free functions.
2. **Performance** — make the hot path (American price / American-IV inversion)
   fast by wiring the Chebyshev `CorrectionCache` (Black-76 + cached correction)
   into de-Am inversion and re-Am scoring, plus warm-started Newton. Prove the
   speedup with a benchmark.
3. **Prove on real data** — load a **real Databento OPRA NBBO (cbbo-1m) chain
   slice** (cached on disk, zero API spend) through the whole pipeline; report fit
   quality (χ², RMSE, fair-value-within-bid-ask) and throughput (µs/quote).

Status: **IMPLEMENTED** (2026-07-04). Shipped: `session` (VolaSession facade),
`opra_panel` (real OPRA cbbo Parquet → QuoteFrame loader + OSI parser), an atx-core
`opra_dbn_to_parquet` offline converter, a nullable per-side `CorrectionCache` hot
path threaded through `american_iv`/`deamer`/`parity`/`surface_parity`/`session`,
and the `opra_parity_bench` real-data harness. Full suite **556/556 green, 0
warnings** under `/W4 /permissive- /WX`; all cache params default null ⇒ the prior
543 tests are byte-identical.

**Real-data result (XOM OPRA cbbo-1m NBBO, 2026-06-05 15:55 ET, cached slice, no
API spend).** 1134 contracts across 19 expiries, spot implied from front PCP =
$150.16. De-Americanized + eSSVI-fit 18 expiries (438 OTM quotes):
- **fair value within bid-ask: mean 98.5%, worst 91.3%** (Vola's headline metric);
- **mean reduced χ² = 0.207** (Vola's C8 benchmark is 0.599, C12 0.021);
- mean vol-RMSE 0.019; per-expiry implied borrow term structure −0.9%…+2.6%.
- **Self-consistency:** cold vs cached hot path agree — 98.5% vs 98.5% in-bid-ask,
  χ² 0.214 vs 0.207.
- **Cold-start fit speed (single-thread):** whole-surface cold fit **9.0 s → ≈0.36 s
  (≈25×)** at held quality — matching a Vola-class cold-start turnaround. Levers:
  barycentric-node precompute in the Andersen–Lake boundary loop; an `al_fast_opts()`
  surface-fit AL preset (7 collocation / 16-pt GL / 2 JN + 2 FP sweeps) as the session
  default, with the IV-inversion tol matched to the pricer accuracy floor (a tighter
  tol collapses safeguarded Newton into bisection and *slows* the fit); removal of a
  redundant per-strike inversion in the borrow solve; borrow-FP Newton warm-starts.
- **Query hot path:** composable **`fair_value` is ≈15.5× faster cached
  (103 µs → 6.6 µs/query)** — Black-76 + Chebyshev correction vs cold Andersen-Lake.
  (The ratio is smaller than the original 122× only because the cold path itself is
  now ≈8× faster; the cache's absolute query cost is unchanged. With the fast cold
  path, the cache no longer accelerates a *one-shot* surface build — its payoff is
  repeated queries.)
- **SIMD/AVX2 (investigated, negative result):** the AL Gauss-Legendre quad loop is
  transcendental-bound with an already-SoA, L1-resident layout, so the only lever is
  faster vector special functions. A portable xsimd AVX2 rewrite measured **≈6.6×
  slower** (xsimd's polynomial exp/log/erfc dwarf the SVML-backed scalar libm);
  Intel SVML vector intrinsics beat scalar but are unavailable under the project's
  clang-cl toolchain (only MSVC cl.exe), and `-fveclib=SVML` would add a fragile
  Intel runtime-DLL dependency. Scalar is the measured in-toolchain optimum; further
  cold-start gains need a batch-across-options SoA American solver.
- **Calendar no-arb (near-money CLOSED, 2026-07-04):** the raw independent-per-
  slice fit crosses in total variance (55 of an 18-slice surface over k∈[−3,3];
  26 inside |k|≤0.6). `CalendarRepair::MonotoneFit` — a θ-floor + active-set
  one-sided w-floor calendar-constrained fit (see the SOTA-HFT roadmap Sprint 5) —
  clears the near-money window **|k|≤0.6: 26 → 0 at held quality** (in-bid-ask
  98.5%→98.5%, χ² 0.207→0.209). Deep-wing (|k|→3, ~20σ, no quotes) strict no-arb
  needs a φ-slope term-structure constraint and is deferred; `Project` gives a
  strict full-grid guarantee at a fit-quality cost (98.5%→20.4% — do not use as a
  default). Vola's calendar-coupled joint mode with per-term error bars remains
  the richer target.

---

## 1. What exists (audit)

- **Pipeline pieces, all green (543 tests):** `american_iv` (invert American→IV),
  `dividend` (hybrid divs + PCP borrow/forward), `deamer` (chain→Euro-equiv IV +
  term borrow/forward), `s3`, `fit_metrics` (reduced-χ²/error bars), `parity`
  (re-Americanized fair-value-within-bid-ask), `panel` (synthetic + CSV),
  `vola_parity` (`run_expiry_parity`), `surface_parity` (`run_surface_parity` →
  `SurfaceParityReport{surface, per_expiry, calendar_arb_free, ...}`).
- **`run_surface_parity` already does the whole build** (de-Am + fit every expiry
  → ascending-T eSSVI `VolSurface` + calendar check + per-expiry parity). It is
  90 % of the facade — what's missing is a *stateful handle* that keeps the
  per-expiry pricing context (F, borrow, q_eff, T) so you can **re-price / query
  after the build** (fair value, greeks, iv at arbitrary K,T).
- **Hot pricer exists but is unused by the pipeline:** `american_price_cached`
  (Black-76 + `CorrectionCache.eval`) is far cheaper than cold Andersen-Lake, but
  `deamer`/`parity` invert & re-price with the COLD AL path. Wiring the cache in
  is the perf win.
- **Real-data fetcher exists in atx-core:** `databento::pull_opra_cbbo_1m_to_parquet`
  writes Parquet `ts, underlying, symbol, bid_px, ask_px, bid_sz, ask_sz`
  (px = 1e-9 fixed-point i64, unset = INT64_MIN; `symbol` = OSI/OCC 21-char).
  A cached XOM slice is on disk as `data/xom_opra_cbbo1m_2026-06-05T1955Z.dbn.zst`
  (DBN, pulled 2026-06-05 15:55 ET). atx-vol's parquet→QuoteFrame loader is
  currently `NotImplemented`.

## 2. Design

### 2.1 `VolaSession` (composable facade) — `session.{hpp,cpp}`

A stateful handle built once from a market snapshot; cheap queries thereafter.

```cpp
struct SessionInputs {           // superset of SurfaceParityInputs
  double S, r; std::vector<DividendEvent> cash_divs; std::int64_t now_ts_ns;
  DeAmOptions deam; CalibOpts calib; double band_k{1.0};
  bool use_correction_cache{true};   // perf: cache-accelerate de-Am/re-Am
};
struct ExpiryContext { double T, expiry_ns, forward, borrow, q_eff, chi2, rmse; std::size_t n_used; };
struct SessionDiagnostics { double worst_frac_within_bidask, mean_chi2, build_ms; bool calendar_arb_free; std::size_t n_slices, n_quotes; };

class VolaSession {
  static Result<VolaSession> build(const Underlying&, const SessionInputs&);
  static Result<VolaSession> from_frame(const QuoteFrame&, const SessionInputs&); // install→pick uid→build
  // queries (no re-fit):
  double iv(double K, double T) const;                         // surface Euro-equiv IV
  double total_variance(double K, double T) const;
  Result<double> fair_value(double K, double T, Side) const;   // re-Americanized model price
  Result<AmericanGreeks> greeks(double K, double T, Side) const;
  // introspection:
  const VolSurface& surface() const; std::span<const ExpiryContext> expiries() const;
  const SessionDiagnostics& diagnostics() const;
};
```

`build` = `run_surface_parity` internals, but retains each slice's
`ExpiryContext`. `fair_value(K,T,side)`: locate bracketing expiries by T,
read `iv` from the surface, interpolate `q_eff`/`forward` linearly in T (exact at
a slice T), then `american_price(S,K,T,iv,r,q_eff,side)` — cache-accelerated when
enabled. `greeks` likewise via `american_greeks`. Stateless queries, thread-safe
after build.

### 2.2 Real-data loader — `opra_panel.{hpp,cpp}`

`load_opra_cbbo_parquet(OpraLoadSpec) -> Result<OpraPanel>`:
- `OpraLoadSpec{ path, underlying, snapshot_iso, r, spot_override(opt), cash_divs }`.
- Read via `atx::core::io::read_parquet`; columns `underlying`(str), `symbol`(str,
  OSI), `bid_px/ask_px/bid_sz/ask_sz`(i64), `ts`(ns). Drop rows with unset px.
- **OSI parse** (robust to root padding): last 15 chars = `YYMMDD` + `C/P` +
  `strike*1000` (8 digits); root = prefix, trimmed. → `QuoteRow`.
- Group into a `QuoteFrame` (one uid, snapshot ts). px scaled 1e-9 → dollars.
- **Spot:** if `spot_override` absent, imply the front-expiry PCP forward
  (`dividend.hpp`) from co-terminal call/put mids and set `S = F_front·e^{-r·T}`
  (XOM has no ex-div between 2026-06-05 and the front expiries). Report implied S.
- Returns `OpraPanel{ QuoteFrame frame; double implied_spot; std::string snapshot_iso; std::size_t n_contracts, n_expiries; }`.

### 2.3 DBN→Parquet converter (atx-core tool) — `examples/opra_dbn_to_parquet.cpp`

Zero-spend: read the cached `*.dbn.zst` via databento-cpp `DbnStore`, map
`instrument_id → OSI` via `PitSymbolMap` for the index date, push into the SAME
column layout `pull_opra_cbbo_1m_to_parquet` uses, `io::write_parquet`. Reuses
`osi_root` semantics. Produces `data/xom_opra_cbbo1m_2026-06-05T1955Z.parquet`.

### 2.4 Performance — cache-accelerated hot path

- Add an OPTIONAL `const CorrectionCache*` to the inversion + re-pricing seams
  (`european_equiv_iv`/`american_implied_vol` in `american_iv`/`deamer`;
  `chain_parity`/`fair_value` in `parity`/`session`). **Null ⇒ current cold AL
  behavior** (all 543 existing tests unchanged).
- `VolaSession::build` (when `use_correction_cache`): build one `CorrectionCache`
  per side over the chain's (k_log, T, σ) box (cold-AL sampled once on a small
  grid), then de-Am inversion and re-Am scoring call `american_price_cached`.
- **Warm-start** each strike's Newton IV solve from the previous strike's root
  (monotone in K) — fewer iterations, more robust.
- Benchmark: `opra_parity_bench` reports total build ms, µs/quote, and
  cold-vs-cached speedup on the real XOM chain.

### 2.5 Acceptance / proof harness — `examples/opra_parity_bench.cpp`

Load the real XOM cbbo panel → `VolaSession::from_frame` → print:
- **Accuracy:** per-expiry forward, implied borrow, fit RMSE(vol), reduced-χ²,
  fraction fair-value-within-bid-ask; surface calendar-arb-free.
- **Speed:** build ms, quotes, µs/quote, cold-vs-cached speedup.
Skips cleanly if the parquet is absent (like the C tests).

## 3. Build sequence

- **Wave A (new files, parallel subagents):** `opra_panel`, `session`,
  `opra_dbn_to_parquet` tool + unit tests (OSI parse, loader on a written fixture,
  session queries on a synthetic panel). Main thread integrates CMake, builds
  `/W4 /permissive- /WX`, runs full suite.
- **Wave B (perf, main thread — edits existing modules):** thread the nullable
  `CorrectionCache*`; warm-start Newton; rebuild + full suite green.
- **Wave C (proof):** run converter on cached DBN → parquet; run
  `opra_parity_bench`; record real-data accuracy + throughput here and in README.

## 4. Out of scope
- Fresh paid Databento pulls (cached slice suffices; fetcher already cost-gated).
- AVX2/AVX-512 batch kernels (scalar cache-accelerated path is the target).
- Native discrete-div PDE pricer (escrowed-forward AL remains the pricer).
