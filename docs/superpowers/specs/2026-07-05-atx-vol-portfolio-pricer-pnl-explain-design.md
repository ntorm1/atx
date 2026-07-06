# PortfolioPricer — PricedSurface-native portfolio pricing + Taylor PnL explain

**Date:** 2026-07-05
**Status:** Design → implementation.
**Scope:** `atx-vol` — a new, clean portfolio pricer built on the v3 curve
framework (`PricedSurface`). New: `portfolio_pricer.hpp/.cpp`, tests, a bench.
Coexists with the legacy `portfolio.hpp` (the faithful C `ats-vol` port, bound to
`VolSurface`/`CorrectionCache`/`Universe`/`MarketBinding`) — that stays untouched.

## 1. Goal

Repeat the archive revamp for the portfolio + portfolio pricer against the new
surface design. A caller should be able to:

1. Take a vector of positions spanning **N unique underlyings** and **M unique
   contracts**, store them in a `Portfolio`.
2. **Price** it: pass one `PricedSurface` per underlying and get a frame of
   `{ids, pv, first-order Greeks, second-order Greeks}`.
3. **Explain PnL**: pass a **base** and a **shifted** `PricedSurface` per
   underlying and get a full **Taylor-expansion PnL-explain** decomposition
   (delta / gamma / vega / volga / vanna / theta / rho / charm + unexplained
   residual) between the two surfaces, plus the same per-position frame.

Both paths are hot-path: throughput matches SOTA American pricing speed because
the per-contract kernel IS the library's SOTA `american_greeks` / `american_price`
(cold Andersen-Lake), called once per **unique** contract with no wrapper cost,
fanned out across threads. The API is clean and depends only on `PricedSurface`.
Validated on synthetic (multi-kind, known-truth) and real SPY+XOM OPRA boards.

## 2. Why a new module (not a rewrite of portfolio.hpp)

The archive revamp introduced `PricedSurface` as a NEW currency (coexisting with
`VolSurface`) and rewrote `surface_archive` because the v2 format was strictly
superseded. The legacy `portfolio.hpp` is different: it is a large, still-passing
port bound to the old world (`Universe`, `MarketBinding`, `UnderlyingMarket`,
`CorrectionCache`, `VolSurface`, bulk/scenario/plan/projection). None of that is
needed to price a `PricedSurface`, and dragging it in would couple the new hot
path to the old headers. So — exactly parallel to `PricedSurface` vs `VolSurface`
— the new pricer is a fresh, lean module that depends only on `priced_surface.hpp`,
and the legacy portfolio is left in place.

## 3. Pricing model — American mark, Black-76 model Greeks

Two `PricedSurface` primitives carry the pricer:

- `fair_value(K, T, side)` — the American Andersen-Lake mark. This is the accurate
  served theo (the surface reproduces its session's board accuracy bit-for-bit —
  the archive headline). The single **expensive** kernel: one AL solve.
- `greeks(K, T, side)` — the library's analytic Black-76 model sensitivities. A
  `PricedSurface` carries no correction cache, so `american_greeks(..., nullptr)`
  is a pure closed-form Black-76 evaluation (NO AL solve — effectively free). The
  conventions are exactly what a spot-based Taylor PnL needs (`american.cpp`
  `american_greeks_first_order`):

```
delta = ∂P/∂S      gamma = ∂²P/∂S²     (spot, m·D and m²·dD/dF)
vega  = ∂P/∂σ      volga = ∂²P/∂σ²
vanna = ∂²P/∂S∂σ   charm = ∂²P/∂S∂t    (spot cross terms)
theta = ∂P/∂t      rho   = ∂P/∂r       (calendar-time: ∂P/∂t = -∂P/∂T)
```

The pricer reports the American mark as the price/PV and the B76 Greeks as the
sensitivities — the same split the session serves. `pnl_explain` decomposes the
FULL American PnL (`fair_value` change) with these B76 Greeks; the
`pnl_unexplained` residual honestly carries the higher-order Taylor terms AND the
early-exercise-premium change the B76 Greeks cannot see (negligible for calls /
index options, larger for deep American puts). A fully American-consistent
decomposition would need bump-reprice Greeks and is deferred. Beyond these two
kernel calls the pricer is bookkeeping — dedup, scatter, fan-out, aggregate.

## 4. API

### 4.1 Position / contract model

```cpp
struct OptionContract {           // a listed option keyed to an underlying surface
  std::uint32_t uid;              // resolves to a PricedSurface (surface.uid())
  double K;                       // strike (>0)
  double T;                       // year-fraction to expiry (>0), at valuation
  Side side;                      // Call / Put
};

struct Position {
  std::uint64_t id{0};            // caller key, surfaced in every frame row
  OptionContract contract{};
  double qty{0.0};                // signed: + long / - short
  double multiplier{100.0};       // deliverable (<=0 or non-finite -> 100)
};
```

### 4.2 Portfolio — dedups contracts (the "M unique contracts" lever)

```cpp
class Portfolio {
  static Result<Portfolio> create(std::span<const Position> positions);
  std::size_t n_positions()   const;   // rows
  std::size_t n_contracts()   const;   // UNIQUE (uid,K,T,side) — one greek solve each
  std::size_t n_underlyings() const;   // unique uids
  std::span<const Position>     positions() const;
  std::span<const std::uint32_t> uids()    const;
};
```

`create` bit-hashes each `(uid,K,T,side)` to a unique-contract index; positions
keep a `qty·multiplier` weight and a contract index. Many positions on one
contract ⇒ one kernel solve. Empty book is valid (⇒ empty frame). `multiplier<=0`
or non-finite defaults to 100.

### 4.3 SurfaceSet — uid → PricedSurface resolver (non-owning)

```cpp
class SurfaceSet {
  static Result<SurfaceSet> create(std::span<const PricedSurface* const> surfaces);
  const PricedSurface* find(std::uint32_t uid) const noexcept;  // nullptr if absent
};
```

Built from a plain vector of surfaces — each surface knows its own `uid()`, so no
parallel uid array is needed. Duplicate uids or null pointers ⇒ `InvalidArgument`.

### 4.4 The pricer

```cpp
struct PriceOptions { unsigned n_threads{1}; };   // 0 => hardware_concurrency()

class PortfolioPricer {
  explicit PortfolioPricer(Portfolio pf);          // holds the book; reuse across snapshots
  Result<PriceFrame> price(const SurfaceSet& surfaces, const PriceOptions& = {}) const;
  Result<PnlFrame>   pnl_explain(const SurfaceSet& base, const SurfaceSet& shifted,
                                 const PriceOptions& = {}) const;
  const Portfolio& portfolio() const;
};
```

Holding the `Portfolio` lets the dedup be paid once and reused as surfaces stream
(the hot path: fixed book, moving market).

### 4.5 Output frames (SoA, input order)

`PriceFrame` — per-position columns `id, uid, pv, price(/share), iv,
delta, gamma, vega, theta, rho, vanna, volga, charm, status`, plus a `total`
aggregate (column sums over Ok lanes). PV and Greeks are **position-scaled**
(`qty·multiplier··`); `price`/`iv` are per-share. Aggregation is a plain sum.

`PnlFrame` — per-position columns `id, uid, pv_base, pv_target,
pnl_total, pnl_delta, pnl_gamma, pnl_vega, pnl_volga, pnl_vanna, pnl_theta,
pnl_rho, pnl_charm, pnl_unexplained`, the per-share state moves
`d_spot, d_vol, d_time, d_rate`, `status`, and a `total`. Every `pnl_*` component
is position-scaled and the eight Taylor terms + `pnl_unexplained` sum to
`pnl_total`.

`PriceStatus { Ok, ModelUnavailable, NumericError, InvalidContract }` — a lean
local enum (no dependency on the legacy `LaneStatus`).

## 5. Algorithms

### 5.1 price

1. Fan out the **unique** contracts across `n_threads` `std::jthread`s. Each
   worker owns a disjoint slice of the result array (no shared mutable state; the
   `PricedSurface` queries are pure const reads): `surf = surfaces.find(uid)`;
   null ⇒ `ModelUnavailable`; `K/T<=0` ⇒ `InvalidContract`; else
   `fv = surf->fair_value(K,T,side)` (American mark) and `g = surf->greeks(...)`
   (B76 Greeks).
2. Scatter serially in input order: `price = fv`, `pv = qty·mult·fv`, greeks
   `= qty·mult·g.*`, `iv = surf->iv(K,T)`. Accumulate `total` in fixed order.

The expensive Greeks solve is parallel over disjoint slots; the cheap scatter +
summation is serial in a fixed order ⇒ the frame is **bit-identical across thread
counts** (no float-add reordering in the reduction).

### 5.2 pnl_explain

Per unique contract, with `base`/`shifted` surfaces resolved by uid:

```
dt   = (now_shifted - now_base) / (365.25·86400·1e9)   // calendar years, library convention
T_b  = contract.T ;  T_t = T_b - dt
g    = base->greeks(K, T_b, side)                       // base B76 Greeks (coefficients)
p_b  = base->fair_value(K, T_b, side)                   // base American mark  (AL solve)
p_t  = shifted->fair_value(K, T_t, side)                // shifted American mark (AL solve)
dS   = shifted.S - base.S
dσ   = shifted->iv(K,T_t) - base->iv(K,T_b)             // vol the contract actually sees
dr   = shifted.r - base.r
```

Per-share Taylor terms (base B76 coefficients):

```
pnl_delta = g.delta·dS            pnl_gamma = ½·g.gamma·dS²
pnl_vega  = g.vega·dσ             pnl_volga = ½·g.volga·dσ²
pnl_vanna = g.vanna·dS·dσ
pnl_theta = g.theta·dt            pnl_rho   = g.rho·dr
pnl_charm = g.charm·dS·dt
pnl_total = p_t - p_b             pnl_unexplained = pnl_total - Σ(above)
```

Scaled by `qty·mult` per position (2 AL solves per contract — base + target).
Same fan-out/serial-scatter determinism as `price`. When base and shifted share
`now_ts` (the common "vol-shift" case) `dt=0` and the theta/charm terms vanish — a
pure vol/spot/rate explain. `pnl_total` is the full American reprice; the Greek
terms are the B76 decomposition and `pnl_unexplained` carries the higher-order +
early-exercise-premium remainder (§3).

## 6. Performance (measured)

- The only expensive kernel is the SOTA Andersen-Lake `fair_value` solve (one per
  unique contract in `price`; two — base + target — in `pnl_explain`); the Greeks
  are analytic B76 (≈free). The pricer adds only a hash-dedup, a pointer lookup,
  and float multiplies per row.
- Dedup collapses M positions → U unique contracts ⇒ U solves.
- `std::jthread` fan-out over unique contracts; determinism preserved (serial
  fixed-order reduction). `total.pv` bit-identical across {1,2,4,8,hw} threads.
- `portfolio_pricer_bench` (64 underlyings, 2688 contracts, mixed convex/eSSVI):
  kernel floor **77 µs/contract** (AL-bound); `price()` sits right on it
  (~91 µs/contract single-thread, ~15% frame-build overhead) and scales to
  **~53 k contracts/s** at hardware concurrency (~4.2× over 1 thread);
  `pnl_explain` ~2× the solves, ~33 k contracts/s at hw.

## 7. Validation (602/602 atx-vol tests green)

**Synthetic (`portfolio_pricer_test`, 10 tests):** multi-underlying, multi-kind
(ConvexDense/eSSVI/SVI) book; dedup (n_contracts < n_positions, shared contract
priced once); price frame `pv/price` bit-identical to `PricedSurface::fair_value`
and Greeks bit-identical to `PricedSurface::greeks` (qty·mult-scaled), `total` =
column sums; per-axis PnL isolation (spot / rate / vol / time shifts each light up
exactly the matching Taylor term — coefficient == base Greek × state move — and
leave the inactive axes exactly zero); components + unexplained == full reprice
(exact); a `q_eff=0` call book (American ≡ European) reconstructs to a tight
residual (`PnlExplain_TaylorReconstruction_Tight`); thread determinism (1 vs 8
bit-identical); degenerate contract ⇒ InvalidContract; missing uid ⇒
ModelUnavailable.

**Real OPRA (`spy_portfolio_pnl_test`):** SPY (index, ConvexDense) + XOM
(single-name) installed into one universe (distinct uids) → `to_priced_surface`
each → a 190-position book across both. Price rows bit-identical to per-contract
`fair_value` (PV) and `greeks`. Controlled spot-only shift → delta/gamma explain
the American PnL with the vol/time/rate axes exactly inert and a **2.05%**
aggregate residual (the early-exercise premium's spot sensitivity on the mixed
index + single-name book). GTEST_SKIP when the fixtures are absent.

## 8. Files

New: `include/atx/vol/portfolio_pricer.hpp`, `src/portfolio_pricer.cpp`,
`tests/portfolio_pricer_test.cpp`, `tests/spy_portfolio_pnl_test.cpp`,
`examples/portfolio_pricer_bench.cpp`.
Modified: `CMakeLists.txt`, `tests/CMakeLists.txt`, `include/atx/vol/vol.hpp`.
