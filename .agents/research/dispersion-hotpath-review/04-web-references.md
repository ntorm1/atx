# 04 — Web References (Annotated Bibliography)

Reference material for the **atx-vol SPY dispersion options backtest engine** code-review sprint
(C++, HFT-grade hot path). Sources are grouped by the six research topics, ranked within each
group by authority/relevance. Each entry: title, URL, date, applicable takeaway for atx-vol,
authority note.

Compiled 2026-07-19. Method: WebSearch + WebFetch, primary/authoritative sources preferred,
claims cross-checked across sources where possible. A few PDF sources could not be machine-parsed
by the fetch model; for those the takeaway is drawn from the search-index abstract and is flagged
`[abstract]`.

---

## Topic 1 — Equity Dispersion Trading Methodology
*(vega- vs gamma- vs theta-neutral construction, index-vs-single-name variance, correlation trading, weighting, rebalancing, risk metrics)*

1. **BNP Paribas Global Markets — "Equity dispersion trading: how, what and when to trade"**
   https://globalmarkets.cib.bnpparibas/equity-dispersion-trading/ · (undated, recent sell-side desk note)
   Takeaway: A dispersion book is built with a *systematic weighting scheme so the basket stays
   gamma-flat / vega-flat / theta-flat over the whole trade life*; dispersion can be run
   vega-neutral OR theta-neutral, on bespoke single-name baskets and mixed maturities. Decompose
   the exposure into three blocks — single-share variance, pairwise correlation, vol dispersion.
   → For atx-vol: the weighting mode (vega/theta/gamma-neutral) must be an explicit, configurable
   leg-sizing policy, not a hard-coded 1:1, and should be re-solved on each rebalance.
   Authority: bulge-bracket equity-derivatives desk (primary practitioner).

2. **Bossu, Strasser & Guichard — "Just What You Need To Know About Variance Swaps"** (JPMorgan Equity Derivatives)
   http://docs.sbossu.com/bossu-strasser-guichard-varswap.pdf · May 2005 · `[abstract]`
   Takeaway: The canonical *vega-neutral* dispersion recipe — each single-stock **variance
   notional is scaled so its vega notional matches the index vega notional** (beta-weighted), so an
   instantaneous index-vol move is offset by the constituent-vol move. Gives the implied-correlation
   definition and shows a correlation swap is quasi-replicated by a variance-dispersion trade.
   → For atx-vol: this is the reference sizing formula to validate the leg-notional solver against.
   Authority: foundational sell-side primer, widely cited.

3. **Jacquier & Slaoui — "Variance Dispersion and Correlation Swaps"** (Imperial College / Birkbeck)
   https://arxiv.org/pdf/1004.0125 · 2010 · `[abstract]`
   Takeaway: Quantifies the ~10bp gap between a dispersion trade's implied correlation and the
   correlation-swap strike, attributable to the **vega, volga (vomma) and vanna** terms — i.e. the
   dispersion book is only a *quasi*-replication of a correlation swap.
   → For atx-vol: correlation-P&L attribution must carry second-order vol greeks, not just vega.
   Authority: peer-style academic (Antoine Jacquier).

4. **Kris Abdelmessih (Moontower) — "Dispersion Trading For The Uninitiated"**
   https://medium.com/@moontower/dispersion-trading-for-the-uninitiated-f96d9f6d6c7a · 2023-06-08
   Takeaway: A vega- or premium-neutral dispersion book is **short correlation convexity =
   negative gamma w.r.t. correlation**: when correlation rises the position becomes involuntarily
   *shorter* vol (index short grows faster than the stock longs). Vega-neutral ≠ correlation-neutral
   in stress; overweighting single-name vega tames the curvature at the cost of directional risk.
   → For atx-vol: the risk board needs a correlation-gamma / stress metric, not just a vega-neutral
   flag; sizing itself is regime-dependent.
   Authority: respected options-trading practitioner (ex-parity/vol MM).

*(Cross-refs from search: IBKR "Dispersion Trading in Practice — the Dirty Version" reinforces the
liquidity/rebalancing-cost gap between clean theory and live baskets; Goldman Sachs Foresi & Vesval
"Equity Correlation Trading" gives the implied-correlation-from-index-and-weights formula.)*

---

## Topic 2 — High-Performance Backtesting Engine Architecture
*(event-driven vs vectorized, look-ahead avoidance, determinism/reproducibility, minimal per-bar allocation, walk-forward)*

1. **NautilusTrader (Nautech Systems) — production-grade, Rust-native, deterministic event-driven engine**
   https://github.com/nautechsystems/nautilus_trader · actively maintained
   Takeaway: **Nanosecond-precision timestamps + strict chronological event ordering make
   look-ahead a structural impossibility** rather than a discipline; an *identical deterministic
   time model runs in both backtest and live* → "research-to-live parity, deploy with no code
   changes." Perf via Rust core + `mimalloc` allocator.
   → For atx-vol: adopt a single monotonic ns clock and one event-dispatch path shared by
   backtest/replay; parity is the design goal, not a nice-to-have.
   Authority: mature open-source production trading engine (closest analogue to atx-vol's target).

2. **Martin Fowler / LMAX — "The LMAX Architecture"**
   https://martinfowler.com/articles/lmax.html · 2011-07-12
   Takeaway: A **single-threaded business-logic processor over a lock-free ring buffer (the
   Disruptor)** reaches 6M ops/s through *mechanical sympathy* (keep code+data hot in cache, one
   writer per cache line to avoid false sharing, no per-event allocation/GC). **Event sourcing** —
   state is fully derivable from the input event stream — gives deterministic replay and
   reproducibility.
   → For atx-vol: the per-bar hot loop should be allocation-free (pre-sized ring/arena buffers),
   single-threaded-deterministic in the core, and replayable from an event log for bit-exact repro.
   Authority: canonical low-latency architecture reference.

3. **QuantStart — "Event-Driven Backtesting with Python, Part I"**
   https://www.quantstart.com/articles/Event-Driven-Backtesting-with-Python-Part-I/ · (©2012+)
   Takeaway: Market → Signal → Order → Fill event types drip-fed through a queue **structurally
   prevent look-ahead**; the same `DataHandler` abstraction is reused for backtest and live; the
   explicit tradeoff is that event-driven is *slower than vectorized* — design the hot path to claw
   that back.
   → For atx-vol: keep the correctness benefit of event ordering but recover speed via columnar/SIMD
   (Topics 3-4), i.e. batch within an event, don't vectorize across time.
   Authority: well-known quant-education reference.

4. **IoT Digital Twin PLM — "Event-Driven Backtesting Engine Architecture: Eliminating Lookahead Bias"**
   https://iotdigitaltwinplm.com/event-driven-backtesting-engine-architecture-algorithmic-trading/ · (recent)
   Takeaway: A **central event queue as message bus** enforces sequential processing that eliminates
   look-ahead; replay events in deterministic timestamp order matching what the live system sees.
   → For atx-vol: use as a checklist for the market-data-handler → strategy → execution split.
   Authority: secondary blog; corroborates 1-3, lower rank.

---

## Topic 3 — Zero-Copy / Memory-Mapped / Columnar Market-Data Access
*(Apache Arrow, mmap, cache-friendly SoA for options chains / surface & quote storage)*

1. **Apache Arrow — "Arrow Columnar Format" specification** (v24.x)
   https://arrow.apache.org/docs/format/Columnar.html · 2025 (living spec)
   Takeaway: **64-byte buffer alignment/padding matches the AVX-512 register width**, so IV/greek
   kernels can loop over a strike array with *no conditional tail logic*; arrays reconstruct
   **zero-copy** from `(offset, length)` pointer arithmetic and are directly **memory-mappable**
   (work with data bigger than RAM, share across processes/languages); **validity bitmaps** encode
   missing strikes / halted venues at ~12.5% overhead instead of sentinels.
   → For atx-vol: store option chains as **structure-of-arrays** (strike, bid, ask, delta, gamma,
   vega as separate 64-byte-aligned columns) so the pricing hot loop loads only the fields it needs
   and vectorizes cleanly.
   Authority: the columnar-format standard (primary spec).

2. **ArcticDB (Man Group) — high-performance columnar time-series / tick database**
   https://github.com/man-group/ArcticDB · https://arcticdb.io/ · actively maintained
   Takeaway: Data is **always stored columnar + compressed**, is **memory-mappable** (LMDB /
   in-memory backends), supports **tick/streaming** ingest, holds 20yr history of 400k+ securities
   per symbol, and offers **versioned "time-travel"** reads.
   → For atx-vol: validates a columnar + mmap store for the options-quote corpus; versioning gives
   *reproducible* backtests pinned to an immutable data snapshot.
   Authority: production quant-fund market-data DB (primary practitioner tooling).

3. **Apache Arrow — Introduction / Use-Cases** (supporting)
   https://arrow.apache.org/docs/format/Intro.html · https://arrow.apache.org/use_cases/ · 2025
   Takeaway: Arrow IPC files memory-map locally for zero-copy sharing across languages/processes —
   useful if the C++ hot loop and a Python research layer share one surface/quote buffer.
   Authority: primary spec, supporting rank.

---

## Topic 4 — SIMD / Vectorized Option Pricing
*(batched Black-76/American greeks, AVX2/AVX-512, branchless IV, Andersen-Lake, Chebyshev, throughput)*

1. **Andersen, Lake & Offengenden — "High-Performance American Option Pricing"** (J. Computational Finance)
   SSRN: https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027 · 2015/2016
   Implementation writeup: https://hpcquantlib.wordpress.com/2022/10/09/high-performance-american-option-pricing/ · 2022-10-09
   Takeaway: Spectral-collocation scheme — **Chebyshev interpolation of the early-exercise boundary
   + Gauss-Legendre quadrature + a Jacobi-Newton fixed-point iteration** — prices American options
   at **~100k prices/sec/CPU at finite-difference-grade accuracy** (10-11 sig digits), several
   orders of magnitude faster than PDE/tree methods. Tunable presets: *fast* `(l,n,m,p)=(7,2,7,27)`
   vs *accurate* `(25,5,13,1e-8)`.
   → For atx-vol: this is the recommended American-exercise pricer for SPY-constituent single-name
   options in the hot loop; expose the fast/accurate preset knobs.
   Authority: BofA quant research; the standard high-performance American method.

2. **Schadner — "An Explicit Solution to Black-Scholes Implied Volatility"** (Univ. Liechtenstein)
   https://arxiv.org/html/2604.24480v1 · arXiv:2604.24480 · 2026-04-27
   Takeaway: A **closed-form IV via the inverse Gaussian quantile — no initial guess, no iteration,
   no branch logic** — recovers IV to machine precision (2.24e-16) at **0.305 µs/eval, ~3.4× faster
   than Jäckel's "Let's Be Rational"** (1.038 µs).
   → For atx-vol: a branchless, iteration-free IV inversion is ideal for a SIMD-batched IV kernel
   (no per-lane divergence) — evaluate as a replacement/fast-path for the current inverter.
   Authority: recent academic; cross-checked against Jäckel as the accepted precision benchmark.

3. **Intel — "Black-Scholes Option Pricing on Intel CPUs and GPUs: SYCL and Optimization Techniques"**
   https://arxiv.org/pdf/2204.13740 · 2022 · `[abstract]`
   Takeaway: Black-Scholes vectorizes to ~8 lanes (256-bit) / **~16 lanes (AVX-512)**; using AVX-512
   over default SSE gave **~2.5× speedup**; a **Structure-of-Arrays layout** is required for
   contiguous SIMD loads; fast-math vs precision is an explicit tradeoff.
   → For atx-vol: gate an AVX-512 pricing kernel behind runtime CPU dispatch with a portable
   fallback, and drive it off SoA option columns (ties to Topic 3).
   Authority: Intel engineering (vendor-authoritative on x86 SIMD).

4. **PIVOT — "Bridging Black-Scholes IV and Price Objectives via Differentiable Jäckel Operator"** (supporting)
   https://arxiv.org/html/2606.17065 · arXiv:2606.17065 · 2026
   Takeaway: Calls a **vectorized Jäckel solver in the forward pass and uses implicit
   differentiation**, deliberately *avoiding backprop through rational-branch logic / masks /
   Householder iterations* — a concrete pattern for keeping a batched IV path branch-divergence-free.
   Authority: recent academic, supporting rank.

*(Cross-ref: `py_vollib_vectorized` — https://py-vollib-vectorized.readthedocs.io/ — documents a
production vectorized/batched greeks+IV API surface worth mirroring for the batch interface shape.)*

---

## Topic 5 — Vol-Surface Calibration Performance
*(SVI/eSSVI fast + warm-started calibration, arbitrage-free constraints, parallel fitting — build-corpus fit throughput)*

1. **Gatheral & Jacquier — "Arbitrage-Free SVI Volatility Surfaces"** (Quantitative Finance)
   https://arxiv.org/pdf/1204.0646 · arXiv:1204.0646 · Mar 2013 (QF 2014)
   Takeaway: The **SSVI parametrization is arbitrage-free by construction** — parameters vary
   smoothly across maturities to kill calendar-spread arbitrage, and convexity conditions kill
   butterfly arbitrage. Calibrate slice-by-slice then stitch into the SSVI surface.
   → For atx-vol: use SSVI's closed-form no-arbitrage constraints as *hard bounds inside* the fit
   loop (reject/clamp rather than post-hoc repair) to keep the build corpus arbitrage-clean.
   Authority: **the** canonical arbitrage-free SVI paper (Gatheral).

2. **"No-arbitrage global parametrization for the eSSVI volatility surface"**
   https://arxiv.org/pdf/2204.00312 · arXiv:2204.00312 · 2022 · `[abstract]`
   Takeaway: A **globally** arbitrage-free eSSVI parametrization (correlation made
   maturity-dependent for better short-end fit), enabling **one global optimization** instead of the
   slice-sequential approach — a natural fit for warm-starting and **parallel** whole-surface fitting.
   → For atx-vol: consider a global-surface fit (warm-started from the prior day's params) to cut
   build-corpus wall-clock and improve short-dated SPY fits.
   Authority: academic; builds on Hendriks & Martini (2019) global eSSVI concept.

3. **Hendriks & Martini (Zeliade Systems) — "Robust calibration and arbitrage-free interpolation of SSVI slices"**
   https://arxiv.org/pdf/1804.04924 · arXiv:1804.04924 · 2018 · `[abstract]`
   Takeaway: A **robust, sequential-in-expiries SSVI slice calibration** with arbitrage-free
   interpolation between slices — the practical baseline the 2022 global method warm-starts against.
   → For atx-vol: reference implementation semantics for per-slice robustness (handles sparse/noisy
   quotes) to benchmark the fit pipeline for correctness before optimizing throughput.
   Authority: Zeliade Systems (quant vendor); widely used practitioner method.

---

## Topic 6 — Options Backtest Realism
*(transaction-cost/market-impact/spread models, borrow/financing, early-exercise & assignment, corporate actions, benchmark-relative stats)*

1. **BSIC (Bocconi Students Investment Club) — "Backtesting Series Ep. 5: Transaction Cost Modelling"**
   https://bsic.it/backtesting-series-episode-5-transaction-cost-modelling/ · 2025-10-05
   Takeaway: Model TC in tiers — **flat / linear / piecewise-linear / quadratic**; slippage as
   `c1·mean_spread + c2` (coefficients fit from execution data); **market impact** via
   **Almgren et al. power-law** (unit cost ∝ T^-β, β≈0.6) or **Obizhaeva-Wang** exponential-decay;
   recalibrate periodically as market ecology drifts.
   → For atx-vol: replace mid-price fills with a **spread + square-root-impact** fill model in the
   execution simulator; make the coefficients config-driven and data-calibrated.
   Authority: student quant club but cites the primary microstructure literature (Almgren,
   Obizhaeva-Wang) — good pointer to primaries.

2. **Days to Expiry — "Options Backtesting: Tools, Methods & Strategy Validation"**
   https://www.daystoexpiry.com/blog/options-backtesting-guide · (recent)
   Takeaway: **$0.10-0.20/leg slippage cuts returns 10-30%**; **don't assume all ITM options
   exercise early** — model actual assignment probabilities and **ex-dividend early-assignment risk
   on ITM calls**; adjust for **dividends / corporate actions** or early-exercise is mismodeled.
   → For atx-vol realism gaps: per-leg slippage, a real assignment/early-exercise model tied to
   ex-div dates, and a corporate-actions adjuster on the constituent chains.
   Authority: options-backtesting practitioner blog (corroborates the microstructure primaries).

3. **Goodwin — "The Information Ratio"** (Financial Analysts Journal) + **Grinold & Kahn, *Active Portfolio Management***
   https://tsgperformance.com/wp-content/uploads/2020/11/Goodwin-information-ratio.pdf · 1998
   Grinold & Kahn (2nd ed. 2000): https://www.semanticscholar.org/paper/75cced88c1199ba9e8607774b62f2af4c92f0875
   Takeaway: Report **benchmark-relative** stats — **alpha** (residual return), **beta** to the
   benchmark, **tracking error** (std of excess return), and **IR = mean excess return / tracking
   error**; IR is *the* standard skill-per-unit-active-risk measure.
   → For atx-vol: the backtest report should include IR / alpha / beta / tracking error vs a vol
   benchmark (e.g. index-vol or a short-vol reference), not just raw P&L and Sharpe.
   Authority: Grinold & Kahn is the foundational active-management text; Goodwin (FAJ) is the
   standard IR reference.

*(Early-exercise pricing itself is covered by Andersen-Lake in Topic 4 — the American pricer and the
assignment model together close the early-exercise realism gap.)*

---

### Source-quality ranking (most authoritative → supporting)
- **Primary/canonical:** Gatheral-Jacquier SVI, Andersen-Lake, Bossu-Strasser-Guichard,
  Grinold & Kahn, Apache Arrow spec, LMAX (Fowler), NautilusTrader.
- **Strong practitioner/vendor:** BNP Paribas desk note, ArcticDB (Man Group), Intel SYCL paper,
  Zeliade SSVI, Moontower, Jacquier-Slaoui.
- **Supporting/secondary:** QuantStart, BSIC, Days to Expiry, IoT-DigitalTwin blog, PIVOT,
  py_vollib_vectorized, Schadner (recent, single-source — verify before load-bearing use).
