# atx-vol library unification — chain → fit → parallel price → update

Status: **design of record** (2026-07-05). Goal: turn atx-vol into a
Vola-Dynamics-style library with one coherent lifecycle —

1. an **option chain**, each option a unique id;
2. passed to a **`PricerFitter`** with an optional config;
3. the fitter fits and **stores a unique_ptr to the fitted surface**;
4. the internal chain is **priced in parallel** with per-field output flags
   (model price, model IV, bid IV, ask IV, mid IV, Greeks);
5. the chain (or a subset of bid/ask by option id) can be **replaced/updated**.

…while holding the existing SPY-OPRA `spy_bidask_bench` accuracy (65.8 %
price-in-band) and performance, showing the API/code is cleaner, and proving the
parallel inversion/pricing layout works. It also closes the two deferred items
from the American-IV-throughput goal: **cross-strike call boundary reuse** and
the **bench cross-config band-cache**.

## What already exists (compose, don't reinvent)

The raw data structures the goal asks for are already in atx-vol:

- **Unique option id** — `ContractId` (`universe.hpp`), packed
  `(uid:24, expiry:16, strike_idx:16, side:1)`, decodable, and *the same id
  `Universe::apply_quotes` keys on*. This is `OptionId`. No side table.
- **SoA chain** — `Chain` (`universe.hpp`): `strikes[]` plus per-`(strike,side)`
  `bids/asks/mids/ivs/…` in one cache line per strike (`chain_index`).
- **Update bid/ask by id** — `Universe::apply_quotes(QuoteBatch)` already does
  the tick-to-quote write (mids recomputed) keyed by `ContractId`.
- **Fit + query** — `VolaSession` (`session.hpp`) builds a `VolSurface` from a
  quote frame and answers `iv/fair_value/greeks` with no refit, carrying the
  per-slice forward/carry and per-side correction caches.
- **Cold IV inversion** — `american_implied_vol` (`american_iv.hpp`), a
  stateless per-call safeguarded Newton (warm-started `AloPricer` inside).

The gap is a **clean facade** that ties these into the five-step lifecycle plus
a **genuine multi-threaded evaluator** (the existing `VolaSession` ladders are
single-threaded by design). So the unification is a thin composition layer, not
a rewrite — lower risk and, by construction, bit-consistent with the validated
`VolaSession` pricing.

## New public surface (two headers)

### `chain.hpp` — the addressable, mutable chain

```cpp
using OptionId = ContractId;                 // reuse the packed universe id

struct OptionRef { OptionId id; double T, strike, bid, ask, mid; Side side; … };

class OptionChain {                          // owns a Universe holding one Underlying
  static Result<OptionChain> from_frame(const QuoteFrame&, double r);
  double spot() const; double rate() const; std::int64_t now_ns() const; Uid uid() const;
  std::vector<OptionId> ids() const;                       // deterministic order
  Result<OptionRef> at(OptionId) const;                    // decode one option
  Status update_quotes(span<const OptionId>, span<const double> bids,
                       span<const double> asks);            // tick-to-quote
  const Underlying& underlying() const;                     // fitter/pricer input
};
```

`update_quotes` is a thin, id-keyed wrapper over `apply_quotes` (unknown ids
silently dropped, mids recomputed). Move-only (owns a `Universe`); `underlying()`
is resolved by `uid` on demand so it survives a move.

### `pricer_fitter.hpp` — fit once, own the surface, price in parallel

```cpp
enum class OutputField : uint32_t {          // bitmask
  ModelPrice, ModelIV, BidIV, AskIV, MidIV, Greeks,
  Prices = ModelPrice|ModelIV, Bands = BidIV|AskIV|MidIV, All = …
};

struct ChainValuation {                      // SoA, row i ↔ ids[i]
  vector<OptionId> ids;
  vector<double> model_price, model_iv, bid_iv, ask_iv, mid_iv;
  vector<AmericanGreeks> greeks;
  OutputField filled;
};

struct PricerConfig { FitPreset preset = Robust; unsigned n_threads = 0; vector<DividendEvent> cash_divs; };

class FittedSurface {                         // wraps VolaSession; move-only
  double iv(K,T); Result<double> fair_value(K,T,side); Result<AmericanGreeks> greeks(K,T,side);
  const VolaSession& session() const; const SessionDiagnostics& diagnostics() const;
};

class PricerFitter {
  explicit PricerFitter(PricerConfig = {});
  Status fit(const OptionChain&);            // builds + STORES unique_ptr<FittedSurface>
  bool fitted() const; const FittedSurface* surface() const;
  Result<ChainValuation> value_chain(const OptionChain&, OutputField, unsigned n_threads = 0) const;
};
```

`fit` maps `PricerConfig` → `SessionInputs` (via `make_session_inputs` +
`apply_fit_preset`), calls `VolaSession::from_frame`, wraps the session in a
`FittedSurface`, and stores it as `std::unique_ptr<FittedSurface>` (exactly the
goal's step 3).

## Parallel evaluator (step 4a)

`value_chain` enumerates `chain.ids()` and writes each option's requested fields
into disjoint SoA slots, fanned out across `n_threads` `std::jthread` workers
(0 ⇒ `hardware_concurrency`, 1 ⇒ serial) with a static block partition — the
same deterministic-by-construction pattern as `calibrate_pool`:

- every worker owns its index range; writes never overlap → no data race;
- all reads are `const` on the immutable `FittedSurface`/`VolaSession` and the
  stateless `american_implied_vol` (its `AloPricer` is a per-call local);
- output is index-ordered, so the result is **bit-identical for any thread
  count** (a determinism gate in the tests and `chain_pricer_bench`).

Per option: `model_iv = surface.iv(K,T)`; `model_price = fair_value` (cached hot
path); `greeks = greeks`; `bid/ask/mid_iv = american_implied_vol(price, S,K,T,r,
q_eff(T), side)` — the **cold, embarrassingly-parallel inversion** the goal wants
demonstrated. `q_eff(T)` and `forward(T)` come from two new `VolaSession` public
accessors (`q_eff_at`/`forward_at`) exposing the existing private carry
interpolation. Only requested fields are computed; the rest stay NaN.

Memory/cache/SIMD: the layout is SoA per output field (sequential writes per
worker). True AVX2 vector transcendentals stay unavailable under clang-cl (see
`american.cpp` PORT NOTE / README) so the per-option kernel remains the
scalar-backed `batch.hpp`/cached pricer; the parallelism is thread-level, which
is where the win is (the inversion is compute-bound and independent per option).

## Deferred item 1 — cross-strike call boundary reuse

`andersen_lake_call_slice(S, {K_i}, T, sigma, r, q, out, opts)` in
`american.hpp/.cpp`. A call `C(S,K,r,q) = P(K,S,q,r)` (McDonald-Schroder), so the
internal put has strike `Kp = S` (the fixed underlying spot) and spot `Sp = K_i`.
The Andersen-Lake early-exercise boundary depends on the internal *strike*
`(Kp=S, T, rp=q, qp=r, sigma)` — identical across call strikes — and `K_i`
enters only the premium quadrature's internal spot. So **one cold boundary solve
serves the whole slice**; each `out[i]` is bit-identical to
`andersen_lake(S, K_i, T, sigma, r, q, Call, opts)`.

Wired into `CorrectionCache::build`: the sampler's innermost loop is over
`k_log` (strike) at fixed `(T, sigma)` with `S = e^{-(r-q)T}` fixed, i.e. exactly
a call slice. For the **call side** the k-row is priced with one boundary solve
instead of `n_k` — an `n_k`× cut in boundary solves on the surface-fit hot path.
The put side (internal strike varies per strike) keeps the scalar path. Output is
bit-identical, so surface numbers (and `spy_bidask_bench`) are unchanged;
`correction_test.cpp` + the 65.8 % gate prove it.

## Deferred item 2 — bench cross-config band-cache

`spy_bidask_bench` inverts bid/ask/mid → American IV per quote, once per config,
across five configs. The band depends only on `(price, S, K, T, r, q_eff, side)`
with `S,r` constant and `method` fixed (ACCURATE) — so for a given `q_eff` the
three IVs are config-independent. The three Fast-preset configs share one
`q_eff` and the two Accurate configs share another, so an
`unordered_map<(side,K,T,q_eff)→(iv_bid,iv_ask,iv_mid)>` persisted across `run()`
calls collapses the 5× redundant inversions to 2×. Metrics unchanged; wall-clock
falls.

## Validation

- **New:** `pricer_fitter_test.cpp` — chain build/ids/at, `update_quotes`,
  `fit` stores the surface, `value_chain` field correctness vs `VolaSession`
  scalar queries, and thread-count determinism (1 vs N bit-identical).
- **New:** `andersen_lake_call_slice` == per-strike `andersen_lake` (grid).
- **New:** `chain_pricer_bench` — parallel scaling {1,2,4,8} + determinism +
  `update_quotes`→revalue on the real SPY board.
- **Held:** full suite green; `spy_bidask_bench` 65.8 % price-in-band unchanged
  at reduced wall-clock.

## Performance follow-up — reaching SOTA inversion throughput

The first cut of `value_chain` timed at ~35 s (debug, 8 threads) for a full
14,556-leg SPY board triple inversion — orders of magnitude off HFT scale. Root
cause + fix (all correctness-neutral, 584/584 held):

**SOTA reference.** Real-time IV inversion is a top-frequency quant task; risk
systems want **millions/sec aggregate** (European LBR ~180 ns, explicit ~59 ns —
Le Floc'h & Healy). American has no closed-form vega, so the SOTA method (Longo,
*Chasing Speed*, SSRN 2025; the Chebyshev-IV literature) is a **cheap surrogate in
the root-find, not a pricer** — precisely atx-vol's `CorrectionCache`. The
American-IV frontier is ~20-33k inv/s/core at the surrogate's accuracy.

**The bug + the levers.**
1. **`value_chain` ran the COLD pricer in the loop** — it passed *no* correction
   cache to `american_implied_vol`, so every residual was a full cold
   Andersen-Lake solve (12 BAW root-finds + sweeps + quadrature + cold polish),
   ~3-8 per inversion. The session already owns per-side Chebyshev caches (the
   documented 15.5× hot path). **Fix:** expose `VolaSession::correction_caches()`
   and route the bid/ask/mid inversions through them, seeded by the surface IV.
2. **`newton_vega` computed the FULL `american_greeks` bundle** (~7 cache evals
   incl. second-order FD) when the Newton step needs only vega. **Fix:** a
   dedicated `american_vega` (one cache `eval_grad`).
3. **Every `CorrectionCache` eval zero-initialized a 32 KB stack scratch** it
   overwrites before reading. **Fix:** drop the dead memset.
4. **No Release build existed.** **Fix:** a `rel` CMake preset (Release, `build-rel/`).

**Measured (real SPY OPRA, 14,556 legs, `build-rel`):**

| | before (debug, cold-in-loop) | after (release, surrogate-in-loop) |
|---|---|---|
| surface fit | 3,542 ms | **245 ms** |
| `value_chain` 1 core | 272 inv/s | **21,395 inv/s** |
| `value_chain` 4 cores | 642 inv/s | **93,612 inv/s** |
| full 43.7k-inversion board reprice | ~137 s | **0.40 s** |

Single-core 20-21k inv/s is inside the ~20-33k SOTA frontier; 4 cores → 94k/s
(8-thread plateau = 4 physical cores). Determinism (bit-identical across thread
counts) preserved. `american_iv_bench` config 4 isolates it on a known-truth
board: cold 1.2k inv/s at 1e-9 round-trip vs surrogate 20.5k inv/s at ~1e-3
near-ATM (wing-degraded where vega vanishes — the cold path is self-consistent by
construction, so it stays machine-exact there; the surrogate's production accuracy
is the re-pricing self-consistency `spy_bidask_bench` gates at 65.8%). Accuracy
gate held bit-for-bit through all four levers.

## Non-goals

- No new pricer/fit math — every number flows through the validated
  `VolaSession`/`andersen_lake`/`american_implied_vol` primitives.
- No SIMD vector transcendentals (toolchain-blocked); parallelism is threads +
  the Chebyshev surrogate. A batched-residual SIMD pass (invert bid/ask/mid of a
  strike together, or vectorize the Clenshaw recursion across strikes) is the next
  lever if sub-µs/inversion is needed — deferred (0.40 s/board already clears the
  fit-cadence bar; per-tick inversion of a few quotes is already µs-scale).
- No multi-underlying pool here (that is `calibrate_pool`); `OptionChain` is the
  single-underlier handle the five-step lifecycle describes.
